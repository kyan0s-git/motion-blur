/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../core/internal.h"
#if defined(MBLUR_X86)
#include <immintrin.h>

void mblur_scalar_accum_lut(mblur_acc *, const uint8_t *, size_t,
                            const uint16_t *, uint32_t);
void mblur_scalar_accum_lut_sub(mblur_acc *, const uint8_t *, size_t,
                                const uint16_t *, uint32_t);
void mblur_scalar_accum_mul(mblur_acc *, const uint8_t *, size_t, uint16_t);
void mblur_scalar_store_lut(mblur_acc *, const uint8_t *, size_t,
                            const uint16_t *, uint32_t);
void mblur_scalar_store_mul(mblur_acc *, const uint8_t *, size_t, uint16_t);
void mblur_scalar_accum_mul_sub(mblur_acc *, const uint8_t *, size_t, uint16_t);
void mblur_scalar_resolve_direct(uint8_t *, const mblur_acc *, size_t,
                                 const mblur_dither *, size_t);
void mblur_scalar_resolve_oetf(uint8_t *, const mblur_acc *, size_t,
                               const uint16_t *, int, const mblur_dither *,
                               size_t);
void mblur_scalar_acc_zero(mblur_acc *, size_t);

/* packus works per 128-bit lane, so the halves come out interleaved; the
 * permute puts the qwords back in source order before the 16-byte store. */
static inline void mblur_avx2_store_narrow(uint8_t *p, __m256i v)
{
    __m256i packed = _mm256_packus_epi16(v, v);
    packed = _mm256_permute4x64_epi64(packed, 0xD8);
    _mm_storeu_si128((__m128i *)(void *)p, _mm256_castsi256_si128(packed));
}

#define MB_NAME(x) mblur_avx2_##x
#define MB_VEC __m256i
#define MB_LANES 16
#define MB_SET1(v) _mm256_set1_epi16((short)(v))
#define MB_LOAD(p) _mm256_loadu_si256((const __m256i *)(const void *)(p))
#define MB_STORE(p, v) _mm256_storeu_si256((__m256i *)(void *)(p), (v))
#define MB_LOAD_U8_WIDEN(p) \
    _mm256_cvtepu8_epi16(_mm_loadu_si128((const __m128i *)(const void *)(p)))
#define MB_MULLO(a, b) _mm256_mullo_epi16((a), (b))
#define MB_MULHI(a, b) _mm256_mulhi_epu16((a), (b))
#define MB_ADD(a, b) _mm256_add_epi16((a), (b))
#define MB_SUB(a, b) _mm256_sub_epi16((a), (b))
#define MB_ADDS(a, b) _mm256_adds_epu16((a), (b))
#define MB_SUBS(a, b) _mm256_subs_epu16((a), (b))
#define MB_SRLI8(v) _mm256_srli_epi16((v), 8)
#define MB_STORE_U8_NARROW(p, v) mblur_avx2_store_narrow((p), (v))
#define MB_KERNEL_SYMBOL mblur_kernels_avx2
#define MB_KERNEL_NAME "avx2"
#define MB_KERNEL_BACKEND MBLUR_BACKEND_CPU_AVX2

#include "blend_simd.inc"
#endif /* MBLUR_X86 */
