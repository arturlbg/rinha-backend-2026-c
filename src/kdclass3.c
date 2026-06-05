#define _GNU_SOURCE

#include "kdclass3.h"

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

#define KDCLASS3_SAMPLE_TARGET 1024u
#define KDCLASS3_STACK_CAPACITY 4096u

static volatile uint64_t kdclass3_touch_sink = 0;

typedef struct {
    char magic[8];
    uint32_t version;
    uint32_t dims;
    uint32_t leaf_size;
    uint32_t fraud_count;
    uint32_t legit_count;
    uint32_t fraud_node_count;
    uint32_t legit_node_count;
    uint32_t fraud_block_count;
    uint32_t legit_block_count;
    uint32_t fraud_root;
    uint32_t legit_root;
    uint32_t node_size;
    uint32_t reserved0;
    uint64_t fraud_nodes_offset;
    uint64_t fraud_block_data_offset;
    uint64_t legit_nodes_offset;
    uint64_t legit_block_data_offset;
    uint64_t total_size;
    uint8_t reserved[24];
} KdClass3FileHeader;

typedef struct {
    KdClass3TreeBuild *tree;
    uint32_t leaf_size;
    const int16_t *source_vectors;
    uint32_t *refs;
    uint32_t next_node;
    uint32_t next_point;
    uint32_t block_capacity;
} TreeBuildContext;

typedef struct {
    uint32_t node;
    uint64_t lower_bound;
} SearchStackEntry;

_Static_assert(sizeof(KdClass3Node) == 84u, "KdClass3Node must stay compact");
_Static_assert(sizeof(KdClass3FileHeader) == KDCLASS3_HEADER_BYTES, "KdClass3 header size mismatch");

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

KdClass3OpenOptions kdclass3_open_options_default(void) {
    KdClass3OpenOptions options = {
        .populate = false,
        .mlock = false,
        .madvise_mode = KDCLASS3_MADVISE_OFF,
    };
    return options;
}

bool kdclass3_madvise_mode_from_string(const char *value, KdClass3MadviseMode *mode) {
    if (mode == NULL) {
        return false;
    }
    if (value == NULL || value[0] == '\0' || strcmp(value, "off") == 0) {
        *mode = KDCLASS3_MADVISE_OFF;
        return true;
    }
    if (strcmp(value, "willneed") == 0) {
        *mode = KDCLASS3_MADVISE_WILLNEED;
        return true;
    }
    if (strcmp(value, "random") == 0) {
        *mode = KDCLASS3_MADVISE_RANDOM;
        return true;
    }
    if (strcmp(value, "sequential") == 0) {
        *mode = KDCLASS3_MADVISE_SEQUENTIAL;
        return true;
    }
    if (strcmp(value, "hugepage") == 0) {
        *mode = KDCLASS3_MADVISE_HUGEPAGE;
        return true;
    }
    if (strcmp(value, "nohugepage") == 0) {
        *mode = KDCLASS3_MADVISE_NOHUGEPAGE;
        return true;
    }
    return false;
}

const char *kdclass3_madvise_mode_name(KdClass3MadviseMode mode) {
    switch (mode) {
        case KDCLASS3_MADVISE_OFF:
            return "off";
        case KDCLASS3_MADVISE_WILLNEED:
            return "willneed";
        case KDCLASS3_MADVISE_RANDOM:
            return "random";
        case KDCLASS3_MADVISE_SEQUENTIAL:
            return "sequential";
        case KDCLASS3_MADVISE_HUGEPAGE:
            return "hugepage";
        case KDCLASS3_MADVISE_NOHUGEPAGE:
            return "nohugepage";
        default:
            return "off";
    }
}

bool kdclass3_impl_from_string(const char *value, KdClass3Impl *impl) {
    if (impl == NULL) {
        return false;
    }
    if (value == NULL || value[0] == '\0' || strcmp(value, "baseline") == 0) {
        *impl = KDCLASS3_IMPL_BASELINE;
        return true;
    }
    if (strcmp(value, "simd_full") == 0) {
        *impl = KDCLASS3_IMPL_SIMD_FULL;
        return true;
    }
    return false;
}

const char *kdclass3_impl_name(KdClass3Impl impl) {
    return impl == KDCLASS3_IMPL_SIMD_FULL ? "simd_full" : "baseline";
}

static int kdclass3_madvise_value(KdClass3MadviseMode mode) {
    switch (mode) {
        case KDCLASS3_MADVISE_WILLNEED:
            return MADV_WILLNEED;
        case KDCLASS3_MADVISE_RANDOM:
            return MADV_RANDOM;
        case KDCLASS3_MADVISE_SEQUENTIAL:
            return MADV_SEQUENTIAL;
#ifdef MADV_HUGEPAGE
        case KDCLASS3_MADVISE_HUGEPAGE:
            return MADV_HUGEPAGE;
#endif
#ifdef MADV_NOHUGEPAGE
        case KDCLASS3_MADVISE_NOHUGEPAGE:
            return MADV_NOHUGEPAGE;
#endif
        default:
            return -1;
    }
}

static bool add_size(size_t *value, size_t add) {
    if (*value > SIZE_MAX - add) {
        return false;
    }
    *value += add;
    return true;
}

size_t kdclass3_tree_block_data_bytes(uint32_t block_count) {
    return (size_t)block_count * IVF8_INDEX_DIMS * IVF8_INDEX_LANES * sizeof(int16_t);
}

static size_t tree_node_bytes(uint32_t node_count) {
    return (size_t)node_count * sizeof(KdClass3Node);
}

size_t kdclass3_expected_file_bytes(uint32_t fraud_node_count,
                                    uint32_t fraud_block_count,
                                    uint32_t legit_node_count,
                                    uint32_t legit_block_count) {
    size_t total = KDCLASS3_HEADER_BYTES;
    if (!add_size(&total, tree_node_bytes(fraud_node_count)) ||
        !add_size(&total, kdclass3_tree_block_data_bytes(fraud_block_count)) ||
        !add_size(&total, tree_node_bytes(legit_node_count)) ||
        !add_size(&total, kdclass3_tree_block_data_bytes(legit_block_count))) {
        return 0;
    }
    return total;
}

uint64_t kdclass3_distance14(const int16_t a[IVF8_INDEX_DIMS], const int16_t b[IVF8_INDEX_DIMS]) {
    uint64_t sum = 0;
    for (uint32_t dim = 0; dim < IVF8_INDEX_DIMS; dim++) {
        int64_t diff = (int64_t)a[dim] - (int64_t)b[dim];
        sum += (uint64_t)(diff * diff);
    }
    return sum;
}

uint64_t kdclass3_bbox_distance(const KdClass3Node *node, const int16_t query[IVF8_INDEX_DIMS]) {
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

static int16_t ref_value(const TreeBuildContext *ctx, uint32_t ref, uint32_t dim) {
    return ctx->source_vectors[(size_t)ref * IVF8_INDEX_DIMS + dim];
}

static bool ref_less_dim(const TreeBuildContext *ctx, uint32_t a, uint32_t b, uint8_t dim) {
    int16_t av = ref_value(ctx, a, dim);
    int16_t bv = ref_value(ctx, b, dim);
    return av < bv || (av == bv && a < b);
}

static int ref_compare_dim(const TreeBuildContext *ctx, uint32_t a, uint32_t b, uint8_t dim) {
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

static void nth_element_refs(const TreeBuildContext *ctx,
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

static uint8_t choose_split_dim(const TreeBuildContext *ctx, size_t lo, size_t hi) {
    int16_t min_values[IVF8_INDEX_DIMS];
    int16_t max_values[IVF8_INDEX_DIMS];
    for (uint32_t dim = 0; dim < IVF8_INDEX_DIMS; dim++) {
        min_values[dim] = INT16_MAX;
        max_values[dim] = INT16_MIN;
    }

    size_t n = hi - lo;
    size_t stride = n > KDCLASS3_SAMPLE_TARGET ? n / KDCLASS3_SAMPLE_TARGET : 1u;
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

static void bbox_init_empty(KdClass3Node *node) {
    for (uint32_t dim = 0; dim < IVF8_INDEX_DIMS; dim++) {
        node->bbox_min[dim] = INT16_MAX;
        node->bbox_max[dim] = INT16_MIN;
    }
}

static void bbox_add_ref(KdClass3Node *node, const TreeBuildContext *ctx, uint32_t ref) {
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

static void bbox_union_child(KdClass3Node *node, const KdClass3Node *child) {
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

static int ensure_block_capacity(TreeBuildContext *ctx, uint32_t needed) {
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

    int16_t *block_data = (int16_t *)realloc(ctx->tree->block_data, kdclass3_tree_block_data_bytes(next));
    if (block_data == NULL) {
        return -1;
    }
    ctx->tree->block_data = block_data;

    size_t old_bytes = kdclass3_tree_block_data_bytes(ctx->block_capacity);
    size_t new_bytes = kdclass3_tree_block_data_bytes(next);
    if (new_bytes > old_bytes) {
        memset((uint8_t *)block_data + old_bytes, 0, new_bytes - old_bytes);
    }
    ctx->block_capacity = next;
    return 0;
}

static int copy_leaf_points(TreeBuildContext *ctx, const uint32_t *refs, size_t lo, size_t hi, KdClass3Node *node) {
    uint32_t count = (uint32_t)(hi - lo);
    uint32_t blocks = (count + IVF8_INDEX_LANES - 1u) / IVF8_INDEX_LANES;
    uint32_t block_start = ctx->tree->block_count;
    if (ensure_block_capacity(ctx, block_start + blocks) != 0) {
        return -1;
    }

    node->left = KDCLASS3_INVALID_NODE;
    node->right = KDCLASS3_INVALID_NODE;
    node->block_start = block_start;
    node->point_start = ctx->next_point;
    node->count = count;
    node->split_value = 0;
    node->split_dim = 0;
    node->flags = KDCLASS3_LEAF_FLAG;
    node->reserved = 0;
    bbox_init_empty(node);

    for (uint32_t block = 0; block < blocks; block++) {
        uint32_t global_block = block_start + block;
        int16_t *block_data = ctx->tree->block_data +
                              (size_t)global_block * IVF8_INDEX_DIMS * IVF8_INDEX_LANES;
        memset(block_data, 0, IVF8_INDEX_DIMS * IVF8_INDEX_LANES * sizeof(int16_t));
    }

    for (uint32_t i = 0; i < count; i++) {
        uint32_t ref = refs[lo + i];
        uint32_t block = block_start + i / IVF8_INDEX_LANES;
        uint32_t lane = i % IVF8_INDEX_LANES;
        int16_t *block_data = ctx->tree->block_data +
                              (size_t)block * IVF8_INDEX_DIMS * IVF8_INDEX_LANES;
        for (uint32_t dim = 0; dim < IVF8_INDEX_DIMS; dim++) {
            block_data[dim * IVF8_INDEX_LANES + lane] =
                ctx->source_vectors[(size_t)ref * IVF8_INDEX_DIMS + dim];
        }
        bbox_add_ref(node, ctx, ref);
    }

    ctx->next_point += count;
    ctx->tree->block_count += blocks;
    return 0;
}

static uint32_t build_range(TreeBuildContext *ctx, size_t lo, size_t hi) {
    if (lo >= hi) {
        return KDCLASS3_INVALID_NODE;
    }

    uint32_t node_index = ctx->next_node++;
    KdClass3Node *node = &ctx->tree->nodes[node_index];
    memset(node, 0, sizeof(*node));
    bbox_init_empty(node);

    size_t n = hi - lo;
    if (n <= ctx->leaf_size) {
        if (copy_leaf_points(ctx, ctx->refs, lo, hi, node) != 0) {
            return KDCLASS3_INVALID_NODE;
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
    if (node->left != KDCLASS3_INVALID_NODE) {
        bbox_union_child(node, &ctx->tree->nodes[node->left]);
    }
    if (node->right != KDCLASS3_INVALID_NODE) {
        bbox_union_child(node, &ctx->tree->nodes[node->right]);
    }
    return node_index;
}

static int build_tree_from_source(KdClass3TreeBuild *tree,
                                  const int16_t *source_vectors,
                                  uint32_t count,
                                  uint32_t leaf_size,
                                  char *err,
                                  size_t err_len,
                                  const char *name) {
    if (tree == NULL || source_vectors == NULL || count < KDCLASS3_TOP_K || leaf_size == 0) {
        if (err != NULL && err_len > 0) {
            (void)snprintf(err, err_len, "kdclass3: invalid %s tree input", name);
        }
        return -1;
    }
    memset(tree, 0, sizeof(*tree));
    tree->count = count;
    tree->root = KDCLASS3_INVALID_NODE;

    tree->nodes = (KdClass3Node *)calloc(count, sizeof(KdClass3Node));
    uint32_t *refs = (uint32_t *)malloc((size_t)count * sizeof(uint32_t));
    if (tree->nodes == NULL || refs == NULL) {
        free(refs);
        free(tree->nodes);
        memset(tree, 0, sizeof(*tree));
        if (err != NULL && err_len > 0) {
            (void)snprintf(err, err_len, "kdclass3: out of memory building %s tree", name);
        }
        return -1;
    }
    for (uint32_t i = 0; i < count; i++) {
        refs[i] = i;
    }

    TreeBuildContext ctx = {
        .tree = tree,
        .leaf_size = leaf_size,
        .source_vectors = source_vectors,
        .refs = refs,
        .next_node = 0,
        .next_point = 0,
        .block_capacity = 0,
    };
    tree->root = build_range(&ctx, 0, count);
    tree->node_count = ctx.next_node;
    free(refs);

    if (tree->root == KDCLASS3_INVALID_NODE ||
        tree->node_count == 0 ||
        ctx.next_point != count ||
        tree->node_count > count ||
        tree->block_count == 0) {
        free(tree->nodes);
        free(tree->block_data);
        memset(tree, 0, sizeof(*tree));
        if (err != NULL && err_len > 0) {
            (void)snprintf(err, err_len, "kdclass3: inconsistent %s tree", name);
        }
        return -1;
    }
    return 0;
}

static void tree_build_free(KdClass3TreeBuild *tree) {
    if (tree == NULL) {
        return;
    }
    free(tree->nodes);
    free(tree->block_data);
    memset(tree, 0, sizeof(*tree));
    tree->root = KDCLASS3_INVALID_NODE;
}

static int split_points_by_label(const int16_t *vectors,
                                 const uint8_t *labels,
                                 uint32_t count,
                                 int16_t **fraud_vectors,
                                 uint32_t *fraud_count,
                                 int16_t **legit_vectors,
                                 uint32_t *legit_count) {
    uint32_t fraud = 0;
    uint32_t legit = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (labels[i] != 0) {
            fraud++;
        } else {
            legit++;
        }
    }
    if (fraud < KDCLASS3_TOP_K || legit < KDCLASS3_TOP_K) {
        return -1;
    }

    int16_t *fv = (int16_t *)malloc((size_t)fraud * IVF8_INDEX_DIMS * sizeof(int16_t));
    int16_t *lv = (int16_t *)malloc((size_t)legit * IVF8_INDEX_DIMS * sizeof(int16_t));
    if (fv == NULL || lv == NULL) {
        free(fv);
        free(lv);
        return -1;
    }

    uint32_t fp = 0;
    uint32_t lp = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (labels[i] != 0) {
            memcpy(fv + (size_t)fp * IVF8_INDEX_DIMS,
                   vectors + (size_t)i * IVF8_INDEX_DIMS,
                   IVF8_INDEX_DIMS * sizeof(int16_t));
            fp++;
        } else {
            memcpy(lv + (size_t)lp * IVF8_INDEX_DIMS,
                   vectors + (size_t)i * IVF8_INDEX_DIMS,
                   IVF8_INDEX_DIMS * sizeof(int16_t));
            lp++;
        }
    }

    *fraud_vectors = fv;
    *fraud_count = fraud;
    *legit_vectors = lv;
    *legit_count = legit;
    return 0;
}

int kdclass3_build_from_points(KdClass3Build *build,
                               const int16_t *vectors,
                               const uint8_t *labels,
                               uint32_t count,
                               uint32_t leaf_size,
                               char *err,
                               size_t err_len) {
    if (build == NULL || vectors == NULL || labels == NULL || count == 0 || leaf_size == 0) {
        set_error(err, err_len, "kdclass3: invalid build input");
        return -1;
    }
    memset(build, 0, sizeof(*build));
    build->leaf_size = leaf_size;

    int16_t *fraud_vectors = NULL;
    int16_t *legit_vectors = NULL;
    uint32_t fraud_count = 0;
    uint32_t legit_count = 0;
    if (split_points_by_label(vectors, labels, count,
                              &fraud_vectors, &fraud_count,
                              &legit_vectors, &legit_count) != 0) {
        set_error(err, err_len, "kdclass3: failed splitting source labels");
        return -1;
    }

    int ok = build_tree_from_source(&build->fraud, fraud_vectors, fraud_count, leaf_size, err, err_len, "fraud") == 0 &&
             build_tree_from_source(&build->legit, legit_vectors, legit_count, leaf_size, err, err_len, "legit") == 0;
    free(fraud_vectors);
    free(legit_vectors);
    if (!ok) {
        kdclass3_build_free(build);
        return -1;
    }
    return 0;
}

static int count_ivf8_labels(const Ivf8Index *index, uint32_t *fraud_count, uint32_t *legit_count) {
    uint32_t fraud = 0;
    uint32_t legit = 0;
    for (uint32_t cluster = 0; cluster < index->k; cluster++) {
        uint32_t remaining = index->counts[cluster];
        for (uint32_t block = index->offsets[cluster];
             block < index->offsets[cluster + 1u] && remaining > 0;
             block++) {
            uint32_t lanes = remaining < IVF8_INDEX_LANES ? remaining : IVF8_INDEX_LANES;
            for (uint32_t lane = 0; lane < lanes; lane++) {
                uint8_t label = index->labels[(size_t)block * IVF8_INDEX_LANES + lane];
                if (label != 0) {
                    fraud++;
                } else {
                    legit++;
                }
            }
            remaining -= lanes;
        }
    }
    *fraud_count = fraud;
    *legit_count = legit;
    return fraud + legit == index->n ? 0 : -1;
}

static int fill_ivf8_class_points(const Ivf8Index *index,
                                  int16_t *fraud_vectors,
                                  uint32_t fraud_capacity,
                                  int16_t *legit_vectors,
                                  uint32_t legit_capacity) {
    uint32_t fraud = 0;
    uint32_t legit = 0;
    for (uint32_t cluster = 0; cluster < index->k; cluster++) {
        uint32_t remaining = index->counts[cluster];
        for (uint32_t block = index->offsets[cluster];
             block < index->offsets[cluster + 1u] && remaining > 0;
             block++) {
            uint32_t lanes = remaining < IVF8_INDEX_LANES ? remaining : IVF8_INDEX_LANES;
            for (uint32_t lane = 0; lane < lanes; lane++) {
                uint8_t label = index->labels[(size_t)block * IVF8_INDEX_LANES + lane];
                int16_t *dst;
                if (label != 0) {
                    if (fraud >= fraud_capacity) {
                        return -1;
                    }
                    dst = fraud_vectors + (size_t)fraud * IVF8_INDEX_DIMS;
                    fraud++;
                } else {
                    if (legit >= legit_capacity) {
                        return -1;
                    }
                    dst = legit_vectors + (size_t)legit * IVF8_INDEX_DIMS;
                    legit++;
                }
                for (uint32_t dim = 0; dim < IVF8_INDEX_DIMS; dim++) {
                    dst[dim] = index->block_data[(size_t)block * IVF8_INDEX_DIMS * IVF8_INDEX_LANES +
                                                 (size_t)dim * IVF8_INDEX_LANES +
                                                 lane];
                }
            }
            remaining -= lanes;
        }
    }
    return fraud == fraud_capacity && legit == legit_capacity ? 0 : -1;
}

int kdclass3_build_from_ivf8(KdClass3Build *build,
                             const Ivf8Index *index,
                             uint32_t leaf_size,
                             char *err,
                             size_t err_len) {
    if (build == NULL || index == NULL || index->n == 0 || leaf_size == 0) {
        set_error(err, err_len, "kdclass3: invalid IVF8 source");
        return -1;
    }
    memset(build, 0, sizeof(*build));
    build->leaf_size = leaf_size;

    uint32_t fraud_count = 0;
    uint32_t legit_count = 0;
    if (count_ivf8_labels(index, &fraud_count, &legit_count) != 0 ||
        fraud_count < KDCLASS3_TOP_K ||
        legit_count < KDCLASS3_TOP_K) {
        set_error(err, err_len, "kdclass3: invalid IVF8 label counts");
        return -1;
    }

    int16_t *fraud_vectors = (int16_t *)malloc((size_t)fraud_count * IVF8_INDEX_DIMS * sizeof(int16_t));
    int16_t *legit_vectors = (int16_t *)malloc((size_t)legit_count * IVF8_INDEX_DIMS * sizeof(int16_t));
    if (fraud_vectors == NULL || legit_vectors == NULL) {
        free(fraud_vectors);
        free(legit_vectors);
        set_error(err, err_len, "kdclass3: out of memory copying IVF8 source");
        return -1;
    }
    if (fill_ivf8_class_points(index, fraud_vectors, fraud_count, legit_vectors, legit_count) != 0) {
        free(fraud_vectors);
        free(legit_vectors);
        set_error(err, err_len, "kdclass3: failed extracting IVF8 records");
        return -1;
    }

    int ok = build_tree_from_source(&build->fraud, fraud_vectors, fraud_count, leaf_size, err, err_len, "fraud") == 0 &&
             build_tree_from_source(&build->legit, legit_vectors, legit_count, leaf_size, err, err_len, "legit") == 0;
    free(fraud_vectors);
    free(legit_vectors);
    if (!ok) {
        kdclass3_build_free(build);
        return -1;
    }
    return 0;
}

void kdclass3_build_free(KdClass3Build *build) {
    if (build == NULL) {
        return;
    }
    tree_build_free(&build->fraud);
    tree_build_free(&build->legit);
    memset(build, 0, sizeof(*build));
}

size_t kdclass3_build_memory_bytes(const KdClass3Build *build) {
    if (build == NULL) {
        return 0;
    }
    return tree_node_bytes(build->fraud.node_count) +
           kdclass3_tree_block_data_bytes(build->fraud.block_count) +
           tree_node_bytes(build->legit.node_count) +
           kdclass3_tree_block_data_bytes(build->legit.block_count);
}

int kdclass3_save(const KdClass3Build *build, const char *path, char *err, size_t err_len) {
    if (build == NULL ||
        build->fraud.nodes == NULL || build->fraud.block_data == NULL ||
        build->legit.nodes == NULL || build->legit.block_data == NULL ||
        path == NULL) {
        set_error(err, err_len, "kdclass3: invalid save input");
        return -1;
    }

    size_t fraud_node_bytes = tree_node_bytes(build->fraud.node_count);
    size_t fraud_block_bytes = kdclass3_tree_block_data_bytes(build->fraud.block_count);
    size_t legit_node_bytes = tree_node_bytes(build->legit.node_count);
    size_t legit_block_bytes = kdclass3_tree_block_data_bytes(build->legit.block_count);
    size_t total = KDCLASS3_HEADER_BYTES;
    if (!add_size(&total, fraud_node_bytes) ||
        !add_size(&total, fraud_block_bytes) ||
        !add_size(&total, legit_node_bytes) ||
        !add_size(&total, legit_block_bytes)) {
        set_error(err, err_len, "kdclass3: save size overflow");
        return -1;
    }

    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        set_errno_error(err, err_len, "kdclass3: fopen");
        return -1;
    }

    KdClass3FileHeader header;
    memset(&header, 0, sizeof(header));
    memcpy(header.magic, KDCLASS3_MAGIC, KDCLASS3_MAGIC_BYTES);
    header.version = KDCLASS3_VERSION;
    header.dims = IVF8_INDEX_DIMS;
    header.leaf_size = build->leaf_size;
    header.fraud_count = build->fraud.count;
    header.legit_count = build->legit.count;
    header.fraud_node_count = build->fraud.node_count;
    header.legit_node_count = build->legit.node_count;
    header.fraud_block_count = build->fraud.block_count;
    header.legit_block_count = build->legit.block_count;
    header.fraud_root = build->fraud.root;
    header.legit_root = build->legit.root;
    header.node_size = sizeof(KdClass3Node);
    header.fraud_nodes_offset = KDCLASS3_HEADER_BYTES;
    header.fraud_block_data_offset = header.fraud_nodes_offset + fraud_node_bytes;
    header.legit_nodes_offset = header.fraud_block_data_offset + fraud_block_bytes;
    header.legit_block_data_offset = header.legit_nodes_offset + legit_node_bytes;
    header.total_size = total;

    bool ok = fwrite(&header, sizeof(header), 1, file) == 1 &&
              fwrite(build->fraud.nodes, sizeof(KdClass3Node), build->fraud.node_count, file) == build->fraud.node_count &&
              fwrite(build->fraud.block_data,
                     sizeof(int16_t),
                     (size_t)build->fraud.block_count * IVF8_INDEX_DIMS * IVF8_INDEX_LANES,
                     file) == (size_t)build->fraud.block_count * IVF8_INDEX_DIMS * IVF8_INDEX_LANES &&
              fwrite(build->legit.nodes, sizeof(KdClass3Node), build->legit.node_count, file) == build->legit.node_count &&
              fwrite(build->legit.block_data,
                     sizeof(int16_t),
                     (size_t)build->legit.block_count * IVF8_INDEX_DIMS * IVF8_INDEX_LANES,
                     file) == (size_t)build->legit.block_count * IVF8_INDEX_DIMS * IVF8_INDEX_LANES;
    int saved_errno = errno;
    fclose(file);
    if (!ok) {
        errno = saved_errno;
        set_errno_error(err, err_len, "kdclass3: fwrite");
        return -1;
    }
    return 0;
}

static int validate_header(const KdClass3FileHeader *header, size_t file_size, char *err, size_t err_len) {
    if (memcmp(header->magic, KDCLASS3_MAGIC, KDCLASS3_MAGIC_BYTES) != 0) {
        set_error(err, err_len, "kdclass3: invalid magic");
        return -1;
    }
    if (header->version != KDCLASS3_VERSION ||
        header->dims != IVF8_INDEX_DIMS ||
        header->node_size != sizeof(KdClass3Node) ||
        header->leaf_size == 0 ||
        header->fraud_count < KDCLASS3_TOP_K ||
        header->legit_count < KDCLASS3_TOP_K ||
        header->fraud_node_count == 0 ||
        header->legit_node_count == 0 ||
        header->fraud_block_count == 0 ||
        header->legit_block_count == 0 ||
        header->fraud_root >= header->fraud_node_count ||
        header->legit_root >= header->legit_node_count) {
        set_error(err, err_len, "kdclass3: invalid metadata");
        return -1;
    }

    size_t expected = kdclass3_expected_file_bytes(header->fraud_node_count,
                                                  header->fraud_block_count,
                                                  header->legit_node_count,
                                                  header->legit_block_count);
    size_t fraud_node_bytes = tree_node_bytes(header->fraud_node_count);
    size_t fraud_block_bytes = kdclass3_tree_block_data_bytes(header->fraud_block_count);
    size_t legit_node_bytes = tree_node_bytes(header->legit_node_count);
    if (expected == 0 ||
        header->fraud_nodes_offset != KDCLASS3_HEADER_BYTES ||
        header->fraud_block_data_offset != header->fraud_nodes_offset + fraud_node_bytes ||
        header->legit_nodes_offset != header->fraud_block_data_offset + fraud_block_bytes ||
        header->legit_block_data_offset != header->legit_nodes_offset + legit_node_bytes ||
        header->total_size != expected ||
        expected != file_size) {
        set_error(err, err_len, "kdclass3: section layout mismatch");
        return -1;
    }
    return 0;
}

int kdclass3_open_with_options(const char *path,
                               KdClass3Index *out,
                               const KdClass3OpenOptions *options,
                               char *err,
                               size_t err_len) {
    if (path == NULL || out == NULL) {
        set_error(err, err_len, "kdclass3: nil open input");
        return -1;
    }
    memset(out, 0, sizeof(*out));
    out->fd = -1;

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        set_errno_error(err, err_len, "kdclass3: open");
        return -1;
    }

    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < (off_t)KDCLASS3_HEADER_BYTES) {
        int saved = errno;
        close(fd);
        errno = saved == 0 ? EINVAL : saved;
        set_errno_error(err, err_len, "kdclass3: fstat");
        return -1;
    }

    KdClass3OpenOptions normalized = options == NULL ? kdclass3_open_options_default() : *options;
    size_t file_size = (size_t)st.st_size;
    int mmap_flags = MAP_PRIVATE;
    bool populate_applied = false;
    int populate_errno = 0;
    if (normalized.populate) {
#ifdef MAP_POPULATE
        mmap_flags |= MAP_POPULATE;
        populate_applied = true;
#else
        populate_errno = ENOTSUP;
#endif
    }
    void *map = mmap(NULL, file_size, PROT_READ, mmap_flags, fd, 0);
    if (map == MAP_FAILED) {
        set_errno_error(err, err_len, "kdclass3: mmap");
        close(fd);
        return -1;
    }

    const KdClass3FileHeader *header = (const KdClass3FileHeader *)map;
    if (validate_header(header, file_size, err, err_len) != 0) {
        (void)munmap(map, file_size);
        close(fd);
        return -1;
    }

    const uint8_t *base = (const uint8_t *)map;
    out->fd = fd;
    out->file_size = file_size;
    out->map = map;
    out->populate_requested = normalized.populate;
    out->populate_applied = populate_applied;
    out->populate_errno = populate_errno;
    out->madvise_mode = normalized.madvise_mode;
    if (normalized.madvise_mode != KDCLASS3_MADVISE_OFF) {
        int advice = kdclass3_madvise_value(normalized.madvise_mode);
        if (advice < 0) {
            out->madvise_errno = ENOTSUP;
        } else if (madvise(map, file_size, advice) == 0) {
            out->madvise_applied = true;
        } else {
            out->madvise_errno = errno;
        }
    }
    out->mlock_requested = normalized.mlock;
    if (normalized.mlock) {
        if (mlock(map, file_size) == 0) {
            out->mlock_applied = true;
        } else {
            out->mlock_errno = errno;
        }
    }
    out->leaf_size = header->leaf_size;
    out->fraud.count = header->fraud_count;
    out->fraud.node_count = header->fraud_node_count;
    out->fraud.block_count = header->fraud_block_count;
    out->fraud.root = header->fraud_root;
    out->fraud.nodes = (const KdClass3Node *)(const void *)(base + header->fraud_nodes_offset);
    out->fraud.block_data = (const int16_t *)(const void *)(base + header->fraud_block_data_offset);
    out->legit.count = header->legit_count;
    out->legit.node_count = header->legit_node_count;
    out->legit.block_count = header->legit_block_count;
    out->legit.root = header->legit_root;
    out->legit.nodes = (const KdClass3Node *)(const void *)(base + header->legit_nodes_offset);
    out->legit.block_data = (const int16_t *)(const void *)(base + header->legit_block_data_offset);
    return 0;
}

int kdclass3_open(const char *path, KdClass3Index *out, char *err, size_t err_len) {
    KdClass3OpenOptions options = kdclass3_open_options_default();
    return kdclass3_open_with_options(path, out, &options, err, err_len);
}

void kdclass3_close(KdClass3Index *index) {
    if (index == NULL) {
        return;
    }
    if (index->map != NULL && index->file_size > 0) {
        if (index->mlock_applied) {
            (void)munlock(index->map, index->file_size);
        }
        (void)munmap(index->map, index->file_size);
    }
    if (index->fd >= 0) {
        (void)close(index->fd);
    }
    memset(index, 0, sizeof(*index));
    index->fd = -1;
}

size_t kdclass3_runtime_memory_bytes(const KdClass3Index *index) {
    return index != NULL ? index->file_size : 0;
}

uint64_t kdclass3_touch_pages(const KdClass3Index *index) {
    if (index == NULL || index->map == NULL || index->file_size == 0) {
        return 0;
    }
    const uint8_t *bytes = (const uint8_t *)index->map;
    uint64_t sum = 0;
    for (size_t offset = 0; offset < index->file_size; offset += 4096u) {
        sum += bytes[offset];
    }
    sum += bytes[index->file_size - 1u];
    kdclass3_touch_sink ^= sum;
    return sum;
}

static void top3_init(KdClass3Neighbor top[KDCLASS3_TOP_K]) {
    for (uint32_t i = 0; i < KDCLASS3_TOP_K; i++) {
        top[i].distance = UINT64_MAX;
        top[i].seq = UINT32_MAX;
    }
}

static bool top3_accepts(const KdClass3Neighbor top[KDCLASS3_TOP_K], uint64_t distance, uint32_t seq) {
    return distance < top[KDCLASS3_TOP_K - 1u].distance ||
           (distance == top[KDCLASS3_TOP_K - 1u].distance && seq < top[KDCLASS3_TOP_K - 1u].seq);
}

static void top3_insert(KdClass3Neighbor top[KDCLASS3_TOP_K], KdClass3Neighbor candidate) {
    if (!top3_accepts(top, candidate.distance, candidate.seq)) {
        return;
    }
    top[KDCLASS3_TOP_K - 1u] = candidate;
    for (uint32_t i = KDCLASS3_TOP_K - 1u; i > 0; i--) {
        bool better = top[i].distance < top[i - 1u].distance ||
                      (top[i].distance == top[i - 1u].distance && top[i].seq < top[i - 1u].seq);
        if (!better) {
            break;
        }
        KdClass3Neighbor tmp = top[i - 1u];
        top[i - 1u] = top[i];
        top[i] = tmp;
    }
}

static bool top3_full(const KdClass3Neighbor top[KDCLASS3_TOP_K]) {
    return top[KDCLASS3_TOP_K - 1u].seq != UINT32_MAX;
}

static bool scan_leaf_cutoff(const KdClass3TreeIndex *tree,
                             const KdClass3Node *node,
                             const int16_t query[IVF8_INDEX_DIMS],
                             uint64_t cutoff,
                             KdClass3ClassSearchResult *result) {
    result->stats.leaves_visited++;
    uint32_t remaining = node->count;
    uint32_t point_offset = 0;
    uint64_t distances[IVF8_INDEX_LANES];

    for (uint32_t block_offset = 0; remaining > 0; block_offset++) {
        uint32_t block = node->block_start + block_offset;
        ivf8_block_distances_avx2(tree->block_data, block, query, distances);
        uint32_t lanes = remaining < IVF8_INDEX_LANES ? remaining : IVF8_INDEX_LANES;
        for (uint32_t lane = 0; lane < lanes; lane++) {
            uint32_t seq = node->point_start + point_offset + lane;
            uint64_t distance = distances[lane];
            result->stats.points_evaluated++;
            if (cutoff != UINT64_MAX && distance > cutoff) {
                continue;
            }
            if (top3_accepts(result->top, distance, seq)) {
                KdClass3Neighbor candidate = {
                    .distance = distance,
                    .seq = seq,
                };
                top3_insert(result->top, candidate);
                if (cutoff != UINT64_MAX && result->top[KDCLASS3_TOP_K - 1u].distance < cutoff) {
                    return true;
                }
            }
        }
        remaining -= lanes;
        point_offset += lanes;
    }
    return false;
}

static bool push_node(SearchStackEntry stack[KDCLASS3_STACK_CAPACITY],
                      uint32_t *stack_len,
                      uint32_t node,
                      uint64_t lower_bound,
                      KdClass3ClassSearchResult *result) {
    if (node == KDCLASS3_INVALID_NODE) {
        return true;
    }
    if (*stack_len >= KDCLASS3_STACK_CAPACITY) {
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

static KdClass3ClassSearchResult search_class3_cutoff(const KdClass3TreeIndex *tree,
                                                      const int16_t query[IVF8_INDEX_DIMS],
                                                      uint64_t cutoff) {
    KdClass3ClassSearchResult result;
    memset(&result, 0, sizeof(result));
    top3_init(result.top);
    result.distance3 = UINT64_MAX;
    if (tree == NULL || tree->nodes == NULL || tree->block_data == NULL || query == NULL || tree->count < KDCLASS3_TOP_K) {
        return result;
    }

    SearchStackEntry stack[KDCLASS3_STACK_CAPACITY];
    uint32_t stack_len = 0;
    (void)push_node(stack, &stack_len, tree->root, 0, &result);

    while (stack_len > 0) {
        SearchStackEntry entry = stack[--stack_len];
        if (entry.node >= tree->node_count) {
            continue;
        }
        if (cutoff != UINT64_MAX && top3_full(result.top) && result.top[KDCLASS3_TOP_K - 1u].distance < cutoff) {
            break;
        }
        uint64_t prune_bound = cutoff;
        if (top3_full(result.top) && result.top[KDCLASS3_TOP_K - 1u].distance < prune_bound) {
            prune_bound = result.top[KDCLASS3_TOP_K - 1u].distance;
        }
        if (entry.lower_bound > prune_bound) {
            result.stats.pruned_branches++;
            continue;
        }

        const KdClass3Node *node = &tree->nodes[entry.node];
        result.stats.nodes_visited++;
        if ((node->flags & KDCLASS3_LEAF_FLAG) != 0) {
            if (scan_leaf_cutoff(tree, node, query, cutoff, &result)) {
                break;
            }
            continue;
        }

        uint32_t left = node->left;
        uint32_t right = node->right;
        uint64_t left_bound = left != KDCLASS3_INVALID_NODE ? kdclass3_bbox_distance(&tree->nodes[left], query) : UINT64_MAX;
        uint64_t right_bound = right != KDCLASS3_INVALID_NODE ? kdclass3_bbox_distance(&tree->nodes[right], query) : UINT64_MAX;

        uint32_t near_node = left_bound <= right_bound ? left : right;
        uint32_t far_node = left_bound <= right_bound ? right : left;
        uint64_t near_bound = left_bound <= right_bound ? left_bound : right_bound;
        uint64_t far_bound = left_bound <= right_bound ? right_bound : left_bound;

        uint64_t worst = result.top[KDCLASS3_TOP_K - 1u].distance;
        bool full = top3_full(result.top);
        prune_bound = cutoff;
        if (full && worst < prune_bound) {
            prune_bound = worst;
        }
        if ((!full || far_bound <= worst) && far_bound <= prune_bound) {
            if (!push_node(stack, &stack_len, far_node, far_bound, &result)) {
                result.stats.pruned_branches++;
            }
        } else if (far_node != KDCLASS3_INVALID_NODE) {
            result.stats.pruned_branches++;
        }
        if ((!full || near_bound <= worst) && near_bound <= prune_bound) {
            if (!push_node(stack, &stack_len, near_node, near_bound, &result)) {
                result.stats.pruned_branches++;
            }
        } else if (near_node != KDCLASS3_INVALID_NODE) {
            result.stats.pruned_branches++;
        }
    }

    result.distance3 = result.top[KDCLASS3_TOP_K - 1u].distance;
    return result;
}

KdClass3ClassSearchResult kdclass3_search_class3(const KdClass3TreeIndex *tree,
                                                 const int16_t query[IVF8_INDEX_DIMS]) {
    return search_class3_cutoff(tree, query, UINT64_MAX);
}

KdClass3SearchResult kdclass3_search(const KdClass3Index *index,
                                     const int16_t query[IVF8_INDEX_DIMS]) {
    KdClass3SearchResult result;
    memset(&result, 0, sizeof(result));
    result.fallback_required = true;
    result.fraud_count = UINT8_MAX;
    result.fraud_distance3 = UINT64_MAX;
    result.legit_distance3 = UINT64_MAX;
    if (index == NULL || query == NULL) {
        return result;
    }

    KdClass3ClassSearchResult legit = kdclass3_search_class3(&index->legit, query);
    KdClass3ClassSearchResult fraud = search_class3_cutoff(&index->fraud, query, legit.distance3);
    result.fraud_distance3 = fraud.distance3;
    result.legit_distance3 = legit.distance3;
    result.fraud_stats = fraud.stats;
    result.legit_stats = legit.stats;

    if (fraud.distance3 < legit.distance3) {
        result.fraud_count = 3u;
        result.fallback_required = false;
    } else if (legit.distance3 < fraud.distance3) {
        result.fraud_count = 0u;
        result.fallback_required = false;
    }
    return result;
}
