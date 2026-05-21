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
    __m256d acc_lo = _mm256_setzero_pd();
    __m256d acc_hi = _mm256_setzero_pd();

    for (uint32_t dim = 0; dim < IVF8_INDEX_DIMS; dim++) {
        const int16_t *values = base + dim * stride;
        __m128i v = _mm_loadu_si128((const __m128i *)(const void *)values);
        __m128i q = _mm_set1_epi16(query[dim]);
        __m128i d16 = _mm_sub_epi16(q, v);
        __m256i d32 = _mm256_cvtepi16_epi32(d16);

        __m128i lo32 = _mm256_castsi256_si128(d32);
        __m128i hi32 = _mm256_extracti128_si256(d32, 1);
        __m256d dlo = _mm256_cvtepi32_pd(lo32);
        __m256d dhi = _mm256_cvtepi32_pd(hi32);
        acc_lo = _mm256_add_pd(acc_lo, _mm256_mul_pd(dlo, dlo));
        acc_hi = _mm256_add_pd(acc_hi, _mm256_mul_pd(dhi, dhi));
    }

    double tmp_lo[4];
    double tmp_hi[4];
    _mm256_storeu_pd(tmp_lo, acc_lo);
    _mm256_storeu_pd(tmp_hi, acc_hi);
    for (uint32_t i = 0; i < 4; i++) {
        out[i] = (uint64_t)tmp_lo[i];
        out[i + 4u] = (uint64_t)tmp_hi[i];
    }
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
