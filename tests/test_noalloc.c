/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The steady-state frame path must not allocate. A malloc inside a realtime
 * filter is a frame drop waiting for the wrong moment, so this is checked
 * mechanically rather than by reading the code: the allocator is wrapped at
 * link time and every call is counted while frames are in flight.
 */
#include "mblur/mblur.h"
#include "mbtest.h"

static int g_watching;
static long g_allocs;

void *__real_malloc(size_t);
void *__real_calloc(size_t, size_t);
void *__real_realloc(void *, size_t);
int __real_posix_memalign(void **, size_t, size_t);

void *__wrap_malloc(size_t n)
{
    if (g_watching)
        g_allocs++;
    return __real_malloc(n);
}

void *__wrap_calloc(size_t n, size_t s)
{
    if (g_watching)
        g_allocs++;
    return __real_calloc(n, s);
}

void *__wrap_realloc(void *p, size_t n)
{
    if (g_watching)
        g_allocs++;
    return __real_realloc(p, n);
}

int __wrap_posix_memalign(void **p, size_t a, size_t n)
{
    if (g_watching)
        g_allocs++;
    return __real_posix_memalign(p, a, n);
}

int main(void)
{
    printf("test_noalloc\n");

    const uint32_t w = 128, h = 64;
    const size_t bytes = (size_t)w * h * 4u;
    uint8_t *buf = malloc(bytes);

    const struct {
        mblur_mode mode;
        mblur_weighting weighting;
        uint32_t threads;
        const char *label;
    } cases[] = {
        {MBLUR_MODE_DECIMATE, MBLUR_W_GAUSSIAN, 1, "decimate, single-threaded"},
        {MBLUR_MODE_DECIMATE, MBLUR_W_EQUAL, 4, "decimate, 4 threads"},
        {MBLUR_MODE_ROLLING, MBLUR_W_EQUAL, 1, "rolling, sliding window"},
        {MBLUR_MODE_ROLLING, MBLUR_W_PYRAMID, 4, "rolling, re-summed"},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char label[96];
        snprintf(label, sizeof(label), "no allocation: %s", cases[i].label);
        MBT_CASE(label);

        mblur_config cfg;
        mblur_config_defaults(&cfg);
        cfg.width = w;
        cfg.height = h;
        cfg.frames = 6;
        cfg.mode = cases[i].mode;
        cfg.weighting = cases[i].weighting;
        cfg.threads = cases[i].threads;

        mblur_ctx *c = mblur_create(&cfg, MBLUR_BACKEND_AUTO, NULL);
        MBT_CHECK(c != NULL, "create failed");

        /* Warm the pipeline (and stdio) before the allocator is watched. */
        for (int f = 0; f < 12; f++) {
            memset(buf, f, bytes);
            mblur_frame in;
            memset(&in, 0, sizeof(in));
            in.plane[0] = buf;
            in.stride[0] = (size_t)w * 4u;
            mblur_submit(c, &in);
            mblur_frame out;
            mblur_pull(c, &out);
        }

        g_allocs = 0;
        g_watching = 1;
        for (int f = 0; f < 60; f++) {
            memset(buf, f, bytes);
            mblur_frame in;
            memset(&in, 0, sizeof(in));
            in.plane[0] = buf;
            in.stride[0] = (size_t)w * 4u;
            mblur_submit(c, &in);
            mblur_frame out;
            mblur_pull(c, &out);
        }
        g_watching = 0;

        MBT_CHECK(g_allocs == 0, "%ld allocation(s) during 60 frames",
                  g_allocs);
        mblur_destroy(c);
        MBT_DONE();
    }

    free(buf);
    return mbtest_report("test_noalloc");
}
