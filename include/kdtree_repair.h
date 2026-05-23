#ifndef RINHA_KDTREE_REPAIR_H
#define RINHA_KDTREE_REPAIR_H

#include "ivf8_search.h"

#include <stdbool.h>
#include <stdint.h>

#define KDTREE_REPAIR_BOUNDARY23_FAR45_THRESHOLD 4500000ull
#define KDTREE_REPAIR_PERFECT_V1_BOUNDARY_WORST_THRESHOLD 3361216ull
#define KDTREE_REPAIR_PERFECT_V1_FAR_WORST_THRESHOLD 3944022ull
#define KDTREE_REPAIR_MINIMAL_V1_WORST_THRESHOLD 4404524ull

typedef enum {
    KDTREE_REPAIR_POLICY_BOUNDARY23_FAR45 = 0,
    KDTREE_REPAIR_POLICY_PERFECT_V1 = 1,
    KDTREE_REPAIR_POLICY_MINIMAL_V1 = 2
} KdTreeRepairPolicy;

bool kdtree_repair_policy_from_string(const char *value, KdTreeRepairPolicy *out);
const char *kdtree_repair_policy_name(KdTreeRepairPolicy policy);
bool kdtree_repair_should_run(KdTreeRepairPolicy policy, const Ivf8SearchTraceResult *trace);

#endif
