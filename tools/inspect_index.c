#include "ivf8_index.h"

#include <stdio.h>

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: inspect_index <index.bin>\n");
        return 2;
    }

    char err[256];
    Ivf8Index index;
    if (ivf8_index_open(argv[1], &index, err, sizeof(err)) != 0) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }

    printf("file_size=%zu\n", index.file_size);
    printf("version=%u n=%u k=%u dims=%u lanes=%u blocks=%u\n",
           index.version, index.n, index.k, index.dims, index.lanes, index.blocks);
    printf("section_offsets centroids=%zu offsets=%zu counts=%zu bbox_min=%zu bbox_max=%zu radii=%zu labels=%zu block_data=%zu total=%zu\n",
           index.layout.centroids_offset,
           index.layout.offsets_offset,
           index.layout.counts_offset,
           index.layout.bbox_min_offset,
           index.layout.bbox_max_offset,
           index.layout.radii_offset,
           index.layout.labels_offset,
           index.layout.block_data_offset,
           index.layout.total_size);
    printf("offsets[0..3]=%u,%u,%u,%u offsets[k]=%u\n",
           index.offsets[0], index.offsets[1], index.offsets[2], index.offsets[3], index.offsets[index.k]);
    printf("counts[0..3]=%u,%u,%u,%u\n",
           index.counts[0], index.counts[1], index.counts[2], index.counts[3]);
    printf("labels[0..7]=%u,%u,%u,%u,%u,%u,%u,%u\n",
           index.labels[0], index.labels[1], index.labels[2], index.labels[3],
           index.labels[4], index.labels[5], index.labels[6], index.labels[7]);

    ivf8_index_close(&index);
    return 0;
}

