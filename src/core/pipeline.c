/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The accumulate/emit state machine.
 *
 * Two shapes of work, chosen by mode:
 *
 * DECIMATE - windows are disjoint, so each incoming frame is folded into a
 *   running accumulator and the accumulator is reset after it is resolved.
 *   Cost is one pass over one frame per input frame, independent of the
 *   blur length, and no ring buffer exists at all. This is the path that
 *   has to be fast.
 *
 * ROLLING - windows overlap and every tap's weight shifts by one position
 *   each frame, so the accumulator has to be rebuilt from a ring of the last
 *   N frames. The exception is a flat kernel, where every tap carries the
 *   same weight and the window can slide instead: add the arriving frame,
 *   subtract the departing one, and the cost collapses back to O(1).
 */
#include "internal.h"
#include "threadpool.h"

struct mblur_ctx {
    mblur_config cfg;
    mblur_layout layout;
    const mblur_kernels *k;
    mblur_backend backend;
    mblur_pool *pool;

    float weights[MBLUR_MAX_FRAMES];
    uint16_t w_q16[MBLUR_MAX_FRAMES];
    float custom[MBLUR_MAX_FRAMES];

    mblur_tap_lut *taplut; /* one per tap, weight folded in */
    uint16_t *oetf;        /* 64K entries, linear Q16 -> encoded Q16 */
    mblur_dither dither;

    mblur_acc *acc;
    uint8_t *out;
    uint8_t *ring; /* rolling mode only: frames x total_bytes, tight */

    size_t plane_offset[MBLUR_MAX_PLANES]; /* sample offset of each plane */
    size_t row_offset[MBLUR_MAX_PLANES];   /* first global row of each plane */
    size_t total_rows;

    uint64_t submitted;
    uint32_t phase;    /* decimate: tap index within the current window */
    uint32_t skip;     /* decimate: frames still to drop for phase alignment */
    uint32_t filled;   /* rolling: valid frames in the ring */
    uint32_t slot;     /* rolling: next ring slot to write */
    int have_output;
    int64_t out_pts;

    int use_lut;        /* linear-light table path */
    int resolve_alpha_phase; /* byte index of alpha in a pixel, or -1 */
    int sliding;        /* rolling mode flat-kernel fast path */
    uint32_t lut_channels;
    size_t memory_bytes;
};

/* ------------------------------------------------------------ helpers */

void *mblur_aligned_alloc(size_t bytes)
{
#if defined(_WIN32)
    return _aligned_malloc(bytes, MBLUR_ALIGN);
#else
    void *p = NULL;
    if (bytes == 0)
        bytes = MBLUR_ALIGN;
    bytes = (bytes + MBLUR_ALIGN - 1) & ~(size_t)(MBLUR_ALIGN - 1);
    if (posix_memalign(&p, MBLUR_ALIGN, bytes) != 0)
        return NULL;
    return p;
#endif
}

void mblur_aligned_free(void *p)
{
#if defined(_WIN32)
    _aligned_free(p);
#else
    free(p);
#endif
}

void mblur_build_dither(mblur_dither *d, int enabled)
{
    memset(d, 0, sizeof(*d));
    if (!enabled)
        return;

    /*
     * A triangular-PDF pattern with zero mean, spanning roughly one output
     * LSB (256 accumulator counts). Deterministic by position so that every
     * backend produces identical bytes. Using a fixed xorshift seed keeps
     * the values white rather than structured, which avoids the crosshatch
     * a small ordered matrix would print onto flat gradients.
     */
    uint32_t s = 0x9E3779B9u;
    int t[MBLUR_DITHER_PERIOD];

    /* Draw half the values, then mirror them, so the table sums to exactly
     * zero instead of merely close to it. A residual bias here would shift
     * the brightness of every frame by a fraction of a step, which is the
     * kind of thing that only shows up as a complaint about washed-out
     * blacks months later. */
    for (uint32_t i = 0; i < MBLUR_DITHER_PERIOD / 2; i++) {
        uint32_t r = mblur_rng_next(&s);
        int a = (int)(r & 0x7Fu);
        int b = (int)((r >> 8) & 0x7Fu);
        t[i] = (a + b) / 2 + 1; /* triangular over 1..128 */
        t[i + MBLUR_DITHER_PERIOD / 2] = -t[i];
    }

    /* Shuffle so the sign pattern is not a visible comb. Fisher-Yates with
     * the same deterministic stream keeps every backend in agreement. */
    for (uint32_t i = MBLUR_DITHER_PERIOD - 1; i > 0; i--) {
        uint32_t j = mblur_rng_next(&s) % (i + 1u);
        int tmp = t[i];
        t[i] = t[j];
        t[j] = tmp;
    }

    for (uint32_t i = 0; i < MBLUR_DITHER_PERIOD; i++) {
        d->pos[i] = (uint16_t)(t[i] > 0 ? t[i] : 0);
        d->neg[i] = (uint16_t)(t[i] < 0 ? -t[i] : 0);
    }
    /* Duplicate so a vector load at any phase stays contiguous. */
    for (uint32_t i = MBLUR_DITHER_PERIOD; i < MBLUR_DITHER_ENTRIES; i++) {
        d->pos[i] = d->pos[i - MBLUR_DITHER_PERIOD];
        d->neg[i] = d->neg[i - MBLUR_DITHER_PERIOD];
    }
}

static void row_lookup(const mblur_ctx *c, size_t grow, uint32_t *plane,
                       size_t *lrow)
{
    for (uint32_t p = c->layout.plane_count; p-- > 0;) {
        if (grow >= c->row_offset[p]) {
            *plane = p;
            *lrow = grow - c->row_offset[p];
            return;
        }
    }
    *plane = 0;
    *lrow = 0;
}

/* --------------------------------------------------------------- jobs */

typedef enum {
    JOB_ACCUM_ADD,
    JOB_ACCUM_SUB,
    JOB_ACCUM_STORE, /* first tap of a window: write instead of accumulate */
    JOB_RESOLVE,
    JOB_ZERO,
    JOB_RING_STORE
} job_kind;

typedef struct {
    mblur_ctx *c;
    job_kind kind;
    const uint8_t *src_plane[MBLUR_MAX_PLANES];
    size_t src_stride[MBLUR_MAX_PLANES];
    const mblur_tap_lut *lut;
    uint16_t w;
    uint8_t *ring_dst; /* JOB_RING_STORE: base of the target ring slot */
} job;

static void job_run(void *user, size_t begin, size_t end, unsigned worker)
{
    (void)worker;
    job *j = (job *)user;
    mblur_ctx *c = j->c;
    const mblur_kernels *k = c->k;

    for (size_t grow = begin; grow < end; grow++) {
        uint32_t p;
        size_t lrow;
        row_lookup(c, grow, &p, &lrow);

        const mblur_plane_info *pi = &c->layout.plane[p];
        const size_t n = pi->bytes_wide;
        const size_t base = c->plane_offset[p] + lrow * n;
        mblur_acc *acc = c->acc + base;

        switch (j->kind) {
        case JOB_ZERO:
            k->acc_zero(acc, n);
            break;

        case JOB_ACCUM_ADD: {
            const uint8_t *src = j->src_plane[p] + lrow * j->src_stride[p];
            if (c->use_lut)
                k->accum_lut(acc, src, n, j->lut->table, c->lut_channels);
            else
                k->accum_mul(acc, src, n, j->w);
            break;
        }

        case JOB_ACCUM_STORE: {
            const uint8_t *src = j->src_plane[p] + lrow * j->src_stride[p];
            if (c->use_lut)
                k->store_lut(acc, src, n, j->lut->table, c->lut_channels);
            else
                k->store_mul(acc, src, n, j->w);
            break;
        }

        case JOB_RING_STORE: {
            const uint8_t *src = j->src_plane[p] + lrow * j->src_stride[p];
            memcpy(j->ring_dst + base, src, n);
            break;
        }

        case JOB_ACCUM_SUB: {
            const uint8_t *src = j->src_plane[p] + lrow * j->src_stride[p];
            if (c->use_lut)
                k->accum_lut_sub(acc, src, n, j->lut->table, c->lut_channels);
            else
                k->accum_mul_sub(acc, src, n, j->w);
            break;
        }

        case JOB_RESOLVE: {
            uint8_t *dst = c->out + base;
            if (c->use_lut)
                k->resolve_oetf(dst, acc, n, c->oetf, c->resolve_alpha_phase,
                                &c->dither, base);
            else
                k->resolve_direct(dst, acc, n, &c->dither, base);
            break;
        }
        }
    }
}

static void run_job(mblur_ctx *c, job *j)
{
    mblur_pool_run(c->pool, c->total_rows, job_run, j);
}

static void set_src_from_frame(mblur_ctx *c, job *j, const mblur_frame *f)
{
    for (uint32_t p = 0; p < c->layout.plane_count; p++) {
        j->src_plane[p] = f->plane[p];
        j->src_stride[p] = f->stride[p] ? f->stride[p]
                                        : c->layout.plane[p].bytes_wide;
    }
}

static void set_src_from_ring(mblur_ctx *c, job *j, uint32_t slot)
{
    uint8_t *frame = c->ring + (size_t)slot * c->layout.total_bytes;
    for (uint32_t p = 0; p < c->layout.plane_count; p++) {
        j->src_plane[p] = frame + c->plane_offset[p];
        j->src_stride[p] = c->layout.plane[p].bytes_wide;
    }
}

/*
 * Copying a whole frame into the ring is as much memory traffic as a blend
 * pass, so it runs across the pool rather than on the submitting thread -
 * otherwise rolling mode pays a fully serial memcpy per frame and the extra
 * cores sit idle for a third of the budget.
 */
static void ring_store(mblur_ctx *c, uint32_t slot, const mblur_frame *f)
{
    job j = {0};
    j.c = c;
    j.kind = JOB_RING_STORE;
    j.ring_dst = c->ring + (size_t)slot * c->layout.total_bytes;
    set_src_from_frame(c, &j, f);
    run_job(c, &j);
}

/* ------------------------------------------------------------- config */

void mblur_config_defaults(mblur_config *cfg)
{
    if (!cfg)
        return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->format = MBLUR_FMT_RGBA8;
    cfg->transfer = MBLUR_TRC_SRGB;
    cfg->frames = 8;
    cfg->mode = MBLUR_MODE_DECIMATE;
    cfg->weighting = MBLUR_W_EQUAL;
    cfg->amount = 1.0f;
    cfg->gauss_std = 2.0f;
    cfg->gauss_mean = 0.0f;
    cfg->gauss_bound = 2.0f;
    cfg->linear_light = 1;
    cfg->dither = 1;
    cfg->threads = 0;
}

mblur_status mblur_config_validate(const mblur_config *cfg, const char **reason)
{
    const char *why = NULL;
    mblur_status st = MBLUR_OK;

    if (!cfg) {
        why = "config is NULL";
        st = MBLUR_ERR_INVALID;
    } else if (cfg->width == 0 || cfg->height == 0) {
        why = "width and height must be non-zero";
        st = MBLUR_ERR_INVALID;
    } else if (cfg->frames == 0 || cfg->frames > MBLUR_MAX_FRAMES) {
        why = "frames must be between 1 and " "64";
        st = MBLUR_ERR_INVALID;
    } else if (cfg->format >= MBLUR_FMT_COUNT) {
        why = "unknown pixel format";
        st = MBLUR_ERR_INVALID;
    } else if (cfg->transfer >= MBLUR_TRC_COUNT) {
        why = "unknown transfer function";
        st = MBLUR_ERR_INVALID;
    } else if (cfg->weighting >= MBLUR_W_COUNT) {
        why = "unknown weighting kernel";
        st = MBLUR_ERR_INVALID;
    } else if (cfg->weighting == MBLUR_W_CUSTOM &&
               (!cfg->custom_weights || cfg->custom_weight_count == 0)) {
        why = "custom weighting needs custom_weights";
        st = MBLUR_ERR_INVALID;
    } else if (cfg->amount <= 0.0f) {
        why = "amount must be greater than zero";
        st = MBLUR_ERR_INVALID;
    } else if ((cfg->format == MBLUR_FMT_NV12 || cfg->format == MBLUR_FMT_I420) &&
               ((cfg->width | cfg->height) & 1u)) {
        why = "4:2:0 formats need even width and height";
        st = MBLUR_ERR_INVALID;
    } else if ((cfg->format == MBLUR_FMT_NV12 || cfg->format == MBLUR_FMT_I420) &&
               cfg->linear_light) {
        /* Linearising Y and blending chroma as if it were light is wrong in
         * a way that shows up as hue shifts on coloured motion. Converting
         * to RGB first would cost more than the blend. Say so rather than
         * silently doing something defensible-looking. */
        why = "linear_light needs an RGB format; blend YUV in its own space "
              "or convert to RGBA first";
        st = MBLUR_ERR_UNSUPPORTED;
    }

    if (reason)
        *reason = why;
    return st;
}

/* ------------------------------------------------------ create/destroy */

static int alloc_all(mblur_ctx *c)
{
    const size_t n = c->layout.total_bytes;

    c->acc = mblur_aligned_alloc(n * sizeof(mblur_acc));
    c->out = mblur_aligned_alloc(n);
    if (!c->acc || !c->out)
        return 0;
    c->memory_bytes = n * sizeof(mblur_acc) + n;

    if (c->cfg.mode == MBLUR_MODE_ROLLING) {
        c->ring = mblur_aligned_alloc(n * c->cfg.frames);
        if (!c->ring)
            return 0;
        c->memory_bytes += n * c->cfg.frames;
    }

    c->taplut = mblur_aligned_alloc(sizeof(mblur_tap_lut) * c->cfg.frames);
    if (!c->taplut)
        return 0;
    c->memory_bytes += sizeof(mblur_tap_lut) * c->cfg.frames;

    if (c->use_lut) {
        c->oetf = mblur_aligned_alloc(MBLUR_OETF_ENTRIES * sizeof(uint16_t));
        if (!c->oetf)
            return 0;
        c->memory_bytes += MBLUR_OETF_ENTRIES * sizeof(uint16_t);
    }
    return 1;
}

mblur_ctx *mblur_create(const mblur_config *cfg, mblur_backend backend,
                        const char **reason)
{
    const char *why = NULL;
    if (reason)
        *reason = NULL;

    if (mblur_config_validate(cfg, &why) != MBLUR_OK) {
        if (reason)
            *reason = why;
        return NULL;
    }

    const mblur_backend resolved = mblur_resolve_backend(backend);
    const mblur_kernels *k = mblur_kernels_for(resolved);
    if (!k) {
        if (reason)
            *reason = "requested backend is not available on this machine";
        return NULL;
    }

    mblur_ctx *c = calloc(1, sizeof(*c));
    if (!c) {
        if (reason)
            *reason = "out of memory";
        return NULL;
    }

    c->cfg = *cfg;
    c->k = k;
    c->backend = resolved;

    if (mblur_layout_init(&c->layout, cfg->format, cfg->width, cfg->height) !=
        MBLUR_OK) {
        free(c);
        if (reason)
            *reason = "unsupported pixel format";
        return NULL;
    }

    /* Copy custom weights so the caller's array need not outlive the call. */
    if (cfg->weighting == MBLUR_W_CUSTOM) {
        uint32_t m = cfg->custom_weight_count;
        if (m > MBLUR_MAX_FRAMES)
            m = MBLUR_MAX_FRAMES;
        memcpy(c->custom, cfg->custom_weights, m * sizeof(float));
        c->cfg.custom_weights = c->custom;
        c->cfg.custom_weight_count = m;
    }

    if (mblur_weights_generate(&c->cfg, c->weights) != MBLUR_OK) {
        free(c);
        if (reason)
            *reason = "could not build the weight kernel";
        return NULL;
    }

    /* The transfer function only has to be undone if it is not already
     * linear; when it is, the cheaper multiply path gives the same answer. */
    c->use_lut = c->cfg.linear_light && c->cfg.transfer != MBLUR_TRC_LINEAR;
    c->lut_channels = (c->layout.plane[0].alpha_index >= 0 && c->use_lut) ? 4u : 1u;
    c->resolve_alpha_phase = c->use_lut ? c->layout.plane[0].alpha_index : -1;

    for (uint32_t i = 0; i < c->cfg.frames; i++) {
        double q = (double)c->weights[i] * 65536.0;
        if (q > 65535.0)
            q = 65535.0;
        if (q < 0.0)
            q = 0.0;
        c->w_q16[i] = (uint16_t)(q + 0.5);
    }

    /* A flat kernel lets rolling mode slide its window instead of rebuilding
     * it, which is worth detecting rather than assuming from the enum: a
     * custom list of identical weights qualifies too. */
    /* Compare the float weights, not their quantised forms: the tap tables
     * are built from the floats, so only exact equality there guarantees
     * every tap shares one table - which is what makes sliding valid. */
    c->sliding = (c->cfg.mode == MBLUR_MODE_ROLLING);
    for (uint32_t i = 1; i < c->cfg.frames && c->sliding; i++) {
        if (c->weights[i] != c->weights[0])
            c->sliding = 0;
    }
    if (c->cfg.frames == 1)
        c->sliding = 0;

    if (!alloc_all(c)) {
        mblur_destroy(c);
        if (reason)
            *reason = "out of memory";
        return NULL;
    }

    for (uint32_t i = 0; i < c->cfg.frames; i++)
        mblur_build_tap_lut(&c->taplut[i], c->weights[i], c->cfg.transfer,
                            c->cfg.linear_light, &c->layout.plane[0]);
    if (c->oetf)
        mblur_build_oetf_table(c->oetf, c->cfg.transfer, c->cfg.linear_light);

    mblur_build_dither(&c->dither, c->cfg.dither);

    size_t sample_off = 0, row_off = 0;
    for (uint32_t p = 0; p < c->layout.plane_count; p++) {
        c->plane_offset[p] = sample_off;
        c->row_offset[p] = row_off;
        sample_off += (size_t)c->layout.plane[p].bytes_wide *
                      c->layout.plane[p].rows;
        row_off += c->layout.plane[p].rows;
    }
    c->total_rows = row_off;

    c->pool = mblur_pool_create(c->cfg.threads);

    mblur_reset(c);
    return c;
}

void mblur_destroy(mblur_ctx *c)
{
    if (!c)
        return;
    mblur_pool_destroy(c->pool);
    mblur_aligned_free(c->acc);
    mblur_aligned_free(c->out);
    mblur_aligned_free(c->ring);
    mblur_aligned_free(c->taplut);
    mblur_aligned_free(c->oetf);
    free(c);
}

void mblur_reset(mblur_ctx *c)
{
    if (!c)
        return;
    job j = {0};
    j.c = c;
    j.kind = JOB_ZERO;
    run_job(c, &j);

    c->submitted = 0;
    c->phase = 0;
    c->filled = 0;
    c->slot = 0;
    c->have_output = 0;
    c->out_pts = 0;
    c->skip = (c->cfg.mode == MBLUR_MODE_DECIMATE)
                  ? (c->cfg.phase_offset % c->cfg.frames)
                  : 0;
}

/* ---------------------------------------------------------- processing */

static void resolve_now(mblur_ctx *c, int64_t pts)
{
    job j = {0};
    j.c = c;
    j.kind = JOB_RESOLVE;
    run_job(c, &j);
    c->have_output = 1;
    c->out_pts = pts;
}

static mblur_status submit_decimate(mblur_ctx *c, const mblur_frame *in)
{
    if (c->skip > 0) {
        /* Dropping frames until the window boundary lands where the caller
         * asked keeps every emitted window complete, rather than making the
         * first one short and differently weighted. */
        c->skip--;
        return MBLUR_OK;
    }

    job j = {0};
    j.c = c;
    /* Opening the window with a store means the accumulator never needs a
     * separate clear - one whole-frame write saved per window. */
    j.kind = (c->phase == 0) ? JOB_ACCUM_STORE : JOB_ACCUM_ADD;
    j.lut = &c->taplut[c->phase];
    j.w = c->w_q16[c->phase];
    set_src_from_frame(c, &j, in);
    run_job(c, &j);

    c->phase++;
    if (c->phase >= c->cfg.frames) {
        resolve_now(c, in->pts);
        c->phase = 0;
    }
    return MBLUR_OK;
}

static mblur_status submit_rolling(mblur_ctx *c, const mblur_frame *in)
{
    const uint32_t n = c->cfg.frames;

    if (c->sliding) {
        /* Subtract the frame about to be evicted before overwriting it. */
        if (c->filled == n) {
            job s = {0};
            s.c = c;
            s.kind = JOB_ACCUM_SUB;
            s.lut = &c->taplut[0];
            s.w = c->w_q16[0];
            set_src_from_ring(c, &s, c->slot);
            run_job(c, &s);
        }
        ring_store(c, c->slot, in);

        job a = {0};
        a.c = c;
        a.kind = JOB_ACCUM_ADD;
        a.lut = &c->taplut[0];
        a.w = c->w_q16[0];
        set_src_from_ring(c, &a, c->slot);
        run_job(c, &a);
    } else {
        ring_store(c, c->slot, in);

        /* Tap 0 is the oldest frame in the window. With `slot` pointing at
         * the frame just written (the newest), the oldest sits one past it. */
        const uint32_t taps = (c->filled < n) ? c->filled + 1u : n;
        const uint32_t oldest = (c->slot + 1u + (n - taps)) % n;
        for (uint32_t t = 0; t < taps; t++) {
            const uint32_t tap_index = n - taps + t;
            job a = {0};
            a.c = c;
            a.kind = (t == 0) ? JOB_ACCUM_STORE : JOB_ACCUM_ADD;
            a.lut = &c->taplut[tap_index];
            a.w = c->w_q16[tap_index];
            set_src_from_ring(c, &a, (oldest + t) % n);
            run_job(c, &a);
        }
    }

    if (c->filled < n)
        c->filled++;
    c->slot = (c->slot + 1u) % n;

    if (c->filled >= n)
        resolve_now(c, in->pts);

    return MBLUR_OK;
}

mblur_status mblur_submit(mblur_ctx *c, const mblur_frame *in)
{
    if (!c || !in)
        return MBLUR_ERR_INVALID;
    for (uint32_t p = 0; p < c->layout.plane_count; p++) {
        if (!in->plane[p])
            return MBLUR_ERR_INVALID;
    }

    c->have_output = 0;
    c->submitted++;

    return (c->cfg.mode == MBLUR_MODE_DECIMATE) ? submit_decimate(c, in)
                                                : submit_rolling(c, in);
}

mblur_status mblur_pull(mblur_ctx *c, mblur_frame *out)
{
    if (!c || !out)
        return MBLUR_ERR_INVALID;
    if (!c->have_output)
        return MBLUR_NO_OUTPUT;

    memset(out, 0, sizeof(*out));
    for (uint32_t p = 0; p < c->layout.plane_count; p++) {
        out->plane[p] = c->out + c->plane_offset[p];
        out->stride[p] = c->layout.plane[p].bytes_wide;
    }
    out->pts = c->out_pts;
    c->have_output = 0;
    return MBLUR_OK;
}

uint32_t mblur_latency_frames(const mblur_ctx *c)
{
    if (!c)
        return 0;
    return (c->cfg.frames > 0) ? c->cfg.frames - 1u : 0u;
}

/* ------------------------------------------------------ introspection */

mblur_backend mblur_ctx_backend(const mblur_ctx *c)
{
    return c ? c->backend : MBLUR_BACKEND_AUTO;
}

size_t mblur_ctx_memory_usage(const mblur_ctx *c)
{
    return c ? c->memory_bytes : 0;
}
