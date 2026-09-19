/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "mblur/mblur.h"
#include "mbtest.h"
#include <math.h>

#define W 64u
#define H 32u
#define RGBA_BYTES (W * H * 4u)

static void fill_flat(uint8_t *buf, uint8_t v)
{
    memset(buf, v, RGBA_BYTES);
}

static mblur_frame frame_of(uint8_t *buf, int64_t pts)
{
    mblur_frame f;
    memset(&f, 0, sizeof(f));
    f.plane[0] = buf;
    f.stride[0] = W * 4u;
    f.pts = pts;
    return f;
}

static uint64_t fnv1a(const uint8_t *p, size_t n)
{
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static void base_cfg(mblur_config *cfg, mblur_mode mode, uint32_t frames)
{
    mblur_config_defaults(cfg);
    cfg->width = W;
    cfg->height = H;
    cfg->format = MBLUR_FMT_RGBA8;
    cfg->frames = frames;
    cfg->mode = mode;
    cfg->dither = 0; /* exactness matters more than banding in tests */
    cfg->threads = 1;
}

int main(void)
{
    printf("test_pipeline\n");

    uint8_t *buf = malloc(RGBA_BYTES);
    uint8_t *buf2 = malloc(RGBA_BYTES);

    /* ------------------------------------------------------------ */
    MBT_CASE("decimate emits one frame per window");
    {
        mblur_config cfg;
        base_cfg(&cfg, MBLUR_MODE_DECIMATE, 4);
        const char *why = NULL;
        mblur_ctx *c = mblur_create(&cfg, MBLUR_BACKEND_AUTO, &why);
        MBT_CHECK(c != NULL, "create failed: %s", why ? why : "?");

        int emitted = 0;
        for (int i = 0; i < 20; i++) {
            fill_flat(buf, (uint8_t)(i * 7));
            mblur_frame in = frame_of(buf, i);
            MBT_CHECK(mblur_submit(c, &in) == MBLUR_OK, "submit %d failed", i);
            mblur_frame out;
            if (mblur_pull(c, &out) == MBLUR_OK) {
                emitted++;
                MBT_CHECK(out.pts == i, "pts %lld should be the window's last",
                          (long long)out.pts);
                MBT_CHECK(((i + 1) % 4) == 0,
                          "emitted on frame %d, which is not a boundary", i);
            }
        }
        MBT_CHECK(emitted == 5, "expected 5 outputs from 20 inputs, got %d",
                  emitted);
        mblur_destroy(c);
    }
    MBT_DONE();

    /* ------------------------------------------------------------ */
    MBT_CASE("rolling emits once the window is full");
    {
        mblur_config cfg;
        base_cfg(&cfg, MBLUR_MODE_ROLLING, 4);
        mblur_ctx *c = mblur_create(&cfg, MBLUR_BACKEND_AUTO, NULL);
        MBT_CHECK(c != NULL, "create failed");

        for (int i = 0; i < 10; i++) {
            fill_flat(buf, 128);
            mblur_frame in = frame_of(buf, i);
            mblur_submit(c, &in);
            mblur_frame out;
            const int got = (mblur_pull(c, &out) == MBLUR_OK);
            MBT_CHECK(got == (i >= 3), "frame %d: emitted=%d", i, got);
        }
        mblur_destroy(c);
    }
    MBT_DONE();

    /* ------------------------------------------------------------ */
    MBT_CASE("a constant input survives every kernel unchanged");
    {
        /* If the weights sum to 1 and the transfer functions invert each
         * other, blending N copies of a flat frame has to return that exact
         * frame. This catches normalisation drift, a missing EOTF, and
         * accumulator overflow in one assertion. */
        for (int w = 0; w < MBLUR_W_COUNT; w++) {
            if (w == MBLUR_W_CUSTOM)
                continue;
            for (int linear = 0; linear <= 1; linear++) {
                for (int v = 0; v <= 255; v += 51) {
                    mblur_config cfg;
                    base_cfg(&cfg, MBLUR_MODE_DECIMATE, 8);
                    cfg.weighting = (mblur_weighting)w;
                    cfg.linear_light = linear;

                    mblur_ctx *c = mblur_create(&cfg, MBLUR_BACKEND_AUTO, NULL);
                    MBT_CHECK(c != NULL, "create failed");

                    mblur_frame out;
                    int have = 0;
                    for (int i = 0; i < 8; i++) {
                        fill_flat(buf, (uint8_t)v);
                        mblur_frame in = frame_of(buf, i);
                        mblur_submit(c, &in);
                        have = (mblur_pull(c, &out) == MBLUR_OK);
                    }
                    MBT_CHECK(have, "no output after a full window");
                    if (have) {
                        int worst = 0;
                        for (size_t i = 0; i < RGBA_BYTES; i++) {
                            int d = abs((int)out.plane[0][i] - v);
                            if (d > worst)
                                worst = d;
                        }
                        MBT_CHECK(worst <= 1,
                                  "%s linear=%d value=%d drifted by %d",
                                  mblur_weighting_name((mblur_weighting)w),
                                  linear, v, worst);
                    }
                    mblur_destroy(c);
                }
            }
        }
    }
    MBT_DONE();

    /* ------------------------------------------------------------ */
    MBT_CASE("rolling and decimate agree on the same window");
    {
        /* Two independent accumulation strategies over identical input must
         * land on the same pixels: rolling's first full window is exactly
         * decimate's first window. */
        for (int w = 0; w < MBLUR_W_COUNT; w++) {
            if (w == MBLUR_W_CUSTOM)
                continue;

            mblur_config a, b;
            base_cfg(&a, MBLUR_MODE_DECIMATE, 6);
            a.weighting = (mblur_weighting)w;
            base_cfg(&b, MBLUR_MODE_ROLLING, 6);
            b.weighting = (mblur_weighting)w;

            mblur_ctx *ca = mblur_create(&a, MBLUR_BACKEND_AUTO, NULL);
            mblur_ctx *cb = mblur_create(&b, MBLUR_BACKEND_AUTO, NULL);
            MBT_CHECK(ca && cb, "create failed");

            mblur_frame oa, ob;
            int have_a = 0, have_b = 0;
            mbtest_rng_state = 0xC0FFEEu;
            for (int i = 0; i < 6; i++) {
                mbtest_fill_random(buf, RGBA_BYTES);
                mblur_frame in = frame_of(buf, i);
                mblur_submit(ca, &in);
                have_a |= (mblur_pull(ca, &oa) == MBLUR_OK);
                mblur_submit(cb, &in);
                have_b |= (mblur_pull(cb, &ob) == MBLUR_OK);
            }
            MBT_CHECK(have_a && have_b, "both modes should have emitted");
            if (have_a && have_b)
                MBT_CHECK(memcmp(oa.plane[0], ob.plane[0], RGBA_BYTES) == 0,
                          "%s: rolling and decimate disagree",
                          mblur_weighting_name((mblur_weighting)w));

            mblur_destroy(ca);
            mblur_destroy(cb);
        }
    }
    MBT_DONE();

    /* ------------------------------------------------------------ */
    MBT_CASE("linear-light blending is brighter than gamma-space");
    {
        /* The whole reason for the linear path: averaging an alternating
         * black/white sequence in gamma space yields ~128, which is far
         * darker than the 188 the physical average actually looks like. */
        int result[2] = {0, 0};
        for (int linear = 0; linear <= 1; linear++) {
            mblur_config cfg;
            base_cfg(&cfg, MBLUR_MODE_DECIMATE, 8);
            cfg.linear_light = linear;
            mblur_ctx *c = mblur_create(&cfg, MBLUR_BACKEND_AUTO, NULL);

            mblur_frame out;
            int have = 0;
            for (int i = 0; i < 8; i++) {
                fill_flat(buf, (i & 1) ? 255 : 0);
                mblur_frame in = frame_of(buf, i);
                mblur_submit(c, &in);
                have = (mblur_pull(c, &out) == MBLUR_OK);
            }
            MBT_CHECK(have, "no output");
            result[linear] = out.plane[0][0];
            mblur_destroy(c);
        }

        MBT_CHECK(abs(result[0] - 127) <= 2,
                  "gamma-space average should be ~127, got %d", result[0]);
        MBT_CHECK(abs(result[1] - 188) <= 3,
                  "linear-light average should be ~188, got %d", result[1]);
        MBT_CHECK(result[1] > result[0] + 40,
                  "linear (%d) must be clearly brighter than gamma (%d)",
                  result[1], result[0]);
    }
    MBT_DONE();

    /* ------------------------------------------------------------ */
    MBT_CASE("every backend produces identical pixels");
    {
        const mblur_backend cands[] = {
            MBLUR_BACKEND_CPU_SCALAR, MBLUR_BACKEND_CPU_SSE2,
            MBLUR_BACKEND_CPU_AVX2, MBLUR_BACKEND_CPU_AVX512};

        uint64_t reference = 0;
        int have_ref = 0;

        for (size_t i = 0; i < sizeof(cands) / sizeof(cands[0]); i++) {
            if (!mblur_backend_available(cands[i]))
                continue;

            mblur_config cfg;
            base_cfg(&cfg, MBLUR_MODE_DECIMATE, 8);
            cfg.weighting = MBLUR_W_GAUSSIAN_SYM;
            cfg.dither = 1; /* dither must be reproducible across backends */
            cfg.threads = 4; /* and across thread splits */

            mblur_ctx *c = mblur_create(&cfg, cands[i], NULL);
            MBT_CHECK(c != NULL, "create failed for %s",
                      mblur_backend_name(cands[i]));

            mblur_frame out;
            int have = 0;
            mbtest_rng_state = 0xABCDEF01u;
            for (int f = 0; f < 8; f++) {
                mbtest_fill_random(buf, RGBA_BYTES);
                mblur_frame in = frame_of(buf, f);
                mblur_submit(c, &in);
                have = (mblur_pull(c, &out) == MBLUR_OK);
            }
            MBT_CHECK(have, "no output from %s", mblur_backend_name(cands[i]));

            const uint64_t h = fnv1a(out.plane[0], RGBA_BYTES);
            if (!have_ref) {
                reference = h;
                have_ref = 1;
            } else {
                MBT_CHECK(h == reference, "%s hash %016llx != %016llx",
                          mblur_backend_name(cands[i]), (unsigned long long)h,
                          (unsigned long long)reference);
            }
            mblur_destroy(c);
        }
    }
    MBT_DONE();

    /* ------------------------------------------------------------ */
    MBT_CASE("thread count does not change the result");
    {
        uint64_t hashes[3];
        const uint32_t threads[3] = {1, 2, 4};
        for (int t = 0; t < 3; t++) {
            mblur_config cfg;
            base_cfg(&cfg, MBLUR_MODE_ROLLING, 5);
            cfg.weighting = MBLUR_W_PYRAMID;
            cfg.dither = 1;
            cfg.threads = threads[t];

            mblur_ctx *c = mblur_create(&cfg, MBLUR_BACKEND_AUTO, NULL);
            mblur_frame out;
            memset(&out, 0, sizeof(out));
            mbtest_rng_state = 0x5EED5EEDu;
            for (int f = 0; f < 5; f++) {
                mbtest_fill_random(buf, RGBA_BYTES);
                mblur_frame in = frame_of(buf, f);
                mblur_submit(c, &in);
                mblur_pull(c, &out);
            }
            hashes[t] = fnv1a(out.plane[0], RGBA_BYTES);
            mblur_destroy(c);
        }
        MBT_CHECK(hashes[0] == hashes[1] && hashes[1] == hashes[2],
                  "thread count changed the output");
    }
    MBT_DONE();

    /* ------------------------------------------------------------ */
    MBT_CASE("strided input is handled");
    {
        const size_t stride = W * 4u + 96u; /* deliberately padded */
        uint8_t *padded = malloc(stride * H);
        memset(padded, 0xEE, stride * H); /* padding must never be read */

        mblur_config cfg;
        base_cfg(&cfg, MBLUR_MODE_DECIMATE, 2);
        mblur_ctx *c = mblur_create(&cfg, MBLUR_BACKEND_AUTO, NULL);

        mblur_frame out;
        int have = 0;
        for (int f = 0; f < 2; f++) {
            for (uint32_t r = 0; r < H; r++)
                memset(padded + r * stride, 77, W * 4u);
            mblur_frame in;
            memset(&in, 0, sizeof(in));
            in.plane[0] = padded;
            in.stride[0] = stride;
            mblur_submit(c, &in);
            have = (mblur_pull(c, &out) == MBLUR_OK);
        }
        MBT_CHECK(have, "no output");
        if (have) {
            int bad = 0;
            for (size_t i = 0; i < RGBA_BYTES; i++)
                if (abs((int)out.plane[0][i] - 77) > 1)
                    bad++;
            MBT_CHECK(bad == 0, "%d samples wrong - padding leaked in", bad);
        }
        mblur_destroy(c);
        free(padded);
    }
    MBT_DONE();

    /* ------------------------------------------------------------ */
    MBT_CASE("phase offset drops frames to realign the window");
    {
        mblur_config cfg;
        base_cfg(&cfg, MBLUR_MODE_DECIMATE, 4);
        cfg.phase_offset = 2;
        mblur_ctx *c = mblur_create(&cfg, MBLUR_BACKEND_AUTO, NULL);

        int first_emit = -1;
        for (int i = 0; i < 10 && first_emit < 0; i++) {
            fill_flat(buf, 50);
            mblur_frame in = frame_of(buf, i);
            mblur_submit(c, &in);
            mblur_frame out;
            if (mblur_pull(c, &out) == MBLUR_OK)
                first_emit = i;
        }
        MBT_CHECK(first_emit == 5,
                  "with offset 2 the first window should close at frame 5, "
                  "got %d", first_emit);
        mblur_destroy(c);
    }
    MBT_DONE();

    /* ------------------------------------------------------------ */
    MBT_CASE("reset clears the window");
    {
        mblur_config cfg;
        base_cfg(&cfg, MBLUR_MODE_ROLLING, 4);
        mblur_ctx *c = mblur_create(&cfg, MBLUR_BACKEND_AUTO, NULL);

        for (int i = 0; i < 4; i++) {
            fill_flat(buf, 200);
            mblur_frame in = frame_of(buf, i);
            mblur_submit(c, &in);
        }
        mblur_reset(c);

        mblur_frame out;
        int emitted_early = 0;
        for (int i = 0; i < 3; i++) {
            fill_flat(buf, 10);
            mblur_frame in = frame_of(buf, i);
            mblur_submit(c, &in);
            if (mblur_pull(c, &out) == MBLUR_OK)
                emitted_early = 1;
        }
        MBT_CHECK(!emitted_early, "reset did not clear the ring");

        fill_flat(buf, 10);
        mblur_frame in = frame_of(buf, 3);
        mblur_submit(c, &in);
        MBT_CHECK(mblur_pull(c, &out) == MBLUR_OK, "no output after refill");
        MBT_CHECK(abs((int)out.plane[0][0] - 10) <= 1,
                  "stale frames leaked through reset: %d", out.plane[0][0]);
        mblur_destroy(c);
    }
    MBT_DONE();

    /* ------------------------------------------------------------ */
    MBT_CASE("NV12 blends, and refuses linear light");
    {
        mblur_config cfg;
        mblur_config_defaults(&cfg);
        cfg.width = W;
        cfg.height = H;
        cfg.format = MBLUR_FMT_NV12;
        cfg.frames = 4;
        cfg.dither = 0;

        const char *why = NULL;
        MBT_CHECK(mblur_create(&cfg, MBLUR_BACKEND_AUTO, &why) == NULL,
                  "NV12 with linear light should be refused");
        MBT_CHECK(why != NULL && strstr(why, "linear_light") != NULL,
                  "the refusal should name the offending option");

        cfg.linear_light = 0;
        mblur_ctx *c = mblur_create(&cfg, MBLUR_BACKEND_AUTO, &why);
        MBT_CHECK(c != NULL, "NV12 create failed: %s", why ? why : "?");

        uint8_t *y = malloc(W * H);
        uint8_t *uv = malloc(W * H / 2);
        mblur_frame out;
        int have = 0;
        for (int i = 0; i < 4; i++) {
            memset(y, 90, W * H);
            memset(uv, 140, W * H / 2);
            mblur_frame in;
            memset(&in, 0, sizeof(in));
            in.plane[0] = y;
            in.stride[0] = W;
            in.plane[1] = uv;
            in.stride[1] = W;
            mblur_submit(c, &in);
            have = (mblur_pull(c, &out) == MBLUR_OK);
        }
        MBT_CHECK(have, "no NV12 output");
        if (have) {
            MBT_CHECK(abs((int)out.plane[0][0] - 90) <= 1, "luma drifted: %d",
                      out.plane[0][0]);
            MBT_CHECK(abs((int)out.plane[1][0] - 140) <= 1, "chroma drifted: %d",
                      out.plane[1][0]);
        }
        free(y);
        free(uv);
        mblur_destroy(c);
    }
    MBT_DONE();

    /* ------------------------------------------------------------ */
    MBT_CASE("bad configs are rejected with a reason");
    {
        const char *why;
        mblur_config cfg;
        base_cfg(&cfg, MBLUR_MODE_DECIMATE, 4);

        cfg.frames = 0;
        MBT_CHECK(mblur_create(&cfg, MBLUR_BACKEND_AUTO, &why) == NULL && why,
                  "zero frames accepted");
        cfg.frames = MBLUR_MAX_FRAMES + 1;
        MBT_CHECK(mblur_create(&cfg, MBLUR_BACKEND_AUTO, &why) == NULL && why,
                  "too many frames accepted");
        cfg.frames = 4;
        cfg.width = 0;
        MBT_CHECK(mblur_create(&cfg, MBLUR_BACKEND_AUTO, &why) == NULL && why,
                  "zero width accepted");
        cfg.width = W;
        cfg.amount = 0.0f;
        MBT_CHECK(mblur_create(&cfg, MBLUR_BACKEND_AUTO, &why) == NULL && why,
                  "zero shutter accepted");
        cfg.amount = 1.0f;
        cfg.weighting = MBLUR_W_CUSTOM;
        cfg.custom_weights = NULL;
        MBT_CHECK(mblur_create(&cfg, MBLUR_BACKEND_AUTO, &why) == NULL && why,
                  "custom weighting without weights accepted");
    }
    MBT_DONE();

    /* ------------------------------------------------------------ */
    MBT_CASE("single-tap blur is a pass-through");
    {
        mblur_config cfg;
        base_cfg(&cfg, MBLUR_MODE_DECIMATE, 1);
        mblur_ctx *c = mblur_create(&cfg, MBLUR_BACKEND_AUTO, NULL);
        MBT_CHECK(mblur_latency_frames(c) == 0, "1 tap should add no latency");

        mbtest_rng_state = 0x1234u;
        mbtest_fill_random(buf, RGBA_BYTES);
        memcpy(buf2, buf, RGBA_BYTES);
        mblur_frame in = frame_of(buf, 0), out;
        mblur_submit(c, &in);
        MBT_CHECK(mblur_pull(c, &out) == MBLUR_OK, "no output");

        int worst = 0;
        for (size_t i = 0; i < RGBA_BYTES; i++) {
            int d = abs((int)out.plane[0][i] - (int)buf2[i]);
            if (d > worst)
                worst = d;
        }
        MBT_CHECK(worst <= 1, "pass-through drifted by %d", worst);
        mblur_destroy(c);
    }
    MBT_DONE();

    free(buf);
    free(buf2);
    return mbtest_report("test_pipeline");
}
