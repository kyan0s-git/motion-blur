/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Argument parsing.
 *
 * Option names follow blur's vocabulary where one exists - blur-amount,
 * blur-weighting, interpolated-fps, input-timescale - so a config someone
 * already has in their head transfers. Unknown values are errors rather
 * than silent fallbacks: a typo in a weighting name should not quietly
 * change the look of a render that takes an hour.
 */
#include "options.h"

#include <stdlib.h>
#include <string.h>

void blur_options_defaults(blur_options *o)
{
    memset(o, 0, sizeof(*o));
    o->format = MBLUR_FMT_NV12;
    o->frames = 4;
    o->mode = MBLUR_MODE_DECIMATE;
    o->weighting = MBLUR_W_EQUAL;
    o->amount = 1.0f;
    o->gauss_std = 2.0f;
    o->gauss_bound = 2.0f;
    o->linear_light = 0; /* NV12 cannot be blended in linear light */
    o->dither = 1;
    o->transfer = MBLUR_TRC_BT709;
    o->backend = MBLUR_BACKEND_AUTO;
    o->interp_mode = "mci";
    o->crf = 18;
    o->encoder = "libx264";
    o->preset = "medium";
    o->copy_audio = 1;
    o->input_timescale = 1.0;
    o->output_timescale = 1.0;
    o->ffmpeg = "ffmpeg";
    o->ffprobe = "ffprobe";
}

void blur_usage(FILE *out, const char *argv0)
{
    fprintf(out,
"blurcli %s - temporal motion blur for video files\n"
"\n"
"usage:\n"
"  %s --input IN --output OUT [options]      blur a file (needs ffmpeg)\n"
"  %s --raw --width W --height H [options]   blur raw frames on a pipe\n"
"\n"
"Motion blur needs more temporal samples than it emits. There are two ways\n"
"to get them, and picking one is the whole job:\n"
"\n"
"  * Source already runs fast (a 240 fps capture you want at 60):\n"
"      --frames 4                      blends every 4 frames into 1\n"
"\n"
"  * Source runs at the rate you want to keep (60 fps in, 60 fps out):\n"
"      --interpolate 8 --frames 8      synthesise 480 fps, blend back to 60\n"
"    Without interpolation, 60 -> 60 can only produce a rolling smear\n"
"    (--mode rolling), not a real shutter integral.\n"
"\n"
"input / output\n"
"  --input FILE            source file, decoded through ffmpeg\n"
"  --output FILE           destination file, encoded through ffmpeg\n"
"  --raw                   read/write raw frames instead of containers\n"
"  --raw-in FILE           raw input (default: stdin)\n"
"  --raw-out FILE          raw output (default: stdout)\n"
"  --width N  --height N   frame size (required for --raw)\n"
"  --fps F                 input frame rate (probed from --input if omitted)\n"
"  --format NAME           rgba8, bgra8, nv12 (default), i420\n"
"\n"
"blur\n"
"  --frames N              frames per blur window (default 4)\n"
"  --mode MODE             decimate (default) or rolling\n"
"  --blur-weighting NAME   equal, gaussian, gaussian_sym, gaussian_reverse,\n"
"                          pyramid, vegas, ascending, descending, custom\n"
"  --custom-weights CSV    weights for --blur-weighting custom\n"
"  --blur-amount F         shutter angle; <1 crisper, >1 ghosting (default 1)\n"
"  --gaussian-std F        gaussian standard deviation (default 2)\n"
"  --gaussian-mean F       gaussian centre (default 0)\n"
"  --gaussian-bound F      gaussian extent (default 2)\n"
"  --phase-offset N        shift where decimate windows start\n"
"  --linear-light          blend in linear light (RGB formats only)\n"
"  --no-dither             disable output dithering\n"
"  --transfer NAME         srgb, bt709 (default), linear\n"
"\n"
"interpolation (before blending, via ffmpeg minterpolate)\n"
"  --interpolate N         multiply the frame rate by N first\n"
"  --interpolated-fps F    interpolate to an absolute rate instead\n"
"  --interpolation-mode M  minterpolate mi_mode: mci (default), blend, dup\n"
"\n"
"encoding\n"
"  --quality N             CRF, 0 lossless to 51 (default 18)\n"
"  --encoder NAME          video encoder (default libx264)\n"
"  --preset NAME           encoder preset (default medium)\n"
"  --filters STR           extra ffmpeg -vf filters after the blur\n"
"  --no-audio              drop audio instead of copying it\n"
"  --input-timescale F     speed factor applied before blurring\n"
"  --output-timescale F    speed factor applied to the result\n"
"\n"
"engine\n"
"  --threads N             worker threads (0 = one per core)\n"
"  --backend NAME          auto (default), scalar, sse2, avx2, avx512\n"
"\n"
"other\n"
"  --ffmpeg PATH           ffmpeg binary (default: ffmpeg on PATH)\n"
"  --ffprobe PATH          ffprobe binary\n"
"  --dry-run               print the ffmpeg commands and exit\n"
"  -v, --verbose           report progress\n"
"  -h, --help              this text\n",
        mblur_version_string(), argv0, argv0);
}

#define NEED_VALUE()                                                       \
    do {                                                                   \
        if (i + 1 >= argc) {                                               \
            fprintf(stderr, "blurcli: %s needs a value\n", argv[i]);       \
            return -1;                                                     \
        }                                                                  \
    } while (0)

int blur_options_parse(blur_options *o, int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];

        if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            blur_usage(stdout, argv[0]);
            return 1;
        } else if (!strcmp(a, "-v") || !strcmp(a, "--verbose")) {
            o->verbose = 1;
        } else if (!strcmp(a, "--dry-run")) {
            o->dry_run = 1;
        } else if (!strcmp(a, "--raw")) {
            o->raw = 1;
        } else if (!strcmp(a, "--linear-light")) {
            o->linear_light = 1;
        } else if (!strcmp(a, "--no-dither")) {
            o->dither = 0;
        } else if (!strcmp(a, "--no-audio")) {
            o->copy_audio = 0;
        } else if (!strcmp(a, "--input")) {
            NEED_VALUE();
            o->input = argv[++i];
        } else if (!strcmp(a, "--output")) {
            NEED_VALUE();
            o->output = argv[++i];
        } else if (!strcmp(a, "--raw-in")) {
            NEED_VALUE();
            o->raw_in = argv[++i];
        } else if (!strcmp(a, "--raw-out")) {
            NEED_VALUE();
            o->raw_out = argv[++i];
        } else if (!strcmp(a, "--width")) {
            NEED_VALUE();
            o->width = (uint32_t)atoi(argv[++i]);
        } else if (!strcmp(a, "--height")) {
            NEED_VALUE();
            o->height = (uint32_t)atoi(argv[++i]);
        } else if (!strcmp(a, "--fps")) {
            NEED_VALUE();
            o->fps = atof(argv[++i]);
        } else if (!strcmp(a, "--format")) {
            NEED_VALUE();
            const int f = mblur_format_parse(argv[++i]);
            if (f < 0) {
                fprintf(stderr, "blurcli: unknown format '%s'\n", argv[i]);
                return -1;
            }
            o->format = (mblur_format)f;
        } else if (!strcmp(a, "--frames")) {
            NEED_VALUE();
            o->frames = (uint32_t)atoi(argv[++i]);
        } else if (!strcmp(a, "--mode")) {
            NEED_VALUE();
            const int m = mblur_mode_parse(argv[++i]);
            if (m < 0) {
                fprintf(stderr, "blurcli: unknown mode '%s'\n", argv[i]);
                return -1;
            }
            o->mode = (mblur_mode)m;
        } else if (!strcmp(a, "--blur-weighting") || !strcmp(a, "--weighting")) {
            NEED_VALUE();
            const int w = mblur_weighting_parse(argv[++i]);
            if (w < 0) {
                fprintf(stderr, "blurcli: unknown weighting '%s'\n", argv[i]);
                return -1;
            }
            o->weighting = (mblur_weighting)w;
        } else if (!strcmp(a, "--custom-weights")) {
            NEED_VALUE();
            o->custom_weights = argv[++i];
            o->weighting = MBLUR_W_CUSTOM;
        } else if (!strcmp(a, "--blur-amount") || !strcmp(a, "--amount")) {
            NEED_VALUE();
            o->amount = (float)atof(argv[++i]);
        } else if (!strcmp(a, "--gaussian-std")) {
            NEED_VALUE();
            o->gauss_std = (float)atof(argv[++i]);
        } else if (!strcmp(a, "--gaussian-mean")) {
            NEED_VALUE();
            o->gauss_mean = (float)atof(argv[++i]);
        } else if (!strcmp(a, "--gaussian-bound")) {
            NEED_VALUE();
            o->gauss_bound = (float)atof(argv[++i]);
        } else if (!strcmp(a, "--phase-offset")) {
            NEED_VALUE();
            o->phase_offset = (uint32_t)atoi(argv[++i]);
        } else if (!strcmp(a, "--transfer")) {
            NEED_VALUE();
            const int t = mblur_transfer_parse(argv[++i]);
            if (t < 0) {
                fprintf(stderr, "blurcli: unknown transfer '%s'\n", argv[i]);
                return -1;
            }
            o->transfer = (mblur_transfer)t;
        } else if (!strcmp(a, "--interpolate")) {
            NEED_VALUE();
            o->interpolate = (uint32_t)atoi(argv[++i]);
        } else if (!strcmp(a, "--interpolated-fps")) {
            NEED_VALUE();
            o->interpolated_fps = atof(argv[++i]);
        } else if (!strcmp(a, "--interpolation-mode")) {
            NEED_VALUE();
            o->interp_mode = argv[++i];
        } else if (!strcmp(a, "--quality") || !strcmp(a, "--crf")) {
            NEED_VALUE();
            o->crf = atoi(argv[++i]);
        } else if (!strcmp(a, "--encoder")) {
            NEED_VALUE();
            o->encoder = argv[++i];
        } else if (!strcmp(a, "--preset")) {
            NEED_VALUE();
            o->preset = argv[++i];
        } else if (!strcmp(a, "--filters")) {
            NEED_VALUE();
            o->extra_filters = argv[++i];
        } else if (!strcmp(a, "--input-timescale")) {
            NEED_VALUE();
            o->input_timescale = atof(argv[++i]);
        } else if (!strcmp(a, "--output-timescale")) {
            NEED_VALUE();
            o->output_timescale = atof(argv[++i]);
        } else if (!strcmp(a, "--threads")) {
            NEED_VALUE();
            o->threads = (uint32_t)atoi(argv[++i]);
        } else if (!strcmp(a, "--backend")) {
            NEED_VALUE();
            const char *name = argv[++i];
            int found = -1;
            for (int b = 0; b < MBLUR_BACKEND_COUNT; b++) {
                if (!strcmp(name, mblur_backend_name((mblur_backend)b))) {
                    found = b;
                    break;
                }
            }
            if (found < 0) {
                fprintf(stderr, "blurcli: unknown backend '%s'\n", name);
                return -1;
            }
            o->backend = (mblur_backend)found;
        } else if (!strcmp(a, "--ffmpeg")) {
            NEED_VALUE();
            o->ffmpeg = argv[++i];
        } else if (!strcmp(a, "--ffprobe")) {
            NEED_VALUE();
            o->ffprobe = argv[++i];
        } else {
            fprintf(stderr, "blurcli: unknown option '%s'\n", a);
            blur_usage(stderr, argv[0]);
            return -1;
        }
    }

    if (!o->raw && !o->input) {
        fprintf(stderr, "blurcli: need --input FILE or --raw\n");
        return -1;
    }
    if (o->raw && (o->width == 0 || o->height == 0)) {
        fprintf(stderr, "blurcli: --raw needs --width and --height\n");
        return -1;
    }
    if (o->input && !o->output && !o->dry_run) {
        fprintf(stderr, "blurcli: --input needs --output\n");
        return -1;
    }
    if (o->interpolate && o->interpolated_fps > 0.0) {
        fprintf(stderr, "blurcli: --interpolate and --interpolated-fps are "
                        "alternatives, not both\n");
        return -1;
    }
    if ((o->interpolate || o->interpolated_fps > 0.0) && o->raw) {
        fprintf(stderr, "blurcli: interpolation runs inside the ffmpeg decode "
                        "stage, so it needs --input rather than --raw\n");
        return -1;
    }

    return 0;
}
