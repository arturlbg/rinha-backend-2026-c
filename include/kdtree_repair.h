#ifndef RINHA_KDTREE_REPAIR_H
#define RINHA_KDTREE_REPAIR_H

#include "ivf8_search.h"

#include <stdbool.h>
#include <stdint.h>

#define KDTREE_REPAIR_BOUNDARY23_FAR45_THRESHOLD 4500000ull

typedef enum {
    KDTREE_REPAIR_POLICY_BOUNDARY23_FAR45 = 0
} KdTreeRepairPolicy;

bool kdtree_repair_policy_from_string(const char *value, KdTreeRepairPolicy *out);
const char *kdtree_repair_policy_name(KdTreeRepairPolicy policy);
bool kdtree_repair_should_run(KdTreeRepairPolicy policy, const Ivf8SearchTraceResult *trace);

#endif
