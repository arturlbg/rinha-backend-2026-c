#include "fastvector.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        failures++; \
    } \
} while (0)

static void check_vector(const char *name, const char *json, const int16_t expected[FASTVECTOR_DIMENSIONS]) {
    int16_t got[FASTVECTOR_DIMENSIONS];
    CHECK(fastvector_vectorize(json, strlen(json), got));
    for (int i = 0; i < FASTVECTOR_DIMENSIONS; i++) {
        if (got[i] != expected[i]) {
            fprintf(stderr, "FAIL %s dim %d: got %d expected %d\n", name, i, got[i], expected[i]);
            failures++;
        }
    }
}

static void test_quantization(void) {
    CHECK(fastvector_quantize(0.0f) == 0);
    CHECK(fastvector_quantize(1.0f) == 10000);
    CHECK(fastvector_quantize(0.5f) == 5000);
    CHECK(fastvector_quantize(-1.0f) == -10000);
}

static void test_timestamp(void) {
    fastvector_timestamp ts;
    CHECK(fastvector_parse_timestamp("2026-03-16T00:00:00Z", 20, &ts));
    CHECK(ts.hour == 0);
    CHECK(ts.weekday == 0);
    CHECK(fastvector_quantize((float)ts.hour / 23.0f) == 0);

    CHECK(fastvector_parse_timestamp("2026-03-15T23:00:00Z", 20, &ts));
    CHECK(ts.hour == 23);
    CHECK(ts.weekday == 6);
    CHECK(fastvector_quantize((float)ts.hour / 23.0f) == 10000);
    CHECK(fastvector_quantize((float)ts.weekday / 6.0f) == 10000);
}

static void test_mcc(void) {
    CHECK(fastvector_mcc_risk_quantized("5411", 4) == 1500);
    CHECK(fastvector_mcc_risk_quantized("7802", 4) == 7500);
    CHECK(fastvector_mcc_risk_quantized("7995", 4) == 8500);
    CHECK(fastvector_mcc_risk_quantized("0000", 4) == 5000);
}

static void test_golden_vectors(void) {
    const char *legit_docs =
        "{\"id\":\"tx-1329056812\","
        "\"transaction\":{\"amount\":41.12,\"installments\":2,\"requested_at\":\"2026-03-11T18:45:53Z\"},"
        "\"customer\":{\"avg_amount\":82.24,\"tx_count_24h\":3,\"known_merchants\":[\"MERC-003\",\"MERC-016\"]},"
        "\"merchant\":{\"id\":\"MERC-016\",\"mcc\":\"5411\",\"avg_amount\":60.25},"
        "\"terminal\":{\"is_online\":false,\"card_present\":true,\"km_from_home\":29.23},"
        "\"last_transaction\":null}";
    const int16_t legit_docs_expected[FASTVECTOR_DIMENSIONS] =
        {41, 1667, 500, 7826, 3333, -10000, -10000, 292, 1500, 0, 10000, 0, 1500, 60};
    check_vector("legit_docs", legit_docs, legit_docs_expected);

    const char *fraud_docs =
        "{\"id\":\"tx-3330991687\","
        "\"transaction\":{\"amount\":9505.97,\"installments\":10,\"requested_at\":\"2026-03-14T05:15:12Z\"},"
        "\"customer\":{\"avg_amount\":81.28,\"tx_count_24h\":20,\"known_merchants\":[\"MERC-008\",\"MERC-007\",\"MERC-005\"]},"
        "\"merchant\":{\"id\":\"MERC-068\",\"mcc\":\"7802\",\"avg_amount\":54.86},"
        "\"terminal\":{\"is_online\":false,\"card_present\":true,\"km_from_home\":952.27},"
        "\"last_transaction\":null}";
    const int16_t fraud_docs_expected[FASTVECTOR_DIMENSIONS] =
        {9506, 8333, 10000, 2174, 8333, -10000, -10000, 9523, 10000, 0, 10000, 10000, 7500, 55};
    check_vector("fraud_docs", fraud_docs, fraud_docs_expected);

    const char *nonnull_last =
        "{\"id\":\"tx-a\","
        "\"transaction\":{\"amount\":1000,\"installments\":12,\"requested_at\":\"2026-03-16T23:00:00Z\"},"
        "\"customer\":{\"avg_amount\":1000,\"tx_count_24h\":10,\"known_merchants\":[\"A\",\"B\"]},"
        "\"merchant\":{\"id\":\"MERC-X\",\"mcc\":\"9999\",\"avg_amount\":5000},"
        "\"terminal\":{\"is_online\":true,\"card_present\":false,\"km_from_home\":100},"
        "\"last_transaction\":{\"timestamp\":\"2026-03-16T22:00:00Z\",\"km_from_current\":50}}";
    const int16_t nonnull_last_expected[FASTVECTOR_DIMENSIONS] =
        {1000, 10000, 1000, 10000, 0, 417, 500, 1000, 5000, 10000, 0, 10000, 5000, 5000};
    check_vector("nonnull_last", nonnull_last, nonnull_last_expected);

    const char *duplicates_known =
        "{\"id\":\"tx-b\","
        "\"transaction\":{\"amount\":0,\"installments\":0,\"requested_at\":\"2026-03-15T00:00:00Z\"},"
        "\"customer\":{\"avg_amount\":0,\"tx_count_24h\":0,\"known_merchants\":[\"MERC-D\",\"MERC-D\",\"X\"]},"
        "\"merchant\":{\"id\":\"MERC-D\",\"mcc\":\"7995\",\"avg_amount\":0},"
        "\"terminal\":{\"is_online\":false,\"card_present\":false,\"km_from_home\":0},"
        "\"last_transaction\":{\"timestamp\":\"2026-03-15T00:00:00Z\",\"km_from_current\":0}}";
    const int16_t duplicates_known_expected[FASTVECTOR_DIMENSIONS] =
        {0, 0, 0, 0, 10000, 0, 0, 0, 0, 0, 0, 0, 8500, 0};
    check_vector("duplicates_known", duplicates_known, duplicates_known_expected);

    const char *clamped =
        "{\"id\":\"tx-c\","
        "\"transaction\":{\"amount\":20000,\"installments\":20,\"requested_at\":\"2026-03-17T12:00:00Z\"},"
        "\"customer\":{\"avg_amount\":100,\"tx_count_24h\":30,\"known_merchants\":[\"KNOWN\"]},"
        "\"merchant\":{\"id\":\"UNKNOWN\",\"mcc\":\"4511\",\"avg_amount\":20000},"
        "\"terminal\":{\"is_online\":true,\"card_present\":true,\"km_from_home\":1500},"
        "\"last_transaction\":{\"timestamp\":\"2026-03-16T12:00:00Z\",\"km_from_current\":2000}}";
    const int16_t clamped_expected[FASTVECTOR_DIMENSIONS] =
        {10000, 10000, 10000, 5217, 1667, 10000, 10000, 10000, 10000, 10000, 10000, 10000, 3500, 10000};
    check_vector("clamped", clamped, clamped_expected);

    const char *avg_zero_positive_amount =
        "{\"id\":\"tx-d\","
        "\"transaction\":{\"amount\":500,\"installments\":6,\"requested_at\":\"2026-03-18T06:00:00Z\"},"
        "\"customer\":{\"avg_amount\":0,\"tx_count_24h\":5,\"known_merchants\":[\"M1\",\"M2\"]},"
        "\"merchant\":{\"id\":\"M2\",\"mcc\":\"5812\",\"avg_amount\":250},"
        "\"terminal\":{\"is_online\":true,\"card_present\":true,\"km_from_home\":250},"
        "\"last_transaction\":null}";
    const int16_t avg_zero_positive_amount_expected[FASTVECTOR_DIMENSIONS] =
        {500, 5000, 10000, 2609, 3333, -10000, -10000, 2500, 2500, 10000, 10000, 0, 3000, 250};
    check_vector("avg_zero_positive_amount", avg_zero_positive_amount, avg_zero_positive_amount_expected);

    const char *negative_clamp =
        "{\"id\":\"tx-e\","
        "\"transaction\":{\"amount\":-5,\"installments\":-1,\"requested_at\":\"2026-03-19T01:00:00Z\"},"
        "\"customer\":{\"avg_amount\":100,\"tx_count_24h\":-1,\"known_merchants\":[\"A\"]},"
        "\"merchant\":{\"id\":\"B\",\"mcc\":\"5311\",\"avg_amount\":-100},"
        "\"terminal\":{\"is_online\":false,\"card_present\":true,\"km_from_home\":-5},"
        "\"last_transaction\":{\"timestamp\":\"2026-03-19T00:00:00Z\",\"km_from_current\":-10}}";
    const int16_t negative_clamp_expected[FASTVECTOR_DIMENSIONS] =
        {0, 0, 0, 435, 5000, 417, 0, 0, 0, 0, 10000, 10000, 2500, 0};
    check_vector("negative_clamp", negative_clamp, negative_clamp_expected);

    const char *mcc_5999 =
        "{\"id\":\"tx-f\","
        "\"transaction\":{\"amount\":1234.56,\"installments\":3,\"requested_at\":\"2026-03-20T09:00:00Z\"},"
        "\"customer\":{\"avg_amount\":617.28,\"tx_count_24h\":1,\"known_merchants\":[]},"
        "\"merchant\":{\"id\":\"M\",\"mcc\":\"5999\",\"avg_amount\":9999.9},"
        "\"terminal\":{\"is_online\":false,\"card_present\":false,\"km_from_home\":12.34},"
        "\"last_transaction\":{\"timestamp\":\"2026-03-19T09:00:00Z\",\"km_from_current\":123.4}}";
    const int16_t mcc_5999_expected[FASTVECTOR_DIMENSIONS] =
        {1235, 2500, 2000, 3913, 6667, 10000, 1234, 123, 500, 0, 0, 10000, 5000, 10000};
    check_vector("mcc_5999", mcc_5999, mcc_5999_expected);

    const char *whitespace_payload =
        "{ \"id\" : \"tx-g\", "
        "\"transaction\" : { \"amount\" : 750, \"installments\" : 1, \"requested_at\" : \"2026-03-21T15:00:00Z\" },"
        "\"customer\" : { \"avg_amount\" : 1500, \"tx_count_24h\" : 2, \"known_merchants\" : [ \"MER-1\", \"MER-2\" ] },"
        "\"merchant\" : { \"id\" : \"MER-2\", \"mcc\" : \"5944\", \"avg_amount\" : 1200 },"
        "\"terminal\" : { \"is_online\" : true, \"card_present\" : false, \"km_from_home\" : 333.33 },"
        "\"last_transaction\" : null }";
    const int16_t whitespace_payload_expected[FASTVECTOR_DIMENSIONS] =
        {750, 833, 500, 6522, 8333, -10000, -10000, 3333, 1000, 10000, 0, 0, 4500, 1200};
    check_vector("whitespace_payload", whitespace_payload, whitespace_payload_expected);

    const char *short_last_gap =
        "{\"id\":\"tx-h\","
        "\"transaction\":{\"amount\":1,\"installments\":1,\"requested_at\":\"2026-03-22T22:00:00Z\"},"
        "\"customer\":{\"avg_amount\":0,\"tx_count_24h\":19,\"known_merchants\":[\"KNOWN\"]},"
        "\"merchant\":{\"id\":\"OTHER\",\"mcc\":\"7801\",\"avg_amount\":1},"
        "\"terminal\":{\"is_online\":false,\"card_present\":true,\"km_from_home\":0.5},"
        "\"last_transaction\":{\"timestamp\":\"2026-03-22T21:30:00Z\",\"km_from_current\":0.5}}";
    const int16_t short_last_gap_expected[FASTVECTOR_DIMENSIONS] =
        {1, 833, 10000, 9565, 10000, 208, 5, 5, 9500, 0, 10000, 10000, 8000, 1};
    check_vector("short_last_gap", short_last_gap, short_last_gap_expected);
}

static void test_null_and_nonnull_last_transaction(void) {
    int16_t out[FASTVECTOR_DIMENSIONS];
    const char *null_payload =
        "{\"transaction\":{\"amount\":10,\"installments\":1,\"requested_at\":\"2026-03-16T10:00:00Z\"},"
        "\"customer\":{\"avg_amount\":10,\"tx_count_24h\":1,\"known_merchants\":[]},"
        "\"merchant\":{\"id\":\"M\",\"mcc\":\"0000\",\"avg_amount\":10},"
        "\"terminal\":{\"is_online\":false,\"card_present\":false,\"km_from_home\":0},"
        "\"last_transaction\":null}";
    CHECK(fastvector_vectorize(null_payload, strlen(null_payload), out));
    CHECK(out[FASTVECTOR_DIM_MINUTES_SINCE_LAST_TX] == FASTVECTOR_SENTINEL);
    CHECK(out[FASTVECTOR_DIM_KM_FROM_LAST_TX] == FASTVECTOR_SENTINEL);

    const char *nonnull_payload =
        "{\"transaction\":{\"amount\":10,\"installments\":1,\"requested_at\":\"2026-03-16T10:00:00Z\"},"
        "\"customer\":{\"avg_amount\":10,\"tx_count_24h\":1,\"known_merchants\":[]},"
        "\"merchant\":{\"id\":\"M\",\"mcc\":\"0000\",\"avg_amount\":10},"
        "\"terminal\":{\"is_online\":false,\"card_present\":false,\"km_from_home\":0},"
        "\"last_transaction\":{\"timestamp\":\"2026-03-16T09:00:00Z\",\"km_from_current\":1}}";
    CHECK(fastvector_vectorize(nonnull_payload, strlen(nonnull_payload), out));
    CHECK(out[FASTVECTOR_DIM_MINUTES_SINCE_LAST_TX] != FASTVECTOR_SENTINEL);
    CHECK(out[FASTVECTOR_DIM_KM_FROM_LAST_TX] != FASTVECTOR_SENTINEL);
}

static void test_invalid_payload(void) {
    int16_t out[FASTVECTOR_DIMENSIONS];
    const char *missing = "{\"transaction\":{}}";
    CHECK(!fastvector_vectorize(missing, strlen(missing), out));
}

int main(void) {
    test_quantization();
    test_timestamp();
    test_mcc();
    test_golden_vectors();
    test_null_and_nonnull_last_transaction();
    test_invalid_payload();

    if (failures != 0) {
        fprintf(stderr, "%d fastvector test failure(s)\n", failures);
        return 1;
    }
    puts("fastvector tests passed");
    return 0;
}
