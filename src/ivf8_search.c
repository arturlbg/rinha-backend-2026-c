#define _POSIX_C_SOURCE 200809L

#include "ivf8_search.h"

#include <limits.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>

static uint64_t search_now_ns(void) {
    struct timespec ts;
#ifdef CLOCK_MONOTONIC_RAW
    (void)clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
#else
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint32_t normalized_probe_count(const Ivf8Index *idx, const Ivf8SearchConfig *cfg) {
    uint32_t probes = IVF8_SEARCH_DEFAULT_PROBES;
    if (cfg != NULL && cfg->probes != 0) {
        probes = cfg->probes;
    }
    if (idx != NULL && probes > idx->k) {
        probes = idx->k;
    }
    if (probes > IVF8_SEARCH_MAX_PROBES) {
        probes = IVF8_SEARCH_MAX_PROBES;
    }
    return probes;
}

static uint32_t normalized_max_candidates(const Ivf8SearchConfig *cfg) {
    if (cfg != NULL && cfg->max_candidates != 0) {
        return cfg->max_candidates;
    }
    return IVF8_SEARCH_DEFAULT_MAX_CANDIDATES;
}

static Ivf8SearchImpl normalized_impl(const Ivf8SearchConfig *cfg) {
    if (cfg == NULL || cfg->impl != IVF8_SEARCH_IMPL_AVX2) {
        return IVF8_SEARCH_IMPL_SCALAR;
    }
    return ivf8_cpu_supports_avx2() ? IVF8_SEARCH_IMPL_AVX2 : IVF8_SEARCH_IMPL_SCALAR;
}

Ivf8SearchImpl ivf8_search_impl_from_string(const char *value, bool *ok) {
    if (ok != NULL) {
        *ok = true;
    }
    if (value == NULL || value[0] == '\0' || strcmp(value, "scalar") == 0) {
        return IVF8_SEARCH_IMPL_SCALAR;
    }
    if (strcmp(value, "avx2") == 0) {
        return IVF8_SEARCH_IMPL_AVX2;
    }
    if (ok != NULL) {
        *ok = false;
    }
    return IVF8_SEARCH_IMPL_SCALAR;
}

const char *ivf8_search_impl_name(Ivf8SearchImpl impl) {
    return impl == IVF8_SEARCH_IMPL_AVX2 ? "avx2" : "scalar";
}

static void insert_probe(Ivf8Probe probes[IVF8_SEARCH_MAX_PROBES], uint32_t n, Ivf8Probe candidate) {
    for (uint32_t pos = 0; pos < n; pos++) {
        if (candidate.distance > probes[pos].distance ||
            (candidate.distance == probes[pos].distance && candidate.cluster > probes[pos].cluster)) {
            continue;
        }
        for (uint32_t shift = n - 1u; shift > pos; shift--) {
            probes[shift] = probes[shift - 1u];
        }
        probes[pos] = candidate;
        return;
    }
}

uint64_t ivf8_centroid_distance(const Ivf8Index *idx, const int16_t query[IVF8_INDEX_DIMS], uint32_t cluster) {
    uint64_t sum = 0;
    uint32_t k = idx->k;
    for (uint32_t dim = 0; dim < IVF8_INDEX_DIMS; dim++) {
        int32_t diff = (int32_t)query[dim] - (int32_t)idx->centroids[dim * k + cluster];
        sum += (uint64_t)(diff * diff);
    }
    return sum;
}

uint64_t ivf8_bbox_distance(const Ivf8Index *idx, const int16_t query[IVF8_INDEX_DIMS], uint32_t cluster) {
    uint64_t sum = 0;
    uint32_t k = idx->k;
    for (uint32_t dim = 0; dim < IVF8_INDEX_DIMS; dim++) {
        int16_t min_value = idx->bbox_min[dim * k + cluster];
        int16_t max_value = idx->bbox_max[dim * k + cluster];
        int32_t diff = 0;
        if (query[dim] < min_value) {
            diff = (int32_t)min_value - (int32_t)query[dim];
        } else if (query[dim] > max_value) {
            diff = (int32_t)query[dim] - (int32_t)max_value;
        }
        sum += (uint64_t)(diff * diff);
    }
    return sum;
}

uint64_t ivf8_block_lane_distance(const int16_t *block_data,
                                  uint32_t block,
                                  uint32_t lane,
                                  const int16_t query[IVF8_INDEX_DIMS]) {
    uint64_t sum = 0;
    uint32_t block_base = block * IVF8_INDEX_DIMS * IVF8_INDEX_LANES;
    for (uint32_t dim = 0; dim < IVF8_INDEX_DIMS; dim++) {
        int32_t diff = (int32_t)query[dim] -
                       (int32_t)block_data[block_base + dim * IVF8_INDEX_LANES + lane];
        sum += (uint64_t)(diff * diff);
    }
    return sum;
}

void ivf8_top5_init(Ivf8Neighbor top[IVF8_SEARCH_TOP_K]) {
    for (uint32_t i = 0; i < IVF8_SEARCH_TOP_K; i++) {
        top[i].distance = UINT64_MAX;
        top[i].fraud = 0;
        top[i].seq = UINT32_MAX;
    }
}

void ivf8_top5_insert(Ivf8Neighbor top[IVF8_SEARCH_TOP_K], Ivf8Neighbor candidate) {
    if (candidate.distance > top[IVF8_SEARCH_TOP_K - 1u].distance ||
        (candidate.distance == top[IVF8_SEARCH_TOP_K - 1u].distance &&
         candidate.seq > top[IVF8_SEARCH_TOP_K - 1u].seq)) {
        return;
    }
    for (uint32_t pos = 0; pos < IVF8_SEARCH_TOP_K; pos++) {
        if (candidate.distance > top[pos].distance ||
            (candidate.distance == top[pos].distance && candidate.seq > top[pos].seq)) {
            continue;
        }
        for (uint32_t shift = IVF8_SEARCH_TOP_K - 1u; shift > pos; shift--) {
            top[shift] = top[shift - 1u];
        }
        top[pos] = candidate;
        return;
    }
}

static bool ivf8_top5_accepts(const Ivf8Neighbor top[IVF8_SEARCH_TOP_K], uint64_t distance, uint32_t seq) {
    return distance < top[IVF8_SEARCH_TOP_K - 1u].distance ||
           (distance == top[IVF8_SEARCH_TOP_K - 1u].distance &&
            seq < top[IVF8_SEARCH_TOP_K - 1u].seq);
}

uint8_t ivf8_top5_fraud_count(const Ivf8Neighbor top[IVF8_SEARCH_TOP_K]) {
    uint8_t count = 0;
    for (uint32_t i = 0; i < IVF8_SEARCH_TOP_K; i++) {
        if (top[i].fraud != 0) {
            count++;
        }
    }
    return count;
}

uint32_t ivf8_select_probes(const Ivf8Index *idx,
                            const int16_t query[IVF8_INDEX_DIMS],
                            uint32_t probe_count,
                            Ivf8Probe probes[IVF8_SEARCH_MAX_PROBES]) {
    if (idx == NULL || query == NULL || probes == NULL || probe_count == 0) {
        return 0;
    }
    if (probe_count > idx->k) {
        probe_count = idx->k;
    }
    if (probe_count > IVF8_SEARCH_MAX_PROBES) {
        probe_count = IVF8_SEARCH_MAX_PROBES;
    }
    for (uint32_t i = 0; i < IVF8_SEARCH_MAX_PROBES; i++) {
        probes[i].cluster = UINT32_MAX;
        probes[i].distance = UINT64_MAX;
    }
    for (uint32_t cluster = 0; cluster < idx->k; cluster++) {
        Ivf8Probe candidate = {
            .cluster = cluster,
            .distance = ivf8_centroid_distance(idx, query, cluster),
        };
        insert_probe(probes, probe_count, candidate);
    }
    return probe_count;
}

static uint32_t ivf8_select_probes_avx2(const Ivf8Index *idx,
                                        const int16_t query[IVF8_INDEX_DIMS],
                                        uint32_t probe_count,
                                        Ivf8Probe probes[IVF8_SEARCH_MAX_PROBES]) {
    if (idx == NULL || query == NULL || probes == NULL || probe_count == 0) {
        return 0;
    }
    if (probe_count > idx->k) {
        probe_count = idx->k;
    }
    if (probe_count > IVF8_SEARCH_MAX_PROBES) {
        probe_count = IVF8_SEARCH_MAX_PROBES;
    }
    for (uint32_t i = 0; i < IVF8_SEARCH_MAX_PROBES; i++) {
        probes[i].cluster = UINT32_MAX;
        probes[i].distance = UINT64_MAX;
    }

    uint32_t cluster = 0;
    uint64_t distances[IVF8_INDEX_LANES];
    for (; cluster + IVF8_INDEX_LANES <= idx->k; cluster += IVF8_INDEX_LANES) {
        ivf8_centroid_distances_avx2(idx, query, cluster, distances);
        for (uint32_t lane = 0; lane < IVF8_INDEX_LANES; lane++) {
            Ivf8Probe candidate = {
                .cluster = cluster + lane,
                .distance = distances[lane],
            };
            insert_probe(probes, probe_count, candidate);
        }
    }
    for (; cluster < idx->k; cluster++) {
        Ivf8Probe candidate = {
            .cluster = cluster,
            .distance = ivf8_centroid_distance(idx, query, cluster),
        };
        insert_probe(probes, probe_count, candidate);
    }
    return probe_count;
}

static uint32_t select_probes_profiled(const Ivf8Index *idx,
                                       const int16_t query[IVF8_INDEX_DIMS],
                                       uint32_t probe_count,
                                       Ivf8SearchImpl impl,
                                       Ivf8Probe probes[IVF8_SEARCH_MAX_PROBES],
                                       Ivf8SearchProfile *profile) {
    if (idx == NULL || query == NULL || probes == NULL || probe_count == 0 || idx->k > IVF8_PRODUCTION_K) {
        return 0;
    }
    if (probe_count > idx->k) {
        probe_count = idx->k;
    }
    if (probe_count > IVF8_SEARCH_MAX_PROBES) {
        probe_count = IVF8_SEARCH_MAX_PROBES;
    }

    uint64_t centroid_distances[IVF8_PRODUCTION_K];
    uint64_t start = search_now_ns();
    if (impl == IVF8_SEARCH_IMPL_AVX2) {
        uint32_t cluster = 0;
        for (; cluster + IVF8_INDEX_LANES <= idx->k; cluster += IVF8_INDEX_LANES) {
            ivf8_centroid_distances_avx2(idx, query, cluster, centroid_distances + cluster);
        }
        for (; cluster < idx->k; cluster++) {
            centroid_distances[cluster] = ivf8_centroid_distance(idx, query, cluster);
        }
    } else {
        for (uint32_t cluster = 0; cluster < idx->k; cluster++) {
            centroid_distances[cluster] = ivf8_centroid_distance(idx, query, cluster);
        }
    }
    profile->centroid_ns += search_now_ns() - start;

    start = search_now_ns();
    for (uint32_t i = 0; i < IVF8_SEARCH_MAX_PROBES; i++) {
        probes[i].cluster = UINT32_MAX;
        probes[i].distance = UINT64_MAX;
    }
    for (uint32_t cluster = 0; cluster < idx->k; cluster++) {
        Ivf8Probe candidate = {
            .cluster = cluster,
            .distance = centroid_distances[cluster],
        };
        insert_probe(probes, probe_count, candidate);
    }
    profile->probe_select_ns += search_now_ns() - start;
    return probe_count;
}

static uint32_t scan_cluster(const Ivf8Index *idx,
                             const int16_t query[IVF8_INDEX_DIMS],
                             uint32_t cluster,
                             uint32_t max_candidates,
                             Ivf8SearchStats *stats,
                             Ivf8SearchProfile *profile,
                             Ivf8Neighbor top[IVF8_SEARCH_TOP_K],
                             uint32_t *seq) {
    uint32_t remaining = idx->counts[cluster];
    if (remaining == 0) {
        return 0;
    }

    uint32_t scanned = 0;
    uint32_t start_block = idx->offsets[cluster];
    uint32_t end_block = idx->offsets[cluster + 1u];
    for (uint32_t block = start_block;
         block < end_block && stats->candidates_scanned < max_candidates && remaining > 0;
         block++) {
        uint32_t end_lane = IVF8_INDEX_LANES;
        if (remaining < end_lane) {
            end_lane = remaining;
        }
        stats->blocks_scanned++;
        uint32_t label_base = block * IVF8_INDEX_LANES;
        for (uint32_t lane = 0; lane < end_lane && stats->candidates_scanned < max_candidates; lane++) {
            uint64_t distance = ivf8_block_lane_distance(idx->block_data, block, lane, query);
            if (ivf8_top5_accepts(top, distance, *seq)) {
                Ivf8Neighbor candidate = {
                    .distance = distance,
                    .fraud = idx->labels[label_base + lane] != 0 ? 1u : 0u,
                    .seq = *seq,
                };
                uint64_t top5_start = profile != NULL ? search_now_ns() : 0u;
                ivf8_top5_insert(top, candidate);
                if (profile != NULL) {
                    profile->top5_ns += search_now_ns() - top5_start;
                }
            }
            (*seq)++;
            stats->candidates_scanned++;
            scanned++;
        }
        remaining -= end_lane;
    }
    return scanned;
}

static uint32_t scan_cluster_avx2(const Ivf8Index *idx,
                                  const int16_t query[IVF8_INDEX_DIMS],
                                  uint32_t cluster,
                                  uint32_t max_candidates,
                                  Ivf8SearchStats *stats,
                                  Ivf8SearchProfile *profile,
                                  Ivf8Neighbor top[IVF8_SEARCH_TOP_K],
                                  uint32_t *seq) {
    uint32_t remaining = idx->counts[cluster];
    if (remaining == 0) {
        return 0;
    }

    uint32_t scanned = 0;
    uint32_t start_block = idx->offsets[cluster];
    uint32_t end_block = idx->offsets[cluster + 1u];
    uint64_t distances[IVF8_INDEX_LANES];
    for (uint32_t block = start_block;
         block < end_block && stats->candidates_scanned < max_candidates && remaining > 0;
         block++) {
        uint32_t end_lane = IVF8_INDEX_LANES;
        if (remaining < end_lane) {
            end_lane = remaining;
        }
        stats->blocks_scanned++;
        ivf8_block_distances_avx2(idx->block_data, block, query, distances);
        uint32_t label_base = block * IVF8_INDEX_LANES;
        for (uint32_t lane = 0; lane < end_lane && stats->candidates_scanned < max_candidates; lane++) {
            if (ivf8_top5_accepts(top, distances[lane], *seq)) {
                Ivf8Neighbor candidate = {
                    .distance = distances[lane],
                    .fraud = idx->labels[label_base + lane] != 0 ? 1u : 0u,
                    .seq = *seq,
                };
                uint64_t top5_start = profile != NULL ? search_now_ns() : 0u;
                ivf8_top5_insert(top, candidate);
                if (profile != NULL) {
                    profile->top5_ns += search_now_ns() - top5_start;
                }
            }
            (*seq)++;
            stats->candidates_scanned++;
            scanned++;
        }
        remaining -= end_lane;
    }
    return scanned;
}

static void scan_probe(const Ivf8Index *idx,
                       const int16_t query[IVF8_INDEX_DIMS],
                       Ivf8Probe probe,
                       uint32_t max_candidates,
                       Ivf8SearchImpl impl,
                       Ivf8SearchStats *stats,
                       Ivf8SearchProfile *profile,
                       Ivf8Neighbor top[IVF8_SEARCH_TOP_K],
                       uint32_t *seq) {
    uint32_t cluster = probe.cluster;
    if (cluster >= idx->k || idx->counts[cluster] == 0) {
        return;
    }

    if (top[IVF8_SEARCH_TOP_K - 1u].seq != UINT32_MAX) {
        uint64_t worst = top[IVF8_SEARCH_TOP_K - 1u].distance;
        if (ivf8_bbox_distance(idx, query, cluster) > worst) {
            stats->bbox_pruned++;
            return;
        }
        if (idx->radii[cluster] < probe.distance && probe.distance - idx->radii[cluster] > worst) {
            stats->radius_pruned++;
            return;
        }
    }

    stats->clusters_scanned++;
    if (idx->counts[cluster] > stats->largest_scanned_cluster_candidates) {
        stats->largest_scanned_cluster_candidates = idx->counts[cluster];
    }
    uint32_t cluster_blocks = idx->offsets[cluster + 1u] - idx->offsets[cluster];
    if (cluster_blocks > stats->largest_scanned_cluster_blocks) {
        stats->largest_scanned_cluster_blocks = cluster_blocks;
    }
    if (impl == IVF8_SEARCH_IMPL_AVX2) {
        (void)scan_cluster_avx2(idx, query, cluster, max_candidates, stats, profile, top, seq);
    } else {
        (void)scan_cluster(idx, query, cluster, max_candidates, stats, profile, top, seq);
    }
}

static Ivf8SearchResult ivf8_search_internal(const Ivf8Index *idx,
                                             const int16_t query[IVF8_INDEX_DIMS],
                                             const Ivf8SearchConfig *cfg,
                                             bool profiled,
                                             Ivf8Neighbor trace_top[IVF8_SEARCH_TOP_K],
                                             Ivf8Probe trace_probes[IVF8_SEARCH_MAX_PROBES],
                                             uint32_t *trace_probe_count) {
    Ivf8SearchResult result;
    memset(&result, 0, sizeof(result));
    if (idx == NULL || query == NULL || idx->k == 0) {
        if (trace_probe_count != NULL) {
            *trace_probe_count = 0;
        }
        return result;
    }

    uint64_t total_start = profiled ? search_now_ns() : 0u;
    uint32_t probe_count = normalized_probe_count(idx, cfg);
    uint32_t max_candidates = normalized_max_candidates(cfg);
    Ivf8SearchImpl impl = normalized_impl(cfg);
    Ivf8Probe probes[IVF8_SEARCH_MAX_PROBES];
    if (profiled) {
        probe_count = select_probes_profiled(idx, query, probe_count, impl, probes, &result.profile);
    } else if (impl == IVF8_SEARCH_IMPL_AVX2) {
        probe_count = ivf8_select_probes_avx2(idx, query, probe_count, probes);
    } else {
        probe_count = ivf8_select_probes(idx, query, probe_count, probes);
    }
    if (trace_probe_count != NULL) {
        *trace_probe_count = probe_count;
    }
    if (trace_probes != NULL) {
        memcpy(trace_probes, probes, sizeof(probes));
    }

    Ivf8Neighbor top[IVF8_SEARCH_TOP_K];
    ivf8_top5_init(top);

    result.stats.centroids_scored = idx->k;
    uint32_t seq = 0;
    uint64_t scan_start = profiled ? search_now_ns() : 0u;
    for (uint32_t probe = 0; probe < probe_count && result.stats.candidates_scanned < max_candidates; probe++) {
        scan_probe(idx, query, probes[probe], max_candidates, impl, &result.stats, profiled ? &result.profile : NULL, top, &seq);
    }
    if (profiled) {
        result.profile.scan_ns = search_now_ns() - scan_start;
    }

    result.fraud_count = ivf8_top5_fraud_count(top);
    if (trace_top != NULL) {
        memcpy(trace_top, top, sizeof(top));
    }
    if (profiled) {
        result.profile.total_ns = search_now_ns() - total_start;
    }
    return result;
}

Ivf8SearchResult ivf8_search(const Ivf8Index *idx,
                              const int16_t query[IVF8_INDEX_DIMS],
                              const Ivf8SearchConfig *cfg) {
    return ivf8_search_internal(idx, query, cfg, false, NULL, NULL, NULL);
}

Ivf8SearchResult ivf8_search_profiled(const Ivf8Index *idx,
                                       const int16_t query[IVF8_INDEX_DIMS],
                                       const Ivf8SearchConfig *cfg) {
    return ivf8_search_internal(idx, query, cfg, true, NULL, NULL, NULL);
}

Ivf8SearchTraceResult ivf8_search_trace(const Ivf8Index *idx,
                                         const int16_t query[IVF8_INDEX_DIMS],
                                         const Ivf8SearchConfig *cfg) {
    Ivf8SearchTraceResult trace;
    memset(&trace, 0, sizeof(trace));
    trace.result = ivf8_search_internal(idx, query, cfg, false, trace.top, trace.probes, &trace.probe_count);
    return trace;
}

Ivf8SearchTraceResult ivf8_search_trace_profiled(const Ivf8Index *idx,
                                                  const int16_t query[IVF8_INDEX_DIMS],
                                                  const Ivf8SearchConfig *cfg) {
    Ivf8SearchTraceResult trace;
    memset(&trace, 0, sizeof(trace));
    trace.result = ivf8_search_internal(idx, query, cfg, true, trace.top, trace.probes, &trace.probe_count);
    return trace;
}

uint8_t ivf8_search_fraud_count(const Ivf8Index *idx,
                                const int16_t query[IVF8_INDEX_DIMS],
                                const Ivf8SearchConfig *cfg) {
    return ivf8_search(idx, query, cfg).fraud_count;
}
