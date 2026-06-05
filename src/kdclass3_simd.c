#include "kdclass3.h"

#include <limits.h>
#include <string.h>

#define KDCLASS3_SIMD_STACK_CAPACITY 4096u

typedef struct {
    uint32_t node;
    uint64_t lower_bound;
} KdClass3SimdStackEntry;

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

static bool scan_leaf_cutoff(const KdClass3TreeIndex *tree,
                             const KdClass3Node *node,
                             const int16_t query[IVF8_INDEX_DIMS],
                             uint64_t cutoff,
                             KdClass3ClassSearchResult *result) {
    result->stats.leaves_visited++;
    uint32_t remaining = node->count;
    uint32_t point_offset = 0;
    uint64_t distances[IVF8_INDEX_LANES];

    for (uint32_t block_offset = 0; remaining > 0; block_offset++) {
        uint32_t block = node->block_start + block_offset;
        ivf8_block_distances_avx2(tree->block_data, block, query, distances);
        uint32_t lanes =
            remaining < IVF8_INDEX_LANES ? remaining : IVF8_INDEX_LANES;
        for (uint32_t lane = 0; lane < lanes; lane++) {
            uint32_t seq = node->point_start + point_offset + lane;
            uint64_t distance = distances[lane];
            result->stats.points_evaluated++;
            if (cutoff != UINT64_MAX && distance > cutoff) {
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

static bool push_node(
    KdClass3SimdStackEntry stack[KDCLASS3_SIMD_STACK_CAPACITY],
    uint32_t *stack_len,
    uint32_t node,
    uint64_t lower_bound,
    KdClass3ClassSearchResult *result) {
    if (node == KDCLASS3_INVALID_NODE) {
        return true;
    }
    if (*stack_len >= KDCLASS3_SIMD_STACK_CAPACITY) {
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

static KdClass3ClassSearchResult search_class3_cutoff_simd_full(
    const KdClass3TreeIndex *tree,
    const int16_t query[IVF8_INDEX_DIMS],
    uint64_t cutoff) {
    KdClass3ClassSearchResult result;
    memset(&result, 0, sizeof(result));
    top3_init(result.top);
    result.distance3 = UINT64_MAX;
    if (tree == NULL || tree->nodes == NULL || tree->block_data == NULL ||
        query == NULL || tree->count < KDCLASS3_TOP_K) {
        return result;
    }

    KdClass3SimdStackEntry stack[KDCLASS3_SIMD_STACK_CAPACITY];
    uint32_t stack_len = 0;
    (void)push_node(stack, &stack_len, tree->root, 0, &result);

    while (stack_len > 0) {
        KdClass3SimdStackEntry entry = stack[--stack_len];
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
            if (scan_leaf_cutoff(tree, node, query, cutoff, &result)) {
                break;
            }
            continue;
        }

        uint32_t left = node->left;
        uint32_t right = node->right;
        uint64_t left_bound =
            left != KDCLASS3_INVALID_NODE
                ? kdclass3_bbox_distance_avx2(&tree->nodes[left], query)
                : UINT64_MAX;
        uint64_t right_bound =
            right != KDCLASS3_INVALID_NODE
                ? kdclass3_bbox_distance_avx2(&tree->nodes[right], query)
                : UINT64_MAX;

        uint32_t near_node = left_bound <= right_bound ? left : right;
        uint32_t far_node = left_bound <= right_bound ? right : left;
        uint64_t near_bound = left_bound <= right_bound ? left_bound : right_bound;
        uint64_t far_bound = left_bound <= right_bound ? right_bound : left_bound;

        uint64_t worst = result.top[KDCLASS3_TOP_K - 1u].distance;
        bool full = top3_full(result.top);
        prune_bound = cutoff;
        if (full && worst < prune_bound) {
            prune_bound = worst;
        }
        if ((!full || far_bound <= worst) && far_bound <= prune_bound) {
            if (!push_node(stack, &stack_len, far_node, far_bound, &result)) {
                result.stats.pruned_branches++;
            }
        } else if (far_node != KDCLASS3_INVALID_NODE) {
            result.stats.pruned_branches++;
        }
        if ((!full || near_bound <= worst) && near_bound <= prune_bound) {
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

KdClass3SearchResult kdclass3_search_simd_full(
    const KdClass3Index *index,
    const int16_t query[IVF8_INDEX_DIMS]) {
    KdClass3SearchResult result;
    memset(&result, 0, sizeof(result));
    result.fallback_required = true;
    result.fraud_count = UINT8_MAX;
    result.fraud_distance3 = UINT64_MAX;
    result.legit_distance3 = UINT64_MAX;
    if (index == NULL || query == NULL) {
        return result;
    }

    KdClass3ClassSearchResult legit =
        search_class3_cutoff_simd_full(&index->legit, query, UINT64_MAX);
    KdClass3ClassSearchResult fraud =
        search_class3_cutoff_simd_full(&index->fraud, query, legit.distance3);
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
