/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Per-frame cost, measured rather than asserted.
 *
 * The numbers this prints are the project's actual performance claim, so it
 * reports the cost per *input* frame (what the realtime path has to absorb
 * every tick) alongside the cost per *output* frame, which are the same
 * thing in rolling mode and differ by the blur length in decimate mode.
 *
 * With --budget-ns it doubles as a CI gate: a non-zero exit means something
 * regressed.
 */
#include "mblur/mblur.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static double now_seconds(void)
{
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)f.QuadPart;
}
#else
#include <time.h>
static double now_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}
#endif

typedef struct {
    const char *label;
    uint32_t width, height;
} resolution;

static const resolution k_res[] = {
    {"720p", 1280, 720},
    {"1080p", 1920, 1080},
    {"1440p", 2560, 1440},
};

static uint32_t rng = 0x2545F491u;
static uint32_t next_rand(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

typedef struct {
    double ns_per_input;
    double ns_per_output;
    double gbps;
    uint32_t outputs;
} result;

/* Bytes a frame of this format occupies, which is also the traffic figure
 * the throughput column is computed from. */
static size_t frame_bytes(mblur_format fmt, uint32_t w, uint32_t h)
{
    switch (fmt) {
    case MBLUR_FMT_NV12:
    case MBLUR_FMT_I420:
        return (size_t)w * h * 3u / 2u;
    default:
        return (size_t)w * h * 4u;
    }
}

static void frame_setup(mblur_frame *f, mblur_format fmt, uint32_t w,
                        uint32_t h, uint8_t *base)
{
    memset(f, 0, sizeof(*f));
    f->plane[0] = base;
    if (fmt == MBLUR_FMT_NV12) {
        f->stride[0] = w;
        f->plane[1] = base + (size_t)w * h;
        f->stride[1] = w;
    } else if (fmt == MBLUR_FMT_I420) {
        f->stride[0] = w;
        f->plane[1] = base + (size_t)w * h;
        f->stride[1] = w / 2u;
        f->plane[2] = f->plane[1] + (size_t)(w / 2u) * (h / 2u);
        f->stride[2] = w / 2u;
    } else {
        f->stride[0] = (size_t)w * 4u;
    }
}

static result run_case(const mblur_config *base, mblur_backend backend,
                       uint32_t frames_to_push, size_t bytes_per_frame,
                       uint8_t **frames, uint32_t frame_count, int *ok)
{
    result r;
    memset(&r, 0, sizeof(r));
    *ok = 0;

    const char *why = NULL;
    mblur_ctx *c = mblur_create(base, backend, &why);
    if (!c) {
        fprintf(stderr, "  create failed: %s\n", why ? why : "?");
        return r;
    }

    mblur_frame in;

    /* Warm up: fault in the accumulator and settle the clocks. */
    for (uint32_t i = 0; i < frames_to_push / 4 + 8; i++) {
        frame_setup(&in, base->format, base->width, base->height,
                    frames[i % frame_count]);
        mblur_submit(c, &in);
        mblur_frame out;
        mblur_pull(c, &out);
    }

    uint32_t outputs = 0;
    const double t0 = now_seconds();
    for (uint32_t i = 0; i < frames_to_push; i++) {
        frame_setup(&in, base->format, base->width, base->height,
                    frames[i % frame_count]);
        in.pts = (int64_t)i;
        mblur_submit(c, &in);
        mblur_frame out;
        if (mblur_pull(c, &out) == MBLUR_OK) {
            outputs++;
            /* Touch the result so nothing can be optimised away. */
            rng += out.plane[0][0];
        }
    }
    const double elapsed = now_seconds() - t0;

    r.ns_per_input = elapsed * 1e9 / (double)frames_to_push;
    r.ns_per_output = outputs ? (elapsed * 1e9 / (double)outputs) : 0.0;
    r.outputs = outputs;
    r.gbps = ((double)bytes_per_frame * (double)frames_to_push) / elapsed / 1e9;

    mblur_destroy(c);
    *ok = 1;
    return r;
}

int main(int argc, char **argv)
{
    int check_budgets = 0;
    double budget_scale = 1.0;
    const char *only_res = NULL;
    uint32_t threads = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--check-budgets"))
            check_budgets = 1;
        else if (!strcmp(argv[i], "--budget-scale") && i + 1 < argc)
            budget_scale = atof(argv[++i]);
        else if (!strcmp(argv[i], "--resolution") && i + 1 < argc)
            only_res = argv[++i];
        else if (!strcmp(argv[i], "--threads") && i + 1 < argc)
            threads = (uint32_t)atoi(argv[++i]);
        else {
            printf("usage: %s [--check-budgets] [--budget-scale F] "
                   "[--resolution 1080p] [--threads N]\n"
                   "\n"
                   "  --check-budgets  exit non-zero if any path at 1080p on "
                   "the best\n"
                   "                   backend exceeds its own budget\n"
                   "  --budget-scale   multiply every budget, for slower "
                   "machines\n",
                   argv[0]);
            return 2;
        }
    }

    printf("mblur %s benchmark\n", mblur_version_string());
    printf("best backend here: %s\n\n",
           mblur_backend_name(mblur_cpu_best_backend()));

    const mblur_backend backends[] = {
        MBLUR_BACKEND_CPU_SCALAR, MBLUR_BACKEND_CPU_SSE2,
        MBLUR_BACKEND_CPU_AVX2, MBLUR_BACKEND_CPU_AVX512};

    /*
     * Each case carries its own budget, because one number cannot describe
     * both a path that folds one frame into an accumulator and a path that
     * re-sums N taps per output frame. Sharing a budget means either the
     * fast path is never really checked, or the O(N) path fails for being
     * exactly what it is defined to be.
     *
     * The numbers are roughly twice the slowest measurement seen across CI
     * runners - the floor is a shared arm64 macOS runner with no SIMD at
     * all - so they catch an algorithmic regression without failing on the
     * timing noise of a busy hosted machine. Use --budget-scale on hardware
     * slower than that.
     */
    const struct {
        mblur_mode mode;
        mblur_weighting weighting;
        int linear;
        mblur_format format;
        const char *label;
        double budget_ns;
    } cases[] = {
        /* The two that matter: NV12 decimate is what the offline encoder
         * path runs, RGBA linear is the quality default. */
        {MBLUR_MODE_DECIMATE, MBLUR_W_EQUAL, 0, MBLUR_FMT_NV12,
         "decimate nv12", 1500000.0},
        {MBLUR_MODE_DECIMATE, MBLUR_W_GAUSSIAN_SYM, 0, MBLUR_FMT_NV12,
         "decimate nv12 w", 1500000.0},
        {MBLUR_MODE_DECIMATE, MBLUR_W_EQUAL, 0, MBLUR_FMT_RGBA8,
         "decimate gamma", 4000000.0},
        {MBLUR_MODE_DECIMATE, MBLUR_W_GAUSSIAN_SYM, 1, MBLUR_FMT_RGBA8,
         "decimate linear", 8000000.0},
        {MBLUR_MODE_ROLLING, MBLUR_W_EQUAL, 0, MBLUR_FMT_RGBA8,
         "rolling slide", 20000000.0},
        /* N passes per output frame by construction; it exists as the
         * arbitrary-weight fallback, not as a fast path. */
        {MBLUR_MODE_ROLLING, MBLUR_W_GAUSSIAN_SYM, 1, MBLUR_FMT_RGBA8,
         "rolling resum", 70000000.0},
    };

    int over_budget = 0;

    for (size_t ri = 0; ri < sizeof(k_res) / sizeof(k_res[0]); ri++) {
        const resolution *res = &k_res[ri];
        if (only_res && strcmp(only_res, res->label))
            continue;

        const size_t bytes = (size_t)res->width * res->height * 4u;
        enum { FRAME_COUNT = 4 };
        uint8_t *frames[FRAME_COUNT];
        for (int i = 0; i < FRAME_COUNT; i++) {
            frames[i] = malloc(bytes);
            if (!frames[i]) {
                fprintf(stderr, "out of memory\n");
                return 1;
            }
            for (size_t j = 0; j < bytes; j += 4) {
                const uint32_t v = next_rand();
                frames[i][j + 0] = (uint8_t)v;
                frames[i][j + 1] = (uint8_t)(v >> 8);
                frames[i][j + 2] = (uint8_t)(v >> 16);
                frames[i][j + 3] = 255;
            }
        }

        printf("== %s (%ux%u, %.1f MB/frame) ==\n", res->label, res->width,
               res->height, (double)bytes / 1e6);
        printf("%-16s %-8s %3s  %10s %10s %8s", "case", "backend", "N",
               "ns/in", "ns/out", "GB/s");
        printf(check_budgets ? "  %10s\n" : "\n", "budget");

        for (size_t ci = 0; ci < sizeof(cases) / sizeof(cases[0]); ci++) {
            for (uint32_t frames_n = 4; frames_n <= 16; frames_n *= 2) {
                for (size_t bi = 0; bi < sizeof(backends) / sizeof(backends[0]);
                     bi++) {
                    if (!mblur_backend_available(backends[bi]))
                        continue;
                    /* The scalar reference is only interesting once. */
                    if (backends[bi] == MBLUR_BACKEND_CPU_SCALAR &&
                        frames_n != 8)
                        continue;

                    mblur_config cfg;
                    mblur_config_defaults(&cfg);
                    cfg.width = res->width;
                    cfg.height = res->height;
                    cfg.frames = frames_n;
                    cfg.mode = cases[ci].mode;
                    cfg.weighting = cases[ci].weighting;
                    cfg.linear_light = cases[ci].linear;
                    cfg.format = cases[ci].format;
                    cfg.threads = threads;

                    int ok = 0;
                    const uint32_t push = (res->height >= 1440) ? 120u : 240u;
                    const size_t traffic =
                        frame_bytes(cases[ci].format, res->width, res->height);
                    result r = run_case(&cfg, backends[bi], push, traffic,
                                        frames, FRAME_COUNT, &ok);
                    if (!ok)
                        continue;

                    printf("%-16s %-8s %3u  %10.0f %10.0f %8.1f",
                           cases[ci].label, mblur_backend_name(backends[bi]),
                           frames_n, r.ns_per_input, r.ns_per_output, r.gbps);

                    /* Only the best backend at 1080p is gated: the other
                     * rows are there to show the shape of the cost, and
                     * holding the scalar reference to the same budget as a
                     * vectorised kernel would be meaningless. */
                    if (check_budgets) {
                        const double budget =
                            cases[ci].budget_ns * budget_scale;
                        printf("  %10.0f", budget);
                        if (backends[bi] == mblur_cpu_best_backend() &&
                            !strcmp(res->label, "1080p")) {
                            if (r.ns_per_input > budget) {
                                printf("  OVER BUDGET");
                                over_budget = 1;
                            }
                        }
                    }
                    printf("\n");
                }
            }
        }
        printf("\n");

        for (int i = 0; i < FRAME_COUNT; i++)
            free(frames[i]);
    }

    if (check_budgets) {
        printf("per-case budgets at 1080p on %s (scale %.2f) -> %s\n",
               mblur_backend_name(mblur_cpu_best_backend()), budget_scale,
               over_budget ? "FAILED" : "all met");
    }
    return over_budget ? 1 : 0;
}
