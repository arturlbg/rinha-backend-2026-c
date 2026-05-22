#define _POSIX_C_SOURCE 200809L

#include "ivf8_index.h"
#include "kdtree.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void usage(void) {
    fprintf(stderr, "usage: build_kdtree --index <index.bin> [--output <tree.bin>]\n");
}

static uint64_t now_ns(void) {
    struct timespec ts;
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

int main(int argc, char **argv) {
    const char *index_path = NULL;
    const char *output_path = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--index") == 0 && i + 1 < argc) {
            index_path = argv[++i];
        } else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            output_path = argv[++i];
        } else {
            usage();
            return 2;
        }
    }
    if (index_path == NULL) {
        usage();
        return 2;
    }

    char err[256];
    Ivf8Index index;
    if (ivf8_index_open(index_path, &index, err, sizeof(err)) != 0) {
        fprintf(stderr, "build_kdtree: %s\n", err);
        return 1;
    }

    printf("source_records=%u\n", kdtree_count_ivf8_records(&index));
    printf("source_blocks=%u\n", index.blocks);
    printf("node_size=%zu\n", sizeof(KdTreeNode));

    KdTree tree;
    uint64_t start = now_ns();
    if (kdtree_build_from_ivf8(&tree, &index) != 0) {
        fprintf(stderr, "build_kdtree: build failed: %s\n", strerror(errno));
        ivf8_index_close(&index);
        return 1;
    }
    uint64_t elapsed = now_ns() - start;

    printf("nodes=%u\n", tree.node_count);
    printf("root=%u\n", tree.root);
    printf("build_seconds=%.3f\n", (double)elapsed / 1000000000.0);
    printf("runtime_memory_bytes=%zu\n", kdtree_runtime_memory_bytes(&tree));
    printf("build_memory_bytes_estimate=%zu\n", kdtree_build_memory_bytes(&tree));
    printf("runtime_memory_mib=%.2f\n", (double)kdtree_runtime_memory_bytes(&tree) / 1048576.0);
    printf("build_memory_mib_estimate=%.2f\n", (double)kdtree_build_memory_bytes(&tree) / 1048576.0);

    if (output_path != NULL) {
        start = now_ns();
        if (kdtree_save_nodes(&tree, output_path) != 0) {
            fprintf(stderr, "build_kdtree: save %s failed: %s\n", output_path, strerror(errno));
            kdtree_free(&tree);
            ivf8_index_close(&index);
            return 1;
        }
        elapsed = now_ns() - start;
        printf("output=%s\n", output_path);
        printf("output_bytes=%zu\n", sizeof(KdTreeNode) * (size_t)tree.node_count + 64u);
        printf("save_seconds=%.3f\n", (double)elapsed / 1000000000.0);
    }

    kdtree_free(&tree);
    ivf8_index_close(&index);
    return 0;
}
