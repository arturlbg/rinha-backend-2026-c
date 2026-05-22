#include "kdtree.h"

#include <stdio.h>
#include <string.h>

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
    Ivf8Neighbor top[KDTREE_TOP_K];
    kdtree_top5_init(top);
    for (uint32_t i = 0; i < count; i++) {
        Ivf8Neighbor candidate = {
            .distance = kdtree_distance14(query, vectors + (size_t)i * IVF8_INDEX_DIMS),
            .fraud = labels[i] != 0 ? 1u : 0u,
            .seq = i,
        };
        kdtree_top5_insert(top, candidate);
    }
    return kdtree_top5_fraud_count(top);
}

static void test_distance(void) {
    int16_t a[IVF8_INDEX_DIMS] = {0};
    int16_t b[IVF8_INDEX_DIMS] = {0};
    a[0] = 3;
    a[1] = -4;
    b[0] = 0;
    b[1] = 0;
    CHECK(kdtree_distance14(a, b) == 25);
}

static void test_top5_ordering(void) {
    Ivf8Neighbor top[KDTREE_TOP_K];
    kdtree_top5_init(top);
    kdtree_top5_insert(top, (Ivf8Neighbor){.distance = 10, .fraud = 0, .seq = 10});
    kdtree_top5_insert(top, (Ivf8Neighbor){.distance = 5, .fraud = 1, .seq = 5});
    kdtree_top5_insert(top, (Ivf8Neighbor){.distance = 5, .fraud = 0, .seq = 3});
    CHECK(top[0].distance == 5);
    CHECK(top[0].seq == 3);
    CHECK(top[1].distance == 5);
    CHECK(top[1].seq == 5);
}

static void test_kdtree_equals_bruteforce(void) {
    int16_t vectors[12 * IVF8_INDEX_DIMS];
    uint8_t labels[12] = {0, 1, 0, 1, 1, 0, 0, 1, 0, 1, 0, 1};
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

    KdTree tree;
    CHECK(kdtree_build_from_points(&tree, vectors, labels, 12) == 0);
    CHECK(tree.node_count == 12);
    CHECK(kdtree_runtime_memory_bytes(&tree) > 0);

    int16_t queries[4][IVF8_INDEX_DIMS];
    memset(queries, 0, sizeof(queries));
    queries[0][0] = 1;
    queries[0][1] = 1;
    queries[1][0] = 9;
    queries[1][1] = 9;
    queries[2][0] = -8;
    queries[2][1] = -8;
    queries[3][0] = 25;
    queries[3][1] = 25;

    for (uint32_t i = 0; i < 4; i++) {
        KdTreeSearchResult result = kdtree_search_top5(&tree, queries[i], NULL);
        CHECK(result.fraud_count == brute_fraud_count(vectors, labels, 12, queries[i]));
        CHECK(result.stats.nodes_visited > 0);
        CHECK(result.stats.distance_evaluations == result.stats.nodes_visited);
    }
    kdtree_free(&tree);
}

static void test_approx_limit(void) {
    int16_t vectors[8 * IVF8_INDEX_DIMS];
    uint8_t labels[8] = {0};
    for (uint32_t i = 0; i < 8; i++) {
        set_point(vectors, i, (int16_t)(i * 10), 0);
    }
    KdTree tree;
    CHECK(kdtree_build_from_points(&tree, vectors, labels, 8) == 0);
    int16_t query[IVF8_INDEX_DIMS] = {0};
    KdTreeSearchConfig cfg = {.max_visited = 3};
    KdTreeSearchResult result = kdtree_search_top5(&tree, query, &cfg);
    CHECK(result.stats.nodes_visited <= 3);
    kdtree_free(&tree);
}

int main(void) {
    test_distance();
    test_top5_ordering();
    test_kdtree_equals_bruteforce();
    test_approx_limit();

    if (failures != 0) {
        fprintf(stderr, "%d test failure(s)\n", failures);
        return 1;
    }
    puts("kdtree tests passed");
    return 0;
}
