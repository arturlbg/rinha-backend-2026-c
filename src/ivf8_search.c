#include "ivf8_search.h"

#include <limits.h>
#include <string.h>

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

static uint32_t scan_cluster(const Ivf8Index *idx,
                             const int16_t query[IVF8_INDEX_DIMS],
                             uint32_t cluster,
                             uint32_t max_candidates,
                             Ivf8SearchStats *stats,
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
        uint32_t label_base = block * IVF8_INDEX_LANES;
        for (uint32_t lane = 0; lane < end_lane && stats->candidates_scanned < max_candidates; lane++) {
            Ivf8Neighbor candidate = {
                .distance = ivf8_block_lane_distance(idx->block_data, block, lane, query),
                .fraud = idx->labels[label_base + lane] != 0 ? 1u : 0u,
                .seq = *seq,
            };
            ivf8_top5_insert(top, candidate);
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
                       Ivf8SearchStats *stats,
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
    (void)scan_cluster(idx, query, cluster, max_candidates, stats, top, seq);
}

Ivf8SearchResult ivf8_search(const Ivf8Index *idx,
                              const int16_t query[IVF8_INDEX_DIMS],
                              const Ivf8SearchConfig *cfg) {
    Ivf8SearchResult result;
    memset(&result, 0, sizeof(result));
    if (idx == NULL || query == NULL || idx->k == 0) {
        return result;
    }

    uint32_t probe_count = normalized_probe_count(idx, cfg);
    uint32_t max_candidates = normalized_max_candidates(cfg);
    Ivf8Probe probes[IVF8_SEARCH_MAX_PROBES];
    probe_count = ivf8_select_probes(idx, query, probe_count, probes);

    Ivf8Neighbor top[IVF8_SEARCH_TOP_K];
    ivf8_top5_init(top);

    result.stats.centroids_scored = idx->k;
    uint32_t seq = 0;
    for (uint32_t probe = 0; probe < probe_count && result.stats.candidates_scanned < max_candidates; probe++) {
        scan_probe(idx, query, probes[probe], max_candidates, &result.stats, top, &seq);
    }

    result.fraud_count = ivf8_top5_fraud_count(top);
    return result;
}

uint8_t ivf8_search_fraud_count(const Ivf8Index *idx,
                                const int16_t query[IVF8_INDEX_DIMS],
                                const Ivf8SearchConfig *cfg) {
    return ivf8_search(idx, query, cfg).fraud_count;
}

