#include "ivf8_search.h"

#include <immintrin.h>
#include <stddef.h>
#include <stdatomic.h>

bool ivf8_cpu_supports_avx2(void) {
    static atomic_int cached = 0;
    int state = atomic_load_explicit(&cached, memory_order_relaxed);
    if (state != 0) {
        return state == 2;
    }
#if defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__))
    __builtin_cpu_init();
    bool supported = __builtin_cpu_supports("avx2") != 0;
#else
    bool supported = false;
#endif
    atomic_store_explicit(&cached, supported ? 2 : 1, memory_order_relaxed);
    return supported;
}

static void distances_soa8_avx2(const int16_t *base,
                                uint32_t stride,
                                const int16_t query[IVF8_INDEX_DIMS],
                                uint64_t out[IVF8_INDEX_LANES]) {
    __m256i acc_lo = _mm256_setzero_si256();
    __m256i acc_hi = _mm256_setzero_si256();

    for (uint32_t dim = 0; dim < IVF8_INDEX_DIMS; dim++) {
        const int16_t *values = base + dim * stride;
        __m128i v = _mm_loadu_si128((const __m128i *)(const void *)values);
        __m128i q = _mm_set1_epi16(query[dim]);
        __m128i d16 = _mm_sub_epi16(q, v);
        __m256i d32 = _mm256_cvtepi16_epi32(d16);
        __m256i squared32 = _mm256_mullo_epi32(d32, d32);

        __m128i lo32 = _mm256_castsi256_si128(squared32);
        __m128i hi32 = _mm256_extracti128_si256(squared32, 1);
        acc_lo = _mm256_add_epi64(acc_lo, _mm256_cvtepu32_epi64(lo32));
        acc_hi = _mm256_add_epi64(acc_hi, _mm256_cvtepu32_epi64(hi32));
    }

    _mm256_storeu_si256((__m256i *)(void *)out, acc_lo);
    _mm256_storeu_si256((__m256i *)(void *)(out + 4u), acc_hi);
}

void ivf8_block_distances_avx2(const int16_t *block_data,
                               uint32_t block,
                               const int16_t query[IVF8_INDEX_DIMS],
                               uint64_t out[IVF8_INDEX_LANES]) {
    const uint32_t block_base = block * IVF8_INDEX_DIMS * IVF8_INDEX_LANES;
    distances_soa8_avx2(block_data + block_base, IVF8_INDEX_LANES, query, out);
}

void ivf8_centroid_distances_avx2(const Ivf8Index *idx,
                                  const int16_t query[IVF8_INDEX_DIMS],
                                  uint32_t cluster_start,
                                  uint64_t out[IVF8_INDEX_LANES]) {
    distances_soa8_avx2(idx->centroids + cluster_start, idx->k, query, out);
}
