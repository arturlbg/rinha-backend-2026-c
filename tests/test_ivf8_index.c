#define _POSIX_C_SOURCE 200809L

#include "ivf8_index.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int failures = 0;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        failures++; \
    } \
} while (0)

static void put_u32_le(unsigned char *p, uint32_t value) {
    p[0] = (unsigned char)(value & 0xffu);
    p[1] = (unsigned char)((value >> 8) & 0xffu);
    p[2] = (unsigned char)((value >> 16) & 0xffu);
    p[3] = (unsigned char)((value >> 24) & 0xffu);
}

static void make_header(unsigned char raw[IVF8_INDEX_HEADER_BYTES],
                        uint32_t version,
                        uint32_t n,
                        uint32_t k,
                        uint32_t dims,
                        uint32_t lanes,
                        uint32_t blocks) {
    memset(raw, 0, IVF8_INDEX_HEADER_BYTES);
    memcpy(raw, IVF8_INDEX_MAGIC, IVF8_INDEX_MAGIC_BYTES);
    put_u32_le(raw + 8, version);
    put_u32_le(raw + 12, n);
    put_u32_le(raw + 16, k);
    put_u32_le(raw + 20, dims);
    put_u32_le(raw + 24, lanes);
    put_u32_le(raw + 28, blocks);
}

static void test_parse_header(void) {
    unsigned char raw[IVF8_INDEX_HEADER_BYTES];
    make_header(raw, IVF8_INDEX_VERSION, IVF8_PRODUCTION_N, IVF8_PRODUCTION_K,
                IVF8_INDEX_DIMS, IVF8_INDEX_LANES, IVF8_PRODUCTION_BLOCKS);

    Ivf8Header header;
    char err[128];
    CHECK(ivf8_index_parse_header(raw, sizeof(raw), &header, err, sizeof(err)) == 0);
    CHECK(header.version == IVF8_INDEX_VERSION);
    CHECK(header.n == IVF8_PRODUCTION_N);
    CHECK(header.k == IVF8_PRODUCTION_K);
    CHECK(header.dims == IVF8_INDEX_DIMS);
    CHECK(header.lanes == IVF8_INDEX_LANES);
    CHECK(header.blocks == IVF8_PRODUCTION_BLOCKS);
}

static void test_reject_bad_headers(void) {
    unsigned char raw[IVF8_INDEX_HEADER_BYTES];
    Ivf8Header header;
    char err[128];

    make_header(raw, IVF8_INDEX_VERSION, IVF8_PRODUCTION_N, IVF8_PRODUCTION_K,
                IVF8_INDEX_DIMS, IVF8_INDEX_LANES, IVF8_PRODUCTION_BLOCKS);
    raw[0] = 'X';
    CHECK(ivf8_index_parse_header(raw, sizeof(raw), &header, err, sizeof(err)) != 0);

    make_header(raw, 99u, IVF8_PRODUCTION_N, IVF8_PRODUCTION_K,
                IVF8_INDEX_DIMS, IVF8_INDEX_LANES, IVF8_PRODUCTION_BLOCKS);
    CHECK(ivf8_index_parse_header(raw, sizeof(raw), &header, err, sizeof(err)) != 0);

    make_header(raw, IVF8_INDEX_VERSION, IVF8_PRODUCTION_N, IVF8_PRODUCTION_K,
                13u, IVF8_INDEX_LANES, IVF8_PRODUCTION_BLOCKS);
    CHECK(ivf8_index_parse_header(raw, sizeof(raw), &header, err, sizeof(err)) != 0);
}

static void test_layout(void) {
    Ivf8Header header = {
        .version = IVF8_INDEX_VERSION,
        .n = IVF8_PRODUCTION_N,
        .k = IVF8_PRODUCTION_K,
        .dims = IVF8_INDEX_DIMS,
        .lanes = IVF8_INDEX_LANES,
        .blocks = IVF8_PRODUCTION_BLOCKS,
    };
    Ivf8Layout layout;
    char err[128];
    CHECK(ivf8_index_compute_layout(&header, &layout, err, sizeof(err)) == 0);
    CHECK(layout.centroids_offset == 64u);
    CHECK(layout.offsets_offset == 114752u);
    CHECK(layout.counts_offset == 131140u);
    CHECK(layout.bbox_min_offset == 147524u);
    CHECK(layout.bbox_max_offset == 262212u);
    CHECK(layout.radii_offset == 376900u);
    CHECK(layout.labels_offset == 409668u);
    CHECK(layout.block_data_offset == 3423908u);
    CHECK(layout.total_size == IVF8_PRODUCTION_FILE_BYTES);
}

static void test_reject_wrong_file_size(void) {
    unsigned char raw[IVF8_INDEX_HEADER_BYTES];
    make_header(raw, IVF8_INDEX_VERSION, IVF8_PRODUCTION_N, IVF8_PRODUCTION_K,
                IVF8_INDEX_DIMS, IVF8_INDEX_LANES, IVF8_PRODUCTION_BLOCKS);

    char path[128];
    (void)snprintf(path, sizeof(path), "/tmp/rinha-ivf8-test-%ld.bin", (long)getpid());
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    CHECK(fd >= 0);
    if (fd < 0) {
        return;
    }
    CHECK(write(fd, raw, sizeof(raw)) == (ssize_t)sizeof(raw));
    close(fd);

    Ivf8Index index;
    char err[128];
    CHECK(ivf8_index_open(path, &index, err, sizeof(err)) != 0);
    CHECK(unlink(path) == 0);
}

static void test_real_index_if_available(void) {
    const char *path = "/data/index.bin";
    if (access(path, R_OK) != 0) {
        puts("real IVF8 index not mounted at /data/index.bin; skipping real-index validation");
        return;
    }

    Ivf8Index index;
    char err[256];
    CHECK(ivf8_index_open(path, &index, err, sizeof(err)) == 0);
    if (failures != 0) {
        fprintf(stderr, "real index load error: %s\n", err);
        return;
    }

    CHECK(index.file_size == IVF8_PRODUCTION_FILE_BYTES);
    CHECK(index.n == IVF8_PRODUCTION_N);
    CHECK(index.k == IVF8_PRODUCTION_K);
    CHECK(index.dims == IVF8_INDEX_DIMS);
    CHECK(index.lanes == IVF8_INDEX_LANES);
    CHECK(index.blocks == IVF8_PRODUCTION_BLOCKS);
    CHECK(index.layout.total_size == index.file_size);
    CHECK(index.offsets[0] == 0);
    CHECK(index.offsets[index.k] == index.blocks);

    uint64_t count_sum = 0;
    for (uint32_t i = 0; i < index.k; i++) {
        CHECK(index.offsets[i + 1u] >= index.offsets[i]);
        CHECK(index.offsets[i + 1u] <= index.blocks);
        count_sum += index.counts[i];
    }
    CHECK(count_sum == index.n);
    CHECK(index.labels != NULL);
    CHECK(index.block_data != NULL);

    ivf8_index_close(&index);
}

int main(void) {
    test_parse_header();
    test_reject_bad_headers();
    test_layout();
    test_reject_wrong_file_size();
    test_real_index_if_available();

    if (failures != 0) {
        fprintf(stderr, "%d ivf8 index test failure(s)\n", failures);
        return 1;
    }
    puts("ivf8 index tests passed");
    return 0;
}
