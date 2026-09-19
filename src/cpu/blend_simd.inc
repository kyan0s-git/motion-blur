/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Shared body for the vectorised kernels. Included once per instruction set
 * by a wrapper that defines the MB_* macros and compiles with that ISA
 * enabled, so a single expression of the algorithm produces the SSE2, AVX2
 * and AVX-512 kernels.
 *
 * Only the gamma-space accumulate and the resolve are vectorised. The
 * linear-light accumulate is a 256-entry table lookup per byte, which no
 * shuffle trick beats and which is bounded by accumulator bandwidth anyway,
 * so every backend shares the scalar version of it.
 */

/* Provided by the includer: MB_NAME(x), MB_VEC, MB_LANES, and the MB_* ops. */

void MB_NAME(store_mul)(mblur_acc *MBLUR_RESTRICT acc,
                        const uint8_t *MBLUR_RESTRICT src, size_t n,
                        uint16_t w)
{
    const MB_VEC vw = MB_SET1(w);
    const MB_VEC v257 = MB_SET1(257);
    size_t i = 0;

    for (; i + MB_LANES <= n; i += MB_LANES) {
        MB_VEC v = MB_LOAD_U8_WIDEN(src + i);
        v = MB_MULLO(v, v257);
        MB_STORE(acc + i, MB_MULHI(v, vw));
    }
    if (i < n)
        mblur_scalar_store_mul(acc + i, src + i, n - i, w);
}

void MB_NAME(accum_mul)(mblur_acc *MBLUR_RESTRICT acc,
                        const uint8_t *MBLUR_RESTRICT src, size_t n,
                        uint16_t w)
{
    const MB_VEC vw = MB_SET1(w);
    const MB_VEC v257 = MB_SET1(257);
    size_t i = 0;

    for (; i + MB_LANES <= n; i += MB_LANES) {
        MB_VEC v = MB_LOAD_U8_WIDEN(src + i);
        v = MB_MULLO(v, v257);          /* 0..255 -> 0..65535 exactly */
        v = MB_MULHI(v, vw);            /* (v * w) >> 16, truncating */
        MB_STORE(acc + i, MB_ADD(MB_LOAD(acc + i), v));
    }
    if (i < n)
        mblur_scalar_accum_mul(acc + i, src + i, n - i, w);
}

void MB_NAME(accum_mul_sub)(mblur_acc *MBLUR_RESTRICT acc,
                            const uint8_t *MBLUR_RESTRICT src, size_t n,
                            uint16_t w)
{
    const MB_VEC vw = MB_SET1(w);
    const MB_VEC v257 = MB_SET1(257);
    size_t i = 0;

    for (; i + MB_LANES <= n; i += MB_LANES) {
        MB_VEC v = MB_LOAD_U8_WIDEN(src + i);
        v = MB_MULLO(v, v257);
        v = MB_MULHI(v, vw);
        MB_STORE(acc + i, MB_SUB(MB_LOAD(acc + i), v));
    }
    if (i < n)
        mblur_scalar_accum_mul_sub(acc + i, src + i, n - i, w);
}

void MB_NAME(resolve_direct)(uint8_t *MBLUR_RESTRICT dst,
                             const mblur_acc *MBLUR_RESTRICT acc, size_t n,
                             const mblur_dither *d, size_t base)
{
    size_t i = 0;

    for (; i + MB_LANES <= n; i += MB_LANES) {
        /* The dither table is duplicated to twice its period, so the window
         * starting at any phase is contiguous and needs no wrap handling. */
        const size_t k = (base + i) & (MBLUR_DITHER_PERIOD - 1);
        MB_VEC v = MB_LOAD(acc + i);
        v = MB_ADDS(v, MB_LOAD(d->pos + k));
        v = MB_SUBS(v, MB_LOAD(d->neg + k));
        MB_STORE_U8_NARROW(dst + i, MB_SRLI8(v));
    }
    if (i < n)
        mblur_scalar_resolve_direct(dst + i, acc + i, n - i, d, base + i);
}

void MB_NAME(resolve_oetf)(uint8_t *dst, const mblur_acc *acc, size_t n,
                           const uint16_t *oetf, int alpha_phase,
                           const mblur_dither *d, size_t base)
{
    /* A 64K-entry gather has no good vector form; the table lookup stays
     * scalar and only the dither-and-narrow tail would vectorise, which is
     * not worth the round trip through memory. */
    mblur_scalar_resolve_oetf(dst, acc, n, oetf, alpha_phase, d, base);
}

const mblur_kernels MB_KERNEL_SYMBOL = {
    .name = MB_KERNEL_NAME,
    .backend = MB_KERNEL_BACKEND,
    .accum_lut = mblur_scalar_accum_lut,
    .accum_mul = MB_NAME(accum_mul),
    .store_lut = mblur_scalar_store_lut,
    .store_mul = MB_NAME(store_mul),
    .accum_mul_sub = MB_NAME(accum_mul_sub),
    .accum_lut_sub = mblur_scalar_accum_lut_sub,
    .resolve_direct = MB_NAME(resolve_direct),
    .resolve_oetf = MB_NAME(resolve_oetf),
    .acc_zero = mblur_scalar_acc_zero,
};
