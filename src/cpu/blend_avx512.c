/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../core/internal.h"
#if defined(MBLUR_X86)
#include <immintrin.h>

void mblur_scalar_accum_lut(mblur_acc *MBLUR_RESTRICT,
                            const uint8_t *MBLUR_RESTRICT, size_t,
                            const uint16_t *MBLUR_RESTRICT, uint32_t);
void mblur_scalar_accum_lut_sub(mblur_acc *MBLUR_RESTRICT,
                                const uint8_t *MBLUR_RESTRICT, size_t,
                                const uint16_t *MBLUR_RESTRICT, uint32_t);
void mblur_scalar_accum_mul(mblur_acc *MBLUR_RESTRICT,
                            const uint8_t *MBLUR_RESTRICT, size_t, uint16_t);
void mblur_scalar_store_lut(mblur_acc *MBLUR_RESTRICT,
                            const uint8_t *MBLUR_RESTRICT, size_t,
                            const uint16_t *MBLUR_RESTRICT, uint32_t);
void mblur_scalar_store_mul(mblur_acc *MBLUR_RESTRICT,
                            const uint8_t *MBLUR_RESTRICT, size_t, uint16_t);
void mblur_scalar_accum_mul_sub(mblur_acc *MBLUR_RESTRICT,
                                const uint8_t *MBLUR_RESTRICT, size_t,
                                uint16_t);
void mblur_scalar_resolve_direct(uint8_t *MBLUR_RESTRICT,
                                 const mblur_acc *MBLUR_RESTRICT, size_t,
                                 const mblur_dither *, size_t);
void mblur_scalar_resolve_oetf(uint8_t *, const mblur_acc *, size_t,
                               const uint16_t *, int, const mblur_dither *,
                               size_t);
void mblur_scalar_acc_zero(mblur_acc *, size_t);

#define MB_NAME(x) mblur_avx512_##x
#define MB_VEC __m512i
#define MB_LANES 32
#define MB_SET1(v) _mm512_set1_epi16((short)(v))
#define MB_LOAD(p) _mm512_loadu_si512((const void *)(p))
#define MB_STORE(p, v) _mm512_storeu_si512((void *)(p), (v))
#define MB_LOAD_U8_WIDEN(p) \
    _mm512_cvtepu8_epi16(_mm256_loadu_si256((const __m256i *)(const void *)(p)))
#define MB_MULLO(a, b) _mm512_mullo_epi16((a), (b))
#define MB_MULHI(a, b) _mm512_mulhi_epu16((a), (b))
#define MB_ADD(a, b) _mm512_add_epi16((a), (b))
#define MB_SUB(a, b) _mm512_sub_epi16((a), (b))
#define MB_ADDS(a, b) _mm512_adds_epu16((a), (b))
#define MB_SUBS(a, b) _mm512_subs_epu16((a), (b))
#define MB_SRLI8(v) _mm512_srli_epi16((v), 8)
/* Values are already <= 255 after the shift, so the truncating narrow is
 * exact and cheaper than a saturating pack. */
#define MB_STORE_U8_NARROW(p, v) \
    _mm256_storeu_si256((__m256i *)(void *)(p), _mm512_cvtepi16_epi8((v)))
#define MB_KERNEL_SYMBOL mblur_kernels_avx512
#define MB_KERNEL_NAME "avx512"
#define MB_KERNEL_BACKEND MBLUR_BACKEND_CPU_AVX512

#include "blend_simd.inc"
#endif /* MBLUR_X86 */
