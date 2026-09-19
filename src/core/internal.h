/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MBLUR_INTERNAL_H
#define MBLUR_INTERNAL_H

#include "mblur/mblur.h"
#include <stdlib.h>
#include <string.h>

/*
 * Every supported pixel format reduces to a list of 8-bit byte planes. The
 * blend itself does not care whether a byte is red, luma or chroma - it only
 * needs to know how wide each plane is and, for the linear-light path, which
 * byte positions are alpha (alpha is already linear and must not be run
 * through the transfer function).
 */
typedef struct mblur_plane_info {
    uint32_t bytes_wide; /* bytes of real data per row */
    uint32_t rows;
    uint32_t channels;   /* repeat period of the channel pattern, in bytes */
    int alpha_index;     /* byte offset of alpha within `channels`, or -1 */
} mblur_plane_info;

typedef struct mblur_layout {
    mblur_plane_info plane[MBLUR_MAX_PLANES];
    uint32_t plane_count;
    size_t total_bytes; /* sum of bytes_wide*rows over all planes */
    int is_yuv;
} mblur_layout;

mblur_status mblur_layout_init(mblur_layout *lay, mblur_format fmt,
                               uint32_t width, uint32_t height);

/*
 * Accumulator format: unsigned Q16 (value * 65535) held in uint16_t.
 *
 * Weights are normalised to sum to 1 and samples are in [0,1], so the
 * running sum can never exceed 1.0 and fits exactly. Each add rounds at
 * 2^-16; across N<=64 taps worst-case drift stays under 2^-10, which is far
 * below the 2^-8 an 8-bit output can represent. Using uint16 rather than
 * float halves accumulator bandwidth, and this kernel is bandwidth-bound.
 */
typedef uint16_t mblur_acc;

#if defined(_MSC_VER)
#define MBLUR_RESTRICT __restrict
#else
#define MBLUR_RESTRICT restrict
#endif

#define MBLUR_LUT_ENTRIES 256
#define MBLUR_OETF_ENTRIES 65536

/*
 * One per tap: maps a source byte straight to its weighted Q16 contribution,
 * with the transfer function and the tap's weight already folded in. That
 * turns the hot loop into "load byte, load table, add" with no multiply.
 * Indexed as lut[channel * 256 + byte], so alpha can use an identity curve
 * while colour channels get the EOTF.
 */
typedef struct mblur_tap_lut {
    uint16_t table[4 * MBLUR_LUT_ENTRIES];
} mblur_tap_lut;

/*
 * Dither tables. Dither has to be reproducible: the SIMD and scalar kernels
 * are compared bit-for-bit in the test suite, which rules out a sequential
 * RNG whose sequence depends on vector width. Instead the pattern is a
 * fixed 64-entry triangular table indexed by the sample's position, stored
 * duplicated to 128 entries so any 32-wide vector can load a contiguous
 * window regardless of alignment. Split into positive and negative halves
 * so both kernels can use saturating unsigned arithmetic and agree exactly.
 */
#define MBLUR_DITHER_PERIOD 64
#define MBLUR_DITHER_ENTRIES 128

typedef struct mblur_dither {
    uint16_t pos[MBLUR_DITHER_ENTRIES];
    uint16_t neg[MBLUR_DITHER_ENTRIES];
} mblur_dither;

void mblur_build_dither(mblur_dither *d, int enabled);

/* Kernel table, selected once at create time by CPU feature detection. */
typedef struct mblur_kernels {
    const char *name;
    mblur_backend backend;

    /*
     * acc[i] += lut[(i % channels) * 256 + src[i]]
     * The linear-light path. `channels` is 4 only when the per-channel
     * tables actually differ (i.e. there is an alpha channel to exempt from
     * the transfer function); otherwise the caller passes 1.
     */
    void (*accum_lut)(mblur_acc *acc, const uint8_t *src, size_t n,
                      const uint16_t *lut, uint32_t channels);

    /* acc[i] += (src[i] * 257 * w) >> 16   (gamma-space, no table) */
    void (*accum_mul)(mblur_acc *acc, const uint8_t *src, size_t n,
                      uint16_t w_q16);

    /*
     * The same two accumulate steps, but writing rather than adding. The
     * first tap of a window has nothing to add to, so starting it with a
     * store removes a separate zeroing pass over the accumulator - one
     * fewer full-frame write per window in decimate mode, and one fewer
     * out of N+1 passes in a re-summed rolling window.
     */
    void (*store_lut)(mblur_acc *acc, const uint8_t *src, size_t n,
                      const uint16_t *lut, uint32_t channels);
    void (*store_mul)(mblur_acc *acc, const uint8_t *src, size_t n,
                      uint16_t w_q16);

    /* As above but subtracting: the sliding-window fast path used by
     * rolling mode when every tap carries the same weight. */
    void (*accum_mul_sub)(mblur_acc *acc, const uint8_t *src, size_t n,
                          uint16_t w_q16);

    /* Table-driven counterpart of accum_mul_sub, so the sliding window is
     * available on the linear-light path too - which is the default, and
     * therefore the case worth making fast. */
    void (*accum_lut_sub)(mblur_acc *acc, const uint8_t *src, size_t n,
                          const uint16_t *lut, uint32_t channels);

    /* dst[i] = dither_and_narrow(acc[i]) */
    void (*resolve_direct)(uint8_t *dst, const mblur_acc *acc, size_t n,
                           const mblur_dither *d, size_t base);

    /*
     * dst[i] = dither_and_narrow(oetf[acc[i]])
     *
     * `alpha_phase` names the byte position of alpha within each 4-byte
     * pixel, or -1 when there is none. Alpha was accumulated linearly and
     * must skip the table; running it through the OETF brightens every
     * transparent pixel, which is subtle enough to ship by accident.
     */
    void (*resolve_oetf)(uint8_t *dst, const mblur_acc *acc, size_t n,
                         const uint16_t *oetf, int alpha_phase,
                         const mblur_dither *d, size_t base);

    void (*acc_zero)(mblur_acc *acc, size_t n);
} mblur_kernels;

const mblur_kernels *mblur_kernels_for(mblur_backend b);
mblur_backend mblur_resolve_backend(mblur_backend requested);

/* Kernel implementations. Each file compiles with its own ISA flags. */
extern const mblur_kernels mblur_kernels_scalar;
#if defined(MBLUR_X86)
extern const mblur_kernels mblur_kernels_sse2;
extern const mblur_kernels mblur_kernels_avx2;
extern const mblur_kernels mblur_kernels_avx512;
#endif

/* colour.c */
double mblur_eotf(double encoded, mblur_transfer trc); /* encoded -> linear */
double mblur_oetf(double linear, mblur_transfer trc);  /* linear -> encoded */
void mblur_build_tap_lut(mblur_tap_lut *lut, float weight, mblur_transfer trc,
                         int linear_light, const mblur_plane_info *pi);
void mblur_build_oetf_table(uint16_t *table, mblur_transfer trc,
                            int linear_light);

/* Xorshift32. Dither only needs to be cheap and white, not cryptographic. */
static inline uint32_t mblur_rng_next(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

#define MBLUR_ALIGN 64
void *mblur_aligned_alloc(size_t bytes);
void mblur_aligned_free(void *p);

#endif /* MBLUR_INTERNAL_H */
