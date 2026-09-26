/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../core/internal.h"
#if defined(MBLUR_X86)
#include <emmintrin.h>

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

#define MB_NAME(x) mblur_sse2_##x
#define MB_VEC __m128i
#define MB_LANES 8
#define MB_SET1(v) _mm_set1_epi16((short)(v))
#define MB_LOAD(p) _mm_loadu_si128((const __m128i *)(const void *)(p))
#define MB_STORE(p, v) _mm_storeu_si128((__m128i *)(void *)(p), (v))
#define MB_LOAD_U8_WIDEN(p) \
    _mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i *)(const void *)(p)), \
                      _mm_setzero_si128())
#define MB_MULLO(a, b) _mm_mullo_epi16((a), (b))
#define MB_MULHI(a, b) _mm_mulhi_epu16((a), (b))
#define MB_ADD(a, b) _mm_add_epi16((a), (b))
#define MB_SUB(a, b) _mm_sub_epi16((a), (b))
#define MB_ADDS(a, b) _mm_adds_epu16((a), (b))
#define MB_SUBS(a, b) _mm_subs_epu16((a), (b))
#define MB_SRLI8(v) _mm_srli_epi16((v), 8)
#define MB_STORE_U8_NARROW(p, v) \
    _mm_storel_epi64((__m128i *)(void *)(p), _mm_packus_epi16((v), (v)))
#define MB_KERNEL_SYMBOL mblur_kernels_sse2
#define MB_KERNEL_NAME "sse2"
#define MB_KERNEL_BACKEND MBLUR_BACKEND_CPU_SSE2

#include "blend_simd.inc"
#endif /* MBLUR_X86 */
