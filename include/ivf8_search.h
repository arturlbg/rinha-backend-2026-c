#ifndef RINHA_IVF8_SEARCH_H
#define RINHA_IVF8_SEARCH_H

#include "ivf8_index.h"

#include <stdint.h>

#define IVF8_SEARCH_TOP_K 5u
#define IVF8_SEARCH_MAX_PROBES 64u
#define IVF8_SEARCH_DEFAULT_MAX_CANDIDATES 4096u
#define IVF8_SEARCH_DEFAULT_PROBES 8u

typedef struct {
    uint32_t max_candidates;
    uint32_t probes;
} Ivf8SearchConfig;

typedef struct {
    uint32_t centroids_scored;
    uint32_t clusters_scanned;
    uint32_t candidates_scanned;
    uint32_t bbox_pruned;
    uint32_t radius_pruned;
} Ivf8SearchStats;

typedef struct {
    uint8_t fraud_count;
    Ivf8SearchStats stats;
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

uint64_t ivf8_centroid_distance(const Ivf8Index *idx, const int16_t query[IVF8_INDEX_DIMS], uint32_t cluster);
uint64_t ivf8_bbox_distance(const Ivf8Index *idx, const int16_t query[IVF8_INDEX_DIMS], uint32_t cluster);
uint64_t ivf8_block_lane_distance(const int16_t *block_data,
                                  uint32_t block,
                                  uint32_t lane,
                                  const int16_t query[IVF8_INDEX_DIMS]);
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
uint8_t ivf8_search_fraud_count(const Ivf8Index *idx,
                                const int16_t query[IVF8_INDEX_DIMS],
                                const Ivf8SearchConfig *cfg);

#endif

