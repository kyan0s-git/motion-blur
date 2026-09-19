/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * blurcli - offline motion blur.
 *
 * Frames come in on a pipe, get blended, and go out on a pipe. When a
 * container file is named, ffmpeg is spawned on both ends to decode and
 * encode; that is a deliberate choice over linking libav*:
 *
 *   - no build-time dependency, so the tool compiles anywhere the core does
 *   - the raw path is the same code, so the interesting part is testable
 *     without a single video file
 *   - interpolation comes free: ffmpeg's minterpolate runs in the decode
 *     stage, which is what makes 60 fps in / 60 fps out with real blur
 *     possible at all
 *
 * The cost is a copy through the pipe rather than staying on the GPU. A
 * zero-copy D3D11VA -> shader -> NVENC path is the obvious optimisation and
 * the design notes for it are in docs/; it is not here because it cannot be
 * written responsibly without a machine to test it on.
 */
#include "options.h"
#include "mblur/mblur.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#define popen _popen
#define pclose _pclose
#define set_binary(f) _setmode(_fileno(f), _O_BINARY)
#else
#define set_binary(f) ((void)(f))
#endif

/* ------------------------------------------------------------ utilities */

static size_t frame_size(mblur_format fmt, uint32_t w, uint32_t h)
{
    switch (fmt) {
    case MBLUR_FMT_NV12:
    case MBLUR_FMT_I420:
        return (size_t)w * h * 3u / 2u;
    default:
        return (size_t)w * h * 4u;
    }
}

static const char *ffmpeg_pix_fmt(mblur_format fmt)
{
    switch (fmt) {
    case MBLUR_FMT_NV12:
        return "nv12";
    case MBLUR_FMT_I420:
        return "yuv420p";
    case MBLUR_FMT_BGRA8:
        return "bgra";
    default:
        return "rgba";
    }
}

static void frame_point_at(mblur_frame *f, mblur_format fmt, uint32_t w,
                           uint32_t h, uint8_t *base)
{
    memset(f, 0, sizeof(*f));
    f->plane[0] = base;
    switch (fmt) {
    case MBLUR_FMT_NV12:
        f->stride[0] = w;
        f->plane[1] = base + (size_t)w * h;
        f->stride[1] = w;
        break;
    case MBLUR_FMT_I420:
        f->stride[0] = w;
        f->plane[1] = base + (size_t)w * h;
        f->stride[1] = w / 2u;
        f->plane[2] = f->plane[1] + (size_t)(w / 2u) * (h / 2u);
        f->stride[2] = w / 2u;
        break;
    default:
        f->stride[0] = (size_t)w * 4u;
        break;
    }
}

/* A growable command string, so building an ffmpeg invocation reads like
 * the command it produces. */
typedef struct {
    char *buf;
    size_t len, cap;
} cmd;

static int cmd_add(cmd *c, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char tmp[1024];
    const int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof(tmp))
        return -1;

    if (c->len + (size_t)n + 1 > c->cap) {
        size_t cap = c->cap ? c->cap * 2 : 512;
        while (cap < c->len + (size_t)n + 1)
            cap *= 2;
        char *nb = realloc(c->buf, cap);
        if (!nb)
            return -1;
        c->buf = nb;
        c->cap = cap;
    }
    memcpy(c->buf + c->len, tmp, (size_t)n + 1);
    c->len += (size_t)n;
    return 0;
}

/* ---------------------------------------------------------- probing */

static int probe_input(const blur_options *o, uint32_t *w, uint32_t *h,
                       double *fps)
{
    cmd c = {0};
    if (cmd_add(&c,
                "\"%s\" -v error -select_streams v:0 -show_entries "
                "stream=width,height,r_frame_rate -of csv=p=0 \"%s\"",
                o->ffprobe, o->input) != 0) {
        free(c.buf);
        return -1;
    }

    FILE *p = popen(c.buf, "r");
    free(c.buf);
    if (!p) {
        fprintf(stderr, "blurcli: could not run '%s'\n", o->ffprobe);
        return -1;
    }

    char line[256] = {0};
    const char *got = fgets(line, sizeof(line), p);
    const int rc = pclose(p);

    if (!got || rc != 0) {
        fprintf(stderr,
                "blurcli: ffprobe could not read '%s' (exit %d). Is ffprobe "
                "installed and on PATH, and is the file a video?\n",
                o->input, rc);
        return -1;
    }

    unsigned pw = 0, ph = 0, num = 0, den = 0;
    if (sscanf(line, "%u,%u,%u/%u", &pw, &ph, &num, &den) != 4 || !pw || !ph ||
        !den) {
        fprintf(stderr, "blurcli: could not parse ffprobe output: %s", line);
        return -1;
    }

    *w = pw;
    *h = ph;
    *fps = (double)num / (double)den;
    return 0;
}

/* -------------------------------------------------- command construction */

static char *build_decode_command(const blur_options *o, uint32_t w, uint32_t h,
                                  double in_fps, double *decoded_fps)
{
    cmd c = {0};
    double rate = in_fps;

    if (cmd_add(&c, "\"%s\" -v error -nostdin", o->ffmpeg) != 0)
        goto fail;

    /* An input timescale is expressed by reinterpreting the input frame
     * rate, which is what blur means by it: the frames are unchanged, only
     * their timing is. */
    if (o->input_timescale != 1.0 && o->input_timescale > 0.0) {
        rate = in_fps / o->input_timescale;
        if (cmd_add(&c, " -r %.6f", rate) != 0)
            goto fail;
    }

    if (cmd_add(&c, " -i \"%s\"", o->input) != 0)
        goto fail;

    double target = 0.0;
    if (o->interpolated_fps > 0.0)
        target = o->interpolated_fps;
    else if (o->interpolate > 1)
        target = rate * (double)o->interpolate;

    if (target > 0.0) {
        /*
         * Interpolating before blending is the only honest way to blur
         * footage that was captured at the rate you want to keep. The
         * synthesised frames are estimates, so artefacts in fast motion are
         * expected - but they are averaged into a blur rather than shown,
         * which hides most of them.
         */
        if (cmd_add(&c, " -vf \"minterpolate=fps=%.6f:mi_mode=%s\"", target,
                    o->interp_mode) != 0)
            goto fail;
        rate = target;
    }

    if (cmd_add(&c, " -f rawvideo -pix_fmt %s -", ffmpeg_pix_fmt(o->format)) != 0)
        goto fail;

    (void)w;
    (void)h;
    *decoded_fps = rate;
    return c.buf;

fail:
    free(c.buf);
    return NULL;
}

static char *build_encode_command(const blur_options *o, uint32_t w, uint32_t h,
                                  double out_fps)
{
    cmd c = {0};

    if (cmd_add(&c,
                "\"%s\" -v error -y -f rawvideo -pix_fmt %s -s %ux%u -r %.6f "
                "-i -",
                o->ffmpeg, ffmpeg_pix_fmt(o->format), w, h, out_fps) != 0)
        goto fail;

    /* Audio is taken from the original file rather than passed through the
     * pipe, which only carries video. */
    if (o->copy_audio && o->input) {
        if (cmd_add(&c, " -i \"%s\" -map 0:v:0 -map 1:a:0? -c:a copy",
                    o->input) != 0)
            goto fail;
    }

    if (o->extra_filters && *o->extra_filters) {
        if (cmd_add(&c, " -vf \"%s\"", o->extra_filters) != 0)
            goto fail;
    }

    if (cmd_add(&c, " -c:v %s -crf %d", o->encoder, o->crf) != 0)
        goto fail;

    /* Not every encoder understands -preset; x264/x265/svt do, the hardware
     * ones mostly do not, so only pass it when it was asked for. */
    if (o->preset && *o->preset &&
        (strstr(o->encoder, "x264") || strstr(o->encoder, "x265") ||
         strstr(o->encoder, "svt")))
        if (cmd_add(&c, " -preset %s", o->preset) != 0)
            goto fail;

    if (o->output_timescale != 1.0 && o->output_timescale > 0.0) {
        if (cmd_add(&c, " -filter:v setpts=%.6f*PTS", 1.0 / o->output_timescale) !=
            0)
            goto fail;
    }

    if (cmd_add(&c, " \"%s\"", o->output) != 0)
        goto fail;

    return c.buf;

fail:
    free(c.buf);
    return NULL;
}

/* ------------------------------------------------------------ the loop */

static int run_blur(const blur_options *o, FILE *in, FILE *out, uint32_t w,
                    uint32_t h)
{
    mblur_config cfg;
    mblur_config_defaults(&cfg);
    cfg.width = w;
    cfg.height = h;
    cfg.format = o->format;
    cfg.transfer = o->transfer;
    cfg.frames = o->frames;
    cfg.mode = o->mode;
    cfg.weighting = o->weighting;
    cfg.amount = o->amount;
    cfg.gauss_std = o->gauss_std;
    cfg.gauss_mean = o->gauss_mean;
    cfg.gauss_bound = o->gauss_bound;
    cfg.phase_offset = o->phase_offset;
    cfg.linear_light = o->linear_light;
    cfg.dither = o->dither;
    cfg.threads = o->threads;

    float custom[MBLUR_MAX_FRAMES];
    if (o->weighting == MBLUR_W_CUSTOM) {
        const int n =
            mblur_weights_parse_csv(o->custom_weights, custom, MBLUR_MAX_FRAMES);
        if (n <= 0) {
            fprintf(stderr, "blurcli: --custom-weights '%s' is not a list of "
                            "non-negative numbers\n",
                    o->custom_weights ? o->custom_weights : "");
            return 1;
        }
        cfg.custom_weights = custom;
        cfg.custom_weight_count = (uint32_t)n;
    }

    const char *why = NULL;
    mblur_ctx *ctx = mblur_create(&cfg, o->backend, &why);
    if (!ctx) {
        fprintf(stderr, "blurcli: %s\n", why ? why : "could not start the blur engine");
        return 1;
    }

    if (o->verbose) {
        fprintf(stderr,
                "blurcli: %ux%u %s, %u frames per window, %s, %s weighting, "
                "backend %s, %.1f MB of buffers\n",
                w, h, mblur_format_name(o->format), o->frames,
                mblur_mode_name(o->mode), mblur_weighting_name(o->weighting),
                mblur_backend_name(mblur_ctx_backend(ctx)),
                (double)mblur_ctx_memory_usage(ctx) / 1e6);
    }

    const size_t bytes = frame_size(o->format, w, h);
    uint8_t *buf = malloc(bytes);
    if (!buf) {
        fprintf(stderr, "blurcli: out of memory\n");
        mblur_destroy(ctx);
        return 1;
    }

    uint64_t read_frames = 0, written = 0;
    int status = 0;

    for (;;) {
        const size_t got = fread(buf, 1, bytes, in);
        if (got == 0)
            break;
        if (got != bytes) {
            /* A partial frame means the decoder died or the raw stream does
             * not match --width/--height/--format. Either way, silently
             * blending garbage would be worse than saying so. */
            fprintf(stderr,
                    "blurcli: short read - %zu of %zu bytes for frame %llu. "
                    "Check that --width, --height and --format match the "
                    "stream.\n",
                    got, bytes, (unsigned long long)read_frames);
            status = 1;
            break;
        }
        read_frames++;

        mblur_frame in_frame;
        frame_point_at(&in_frame, o->format, w, h, buf);
        in_frame.pts = (int64_t)read_frames;

        if (mblur_submit(ctx, &in_frame) != MBLUR_OK) {
            fprintf(stderr, "blurcli: the engine rejected frame %llu\n",
                    (unsigned long long)read_frames);
            status = 1;
            break;
        }

        mblur_frame out_frame;
        if (mblur_pull(ctx, &out_frame) == MBLUR_OK) {
            /* Planes come back tightly packed, so each one is a single
             * contiguous write. */
            int failed = 0;
            for (int p = 0; p < MBLUR_MAX_PLANES && out_frame.plane[p]; p++) {
                size_t rows, row_bytes;
                if (o->format == MBLUR_FMT_NV12) {
                    row_bytes = w;
                    rows = (p == 0) ? h : h / 2u;
                } else if (o->format == MBLUR_FMT_I420) {
                    row_bytes = (p == 0) ? w : w / 2u;
                    rows = (p == 0) ? h : h / 2u;
                } else {
                    row_bytes = (size_t)w * 4u;
                    rows = h;
                }
                if (fwrite(out_frame.plane[p], 1, row_bytes * rows, out) !=
                    row_bytes * rows) {
                    failed = 1;
                    break;
                }
            }
            if (failed) {
                fprintf(stderr, "blurcli: could not write frame %llu - the "
                                "encoder may have exited\n",
                        (unsigned long long)written);
                status = 1;
                break;
            }
            written++;
            if (o->verbose && (written % 60) == 0)
                fprintf(stderr, "\rblurcli: %llu in, %llu out",
                        (unsigned long long)read_frames,
                        (unsigned long long)written);
        }
    }

    if (o->verbose)
        fprintf(stderr, "\rblurcli: %llu frames in, %llu out%*s\n",
                (unsigned long long)read_frames, (unsigned long long)written,
                20, "");

    if (status == 0 && written == 0) {
        fprintf(stderr,
                "blurcli: no output frames. %llu frames arrived but a window "
                "needs %u, so there was never a complete one.\n",
                (unsigned long long)read_frames, o->frames);
        status = 1;
    }

    free(buf);
    mblur_destroy(ctx);
    return status;
}

/* ---------------------------------------------------------------- main */

int main(int argc, char **argv)
{
    blur_options o;
    blur_options_defaults(&o);

    const int pr = blur_options_parse(&o, argc, argv);
    if (pr != 0)
        return (pr > 0) ? 0 : 2;

    if (o.linear_light &&
        (o.format == MBLUR_FMT_NV12 || o.format == MBLUR_FMT_I420)) {
        fprintf(stderr,
                "blurcli: --linear-light needs an RGB format. Either drop it, "
                "or add --format rgba8 to decode to RGB first (slower, but "
                "colour-correct through the blur).\n");
        return 2;
    }

    if (o.raw) {
        FILE *in = o.raw_in ? fopen(o.raw_in, "rb") : stdin;
        if (!in) {
            fprintf(stderr, "blurcli: cannot open '%s'\n", o.raw_in);
            return 1;
        }
        FILE *out = o.raw_out ? fopen(o.raw_out, "wb") : stdout;
        if (!out) {
            fprintf(stderr, "blurcli: cannot create '%s'\n", o.raw_out);
            if (in != stdin)
                fclose(in);
            return 1;
        }
        set_binary(in);
        set_binary(out);

        const int rc = run_blur(&o, in, out, o.width, o.height);

        if (in != stdin)
            fclose(in);
        if (out != stdout)
            fclose(out);
        else
            fflush(stdout);
        return rc;
    }

    uint32_t w = o.width, h = o.height;
    double fps = o.fps;

    if (w == 0 || h == 0 || fps <= 0.0) {
        uint32_t pw, ph;
        double pfps;
        if (probe_input(&o, &pw, &ph, &pfps) != 0)
            return 1;
        if (w == 0)
            w = pw;
        if (h == 0)
            h = ph;
        if (fps <= 0.0)
            fps = pfps;
    }

    double decoded_fps = fps;
    char *decode = build_decode_command(&o, w, h, fps, &decoded_fps);
    if (!decode) {
        fprintf(stderr, "blurcli: could not build the decode command\n");
        return 1;
    }

    /* Decimating divides the frame rate; rolling keeps it. This is the
     * number the encoder must be told, or the result plays at the wrong
     * speed. */
    const double out_fps = (o.mode == MBLUR_MODE_DECIMATE)
                               ? decoded_fps / (double)o.frames
                               : decoded_fps;

    char *encode = build_encode_command(&o, w, h, out_fps);
    if (!encode) {
        fprintf(stderr, "blurcli: could not build the encode command\n");
        free(decode);
        return 1;
    }

    if (o.verbose || o.dry_run) {
        fprintf(stderr, "blurcli: %ux%u, %.3f fps in", w, h, fps);
        if (decoded_fps != fps)
            fprintf(stderr, " -> %.3f fps interpolated", decoded_fps);
        fprintf(stderr, " -> %.3f fps out\n", out_fps);
        fprintf(stderr, "blurcli: decode: %s\n", decode);
        fprintf(stderr, "blurcli: encode: %s\n", encode);
    }

    if (o.dry_run) {
        free(decode);
        free(encode);
        return 0;
    }

    FILE *dec = popen(decode, "r");
    if (!dec) {
        fprintf(stderr, "blurcli: could not start ffmpeg for decoding\n");
        free(decode);
        free(encode);
        return 1;
    }
    FILE *enc = popen(encode, "w");
    if (!enc) {
        fprintf(stderr, "blurcli: could not start ffmpeg for encoding\n");
        pclose(dec);
        free(decode);
        free(encode);
        return 1;
    }

    const int rc = run_blur(&o, dec, enc, w, h);

    const int dec_rc = pclose(dec);
    const int enc_rc = pclose(enc);

    free(decode);
    free(encode);

    if (rc != 0)
        return rc;
    if (enc_rc != 0) {
        fprintf(stderr, "blurcli: the encoder exited with status %d\n", enc_rc);
        return 1;
    }
    if (dec_rc != 0)
        fprintf(stderr, "blurcli: warning - the decoder exited with status %d\n",
                dec_rc);

    if (o.verbose)
        fprintf(stderr, "blurcli: wrote %s\n", o.output);
    return 0;
}
