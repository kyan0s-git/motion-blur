/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * mblur - a fast temporal motion blur engine.
 *
 * This is the whole public surface. It is deliberately C99 with a stable,
 * flat ABI so the OBS plugin, the offline CLI and the test suite can all
 * drive the same engine without agreeing on a C++ standard library.
 *
 * Usage is push/pull:
 *
 *     mblur_config cfg;
 *     mblur_config_defaults(&cfg);
 *     cfg.width = 1920; cfg.height = 1080; cfg.frames = 8;
 *     mblur_ctx *ctx = mblur_create(&cfg, MBLUR_BACKEND_AUTO, NULL);
 *
 *     while (have_input) {
 *         mblur_submit(ctx, &in);
 *         if (mblur_pull(ctx, &out) == MBLUR_OK)
 *             encode(&out);
 *     }
 *
 * In MBLUR_MODE_ROLLING every submit yields a frame (after the ring warms
 * up). In MBLUR_MODE_DECIMATE one frame comes out per `frames` submitted.
 * That single difference is what separates "blend the last N frames" from
 * "integrate a real shutter interval", and it is why decimate mode needs a
 * source running at N x the output frame rate.
 *
 * Every allocation happens in mblur_create(). Nothing in the submit/pull
 * path allocates, locks or spawns threads.
 */
#ifndef MBLUR_H
#define MBLUR_H

#include <stddef.h>
#include <stdint.h>

#if defined(__cplusplus)
extern "C" {
#endif

#if defined(_WIN32) && defined(MBLUR_SHARED)
#  if defined(MBLUR_BUILDING)
#    define MBLUR_API __declspec(dllexport)
#  else
#    define MBLUR_API __declspec(dllimport)
#  endif
#else
#  define MBLUR_API
#endif

#define MBLUR_VERSION_MAJOR 0
#define MBLUR_VERSION_MINOR 1
#define MBLUR_VERSION_PATCH 0

/* Hard cap on the blur window. 64 taps at 1080p RGBA is already 530 MB of
 * ring buffer; beyond this the D3D11 decoder array-slice limit bites too. */
#define MBLUR_MAX_FRAMES 64
#define MBLUR_MAX_PLANES 3

typedef enum mblur_status {
    MBLUR_OK = 0,
    MBLUR_NO_OUTPUT = 1,    /* not an error: the window is not complete yet */
    MBLUR_ERR_INVALID = -1, /* a config field or argument is out of range */
    MBLUR_ERR_MEMORY = -2,
    MBLUR_ERR_UNSUPPORTED = -3, /* valid request, no backend can serve it */
    MBLUR_ERR_BACKEND = -4      /* the backend itself failed (GPU lost, etc.) */
} mblur_status;

typedef enum mblur_format {
    MBLUR_FMT_RGBA8 = 0, /* packed 8-bit, 4 channels, alpha last */
    MBLUR_FMT_BGRA8,     /* packed 8-bit, 4 channels, what OBS/D3D hand you */
    MBLUR_FMT_NV12,      /* planar Y + interleaved UV, 4:2:0, 8-bit */
    MBLUR_FMT_I420,      /* planar Y + U + V, 4:2:0, 8-bit */
    MBLUR_FMT_COUNT
} mblur_format;

/*
 * Transfer function of the *input and output* frames. Blending happens in
 * linear light (see mblur_config.linear_light), so this tells the engine
 * what to undo on ingest and redo on resolve.
 */
typedef enum mblur_transfer {
    MBLUR_TRC_SRGB = 0, /* sRGB piecewise curve - the usual desktop case */
    MBLUR_TRC_BT709,    /* BT.709 / BT.1886 video gamma */
    MBLUR_TRC_LINEAR,   /* already linear; ingest and resolve are no-ops */
    MBLUR_TRC_COUNT
} mblur_transfer;

/*
 * Frame weighting kernels. Names match the vocabulary blur/danser users
 * already have, so configs port over without a translation table.
 */
typedef enum mblur_weighting {
    MBLUR_W_EQUAL = 0,     /* flat box filter - a true 360 degree shutter */
    MBLUR_W_GAUSSIAN,      /* gaussian centred by gauss_mean */
    MBLUR_W_GAUSSIAN_SYM,  /* gaussian forced symmetric about the centre */
    MBLUR_W_GAUSSIAN_REV,  /* gaussian mirrored: heavy at both ends */
    MBLUR_W_PYRAMID,       /* linear ramp up then down */
    MBLUR_W_VEGAS,         /* Vegas Pro's frame-blend shape: ends weighted */
    MBLUR_W_ASCENDING,     /* linear ramp, newest frame heaviest */
    MBLUR_W_DESCENDING,    /* linear ramp, oldest frame heaviest */
    MBLUR_W_CUSTOM,        /* caller supplies custom_weights[] */
    MBLUR_W_COUNT
} mblur_weighting;

typedef enum mblur_mode {
    /*
     * One output per input. Blends the trailing N frames, so each source
     * frame contributes to N outputs. Needs no reconfiguration anywhere,
     * but it is a smear, not a shutter integral. This is the honest
     * fallback when the source cannot be oversampled.
     */
    MBLUR_MODE_ROLLING = 0,
    /*
     * One output per N inputs, over disjoint windows. This is a real
     * shutter integral and the only physically correct mode - it requires
     * the source to run at N x the output rate.
     */
    MBLUR_MODE_DECIMATE
} mblur_mode;

typedef enum mblur_backend {
    MBLUR_BACKEND_AUTO = 0,
    MBLUR_BACKEND_CPU_SCALAR, /* portable reference; also the test oracle */
    MBLUR_BACKEND_CPU_SSE2,
    MBLUR_BACKEND_CPU_AVX2,
    MBLUR_BACKEND_CPU_AVX512,
    MBLUR_BACKEND_D3D11,      /* Windows only; used by the offline app */
    MBLUR_BACKEND_COUNT
} mblur_backend;

typedef struct mblur_frame {
    uint8_t *plane[MBLUR_MAX_PLANES];
    size_t stride[MBLUR_MAX_PLANES]; /* bytes per row, may exceed width */
    int64_t pts;                     /* opaque; carried through to output */
} mblur_frame;

typedef struct mblur_config {
    uint32_t width;
    uint32_t height;
    mblur_format format;
    mblur_transfer transfer;

    uint32_t frames;         /* taps in the blur window, 1..MBLUR_MAX_FRAMES */
    mblur_mode mode;
    mblur_weighting weighting;

    /*
     * Shutter angle, as a fraction of the window.
     *   1.0  - integrate the whole window (360 degree shutter)
     *   0.5  - integrate the middle half, crisper, more strobed
     *   >1.0 - the kernel is stretched past the window, deliberately
     *          ghosting, which is what blur's "blur amount" above 1 does
     * Taps outside the shutter get weight 0 and are skipped entirely, so a
     * small amount is also cheaper.
     */
    float amount;

    float gauss_std;   /* gaussian standard deviation (default 2.0) */
    float gauss_mean;  /* centre, in normalised window units (default 0.0) */
    float gauss_bound; /* +/- extent sampled from the gaussian (default 2.0) */

    const float *custom_weights; /* MBLUR_W_CUSTOM: copied during create */
    uint32_t custom_weight_count;

    /*
     * Blend in linear light. Averaging gamma-encoded values darkens the
     * trail because the sRGB curve is concave - a white object crossing a
     * dark background leaves grey mud instead of a bright streak. Leave
     * this on unless you are deliberately matching a tool that gets it
     * wrong. Ignored (and rejected) for the YUV formats, which can only be
     * blended in their own encoded space.
     */
    int linear_light;

    /* Triangular-PDF dither on resolve. High-precision accumulation into an
     * 8-bit output bands visibly in gradients without it. */
    int dither;

    /*
     * DECIMATE only. Which input frame index starts a window. OBS's encoder
     * frame-rate divisor keeps every Nth tick with its own phase; if it
     * disagrees with ours the result is one frame of extra latency, not
     * corruption, but this lets a user who can see it dial it out.
     */
    uint32_t phase_offset;

    uint32_t threads; /* 0 = pick from the machine; 1 = stay on this thread */
} mblur_config;

typedef struct mblur_ctx mblur_ctx;

/* ---------------------------------------------------------------- setup */

MBLUR_API void mblur_config_defaults(mblur_config *cfg);

/* Validates cfg and reports what is wrong. `reason` may be NULL; when
 * given it receives a static, human-readable string on failure. */
MBLUR_API mblur_status mblur_config_validate(const mblur_config *cfg,
                                             const char **reason);

/* `reason` may be NULL. On failure returns NULL and sets *reason. */
MBLUR_API mblur_ctx *mblur_create(const mblur_config *cfg,
                                  mblur_backend backend,
                                  const char **reason);

MBLUR_API void mblur_destroy(mblur_ctx *ctx);

/* Drops all buffered frames and restarts the window. Call this on a seek or
 * a scene cut; blending across a cut produces a very visible double image. */
MBLUR_API void mblur_reset(mblur_ctx *ctx);

/* ----------------------------------------------------------- processing */

/* Hands one source frame to the engine. The frame is consumed immediately -
 * the caller may reuse or free its memory as soon as this returns. */
MBLUR_API mblur_status mblur_submit(mblur_ctx *ctx, const mblur_frame *in);

/* Returns MBLUR_OK and fills `out` when a blended frame is ready, or
 * MBLUR_NO_OUTPUT when the window is still filling. */
MBLUR_API mblur_status mblur_pull(mblur_ctx *ctx, mblur_frame *out);

/* How many submits are still needed before the first pull can succeed.
 * Useful for priming a pipeline and for reporting honest latency. */
MBLUR_API uint32_t mblur_latency_frames(const mblur_ctx *ctx);

/* ------------------------------------------------------------- kernels */

/*
 * Generates the normalised weights for a config into `out` (which must hold
 * cfg->frames floats). Exposed because the GPU backends and the OBS effect
 * upload the same numbers to a shader, and because it is worth testing on
 * its own. The result always sums to 1 for any valid config.
 */
MBLUR_API mblur_status mblur_weights_generate(const mblur_config *cfg,
                                              float *out);

/* --------------------------------------------------------- introspection */

MBLUR_API const char *mblur_version_string(void);
MBLUR_API const char *mblur_status_string(mblur_status st);
MBLUR_API const char *mblur_backend_name(mblur_backend b);
MBLUR_API mblur_backend mblur_ctx_backend(const mblur_ctx *ctx);

/* Best CPU backend this machine can actually run. */
MBLUR_API mblur_backend mblur_cpu_best_backend(void);

/* Whether this machine can run a given backend. Callers that want to sweep
 * every implementation (the benchmark, the test suite) need to know what is
 * actually there rather than discovering it through a failed create. */
MBLUR_API int mblur_backend_available(mblur_backend b);

/* Bytes held by the context - ring, accumulator and scratch. Worth showing
 * in a UI: at 1440p with 16 taps this is not a small number. */
MBLUR_API size_t mblur_ctx_memory_usage(const mblur_ctx *ctx);

/* ------------------------------------------------------------- parsing */

/* Name <-> enum for config files and CLI flags. Parsers return -1 on an
 * unknown name so callers can report it rather than silently defaulting. */
MBLUR_API const char *mblur_weighting_name(mblur_weighting w);
MBLUR_API int mblur_weighting_parse(const char *name);
MBLUR_API const char *mblur_format_name(mblur_format f);
MBLUR_API int mblur_format_parse(const char *name);
MBLUR_API const char *mblur_transfer_name(mblur_transfer t);
MBLUR_API int mblur_transfer_parse(const char *name);
MBLUR_API const char *mblur_mode_name(mblur_mode m);
MBLUR_API int mblur_mode_parse(const char *name);

/*
 * Parses "0.1, 0.2, 0.4, 0.2, 0.1" into out[]. Returns the count written,
 * or -1 on a malformed list. Weights need not be normalised.
 */
MBLUR_API int mblur_weights_parse_csv(const char *csv, float *out,
                                      uint32_t max_out);

#if defined(__cplusplus)
} /* extern "C" */
#endif

#endif /* MBLUR_H */
