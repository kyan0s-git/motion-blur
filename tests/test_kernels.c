/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Every vectorised kernel must produce byte-identical output to the scalar
 * reference - not "close", identical. That is what lets the engine pick a
 * backend at runtime without the picture changing, and it is what makes the
 * golden-hash tests meaningful on any machine.
 */
#include "core/internal.h"
#include "mbtest.h"
#include <math.h>

static const mblur_backend k_candidates[] = {
    MBLUR_BACKEND_CPU_SCALAR, MBLUR_BACKEND_CPU_SSE2, MBLUR_BACKEND_CPU_AVX2,
    MBLUR_BACKEND_CPU_AVX512};

/* Sizes chosen to exercise every tail: exact multiples of 8/16/32 lanes and
 * awkward remainders either side of them. */
static const size_t k_sizes[] = {1, 7, 8, 15, 16, 17, 31, 32, 33, 63, 64, 65,
                                 127, 128, 255, 4096, 4099};

int main(void)
{
    printf("test_kernels (best available: %s)\n",
           mblur_backend_name(mblur_cpu_best_backend()));

    enum { MAXN = 4099 };
    uint8_t *src = malloc(MAXN);
    mblur_acc *ref = malloc(MAXN * sizeof(mblur_acc));
    mblur_acc *got = malloc(MAXN * sizeof(mblur_acc));
    mblur_acc *seed = malloc(MAXN * sizeof(mblur_acc));
    uint8_t *dst_ref = malloc(MAXN);
    uint8_t *dst_got = malloc(MAXN);
    uint16_t *oetf = malloc(MBLUR_OETF_ENTRIES * sizeof(uint16_t));

    mblur_dither dither;
    mblur_build_dither(&dither, 1);
    mblur_build_oetf_table(oetf, MBLUR_TRC_SRGB, 1);

    mblur_tap_lut lut;
    const mblur_plane_info rgba = {0, 0, 4, 3};
    mblur_build_tap_lut(&lut, 0.125f, MBLUR_TRC_SRGB, 1, &rgba);

    for (size_t ci = 0; ci < sizeof(k_candidates) / sizeof(k_candidates[0]);
         ci++) {
        const mblur_backend b = k_candidates[ci];
        if (!mblur_backend_available(b))
            continue; /* not supported here; skipping is the honest result */

        const mblur_kernels *k = mblur_kernels_for(b);
        const mblur_kernels *s = &mblur_kernels_scalar;

        char label[96];
        snprintf(label, sizeof(label), "%s matches scalar bit for bit", k->name);
        MBT_CASE(label);

        for (size_t si = 0; si < sizeof(k_sizes) / sizeof(k_sizes[0]); si++) {
            const size_t n = k_sizes[si];

            for (int trial = 0; trial < 4; trial++) {
                mbtest_fill_random(src, n);
                for (size_t i = 0; i < n; i++)
                    seed[i] = (mblur_acc)(mbtest_rand() >> 20); /* 0..4095 */

                const uint16_t w = (uint16_t)(2048 + (mbtest_rand() & 0x3FFF));

                /* accum_mul */
                memcpy(ref, seed, n * sizeof(mblur_acc));
                memcpy(got, seed, n * sizeof(mblur_acc));
                s->accum_mul(ref, src, n, w);
                k->accum_mul(got, src, n, w);
                MBT_CHECK(memcmp(ref, got, n * sizeof(mblur_acc)) == 0,
                          "%s accum_mul differs at n=%zu", k->name, n);

                /* accum_mul_sub */
                memcpy(ref, seed, n * sizeof(mblur_acc));
                memcpy(got, seed, n * sizeof(mblur_acc));
                s->accum_mul_sub(ref, src, n, w);
                k->accum_mul_sub(got, src, n, w);
                MBT_CHECK(memcmp(ref, got, n * sizeof(mblur_acc)) == 0,
                          "%s accum_mul_sub differs at n=%zu", k->name, n);

                /* accum_lut, both channel layouts */
                for (uint32_t ch = 1; ch <= 4; ch += 3) {
                    memcpy(ref, seed, n * sizeof(mblur_acc));
                    memcpy(got, seed, n * sizeof(mblur_acc));
                    s->accum_lut(ref, src, n, lut.table, ch);
                    k->accum_lut(got, src, n, lut.table, ch);
                    MBT_CHECK(memcmp(ref, got, n * sizeof(mblur_acc)) == 0,
                              "%s accum_lut(ch=%u) differs at n=%zu", k->name,
                              ch, n);
                }

                /* resolve, at several dither phases - the vector path loads a
                 * window from the duplicated table and must agree with the
                 * scalar indexing at every offset. */
                for (size_t base = 0; base < 70; base += 13) {
                    memset(dst_ref, 0xAA, n);
                    memset(dst_got, 0x55, n);
                    s->resolve_direct(dst_ref, seed, n, &dither, base);
                    k->resolve_direct(dst_got, seed, n, &dither, base);
                    MBT_CHECK(memcmp(dst_ref, dst_got, n) == 0,
                              "%s resolve_direct differs at n=%zu base=%zu",
                              k->name, n, base);

                    memset(dst_ref, 0xAA, n);
                    memset(dst_got, 0x55, n);
                    s->resolve_oetf(dst_ref, seed, n, oetf, 3, &dither, base);
                    k->resolve_oetf(dst_got, seed, n, oetf, 3, &dither, base);
                    MBT_CHECK(memcmp(dst_ref, dst_got, n) == 0,
                              "%s resolve_oetf differs at n=%zu base=%zu",
                              k->name, n, base);
                }
            }
        }
        MBT_DONE();
    }

    MBT_CASE("dither is zero-mean and bounded");
    {
        mblur_dither d;
        mblur_build_dither(&d, 1);
        long sum = 0;
        for (uint32_t i = 0; i < MBLUR_DITHER_PERIOD; i++) {
            MBT_CHECK(!(d.pos[i] && d.neg[i]),
                      "entry %u has both a positive and negative part", i);
            MBT_CHECK(d.pos[i] < 256 && d.neg[i] < 256,
                      "entry %u exceeds one output LSB", i);
            sum += (long)d.pos[i] - (long)d.neg[i];
        }
        /* A bias here would shift the whole picture's brightness. */
        MBT_CHECK(sum == 0, "dither is biased by %ld counts", sum);

        for (uint32_t i = MBLUR_DITHER_PERIOD; i < MBLUR_DITHER_ENTRIES; i++) {
            MBT_CHECK(d.pos[i] == d.pos[i - MBLUR_DITHER_PERIOD] &&
                          d.neg[i] == d.neg[i - MBLUR_DITHER_PERIOD],
                      "the duplicated half diverges at %u", i);
        }

        mblur_build_dither(&d, 0);
        for (uint32_t i = 0; i < MBLUR_DITHER_ENTRIES; i++)
            MBT_CHECK(d.pos[i] == 0 && d.neg[i] == 0,
                      "disabled dither is not zero at %u", i);
    }
    MBT_DONE();

    MBT_CASE("transfer functions round-trip");
    {
        for (int v = 0; v <= 255; v++) {
            double e = v / 255.0;
            for (int t = 0; t < MBLUR_TRC_COUNT; t++) {
                double round = mblur_oetf(mblur_eotf(e, (mblur_transfer)t),
                                          (mblur_transfer)t);
                MBT_CHECK(fabs(round - e) < 1e-6,
                          "trc %d did not round-trip at %d (%.9f)", t, v, round);
            }
        }
        /* The classic sanity check: mid-grey sRGB is about 21.6% of full
         * intensity, not 50%. If this ever reads 0.5 the EOTF is a no-op and
         * every blend is happening in the wrong space. */
        MBT_CHECK(fabs(mblur_eotf(0.5, MBLUR_TRC_SRGB) - 0.2140) < 0.002,
                  "sRGB EOTF at 0.5 = %.4f", mblur_eotf(0.5, MBLUR_TRC_SRGB));
    }
    MBT_DONE();

    free(src); free(ref); free(got); free(seed);
    free(dst_ref); free(dst_got); free(oetf);
    return mbtest_report("test_kernels");
}
