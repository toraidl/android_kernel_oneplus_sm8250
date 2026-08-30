/* SPDX-License-Identifier: GPL-2.0-only */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM qos_sched

#if !defined(_TRACE_QOS_SCHED_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_QOS_SCHED_H

#include <linux/sched.h>
#include <linux/tracepoint.h>

TRACE_EVENT(qos_sched_decision,

		TP_PROTO(struct task_struct *task, int level, int eas_cpu,
			 int final_cpu, int reason, int mode,
			 unsigned long task_util, unsigned long cpu_util,
			 unsigned long capacity, unsigned long spare),

		TP_ARGS(task, level, eas_cpu, final_cpu, reason, mode,
			task_util, cpu_util, capacity, spare),

	TP_STRUCT__entry(
		__array(char, comm, TASK_COMM_LEN)
		__field(pid_t, pid)
		__field(int, level)
		__field(int, eas_cpu)
		__field(int, final_cpu)
		__field(int, reason)
		__field(int, mode)
		__field(unsigned long, task_util)
		__field(unsigned long, cpu_util)
		__field(unsigned long, capacity)
		__field(unsigned long, spare)
	),

	TP_fast_assign(
		memcpy(__entry->comm, task->comm, TASK_COMM_LEN);
		__entry->pid = task->pid;
		__entry->level = level;
		__entry->eas_cpu = eas_cpu;
		__entry->final_cpu = final_cpu;
		__entry->reason = reason;
		__entry->mode = mode;
		__entry->task_util = task_util;
		__entry->cpu_util = cpu_util;
		__entry->capacity = capacity;
		__entry->spare = spare;
	),

	TP_printk("comm=%s pid=%d level=%d eas_cpu=%d final_cpu=%d reason=%d mode=%d task_util=%lu cpu_util=%lu capacity=%lu spare=%lu",
		  __entry->comm, __entry->pid, __entry->level,
		  __entry->eas_cpu, __entry->final_cpu, __entry->reason,
		  __entry->mode, __entry->task_util, __entry->cpu_util,
		  __entry->capacity, __entry->spare)
);

#endif /* _TRACE_QOS_SCHED_H */

#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE qos_sched
#include <trace/define_trace.h>
