/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MOTION_BLUR_OBS_H
#define MOTION_BLUR_OBS_H

#include <obs-module.h>
#include "mblur/mblur.h"

enum mb_mode {
	MB_MODE_DECIMATE = 0,
	MB_MODE_ROLLING = 1,
};

extern struct obs_source_info motion_blur_filter_info;

/*
 * Frontend integration. Registering a filter instance means "when a
 * recording starts, divide the encoder's frame rate by this many" - the one
 * mechanism OBS offers for encoding below the canvas rate. Passing
 * enabled = false unregisters. Safe to call from any thread and safe to
 * call when the frontend API is not available, in which case it does
 * nothing but say so once.
 */
void mb_frontend_set_divisor_source(obs_source_t *source, bool enabled,
				    uint32_t frames);

void mb_frontend_init(void);
void mb_frontend_shutdown(void);

#endif /* MOTION_BLUR_OBS_H */
