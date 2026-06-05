#include "kdclass3_opt.h"

#include <immintrin.h>
#include <stddef.h>

static uint32_t alive_mask(__m256i acc_lo, __m256i acc_hi, __m256i limit_vec) {
    int lo_gt = _mm256_movemask_pd(
        _mm256_castsi256_pd(_mm256_cmpgt_epi64(acc_lo, limit_vec)));
    int hi_gt = _mm256_movemask_pd(
        _mm256_castsi256_pd(_mm256_cmpgt_epi64(acc_hi, limit_vec)));
    return ((uint32_t)(~lo_gt) & 0x0fu) |
           (((uint32_t)(~hi_gt) & 0x0fu) << 4u);
}

static void bbox_accumulate(__m128i q16,
                            __m128i min16,
                            __m128i max16,
                            __m256i lane_mask,
                            __m256i *acc_lo,
                            __m256i *acc_hi) {
    __m256i q32 = _mm256_cvtepi16_epi32(q16);
    __m256i min32 = _mm256_cvtepi16_epi32(min16);
    __m256i max32 = _mm256_cvtepi16_epi32(max16);
    __m256i zero = _mm256_setzero_si256();
    __m256i below = _mm256_max_epi32(_mm256_sub_epi32(min32, q32), zero);
    __m256i above = _mm256_max_epi32(_mm256_sub_epi32(q32, max32), zero);
    __m256i diff = _mm256_and_si256(_mm256_max_epi32(below, above), lane_mask);
    __m256i squared32 = _mm256_mullo_epi32(diff, diff);
    __m128i lo32 = _mm256_castsi256_si128(squared32);
    __m128i hi32 = _mm256_extracti128_si256(squared32, 1);
    *acc_lo = _mm256_add_epi64(*acc_lo, _mm256_cvtepu32_epi64(lo32));
    *acc_hi = _mm256_add_epi64(*acc_hi, _mm256_cvtepu32_epi64(hi32));
}

static uint64_t sum_accumulators(__m256i acc_lo, __m256i acc_hi) {
    uint64_t values[IVF8_INDEX_LANES];
    _mm256_storeu_si256((__m256i *)(void *)values, acc_lo);
    _mm256_storeu_si256((__m256i *)(void *)(values + 4u), acc_hi);
    uint64_t sum = 0;
    for (uint32_t lane = 0; lane < IVF8_INDEX_LANES; lane++) {
        sum += values[lane];
    }
    return sum;
}

uint64_t kdclass3_opt_bbox_distance_avx2(
    const KdClass3Node *node,
    const int16_t query[IVF8_INDEX_DIMS],
    uint64_t limit,
    uint32_t *dimensions_evaluated) {
    if (node == NULL || query == NULL) {
        return UINT64_MAX;
    }
    __m256i acc_lo = _mm256_setzero_si256();
    __m256i acc_hi = _mm256_setzero_si256();
    __m256i all_lanes = _mm256_set1_epi32(-1);
    __m256i six_lanes =
        _mm256_setr_epi32(-1, -1, -1, -1, -1, -1, 0, 0);

    bbox_accumulate(
        _mm_loadu_si128((const __m128i *)(const void *)query),
        _mm_loadu_si128((const __m128i *)(const void *)node->bbox_min),
        _mm_loadu_si128((const __m128i *)(const void *)node->bbox_max),
        all_lanes, &acc_lo, &acc_hi);
    if (limit != UINT64_MAX) {
        uint64_t partial = sum_accumulators(acc_lo, acc_hi);
        if (partial > limit) {
            if (dimensions_evaluated != NULL) {
                *dimensions_evaluated = 8u;
            }
            return partial;
        }
    }

    __m128i query_tail = _mm_setr_epi16(
        query[8], query[9], query[10], query[11],
        query[12], query[13], 0, 0);
    bbox_accumulate(
        query_tail,
        _mm_loadu_si128((const __m128i *)(const void *)(node->bbox_min + 8u)),
        _mm_loadu_si128((const __m128i *)(const void *)(node->bbox_max + 8u)),
        six_lanes, &acc_lo, &acc_hi);
    if (dimensions_evaluated != NULL) {
        *dimensions_evaluated = IVF8_INDEX_DIMS;
    }
    return sum_accumulators(acc_lo, acc_hi);
}

uint32_t kdclass3_opt_block_distances_avx2(
    const int16_t *block_data,
    uint32_t block,
    const int16_t query[IVF8_INDEX_DIMS],
    uint64_t limit,
    uint64_t out[IVF8_INDEX_LANES],
    uint32_t *dimensions_evaluated) {
    const size_t block_base =
        (size_t)block * IVF8_INDEX_DIMS * IVF8_INDEX_LANES;
    const int16_t *base = block_data + block_base;
    __m256i acc_lo = _mm256_setzero_si256();
    __m256i acc_hi = _mm256_setzero_si256();
    __m256i limit_vec = _mm256_set1_epi64x((long long)limit);

    for (uint32_t dim = 0; dim < IVF8_INDEX_DIMS; dim++) {
        const int16_t *values = base + dim * IVF8_INDEX_LANES;
        __m128i v = _mm_loadu_si128((const __m128i *)(const void *)values);
        __m128i q = _mm_set1_epi16(query[dim]);
        __m128i d16 = _mm_sub_epi16(q, v);
        __m256i d32 = _mm256_cvtepi16_epi32(d16);
        __m256i squared32 = _mm256_mullo_epi32(d32, d32);
        __m128i lo32 = _mm256_castsi256_si128(squared32);
        __m128i hi32 = _mm256_extracti128_si256(squared32, 1);
        acc_lo = _mm256_add_epi64(acc_lo, _mm256_cvtepu32_epi64(lo32));
        acc_hi = _mm256_add_epi64(acc_hi, _mm256_cvtepu32_epi64(hi32));

        uint32_t completed = dim + 1u;
        bool checkpoint = completed == 8u || completed == IVF8_INDEX_DIMS;
        if (limit != UINT64_MAX && checkpoint &&
            alive_mask(acc_lo, acc_hi, limit_vec) == 0u) {
            if (dimensions_evaluated != NULL) {
                *dimensions_evaluated = completed;
            }
            return 0u;
        }
    }

    _mm256_storeu_si256((__m256i *)(void *)out, acc_lo);
    _mm256_storeu_si256((__m256i *)(void *)(out + 4u), acc_hi);
    if (dimensions_evaluated != NULL) {
        *dimensions_evaluated = IVF8_INDEX_DIMS;
    }
    return limit == UINT64_MAX ? 0xffu : alive_mask(acc_lo, acc_hi, limit_vec);
}
