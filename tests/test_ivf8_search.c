#include "ivf8_search.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        failures++; \
    } \
} while (0)

typedef struct {
    Ivf8Index index;
    int16_t centroids[IVF8_INDEX_DIMS * 3u];
    uint32_t offsets[4];
    uint32_t counts[3];
    int16_t bbox_min[IVF8_INDEX_DIMS * 3u];
    int16_t bbox_max[IVF8_INDEX_DIMS * 3u];
    uint64_t radii[3];
    uint8_t labels[3u * IVF8_INDEX_LANES];
    int16_t block_data[3u * IVF8_INDEX_DIMS * IVF8_INDEX_LANES];
} TinyIndex;

static void set_soa(int16_t *values, uint32_t k, uint32_t cluster, uint32_t dim, int16_t value) {
    values[dim * k + cluster] = value;
}

static void set_lane(TinyIndex *tiny, uint32_t block, uint32_t lane, int16_t value) {
    uint32_t base = block * IVF8_INDEX_DIMS * IVF8_INDEX_LANES;
    for (uint32_t dim = 0; dim < IVF8_INDEX_DIMS; dim++) {
        tiny->block_data[base + dim * IVF8_INDEX_LANES + lane] = 0;
    }
    tiny->block_data[base + lane] = value;
}

static void make_tiny_index(TinyIndex *tiny) {
    memset(tiny, 0, sizeof(*tiny));
    tiny->index.n = 9;
    tiny->index.k = 3;
    tiny->index.dims = IVF8_INDEX_DIMS;
    tiny->index.lanes = IVF8_INDEX_LANES;
    tiny->index.blocks = 3;
    tiny->index.centroids = tiny->centroids;
    tiny->index.offsets = tiny->offsets;
    tiny->index.counts = tiny->counts;
    tiny->index.bbox_min = tiny->bbox_min;
    tiny->index.bbox_max = tiny->bbox_max;
    tiny->index.radii = tiny->radii;
    tiny->index.labels = tiny->labels;
    tiny->index.block_data = tiny->block_data;

    tiny->offsets[0] = 0;
    tiny->offsets[1] = 1;
    tiny->offsets[2] = 2;
    tiny->offsets[3] = 3;
    tiny->counts[0] = 6;
    tiny->counts[1] = 2;
    tiny->counts[2] = 1;

    for (uint32_t cluster = 0; cluster < 3; cluster++) {
        for (uint32_t dim = 0; dim < IVF8_INDEX_DIMS; dim++) {
            set_soa(tiny->bbox_min, 3, cluster, dim, 0);
            set_soa(tiny->bbox_max, 3, cluster, dim, 1000);
        }
    }
    set_soa(tiny->centroids, 3, 0, 0, 0);
    set_soa(tiny->centroids, 3, 1, 0, 100);
    set_soa(tiny->centroids, 3, 2, 0, 100);
    tiny->radii[0] = 10000;
    tiny->radii[1] = 10000;
    tiny->radii[2] = 10000;

    set_lane(tiny, 0, 0, 1);
    set_lane(tiny, 0, 1, 2);
    set_lane(tiny, 0, 2, 3);
    set_lane(tiny, 0, 3, 4);
    set_lane(tiny, 0, 4, 5);
    set_lane(tiny, 0, 5, 6);
    tiny->labels[0] = 1;
    tiny->labels[1] = 0;
    tiny->labels[2] = 1;
    tiny->labels[3] = 0;
    tiny->labels[4] = 1;
    tiny->labels[5] = 1;
    tiny->labels[6] = 1;
    tiny->labels[7] = 1;

    set_lane(tiny, 1, 0, 100);
    set_lane(tiny, 1, 1, 110);
    tiny->labels[8] = 0;
    tiny->labels[9] = 0;

    set_lane(tiny, 2, 0, 200);
    tiny->labels[16] = 1;
}

static void test_distance(void) {
    int16_t query[IVF8_INDEX_DIMS] = {0};
    int16_t block_data[IVF8_INDEX_DIMS * IVF8_INDEX_LANES] = {0};
    block_data[0 * IVF8_INDEX_LANES + 0] = 3;
    block_data[1 * IVF8_INDEX_LANES + 0] = 4;
    CHECK(ivf8_block_lane_distance(block_data, 0, 0, query) == 25);
}

static void test_search_impl_parse_and_detection(void) {
    bool ok = false;
    CHECK(ivf8_search_impl_from_string("scalar", &ok) == IVF8_SEARCH_IMPL_SCALAR);
    CHECK(ok);
    CHECK(ivf8_search_impl_from_string("avx2", &ok) == IVF8_SEARCH_IMPL_AVX2);
    CHECK(ok);
    CHECK(ivf8_search_impl_from_string("bogus", &ok) == IVF8_SEARCH_IMPL_SCALAR);
    CHECK(!ok);
    CHECK(strcmp(ivf8_search_impl_name(IVF8_SEARCH_IMPL_AVX2), "avx2") == 0);
}

static void test_avx2_block_distances_match_scalar(void) {
    if (!ivf8_cpu_supports_avx2()) {
        return;
    }

    int16_t query[IVF8_INDEX_DIMS];
    int16_t block_data[IVF8_INDEX_DIMS * IVF8_INDEX_LANES];
    for (uint32_t dim = 0; dim < IVF8_INDEX_DIMS; dim++) {
        query[dim] = (dim % 2u == 0) ? (int16_t)(-10000 + (int32_t)dim * 1000) :
                                      (int16_t)(10000 - (int32_t)dim * 700);
        for (uint32_t lane = 0; lane < IVF8_INDEX_LANES; lane++) {
            int32_t value = ((int32_t)dim - (int32_t)lane) * 777;
            if (dim == 3 && lane == 2) {
                value = -10000;
            }
            if (dim == 9 && lane == 6) {
                value = 10000;
            }
            block_data[dim * IVF8_INDEX_LANES + lane] = (int16_t)value;
        }
    }

    uint64_t avx2[IVF8_INDEX_LANES];
    ivf8_block_distances_avx2(block_data, 0, query, avx2);
    for (uint32_t lane = 0; lane < IVF8_INDEX_LANES; lane++) {
        CHECK(avx2[lane] == ivf8_block_lane_distance(block_data, 0, lane, query));
    }
}

static void test_top5_ordering_and_ties(void) {
    Ivf8Neighbor top[IVF8_SEARCH_TOP_K];
    ivf8_top5_init(top);
    ivf8_top5_insert(top, (Ivf8Neighbor){.distance = 30, .fraud = 1, .seq = 3});
    ivf8_top5_insert(top, (Ivf8Neighbor){.distance = 10, .fraud = 0, .seq = 1});
    ivf8_top5_insert(top, (Ivf8Neighbor){.distance = 20, .fraud = 1, .seq = 2});
    ivf8_top5_insert(top, (Ivf8Neighbor){.distance = 20, .fraud = 0, .seq = 0});
    CHECK(top[0].distance == 10 && top[0].seq == 1);
    CHECK(top[1].distance == 20 && top[1].seq == 0);
    CHECK(top[2].distance == 20 && top[2].seq == 2);
    CHECK(ivf8_top5_fraud_count(top) == 2);
}

static void test_probe_selection(void) {
    TinyIndex tiny;
    make_tiny_index(&tiny);
    int16_t query[IVF8_INDEX_DIMS] = {0};
    Ivf8Probe probes[IVF8_SEARCH_MAX_PROBES];
    CHECK(ivf8_select_probes(&tiny.index, query, 3, probes) == 3);
    CHECK(probes[0].cluster == 0);
    CHECK(probes[1].cluster == 1);
    CHECK(probes[2].cluster == 2);
    CHECK(probes[1].distance == probes[2].distance);
}

static void test_tiny_search_and_phantom_lanes(void) {
    TinyIndex tiny;
    make_tiny_index(&tiny);
    int16_t query[IVF8_INDEX_DIMS] = {0};
    Ivf8SearchConfig cfg = {.max_candidates = 5, .probes = 1};
    Ivf8SearchResult result = ivf8_search(&tiny.index, query, &cfg);
    CHECK(result.stats.candidates_scanned == 5);
    CHECK(result.stats.clusters_scanned == 1);
    CHECK(result.fraud_count == 3);

    cfg.max_candidates = 8;
    result = ivf8_search(&tiny.index, query, &cfg);
    CHECK(result.stats.candidates_scanned == 6);
    CHECK(result.fraud_count == 3);

    if (ivf8_cpu_supports_avx2()) {
        cfg.max_candidates = 8;
        cfg.impl = IVF8_SEARCH_IMPL_AVX2;
        Ivf8SearchResult avx2 = ivf8_search(&tiny.index, query, &cfg);
        cfg.impl = IVF8_SEARCH_IMPL_SCALAR;
        Ivf8SearchResult scalar = ivf8_search(&tiny.index, query, &cfg);
        CHECK(avx2.fraud_count == scalar.fraud_count);
        CHECK(avx2.stats.candidates_scanned == scalar.stats.candidates_scanned);
    }
}

static void test_candidate_cap_across_probes(void) {
    TinyIndex tiny;
    make_tiny_index(&tiny);
    int16_t query[IVF8_INDEX_DIMS] = {0};
    Ivf8SearchConfig cfg = {.max_candidates = 7, .probes = 3};
    Ivf8SearchResult result = ivf8_search(&tiny.index, query, &cfg);
    CHECK(result.stats.candidates_scanned == 7);
    CHECK(result.stats.clusters_scanned == 2);
}

int main(void) {
    test_distance();
    test_search_impl_parse_and_detection();
    test_avx2_block_distances_match_scalar();
    test_top5_ordering_and_ties();
    test_probe_selection();
    test_tiny_search_and_phantom_lanes();
    test_candidate_cap_across_probes();

    if (failures != 0) {
        fprintf(stderr, "%d ivf8 search test failure(s)\n", failures);
        return 1;
    }
    puts("ivf8 search tests passed");
    return 0;
}
