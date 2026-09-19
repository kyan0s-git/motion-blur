/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef BLURCLI_OPTIONS_H
#define BLURCLI_OPTIONS_H

#include "mblur/mblur.h"
#include <stdio.h>

typedef struct blur_options {
    /* Input selection: exactly one of these. */
    const char *input;  /* container file, decoded through ffmpeg */
    int raw;            /* raw frames on stdin/--raw-in instead */
    const char *raw_in;
    const char *raw_out;

    const char *output; /* container file, encoded through ffmpeg */

    uint32_t width, height;
    double fps;          /* input frame rate; probed when not given */
    mblur_format format;

    /* Blur */
    uint32_t frames;
    mblur_mode mode;
    mblur_weighting weighting;
    const char *custom_weights;
    float amount;
    float gauss_std, gauss_mean, gauss_bound;
    uint32_t phase_offset;
    int linear_light;
    int dither;
    mblur_transfer transfer;
    uint32_t threads;
    mblur_backend backend;

    /* Interpolation, applied before blending. The only way to get a real
     * shutter integral out of footage that was already captured at the rate
     * you want to keep. */
    uint32_t interpolate;      /* multiplier, 0 = off */
    double interpolated_fps;   /* absolute target, overrides the multiplier */
    const char *interp_mode;   /* ffmpeg minterpolate mi_mode */

    /* Encoding */
    int crf;
    const char *encoder;
    const char *preset;
    const char *extra_filters;
    int copy_audio;

    /* Speed changes, in blur's vocabulary. */
    double input_timescale;
    double output_timescale;

    const char *ffmpeg;  /* binary names, overridable */
    const char *ffprobe;

    int verbose;
    int dry_run;
} blur_options;

void blur_options_defaults(blur_options *o);
int blur_options_parse(blur_options *o, int argc, char **argv);
void blur_usage(FILE *out, const char *argv0);

#endif /* BLURCLI_OPTIONS_H */
