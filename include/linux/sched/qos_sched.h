/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Oplus scheduler QoS policy for asymmetric WALT systems.
 */
#ifndef _LINUX_SCHED_QOS_SCHED_H
#define _LINUX_SCHED_QOS_SCHED_H

struct task_struct;

enum qos_sched_level {
	QOS_LEVEL_LOW = 0,
	QOS_LEVEL_NORMAL,
	QOS_LEVEL_HIGH,
	QOS_LEVEL_CRITICAL,
};

enum qos_sched_reason {
	QOS_REASON_EAS_KEEP = 0,
	QOS_REASON_FBG_BYPASS,
	QOS_REASON_UX_BYPASS,
	QOS_REASON_LOW_AVOID_MAX,
	QOS_REASON_NO_VALID_CPU,
};

extern int sysctl_sched_qos_enable;
extern int sysctl_sched_qos_mode;
extern int sysctl_sched_qos_debug;
extern int sysctl_sched_qos_safety_margin;

enum qos_sched_level qos_sched_task_level(struct task_struct *task);
bool qos_sched_skip_cpu(struct task_struct *task, int cpu);
void qos_sched_adjust_target(struct task_struct *task, int eas_cpu,
			     int *target_cpu);

#endif /* _LINUX_SCHED_QOS_SCHED_H */
