/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "mblur/mblur.h"
#include "mbtest.h"
#include <math.h>

static void base_config(mblur_config *cfg, uint32_t frames, mblur_weighting w)
{
    mblur_config_defaults(cfg);
    cfg->width = 64;
    cfg->height = 64;
    cfg->frames = frames;
    cfg->weighting = w;
}

int main(void)
{
    printf("test_weights\n");

    MBT_CASE("every kernel normalises to 1");
    for (int w = 0; w < MBLUR_W_COUNT; w++) {
        if (w == MBLUR_W_CUSTOM)
            continue;
        for (uint32_t n = 1; n <= 32; n++) {
            for (float amount = 0.25f; amount <= 2.0f; amount += 0.25f) {
                mblur_config cfg;
                base_config(&cfg, n, (mblur_weighting)w);
                cfg.amount = amount;

                float weights[MBLUR_MAX_FRAMES];
                MBT_CHECK(mblur_weights_generate(&cfg, weights) == MBLUR_OK,
                          "generate failed for %s n=%u amount=%.2f",
                          mblur_weighting_name((mblur_weighting)w), n, amount);

                double sum = 0.0;
                for (uint32_t i = 0; i < n; i++) {
                    MBT_CHECK(weights[i] >= 0.0f,
                              "%s n=%u tap %u is negative (%f)",
                              mblur_weighting_name((mblur_weighting)w), n, i,
                              weights[i]);
                    sum += weights[i];
                }
                MBT_CHECK(fabs(sum - 1.0) < 1e-6,
                          "%s n=%u amount=%.2f sums to %.9f",
                          mblur_weighting_name((mblur_weighting)w), n, amount,
                          sum);
            }
        }
    }
    MBT_DONE();

    MBT_CASE("equal weighting is flat");
    {
        mblur_config cfg;
        base_config(&cfg, 8, MBLUR_W_EQUAL);
        float weights[MBLUR_MAX_FRAMES];
        mblur_weights_generate(&cfg, weights);
        for (uint32_t i = 0; i < 8; i++)
            MBT_CLOSE(weights[i], 0.125, 1e-6);
    }
    MBT_DONE();

    MBT_CASE("shutter angle below 1 drops the outer taps");
    {
        mblur_config cfg;
        base_config(&cfg, 8, MBLUR_W_EQUAL);
        cfg.amount = 0.5f;
        float weights[MBLUR_MAX_FRAMES];
        mblur_weights_generate(&cfg, weights);

        /* Tap centres sit at +/-0.875, 0.625, 0.375, 0.125, so a half-open
         * shutter keeps exactly the middle four. */
        MBT_CLOSE(weights[0], 0.0, 1e-9);
        MBT_CLOSE(weights[1], 0.0, 1e-9);
        MBT_CLOSE(weights[2], 0.25, 1e-6);
        MBT_CLOSE(weights[5], 0.25, 1e-6);
        MBT_CLOSE(weights[6], 0.0, 1e-9);
        MBT_CLOSE(weights[7], 0.0, 1e-9);
    }
    MBT_DONE();

    MBT_CASE("a wider shutter flattens a gaussian");
    {
        float narrow[MBLUR_MAX_FRAMES], wide[MBLUR_MAX_FRAMES];
        mblur_config cfg;
        base_config(&cfg, 16, MBLUR_W_GAUSSIAN_SYM);
        cfg.amount = 1.0f;
        mblur_weights_generate(&cfg, narrow);
        cfg.amount = 4.0f;
        mblur_weights_generate(&cfg, wide);
        /* Flatter means the peak tap holds less of the total. */
        MBT_CHECK(wide[8] < narrow[8],
                  "peak did not flatten: %f vs %f", wide[8], narrow[8]);
    }
    MBT_DONE();

    MBT_CASE("ascending and descending mirror each other");
    {
        float asc[MBLUR_MAX_FRAMES], desc[MBLUR_MAX_FRAMES];
        mblur_config cfg;
        base_config(&cfg, 10, MBLUR_W_ASCENDING);
        mblur_weights_generate(&cfg, asc);
        cfg.weighting = MBLUR_W_DESCENDING;
        mblur_weights_generate(&cfg, desc);
        for (uint32_t i = 0; i < 10; i++)
            MBT_CLOSE(asc[i], desc[9 - i], 1e-6);
    }
    MBT_DONE();

    MBT_CASE("custom weights resample to the tap count");
    {
        const float custom[3] = {1.0f, 2.0f, 1.0f};
        mblur_config cfg;
        base_config(&cfg, 5, MBLUR_W_CUSTOM);
        cfg.custom_weights = custom;
        cfg.custom_weight_count = 3;

        float weights[MBLUR_MAX_FRAMES];
        MBT_CHECK(mblur_weights_generate(&cfg, weights) == MBLUR_OK,
                  "custom generate failed");
        double sum = 0.0;
        for (uint32_t i = 0; i < 5; i++)
            sum += weights[i];
        MBT_CLOSE(sum, 1.0, 1e-6);
        MBT_CHECK(weights[2] > weights[0], "resampled peak is not centred");
        MBT_CLOSE(weights[0], weights[4], 1e-6);
    }
    MBT_DONE();

    MBT_CASE("csv parsing accepts lists and rejects junk");
    {
        float out[MBLUR_MAX_FRAMES];
        MBT_CHECK(mblur_weights_parse_csv("1, 2,3 ,4", out, MBLUR_MAX_FRAMES) == 4,
                  "expected 4 values");
        MBT_CLOSE(out[3], 4.0, 1e-9);
        MBT_CHECK(mblur_weights_parse_csv("0.5 0.25", out, MBLUR_MAX_FRAMES) == 2,
                  "whitespace separation should work");
        MBT_CHECK(mblur_weights_parse_csv("1,two,3", out, MBLUR_MAX_FRAMES) == -1,
                  "a non-numeric entry must be an error, not a skip");
        MBT_CHECK(mblur_weights_parse_csv("1,-2", out, MBLUR_MAX_FRAMES) == -1,
                  "negative weights must be rejected");
        MBT_CHECK(mblur_weights_parse_csv("", out, MBLUR_MAX_FRAMES) == -1,
                  "an empty list must be an error");
    }
    MBT_DONE();

    MBT_CASE("name round-trips");
    {
        for (int i = 0; i < MBLUR_W_COUNT; i++)
            MBT_CHECK(mblur_weighting_parse(
                          mblur_weighting_name((mblur_weighting)i)) == i,
                      "weighting %d did not round-trip", i);
        for (int i = 0; i < MBLUR_FMT_COUNT; i++)
            MBT_CHECK(mblur_format_parse(mblur_format_name((mblur_format)i)) == i,
                      "format %d did not round-trip", i);
        MBT_CHECK(mblur_weighting_parse("GAUSSIAN-SYM") == MBLUR_W_GAUSSIAN_SYM,
                  "case and dashes should be accepted");
        MBT_CHECK(mblur_weighting_parse("nonsense") == -1,
                  "unknown names must report an error");
    }
    MBT_DONE();

    return mbtest_report("test_weights");
}
