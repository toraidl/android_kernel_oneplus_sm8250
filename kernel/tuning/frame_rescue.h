/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Frame Rescue Lite state shared by the frame-group implementation.
 *
 * The state is intentionally inert in this commit. Deadline timers,
 * utilization clamps and userspace controls are separate changes.
 */

#ifndef _FRAME_RESCUE_H
#define _FRAME_RESCUE_H

#include <linux/types.h>

struct frame_rescue_state {
	u64 deadline_ns;
	unsigned long min_util;
	bool armed;
	bool active;
};

static inline void frame_rescue_state_reset(struct frame_rescue_state *state)
{
	state->deadline_ns = 0;
	state->min_util = 0;
	state->armed = false;
	state->active = false;
}

#endif /* _FRAME_RESCUE_H */
