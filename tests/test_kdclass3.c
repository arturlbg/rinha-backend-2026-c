#include "kdclass3.h"

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

static uint64_t brute_class_d3(const int16_t *vectors,
                               const uint8_t *labels,
                               uint32_t count,
                               uint8_t wanted_label,
                               const int16_t query[IVF8_INDEX_DIMS]) {
    KdClass3Neighbor top[KDCLASS3_TOP_K];
    for (uint32_t i = 0; i < KDCLASS3_TOP_K; i++) {
        top[i].distance = UINT64_MAX;
        top[i].seq = UINT32_MAX;
    }
    for (uint32_t i = 0; i < count; i++) {
        if ((labels[i] != 0 ? 1u : 0u) != wanted_label) {
            continue;
        }
        KdClass3Neighbor candidate = {
            .distance = kdclass3_distance14(query, vectors + (size_t)i * IVF8_INDEX_DIMS),
            .seq = i,
        };
        if (candidate.distance > top[KDCLASS3_TOP_K - 1u].distance ||
            (candidate.distance == top[KDCLASS3_TOP_K - 1u].distance &&
             candidate.seq >= top[KDCLASS3_TOP_K - 1u].seq)) {
            continue;
        }
        top[KDCLASS3_TOP_K - 1u] = candidate;
        for (uint32_t j = KDCLASS3_TOP_K - 1u; j > 0; j--) {
            bool better = top[j].distance < top[j - 1u].distance ||
                          (top[j].distance == top[j - 1u].distance && top[j].seq < top[j - 1u].seq);
            if (!better) {
                break;
            }
            KdClass3Neighbor tmp = top[j - 1u];
            top[j - 1u] = top[j];
            top[j] = tmp;
        }
    }
    return top[KDCLASS3_TOP_K - 1u].distance;
}

static uint8_t brute_top5_fraud_count(const int16_t *vectors,
                                      const uint8_t *labels,
                                      uint32_t count,
                                      const int16_t query[IVF8_INDEX_DIMS]) {
    Ivf8Neighbor top[IVF8_SEARCH_TOP_K];
    ivf8_top5_init(top);
    for (uint32_t i = 0; i < count; i++) {
        Ivf8Neighbor candidate = {
            .distance = kdclass3_distance14(query, vectors + (size_t)i * IVF8_INDEX_DIMS),
            .fraud = labels[i] != 0 ? 1u : 0u,
            .seq = i,
        };
        ivf8_top5_insert(top, candidate);
    }
    return ivf8_top5_fraud_count(top);
}

static void build_save_open(const int16_t *vectors,
                            const uint8_t *labels,
                            uint32_t count,
                            const char *path,
                            KdClass3Build *build,
                            KdClass3Index *index) {
    char err[256];
    CHECK(kdclass3_build_from_points(build, vectors, labels, count, 2, err, sizeof(err)) == 0);
    CHECK(build->leaf_size == 2);
    CHECK(build->fraud.count >= 3);
    CHECK(build->legit.count >= 3);
    CHECK(kdclass3_build_memory_bytes(build) > 0);
    CHECK(kdclass3_save(build, path, err, sizeof(err)) == 0);
    CHECK(kdclass3_open(path, index, err, sizeof(err)) == 0);
    CHECK(index->file_size == kdclass3_expected_file_bytes(index->fraud.node_count,
                                                           index->fraud.block_count,
                                                           index->legit.node_count,
                                                           index->legit.block_count));
}

static void test_clear_fraud_and_legit_majority(void) {
    int16_t vectors[8 * IVF8_INDEX_DIMS];
    uint8_t labels[8] = {1, 1, 1, 0, 0, 0, 1, 0};
    set_point(vectors, 0, 0, 0);
    set_point(vectors, 1, 1, 0);
    set_point(vectors, 2, 2, 0);
    set_point(vectors, 3, 10, 0);
    set_point(vectors, 4, 11, 0);
    set_point(vectors, 5, 12, 0);
    set_point(vectors, 6, 3, 0);
    set_point(vectors, 7, 13, 0);

    KdClass3Build build;
    KdClass3Index index;
    const char *path = "/tmp/kdclass3-majority.bin";
    build_save_open(vectors, labels, 8, path, &build, &index);

    int16_t fraud_query[IVF8_INDEX_DIMS] = {0};
    fraud_query[0] = 0;
    KdClass3SearchResult fraud = kdclass3_search(&index, fraud_query);
    CHECK(!fraud.fallback_required);
    CHECK(fraud.fraud_count == 3u);
    CHECK(fraud.fraud_distance3 < fraud.legit_distance3);
    CHECK(fraud.fraud_distance3 == brute_class_d3(vectors, labels, 8, 1u, fraud_query));
    CHECK(fraud.legit_distance3 == brute_class_d3(vectors, labels, 8, 0u, fraud_query));
    CHECK(brute_top5_fraud_count(vectors, labels, 8, fraud_query) >= 3u);

    int16_t legit_query[IVF8_INDEX_DIMS] = {0};
    legit_query[0] = 12;
    KdClass3SearchResult legit = kdclass3_search(&index, legit_query);
    CHECK(!legit.fallback_required);
    CHECK(legit.fraud_count == 0u);
    CHECK(legit.legit_distance3 < legit.fraud_distance3);
    CHECK(brute_top5_fraud_count(vectors, labels, 8, legit_query) < 3u);

    kdclass3_close(&index);
    kdclass3_build_free(&build);
    (void)unlink(path);
}

static void test_equal_distance_requires_fallback(void) {
    int16_t vectors[6 * IVF8_INDEX_DIMS];
    uint8_t labels[6] = {1, 1, 1, 0, 0, 0};
    set_point(vectors, 0, -1, 0);
    set_point(vectors, 1, 0, 0);
    set_point(vectors, 2, 1, 0);
    set_point(vectors, 3, -1, 0);
    set_point(vectors, 4, 0, 0);
    set_point(vectors, 5, 1, 0);

    KdClass3Build build;
    KdClass3Index index;
    const char *path = "/tmp/kdclass3-tie.bin";
    build_save_open(vectors, labels, 6, path, &build, &index);

    int16_t query[IVF8_INDEX_DIMS] = {0};
    KdClass3SearchResult result = kdclass3_search(&index, query);
    CHECK(result.fallback_required);
    CHECK(result.fraud_distance3 == result.legit_distance3);
    CHECK(brute_top5_fraud_count(vectors, labels, 6, query) <= 5u);

    kdclass3_close(&index);
    kdclass3_build_free(&build);
    (void)unlink(path);
}

static void test_reject_wrong_magic(void) {
    const char *path = "/tmp/kdclass3-bad.bin";
    FILE *file = fopen(path, "wb");
    CHECK(file != NULL);
    if (file != NULL) {
        char bytes[KDCLASS3_HEADER_BYTES];
        memset(bytes, 0, sizeof(bytes));
        memcpy(bytes, "BADMAGIC", 8);
        CHECK(fwrite(bytes, sizeof(bytes), 1, file) == 1);
        fclose(file);
    }
    char err[256];
    KdClass3Index index;
    CHECK(kdclass3_open(path, &index, err, sizeof(err)) != 0);
    (void)unlink(path);
}

int main(void) {
    test_clear_fraud_and_legit_majority();
    test_equal_distance_requires_fallback();
    test_reject_wrong_magic();

    if (failures != 0) {
        fprintf(stderr, "%d test failure(s)\n", failures);
        return 1;
    }
    puts("kdclass3 tests passed");
    return 0;
}
