#include "kdprimary.h"

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
    Ivf8Neighbor top[KDPRIMARY_TOP_K];
    ivf8_top5_init(top);
    for (uint32_t i = 0; i < count; i++) {
        Ivf8Neighbor candidate = {
            .distance = kdprimary_distance14(query, vectors + (size_t)i * IVF8_INDEX_DIMS),
            .fraud = labels[i] != 0 ? 1u : 0u,
            .seq = i,
        };
        ivf8_top5_insert(top, candidate);
    }
    return ivf8_top5_fraud_count(top);
}

static void test_distance(void) {
    int16_t a[IVF8_INDEX_DIMS] = {0};
    int16_t b[IVF8_INDEX_DIMS] = {0};
    a[0] = 3;
    a[1] = -4;
    CHECK(kdprimary_distance14(a, b) == 25);
}

static void test_build_save_load_search(void) {
    int16_t vectors[16 * IVF8_INDEX_DIMS];
    uint8_t labels[16] = {0, 1, 0, 1, 1, 0, 0, 1, 0, 1, 0, 1, 1, 0, 1, 0};
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

    char err[256];
    KdPrimaryBuild build;
    CHECK(kdprimary_build_from_points(&build, vectors, labels, 16, 4, err, sizeof(err)) == 0);
    CHECK(build.count == 16);
    CHECK(build.leaf_size == 4);
    CHECK(build.node_count > 0);
    CHECK(build.node_count < build.count);
    CHECK(kdprimary_build_memory_bytes(&build) > 0);

    const char *path = "/tmp/kdprimary-test.bin";
    CHECK(kdprimary_save(&build, path, err, sizeof(err)) == 0);

    KdPrimaryIndex index;
    CHECK(kdprimary_open(path, &index, err, sizeof(err)) == 0);
    CHECK(index.count == 16);
    CHECK(index.leaf_size == 4);
    CHECK(index.file_size == kdprimary_expected_file_bytes(index.count, index.node_count));

    int16_t queries[5][IVF8_INDEX_DIMS];
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

    for (uint32_t i = 0; i < 5; i++) {
        KdPrimarySearchResult result = kdprimary_search_top5(&index, queries[i]);
        CHECK(result.fraud_count == brute_fraud_count(index.vectors, index.labels, index.count, queries[i]));
        CHECK(result.stats.nodes_visited > 0);
        CHECK(result.stats.leaves_visited > 0);
        CHECK(result.stats.points_evaluated > 0);
    }

    kdprimary_close(&index);
    kdprimary_build_free(&build);
    (void)unlink(path);
}

static void test_reject_wrong_magic(void) {
    const char *path = "/tmp/kdprimary-bad.bin";
    FILE *file = fopen(path, "wb");
    CHECK(file != NULL);
    if (file != NULL) {
        char bytes[KDPRIMARY_HEADER_BYTES];
        memset(bytes, 0, sizeof(bytes));
        memcpy(bytes, "BADMAGIC", 8);
        CHECK(fwrite(bytes, sizeof(bytes), 1, file) == 1);
        fclose(file);
    }
    char err[256];
    KdPrimaryIndex index;
    CHECK(kdprimary_open(path, &index, err, sizeof(err)) != 0);
    (void)unlink(path);
}

static void test_point_node_mode(void) {
    int16_t vectors[8 * IVF8_INDEX_DIMS];
    uint8_t labels[8] = {0, 1, 0, 1, 1, 0, 0, 1};
    for (uint32_t i = 0; i < 8; i++) {
        set_point(vectors, i, (int16_t)(i * 7), (int16_t)(20 - (int32_t)i * 3));
    }

    char err[256];
    KdPrimaryBuild build;
    CHECK(kdprimary_build_from_points(&build, vectors, labels, 8, 1, err, sizeof(err)) == 0);
    CHECK(build.node_count == 8);
    CHECK(build.count == 8);

    const char *path = "/tmp/kdprimary-point-test.bin";
    CHECK(kdprimary_save(&build, path, err, sizeof(err)) == 0);

    KdPrimaryIndex index;
    CHECK(kdprimary_open(path, &index, err, sizeof(err)) == 0);
    CHECK(index.leaf_size == 1);
    CHECK(index.node_count == 8);

    int16_t query[IVF8_INDEX_DIMS] = {0};
    query[0] = 14;
    query[1] = 14;
    KdPrimarySearchResult result = kdprimary_search_top5(&index, query);
    CHECK(result.fraud_count == brute_fraud_count(index.vectors, index.labels, index.count, query));
    CHECK(result.stats.points_evaluated > 0);

    kdprimary_close(&index);
    kdprimary_build_free(&build);
    (void)unlink(path);
}

int main(void) {
    test_distance();
    test_build_save_load_search();
    test_reject_wrong_magic();
    test_point_node_mode();

    if (failures != 0) {
        fprintf(stderr, "%d test failure(s)\n", failures);
        return 1;
    }
    puts("kdprimary tests passed");
    return 0;
}
