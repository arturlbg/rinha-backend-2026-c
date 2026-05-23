#include "kdtree_repair.h"

#include <string.h>

bool kdtree_repair_policy_from_string(const char *value, KdTreeRepairPolicy *out) {
    if (value == NULL || value[0] == '\0' || strcmp(value, "boundary23_far45") == 0) {
        if (out != NULL) {
            *out = KDTREE_REPAIR_POLICY_BOUNDARY23_FAR45;
        }
        return true;
    }
    return false;
}

const char *kdtree_repair_policy_name(KdTreeRepairPolicy policy) {
    switch (policy) {
        case KDTREE_REPAIR_POLICY_BOUNDARY23_FAR45:
            return "boundary23_far45";
    }
    return "unknown";
}

bool kdtree_repair_should_run(KdTreeRepairPolicy policy, const Ivf8SearchTraceResult *trace) {
    if (trace == NULL) {
        return false;
    }
    switch (policy) {
        case KDTREE_REPAIR_POLICY_BOUNDARY23_FAR45: {
            uint8_t fraud_count = trace->result.fraud_count;
            uint64_t worst = trace->top[IVF8_SEARCH_TOP_K - 1u].distance;
            return fraud_count == 2u ||
                   fraud_count == 3u ||
                   ((fraud_count == 4u || fraud_count == 5u) &&
                    worst >= KDTREE_REPAIR_BOUNDARY23_FAR45_THRESHOLD);
        }
    }
    return false;
}
