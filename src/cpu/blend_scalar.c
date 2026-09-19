/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The portable reference implementation.
 *
 * This file is the definition of correct: every vectorised kernel is
 * required by the test suite to produce byte-identical output to these
 * functions, which is why the arithmetic here is written in terms of
 * operations that map one-to-one onto saturating SIMD instructions rather
 * than in whatever form reads most naturally.
 */
#include "../core/internal.h"

void mblur_scalar_accum_lut(mblur_acc *MBLUR_RESTRICT acc,
                            const uint8_t *MBLUR_RESTRICT src, size_t n,
                            const uint16_t *MBLUR_RESTRICT lut,
                            uint32_t channels)
{
    size_t i = 0;

    if (channels == 4) {
        /* Unrolled so the channel index is a constant in each statement and
         * the four table loads can issue independently. */
        for (; i + 4 <= n; i += 4) {
            acc[i + 0] = (mblur_acc)(acc[i + 0] + lut[0 * 256 + src[i + 0]]);
            acc[i + 1] = (mblur_acc)(acc[i + 1] + lut[1 * 256 + src[i + 1]]);
            acc[i + 2] = (mblur_acc)(acc[i + 2] + lut[2 * 256 + src[i + 2]]);
            acc[i + 3] = (mblur_acc)(acc[i + 3] + lut[3 * 256 + src[i + 3]]);
        }
        for (; i < n; i++)
            acc[i] = (mblur_acc)(acc[i] + lut[(i & 3) * 256 + src[i]]);
        return;
    }

    for (; i + 8 <= n; i += 8) {
        acc[i + 0] = (mblur_acc)(acc[i + 0] + lut[src[i + 0]]);
        acc[i + 1] = (mblur_acc)(acc[i + 1] + lut[src[i + 1]]);
        acc[i + 2] = (mblur_acc)(acc[i + 2] + lut[src[i + 2]]);
        acc[i + 3] = (mblur_acc)(acc[i + 3] + lut[src[i + 3]]);
        acc[i + 4] = (mblur_acc)(acc[i + 4] + lut[src[i + 4]]);
        acc[i + 5] = (mblur_acc)(acc[i + 5] + lut[src[i + 5]]);
        acc[i + 6] = (mblur_acc)(acc[i + 6] + lut[src[i + 6]]);
        acc[i + 7] = (mblur_acc)(acc[i + 7] + lut[src[i + 7]]);
    }
    for (; i < n; i++)
        acc[i] = (mblur_acc)(acc[i] + lut[src[i]]);
}

void mblur_scalar_accum_lut_sub(mblur_acc *MBLUR_RESTRICT acc,
                                const uint8_t *MBLUR_RESTRICT src, size_t n,
                                const uint16_t *MBLUR_RESTRICT lut,
                                uint32_t channels)
{
    size_t i = 0;

    if (channels == 4) {
        for (; i + 4 <= n; i += 4) {
            acc[i + 0] = (mblur_acc)(acc[i + 0] - lut[0 * 256 + src[i + 0]]);
            acc[i + 1] = (mblur_acc)(acc[i + 1] - lut[1 * 256 + src[i + 1]]);
            acc[i + 2] = (mblur_acc)(acc[i + 2] - lut[2 * 256 + src[i + 2]]);
            acc[i + 3] = (mblur_acc)(acc[i + 3] - lut[3 * 256 + src[i + 3]]);
        }
        for (; i < n; i++)
            acc[i] = (mblur_acc)(acc[i] - lut[(i & 3) * 256 + src[i]]);
        return;
    }

    for (; i < n; i++)
        acc[i] = (mblur_acc)(acc[i] - lut[src[i]]);
}

void mblur_scalar_store_lut(mblur_acc *MBLUR_RESTRICT acc,
                            const uint8_t *MBLUR_RESTRICT src, size_t n,
                            const uint16_t *MBLUR_RESTRICT lut,
                            uint32_t channels)
{
    size_t i = 0;

    if (channels == 4) {
        for (; i + 4 <= n; i += 4) {
            acc[i + 0] = lut[0 * 256 + src[i + 0]];
            acc[i + 1] = lut[1 * 256 + src[i + 1]];
            acc[i + 2] = lut[2 * 256 + src[i + 2]];
            acc[i + 3] = lut[3 * 256 + src[i + 3]];
        }
        for (; i < n; i++)
            acc[i] = lut[(i & 3) * 256 + src[i]];
        return;
    }

    for (; i < n; i++)
        acc[i] = lut[src[i]];
}

void mblur_scalar_store_mul(mblur_acc *MBLUR_RESTRICT acc,
                            const uint8_t *MBLUR_RESTRICT src, size_t n,
                            uint16_t w)
{
    for (size_t i = 0; i < n; i++) {
        uint32_t v = (uint32_t)src[i] * 257u;
        acc[i] = (mblur_acc)((v * (uint32_t)w) >> 16);
    }
}

void mblur_scalar_accum_mul(mblur_acc *MBLUR_RESTRICT acc,
                            const uint8_t *MBLUR_RESTRICT src, size_t n,
                            uint16_t w)
{
    /* src * 257 maps 0..255 onto 0..65535 exactly; the high half of the
     * product with a Q16 weight is the weighted contribution. Truncating
     * (not rounding) matches _mm*_mulhi_epu16 bit for bit. */
    for (size_t i = 0; i < n; i++) {
        uint32_t v = (uint32_t)src[i] * 257u;
        acc[i] = (mblur_acc)(acc[i] + (uint16_t)((v * (uint32_t)w) >> 16));
    }
}

void mblur_scalar_accum_mul_sub(mblur_acc *MBLUR_RESTRICT acc,
                                const uint8_t *MBLUR_RESTRICT src, size_t n,
                                uint16_t w)
{
    for (size_t i = 0; i < n; i++) {
        uint32_t v = (uint32_t)src[i] * 257u;
        acc[i] = (mblur_acc)(acc[i] - (uint16_t)((v * (uint32_t)w) >> 16));
    }
}

static inline uint8_t narrow(uint16_t v, uint16_t dpos, uint16_t dneg)
{
    uint32_t t = (uint32_t)v + dpos;
    if (t > 65535u)
        t = 65535u; /* saturating add */
    t = (t > dneg) ? (t - dneg) : 0u; /* saturating subtract */
    return (uint8_t)(t >> 8);
}

void mblur_scalar_resolve_direct(uint8_t *dst, const mblur_acc *acc, size_t n,
                                 const mblur_dither *d, size_t base)
{
    for (size_t i = 0; i < n; i++) {
        size_t k = (base + i) & (MBLUR_DITHER_PERIOD - 1);
        dst[i] = narrow(acc[i], d->pos[k], d->neg[k]);
    }
}

void mblur_scalar_resolve_oetf(uint8_t *dst, const mblur_acc *acc, size_t n,
                               const uint16_t *oetf, int alpha_phase,
                               const mblur_dither *d, size_t base)
{
    for (size_t i = 0; i < n; i++) {
        const size_t pos = base + i;
        const size_t k = pos & (MBLUR_DITHER_PERIOD - 1);
        const int is_alpha = (alpha_phase >= 0) &&
                             ((pos & 3u) == (size_t)alpha_phase);
        const uint16_t v = is_alpha ? acc[i] : oetf[acc[i]];
        dst[i] = narrow(v, d->pos[k], d->neg[k]);
    }
}

void mblur_scalar_acc_zero(mblur_acc *acc, size_t n)
{
    memset(acc, 0, n * sizeof(*acc));
}

const mblur_kernels mblur_kernels_scalar = {
    .name = "scalar",
    .backend = MBLUR_BACKEND_CPU_SCALAR,
    .accum_lut = mblur_scalar_accum_lut,
    .accum_mul = mblur_scalar_accum_mul,
    .store_lut = mblur_scalar_store_lut,
    .store_mul = mblur_scalar_store_mul,
    .accum_mul_sub = mblur_scalar_accum_mul_sub,
    .accum_lut_sub = mblur_scalar_accum_lut_sub,
    .resolve_direct = mblur_scalar_resolve_direct,
    .resolve_oetf = mblur_scalar_resolve_oetf,
    .acc_zero = mblur_scalar_acc_zero,
};
