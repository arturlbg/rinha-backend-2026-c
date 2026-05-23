#include "kdtree.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define KDTREE_FILE_MAGIC "RKDTREE1"
#define KDTREE_FILE_VERSION 1u
#define KDTREE_SAMPLE_TARGET 1024u

typedef struct {
    char magic[8];
    uint32_t version;
    uint32_t count;
    uint32_t node_count;
    uint32_t root;
    uint32_t dims;
    uint32_t node_size;
    uint32_t reserved0;
    uint32_t reserved1;
    uint64_t nodes_offset;
    uint64_t total_size;
    uint8_t reserved[8];
} KdTreeFileHeader;

typedef struct {
    KdTree *tree;
    uint32_t *refs;
} BuildContext;

uint64_t kdtree_distance14(const int16_t a[IVF8_INDEX_DIMS], const int16_t b[IVF8_INDEX_DIMS]) {
    uint64_t sum = 0;
    for (uint32_t dim = 0; dim < IVF8_INDEX_DIMS; dim++) {
        int64_t diff = (int64_t)a[dim] - (int64_t)b[dim];
        sum += (uint64_t)(diff * diff);
    }
    return sum;
}

void kdtree_top5_init(Ivf8Neighbor top[KDTREE_TOP_K]) {
    ivf8_top5_init(top);
}

void kdtree_top5_insert(Ivf8Neighbor top[KDTREE_TOP_K], Ivf8Neighbor candidate) {
    ivf8_top5_insert(top, candidate);
}

uint8_t kdtree_top5_fraud_count(const Ivf8Neighbor top[KDTREE_TOP_K]) {
    return ivf8_top5_fraud_count(top);
}

static uint32_t ref_block(uint32_t ref) {
    return ref / IVF8_INDEX_LANES;
}

static uint32_t ref_lane(uint32_t ref) {
    return ref % IVF8_INDEX_LANES;
}

const int16_t *kdtree_vector(const KdTree *tree, uint32_t ref) {
    if (tree == NULL) {
        return NULL;
    }
    if (tree->source_kind == KDTREE_SOURCE_OWNED) {
        if (ref >= tree->count) {
            return NULL;
        }
        return tree->owned_vectors + (size_t)ref * IVF8_INDEX_DIMS;
    }
    if (tree->index == NULL || ref_block(ref) >= tree->index->blocks) {
        return NULL;
    }
    return tree->index->block_data +
           (size_t)ref_block(ref) * IVF8_INDEX_DIMS * IVF8_INDEX_LANES +
           ref_lane(ref);
}

uint8_t kdtree_label(const KdTree *tree, uint32_t ref) {
    if (tree == NULL) {
        return 0;
    }
    if (tree->source_kind == KDTREE_SOURCE_OWNED) {
        return ref < tree->count && tree->owned_labels[ref] != 0 ? 1u : 0u;
    }
    if (tree->index == NULL || ref_block(ref) >= tree->index->blocks) {
        return 0;
    }
    return tree->index->labels[ref] != 0 ? 1u : 0u;
}

static int16_t ref_dim_value(const KdTree *tree, uint32_t ref, uint32_t dim) {
    const int16_t *vector = kdtree_vector(tree, ref);
    if (vector == NULL) {
        return 0;
    }
    if (tree->source_kind == KDTREE_SOURCE_IVF8) {
        return vector[dim * IVF8_INDEX_LANES];
    }
    return vector[dim];
}

uint32_t kdtree_count_ivf8_records(const Ivf8Index *index) {
    if (index == NULL) {
        return 0;
    }
    uint64_t total = 0;
    for (uint32_t cluster = 0; cluster < index->k; cluster++) {
        total += index->counts[cluster];
    }
    return total > UINT32_MAX ? 0 : (uint32_t)total;
}

static int fill_ivf8_refs(const Ivf8Index *index, uint32_t *refs, uint32_t count) {
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
                refs[pos++] = block * IVF8_INDEX_LANES + lane;
            }
            remaining -= lanes;
        }
    }
    return pos == count ? 0 : -1;
}

static bool ref_less_dim(const KdTree *tree, uint32_t a, uint32_t b, uint8_t dim) {
    int16_t av = ref_dim_value(tree, a, dim);
    int16_t bv = ref_dim_value(tree, b, dim);
    return av < bv || (av == bv && a < b);
}

static int ref_compare_dim(const KdTree *tree, uint32_t a, uint32_t b, uint8_t dim) {
    if (ref_less_dim(tree, a, b, dim)) {
        return -1;
    }
    if (ref_less_dim(tree, b, a, dim)) {
        return 1;
    }
    return 0;
}

static void swap_u32(uint32_t *a, uint32_t *b) {
    uint32_t tmp = *a;
    *a = *b;
    *b = tmp;
}

static void nth_element_refs(const KdTree *tree, uint32_t *refs, size_t left, size_t nth, size_t right, uint8_t dim) {
    while (right - left > 1u) {
        uint32_t pivot = refs[left + (right - left) / 2u];
        size_t lt = left;
        size_t i = left;
        size_t gt = right;
        while (i < gt) {
            int cmp = ref_compare_dim(tree, refs[i], pivot, dim);
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

static uint8_t choose_split_dim(const KdTree *tree, const uint32_t *refs, size_t lo, size_t hi) {
    int16_t min_values[IVF8_INDEX_DIMS];
    int16_t max_values[IVF8_INDEX_DIMS];
    for (uint32_t dim = 0; dim < IVF8_INDEX_DIMS; dim++) {
        min_values[dim] = INT16_MAX;
        max_values[dim] = INT16_MIN;
    }

    size_t n = hi - lo;
    size_t stride = n > KDTREE_SAMPLE_TARGET ? n / KDTREE_SAMPLE_TARGET : 1u;
    for (size_t i = lo; i < hi; i += stride) {
        uint32_t ref = refs[i];
        for (uint32_t dim = 0; dim < IVF8_INDEX_DIMS; dim++) {
            int16_t value = ref_dim_value(tree, ref, dim);
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

static uint32_t build_range(BuildContext *ctx, size_t lo, size_t hi) {
    if (lo >= hi) {
        return KDTREE_INVALID_NODE;
    }

    uint8_t dim = choose_split_dim(ctx->tree, ctx->refs, lo, hi);
    size_t mid = lo + (hi - lo) / 2u;
    nth_element_refs(ctx->tree, ctx->refs, lo, mid, hi, dim);

    uint32_t node_index = ctx->tree->node_count++;
    uint32_t ref = ctx->refs[mid];
    KdTreeNode *node = &ctx->tree->nodes[node_index];
    node->ref = ref;
    node->split_dim = dim;
    node->split_value = ref_dim_value(ctx->tree, ref, dim);
    node->label = kdtree_label(ctx->tree, ref);
    node->reserved = 0;
    node->left = build_range(ctx, lo, mid);
    node->right = build_range(ctx, mid + 1u, hi);
    return node_index;
}

static int build_from_refs(KdTree *tree, uint32_t *refs) {
    tree->nodes = (KdTreeNode *)calloc(tree->count, sizeof(KdTreeNode));
    if (tree->nodes == NULL) {
        return -1;
    }
    tree->node_count = 0;
    BuildContext ctx = {
        .tree = tree,
        .refs = refs,
    };
    tree->root = build_range(&ctx, 0, tree->count);
    return tree->node_count == tree->count ? 0 : -1;
}

int kdtree_build_from_points(KdTree *tree,
                             const int16_t *vectors,
                             const uint8_t *labels,
                             uint32_t count) {
    if (tree == NULL || vectors == NULL || labels == NULL || count == 0) {
        errno = EINVAL;
        return -1;
    }
    memset(tree, 0, sizeof(*tree));
    tree->source_kind = KDTREE_SOURCE_OWNED;
    tree->count = count;
    tree->root = KDTREE_INVALID_NODE;
    tree->owned_vectors = (int16_t *)malloc((size_t)count * IVF8_INDEX_DIMS * sizeof(int16_t));
    tree->owned_labels = (uint8_t *)malloc((size_t)count * sizeof(uint8_t));
    uint32_t *refs = (uint32_t *)malloc((size_t)count * sizeof(uint32_t));
    if (tree->owned_vectors == NULL || tree->owned_labels == NULL || refs == NULL) {
        free(refs);
        kdtree_free(tree);
        return -1;
    }
    memcpy(tree->owned_vectors, vectors, (size_t)count * IVF8_INDEX_DIMS * sizeof(int16_t));
    memcpy(tree->owned_labels, labels, (size_t)count * sizeof(uint8_t));
    for (uint32_t i = 0; i < count; i++) {
        refs[i] = i;
    }
    int result = build_from_refs(tree, refs);
    free(refs);
    if (result != 0) {
        kdtree_free(tree);
    }
    return result;
}

int kdtree_build_from_ivf8(KdTree *tree, const Ivf8Index *index) {
    if (tree == NULL || index == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(tree, 0, sizeof(*tree));
    tree->source_kind = KDTREE_SOURCE_IVF8;
    tree->index = index;
    tree->count = kdtree_count_ivf8_records(index);
    tree->root = KDTREE_INVALID_NODE;
    if (tree->count == 0) {
        errno = EINVAL;
        return -1;
    }

    uint32_t *refs = (uint32_t *)malloc((size_t)tree->count * sizeof(uint32_t));
    if (refs == NULL) {
        return -1;
    }
    if (fill_ivf8_refs(index, refs, tree->count) != 0) {
        free(refs);
        errno = EINVAL;
        return -1;
    }
    int result = build_from_refs(tree, refs);
    free(refs);
    if (result != 0) {
        kdtree_free(tree);
    }
    return result;
}

void kdtree_free(KdTree *tree) {
    if (tree == NULL) {
        return;
    }
    if (tree->node_map != NULL && tree->node_map_size != 0) {
        (void)munmap(tree->node_map, tree->node_map_size);
    } else {
        free(tree->nodes);
    }
    free(tree->owned_vectors);
    free(tree->owned_labels);
    memset(tree, 0, sizeof(*tree));
    tree->root = KDTREE_INVALID_NODE;
}

size_t kdtree_runtime_memory_bytes(const KdTree *tree) {
    if (tree == NULL) {
        return 0;
    }
    size_t bytes = (size_t)tree->node_count * sizeof(KdTreeNode);
    if (tree->source_kind == KDTREE_SOURCE_OWNED) {
        bytes += (size_t)tree->count * IVF8_INDEX_DIMS * sizeof(int16_t);
        bytes += (size_t)tree->count * sizeof(uint8_t);
    }
    return bytes;
}

size_t kdtree_build_memory_bytes(const KdTree *tree) {
    if (tree == NULL) {
        return 0;
    }
    return kdtree_runtime_memory_bytes(tree) + (size_t)tree->count * sizeof(uint32_t);
}

static bool top_accepts(const Ivf8Neighbor top[KDTREE_TOP_K], uint64_t distance, uint32_t seq) {
    return distance < top[KDTREE_TOP_K - 1u].distance ||
           (distance == top[KDTREE_TOP_K - 1u].distance && seq < top[KDTREE_TOP_K - 1u].seq);
}

static void search_node(const KdTree *tree,
                        uint32_t node_index,
                        const int16_t query[IVF8_INDEX_DIMS],
                        const KdTreeSearchConfig *cfg,
                        KdTreeSearchResult *result,
                        uint32_t depth) {
    if (node_index == KDTREE_INVALID_NODE) {
        return;
    }
    if (cfg != NULL && cfg->max_visited != 0 && result->stats.nodes_visited >= cfg->max_visited) {
        return;
    }
    if (depth > result->stats.max_depth) {
        result->stats.max_depth = depth;
    }

    const KdTreeNode *node = &tree->nodes[node_index];
    const int16_t *vector = kdtree_vector(tree, node->ref);
    if (vector == NULL) {
        return;
    }

    result->stats.nodes_visited++;
    result->stats.distance_evaluations++;
    uint64_t distance = 0;
    if (tree->source_kind == KDTREE_SOURCE_IVF8) {
        int16_t tmp[IVF8_INDEX_DIMS];
        for (uint32_t dim = 0; dim < IVF8_INDEX_DIMS; dim++) {
            tmp[dim] = vector[dim * IVF8_INDEX_LANES];
        }
        distance = kdtree_distance14(query, tmp);
    } else {
        distance = kdtree_distance14(query, vector);
    }
    if (top_accepts(result->top, distance, node->ref)) {
        Ivf8Neighbor candidate = {
            .distance = distance,
            .fraud = node->label != 0 ? 1u : 0u,
            .seq = node->ref,
        };
        kdtree_top5_insert(result->top, candidate);
    }

    int32_t diff = (int32_t)query[node->split_dim] - (int32_t)node->split_value;
    uint32_t near_child = diff <= 0 ? node->left : node->right;
    uint32_t far_child = diff <= 0 ? node->right : node->left;
    search_node(tree, near_child, query, cfg, result, depth + 1u);

    uint64_t worst = result->top[KDTREE_TOP_K - 1u].distance;
    int64_t plane_diff = (int64_t)diff;
    uint64_t plane_distance = (uint64_t)(plane_diff * plane_diff);
    if (result->top[KDTREE_TOP_K - 1u].seq == UINT32_MAX || plane_distance <= worst) {
        search_node(tree, far_child, query, cfg, result, depth + 1u);
    } else {
        result->stats.pruned_branches++;
    }
}

KdTreeSearchResult kdtree_search_top5(const KdTree *tree,
                                      const int16_t query[IVF8_INDEX_DIMS],
                                      const KdTreeSearchConfig *cfg) {
    KdTreeSearchResult result;
    memset(&result, 0, sizeof(result));
    kdtree_top5_init(result.top);
    if (tree == NULL || tree->nodes == NULL || query == NULL || tree->root == KDTREE_INVALID_NODE) {
        return result;
    }
    search_node(tree, tree->root, query, cfg, &result, 1u);
    result.fraud_count = kdtree_top5_fraud_count(result.top);
    return result;
}

uint8_t kdtree_search_fraud_count(const KdTree *tree,
                                  const int16_t query[IVF8_INDEX_DIMS],
                                  const KdTreeSearchConfig *cfg) {
    return kdtree_search_top5(tree, query, cfg).fraud_count;
}

int kdtree_save_nodes(const KdTree *tree, const char *path) {
    if (tree == NULL || tree->nodes == NULL || path == NULL) {
        errno = EINVAL;
        return -1;
    }
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        return -1;
    }
    KdTreeFileHeader header;
    memset(&header, 0, sizeof(header));
    memcpy(header.magic, KDTREE_FILE_MAGIC, sizeof(header.magic));
    header.version = KDTREE_FILE_VERSION;
    header.count = tree->count;
    header.node_count = tree->node_count;
    header.root = tree->root;
    header.dims = IVF8_INDEX_DIMS;
    header.node_size = sizeof(KdTreeNode);
    header.nodes_offset = sizeof(header);
    header.total_size = sizeof(header) + (uint64_t)tree->node_count * sizeof(KdTreeNode);
    bool ok = fwrite(&header, sizeof(header), 1, file) == 1 &&
              fwrite(tree->nodes, sizeof(KdTreeNode), tree->node_count, file) == tree->node_count;
    int saved_errno = errno;
    fclose(file);
    if (!ok) {
        errno = saved_errno;
        return -1;
    }
    return 0;
}

int kdtree_load_nodes_for_ivf8(KdTree *tree, const Ivf8Index *index, const char *path) {
    if (tree == NULL || index == NULL || path == NULL) {
        errno = EINVAL;
        return -1;
    }
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return -1;
    }
    KdTreeFileHeader header;
    if (fread(&header, sizeof(header), 1, file) != 1) {
        fclose(file);
        errno = EINVAL;
        return -1;
    }
    if (memcmp(header.magic, KDTREE_FILE_MAGIC, sizeof(header.magic)) != 0 ||
        header.version != KDTREE_FILE_VERSION ||
        header.dims != IVF8_INDEX_DIMS ||
        header.node_size != sizeof(KdTreeNode) ||
        header.count != index->n ||
        header.node_count != index->n ||
        header.nodes_offset != sizeof(header)) {
        fclose(file);
        errno = EINVAL;
        return -1;
    }
    KdTreeNode *nodes = (KdTreeNode *)malloc((size_t)header.node_count * sizeof(KdTreeNode));
    if (nodes == NULL) {
        fclose(file);
        return -1;
    }
    if (fread(nodes, sizeof(KdTreeNode), header.node_count, file) != header.node_count) {
        free(nodes);
        fclose(file);
        errno = EINVAL;
        return -1;
    }
    fclose(file);

    memset(tree, 0, sizeof(*tree));
    tree->source_kind = KDTREE_SOURCE_IVF8;
    tree->index = index;
    tree->count = header.count;
    tree->root = header.root;
    tree->node_count = header.node_count;
    tree->nodes = nodes;
    return 0;
}

int kdtree_mmap_nodes_for_ivf8(KdTree *tree, const Ivf8Index *index, const char *path) {
    if (tree == NULL || index == NULL || path == NULL) {
        errno = EINVAL;
        return -1;
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }

    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        int saved = errno;
        close(fd);
        errno = saved == 0 ? EINVAL : saved;
        return -1;
    }
    size_t file_size = (size_t)st.st_size;
    void *map = mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
    int saved_errno = errno;
    close(fd);
    if (map == MAP_FAILED) {
        errno = saved_errno;
        return -1;
    }

    if (file_size < sizeof(KdTreeFileHeader)) {
        (void)munmap(map, file_size);
        errno = EINVAL;
        return -1;
    }
    const KdTreeFileHeader *header = (const KdTreeFileHeader *)map;
    uint64_t expected_size = sizeof(*header) + (uint64_t)header->node_count * sizeof(KdTreeNode);
    if (memcmp(header->magic, KDTREE_FILE_MAGIC, sizeof(header->magic)) != 0 ||
        header->version != KDTREE_FILE_VERSION ||
        header->dims != IVF8_INDEX_DIMS ||
        header->node_size != sizeof(KdTreeNode) ||
        header->count != index->n ||
        header->node_count != index->n ||
        header->nodes_offset != sizeof(*header) ||
        header->total_size != expected_size ||
        expected_size != (uint64_t)file_size) {
        (void)munmap(map, file_size);
        errno = EINVAL;
        return -1;
    }

    memset(tree, 0, sizeof(*tree));
    tree->source_kind = KDTREE_SOURCE_IVF8;
    tree->index = index;
    tree->count = header->count;
    tree->root = header->root;
    tree->node_count = header->node_count;
    tree->nodes = (KdTreeNode *)((uint8_t *)map + header->nodes_offset);
    tree->node_map = map;
    tree->node_map_size = file_size;
    return 0;
}
