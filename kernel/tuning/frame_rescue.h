/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Frame Rescue Lite state shared by the frame-group implementation.
 *
 * The state is intentionally inert in this commit. Deadline timers,
 * utilization clamps and userspace controls are separate changes.
 */

#ifndef _FRAME_RESCUE_H
#define _FRAME_RESCUE_H

#include <linux/hrtimer.h>
#include <linux/types.h>

#define FRAME_RESCUE_DEADLINE_NUM	614U
#define FRAME_RESCUE_DEADLINE_SHIFT	10
#define FRAME_RESCUE_MIN_UTIL		384U

struct frame_rescue_state {
	struct hrtimer timer;
	u64 deadline_ns;
	u64 generation;
	unsigned long min_util;
	bool armed;
	bool active;
};

static inline void frame_rescue_state_reset(struct frame_rescue_state *state)
{
	state->deadline_ns = 0;
	state->generation = 0;
	state->min_util = 0;
	state->armed = false;
	state->active = false;
}

bool frame_rescue_enabled(void);
void frame_rescue_init(void);

#endif /* _FRAME_RESCUE_H */
