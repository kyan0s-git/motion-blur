/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Transfer functions and the lookup tables built from them.
 *
 * Why tables: the input is 8-bit, so there are only 256 possible values per
 * channel. Folding the EOTF *and* the tap's weight into a 256-entry table
 * turns the hot loop into "load byte, load table, add" - no transcendentals,
 * no multiply, and no approximation error. The table is 512 bytes per
 * channel and stays resident in L1.
 */
#include "internal.h"
#include <math.h>

double mblur_eotf(double e, mblur_transfer trc)
{
    if (e <= 0.0)
        return 0.0;
    if (e >= 1.0)
        return 1.0;

    switch (trc) {
    case MBLUR_TRC_SRGB:
        return (e <= 0.04045) ? (e / 12.92)
                              : pow((e + 0.055) / 1.055, 2.4);
    case MBLUR_TRC_BT709:
        /* Inverse of the BT.709 OETF. */
        return (e < 0.081) ? (e / 4.5)
                           : pow((e + 0.099) / 1.099, 1.0 / 0.45);
    case MBLUR_TRC_LINEAR:
    default:
        return e;
    }
}

double mblur_oetf(double l, mblur_transfer trc)
{
    if (l <= 0.0)
        return 0.0;
    if (l >= 1.0)
        return 1.0;

    switch (trc) {
    case MBLUR_TRC_SRGB:
        return (l <= 0.0031308) ? (l * 12.92)
                                : (1.055 * pow(l, 1.0 / 2.4) - 0.055);
    case MBLUR_TRC_BT709:
        return (l < 0.018) ? (l * 4.5) : (1.099 * pow(l, 0.45) - 0.099);
    case MBLUR_TRC_LINEAR:
    default:
        return l;
    }
}

/*
 * Rounding the tap tables up would let their sum exceed the accumulator's
 * range: eight taps of 0.125 each round to 8192, and 8 * 8192 is 65536,
 * which wraps to zero - white frames would come out black. Truncating
 * guarantees sum(floor(w_i * x)) <= floor(sum(w_i) * x) <= 65535, at a cost
 * of at most N counts out of 65535, far below one 8-bit step.
 */
static uint16_t q16_floor(double v)
{
    if (v <= 0.0)
        return 0;
    if (v >= 1.0)
        return 65535;
    return (uint16_t)(v * 65535.0);
}

static uint16_t q16(double v)
{
    if (v <= 0.0)
        return 0;
    if (v >= 1.0)
        return 65535;
    return (uint16_t)(v * 65535.0 + 0.5);
}

void mblur_build_tap_lut(mblur_tap_lut *lut, float weight, mblur_transfer trc,
                         int linear_light, const mblur_plane_info *pi)
{
    for (uint32_t ch = 0; ch < 4; ch++) {
        /* Alpha is already linear - running it through the EOTF would make
         * transparent edges shift as they blur. */
        const int is_alpha = (pi->alpha_index >= 0 &&
                              ch == (uint32_t)pi->alpha_index);
        const int apply = linear_light && !is_alpha;

        for (uint32_t v = 0; v < MBLUR_LUT_ENTRIES; v++) {
            double e = (double)v / 255.0;
            double s = apply ? mblur_eotf(e, trc) : e;
            lut->table[ch * MBLUR_LUT_ENTRIES + v] =
                q16_floor((double)weight * s);
        }
    }
}

void mblur_build_oetf_table(uint16_t *table, mblur_transfer trc,
                            int linear_light)
{
    for (uint32_t a = 0; a < MBLUR_OETF_ENTRIES; a++) {
        double l = (double)a / 65535.0;
        double e = linear_light ? mblur_oetf(l, trc) : l;
        /* Q16 so that resolve can add a dither offset and shift by 8. */
        table[a] = q16(e);
    }
}
