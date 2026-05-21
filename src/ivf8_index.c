#define _POSIX_C_SOURCE 200809L

#include "ivf8_index.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static void set_error(char *err, size_t err_len, const char *message) {
    if (err != NULL && err_len > 0) {
        (void)snprintf(err, err_len, "%s", message);
    }
}

static void set_errno_error(char *err, size_t err_len, const char *prefix) {
    if (err != NULL && err_len > 0) {
        (void)snprintf(err, err_len, "%s: %s", prefix, strerror(errno));
    }
}

static uint32_t read_u32_le(const uint8_t *p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static bool add_size(size_t *value, size_t add) {
    if (*value > SIZE_MAX - add) {
        return false;
    }
    *value += add;
    return true;
}

static bool mul_size(size_t a, size_t b, size_t *out) {
    if (a != 0 && b > SIZE_MAX / a) {
        return false;
    }
    *out = a * b;
    return true;
}

static bool section_bytes(size_t count, size_t elem_size, size_t *out) {
    return mul_size(count, elem_size, out);
}

int ivf8_index_parse_header(const void *data, size_t len, Ivf8Header *out, char *err, size_t err_len) {
    if (data == NULL || out == NULL) {
        set_error(err, err_len, "ivf8: nil header input");
        return -1;
    }
    if (len < IVF8_INDEX_HEADER_BYTES) {
        set_error(err, err_len, "ivf8: header too small");
        return -1;
    }

    const uint8_t *raw = (const uint8_t *)data;
    if (memcmp(raw, IVF8_INDEX_MAGIC, IVF8_INDEX_MAGIC_BYTES) != 0) {
        set_error(err, err_len, "ivf8: invalid magic");
        return -1;
    }

    Ivf8Header header;
    header.version = read_u32_le(raw + 8);
    header.n = read_u32_le(raw + 12);
    header.k = read_u32_le(raw + 16);
    header.dims = read_u32_le(raw + 20);
    header.lanes = read_u32_le(raw + 24);
    header.blocks = read_u32_le(raw + 28);

    if (header.version != IVF8_INDEX_VERSION) {
        set_error(err, err_len, "ivf8: unsupported version");
        return -1;
    }
    if (header.dims != IVF8_INDEX_DIMS || header.lanes != IVF8_INDEX_LANES) {
        set_error(err, err_len, "ivf8: invalid dimensions or lanes");
        return -1;
    }
    if (header.n == 0 || header.k == 0 || header.blocks == 0) {
        set_error(err, err_len, "ivf8: empty index metadata");
        return -1;
    }

    *out = header;
    return 0;
}

int ivf8_index_compute_layout(const Ivf8Header *header, Ivf8Layout *out, char *err, size_t err_len) {
    if (header == NULL || out == NULL) {
        set_error(err, err_len, "ivf8: nil layout input");
        return -1;
    }

    size_t k = header->k;
    size_t dims = header->dims;
    size_t lanes = header->lanes;
    size_t blocks = header->blocks;
    size_t offset = IVF8_INDEX_HEADER_BYTES;
    size_t bytes = 0;
    size_t elements = 0;
    Ivf8Layout layout;
    memset(&layout, 0, sizeof(layout));

    layout.centroids_offset = offset;
    if (!mul_size(k, dims, &elements) ||
        !section_bytes(elements, sizeof(int16_t), &bytes) ||
        !add_size(&offset, bytes)) {
        set_error(err, err_len, "ivf8: centroids size overflow");
        return -1;
    }

    layout.offsets_offset = offset;
    if (!section_bytes(k + 1u, sizeof(uint32_t), &bytes) || !add_size(&offset, bytes)) {
        set_error(err, err_len, "ivf8: offsets size overflow");
        return -1;
    }

    layout.counts_offset = offset;
    if (!section_bytes(k, sizeof(uint32_t), &bytes) || !add_size(&offset, bytes)) {
        set_error(err, err_len, "ivf8: counts size overflow");
        return -1;
    }

    layout.bbox_min_offset = offset;
    if (!mul_size(k, dims, &elements) ||
        !section_bytes(elements, sizeof(int16_t), &bytes) ||
        !add_size(&offset, bytes)) {
        set_error(err, err_len, "ivf8: bbox_min size overflow");
        return -1;
    }

    layout.bbox_max_offset = offset;
    if (!mul_size(k, dims, &elements) ||
        !section_bytes(elements, sizeof(int16_t), &bytes) ||
        !add_size(&offset, bytes)) {
        set_error(err, err_len, "ivf8: bbox_max size overflow");
        return -1;
    }

    layout.radii_offset = offset;
    if (!section_bytes(k, sizeof(uint64_t), &bytes) || !add_size(&offset, bytes)) {
        set_error(err, err_len, "ivf8: radii size overflow");
        return -1;
    }

    layout.labels_offset = offset;
    if (!mul_size(blocks, lanes, &elements) ||
        !section_bytes(elements, sizeof(uint8_t), &bytes) ||
        !add_size(&offset, bytes)) {
        set_error(err, err_len, "ivf8: labels size overflow");
        return -1;
    }

    layout.block_data_offset = offset;
    if (!mul_size(blocks, dims, &elements) ||
        !mul_size(elements, lanes, &elements) ||
        !section_bytes(elements, sizeof(int16_t), &bytes) ||
        !add_size(&offset, bytes)) {
        set_error(err, err_len, "ivf8: block_data size overflow");
        return -1;
    }

    layout.total_size = offset;
    *out = layout;
    return 0;
}

static int validate_production_header(const Ivf8Header *header, char *err, size_t err_len) {
    if (header->n != IVF8_PRODUCTION_N ||
        header->k != IVF8_PRODUCTION_K ||
        header->dims != IVF8_INDEX_DIMS ||
        header->lanes != IVF8_INDEX_LANES ||
        header->blocks != IVF8_PRODUCTION_BLOCKS) {
        set_error(err, err_len, "ivf8: index metadata does not match production IVF8 K4096 format");
        return -1;
    }
    return 0;
}

int ivf8_index_validate(const Ivf8Index *index, char *err, size_t err_len) {
    if (index == NULL || index->map == NULL) {
        set_error(err, err_len, "ivf8: nil mapped index");
        return -1;
    }
    if (index->file_size != index->layout.total_size) {
        set_error(err, err_len, "ivf8: mapped file size mismatch");
        return -1;
    }
    if (index->offsets[0] != 0 || index->offsets[index->k] != index->blocks) {
        set_error(err, err_len, "ivf8: invalid cluster offsets");
        return -1;
    }

    uint64_t count_sum = 0;
    for (uint32_t i = 0; i < index->k; i++) {
        if (index->offsets[i + 1u] < index->offsets[i]) {
            set_error(err, err_len, "ivf8: cluster offsets are not monotonic");
            return -1;
        }
        if (index->offsets[i + 1u] > index->blocks) {
            set_error(err, err_len, "ivf8: cluster offset exceeds block count");
            return -1;
        }
        count_sum += index->counts[i];
    }
    if (count_sum != index->n) {
        set_error(err, err_len, "ivf8: counts do not sum to record count");
        return -1;
    }
    return 0;
}

int ivf8_index_open(const char *path, Ivf8Index *out, char *err, size_t err_len) {
    if (path == NULL || out == NULL) {
        set_error(err, err_len, "ivf8: nil open input");
        return -1;
    }
    memset(out, 0, sizeof(*out));
    out->fd = -1;

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        set_errno_error(err, err_len, "ivf8: open");
        return -1;
    }

    struct stat st;
    if (fstat(fd, &st) != 0) {
        set_errno_error(err, err_len, "ivf8: fstat");
        close(fd);
        return -1;
    }
    if (st.st_size < (off_t)IVF8_INDEX_HEADER_BYTES) {
        set_error(err, err_len, "ivf8: file too small");
        close(fd);
        return -1;
    }

    size_t file_size = (size_t)st.st_size;
    void *map = mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) {
        set_errno_error(err, err_len, "ivf8: mmap");
        close(fd);
        return -1;
    }

    Ivf8Header header;
    Ivf8Layout layout;
    if (ivf8_index_parse_header(map, file_size, &header, err, err_len) != 0 ||
        validate_production_header(&header, err, err_len) != 0 ||
        ivf8_index_compute_layout(&header, &layout, err, err_len) != 0) {
        munmap(map, file_size);
        close(fd);
        return -1;
    }
    if (layout.total_size != file_size) {
        set_error(err, err_len, "ivf8: file size does not match computed layout");
        munmap(map, file_size);
        close(fd);
        return -1;
    }

    const uint8_t *base = (const uint8_t *)map;
    out->fd = fd;
    out->file_size = file_size;
    out->map = map;
    out->version = header.version;
    out->n = header.n;
    out->k = header.k;
    out->dims = header.dims;
    out->lanes = header.lanes;
    out->blocks = header.blocks;
    out->layout = layout;
    out->centroids = (const int16_t *)(const void *)(base + layout.centroids_offset);
    out->offsets = (const uint32_t *)(const void *)(base + layout.offsets_offset);
    out->counts = (const uint32_t *)(const void *)(base + layout.counts_offset);
    out->bbox_min = (const int16_t *)(const void *)(base + layout.bbox_min_offset);
    out->bbox_max = (const int16_t *)(const void *)(base + layout.bbox_max_offset);
    out->radii = (const uint64_t *)(const void *)(base + layout.radii_offset);
    out->labels = base + layout.labels_offset;
    out->block_data = (const int16_t *)(const void *)(base + layout.block_data_offset);

    if (ivf8_index_validate(out, err, err_len) != 0) {
        ivf8_index_close(out);
        return -1;
    }
    return 0;
}

void ivf8_index_close(Ivf8Index *index) {
    if (index == NULL) {
        return;
    }
    if (index->map != NULL && index->file_size > 0) {
        (void)munmap(index->map, index->file_size);
    }
    if (index->fd >= 0) {
        (void)close(index->fd);
    }
    memset(index, 0, sizeof(*index));
    index->fd = -1;
}
