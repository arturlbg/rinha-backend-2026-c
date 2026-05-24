#ifndef RINHA_KDPRIMARY2_H
#define RINHA_KDPRIMARY2_H

#include "ivf8_index.h"
#include "ivf8_search.h"

#include <stddef.h>
#include <stdint.h>

#define KDPRIMARY2_MAGIC "RKDP2IDX"
#define KDPRIMARY2_MAGIC_BYTES 8u
#define KDPRIMARY2_VERSION 1u
#define KDPRIMARY2_HEADER_BYTES 128u
#define KDPRIMARY2_INVALID_NODE UINT32_MAX
#define KDPRIMARY2_LEAF_FLAG 1u
#define KDPRIMARY2_TOP_K IVF8_SEARCH_TOP_K

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
} KdPrimary2Node;

typedef struct {
    uint32_t count;
    uint32_t node_count;
    uint32_t block_count;
    uint32_t root;
    uint32_t leaf_size;
    KdPrimary2Node *nodes;
    int16_t *block_data;
    uint8_t *labels;
} KdPrimary2Build;

typedef struct {
    int fd;
    size_t file_size;
    void *map;

    uint32_t count;
    uint32_t node_count;
    uint32_t block_count;
    uint32_t root;
    uint32_t leaf_size;

    const KdPrimary2Node *nodes;
    const int16_t *block_data;
    const uint8_t *labels;
} KdPrimary2Index;

typedef struct {
    uint32_t nodes_visited;
    uint32_t leaves_visited;
    uint32_t points_evaluated;
    uint32_t pruned_branches;
    uint32_t max_stack;
} KdPrimary2SearchStats;

typedef struct {
    uint8_t fraud_count;
    Ivf8Neighbor top[KDPRIMARY2_TOP_K];
    KdPrimary2SearchStats stats;
} KdPrimary2SearchResult;

uint64_t kdprimary2_distance14(const int16_t a[IVF8_INDEX_DIMS], const int16_t b[IVF8_INDEX_DIMS]);
uint64_t kdprimary2_bbox_distance(const KdPrimary2Node *node, const int16_t query[IVF8_INDEX_DIMS]);
void kdprimary2_leaf_block_distances_avx2(const int16_t *block_data,
                                          uint32_t block,
                                          const int16_t query[IVF8_INDEX_DIMS],
                                          uint64_t out[IVF8_INDEX_LANES]);

int kdprimary2_build_from_points(KdPrimary2Build *build,
                                 const int16_t *vectors,
                                 const uint8_t *labels,
                                 uint32_t count,
                                 uint32_t leaf_size,
                                 char *err,
                                 size_t err_len);
int kdprimary2_build_from_ivf8(KdPrimary2Build *build,
                               const Ivf8Index *index,
                               uint32_t leaf_size,
                               char *err,
                               size_t err_len);
void kdprimary2_build_free(KdPrimary2Build *build);

int kdprimary2_save(const KdPrimary2Build *build, const char *path, char *err, size_t err_len);
int kdprimary2_open(const char *path, KdPrimary2Index *out, char *err, size_t err_len);
void kdprimary2_close(KdPrimary2Index *index);

size_t kdprimary2_build_memory_bytes(const KdPrimary2Build *build);
size_t kdprimary2_runtime_memory_bytes(const KdPrimary2Index *index);
size_t kdprimary2_expected_file_bytes(uint32_t node_count, uint32_t block_count);
uint64_t kdprimary2_touch_pages(const KdPrimary2Index *index);

KdPrimary2SearchResult kdprimary2_search_top5(const KdPrimary2Index *index,
                                              const int16_t query[IVF8_INDEX_DIMS]);
uint8_t kdprimary2_search_fraud_count(const KdPrimary2Index *index,
                                      const int16_t query[IVF8_INDEX_DIMS]);

#endif
