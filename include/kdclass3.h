#ifndef RINHA_KDCLASS3_H
#define RINHA_KDCLASS3_H

#include "ivf8_index.h"
#include "ivf8_search.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define KDCLASS3_MAGIC "RKDC3IDX"
#define KDCLASS3_MAGIC_BYTES 8u
#define KDCLASS3_VERSION 1u
#define KDCLASS3_HEADER_BYTES 128u
#define KDCLASS3_INVALID_NODE UINT32_MAX
#define KDCLASS3_LEAF_FLAG 1u
#define KDCLASS3_TOP_K 3u

typedef struct {
    uint32_t left;
    uint32_t right;
    uint32_t block_start;
    uint32_t point_start;
    uint32_t count;
    int16_t bbox_min[IVF8_INDEX_DIMS];
    int16_t bbox_max[IVF8_INDEX_DIMS];
    int16_t split_value;
    uint8_t split_dim;
    uint8_t flags;
    uint16_t reserved;
} KdClass3Node;

typedef struct {
    uint32_t count;
    uint32_t node_count;
    uint32_t block_count;
    uint32_t root;
    KdClass3Node *nodes;
    int16_t *block_data;
} KdClass3TreeBuild;

typedef struct {
    uint32_t leaf_size;
    KdClass3TreeBuild fraud;
    KdClass3TreeBuild legit;
} KdClass3Build;

typedef struct {
    uint32_t count;
    uint32_t node_count;
    uint32_t block_count;
    uint32_t root;
    const KdClass3Node *nodes;
    const int16_t *block_data;
} KdClass3TreeIndex;

typedef struct {
    int fd;
    size_t file_size;
    void *map;
    uint32_t leaf_size;
    KdClass3TreeIndex fraud;
    KdClass3TreeIndex legit;
} KdClass3Index;

typedef struct {
    uint32_t nodes_visited;
    uint32_t leaves_visited;
    uint32_t points_evaluated;
    uint32_t pruned_branches;
    uint32_t max_stack;
} KdClass3SearchStats;

typedef struct {
    uint64_t distance;
    uint32_t seq;
} KdClass3Neighbor;

typedef struct {
    uint64_t distance3;
    KdClass3Neighbor top[KDCLASS3_TOP_K];
    KdClass3SearchStats stats;
} KdClass3ClassSearchResult;

typedef struct {
    uint8_t fraud_count;
    bool fallback_required;
    uint64_t fraud_distance3;
    uint64_t legit_distance3;
    KdClass3SearchStats fraud_stats;
    KdClass3SearchStats legit_stats;
} KdClass3SearchResult;

uint64_t kdclass3_distance14(const int16_t a[IVF8_INDEX_DIMS], const int16_t b[IVF8_INDEX_DIMS]);
uint64_t kdclass3_bbox_distance(const KdClass3Node *node, const int16_t query[IVF8_INDEX_DIMS]);

int kdclass3_build_from_points(KdClass3Build *build,
                               const int16_t *vectors,
                               const uint8_t *labels,
                               uint32_t count,
                               uint32_t leaf_size,
                               char *err,
                               size_t err_len);
int kdclass3_build_from_ivf8(KdClass3Build *build,
                             const Ivf8Index *index,
                             uint32_t leaf_size,
                             char *err,
                             size_t err_len);
void kdclass3_build_free(KdClass3Build *build);

int kdclass3_save(const KdClass3Build *build, const char *path, char *err, size_t err_len);
int kdclass3_open(const char *path, KdClass3Index *out, char *err, size_t err_len);
void kdclass3_close(KdClass3Index *index);

size_t kdclass3_tree_block_data_bytes(uint32_t block_count);
size_t kdclass3_expected_file_bytes(uint32_t fraud_node_count,
                                    uint32_t fraud_block_count,
                                    uint32_t legit_node_count,
                                    uint32_t legit_block_count);
size_t kdclass3_build_memory_bytes(const KdClass3Build *build);
size_t kdclass3_runtime_memory_bytes(const KdClass3Index *index);
uint64_t kdclass3_touch_pages(const KdClass3Index *index);

KdClass3ClassSearchResult kdclass3_search_class3(const KdClass3TreeIndex *tree,
                                                 const int16_t query[IVF8_INDEX_DIMS]);
KdClass3SearchResult kdclass3_search(const KdClass3Index *index,
                                     const int16_t query[IVF8_INDEX_DIMS]);

/*
 * Binary-label k=5 majority rule:
 *
 * Let F3 be the squared distance to the 3rd nearest fraudulent reference and
 * L3 be the squared distance to the 3rd nearest legitimate reference. If
 * F3 < L3, then at least three fraud references appear before the 3rd legit,
 * so the exact top-5 majority is fraud. Symmetrically, L3 < F3 implies legit.
 * When F3 == L3, cross-class tie-breaking can affect which labels occupy the
 * exact top-5, so kdclass3_search reports fallback_required=true and callers
 * must use the current KD-primary2 exact top-5 result.
 */

#endif
