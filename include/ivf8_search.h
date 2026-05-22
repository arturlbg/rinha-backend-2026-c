#ifndef RINHA_IVF8_SEARCH_H
#define RINHA_IVF8_SEARCH_H

#include "ivf8_index.h"

#include <stdbool.h>
#include <stdint.h>

#define IVF8_SEARCH_TOP_K 5u
#define IVF8_SEARCH_MAX_PROBES 64u
#define IVF8_SEARCH_DEFAULT_MAX_CANDIDATES 4096u
#define IVF8_SEARCH_DEFAULT_PROBES 8u

typedef enum {
    IVF8_SEARCH_IMPL_SCALAR = 0,
    IVF8_SEARCH_IMPL_AVX2 = 1
} Ivf8SearchImpl;

typedef struct {
    uint32_t max_candidates;
    uint32_t probes;
    Ivf8SearchImpl impl;
} Ivf8SearchConfig;

typedef struct {
    uint32_t centroids_scored;
    uint32_t clusters_scanned;
    uint32_t candidates_scanned;
    uint32_t blocks_scanned;
    uint32_t largest_scanned_cluster_candidates;
    uint32_t largest_scanned_cluster_blocks;
    uint32_t bbox_pruned;
    uint32_t radius_pruned;
} Ivf8SearchStats;

typedef struct {
    uint64_t total_ns;
    uint64_t centroid_ns;
    uint64_t probe_select_ns;
    uint64_t scan_ns;
    uint64_t top5_ns;
} Ivf8SearchProfile;

typedef struct {
    uint8_t fraud_count;
    Ivf8SearchStats stats;
    Ivf8SearchProfile profile;
} Ivf8SearchResult;

typedef struct {
    uint64_t distance;
    uint8_t fraud;
    uint32_t seq;
} Ivf8Neighbor;

typedef struct {
    uint32_t cluster;
    uint64_t distance;
} Ivf8Probe;

typedef struct {
    Ivf8SearchResult result;
    Ivf8Neighbor top[IVF8_SEARCH_TOP_K];
    Ivf8Probe probes[IVF8_SEARCH_MAX_PROBES];
    uint32_t probe_count;
} Ivf8SearchTraceResult;

uint64_t ivf8_centroid_distance(const Ivf8Index *idx, const int16_t query[IVF8_INDEX_DIMS], uint32_t cluster);
uint64_t ivf8_bbox_distance(const Ivf8Index *idx, const int16_t query[IVF8_INDEX_DIMS], uint32_t cluster);
uint64_t ivf8_block_lane_distance(const int16_t *block_data,
                                  uint32_t block,
                                  uint32_t lane,
                                  const int16_t query[IVF8_INDEX_DIMS]);
bool ivf8_cpu_supports_avx2(void);
Ivf8SearchImpl ivf8_search_impl_from_string(const char *value, bool *ok);
const char *ivf8_search_impl_name(Ivf8SearchImpl impl);
void ivf8_block_distances_avx2(const int16_t *block_data,
                               uint32_t block,
                               const int16_t query[IVF8_INDEX_DIMS],
                               uint64_t out[IVF8_INDEX_LANES]);
void ivf8_centroid_distances_avx2(const Ivf8Index *idx,
                                  const int16_t query[IVF8_INDEX_DIMS],
                                  uint32_t cluster_start,
                                  uint64_t out[IVF8_INDEX_LANES]);
void ivf8_top5_init(Ivf8Neighbor top[IVF8_SEARCH_TOP_K]);
void ivf8_top5_insert(Ivf8Neighbor top[IVF8_SEARCH_TOP_K], Ivf8Neighbor candidate);
uint8_t ivf8_top5_fraud_count(const Ivf8Neighbor top[IVF8_SEARCH_TOP_K]);
uint32_t ivf8_select_probes(const Ivf8Index *idx,
                            const int16_t query[IVF8_INDEX_DIMS],
                            uint32_t probe_count,
                            Ivf8Probe probes[IVF8_SEARCH_MAX_PROBES]);
Ivf8SearchResult ivf8_search(const Ivf8Index *idx,
                              const int16_t query[IVF8_INDEX_DIMS],
                              const Ivf8SearchConfig *cfg);
Ivf8SearchResult ivf8_search_profiled(const Ivf8Index *idx,
                                       const int16_t query[IVF8_INDEX_DIMS],
                                       const Ivf8SearchConfig *cfg);
Ivf8SearchTraceResult ivf8_search_trace(const Ivf8Index *idx,
                                         const int16_t query[IVF8_INDEX_DIMS],
                                         const Ivf8SearchConfig *cfg);
Ivf8SearchTraceResult ivf8_search_trace_profiled(const Ivf8Index *idx,
                                                  const int16_t query[IVF8_INDEX_DIMS],
                                                  const Ivf8SearchConfig *cfg);
uint8_t ivf8_search_fraud_count(const Ivf8Index *idx,
                                const int16_t query[IVF8_INDEX_DIMS],
                                const Ivf8SearchConfig *cfg);

#endif
