#define _POSIX_C_SOURCE 200809L

#include "kdprimary2.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define KDPRIMARY2_SAMPLE_TARGET 1024u
#define KDPRIMARY2_STACK_CAPACITY 4096u

static volatile uint64_t kdprimary2_touch_sink = 0;

typedef struct {
    char magic[8];
    uint32_t version;
    uint32_t n;
    uint32_t dims;
    uint32_t leaf_size;
    uint32_t node_count;
    uint32_t block_count;
    uint32_t root;
    uint32_t node_size;
    uint32_t vector_block_bytes;
    uint32_t label_block_bytes;
    uint32_t reserved0;
    uint64_t nodes_offset;
    uint64_t block_data_offset;
    uint64_t labels_offset;
    uint64_t total_size;
    uint8_t reserved[40];
} KdPrimary2FileHeader;

typedef struct {
    KdPrimary2Build *build;
    const int16_t *source_vectors;
    const uint8_t *source_labels;
    uint32_t *refs;
    uint32_t next_node;
    uint32_t next_point;
    uint32_t block_capacity;
} BuildContext;

typedef struct {
    uint32_t node;
    uint64_t lower_bound;
} SearchStackEntry;

_Static_assert(sizeof(KdPrimary2Node) == 84u, "KdPrimary2Node must stay compact");
_Static_assert(sizeof(KdPrimary2FileHeader) == KDPRIMARY2_HEADER_BYTES, "KdPrimary2 header size mismatch");

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

static bool add_size(size_t *value, size_t add) {
    if (*value > SIZE_MAX - add) {
        return false;
    }
    *value += add;
    return true;
}

static size_t block_data_bytes(uint32_t block_count) {
    return (size_t)block_count * IVF8_INDEX_DIMS * IVF8_INDEX_LANES * sizeof(int16_t);
}

static size_t label_bytes(uint32_t block_count) {
    return (size_t)block_count * IVF8_INDEX_LANES * sizeof(uint8_t);
}

size_t kdprimary2_expected_file_bytes(uint32_t node_count, uint32_t block_count) {
    size_t total = KDPRIMARY2_HEADER_BYTES;
    if (!add_size(&total, (size_t)node_count * sizeof(KdPrimary2Node)) ||
        !add_size(&total, block_data_bytes(block_count)) ||
        !add_size(&total, label_bytes(block_count))) {
        return 0;
    }
    return total;
}

uint64_t kdprimary2_distance14(const int16_t a[IVF8_INDEX_DIMS], const int16_t b[IVF8_INDEX_DIMS]) {
    uint64_t sum = 0;
    for (uint32_t dim = 0; dim < IVF8_INDEX_DIMS; dim++) {
        int64_t diff = (int64_t)a[dim] - (int64_t)b[dim];
        sum += (uint64_t)(diff * diff);
    }
    return sum;
}

uint64_t kdprimary2_bbox_distance(const KdPrimary2Node *node, const int16_t query[IVF8_INDEX_DIMS]) {
    uint64_t sum = 0;
    if (node == NULL || query == NULL) {
        return UINT64_MAX;
    }
    for (uint32_t dim = 0; dim < IVF8_INDEX_DIMS; dim++) {
        int64_t diff = 0;
        if (query[dim] < node->bbox_min[dim]) {
            diff = (int64_t)node->bbox_min[dim] - (int64_t)query[dim];
        } else if (query[dim] > node->bbox_max[dim]) {
            diff = (int64_t)query[dim] - (int64_t)node->bbox_max[dim];
        }
        sum += (uint64_t)(diff * diff);
    }
    return sum;
}

static uint64_t kdprimary2_bbox_distance_limit(const KdPrimary2Node *node,
                                               const int16_t query[IVF8_INDEX_DIMS],
                                               uint64_t limit) {
    uint64_t sum = 0;
    if (node == NULL || query == NULL) {
        return UINT64_MAX;
    }
    for (uint32_t dim = 0; dim < IVF8_INDEX_DIMS; dim++) {
        int64_t diff = 0;
        if (query[dim] < node->bbox_min[dim]) {
            diff = (int64_t)node->bbox_min[dim] - (int64_t)query[dim];
        } else if (query[dim] > node->bbox_max[dim]) {
            diff = (int64_t)query[dim] - (int64_t)node->bbox_max[dim];
        }
        sum += (uint64_t)(diff * diff);
        if (sum > limit) {
            return sum;
        }
    }
    return sum;
}

void kdprimary2_leaf_block_distances_avx2(const int16_t *block_data,
                                          uint32_t block,
                                          const int16_t query[IVF8_INDEX_DIMS],
                                          uint64_t out[IVF8_INDEX_LANES]) {
    ivf8_block_distances_avx2(block_data, block, query, out);
}

static int16_t ref_value(const BuildContext *ctx, uint32_t ref, uint32_t dim) {
    return ctx->source_vectors[(size_t)ref * IVF8_INDEX_DIMS + dim];
}

static bool ref_less_dim(const BuildContext *ctx, uint32_t a, uint32_t b, uint8_t dim) {
    int16_t av = ref_value(ctx, a, dim);
    int16_t bv = ref_value(ctx, b, dim);
    return av < bv || (av == bv && a < b);
}

static int ref_compare_dim(const BuildContext *ctx, uint32_t a, uint32_t b, uint8_t dim) {
    if (ref_less_dim(ctx, a, b, dim)) {
        return -1;
    }
    if (ref_less_dim(ctx, b, a, dim)) {
        return 1;
    }
    return 0;
}

static void swap_u32(uint32_t *a, uint32_t *b) {
    uint32_t tmp = *a;
    *a = *b;
    *b = tmp;
}

static void nth_element_refs(const BuildContext *ctx,
                             uint32_t *refs,
                             size_t left,
                             size_t nth,
                             size_t right,
                             uint8_t dim) {
    while (right - left > 1u) {
        uint32_t pivot = refs[left + (right - left) / 2u];
        size_t lt = left;
        size_t i = left;
        size_t gt = right;
        while (i < gt) {
            int cmp = ref_compare_dim(ctx, refs[i], pivot, dim);
            if (cmp < 0) {
                swap_u32(&refs[lt], &refs[i]);
                lt++;
                i++;
            } else if (cmp > 0) {
                gt--;
                swap_u32(&refs[i], &refs[gt]);
            } else {
                i++;
            }
        }
        if (nth < lt) {
            right = lt;
        } else if (nth >= gt) {
            left = gt;
        } else {
            return;
        }
    }
}

static uint8_t choose_split_dim(const BuildContext *ctx, size_t lo, size_t hi) {
    int16_t min_values[IVF8_INDEX_DIMS];
    int16_t max_values[IVF8_INDEX_DIMS];
    for (uint32_t dim = 0; dim < IVF8_INDEX_DIMS; dim++) {
        min_values[dim] = INT16_MAX;
        max_values[dim] = INT16_MIN;
    }

    size_t n = hi - lo;
    size_t stride = n > KDPRIMARY2_SAMPLE_TARGET ? n / KDPRIMARY2_SAMPLE_TARGET : 1u;
    for (size_t i = lo; i < hi; i += stride) {
        uint32_t ref = ctx->refs[i];
        for (uint32_t dim = 0; dim < IVF8_INDEX_DIMS; dim++) {
            int16_t value = ref_value(ctx, ref, dim);
            if (value < min_values[dim]) {
                min_values[dim] = value;
            }
            if (value > max_values[dim]) {
                max_values[dim] = value;
            }
        }
    }

    uint8_t best_dim = 0;
    int32_t best_range = -1;
    for (uint32_t dim = 0; dim < IVF8_INDEX_DIMS; dim++) {
        int32_t range = (int32_t)max_values[dim] - (int32_t)min_values[dim];
        if (range > best_range) {
            best_range = range;
            best_dim = (uint8_t)dim;
        }
    }
    return best_dim;
}

static int ensure_block_capacity(BuildContext *ctx, uint32_t needed) {
    if (needed <= ctx->block_capacity) {
        return 0;
    }
    uint32_t next = ctx->block_capacity == 0 ? 1024u : ctx->block_capacity;
    while (next < needed) {
        if (next > UINT32_MAX / 2u) {
            return -1;
        }
        next *= 2u;
    }

    int16_t *block_data = (int16_t *)realloc(ctx->build->block_data, block_data_bytes(next));
    if (block_data == NULL) {
        return -1;
    }
    ctx->build->block_data = block_data;

    uint8_t *labels = (uint8_t *)realloc(ctx->build->labels, label_bytes(next));
    if (labels == NULL) {
        return -1;
    }
    ctx->build->labels = labels;

    size_t old_block_bytes = block_data_bytes(ctx->block_capacity);
    size_t new_block_bytes = block_data_bytes(next);
    size_t old_label_bytes = label_bytes(ctx->block_capacity);
    size_t new_label_bytes = label_bytes(next);
    if (new_block_bytes > old_block_bytes) {
        memset((uint8_t *)block_data + old_block_bytes, 0, new_block_bytes - old_block_bytes);
    }
    if (new_label_bytes > old_label_bytes) {
        memset(labels + old_label_bytes, 0, new_label_bytes - old_label_bytes);
    }
    ctx->block_capacity = next;
    return 0;
}

static void bbox_init_empty(KdPrimary2Node *node) {
    for (uint32_t dim = 0; dim < IVF8_INDEX_DIMS; dim++) {
        node->bbox_min[dim] = INT16_MAX;
        node->bbox_max[dim] = INT16_MIN;
    }
}

static void bbox_add_ref(KdPrimary2Node *node, const BuildContext *ctx, uint32_t ref) {
    for (uint32_t dim = 0; dim < IVF8_INDEX_DIMS; dim++) {
        int16_t value = ref_value(ctx, ref, dim);
        if (value < node->bbox_min[dim]) {
            node->bbox_min[dim] = value;
        }
        if (value > node->bbox_max[dim]) {
            node->bbox_max[dim] = value;
        }
    }
}

static void bbox_union_child(KdPrimary2Node *node, const KdPrimary2Node *child) {
    if (child == NULL) {
        return;
    }
    for (uint32_t dim = 0; dim < IVF8_INDEX_DIMS; dim++) {
        if (child->bbox_min[dim] < node->bbox_min[dim]) {
            node->bbox_min[dim] = child->bbox_min[dim];
        }
        if (child->bbox_max[dim] > node->bbox_max[dim]) {
            node->bbox_max[dim] = child->bbox_max[dim];
        }
    }
}

static int copy_leaf_points(BuildContext *ctx, const uint32_t *refs, size_t lo, size_t hi, KdPrimary2Node *node) {
    uint32_t count = (uint32_t)(hi - lo);
    uint32_t blocks = (count + IVF8_INDEX_LANES - 1u) / IVF8_INDEX_LANES;
    uint32_t block_start = ctx->build->block_count;
    if (ensure_block_capacity(ctx, block_start + blocks) != 0) {
        return -1;
    }

    node->left = KDPRIMARY2_INVALID_NODE;
    node->right = KDPRIMARY2_INVALID_NODE;
    node->block_start = block_start;
    node->point_start = ctx->next_point;
    node->count = count;
    node->split_value = 0;
    node->split_dim = 0;
    node->flags = KDPRIMARY2_LEAF_FLAG;
    node->reserved = 0;
    bbox_init_empty(node);

    for (uint32_t block = 0; block < blocks; block++) {
        uint32_t global_block = block_start + block;
        int16_t *block_data = ctx->build->block_data +
                              (size_t)global_block * IVF8_INDEX_DIMS * IVF8_INDEX_LANES;
        uint8_t *labels = ctx->build->labels + (size_t)global_block * IVF8_INDEX_LANES;
        memset(block_data, 0, IVF8_INDEX_DIMS * IVF8_INDEX_LANES * sizeof(int16_t));
        memset(labels, 0, IVF8_INDEX_LANES * sizeof(uint8_t));
    }

    for (uint32_t i = 0; i < count; i++) {
        uint32_t ref = refs[lo + i];
        uint32_t block = block_start + i / IVF8_INDEX_LANES;
        uint32_t lane = i % IVF8_INDEX_LANES;
        int16_t *block_data = ctx->build->block_data +
                              (size_t)block * IVF8_INDEX_DIMS * IVF8_INDEX_LANES;
        for (uint32_t dim = 0; dim < IVF8_INDEX_DIMS; dim++) {
            block_data[dim * IVF8_INDEX_LANES + lane] =
                ctx->source_vectors[(size_t)ref * IVF8_INDEX_DIMS + dim];
        }
        ctx->build->labels[(size_t)block * IVF8_INDEX_LANES + lane] =
            ctx->source_labels[ref] != 0 ? 1u : 0u;
        bbox_add_ref(node, ctx, ref);
    }

    ctx->next_point += count;
    ctx->build->block_count += blocks;
    return 0;
}

static uint32_t build_range(BuildContext *ctx, size_t lo, size_t hi) {
    if (lo >= hi) {
        return KDPRIMARY2_INVALID_NODE;
    }

    uint32_t node_index = ctx->next_node++;
    KdPrimary2Node *node = &ctx->build->nodes[node_index];
    memset(node, 0, sizeof(*node));
    bbox_init_empty(node);

    size_t n = hi - lo;
    if (n <= ctx->build->leaf_size) {
        if (copy_leaf_points(ctx, ctx->refs, lo, hi, node) != 0) {
            return KDPRIMARY2_INVALID_NODE;
        }
        return node_index;
    }

    uint8_t dim = choose_split_dim(ctx, lo, hi);
    size_t mid = lo + n / 2u;
    nth_element_refs(ctx, ctx->refs, lo, mid, hi, dim);

    node->left = build_range(ctx, lo, mid);
    node->right = build_range(ctx, mid, hi);
    node->block_start = 0;
    node->point_start = 0;
    node->count = 0;
    node->split_dim = dim;
    node->split_value = ref_value(ctx, ctx->refs[mid], dim);
    node->flags = 0;
    node->reserved = 0;
    if (node->left != KDPRIMARY2_INVALID_NODE) {
        bbox_union_child(node, &ctx->build->nodes[node->left]);
    }
    if (node->right != KDPRIMARY2_INVALID_NODE) {
        bbox_union_child(node, &ctx->build->nodes[node->right]);
    }
    return node_index;
}

static int fill_ivf8_points(const Ivf8Index *index, int16_t *vectors, uint8_t *labels, uint32_t count) {
    uint32_t pos = 0;
    for (uint32_t cluster = 0; cluster < index->k; cluster++) {
        uint32_t remaining = index->counts[cluster];
        for (uint32_t block = index->offsets[cluster];
             block < index->offsets[cluster + 1u] && remaining > 0;
             block++) {
            uint32_t lanes = remaining < IVF8_INDEX_LANES ? remaining : IVF8_INDEX_LANES;
            for (uint32_t lane = 0; lane < lanes; lane++) {
                if (pos >= count) {
                    return -1;
                }
                for (uint32_t dim = 0; dim < IVF8_INDEX_DIMS; dim++) {
                    vectors[(size_t)pos * IVF8_INDEX_DIMS + dim] =
                        index->block_data[(size_t)block * IVF8_INDEX_DIMS * IVF8_INDEX_LANES +
                                          (size_t)dim * IVF8_INDEX_LANES +
                                          lane];
                }
                labels[pos] = index->labels[(size_t)block * IVF8_INDEX_LANES + lane] != 0 ? 1u : 0u;
                pos++;
            }
            remaining -= lanes;
        }
    }
    return pos == count ? 0 : -1;
}

static int build_from_source(KdPrimary2Build *build,
                             const int16_t *source_vectors,
                             const uint8_t *source_labels,
                             uint32_t count,
                             uint32_t leaf_size,
                             char *err,
                             size_t err_len) {
    if (build == NULL || source_vectors == NULL || source_labels == NULL || count == 0 || leaf_size == 0) {
        set_error(err, err_len, "kdprimary2: invalid build input");
        return -1;
    }
    memset(build, 0, sizeof(*build));
    build->count = count;
    build->leaf_size = leaf_size;
    build->root = KDPRIMARY2_INVALID_NODE;

    build->nodes = (KdPrimary2Node *)calloc(count, sizeof(KdPrimary2Node));
    uint32_t *refs = (uint32_t *)malloc((size_t)count * sizeof(uint32_t));
    if (build->nodes == NULL || refs == NULL) {
        free(refs);
        kdprimary2_build_free(build);
        set_error(err, err_len, "kdprimary2: out of memory during build");
        return -1;
    }
    for (uint32_t i = 0; i < count; i++) {
        refs[i] = i;
    }

    BuildContext ctx = {
        .build = build,
        .source_vectors = source_vectors,
        .source_labels = source_labels,
        .refs = refs,
        .next_node = 0,
        .next_point = 0,
        .block_capacity = 0,
    };
    build->root = build_range(&ctx, 0, count);
    build->node_count = ctx.next_node;
    free(refs);

    if (build->root == KDPRIMARY2_INVALID_NODE ||
        build->node_count == 0 ||
        ctx.next_point != count ||
        build->node_count > count ||
        build->block_count == 0) {
        kdprimary2_build_free(build);
        set_error(err, err_len, "kdprimary2: inconsistent build result");
        return -1;
    }
    return 0;
}

int kdprimary2_build_from_points(KdPrimary2Build *build,
                                 const int16_t *vectors,
                                 const uint8_t *labels,
                                 uint32_t count,
                                 uint32_t leaf_size,
                                 char *err,
                                 size_t err_len) {
    return build_from_source(build, vectors, labels, count, leaf_size, err, err_len);
}

int kdprimary2_build_from_ivf8(KdPrimary2Build *build,
                               const Ivf8Index *index,
                               uint32_t leaf_size,
                               char *err,
                               size_t err_len) {
    if (build == NULL || index == NULL || index->n == 0) {
        set_error(err, err_len, "kdprimary2: invalid IVF8 source");
        return -1;
    }

    int16_t *source_vectors = (int16_t *)malloc((size_t)index->n * IVF8_INDEX_DIMS * sizeof(int16_t));
    uint8_t *source_labels = (uint8_t *)malloc((size_t)index->n * sizeof(uint8_t));
    if (source_vectors == NULL || source_labels == NULL) {
        free(source_vectors);
        free(source_labels);
        set_error(err, err_len, "kdprimary2: out of memory copying IVF8 source");
        return -1;
    }
    if (fill_ivf8_points(index, source_vectors, source_labels, index->n) != 0) {
        free(source_vectors);
        free(source_labels);
        set_error(err, err_len, "kdprimary2: failed to extract IVF8 records");
        return -1;
    }

    int result = build_from_source(build, source_vectors, source_labels, index->n, leaf_size, err, err_len);
    free(source_vectors);
    free(source_labels);
    return result;
}

void kdprimary2_build_free(KdPrimary2Build *build) {
    if (build == NULL) {
        return;
    }
    free(build->nodes);
    free(build->block_data);
    free(build->labels);
    memset(build, 0, sizeof(*build));
    build->root = KDPRIMARY2_INVALID_NODE;
}

size_t kdprimary2_build_memory_bytes(const KdPrimary2Build *build) {
    if (build == NULL) {
        return 0;
    }
    return (size_t)build->node_count * sizeof(KdPrimary2Node) +
           block_data_bytes(build->block_count) +
           label_bytes(build->block_count);
}

int kdprimary2_save(const KdPrimary2Build *build, const char *path, char *err, size_t err_len) {
    if (build == NULL || build->nodes == NULL || build->block_data == NULL || build->labels == NULL || path == NULL) {
        set_error(err, err_len, "kdprimary2: invalid save input");
        return -1;
    }

    size_t node_bytes = (size_t)build->node_count * sizeof(KdPrimary2Node);
    size_t vector_bytes = block_data_bytes(build->block_count);
    size_t labels_len = label_bytes(build->block_count);
    size_t total = KDPRIMARY2_HEADER_BYTES;
    if (!add_size(&total, node_bytes) || !add_size(&total, vector_bytes) || !add_size(&total, labels_len)) {
        set_error(err, err_len, "kdprimary2: save size overflow");
        return -1;
    }
    if (vector_bytes > UINT32_MAX || labels_len > UINT32_MAX) {
        set_error(err, err_len, "kdprimary2: section too large for v2 header");
        return -1;
    }

    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        set_errno_error(err, err_len, "kdprimary2: fopen");
        return -1;
    }

    KdPrimary2FileHeader header;
    memset(&header, 0, sizeof(header));
    memcpy(header.magic, KDPRIMARY2_MAGIC, KDPRIMARY2_MAGIC_BYTES);
    header.version = KDPRIMARY2_VERSION;
    header.n = build->count;
    header.dims = IVF8_INDEX_DIMS;
    header.leaf_size = build->leaf_size;
    header.node_count = build->node_count;
    header.block_count = build->block_count;
    header.root = build->root;
    header.node_size = sizeof(KdPrimary2Node);
    header.vector_block_bytes = (uint32_t)vector_bytes;
    header.label_block_bytes = (uint32_t)labels_len;
    header.nodes_offset = KDPRIMARY2_HEADER_BYTES;
    header.block_data_offset = header.nodes_offset + node_bytes;
    header.labels_offset = header.block_data_offset + vector_bytes;
    header.total_size = total;

    bool ok = fwrite(&header, sizeof(header), 1, file) == 1 &&
              fwrite(build->nodes, sizeof(KdPrimary2Node), build->node_count, file) == build->node_count &&
              fwrite(build->block_data,
                     sizeof(int16_t),
                     (size_t)build->block_count * IVF8_INDEX_DIMS * IVF8_INDEX_LANES,
                     file) == (size_t)build->block_count * IVF8_INDEX_DIMS * IVF8_INDEX_LANES &&
              fwrite(build->labels, sizeof(uint8_t), labels_len, file) == labels_len;
    int saved_errno = errno;
    fclose(file);
    if (!ok) {
        errno = saved_errno;
        set_errno_error(err, err_len, "kdprimary2: fwrite");
        return -1;
    }
    return 0;
}

static int validate_header(const KdPrimary2FileHeader *header, size_t file_size, char *err, size_t err_len) {
    if (memcmp(header->magic, KDPRIMARY2_MAGIC, KDPRIMARY2_MAGIC_BYTES) != 0) {
        set_error(err, err_len, "kdprimary2: invalid magic");
        return -1;
    }
    if (header->version != KDPRIMARY2_VERSION ||
        header->dims != IVF8_INDEX_DIMS ||
        header->node_size != sizeof(KdPrimary2Node) ||
        header->n == 0 ||
        header->node_count == 0 ||
        header->block_count == 0 ||
        header->leaf_size == 0 ||
        header->root >= header->node_count) {
        set_error(err, err_len, "kdprimary2: invalid metadata");
        return -1;
    }

    size_t node_bytes = (size_t)header->node_count * sizeof(KdPrimary2Node);
    size_t vector_bytes = block_data_bytes(header->block_count);
    size_t labels_len = label_bytes(header->block_count);
    size_t expected = KDPRIMARY2_HEADER_BYTES;
    if (!add_size(&expected, node_bytes) || !add_size(&expected, vector_bytes) || !add_size(&expected, labels_len)) {
        set_error(err, err_len, "kdprimary2: file size overflow");
        return -1;
    }
    if (header->vector_block_bytes != vector_bytes ||
        header->label_block_bytes != labels_len ||
        header->nodes_offset != KDPRIMARY2_HEADER_BYTES ||
        header->block_data_offset != header->nodes_offset + node_bytes ||
        header->labels_offset != header->block_data_offset + vector_bytes ||
        header->total_size != expected ||
        expected != file_size) {
        set_error(err, err_len, "kdprimary2: section layout mismatch");
        return -1;
    }
    return 0;
}

int kdprimary2_open(const char *path, KdPrimary2Index *out, char *err, size_t err_len) {
    if (path == NULL || out == NULL) {
        set_error(err, err_len, "kdprimary2: nil open input");
        return -1;
    }
    memset(out, 0, sizeof(*out));
    out->fd = -1;

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        set_errno_error(err, err_len, "kdprimary2: open");
        return -1;
    }

    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < (off_t)KDPRIMARY2_HEADER_BYTES) {
        int saved = errno;
        close(fd);
        errno = saved == 0 ? EINVAL : saved;
        set_errno_error(err, err_len, "kdprimary2: fstat");
        return -1;
    }

    size_t file_size = (size_t)st.st_size;
    void *map = mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) {
        set_errno_error(err, err_len, "kdprimary2: mmap");
        close(fd);
        return -1;
    }

    const KdPrimary2FileHeader *header = (const KdPrimary2FileHeader *)map;
    if (validate_header(header, file_size, err, err_len) != 0) {
        (void)munmap(map, file_size);
        close(fd);
        return -1;
    }

    const uint8_t *base = (const uint8_t *)map;
    out->fd = fd;
    out->file_size = file_size;
    out->map = map;
    out->count = header->n;
    out->node_count = header->node_count;
    out->block_count = header->block_count;
    out->root = header->root;
    out->leaf_size = header->leaf_size;
    out->nodes = (const KdPrimary2Node *)(const void *)(base + header->nodes_offset);
    out->block_data = (const int16_t *)(const void *)(base + header->block_data_offset);
    out->labels = base + header->labels_offset;
    return 0;
}

void kdprimary2_close(KdPrimary2Index *index) {
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

size_t kdprimary2_runtime_memory_bytes(const KdPrimary2Index *index) {
    return index != NULL ? index->file_size : 0;
}

uint64_t kdprimary2_touch_pages(const KdPrimary2Index *index) {
    if (index == NULL || index->map == NULL || index->file_size == 0) {
        return 0;
    }
    const uint8_t *bytes = (const uint8_t *)index->map;
    uint64_t sum = 0;
    for (size_t offset = 0; offset < index->file_size; offset += 4096u) {
        sum += bytes[offset];
    }
    sum += bytes[index->file_size - 1u];
    kdprimary2_touch_sink ^= sum;
    return sum;
}

static bool top_accepts(const Ivf8Neighbor top[KDPRIMARY2_TOP_K], uint64_t distance, uint32_t seq) {
    return distance < top[KDPRIMARY2_TOP_K - 1u].distance ||
           (distance == top[KDPRIMARY2_TOP_K - 1u].distance && seq < top[KDPRIMARY2_TOP_K - 1u].seq);
}

static void scan_leaf(const KdPrimary2Index *index,
                      const KdPrimary2Node *node,
                      const int16_t query[IVF8_INDEX_DIMS],
                      KdPrimary2SearchResult *result) {
    result->stats.leaves_visited++;
    uint32_t remaining = node->count;
    uint32_t point_offset = 0;
    uint64_t distances[IVF8_INDEX_LANES];

    for (uint32_t block_offset = 0; remaining > 0; block_offset++) {
        uint32_t block = node->block_start + block_offset;
        kdprimary2_leaf_block_distances_avx2(index->block_data, block, query, distances);
        uint32_t lanes = remaining < IVF8_INDEX_LANES ? remaining : IVF8_INDEX_LANES;
        const uint8_t *labels = index->labels + (size_t)block * IVF8_INDEX_LANES;
        for (uint32_t lane = 0; lane < lanes; lane++) {
            uint32_t seq = node->point_start + point_offset + lane;
            uint64_t distance = distances[lane];
            result->stats.points_evaluated++;
            if (top_accepts(result->top, distance, seq)) {
                Ivf8Neighbor candidate = {
                    .distance = distance,
                    .fraud = labels[lane] != 0 ? 1u : 0u,
                    .seq = seq,
                };
                ivf8_top5_insert(result->top, candidate);
            }
        }
        remaining -= lanes;
        point_offset += lanes;
    }
}

static bool top_full(const Ivf8Neighbor top[KDPRIMARY2_TOP_K]) {
    return top[KDPRIMARY2_TOP_K - 1u].seq != UINT32_MAX;
}

static bool push_node(SearchStackEntry stack[KDPRIMARY2_STACK_CAPACITY],
                      uint32_t *stack_len,
                      uint32_t node,
                      uint64_t lower_bound,
                      KdPrimary2SearchResult *result) {
    if (node == KDPRIMARY2_INVALID_NODE) {
        return true;
    }
    if (*stack_len >= KDPRIMARY2_STACK_CAPACITY) {
        return false;
    }
    stack[*stack_len].node = node;
    stack[*stack_len].lower_bound = lower_bound;
    (*stack_len)++;
    if (*stack_len > result->stats.max_stack) {
        result->stats.max_stack = *stack_len;
    }
    return true;
}

KdPrimary2SearchResult kdprimary2_search_top5(const KdPrimary2Index *index,
                                              const int16_t query[IVF8_INDEX_DIMS]) {
    KdPrimary2SearchResult result;
    memset(&result, 0, sizeof(result));
    ivf8_top5_init(result.top);
    if (index == NULL || index->nodes == NULL || index->block_data == NULL || index->labels == NULL || query == NULL) {
        return result;
    }

    SearchStackEntry stack[KDPRIMARY2_STACK_CAPACITY];
    uint32_t stack_len = 0;
    (void)push_node(stack, &stack_len, index->root, 0, &result);

    while (stack_len > 0) {
        SearchStackEntry entry = stack[--stack_len];
        if (entry.node >= index->node_count) {
            continue;
        }
        if (top_full(result.top) && entry.lower_bound > result.top[KDPRIMARY2_TOP_K - 1u].distance) {
            result.stats.pruned_branches++;
            continue;
        }

        const KdPrimary2Node *node = &index->nodes[entry.node];
        result.stats.nodes_visited++;
        if ((node->flags & KDPRIMARY2_LEAF_FLAG) != 0) {
            scan_leaf(index, node, query, &result);
            continue;
        }

        uint32_t left = node->left;
        uint32_t right = node->right;
        uint64_t worst = result.top[KDPRIMARY2_TOP_K - 1u].distance;
        bool full = top_full(result.top);
        uint64_t limit = full ? worst : UINT64_MAX;
        uint64_t left_bound = left != KDPRIMARY2_INVALID_NODE ? kdprimary2_bbox_distance_limit(&index->nodes[left], query, limit) : UINT64_MAX;
        uint64_t right_bound = right != KDPRIMARY2_INVALID_NODE ? kdprimary2_bbox_distance_limit(&index->nodes[right], query, limit) : UINT64_MAX;

        uint32_t near_node = left_bound <= right_bound ? left : right;
        uint32_t far_node = left_bound <= right_bound ? right : left;
        uint64_t near_bound = left_bound <= right_bound ? left_bound : right_bound;
        uint64_t far_bound = left_bound <= right_bound ? right_bound : left_bound;

        if (!full || far_bound <= worst) {
            if (!push_node(stack, &stack_len, far_node, far_bound, &result)) {
                result.stats.pruned_branches++;
            }
        } else if (far_node != KDPRIMARY2_INVALID_NODE) {
            result.stats.pruned_branches++;
        }
        if (!full || near_bound <= worst) {
            if (!push_node(stack, &stack_len, near_node, near_bound, &result)) {
                result.stats.pruned_branches++;
            }
        } else if (near_node != KDPRIMARY2_INVALID_NODE) {
            result.stats.pruned_branches++;
        }
    }

    result.fraud_count = ivf8_top5_fraud_count(result.top);
    return result;
}

uint8_t kdprimary2_search_fraud_count(const KdPrimary2Index *index,
                                      const int16_t query[IVF8_INDEX_DIMS]) {
    return kdprimary2_search_top5(index, query).fraud_count;
}
