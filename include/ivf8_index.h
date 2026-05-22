#ifndef RINHA_IVF8_INDEX_H
#define RINHA_IVF8_INDEX_H

#include <stddef.h>
#include <stdint.h>

#define IVF8_INDEX_MAGIC "RIVF8IDX"
#define IVF8_INDEX_MAGIC_BYTES 8
#define IVF8_INDEX_VERSION 1u
#define IVF8_INDEX_HEADER_BYTES 64u
#define IVF8_INDEX_DIMS 14u
#define IVF8_INDEX_LANES 8u

#define IVF8_PRODUCTION_N 3000000u
#define IVF8_PRODUCTION_K 4096u
#define IVF8_PRODUCTION_BLOCKS 376780u
#define IVF8_PRODUCTION_FILE_BYTES 87822628u

typedef struct {
    uint32_t version;
    uint32_t n;
    uint32_t k;
    uint32_t dims;
    uint32_t lanes;
    uint32_t blocks;
} Ivf8Header;

typedef struct {
    size_t centroids_offset;
    size_t offsets_offset;
    size_t counts_offset;
    size_t bbox_min_offset;
    size_t bbox_max_offset;
    size_t radii_offset;
    size_t labels_offset;
    size_t block_data_offset;
    size_t total_size;
} Ivf8Layout;

typedef struct {
    int fd;
    size_t file_size;
    void *map;

    uint32_t version;
    uint32_t n;
    uint32_t k;
    uint32_t dims;
    uint32_t lanes;
    uint32_t blocks;

    Ivf8Layout layout;

    const int16_t *centroids;
    const uint32_t *offsets;
    const uint32_t *counts;
    const int16_t *bbox_min;
    const int16_t *bbox_max;
    const uint64_t *radii;
    const uint8_t *labels;
    const int16_t *block_data;
} Ivf8Index;

int ivf8_index_parse_header(const void *data, size_t len, Ivf8Header *out, char *err, size_t err_len);
int ivf8_index_compute_layout(const Ivf8Header *header, Ivf8Layout *out, char *err, size_t err_len);
int ivf8_index_open(const char *path, Ivf8Index *out, char *err, size_t err_len);
int ivf8_index_validate(const Ivf8Index *index, char *err, size_t err_len);
void ivf8_index_close(Ivf8Index *index);
uint32_t ivf8_index_apply_memory_advice(const Ivf8Index *index);
uint64_t ivf8_index_touch_pages(const Ivf8Index *index);

#endif
