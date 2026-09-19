/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "motion-blur.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("motion-blur", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Temporal motion blur: blends several canvas frames into one "
	       "encoded frame.";
}

MODULE_EXPORT const char *obs_module_name(void)
{
	return "Motion Blur";
}

bool obs_module_load(void)
{
	/*
	 * Logged at INFO on purpose. OBS refuses a plugin built against a
	 * newer libobs than the host and reports it only at LOG_DEBUG, so
	 * without a line like this the user's symptom is "the filter isn't
	 * there" with nothing in the log to explain it. Seeing this line, or
	 * not seeing it, is the first question to ask in any bug report.
	 */
	blog(LOG_INFO, "[motion-blur] loaded, engine %s (libobs %s)",
	     mblur_version_string(), obs_get_version_string());

	obs_register_source(&motion_blur_filter_info);
	mb_frontend_init();
	return true;
}

void obs_module_unload(void)
{
	mb_frontend_shutdown();
	blog(LOG_INFO, "[motion-blur] unloaded");
}
