#include "kdclass3_opt.h"

#include <limits.h>
#include <string.h>

#define KDCLASS3_OPT_STACK_CAPACITY 4096u

typedef struct {
    uint32_t node;
    uint64_t lower_bound;
} KdClass3OptStackEntry;

static void top3_init(KdClass3Neighbor top[KDCLASS3_TOP_K]) {
    for (uint32_t i = 0; i < KDCLASS3_TOP_K; i++) {
        top[i].distance = UINT64_MAX;
        top[i].seq = UINT32_MAX;
    }
}

static bool top3_accepts(const KdClass3Neighbor top[KDCLASS3_TOP_K],
                         uint64_t distance,
                         uint32_t seq) {
    return distance < top[KDCLASS3_TOP_K - 1u].distance ||
           (distance == top[KDCLASS3_TOP_K - 1u].distance &&
            seq < top[KDCLASS3_TOP_K - 1u].seq);
}

static void top3_insert(KdClass3Neighbor top[KDCLASS3_TOP_K],
                        KdClass3Neighbor candidate) {
    if (!top3_accepts(top, candidate.distance, candidate.seq)) {
        return;
    }
    top[KDCLASS3_TOP_K - 1u] = candidate;
    for (uint32_t i = KDCLASS3_TOP_K - 1u; i > 0; i--) {
        bool better = top[i].distance < top[i - 1u].distance ||
                      (top[i].distance == top[i - 1u].distance &&
                       top[i].seq < top[i - 1u].seq);
        if (!better) {
            break;
        }
        KdClass3Neighbor tmp = top[i - 1u];
        top[i - 1u] = top[i];
        top[i] = tmp;
    }
}

static bool top3_full(const KdClass3Neighbor top[KDCLASS3_TOP_K]) {
    return top[KDCLASS3_TOP_K - 1u].seq != UINT32_MAX;
}

static uint32_t popcount8(uint32_t value) {
    value &= 0xffu;
    value = value - ((value >> 1u) & 0x55u);
    value = (value & 0x33u) + ((value >> 2u) & 0x33u);
    return (value + (value >> 4u)) & 0x0fu;
}

static uint64_t bbox_distance_limit(
    const KdClass3Node *node,
    const int16_t query[IVF8_INDEX_DIMS],
    uint64_t limit,
    KdClass3OptStats *stats) {
    uint64_t sum = 0;
    if (node == NULL || query == NULL) {
        return UINT64_MAX;
    }
    for (uint32_t dim = 0; dim < IVF8_INDEX_DIMS; dim++) {
        int64_t diff = 0;
        if (query[dim] < node->bbox_min[dim]) {
            diff = (int64_t)node->bbox_min[dim] - (int64_t)query[dim];
        } else if (query[dim] > node->bbox_max[dim]) {
            diff = (int64_t)query[dim] - (int64_t)node->bbox_max[dim];
        }
        sum += (uint64_t)(diff * diff);
        stats->bbox_dimensions_evaluated++;
        if (sum > limit) {
            return sum;
        }
    }
    return sum;
}

static uint64_t bbox_distance_full(
    const KdClass3Node *node,
    const int16_t query[IVF8_INDEX_DIMS],
    KdClass3OptStats *stats) {
    uint64_t sum = 0;
    if (node == NULL || query == NULL) {
        return UINT64_MAX;
    }
    for (uint32_t dim = 0; dim < IVF8_INDEX_DIMS; dim++) {
        int64_t diff = 0;
        if (query[dim] < node->bbox_min[dim]) {
            diff = (int64_t)node->bbox_min[dim] - (int64_t)query[dim];
        } else if (query[dim] > node->bbox_max[dim]) {
            diff = (int64_t)query[dim] - (int64_t)node->bbox_max[dim];
        }
        sum += (uint64_t)(diff * diff);
        stats->bbox_dimensions_evaluated++;
    }
    return sum;
}

static bool scan_leaf_cutoff(
    const KdClass3TreeIndex *tree,
    const KdClass3Node *node,
    const int16_t query[IVF8_INDEX_DIMS],
    uint64_t cutoff,
    KdClass3OptMode mode,
    KdClass3OptClassSearchResult *result) {
    result->stats.leaves_visited++;
    uint32_t remaining = node->count;
    uint32_t point_offset = 0;
    uint64_t distances[IVF8_INDEX_LANES];

    for (uint32_t block_offset = 0; remaining > 0; block_offset++) {
        uint32_t lanes =
            remaining < IVF8_INDEX_LANES ? remaining : IVF8_INDEX_LANES;
        uint32_t lane_mask = (1u << lanes) - 1u;
        uint64_t limit = cutoff;
        if (top3_full(result->top) &&
            result->top[KDCLASS3_TOP_K - 1u].distance < limit) {
            limit = result->top[KDCLASS3_TOP_K - 1u].distance;
        }

        result->stats.blocks_evaluated++;
        uint32_t survivors = lane_mask;
        bool checkpoint_leaf =
            mode == KDCLASS3_OPT_CHECKPOINT_ONLY ||
            mode == KDCLASS3_OPT_COMBINED ||
            mode == KDCLASS3_OPT_SIMD_COMBINED;
        if (!checkpoint_leaf) {
            ivf8_block_distances_avx2(
                tree->block_data, node->block_start + block_offset,
                query, distances);
        } else {
            uint32_t dimensions = 0;
            survivors = kdclass3_opt_block_distances_avx2(
                tree->block_data,
                node->block_start + block_offset,
                query,
                limit,
                distances,
                &dimensions) & lane_mask;
            if (survivors == 0u && dimensions < IVF8_INDEX_DIMS) {
                result->stats.blocks_checkpoint_pruned++;
            }
        }
        result->stats.lanes_limit_pruned += lanes - popcount8(survivors);
        result->stats.points_evaluated +=
            checkpoint_leaf ? popcount8(survivors) : lanes;

        for (uint32_t lane = 0; lane < lanes; lane++) {
            if ((survivors & (1u << lane)) == 0u) {
                continue;
            }
            uint32_t seq = node->point_start + point_offset + lane;
            uint64_t distance = distances[lane];
            if (limit != UINT64_MAX && distance > limit) {
                continue;
            }
            if (top3_accepts(result->top, distance, seq)) {
                KdClass3Neighbor candidate = {
                    .distance = distance,
                    .seq = seq,
                };
                top3_insert(result->top, candidate);
                if (cutoff != UINT64_MAX &&
                    result->top[KDCLASS3_TOP_K - 1u].distance < cutoff) {
                    return true;
                }
            }
        }
        remaining -= lanes;
        point_offset += lanes;
    }
    return false;
}

static bool push_node(KdClass3OptStackEntry stack[KDCLASS3_OPT_STACK_CAPACITY],
                      uint32_t *stack_len,
                      uint32_t node,
                      uint64_t lower_bound,
                      KdClass3OptClassSearchResult *result) {
    if (node == KDCLASS3_INVALID_NODE) {
        return true;
    }
    if (*stack_len >= KDCLASS3_OPT_STACK_CAPACITY) {
        return false;
    }
    stack[*stack_len].node = node;
    stack[*stack_len].lower_bound = lower_bound;
    (*stack_len)++;
    if (*stack_len > result->stats.max_stack) {
        result->stats.max_stack = *stack_len;
    }
    return true;
}

static KdClass3OptClassSearchResult search_class3_cutoff(
    const KdClass3TreeIndex *tree,
    const int16_t query[IVF8_INDEX_DIMS],
    uint64_t cutoff,
    KdClass3OptMode mode) {
    KdClass3OptClassSearchResult result;
    memset(&result, 0, sizeof(result));
    top3_init(result.top);
    result.distance3 = UINT64_MAX;
    if (tree == NULL || tree->nodes == NULL || tree->block_data == NULL ||
        query == NULL || tree->count < KDCLASS3_TOP_K) {
        return result;
    }

    KdClass3OptStackEntry stack[KDCLASS3_OPT_STACK_CAPACITY];
    uint32_t stack_len = 0;
    (void)push_node(stack, &stack_len, tree->root, 0, &result);

    while (stack_len > 0) {
        KdClass3OptStackEntry entry = stack[--stack_len];
        if (entry.node >= tree->node_count) {
            continue;
        }
        if (cutoff != UINT64_MAX && top3_full(result.top) &&
            result.top[KDCLASS3_TOP_K - 1u].distance < cutoff) {
            break;
        }
        uint64_t prune_bound = cutoff;
        if (top3_full(result.top) &&
            result.top[KDCLASS3_TOP_K - 1u].distance < prune_bound) {
            prune_bound = result.top[KDCLASS3_TOP_K - 1u].distance;
        }
        if (entry.lower_bound > prune_bound) {
            result.stats.pruned_branches++;
            continue;
        }

        const KdClass3Node *node = &tree->nodes[entry.node];
        result.stats.nodes_visited++;
        if ((node->flags & KDCLASS3_LEAF_FLAG) != 0) {
            if (scan_leaf_cutoff(tree, node, query, cutoff, mode, &result)) {
                break;
            }
            continue;
        }

        uint32_t left = node->left;
        uint32_t right = node->right;
        bool bounded_bbox =
            mode == KDCLASS3_OPT_BBOX_ONLY || mode == KDCLASS3_OPT_COMBINED;
        bool simd_bbox =
            mode == KDCLASS3_OPT_SIMD_BBOX_ONLY ||
            mode == KDCLASS3_OPT_SIMD_COMBINED ||
            mode == KDCLASS3_OPT_SIMD_BBOX_FULL;
        uint64_t simd_limit =
            mode == KDCLASS3_OPT_SIMD_BBOX_FULL ? UINT64_MAX : prune_bound;
        uint64_t left_bound = UINT64_MAX;
        uint64_t right_bound = UINT64_MAX;
        if (left != KDCLASS3_INVALID_NODE) {
            if (simd_bbox) {
                uint32_t dimensions = 0;
                left_bound = kdclass3_opt_bbox_distance_avx2(
                    &tree->nodes[left], query, simd_limit, &dimensions);
                result.stats.bbox_dimensions_evaluated += dimensions;
            } else {
                left_bound = bounded_bbox
                    ? bbox_distance_limit(&tree->nodes[left], query, prune_bound,
                                          &result.stats)
                    : bbox_distance_full(&tree->nodes[left], query, &result.stats);
            }
        }
        if (right != KDCLASS3_INVALID_NODE) {
            if (simd_bbox) {
                uint32_t dimensions = 0;
                right_bound = kdclass3_opt_bbox_distance_avx2(
                    &tree->nodes[right], query, simd_limit, &dimensions);
                result.stats.bbox_dimensions_evaluated += dimensions;
            } else {
                right_bound = bounded_bbox
                    ? bbox_distance_limit(&tree->nodes[right], query, prune_bound,
                                          &result.stats)
                    : bbox_distance_full(&tree->nodes[right], query, &result.stats);
            }
        }

        uint32_t near_node = left_bound <= right_bound ? left : right;
        uint32_t far_node = left_bound <= right_bound ? right : left;
        uint64_t near_bound =
            left_bound <= right_bound ? left_bound : right_bound;
        uint64_t far_bound =
            left_bound <= right_bound ? right_bound : left_bound;

        if (far_bound <= prune_bound) {
            if (!push_node(stack, &stack_len, far_node, far_bound, &result)) {
                result.stats.pruned_branches++;
            }
        } else if (far_node != KDCLASS3_INVALID_NODE) {
            result.stats.pruned_branches++;
        }
        if (near_bound <= prune_bound) {
            if (!push_node(stack, &stack_len, near_node, near_bound, &result)) {
                result.stats.pruned_branches++;
            }
        } else if (near_node != KDCLASS3_INVALID_NODE) {
            result.stats.pruned_branches++;
        }
    }

    result.distance3 = result.top[KDCLASS3_TOP_K - 1u].distance;
    return result;
}

KdClass3OptClassSearchResult kdclass3_opt_search_class3(
    const KdClass3TreeIndex *tree,
    const int16_t query[IVF8_INDEX_DIMS]) {
    return search_class3_cutoff(tree, query, UINT64_MAX,
                                KDCLASS3_OPT_SIMD_BBOX_FULL);
}

KdClass3OptSearchResult kdclass3_opt_search(
    const KdClass3Index *index,
    const int16_t query[IVF8_INDEX_DIMS]) {
    return kdclass3_opt_search_mode(index, query, KDCLASS3_OPT_SIMD_BBOX_FULL);
}

KdClass3OptSearchResult kdclass3_opt_search_mode(
    const KdClass3Index *index,
    const int16_t query[IVF8_INDEX_DIMS],
    KdClass3OptMode mode) {
    KdClass3OptSearchResult result;
    memset(&result, 0, sizeof(result));
    result.fallback_required = true;
    result.fraud_count = UINT8_MAX;
    result.fraud_distance3 = UINT64_MAX;
    result.legit_distance3 = UINT64_MAX;
    if (index == NULL || query == NULL) {
        return result;
    }

    KdClass3OptClassSearchResult legit =
        search_class3_cutoff(&index->legit, query, UINT64_MAX, mode);
    KdClass3OptClassSearchResult fraud =
        search_class3_cutoff(&index->fraud, query, legit.distance3, mode);
    result.fraud_distance3 = fraud.distance3;
    result.legit_distance3 = legit.distance3;
    result.fraud_stats = fraud.stats;
    result.legit_stats = legit.stats;
    if (fraud.distance3 < legit.distance3) {
        result.fraud_count = 3u;
        result.fallback_required = false;
    } else if (legit.distance3 < fraud.distance3) {
        result.fraud_count = 0u;
        result.fallback_required = false;
    }
    return result;
}
