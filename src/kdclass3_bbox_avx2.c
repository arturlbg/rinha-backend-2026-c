#include "kdclass3.h"

#include <immintrin.h>
#include <stddef.h>

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

uint64_t kdclass3_bbox_distance_avx2(
    const KdClass3Node *node,
    const int16_t query[IVF8_INDEX_DIMS]) {
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

    __m128i query_tail = _mm_setr_epi16(
        query[8], query[9], query[10], query[11],
        query[12], query[13], 0, 0);
    bbox_accumulate(
        query_tail,
        _mm_loadu_si128((const __m128i *)(const void *)(node->bbox_min + 8u)),
        _mm_loadu_si128((const __m128i *)(const void *)(node->bbox_max + 8u)),
        six_lanes, &acc_lo, &acc_hi);
    return sum_accumulators(acc_lo, acc_hi);
}
