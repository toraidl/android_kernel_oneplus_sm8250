/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Frame Rescue Lite state shared by the frame-group implementation.
 *
 * Deadline timers, utilization clamps and the optional rq-context kick
 * remain behind CONFIG_OPLUS_FRAME_RESCUE_LITE and a runtime gate.
 */

#ifndef _FRAME_RESCUE_H
#define _FRAME_RESCUE_H

#include <linux/atomic.h>
#include <linux/hrtimer.h>
#include <linux/smp.h>
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
#ifdef CONFIG_SMP
	call_single_data_t kick_csd;
	atomic_t kick_pending;
	int kick_cpu;
	unsigned int kick_flags;
	u64 kick_generation;
#endif
};

static inline void frame_rescue_state_reset(struct frame_rescue_state *state)
{
	state->deadline_ns = 0;
	state->generation = 0;
	state->min_util = 0;
	state->armed = false;
	state->active = false;
#ifdef CONFIG_SMP
	state->kick_csd.func = NULL;
	state->kick_csd.info = NULL;
	state->kick_csd.flags = 0;
	atomic_set(&state->kick_pending, 0);
	state->kick_cpu = -1;
	state->kick_flags = 0;
	state->kick_generation = 0;
#endif
}

bool frame_rescue_enabled(void);
void frame_rescue_init(void);

#endif /* _FRAME_RESCUE_H */
