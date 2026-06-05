#include "kdclass3_opt.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int failures = 0;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        failures++; \
    } \
} while (0)

static void set_point(int16_t *vectors, uint32_t idx, int16_t x, int16_t y) {
    int16_t *point = vectors + (size_t)idx * IVF8_INDEX_DIMS;
    memset(point, 0, IVF8_INDEX_DIMS * sizeof(int16_t));
    point[0] = x;
    point[1] = y;
}

static void compare_results(const KdClass3SearchResult *baseline,
                            const KdClass3OptSearchResult *optimized) {
    CHECK(baseline->fraud_count == optimized->fraud_count);
    CHECK(baseline->fallback_required == optimized->fallback_required);
    CHECK(baseline->fraud_distance3 == optimized->fraud_distance3);
    CHECK(baseline->legit_distance3 == optimized->legit_distance3);
}

static void test_search_parity(void) {
    int16_t vectors[12 * IVF8_INDEX_DIMS];
    uint8_t labels[12] = {1, 1, 1, 0, 0, 0, 1, 0, 1, 0, 1, 0};
    for (uint32_t i = 0; i < 12; i++) {
        set_point(vectors, i, (int16_t)(i * 3u), (int16_t)(20 - (int)i));
    }

    char err[256];
    const char *path = "/tmp/kdclass3-opt-parity.bin";
    KdClass3Build build;
    KdClass3Index index;
    CHECK(kdclass3_build_from_points(&build, vectors, labels, 12, 3, err, sizeof(err)) == 0);
    CHECK(kdclass3_save(&build, path, err, sizeof(err)) == 0);
    CHECK(kdclass3_open(path, &index, err, sizeof(err)) == 0);

    for (int x = -4; x <= 40; x++) {
        int16_t query[IVF8_INDEX_DIMS] = {0};
        query[0] = (int16_t)x;
        query[1] = 8;
        KdClass3SearchResult baseline = kdclass3_search(&index, query);
        for (KdClass3OptMode mode = KDCLASS3_OPT_BBOX_ONLY;
             mode <= KDCLASS3_OPT_SIMD_BBOX_FULL;
             mode = (KdClass3OptMode)(mode + 1)) {
            KdClass3OptSearchResult optimized =
                kdclass3_opt_search_mode(&index, query, mode);
            compare_results(&baseline, &optimized);
        }
    }

    kdclass3_close(&index);
    kdclass3_build_free(&build);
    (void)unlink(path);
}

static void test_equal_distance_fallback_parity(void) {
    int16_t vectors[6 * IVF8_INDEX_DIMS];
    uint8_t labels[6] = {1, 1, 1, 0, 0, 0};
    set_point(vectors, 0, -1, 0);
    set_point(vectors, 1, 0, 0);
    set_point(vectors, 2, 1, 0);
    set_point(vectors, 3, -1, 0);
    set_point(vectors, 4, 0, 0);
    set_point(vectors, 5, 1, 0);

    char err[256];
    const char *path = "/tmp/kdclass3-opt-tie.bin";
    KdClass3Build build;
    KdClass3Index index;
    CHECK(kdclass3_build_from_points(&build, vectors, labels, 6, 2, err, sizeof(err)) == 0);
    CHECK(kdclass3_save(&build, path, err, sizeof(err)) == 0);
    CHECK(kdclass3_open(path, &index, err, sizeof(err)) == 0);

    int16_t query[IVF8_INDEX_DIMS] = {0};
    KdClass3SearchResult baseline = kdclass3_search(&index, query);
    for (KdClass3OptMode mode = KDCLASS3_OPT_BBOX_ONLY;
         mode <= KDCLASS3_OPT_SIMD_BBOX_FULL;
         mode = (KdClass3OptMode)(mode + 1)) {
        KdClass3OptSearchResult optimized =
            kdclass3_opt_search_mode(&index, query, mode);
        compare_results(&baseline, &optimized);
        CHECK(optimized.fallback_required);
    }

    kdclass3_close(&index);
    kdclass3_build_free(&build);
    (void)unlink(path);
}

static void test_checkpoint_distances(void) {
    int16_t block[IVF8_INDEX_DIMS * IVF8_INDEX_LANES] = {0};
    int16_t query[IVF8_INDEX_DIMS] = {0};
    for (uint32_t lane = 0; lane < IVF8_INDEX_LANES; lane++) {
        for (uint32_t dim = 0; dim < IVF8_INDEX_DIMS; dim++) {
            block[dim * IVF8_INDEX_LANES + lane] =
                (int16_t)(lane * 10u + dim);
        }
    }

    uint64_t full[IVF8_INDEX_LANES];
    uint64_t checkpoint[IVF8_INDEX_LANES];
    ivf8_block_distances_avx2(block, 0, query, full);

    uint32_t dims = 0;
    uint32_t mask = kdclass3_opt_block_distances_avx2(
        block, 0, query, UINT64_MAX, checkpoint, &dims);
    CHECK(mask == 0xffu);
    CHECK(dims == IVF8_INDEX_DIMS);
    CHECK(memcmp(full, checkpoint, sizeof(full)) == 0);

    mask = kdclass3_opt_block_distances_avx2(
        block, 0, query, full[3], checkpoint, &dims);
    for (uint32_t lane = 0; lane < IVF8_INDEX_LANES; lane++) {
        bool expected = full[lane] <= full[3];
        CHECK(((mask >> lane) & 1u) == (expected ? 1u : 0u));
    }
    CHECK((mask & (1u << 3u)) != 0u);

    mask = kdclass3_opt_block_distances_avx2(
        block, 0, query, 0, checkpoint, &dims);
    CHECK(mask == 0u);
    CHECK(dims < IVF8_INDEX_DIMS);
}

static void test_simd_bbox_distance(void) {
    KdClass3Node node;
    memset(&node, 0, sizeof(node));
    int16_t query[IVF8_INDEX_DIMS];
    for (uint32_t dim = 0; dim < IVF8_INDEX_DIMS; dim++) {
        node.bbox_min[dim] = (int16_t)(-200 + (int)dim);
        node.bbox_max[dim] = (int16_t)(100 + (int)dim * 2);
        query[dim] = (int16_t)(dim % 3u == 0u ? 500 : -500);
    }
    uint64_t expected = kdclass3_bbox_distance(&node, query);
    uint32_t dims = 0;
    CHECK(kdclass3_opt_bbox_distance_avx2(
              &node, query, UINT64_MAX, &dims) == expected);
    CHECK(dims == IVF8_INDEX_DIMS);
    uint64_t partial = kdclass3_opt_bbox_distance_avx2(
        &node, query, 0, &dims);
    CHECK(partial > 0u);
    CHECK(dims == 8u);
}

int main(void) {
    test_search_parity();
    test_equal_distance_fallback_parity();
    test_checkpoint_distances();
    test_simd_bbox_distance();
    if (failures != 0) {
        fprintf(stderr, "%d test failure(s)\n", failures);
        return 1;
    }
    puts("kdclass3 opt tests passed");
    return 0;
}
