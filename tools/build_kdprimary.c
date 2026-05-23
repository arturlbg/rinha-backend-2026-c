#define _POSIX_C_SOURCE 200809L

#include "ivf8_index.h"
#include "kdprimary.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void usage(void) {
    fprintf(stderr, "usage: build_kdprimary --index <index.bin> --output <kdprimary.bin> [--leaf-size N]\n");
}

static uint64_t now_ns(void) {
    struct timespec ts;
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

int main(int argc, char **argv) {
    const char *index_path = NULL;
    const char *output_path = NULL;
    uint32_t leaf_size = 32u;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--index") == 0 && i + 1 < argc) {
            index_path = argv[++i];
        } else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            output_path = argv[++i];
        } else if (strcmp(argv[i], "--leaf-size") == 0 && i + 1 < argc) {
            leaf_size = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else {
            usage();
            return 2;
        }
    }

    if (index_path == NULL || output_path == NULL || leaf_size == 0) {
        usage();
        return 2;
    }

    char err[256];
    Ivf8Index index;
    if (ivf8_index_open(index_path, &index, err, sizeof(err)) != 0) {
        fprintf(stderr, "build_kdprimary: %s\n", err);
        return 1;
    }

    printf("source_index=%s\n", index_path);
    printf("source_records=%u\n", index.n);
    printf("leaf_size=%u\n", leaf_size);
    printf("node_size=%zu\n", sizeof(KdPrimaryNode));

    KdPrimaryBuild build;
    uint64_t start = now_ns();
    if (kdprimary_build_from_ivf8(&build, &index, leaf_size, err, sizeof(err)) != 0) {
        fprintf(stderr, "build_kdprimary: %s\n", err);
        ivf8_index_close(&index);
        return 1;
    }
    uint64_t build_elapsed = now_ns() - start;

    start = now_ns();
    if (kdprimary_save(&build, output_path, err, sizeof(err)) != 0) {
        fprintf(stderr, "build_kdprimary: %s\n", err);
        kdprimary_build_free(&build);
        ivf8_index_close(&index);
        return 1;
    }
    uint64_t save_elapsed = now_ns() - start;

    size_t file_bytes = kdprimary_expected_file_bytes(build.count, build.node_count);
    printf("nodes=%u\n", build.node_count);
    printf("root=%u\n", build.root);
    printf("points=%u\n", build.count);
    printf("build_seconds=%.3f\n", (double)build_elapsed / 1000000000.0);
    printf("save_seconds=%.3f\n", (double)save_elapsed / 1000000000.0);
    printf("output=%s\n", output_path);
    printf("output_bytes=%zu\n", file_bytes);
    printf("output_mib=%.2f\n", (double)file_bytes / 1048576.0);
    printf("runtime_memory_bytes=%zu\n", file_bytes);
    printf("runtime_memory_mib=%.2f\n", (double)file_bytes / 1048576.0);
    printf("build_memory_bytes_estimate=%zu\n", kdprimary_build_memory_bytes(&build));
    printf("build_memory_mib_estimate=%.2f\n", (double)kdprimary_build_memory_bytes(&build) / 1048576.0);

    kdprimary_build_free(&build);
    ivf8_index_close(&index);
    return 0;
}
