#define _POSIX_C_SOURCE 200809L

#include "ivf8_index.h"
#include "kdclass3.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void usage(void) {
    fprintf(stderr, "usage: build_kdclass3 --index <index.bin> --output <kdclass3.bin> [--leaf-size N]\n");
}

static uint64_t now_ns(void) {
    struct timespec ts;
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

int main(int argc, char **argv) {
    const char *index_path = NULL;
    const char *output_path = NULL;
    uint32_t leaf_size = 64u;

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
        fprintf(stderr, "build_kdclass3: %s\n", err);
        return 1;
    }

    KdClass3Build build;
    uint64_t build_start = now_ns();
    if (kdclass3_build_from_ivf8(&build, &index, leaf_size, err, sizeof(err)) != 0) {
        fprintf(stderr, "build_kdclass3: %s\n", err);
        ivf8_index_close(&index);
        return 1;
    }
    uint64_t build_ns = now_ns() - build_start;

    uint64_t save_start = now_ns();
    if (kdclass3_save(&build, output_path, err, sizeof(err)) != 0) {
        fprintf(stderr, "build_kdclass3: %s\n", err);
        kdclass3_build_free(&build);
        ivf8_index_close(&index);
        return 1;
    }
    uint64_t save_ns = now_ns() - save_start;

    size_t file_bytes = kdclass3_expected_file_bytes(build.fraud.node_count,
                                                     build.fraud.block_count,
                                                     build.legit.node_count,
                                                     build.legit.block_count);
    printf("index=%s\n", index_path);
    printf("output=%s\n", output_path);
    printf("leaf_size=%u\n", leaf_size);
    printf("fraud_points=%u\n", build.fraud.count);
    printf("legit_points=%u\n", build.legit.count);
    printf("fraud_nodes=%u\n", build.fraud.node_count);
    printf("legit_nodes=%u\n", build.legit.node_count);
    printf("fraud_blocks=%u\n", build.fraud.block_count);
    printf("legit_blocks=%u\n", build.legit.block_count);
    printf("file_bytes=%zu\n", file_bytes);
    printf("file_mib=%.2f\n", (double)file_bytes / 1048576.0);
    printf("build_memory_bytes_estimate=%zu\n", kdclass3_build_memory_bytes(&build));
    printf("build_memory_mib_estimate=%.2f\n", (double)kdclass3_build_memory_bytes(&build) / 1048576.0);
    printf("build_ms=%.3f\n", (double)build_ns / 1000000.0);
    printf("save_ms=%.3f\n", (double)save_ns / 1000000.0);

    kdclass3_build_free(&build);
    ivf8_index_close(&index);
    return 0;
}
