#ifndef RINHA_KDCLASS3_OPT_H
#define RINHA_KDCLASS3_OPT_H

#include "kdclass3.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t nodes_visited;
    uint32_t leaves_visited;
    uint32_t points_evaluated;
    uint32_t pruned_branches;
    uint32_t max_stack;
    uint32_t bbox_dimensions_evaluated;
    uint32_t blocks_evaluated;
    uint32_t blocks_checkpoint_pruned;
    uint32_t lanes_limit_pruned;
} KdClass3OptStats;

typedef struct {
    uint64_t distance3;
    KdClass3Neighbor top[KDCLASS3_TOP_K];
    KdClass3OptStats stats;
} KdClass3OptClassSearchResult;

typedef struct {
    uint8_t fraud_count;
    bool fallback_required;
    uint64_t fraud_distance3;
    uint64_t legit_distance3;
    KdClass3OptStats fraud_stats;
    KdClass3OptStats legit_stats;
} KdClass3OptSearchResult;

typedef enum {
    KDCLASS3_OPT_BBOX_ONLY = 0,
    KDCLASS3_OPT_CHECKPOINT_ONLY = 1,
    KDCLASS3_OPT_COMBINED = 2,
    KDCLASS3_OPT_SIMD_BBOX_ONLY = 3,
    KDCLASS3_OPT_SIMD_COMBINED = 4,
    KDCLASS3_OPT_SIMD_BBOX_FULL = 5
} KdClass3OptMode;

/*
 * Offline Phase 20A experiment. The shipping kdclass3 search remains in
 * kdclass3.c; these functions are linked only into the opt test/evaluator.
 */
KdClass3OptClassSearchResult kdclass3_opt_search_class3(
    const KdClass3TreeIndex *tree,
    const int16_t query[IVF8_INDEX_DIMS]);
KdClass3OptSearchResult kdclass3_opt_search(
    const KdClass3Index *index,
    const int16_t query[IVF8_INDEX_DIMS]);
KdClass3OptSearchResult kdclass3_opt_search_mode(
    const KdClass3Index *index,
    const int16_t query[IVF8_INDEX_DIMS],
    KdClass3OptMode mode);

/*
 * Returns a mask of lanes whose exact distance is <= limit. Partial sums are
 * monotonic, so a block can stop early only when all lanes exceed the limit.
 * Equality is intentionally retained for deterministic tie/fallback behavior.
 */
uint32_t kdclass3_opt_block_distances_avx2(
    const int16_t *block_data,
    uint32_t block,
    const int16_t query[IVF8_INDEX_DIMS],
    uint64_t limit,
    uint64_t out[IVF8_INDEX_LANES],
    uint32_t *dimensions_evaluated);
uint64_t kdclass3_opt_bbox_distance_avx2(
    const KdClass3Node *node,
    const int16_t query[IVF8_INDEX_DIMS],
    uint64_t limit,
    uint32_t *dimensions_evaluated);

#endif
