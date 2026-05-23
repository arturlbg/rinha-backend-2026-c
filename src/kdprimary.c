#define _POSIX_C_SOURCE 200809L

#include "kdprimary.h"

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

#define KDPRIMARY_SAMPLE_TARGET 1024u

static volatile uint64_t kdprimary_touch_sink = 0;

typedef struct {
    char magic[8];
    uint32_t version;
    uint32_t n;
    uint32_t dims;
    uint32_t leaf_size;
    uint32_t node_count;
    uint32_t root;
    uint32_t node_size;
    uint32_t vector_bytes;
    uint32_t label_bytes;
    uint32_t reserved0;
    uint64_t nodes_offset;
    uint64_t vectors_offset;
    uint64_t labels_offset;
    uint64_t total_size;
    uint8_t reserved[48];
} KdPrimaryFileHeader;

typedef struct {
    KdPrimaryBuild *build;
    const int16_t *source_vectors;
    const uint8_t *source_labels;
    uint32_t *refs;
    uint32_t next_node;
    uint32_t next_point;
} BuildContext;

_Static_assert(sizeof(KdPrimaryNode) == 20u, "KdPrimaryNode must stay compact");
_Static_assert(sizeof(KdPrimaryFileHeader) == KDPRIMARY_HEADER_BYTES, "KdPrimary header size mismatch");

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

static bool mul_size(size_t a, size_t b, size_t *out) {
    if (a != 0 && b > SIZE_MAX / a) {
        return false;
    }
    *out = a * b;
    return true;
}

static bool add_size(size_t *value, size_t add) {
    if (*value > SIZE_MAX - add) {
        return false;
    }
    *value += add;
    return true;
}

static size_t vectors_bytes(uint32_t count) {
    return (size_t)count * IVF8_INDEX_DIMS * sizeof(int16_t);
}

size_t kdprimary_expected_file_bytes(uint32_t count, uint32_t node_count) {
    size_t total = KDPRIMARY_HEADER_BYTES;
    if (!add_size(&total, (size_t)node_count * sizeof(KdPrimaryNode)) ||
        !add_size(&total, vectors_bytes(count)) ||
        !add_size(&total, (size_t)count * sizeof(uint8_t))) {
        return 0;
    }
    return total;
}

uint64_t kdprimary_distance14(const int16_t a[IVF8_INDEX_DIMS], const int16_t b[IVF8_INDEX_DIMS]) {
    uint64_t sum = 0;
    for (uint32_t dim = 0; dim < IVF8_INDEX_DIMS; dim++) {
        int64_t diff = (int64_t)a[dim] - (int64_t)b[dim];
        sum += (uint64_t)(diff * diff);
    }
    return sum;
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
    size_t stride = n > KDPRIMARY_SAMPLE_TARGET ? n / KDPRIMARY_SAMPLE_TARGET : 1u;
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

static void copy_leaf_points(BuildContext *ctx, const uint32_t *refs, size_t lo, size_t hi, KdPrimaryNode *node) {
    node->start = ctx->next_point;
    node->count = (uint32_t)(hi - lo);
    node->flags = KDPRIMARY_LEAF_FLAG;
    node->split_dim = 0;
    node->split_value = 0;
    node->left = KDPRIMARY_INVALID_NODE;
    node->right = KDPRIMARY_INVALID_NODE;

    for (size_t i = lo; i < hi; i++) {
        uint32_t ref = refs[i];
        uint32_t dst = ctx->next_point++;
        memcpy(ctx->build->vectors + (size_t)dst * IVF8_INDEX_DIMS,
               ctx->source_vectors + (size_t)ref * IVF8_INDEX_DIMS,
               IVF8_INDEX_DIMS * sizeof(int16_t));
        ctx->build->labels[dst] = ctx->source_labels[ref] != 0 ? 1u : 0u;
    }
}

static uint32_t copy_one_point(BuildContext *ctx, uint32_t ref) {
    uint32_t dst = ctx->next_point++;
    memcpy(ctx->build->vectors + (size_t)dst * IVF8_INDEX_DIMS,
           ctx->source_vectors + (size_t)ref * IVF8_INDEX_DIMS,
           IVF8_INDEX_DIMS * sizeof(int16_t));
    ctx->build->labels[dst] = ctx->source_labels[ref] != 0 ? 1u : 0u;
    return dst;
}

static uint32_t build_range(BuildContext *ctx, size_t lo, size_t hi) {
    if (lo >= hi) {
        return KDPRIMARY_INVALID_NODE;
    }

    uint32_t node_index = ctx->next_node++;
    KdPrimaryNode *node = &ctx->build->nodes[node_index];
    memset(node, 0, sizeof(*node));

    size_t n = hi - lo;
    if (ctx->build->leaf_size == 1u) {
        size_t mid = lo + n / 2u;
        uint8_t dim = n > 1u ? choose_split_dim(ctx, lo, hi) : 0u;
        nth_element_refs(ctx, ctx->refs, lo, mid, hi, dim);
        uint32_t ref = ctx->refs[mid];
        node->flags = KDPRIMARY_POINT_FLAG;
        node->split_dim = dim;
        node->split_value = ref_value(ctx, ref, dim);
        node->start = copy_one_point(ctx, ref);
        node->count = 1;
        node->left = build_range(ctx, lo, mid);
        node->right = build_range(ctx, mid + 1u, hi);
        return node_index;
    }

    if (n <= ctx->build->leaf_size) {
        copy_leaf_points(ctx, ctx->refs, lo, hi, node);
        return node_index;
    }

    uint8_t dim = choose_split_dim(ctx, lo, hi);
    size_t mid = lo + n / 2u;
    nth_element_refs(ctx, ctx->refs, lo, mid, hi, dim);

    node->flags = 0;
    node->split_dim = dim;
    node->split_value = ref_value(ctx, ctx->refs[mid], dim);
    node->start = 0;
    node->count = 0;
    node->left = build_range(ctx, lo, mid);
    node->right = build_range(ctx, mid, hi);
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

static int build_from_source(KdPrimaryBuild *build,
                             const int16_t *source_vectors,
                             const uint8_t *source_labels,
                             uint32_t count,
                             uint32_t leaf_size,
                             char *err,
                             size_t err_len) {
    if (build == NULL || source_vectors == NULL || source_labels == NULL || count == 0 || leaf_size == 0) {
        set_error(err, err_len, "kdprimary: invalid build input");
        return -1;
    }
    memset(build, 0, sizeof(*build));
    build->count = count;
    build->leaf_size = leaf_size;
    build->root = KDPRIMARY_INVALID_NODE;

    size_t node_bytes = 0;
    if (!mul_size((size_t)count, sizeof(KdPrimaryNode), &node_bytes)) {
        set_error(err, err_len, "kdprimary: node allocation overflow");
        return -1;
    }
    build->nodes = (KdPrimaryNode *)calloc(count, sizeof(KdPrimaryNode));
    build->vectors = (int16_t *)malloc(vectors_bytes(count));
    build->labels = (uint8_t *)malloc((size_t)count * sizeof(uint8_t));
    uint32_t *refs = (uint32_t *)malloc((size_t)count * sizeof(uint32_t));
    if (build->nodes == NULL || build->vectors == NULL || build->labels == NULL || refs == NULL) {
        free(refs);
        kdprimary_build_free(build);
        set_error(err, err_len, "kdprimary: out of memory during build");
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
    };
    build->root = build_range(&ctx, 0, count);
    build->node_count = ctx.next_node;
    free(refs);

    if (build->root == KDPRIMARY_INVALID_NODE ||
        build->node_count == 0 ||
        ctx.next_point != count ||
        build->node_count > count) {
        kdprimary_build_free(build);
        set_error(err, err_len, "kdprimary: inconsistent build result");
        return -1;
    }
    return 0;
}

int kdprimary_build_from_points(KdPrimaryBuild *build,
                                const int16_t *vectors,
                                const uint8_t *labels,
                                uint32_t count,
                                uint32_t leaf_size,
                                char *err,
                                size_t err_len) {
    return build_from_source(build, vectors, labels, count, leaf_size, err, err_len);
}

int kdprimary_build_from_ivf8(KdPrimaryBuild *build,
                              const Ivf8Index *index,
                              uint32_t leaf_size,
                              char *err,
                              size_t err_len) {
    if (build == NULL || index == NULL || index->n == 0) {
        set_error(err, err_len, "kdprimary: invalid IVF8 source");
        return -1;
    }

    int16_t *source_vectors = (int16_t *)malloc(vectors_bytes(index->n));
    uint8_t *source_labels = (uint8_t *)malloc((size_t)index->n * sizeof(uint8_t));
    if (source_vectors == NULL || source_labels == NULL) {
        free(source_vectors);
        free(source_labels);
        set_error(err, err_len, "kdprimary: out of memory copying IVF8 source");
        return -1;
    }
    if (fill_ivf8_points(index, source_vectors, source_labels, index->n) != 0) {
        free(source_vectors);
        free(source_labels);
        set_error(err, err_len, "kdprimary: failed to extract IVF8 records");
        return -1;
    }

    int result = build_from_source(build, source_vectors, source_labels, index->n, leaf_size, err, err_len);
    free(source_vectors);
    free(source_labels);
    return result;
}

void kdprimary_build_free(KdPrimaryBuild *build) {
    if (build == NULL) {
        return;
    }
    free(build->nodes);
    free(build->vectors);
    free(build->labels);
    memset(build, 0, sizeof(*build));
    build->root = KDPRIMARY_INVALID_NODE;
}

size_t kdprimary_build_memory_bytes(const KdPrimaryBuild *build) {
    if (build == NULL) {
        return 0;
    }
    return (size_t)build->node_count * sizeof(KdPrimaryNode) +
           vectors_bytes(build->count) +
           (size_t)build->count * sizeof(uint8_t);
}

int kdprimary_save(const KdPrimaryBuild *build, const char *path, char *err, size_t err_len) {
    if (build == NULL || build->nodes == NULL || build->vectors == NULL || build->labels == NULL || path == NULL) {
        set_error(err, err_len, "kdprimary: invalid save input");
        return -1;
    }

    size_t node_bytes = (size_t)build->node_count * sizeof(KdPrimaryNode);
    size_t vector_bytes = vectors_bytes(build->count);
    size_t label_bytes = (size_t)build->count * sizeof(uint8_t);
    size_t total = KDPRIMARY_HEADER_BYTES;
    if (!add_size(&total, node_bytes) || !add_size(&total, vector_bytes) || !add_size(&total, label_bytes)) {
        set_error(err, err_len, "kdprimary: save size overflow");
        return -1;
    }
    if (vector_bytes > UINT32_MAX || label_bytes > UINT32_MAX) {
        set_error(err, err_len, "kdprimary: section too large for v1 header");
        return -1;
    }

    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        set_errno_error(err, err_len, "kdprimary: fopen");
        return -1;
    }

    KdPrimaryFileHeader header;
    memset(&header, 0, sizeof(header));
    memcpy(header.magic, KDPRIMARY_MAGIC, KDPRIMARY_MAGIC_BYTES);
    header.version = KDPRIMARY_VERSION;
    header.n = build->count;
    header.dims = IVF8_INDEX_DIMS;
    header.leaf_size = build->leaf_size;
    header.node_count = build->node_count;
    header.root = build->root;
    header.node_size = sizeof(KdPrimaryNode);
    header.vector_bytes = (uint32_t)vector_bytes;
    header.label_bytes = (uint32_t)label_bytes;
    header.nodes_offset = KDPRIMARY_HEADER_BYTES;
    header.vectors_offset = header.nodes_offset + node_bytes;
    header.labels_offset = header.vectors_offset + vector_bytes;
    header.total_size = total;

    bool ok = fwrite(&header, sizeof(header), 1, file) == 1 &&
              fwrite(build->nodes, sizeof(KdPrimaryNode), build->node_count, file) == build->node_count &&
              fwrite(build->vectors, sizeof(int16_t), (size_t)build->count * IVF8_INDEX_DIMS, file) ==
                  (size_t)build->count * IVF8_INDEX_DIMS &&
              fwrite(build->labels, sizeof(uint8_t), build->count, file) == build->count;
    int saved_errno = errno;
    fclose(file);
    if (!ok) {
        errno = saved_errno;
        set_errno_error(err, err_len, "kdprimary: fwrite");
        return -1;
    }
    return 0;
}

static int validate_header(const KdPrimaryFileHeader *header, size_t file_size, char *err, size_t err_len) {
    if (memcmp(header->magic, KDPRIMARY_MAGIC, KDPRIMARY_MAGIC_BYTES) != 0) {
        set_error(err, err_len, "kdprimary: invalid magic");
        return -1;
    }
    if (header->version != KDPRIMARY_VERSION ||
        header->dims != IVF8_INDEX_DIMS ||
        header->node_size != sizeof(KdPrimaryNode) ||
        header->n == 0 ||
        header->node_count == 0 ||
        header->leaf_size == 0 ||
        header->root >= header->node_count) {
        set_error(err, err_len, "kdprimary: invalid metadata");
        return -1;
    }

    size_t node_bytes = (size_t)header->node_count * sizeof(KdPrimaryNode);
    size_t vector_bytes = vectors_bytes(header->n);
    size_t label_bytes = (size_t)header->n * sizeof(uint8_t);
    size_t expected = KDPRIMARY_HEADER_BYTES;
    if (!add_size(&expected, node_bytes) || !add_size(&expected, vector_bytes) || !add_size(&expected, label_bytes)) {
        set_error(err, err_len, "kdprimary: file size overflow");
        return -1;
    }
    if (header->vector_bytes != vector_bytes ||
        header->label_bytes != label_bytes ||
        header->nodes_offset != KDPRIMARY_HEADER_BYTES ||
        header->vectors_offset != header->nodes_offset + node_bytes ||
        header->labels_offset != header->vectors_offset + vector_bytes ||
        header->total_size != expected ||
        expected != file_size) {
        set_error(err, err_len, "kdprimary: section layout mismatch");
        return -1;
    }
    return 0;
}

int kdprimary_open(const char *path, KdPrimaryIndex *out, char *err, size_t err_len) {
    if (path == NULL || out == NULL) {
        set_error(err, err_len, "kdprimary: nil open input");
        return -1;
    }
    memset(out, 0, sizeof(*out));
    out->fd = -1;

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        set_errno_error(err, err_len, "kdprimary: open");
        return -1;
    }

    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < (off_t)KDPRIMARY_HEADER_BYTES) {
        int saved = errno;
        close(fd);
        errno = saved == 0 ? EINVAL : saved;
        set_errno_error(err, err_len, "kdprimary: fstat");
        return -1;
    }

    size_t file_size = (size_t)st.st_size;
    void *map = mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) {
        set_errno_error(err, err_len, "kdprimary: mmap");
        close(fd);
        return -1;
    }

    const KdPrimaryFileHeader *header = (const KdPrimaryFileHeader *)map;
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
    out->root = header->root;
    out->leaf_size = header->leaf_size;
    out->nodes = (const KdPrimaryNode *)(const void *)(base + header->nodes_offset);
    out->vectors = (const int16_t *)(const void *)(base + header->vectors_offset);
    out->labels = base + header->labels_offset;
    return 0;
}

void kdprimary_close(KdPrimaryIndex *index) {
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

size_t kdprimary_runtime_memory_bytes(const KdPrimaryIndex *index) {
    if (index == NULL) {
        return 0;
    }
    return index->file_size;
}

uint64_t kdprimary_touch_pages(const KdPrimaryIndex *index) {
    if (index == NULL || index->map == NULL || index->file_size == 0) {
        return 0;
    }
    const uint8_t *bytes = (const uint8_t *)index->map;
    uint64_t sum = 0;
    for (size_t offset = 0; offset < index->file_size; offset += 4096u) {
        sum += bytes[offset];
    }
    sum += bytes[index->file_size - 1u];
    kdprimary_touch_sink ^= sum;
    return sum;
}

static bool top_accepts(const Ivf8Neighbor top[KDPRIMARY_TOP_K], uint64_t distance, uint32_t seq) {
    return distance < top[KDPRIMARY_TOP_K - 1u].distance ||
           (distance == top[KDPRIMARY_TOP_K - 1u].distance && seq < top[KDPRIMARY_TOP_K - 1u].seq);
}

static void scan_leaf(const KdPrimaryIndex *index,
                      const KdPrimaryNode *node,
                      const int16_t query[IVF8_INDEX_DIMS],
                      KdPrimarySearchResult *result) {
    result->stats.leaves_visited++;
    for (uint32_t i = 0; i < node->count; i++) {
        uint32_t point = node->start + i;
        const int16_t *vector = index->vectors + (size_t)point * IVF8_INDEX_DIMS;
        uint64_t distance = kdprimary_distance14(query, vector);
        result->stats.points_evaluated++;
        if (top_accepts(result->top, distance, point)) {
            Ivf8Neighbor candidate = {
                .distance = distance,
                .fraud = index->labels[point] != 0 ? 1u : 0u,
                .seq = point,
            };
            ivf8_top5_insert(result->top, candidate);
        }
    }
}

static void scan_point_node(const KdPrimaryIndex *index,
                            const KdPrimaryNode *node,
                            const int16_t query[IVF8_INDEX_DIMS],
                            KdPrimarySearchResult *result) {
    if (node->start >= index->count) {
        return;
    }
    const int16_t *vector = index->vectors + (size_t)node->start * IVF8_INDEX_DIMS;
    uint64_t distance = kdprimary_distance14(query, vector);
    result->stats.points_evaluated++;
    if (top_accepts(result->top, distance, node->start)) {
        Ivf8Neighbor candidate = {
            .distance = distance,
            .fraud = index->labels[node->start] != 0 ? 1u : 0u,
            .seq = node->start,
        };
        ivf8_top5_insert(result->top, candidate);
    }
}

static void search_node(const KdPrimaryIndex *index,
                        uint32_t node_index,
                        const int16_t query[IVF8_INDEX_DIMS],
                        KdPrimarySearchResult *result,
                        uint32_t depth) {
    if (node_index == KDPRIMARY_INVALID_NODE || node_index >= index->node_count) {
        return;
    }
    if (depth > result->stats.max_depth) {
        result->stats.max_depth = depth;
    }
    result->stats.nodes_visited++;

    const KdPrimaryNode *node = &index->nodes[node_index];
    if ((node->flags & KDPRIMARY_LEAF_FLAG) != 0) {
        scan_leaf(index, node, query, result);
        return;
    }
    if ((node->flags & KDPRIMARY_POINT_FLAG) != 0) {
        scan_point_node(index, node, query, result);
        if (node->left == KDPRIMARY_INVALID_NODE && node->right == KDPRIMARY_INVALID_NODE) {
            return;
        }
    }

    int32_t diff = (int32_t)query[node->split_dim] - (int32_t)node->split_value;
    uint32_t near_child = diff <= 0 ? node->left : node->right;
    uint32_t far_child = diff <= 0 ? node->right : node->left;

    search_node(index, near_child, query, result, depth + 1u);

    uint64_t worst = result->top[KDPRIMARY_TOP_K - 1u].distance;
    int64_t plane_diff = (int64_t)diff;
    uint64_t plane_distance = (uint64_t)(plane_diff * plane_diff);
    if (result->top[KDPRIMARY_TOP_K - 1u].seq == UINT32_MAX || plane_distance <= worst) {
        search_node(index, far_child, query, result, depth + 1u);
    } else {
        result->stats.pruned_branches++;
    }
}

KdPrimarySearchResult kdprimary_search_top5(const KdPrimaryIndex *index,
                                            const int16_t query[IVF8_INDEX_DIMS]) {
    KdPrimarySearchResult result;
    memset(&result, 0, sizeof(result));
    ivf8_top5_init(result.top);
    if (index == NULL || index->nodes == NULL || index->vectors == NULL || index->labels == NULL || query == NULL) {
        return result;
    }
    search_node(index, index->root, query, &result, 1u);
    result.fraud_count = ivf8_top5_fraud_count(result.top);
    return result;
}

uint8_t kdprimary_search_fraud_count(const KdPrimaryIndex *index,
                                     const int16_t query[IVF8_INDEX_DIMS]) {
    return kdprimary_search_top5(index, query).fraud_count;
}
