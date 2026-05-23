#include "kdtree_repair.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        failures++; \
    } \
} while (0)

static Ivf8SearchTraceResult make_trace(uint8_t fraud_count, uint64_t worst_distance) {
    Ivf8SearchTraceResult trace;
    memset(&trace, 0, sizeof(trace));
    trace.result.fraud_count = fraud_count;
    for (uint32_t i = 0; i < IVF8_SEARCH_TOP_K; i++) {
        trace.top[i].distance = (uint64_t)i;
    }
    trace.top[IVF8_SEARCH_TOP_K - 1u].distance = worst_distance;
    return trace;
}

static void test_policy_parse(void) {
    KdTreeRepairPolicy policy;
    CHECK(kdtree_repair_policy_from_string("boundary23_far45", &policy));
    CHECK(policy == KDTREE_REPAIR_POLICY_BOUNDARY23_FAR45);
    CHECK(kdtree_repair_policy_from_string("", &policy));
    CHECK(!kdtree_repair_policy_from_string("other", &policy));
    CHECK(strcmp(kdtree_repair_policy_name(policy), "boundary23_far45") == 0);
}

static void test_boundary23_far45(void) {
    KdTreeRepairPolicy policy = KDTREE_REPAIR_POLICY_BOUNDARY23_FAR45;
    Ivf8SearchTraceResult trace = {0};
    CHECK(!kdtree_repair_should_run(policy, &trace));
    trace = make_trace(2, 1);
    CHECK(kdtree_repair_should_run(policy, &trace));
    trace = make_trace(3, 1);
    CHECK(kdtree_repair_should_run(policy, &trace));
    trace = make_trace(4, KDTREE_REPAIR_BOUNDARY23_FAR45_THRESHOLD - 1u);
    CHECK(!kdtree_repair_should_run(policy, &trace));
    trace = make_trace(4, KDTREE_REPAIR_BOUNDARY23_FAR45_THRESHOLD);
    CHECK(kdtree_repair_should_run(policy, &trace));
    trace = make_trace(5, KDTREE_REPAIR_BOUNDARY23_FAR45_THRESHOLD - 1u);
    CHECK(!kdtree_repair_should_run(policy, &trace));
    trace = make_trace(5, KDTREE_REPAIR_BOUNDARY23_FAR45_THRESHOLD);
    CHECK(kdtree_repair_should_run(policy, &trace));
    trace = make_trace(0, UINT64_MAX);
    CHECK(!kdtree_repair_should_run(policy, &trace));
    trace = make_trace(1, UINT64_MAX);
    CHECK(!kdtree_repair_should_run(policy, &trace));
}

int main(void) {
    test_policy_parse();
    test_boundary23_far45();

    if (failures != 0) {
        fprintf(stderr, "%d kdtree repair test failure(s)\n", failures);
        return 1;
    }
    puts("kdtree repair tests passed");
    return 0;
}
