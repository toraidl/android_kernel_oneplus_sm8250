// SPDX-License-Identifier: GPL-2.0-only
/*
 * Frame Rescue Lite runtime gate.
 *
 * Deadline handling is kept in frame_group.c because it must use the
 * existing per-group locks. Userspace controls and util policy are separate
 * follow-up changes; the gate is therefore always disabled at boot here.
 */

#include <linux/compiler.h>

#include "frame_rescue.h"

static bool frame_rescue_runtime_enabled;

bool frame_rescue_enabled(void)
{
	return READ_ONCE(frame_rescue_runtime_enabled);
}

void frame_rescue_init(void)
{
	WRITE_ONCE(frame_rescue_runtime_enabled, false);
}
