/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Making the canvas run faster than the recording.
 *
 * A filter cannot invent temporal samples. It is handed exactly one image
 * per graphics tick, so a true shutter integral needs the canvas ticking
 * above the recording frame rate - 240 fps canvas blended 4:1 into a 60 fps
 * recording, which is what this file arranges.
 *
 * The mechanism is obs_encoder_set_frame_rate_divisor(). It is a first-class
 * libobs feature: video-io decimates with an explicit counter so encoders
 * started together land on the same frames, the GPU (NVENC texture) encode
 * path honours it, and encoder groups compute an LCM so a 240 fps stream and
 * a 60 fps recording keep their keyframes aligned. It can only be set while
 * the encoder is stopped, which is why this hooks RECORDING_STARTING.
 *
 * What does not work, so nobody tries it again: per-canvas frame rates.
 * obs_init_video_mix() overwrites every auxiliary mix's fps with the main
 * canvas's, with a comment saying the main view's graphics thread drives all
 * frame output. obs_view_add2() and the canvas API give you another
 * resolution and scene, never another clock. There is one graphics thread
 * and one frame interval, and that is by design.
 */
#include "motion-blur.h"

#if !defined(MBLUR_HAVE_OBS_FRONTEND)

/*
 * Built where obs-frontend-api is unavailable. The filter and both blur
 * modes work normally; only the automatic divisor is missing, and the user
 * is told once rather than left wondering why the recording still runs at
 * the canvas rate.
 */
void mb_frontend_set_divisor_source(obs_source_t *source, bool enabled,
				    uint32_t frames)
{
	UNUSED_PARAMETER(source);
	UNUSED_PARAMETER(frames);

	static bool warned = false;
	if (enabled && !warned) {
		warned = true;
		blog(LOG_WARNING,
		     "[motion-blur] this build has no obs-frontend-api, so the "
		     "recording frame rate divisor cannot be set "
		     "automatically. Decimate mode still blends correctly, but "
		     "the recording will run at the full canvas rate unless "
		     "something else divides it.");
	}
}

void mb_frontend_init(void)
{
}

void mb_frontend_shutdown(void)
{
}

#else /* MBLUR_HAVE_OBS_FRONTEND */

#include <obs-frontend-api.h>

/*
 * A mutex, without pulling in OBS's <util/threading.h>. That header reaches
 * for <pthread.h>, which on Windows means OBS's bundled w32-pthreads - a
 * separate library with its own DLL and import lib. Dragging that in for one
 * mutex would defeat the point of building against OBS's shipped binaries,
 * so the lock is spelled out here the same way the engine's thread pool does
 * it.
 */
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
typedef CRITICAL_SECTION mb_mutex;
#define MB_MUTEX_INIT(m) (InitializeCriticalSection(m), 0)
#define MB_MUTEX_FREE(m) DeleteCriticalSection(m)
#define MB_LOCK(m) EnterCriticalSection(m)
#define MB_UNLOCK(m) LeaveCriticalSection(m)
#else
#include <pthread.h>
typedef pthread_mutex_t mb_mutex;
#define MB_MUTEX_INIT(m) pthread_mutex_init(m, NULL)
#define MB_MUTEX_FREE(m) pthread_mutex_destroy(m)
#define MB_LOCK(m) pthread_mutex_lock(m)
#define MB_UNLOCK(m) pthread_mutex_unlock(m)
#endif

/*
 * At most one filter drives the divisor. Two filters asking for different
 * divisors is a contradiction - there is only one recording encoder - so the
 * most recent one wins and the situation is logged rather than resolved
 * silently.
 */
static obs_weak_source_t *g_driver;
static uint32_t g_divisor;
static mb_mutex g_lock;
static bool g_initialised;

static void apply_divisor_to_recording(void)
{
	uint32_t divisor;

	MB_LOCK(&g_lock);
	divisor = g_divisor;
	obs_source_t *driver = g_driver ? obs_weak_source_get_source(g_driver)
					: NULL;
	MB_UNLOCK(&g_lock);

	if (!driver)
		return;
	obs_source_release(driver);

	if (divisor < 2)
		return;

	obs_output_t *output = obs_frontend_get_recording_output();
	if (!output) {
		blog(LOG_WARNING, "[motion-blur] no recording output to "
				  "configure");
		return;
	}

	struct obs_video_info ovi;
	const bool have_ovi = obs_get_video_info(&ovi);
	const double canvas_fps =
		have_ovi ? (double)ovi.fps_num / (double)ovi.fps_den : 0.0;

	bool applied = false;
	for (size_t i = 0; i < MAX_OUTPUT_VIDEO_ENCODERS; i++) {
		obs_encoder_t *enc = obs_output_get_video_encoder2(output, i);
		if (!enc)
			continue;
		if (obs_encoder_set_frame_rate_divisor(enc, divisor)) {
			applied = true;
			blog(LOG_INFO,
			     "[motion-blur] recording encoder %zu set to "
			     "1/%u of the canvas: %.3f fps canvas -> %.3f fps "
			     "recorded",
			     i, divisor, canvas_fps,
			     canvas_fps / (double)divisor);
		} else {
			blog(LOG_WARNING,
			     "[motion-blur] could not set a frame rate divisor "
			     "on recording encoder %zu; it may already be "
			     "running",
			     i);
		}
	}

	if (!applied)
		blog(LOG_WARNING, "[motion-blur] no encoder accepted the "
				  "divisor; the recording will run at the full "
				  "canvas rate");

	obs_output_release(output);
}

/*
 * The blur is only real if the canvas actually delivers distinct frames at
 * the higher rate. Two ways it does not, both of which look like "the blur
 * does nothing" or "the blur pulses", and neither of which the plugin can
 * detect from inside a filter - so they get said plainly in the log.
 */
static void warn_about_canvas(uint32_t divisor)
{
	struct obs_video_info ovi;
	if (!obs_get_video_info(&ovi))
		return;

	const double canvas_fps = (double)ovi.fps_num / (double)ovi.fps_den;
	const double recorded = canvas_fps / (double)divisor;

	blog(LOG_INFO,
	     "[motion-blur] canvas %.3f fps, %u frames per blur window, "
	     "recording %.3f fps",
	     canvas_fps, divisor, recorded);

	if (divisor < 2) {
		blog(LOG_WARNING,
		     "[motion-blur] a divisor below 2 means every canvas frame "
		     "is encoded, so decimate mode has no extra samples to "
		     "integrate and behaves like rolling blend");
	}

	blog(LOG_INFO,
	     "[motion-blur] for a true shutter integral the canvas must run at "
	     "%u x the frame rate you want recorded, AND the captured "
	     "application must actually present that fast - a %.0f fps game on "
	     "a %.0f fps canvas is captured %u times per unique frame and the "
	     "blur is a no-op",
	     divisor, recorded, canvas_fps, divisor);
}

static void on_frontend_event(enum obs_frontend_event event, void *data)
{
	UNUSED_PARAMETER(data);

	switch (event) {
	case OBS_FRONTEND_EVENT_RECORDING_STARTING:
		apply_divisor_to_recording();
		break;
	default:
		break;
	}
}

void mb_frontend_set_divisor_source(obs_source_t *source, bool enabled,
				    uint32_t frames)
{
	if (!g_initialised)
		return;

	MB_LOCK(&g_lock);

	if (!enabled) {
		/* Only clear the registration if this source owns it -
		 * otherwise disabling one filter would silently switch off
		 * another one that is still driving the divisor. A weak ref
		 * to an already-destroyed source resolves to NULL, and that
		 * registration is dead either way, so clear it too rather
		 * than leaving a divisor nobody can turn off. */
		obs_source_t *current =
			g_driver ? obs_weak_source_get_source(g_driver) : NULL;
		if (current == source || current == NULL) {
			obs_weak_source_release(g_driver);
			g_driver = NULL;
			g_divisor = 0;
		}
		if (current)
			obs_source_release(current);
		MB_UNLOCK(&g_lock);
		return;
	}

	obs_source_t *current = g_driver ? obs_weak_source_get_source(g_driver)
					 : NULL;
	if (current && current != source) {
		blog(LOG_WARNING,
		     "[motion-blur] '%s' is taking over the recording frame "
		     "rate divisor from '%s'; only one filter can drive it",
		     obs_source_get_name(source), obs_source_get_name(current));
	}
	if (current)
		obs_source_release(current);

	obs_weak_source_release(g_driver);
	g_driver = obs_source_get_weak_source(source);
	g_divisor = frames;

	MB_UNLOCK(&g_lock);

	warn_about_canvas(frames);
}

void mb_frontend_init(void)
{
	if (MB_MUTEX_INIT(&g_lock) != 0) {
		blog(LOG_ERROR, "[motion-blur] could not create frontend lock; "
				"the frame rate divisor will not be applied");
		return;
	}
	g_initialised = true;
	obs_frontend_add_event_callback(on_frontend_event, NULL);
}

void mb_frontend_shutdown(void)
{
	if (!g_initialised)
		return;

	obs_frontend_remove_event_callback(on_frontend_event, NULL);

	MB_LOCK(&g_lock);
	obs_weak_source_release(g_driver);
	g_driver = NULL;
	g_divisor = 0;
	MB_UNLOCK(&g_lock);

	MB_MUTEX_FREE(&g_lock);
	g_initialised = false;
}

#endif /* MBLUR_HAVE_OBS_FRONTEND */
