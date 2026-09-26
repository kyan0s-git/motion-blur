/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The OBS video filter.
 *
 * Structurally this follows obs-filters' gpu-delay.c, which is the only
 * shipped filter that buffers frames across ticks and therefore the only
 * reference for doing it correctly. Three things it taught us, each of
 * which is a bug if you get it wrong:
 *
 *  1. video_render is NOT called once per frame. It runs once per reference
 *     to the source: zero times when hidden, and several times with Studio
 *     Mode, a projector, or the properties dialog open. Only video_tick is
 *     once per frame. Without the processed_frame guard the accumulator
 *     advances two or three times per frame and the blur length silently
 *     changes when the user opens a window.
 *
 *  2. obs_source_process_filter_begin/end cannot be used for the
 *     accumulator: it owns exactly one texture, and its bypass fast path
 *     renders the parent directly without rendering to a texture at all.
 *     We own our own render targets instead.
 *
 *  3. The capture pass must force (ONE, ZERO) blending, otherwise the
 *     target is composited onto the render target rather than copied, and
 *     the frame we accumulate is not the frame the source produced.
 *
 * A filter cannot sample the timeline faster than the canvas ticks, so
 * DECIMATE mode only produces a true shutter integral when the canvas runs
 * above the recording frame rate. See frontend-helper.c.
 */
#include "motion-blur.h"

#define S_FRAMES "frames"
#define S_MODE "mode"
#define S_WEIGHTING "weighting"
#define S_CUSTOM_WEIGHTS "custom_weights"
#define S_AMOUNT "amount"
#define S_GAUSS_STD "gauss_std"
#define S_GAUSS_MEAN "gauss_mean"
#define S_GAUSS_BOUND "gauss_bound"
#define S_PHASE "phase_offset"
#define S_DRIVE_DIVISOR "drive_divisor"

#define T_(x) obs_module_text(x)

struct motion_blur {
	obs_source_t *source;

	/* Settings, copied out of obs_data_t in update() so the graphics
	 * thread never touches settings objects. */
	uint32_t frames;
	int mode; /* MB_MODE_ROLLING | MB_MODE_DECIMATE */
	int weighting;
	float amount;
	float gauss_std;
	float gauss_mean;
	float gauss_bound;
	uint32_t phase_offset;
	bool drive_divisor;
	float weights[MBLUR_MAX_FRAMES];

	/* Graphics state. Only touched on the graphics thread. */
	gs_effect_t *effect;
	gs_eparam_t *param_image;
	gs_eparam_t *param_weight;

	gs_texrender_t *capture;
	gs_texture_t *accum;    /* window being built */
	gs_texture_t *snapshot; /* last completed window, what we draw */
	gs_texrender_t *ring[MBLUR_MAX_FRAMES]; /* rolling mode only */

	uint32_t tex_cx, tex_cy;
	enum gs_color_space space;
	bool have_snapshot;

	uint32_t phase;  /* decimate: tap index within the window */
	uint32_t skip;   /* decimate: frames still dropped for phase alignment */
	uint32_t filled; /* rolling: valid ring entries */
	uint32_t slot;   /* rolling: next ring slot */

	bool processed_frame;
	bool rebuild;
	volatile long registered;
};

/* --------------------------------------------------------------- helpers */

static void free_textures(struct motion_blur *mb)
{
	obs_enter_graphics();
	if (mb->accum) {
		gs_texture_destroy(mb->accum);
		mb->accum = NULL;
	}
	if (mb->snapshot) {
		gs_texture_destroy(mb->snapshot);
		mb->snapshot = NULL;
	}
	if (mb->capture) {
		gs_texrender_destroy(mb->capture);
		mb->capture = NULL;
	}
	for (uint32_t i = 0; i < MBLUR_MAX_FRAMES; i++) {
		if (mb->ring[i]) {
			gs_texrender_destroy(mb->ring[i]);
			mb->ring[i] = NULL;
		}
	}
	obs_leave_graphics();

	mb->tex_cx = mb->tex_cy = 0;
	mb->have_snapshot = false;
	mb->phase = 0;
	mb->filled = 0;
	mb->slot = 0;
}

static void reset_window(struct motion_blur *mb)
{
	mb->phase = 0;
	mb->filled = 0;
	mb->slot = 0;
	mb->have_snapshot = false;
	mb->skip = (mb->mode == MB_MODE_DECIMATE)
			   ? (mb->phase_offset % (mb->frames ? mb->frames : 1))
			   : 0;
}

/*
 * Accumulating in a 16-bit float target is the right trade: 8-bit loses
 * about three bits on every add and bands visibly, while 32-bit doubles
 * VRAM and bandwidth for a precision nobody can see. gs_get_format_from_space
 * already returns RGBA16F for every HDR space, so SDR and HDR end up on one
 * code path.
 */
static enum gs_color_format accum_format(enum gs_color_space space)
{
	const enum gs_color_format fmt = gs_get_format_from_space(space);
	return (fmt == GS_RGBA16F || fmt == GS_RGBA32F) ? fmt : GS_RGBA16F;
}

static bool ensure_textures(struct motion_blur *mb, uint32_t cx, uint32_t cy,
			    enum gs_color_space space)
{
	const bool size_changed = (cx != mb->tex_cx || cy != mb->tex_cy);
	const bool space_changed = (space != mb->space);

	if (!size_changed && !space_changed && !mb->rebuild && mb->accum)
		return true;

	free_textures(mb);
	mb->rebuild = false;
	mb->tex_cx = cx;
	mb->tex_cy = cy;
	mb->space = space;

	if (cx == 0 || cy == 0)
		return false;

	const enum gs_color_format fmt = accum_format(space);

	mb->capture = gs_texrender_create(fmt, GS_ZS_NONE);
	mb->accum = gs_texture_create(cx, cy, fmt, 1, NULL, GS_RENDER_TARGET);
	mb->snapshot = gs_texture_create(cx, cy, fmt, 1, NULL, GS_RENDER_TARGET);
	if (!mb->capture || !mb->accum || !mb->snapshot) {
		blog(LOG_ERROR, "[motion-blur] could not allocate %ux%u targets",
		     cx, cy);
		free_textures(mb);
		return false;
	}

	if (mb->mode == MB_MODE_ROLLING) {
		for (uint32_t i = 0; i < mb->frames; i++) {
			mb->ring[i] = gs_texrender_create(fmt, GS_ZS_NONE);
			if (!mb->ring[i]) {
				blog(LOG_ERROR,
				     "[motion-blur] could not allocate ring slot %u",
				     i);
				free_textures(mb);
				return false;
			}
		}
	}

	reset_window(mb);
	return true;
}

/* Draws `tex` over the whole of the current render target. */
static void draw_texture(struct motion_blur *mb, gs_texture_t *tex,
			 const char *technique, float weight, uint32_t cx,
			 uint32_t cy)
{
	gs_effect_set_texture_srgb(mb->param_image, tex);
	if (mb->param_weight)
		gs_effect_set_float(mb->param_weight, weight);

	while (gs_effect_loop(mb->effect, technique))
		gs_draw_sprite(tex, 0, cx, cy);
}

static void set_target(struct motion_blur *mb, gs_texture_t *target,
		       uint32_t cx, uint32_t cy)
{
	gs_set_render_target_with_color_space(target, NULL, mb->space);
	gs_set_viewport(0, 0, (int)cx, (int)cy);
	gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);
}

/*
 * Renders the filter's target into `tr`. The source must be copied, not
 * composited, so the blend function is forced for the duration.
 */
static bool capture_target(struct motion_blur *mb, gs_texrender_t *tr,
			   uint32_t cx, uint32_t cy)
{
	obs_source_t *target = obs_filter_get_target(mb->source);
	obs_source_t *parent = obs_filter_get_parent(mb->source);
	if (!target || !parent)
		return false;

	gs_texrender_reset(tr);
	if (!gs_texrender_begin_with_color_space(tr, cx, cy, mb->space))
		return false;

	struct vec4 clear;
	vec4_zero(&clear);
	gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
	gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);

	const uint32_t flags = obs_source_get_output_flags(target);
	const bool custom_draw = (flags & OBS_SOURCE_CUSTOM_DRAW) != 0;
	const bool async = (flags & OBS_SOURCE_ASYNC) != 0;

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

	if (target == parent && !custom_draw && !async)
		obs_source_default_render(target);
	else
		obs_source_video_render(target);

	gs_blend_state_pop();
	gs_texrender_end(tr);
	return true;
}

/* ------------------------------------------------------------ the blend */

static void accumulate_decimate(struct motion_blur *mb, uint32_t cx,
				uint32_t cy)
{
	if (mb->skip > 0) {
		/* Dropping frames until the window boundary lands where the
		 * user asked keeps every emitted window complete, instead of
		 * making the first one short and differently weighted. */
		mb->skip--;
		return;
	}

	if (!capture_target(mb, mb->capture, cx, cy))
		return;

	gs_texture_t *captured = gs_texrender_get_texture(mb->capture);
	if (!captured)
		return;

	set_target(mb, mb->accum, cx, cy);

	gs_blend_state_push();
	/* Opening the window with a plain write saves a separate clear pass;
	 * every later tap adds to it. */
	if (mb->phase == 0)
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
	else
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ONE);
	gs_enable_blending(true);

	draw_texture(mb, captured, "Accumulate", mb->weights[mb->phase], cx, cy);
	gs_blend_state_pop();

	mb->phase++;
	if (mb->phase >= mb->frames) {
		gs_copy_texture(mb->snapshot, mb->accum);
		mb->have_snapshot = true;
		mb->phase = 0;
	}
}

static void accumulate_rolling(struct motion_blur *mb, uint32_t cx, uint32_t cy)
{
	if (!capture_target(mb, mb->ring[mb->slot], cx, cy))
		return;

	if (mb->filled < mb->frames)
		mb->filled++;

	const uint32_t n = mb->frames;
	const uint32_t taps = mb->filled;
	/* slot still points at the frame just written - the newest - so the
	 * oldest frame of a full window sits one past it. */
	const uint32_t oldest = (mb->slot + 1u + (n - taps)) % n;

	set_target(mb, mb->accum, cx, cy);
	gs_blend_state_push();
	gs_enable_blending(true);

	for (uint32_t t = 0; t < taps; t++) {
		gs_texture_t *tex =
			gs_texrender_get_texture(mb->ring[(oldest + t) % n]);
		if (!tex)
			continue;

		if (t == 0)
			gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
		else
			gs_blend_function(GS_BLEND_ONE, GS_BLEND_ONE);

		draw_texture(mb, tex, "Accumulate", mb->weights[n - taps + t], cx,
			     cy);
	}
	gs_blend_state_pop();

	mb->slot = (mb->slot + 1u) % n;

	if (mb->filled >= n) {
		gs_copy_texture(mb->snapshot, mb->accum);
		mb->have_snapshot = true;
	}
}

/* ----------------------------------------------------------- obs callbacks */

static const char *mb_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return T_("MotionBlur");
}

static void mb_update(void *data, obs_data_t *settings)
{
	struct motion_blur *mb = data;

	const uint32_t frames =
		(uint32_t)obs_data_get_int(settings, S_FRAMES);
	const int mode = (int)obs_data_get_int(settings, S_MODE);
	const uint32_t phase =
		(uint32_t)obs_data_get_int(settings, S_PHASE);

	/* A different tap count or mode changes how many render targets are
	 * needed; a different phase offset changes where windows start. Both
	 * are handled by rebuilding on the graphics thread, which also resets
	 * the window - moving the phase while a window is half full would
	 * otherwise emit one frame with the wrong weights. */
	if (frames != mb->frames || mode != mb->mode ||
	    phase != mb->phase_offset)
		mb->rebuild = true;

	mb->frames = frames;
	mb->mode = mode;
	mb->phase_offset = phase;
	mb->amount = (float)obs_data_get_double(settings, S_AMOUNT);
	mb->gauss_std = (float)obs_data_get_double(settings, S_GAUSS_STD);
	mb->gauss_mean = (float)obs_data_get_double(settings, S_GAUSS_MEAN);
	mb->gauss_bound = (float)obs_data_get_double(settings, S_GAUSS_BOUND);
	mb->drive_divisor = obs_data_get_bool(settings, S_DRIVE_DIVISOR);

	const char *wname = obs_data_get_string(settings, S_WEIGHTING);
	int w = mblur_weighting_parse(wname);
	if (w < 0)
		w = MBLUR_W_EQUAL;
	mb->weighting = w;

	/* The weights themselves come from the shared core, so the plugin and
	 * the offline tool cannot drift apart on what "gaussian" means. */
	mblur_config cfg;
	mblur_config_defaults(&cfg);
	cfg.width = 16; /* only the kernel fields matter here */
	cfg.height = 16;
	cfg.frames = mb->frames;
	cfg.weighting = (mblur_weighting)mb->weighting;
	cfg.amount = mb->amount;
	cfg.gauss_std = mb->gauss_std;
	cfg.gauss_mean = mb->gauss_mean;
	cfg.gauss_bound = mb->gauss_bound;

	float custom[MBLUR_MAX_FRAMES];
	if (mb->weighting == MBLUR_W_CUSTOM) {
		const char *csv = obs_data_get_string(settings, S_CUSTOM_WEIGHTS);
		const int count =
			mblur_weights_parse_csv(csv, custom, MBLUR_MAX_FRAMES);
		if (count > 0) {
			cfg.custom_weights = custom;
			cfg.custom_weight_count = (uint32_t)count;
		} else {
			blog(LOG_WARNING,
			     "[motion-blur] custom weights '%s' are not a valid "
			     "list; falling back to equal weighting",
			     csv ? csv : "");
			cfg.weighting = MBLUR_W_EQUAL;
			mb->weighting = MBLUR_W_EQUAL;
		}
	}

	if (mblur_weights_generate(&cfg, mb->weights) != MBLUR_OK) {
		for (uint32_t i = 0; i < mb->frames; i++)
			mb->weights[i] = 1.0f / (float)mb->frames;
	}

	mb_frontend_set_divisor_source(mb->source, mb->drive_divisor,
				       mb->frames);
}

static void *mb_create(obs_data_t *settings, obs_source_t *source)
{
	struct motion_blur *mb = bzalloc(sizeof(*mb));
	mb->source = source;
	mb->frames = 4;
	mb->mode = MB_MODE_DECIMATE;
	mb->space = GS_CS_SRGB;

	char *path = obs_module_file("effects/accumulate.effect");
	if (!path) {
		blog(LOG_ERROR, "[motion-blur] accumulate.effect is missing - "
				"the plugin's data directory was not installed");
		bfree(mb);
		return NULL;
	}

	obs_enter_graphics();
	char *errors = NULL;
	mb->effect = gs_effect_create_from_file(path, &errors);
	if (mb->effect) {
		mb->param_image = gs_effect_get_param_by_name(mb->effect, "image");
		mb->param_weight =
			gs_effect_get_param_by_name(mb->effect, "weight");
	}
	obs_leave_graphics();
	bfree(path);

	if (!mb->effect) {
		blog(LOG_ERROR, "[motion-blur] failed to compile "
				"accumulate.effect: %s",
		     errors ? errors : "no compiler output");
		bfree(errors);
		bfree(mb);
		return NULL;
	}
	bfree(errors);

	mb_update(mb, settings);
	return mb;
}

static void mb_destroy(void *data)
{
	struct motion_blur *mb = data;

	mb_frontend_set_divisor_source(mb->source, false, 0);
	free_textures(mb);

	obs_enter_graphics();
	if (mb->effect)
		gs_effect_destroy(mb->effect);
	obs_leave_graphics();

	bfree(mb);
}

static void mb_video_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(seconds);
	struct motion_blur *mb = data;
	/* This is the only callback that runs exactly once per frame, so it
	 * is where the per-frame guard is armed. */
	mb->processed_frame = false;
}

static enum gs_color_space
mb_video_get_color_space(void *data, size_t count,
			 const enum gs_color_space *preferred)
{
	struct motion_blur *mb = data;

	obs_source_t *target = obs_filter_get_target(mb->source);
	if (!target)
		return count ? preferred[0] : GS_CS_SRGB;

	/* The filter does not convert - it emits whatever space it was given,
	 * so it should report the target's space when the caller can take it. */
	const enum gs_color_space source_space =
		obs_source_get_color_space(target, count, preferred);
	for (size_t i = 0; i < count; i++) {
		if (preferred[i] == source_space)
			return source_space;
	}
	return count ? preferred[0] : GS_CS_SRGB;
}

static void mb_video_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct motion_blur *mb = data;

	obs_source_t *target = obs_filter_get_target(mb->source);
	obs_source_t *parent = obs_filter_get_parent(mb->source);
	if (!target || !parent || !mb->effect) {
		obs_source_skip_video_filter(mb->source);
		return;
	}

	const uint32_t cx = obs_source_get_base_width(target);
	const uint32_t cy = obs_source_get_base_height(target);
	if (cx == 0 || cy == 0) {
		obs_source_skip_video_filter(mb->source);
		return;
	}

	static const enum gs_color_space supported[] = {
		GS_CS_SRGB,
		GS_CS_SRGB_16F,
		GS_CS_709_EXTENDED,
	};
	const enum gs_color_space space = obs_source_get_color_space(
		target, OBS_COUNTOF(supported), supported);

	if (!ensure_textures(mb, cx, cy, space)) {
		obs_source_skip_video_filter(mb->source);
		return;
	}

	/*
	 * Advance the window at most once per frame. Studio Mode, projectors
	 * and the properties preview all call this again within the same
	 * tick; those extra calls must redraw the existing result rather than
	 * fold the same image into the accumulator two or three more times.
	 */
	if (!mb->processed_frame) {
		mb->processed_frame = true;

		const bool prev_srgb = gs_framebuffer_srgb_enabled();
		const bool prev_linear = gs_get_linear_srgb();

		/* Sampling through an sRGB view is what puts linear values in
		 * the accumulator, at no ALU cost. Averaging gamma-encoded
		 * values instead is the classic dark-blur artifact: a bright
		 * object crossing a dark background leaves grey mud rather
		 * than a bright streak. */
		gs_set_linear_srgb(true);
		gs_enable_framebuffer_srgb(true);

		gs_viewport_push();
		gs_projection_push();
		gs_matrix_push();
		gs_matrix_identity();

		gs_texture_t *prev_target = gs_get_render_target();
		const enum gs_color_space prev_space = gs_get_color_space();

		if (mb->mode == MB_MODE_DECIMATE)
			accumulate_decimate(mb, cx, cy);
		else
			accumulate_rolling(mb, cx, cy);

		gs_set_render_target_with_color_space(prev_target, NULL,
						      prev_space);

		gs_matrix_pop();
		gs_projection_pop();
		gs_viewport_pop();

		gs_enable_framebuffer_srgb(prev_srgb);
		gs_set_linear_srgb(prev_linear);
	}

	if (!mb->have_snapshot) {
		/* Nothing complete yet - pass the source through rather than
		 * showing a partially accumulated, too-dark frame. */
		obs_source_skip_video_filter(mb->source);
		return;
	}

	const bool prev_srgb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(true);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
	draw_texture(mb, mb->snapshot, "Resolve", 1.0f, cx, cy);
	gs_blend_state_pop();
	gs_enable_framebuffer_srgb(prev_srgb);
}

/* ------------------------------------------------------------- properties */

static bool weighting_modified(obs_properties_t *props, obs_property_t *p,
			       obs_data_t *settings)
{
	UNUSED_PARAMETER(p);
	const char *name = obs_data_get_string(settings, S_WEIGHTING);
	const bool custom = (mblur_weighting_parse(name) == MBLUR_W_CUSTOM);
	const bool gaussian =
		(mblur_weighting_parse(name) == MBLUR_W_GAUSSIAN) ||
		(mblur_weighting_parse(name) == MBLUR_W_GAUSSIAN_SYM) ||
		(mblur_weighting_parse(name) == MBLUR_W_GAUSSIAN_REV);

	obs_property_set_visible(obs_properties_get(props, S_CUSTOM_WEIGHTS),
				 custom);
	obs_property_set_visible(obs_properties_get(props, S_GAUSS_STD), gaussian);
	obs_property_set_visible(obs_properties_get(props, S_GAUSS_MEAN),
				 gaussian);
	obs_property_set_visible(obs_properties_get(props, S_GAUSS_BOUND),
				 gaussian);
	return true;
}

static bool mode_modified(obs_properties_t *props, obs_property_t *p,
			  obs_data_t *settings)
{
	UNUSED_PARAMETER(p);
	const bool decimate =
		obs_data_get_int(settings, S_MODE) == MB_MODE_DECIMATE;
	obs_property_set_visible(obs_properties_get(props, S_PHASE), decimate);
	obs_property_set_visible(obs_properties_get(props, S_DRIVE_DIVISOR),
				 decimate);
	return true;
}

static obs_properties_t *mb_get_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();

	obs_property_t *mode = obs_properties_add_list(
		props, S_MODE, T_("Mode"), OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(mode, T_("Mode.Decimate"), MB_MODE_DECIMATE);
	obs_property_list_add_int(mode, T_("Mode.Rolling"), MB_MODE_ROLLING);
	obs_property_set_long_description(mode, T_("Mode.Description"));
	obs_property_set_modified_callback(mode, mode_modified);

	obs_property_t *frames = obs_properties_add_int_slider(
		props, S_FRAMES, T_("Frames"), 2, 32, 1);
	obs_property_int_set_suffix(frames, T_("Frames.Suffix"));
	obs_property_set_long_description(frames, T_("Frames.Description"));

	obs_property_t *weighting = obs_properties_add_list(
		props, S_WEIGHTING, T_("Weighting"), OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_STRING);
	for (int i = 0; i < MBLUR_W_COUNT; i++) {
		const char *name = mblur_weighting_name((mblur_weighting)i);
		obs_property_list_add_string(weighting, name, name);
	}
	obs_property_set_modified_callback(weighting, weighting_modified);

	obs_properties_add_text(props, S_CUSTOM_WEIGHTS, T_("CustomWeights"),
				OBS_TEXT_DEFAULT);

	obs_property_t *amount = obs_properties_add_float_slider(
		props, S_AMOUNT, T_("Amount"), 0.05, 4.0, 0.05);
	obs_property_set_long_description(amount, T_("Amount.Description"));

	obs_properties_add_float_slider(props, S_GAUSS_STD, T_("GaussStd"), 0.1,
					8.0, 0.1);
	obs_properties_add_float_slider(props, S_GAUSS_MEAN, T_("GaussMean"), -4.0,
					4.0, 0.1);
	obs_properties_add_float_slider(props, S_GAUSS_BOUND, T_("GaussBound"),
					0.5, 8.0, 0.1);

	obs_properties_add_int_slider(props, S_PHASE, T_("PhaseOffset"), 0, 31, 1);

	obs_property_t *drive = obs_properties_add_bool(props, S_DRIVE_DIVISOR,
							T_("DriveDivisor"));
	obs_property_set_long_description(drive, T_("DriveDivisor.Description"));

	return props;
}

static void mb_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, S_FRAMES, 4);
	obs_data_set_default_int(settings, S_MODE, MB_MODE_DECIMATE);
	obs_data_set_default_string(settings, S_WEIGHTING,
				    mblur_weighting_name(MBLUR_W_EQUAL));
	obs_data_set_default_string(settings, S_CUSTOM_WEIGHTS, "1, 1, 1, 1");
	obs_data_set_default_double(settings, S_AMOUNT, 1.0);
	obs_data_set_default_double(settings, S_GAUSS_STD, 2.0);
	obs_data_set_default_double(settings, S_GAUSS_MEAN, 0.0);
	obs_data_set_default_double(settings, S_GAUSS_BOUND, 2.0);
	obs_data_set_default_int(settings, S_PHASE, 0);
	obs_data_set_default_bool(settings, S_DRIVE_DIVISOR, false);
}

struct obs_source_info motion_blur_filter_info = {
	.id = "motion_blur_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB,
	.get_name = mb_get_name,
	.create = mb_create,
	.destroy = mb_destroy,
	.update = mb_update,
	.get_defaults = mb_get_defaults,
	.get_properties = mb_get_properties,
	.video_tick = mb_video_tick,
	.video_render = mb_video_render,
	.video_get_color_space = mb_video_get_color_space,
};
