/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Weight kernel generation.
 *
 * Every kernel is defined as a continuous shape over a centred coordinate
 * c in [-1, 1], sampled at the centre of each tap. That keeps the shapes
 * independent of the tap count: 8 taps and 32 taps of "gaussian" describe
 * the same curve at different resolutions, so changing the blur length does
 * not change the look.
 *
 * The shutter angle (cfg->amount) is applied as a domain scale:
 *
 *   amount < 1  narrows the shutter - taps outside +/-amount are dropped
 *               entirely (cheaper as well as crisper)
 *   amount = 1  integrate the whole window
 *   amount > 1  stretches the domain, flattening the curve towards equal
 *               weighting, which is the deliberate ghosting that blur's
 *               "blur amount" above 1 produces
 */
#include "internal.h"
#include <math.h>

static double shape_gaussian(double c, double std, double mean, double bound)
{
    double x = c * bound - mean;
    if (std <= 1e-6)
        return (fabs(x) < 1e-6) ? 1.0 : 0.0;
    return exp(-0.5 * (x * x) / (std * std));
}

mblur_status mblur_weights_generate(const mblur_config *cfg, float *out)
{
    if (!cfg || !out || cfg->frames == 0 || cfg->frames > MBLUR_MAX_FRAMES)
        return MBLUR_ERR_INVALID;

    const uint32_t n = cfg->frames;
    const double amount = (cfg->amount > 1e-4) ? (double)cfg->amount : 1e-4;
    const double std = (cfg->gauss_std > 0.0f) ? (double)cfg->gauss_std : 2.0;
    const double mean = (double)cfg->gauss_mean;
    const double bound = (cfg->gauss_bound > 0.0f) ? (double)cfg->gauss_bound : 2.0;

    double raw[MBLUR_MAX_FRAMES];

    if (cfg->weighting == MBLUR_W_CUSTOM) {
        if (!cfg->custom_weights || cfg->custom_weight_count == 0)
            return MBLUR_ERR_INVALID;
        /* Resample the supplied list onto n taps so a 5-entry list still
         * means something at 16 taps, rather than being an error. */
        const uint32_t m = cfg->custom_weight_count;
        for (uint32_t i = 0; i < n; i++) {
            if (m == n) {
                raw[i] = (double)cfg->custom_weights[i];
            } else if (n == 1) {
                raw[i] = (double)cfg->custom_weights[m / 2];
            } else {
                double pos = (double)i * (double)(m - 1) / (double)(n - 1);
                uint32_t lo = (uint32_t)pos;
                uint32_t hi = (lo + 1 < m) ? lo + 1 : m - 1;
                double frac = pos - (double)lo;
                raw[i] = (double)cfg->custom_weights[lo] * (1.0 - frac) +
                         (double)cfg->custom_weights[hi] * frac;
            }
            if (raw[i] < 0.0)
                raw[i] = 0.0;
        }
    } else {
        for (uint32_t i = 0; i < n; i++) {
            /* Centre of tap i mapped into [-1, 1], then scaled by the
             * shutter angle. */
            double u = ((double)i + 0.5) / (double)n;
            double c = 2.0 * u - 1.0;

            if (fabs(c) > amount) {
                raw[i] = 0.0; /* outside the shutter */
                continue;
            }
            double cs = c / amount;

            switch (cfg->weighting) {
            case MBLUR_W_EQUAL:
                raw[i] = 1.0;
                break;
            case MBLUR_W_PYRAMID:
                raw[i] = 1.0 - fabs(cs);
                break;
            case MBLUR_W_ASCENDING:
                raw[i] = (cs + 1.0) * 0.5;
                break;
            case MBLUR_W_DESCENDING:
                raw[i] = (1.0 - cs) * 0.5;
                break;
            case MBLUR_W_GAUSSIAN:
                raw[i] = shape_gaussian(cs, std, mean, bound);
                break;
            case MBLUR_W_GAUSSIAN_SYM:
                raw[i] = shape_gaussian(cs, std, 0.0, bound);
                break;
            case MBLUR_W_GAUSSIAN_REV:
                /* Mirrored: the centre is suppressed and the ends carry the
                 * energy, which reads as a double-exposure rather than a
                 * streak. The floor keeps the kernel from collapsing to
                 * zero when std is large. */
                raw[i] = 1.0 - shape_gaussian(cs, std, mean, bound) + 1e-3;
                break;
            case MBLUR_W_VEGAS:
                /* Trapezoid: flat interior with half-weight end taps, the
                 * shape Vegas' frame blending produces. */
                raw[i] = (i == 0 || i == n - 1) ? 0.5 : 1.0;
                break;
            default:
                return MBLUR_ERR_INVALID;
            }

            if (raw[i] < 0.0)
                raw[i] = 0.0;
        }
    }

    double sum = 0.0;
    for (uint32_t i = 0; i < n; i++)
        sum += raw[i];

    if (!(sum > 0.0)) {
        /* Degenerate parameters (e.g. a vanishing shutter) would otherwise
         * produce a black frame. Fall back to the centre tap: the image
         * passes through unblurred, which is the sane failure. */
        for (uint32_t i = 0; i < n; i++)
            out[i] = 0.0f;
        out[n / 2] = 1.0f;
        return MBLUR_OK;
    }

    /*
     * Straight normalisation, with no residual pushed into any one tap.
     * Nudging a single weight to make the floats sum to exactly 1 would
     * break the symmetry a flat kernel is supposed to have - identical taps
     * would no longer be bitwise identical, and rolling mode's sliding
     * window (which relies on every tap sharing one table) would quietly
     * diverge from the same blur computed in decimate mode. The leftover is
     * around 1e-7, and the tap tables truncate rather than round, so the
     * accumulator cannot overflow regardless.
     */
    for (uint32_t i = 0; i < n; i++)
        out[i] = (float)(raw[i] / sum);

    return MBLUR_OK;
}
