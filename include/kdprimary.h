#ifndef RINHA_KDPRIMARY_H
#define RINHA_KDPRIMARY_H

#include "ivf8_index.h"
#include "ivf8_search.h"

#include <stddef.h>
#include <stdint.h>

#define KDPRIMARY_MAGIC "RKDP1IDX"
#define KDPRIMARY_MAGIC_BYTES 8u
#define KDPRIMARY_VERSION 1u
#define KDPRIMARY_HEADER_BYTES 128u
#define KDPRIMARY_INVALID_NODE UINT32_MAX
#define KDPRIMARY_LEAF_FLAG 1u
#define KDPRIMARY_POINT_FLAG 2u
#define KDPRIMARY_TOP_K IVF8_SEARCH_TOP_K

typedef struct {
    uint32_t left;
    uint32_t right;
    uint32_t start;
    uint32_t count;
    int16_t split_value;
    uint8_t split_dim;
    uint8_t flags;
} KdPrimaryNode;

typedef struct {
    uint32_t count;
    uint32_t node_count;
    uint32_t root;
    uint32_t leaf_size;
    KdPrimaryNode *nodes;
    int16_t *vectors;
    uint8_t *labels;
} KdPrimaryBuild;

typedef struct {
    int fd;
    size_t file_size;
    void *map;

    uint32_t count;
    uint32_t node_count;
    uint32_t root;
    uint32_t leaf_size;

    const KdPrimaryNode *nodes;
    const int16_t *vectors;
    const uint8_t *labels;
} KdPrimaryIndex;

typedef struct {
    uint32_t nodes_visited;
    uint32_t leaves_visited;
    uint32_t points_evaluated;
    uint32_t pruned_branches;
    uint32_t max_depth;
} KdPrimarySearchStats;

typedef struct {
    uint8_t fraud_count;
    Ivf8Neighbor top[KDPRIMARY_TOP_K];
    KdPrimarySearchStats stats;
} KdPrimarySearchResult;

uint64_t kdprimary_distance14(const int16_t a[IVF8_INDEX_DIMS], const int16_t b[IVF8_INDEX_DIMS]);

int kdprimary_build_from_points(KdPrimaryBuild *build,
                                const int16_t *vectors,
                                const uint8_t *labels,
                                uint32_t count,
                                uint32_t leaf_size,
                                char *err,
                                size_t err_len);
int kdprimary_build_from_ivf8(KdPrimaryBuild *build,
                              const Ivf8Index *index,
                              uint32_t leaf_size,
                              char *err,
                              size_t err_len);
void kdprimary_build_free(KdPrimaryBuild *build);

int kdprimary_save(const KdPrimaryBuild *build, const char *path, char *err, size_t err_len);
int kdprimary_open(const char *path, KdPrimaryIndex *out, char *err, size_t err_len);
void kdprimary_close(KdPrimaryIndex *index);

size_t kdprimary_build_memory_bytes(const KdPrimaryBuild *build);
size_t kdprimary_runtime_memory_bytes(const KdPrimaryIndex *index);
size_t kdprimary_expected_file_bytes(uint32_t count, uint32_t node_count);
uint64_t kdprimary_touch_pages(const KdPrimaryIndex *index);

KdPrimarySearchResult kdprimary_search_top5(const KdPrimaryIndex *index,
                                            const int16_t query[IVF8_INDEX_DIMS]);
uint8_t kdprimary_search_fraud_count(const KdPrimaryIndex *index,
                                     const int16_t query[IVF8_INDEX_DIMS]);

#endif
