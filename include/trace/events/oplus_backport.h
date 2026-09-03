/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Tracepoints shared by the Oplus backport bring-up work.
 *
 * Keep this event deliberately generic.  It lets Binder, FrameBoost and
 * future backport experiments be correlated without adding a new userspace
 * ABI or changing any scheduler decision.
 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM oplus_backport

#if !defined(_TRACE_OPLUS_BACKPORT_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_OPLUS_BACKPORT_H

#include <linux/sched.h>
#include <linux/string.h>
#include <linux/tracepoint.h>

TRACE_EVENT(oplus_backport_event,

	TP_PROTO(const char *subsystem, const char *action,
		 pid_t pid, pid_t tgid, int arg0, int arg1, int arg2, int arg3),

	TP_ARGS(subsystem, action, pid, tgid, arg0, arg1, arg2, arg3),

	TP_STRUCT__entry(
		__array(char, subsystem, 16)
		__array(char, action, 24)
		__field(pid_t, pid)
		__field(pid_t, tgid)
		__field(int, arg0)
		__field(int, arg1)
		__field(int, arg2)
		__field(int, arg3)
	),

	TP_fast_assign(
		strscpy(__entry->subsystem, subsystem,
			sizeof(__entry->subsystem));
		strscpy(__entry->action, action, sizeof(__entry->action));
		__entry->pid = pid;
		__entry->tgid = tgid;
		__entry->arg0 = arg0;
		__entry->arg1 = arg1;
		__entry->arg2 = arg2;
		__entry->arg3 = arg3;
	),

	TP_printk("subsystem=%s action=%s pid=%d tgid=%d arg0=%d arg1=%d arg2=%d arg3=%d",
		  __entry->subsystem, __entry->action, __entry->pid,
		  __entry->tgid, __entry->arg0, __entry->arg1,
		  __entry->arg2, __entry->arg3)
);

#endif /* _TRACE_OPLUS_BACKPORT_H */

#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE oplus_backport
#include <trace/define_trace.h>
