/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * End-to-end test of blurcli's raw path: synthesise frames, run the real
 * binary over them, check what comes out. The container path needs ffmpeg
 * and is covered by the dry-run check below, which verifies the commands we
 * would hand it without needing it installed.
 */
#include "mbtest.h"
#include "mblur/mblur.h"

#include <stdarg.h>
#include <stdio.h>

#if defined(_WIN32)
#define popen _popen
#define pclose _pclose
#endif

#ifndef BLURCLI_PATH
#define BLURCLI_PATH "blurcli"
#endif

#define W 64u
#define H 32u
#define NV12_BYTES (W * H * 3u / 2u)

static void write_flat_frames(const char *path, int count, uint8_t luma,
                              uint8_t chroma)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    uint8_t *buf = malloc(NV12_BYTES);
    memset(buf, luma, (size_t)W * H);
    memset(buf + (size_t)W * H, chroma, (size_t)W * H / 2u);
    for (int i = 0; i < count; i++)
        fwrite(buf, 1, NV12_BYTES, f);
    free(buf);
    fclose(f);
}

static long file_size(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fclose(f);
    return n;
}

static int run(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char cmd[2048];
    vsnprintf(cmd, sizeof(cmd), fmt, ap);
    va_end(ap);
    return system(cmd);
}

int main(void)
{
    printf("test_cli (%s)\n", BLURCLI_PATH);

    const char *in = "mbtest_in.nv12";
    const char *out = "mbtest_out.nv12";

    MBT_CASE("decimate: 16 frames in, 4 out");
    {
        write_flat_frames(in, 16, 120, 128);
        const int rc = run("\"%s\" --raw --width %u --height %u --frames 4 "
                           "--mode decimate --raw-in %s --raw-out %s",
                           BLURCLI_PATH, W, H, in, out);
        MBT_CHECK(rc == 0, "blurcli exited with %d", rc);
        MBT_CHECK(file_size(out) == (long)(NV12_BYTES * 4),
                  "expected 4 frames (%ld bytes), got %ld",
                  (long)(NV12_BYTES * 4), file_size(out));
    }
    MBT_DONE();

    MBT_CASE("rolling: 16 frames in, 13 out");
    {
        write_flat_frames(in, 16, 120, 128);
        const int rc = run("\"%s\" --raw --width %u --height %u --frames 4 "
                           "--mode rolling --raw-in %s --raw-out %s",
                           BLURCLI_PATH, W, H, in, out);
        MBT_CHECK(rc == 0, "blurcli exited with %d", rc);
        /* The first three frames only fill the window. */
        MBT_CHECK(file_size(out) == (long)(NV12_BYTES * 13),
                  "expected 13 frames, got %ld bytes", file_size(out));
    }
    MBT_DONE();

    MBT_CASE("a flat clip comes back unchanged");
    {
        write_flat_frames(in, 8, 77, 200);
        const int rc = run("\"%s\" --raw --width %u --height %u --frames 8 "
                           "--no-dither --raw-in %s --raw-out %s",
                           BLURCLI_PATH, W, H, in, out);
        MBT_CHECK(rc == 0, "blurcli exited with %d", rc);

        FILE *f = fopen(out, "rb");
        MBT_CHECK(f != NULL, "no output file");
        if (f) {
            uint8_t *buf = malloc(NV12_BYTES);
            const size_t got = fread(buf, 1, NV12_BYTES, f);
            MBT_CHECK(got == NV12_BYTES, "short output frame");
            int bad = 0;
            for (size_t i = 0; i < (size_t)W * H; i++)
                if (abs((int)buf[i] - 77) > 1)
                    bad++;
            for (size_t i = (size_t)W * H; i < NV12_BYTES; i++)
                if (abs((int)buf[i] - 200) > 1)
                    bad++;
            MBT_CHECK(bad == 0, "%d samples drifted", bad);
            free(buf);
            fclose(f);
        }
    }
    MBT_DONE();

    MBT_CASE("a short clip reports that it produced nothing");
    {
        /* Three frames cannot fill a window of eight. Exiting 0 with an
         * empty file would look like success and waste someone's evening. */
        write_flat_frames(in, 3, 100, 128);
        const int rc = run("\"%s\" --raw --width %u --height %u --frames 8 "
                           "--raw-in %s --raw-out %s 2>/dev/null",
                           BLURCLI_PATH, W, H, in, out);
        MBT_CHECK(rc != 0, "expected a non-zero exit, got %d", rc);
    }
    MBT_DONE();

    MBT_CASE("bad arguments are refused");
    {
        MBT_CHECK(run("\"%s\" --raw --width 64 2>/dev/null", BLURCLI_PATH) != 0,
                  "missing --height accepted");
        MBT_CHECK(run("\"%s\" --raw --width 64 --height 32 --weighting nope "
                      "2>/dev/null",
                      BLURCLI_PATH) != 0,
                  "unknown weighting accepted");
        MBT_CHECK(run("\"%s\" --raw --width 64 --height 32 --linear-light "
                      "2>/dev/null",
                      BLURCLI_PATH) != 0,
                  "linear light with NV12 accepted");
        MBT_CHECK(run("\"%s\" --raw --width 64 --height 32 --interpolate 4 "
                      "2>/dev/null",
                      BLURCLI_PATH) != 0,
                  "interpolation with --raw accepted");
    }
    MBT_DONE();

    MBT_CASE("dry run builds sane ffmpeg commands");
    {
        /* The container path cannot be exercised without ffmpeg, but the
         * commands it would run can be. Frame rate arithmetic is the thing
         * worth checking: get it wrong and the result plays at the wrong
         * speed, which is easy to miss and hard to diagnose. */
        char cmd[1024];
        snprintf(cmd, sizeof(cmd),
                 "\"%s\" --input clip.mp4 --output out.mp4 --width 1920 "
                 "--height 1080 --fps 240 --frames 4 --dry-run 2>&1",
                 BLURCLI_PATH);
        FILE *p = popen(cmd, "r");
        MBT_CHECK(p != NULL, "could not run blurcli");

        char text[8192] = {0};
        size_t len = 0;
        if (p) {
            len = fread(text, 1, sizeof(text) - 1, p);
            pclose(p);
        }
        text[len] = '\0';

        MBT_CHECK(strstr(text, "240.000 fps in") != NULL,
                  "input rate missing from:\n%s", text);
        MBT_CHECK(strstr(text, "60.000 fps out") != NULL,
                  "240 fps decimated by 4 should record at 60:\n%s", text);
        MBT_CHECK(strstr(text, "-f rawvideo -pix_fmt nv12 -") != NULL,
                  "decode command should emit raw nv12:\n%s", text);
        MBT_CHECK(strstr(text, "-r 60.000000") != NULL,
                  "encoder should be told the output rate:\n%s", text);

        /* And with interpolation the arithmetic runs the other way. */
        snprintf(cmd, sizeof(cmd),
                 "\"%s\" --input clip.mp4 --output out.mp4 --width 1920 "
                 "--height 1080 --fps 60 --interpolate 8 --frames 8 "
                 "--dry-run 2>&1",
                 BLURCLI_PATH);
        p = popen(cmd, "r");
        memset(text, 0, sizeof(text));
        if (p) {
            len = fread(text, 1, sizeof(text) - 1, p);
            pclose(p);
        }
        text[len] = '\0';

        MBT_CHECK(strstr(text, "minterpolate=fps=480.000000") != NULL,
                  "60 fps x8 should interpolate to 480:\n%s", text);
        MBT_CHECK(strstr(text, "60.000 fps out") != NULL,
                  "480 fps decimated by 8 should return to 60:\n%s", text);
    }
    MBT_DONE();

    remove(in);
    remove(out);
    return mbtest_report("test_cli");
}
