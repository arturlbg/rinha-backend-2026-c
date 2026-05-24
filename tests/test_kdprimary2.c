#include "kdprimary2.h"

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

static uint8_t brute_fraud_count(const int16_t *vectors,
                                 const uint8_t *labels,
                                 uint32_t count,
                                 const int16_t query[IVF8_INDEX_DIMS]) {
    Ivf8Neighbor top[KDPRIMARY2_TOP_K];
    ivf8_top5_init(top);
    for (uint32_t i = 0; i < count; i++) {
        Ivf8Neighbor candidate = {
            .distance = kdprimary2_distance14(query, vectors + (size_t)i * IVF8_INDEX_DIMS),
            .fraud = labels[i] != 0 ? 1u : 0u,
            .seq = i,
        };
        ivf8_top5_insert(top, candidate);
    }
    return ivf8_top5_fraud_count(top);
}

static void test_distance_and_bbox(void) {
    int16_t a[IVF8_INDEX_DIMS] = {0};
    int16_t b[IVF8_INDEX_DIMS] = {0};
    a[0] = 3;
    a[1] = -4;
    CHECK(kdprimary2_distance14(a, b) == 25);

    KdPrimary2Node node;
    memset(&node, 0, sizeof(node));
    for (uint32_t dim = 0; dim < IVF8_INDEX_DIMS; dim++) {
        node.bbox_min[dim] = 0;
        node.bbox_max[dim] = 10;
    }
    int16_t query[IVF8_INDEX_DIMS] = {0};
    query[0] = -3;
    query[1] = 14;
    CHECK(kdprimary2_bbox_distance(&node, query) == 25);
}

static void test_build_save_load_search(void) {
    int16_t vectors[17 * IVF8_INDEX_DIMS];
    uint8_t labels[17] = {0, 1, 0, 1, 1, 0, 0, 1, 0, 1, 0, 1, 1, 0, 1, 0, 1};
    set_point(vectors, 0, 0, 0);
    set_point(vectors, 1, 10, 0);
    set_point(vectors, 2, 0, 10);
    set_point(vectors, 3, 10, 10);
    set_point(vectors, 4, -10, 0);
    set_point(vectors, 5, 0, -10);
    set_point(vectors, 6, -10, -10);
    set_point(vectors, 7, 30, 30);
    set_point(vectors, 8, -30, -30);
    set_point(vectors, 9, 4, 4);
    set_point(vectors, 10, 5, 5);
    set_point(vectors, 11, 6, 6);
    set_point(vectors, 12, 40, -2);
    set_point(vectors, 13, -2, 40);
    set_point(vectors, 14, 25, 25);
    set_point(vectors, 15, -25, -25);
    set_point(vectors, 16, 7, 7);

    char err[256];
    KdPrimary2Build build;
    CHECK(kdprimary2_build_from_points(&build, vectors, labels, 17, 4, err, sizeof(err)) == 0);
    CHECK(build.count == 17);
    CHECK(build.leaf_size == 4);
    CHECK(build.node_count > 0);
    CHECK(build.block_count > 0);
    CHECK(kdprimary2_build_memory_bytes(&build) > 0);

    const char *path = "/tmp/kdprimary2-test.bin";
    CHECK(kdprimary2_save(&build, path, err, sizeof(err)) == 0);

    KdPrimary2Index index;
    CHECK(kdprimary2_open(path, &index, err, sizeof(err)) == 0);
    CHECK(index.count == 17);
    CHECK(index.leaf_size == 4);
    CHECK(index.file_size == kdprimary2_expected_file_bytes(index.node_count, index.block_count));

    int16_t queries[6][IVF8_INDEX_DIMS];
    memset(queries, 0, sizeof(queries));
    queries[0][0] = 1;
    queries[0][1] = 1;
    queries[1][0] = 9;
    queries[1][1] = 9;
    queries[2][0] = -8;
    queries[2][1] = -8;
    queries[3][0] = 25;
    queries[3][1] = 25;
    queries[4][0] = 35;
    queries[4][1] = 0;
    queries[5][0] = 7;
    queries[5][1] = 7;

    for (uint32_t i = 0; i < 6; i++) {
        KdPrimary2SearchResult result = kdprimary2_search_top5(&index, queries[i]);
        CHECK(result.fraud_count == brute_fraud_count(vectors, labels, 17, queries[i]));
        CHECK(result.stats.nodes_visited > 0);
        CHECK(result.stats.leaves_visited > 0);
        CHECK(result.stats.points_evaluated > 0);
    }

    kdprimary2_close(&index);
    kdprimary2_build_free(&build);
    (void)unlink(path);
}

static void test_reject_wrong_magic(void) {
    const char *path = "/tmp/kdprimary2-bad.bin";
    FILE *file = fopen(path, "wb");
    CHECK(file != NULL);
    if (file != NULL) {
        char bytes[KDPRIMARY2_HEADER_BYTES];
        memset(bytes, 0, sizeof(bytes));
        memcpy(bytes, "BADMAGIC", 8);
        CHECK(fwrite(bytes, sizeof(bytes), 1, file) == 1);
        fclose(file);
    }
    char err[256];
    KdPrimary2Index index;
    CHECK(kdprimary2_open(path, &index, err, sizeof(err)) != 0);
    (void)unlink(path);
}

static void test_avx2_leaf_distances_ignore_phantoms(void) {
    int16_t block[IVF8_INDEX_DIMS * IVF8_INDEX_LANES];
    memset(block, 0, sizeof(block));
    for (uint32_t dim = 0; dim < IVF8_INDEX_DIMS; dim++) {
        for (uint32_t lane = 0; lane < IVF8_INDEX_LANES; lane++) {
            block[dim * IVF8_INDEX_LANES + lane] = (int16_t)(dim + lane);
        }
    }
    int16_t query[IVF8_INDEX_DIMS] = {0};
    uint64_t distances[IVF8_INDEX_LANES];
    kdprimary2_leaf_block_distances_avx2(block, 0, query, distances);
    for (uint32_t lane = 0; lane < IVF8_INDEX_LANES; lane++) {
        uint64_t scalar = 0;
        for (uint32_t dim = 0; dim < IVF8_INDEX_DIMS; dim++) {
            int64_t diff = -(int64_t)block[dim * IVF8_INDEX_LANES + lane];
            scalar += (uint64_t)(diff * diff);
        }
        CHECK(distances[lane] == scalar);
    }
}

int main(void) {
    test_distance_and_bbox();
    test_build_save_load_search();
    test_reject_wrong_magic();
    test_avx2_leaf_distances_ignore_phantoms();

    if (failures != 0) {
        fprintf(stderr, "%d test failure(s)\n", failures);
        return 1;
    }
    puts("kdprimary2 tests passed");
    return 0;
}
