#ifndef RINHA_KDTREE_H
#define RINHA_KDTREE_H

#include "ivf8_index.h"
#include "ivf8_search.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define KDTREE_INVALID_NODE UINT32_MAX
#define KDTREE_TOP_K IVF8_SEARCH_TOP_K

typedef enum {
    KDTREE_SOURCE_OWNED = 0,
    KDTREE_SOURCE_IVF8 = 1
} KdTreeSourceKind;

typedef struct {
    uint32_t left;
    uint32_t right;
    uint32_t ref;
    int16_t split_value;
    uint8_t split_dim;
    uint8_t label;
    uint16_t reserved;
} KdTreeNode;

typedef struct {
    KdTreeSourceKind source_kind;
    const Ivf8Index *index;
    int16_t *owned_vectors;
    uint8_t *owned_labels;
    uint32_t count;
    uint32_t root;
    uint32_t node_count;
    KdTreeNode *nodes;
} KdTree;

typedef struct {
    uint32_t max_visited;
} KdTreeSearchConfig;

typedef struct {
    uint32_t nodes_visited;
    uint32_t distance_evaluations;
    uint32_t pruned_branches;
    uint32_t max_depth;
} KdTreeSearchStats;

typedef struct {
    uint8_t fraud_count;
    Ivf8Neighbor top[KDTREE_TOP_K];
    KdTreeSearchStats stats;
} KdTreeSearchResult;

uint64_t kdtree_distance14(const int16_t a[IVF8_INDEX_DIMS], const int16_t b[IVF8_INDEX_DIMS]);
void kdtree_top5_init(Ivf8Neighbor top[KDTREE_TOP_K]);
void kdtree_top5_insert(Ivf8Neighbor top[KDTREE_TOP_K], Ivf8Neighbor candidate);
uint8_t kdtree_top5_fraud_count(const Ivf8Neighbor top[KDTREE_TOP_K]);

int kdtree_build_from_points(KdTree *tree,
                             const int16_t *vectors,
                             const uint8_t *labels,
                             uint32_t count);
int kdtree_build_from_ivf8(KdTree *tree, const Ivf8Index *index);
uint32_t kdtree_count_ivf8_records(const Ivf8Index *index);
void kdtree_free(KdTree *tree);

const int16_t *kdtree_vector(const KdTree *tree, uint32_t ref);
uint8_t kdtree_label(const KdTree *tree, uint32_t ref);
size_t kdtree_runtime_memory_bytes(const KdTree *tree);
size_t kdtree_build_memory_bytes(const KdTree *tree);

KdTreeSearchResult kdtree_search_top5(const KdTree *tree,
                                      const int16_t query[IVF8_INDEX_DIMS],
                                      const KdTreeSearchConfig *cfg);
uint8_t kdtree_search_fraud_count(const KdTree *tree,
                                  const int16_t query[IVF8_INDEX_DIMS],
                                  const KdTreeSearchConfig *cfg);

int kdtree_save_nodes(const KdTree *tree, const char *path);
int kdtree_load_nodes_for_ivf8(KdTree *tree, const Ivf8Index *index, const char *path);

#endif
