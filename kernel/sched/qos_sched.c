// SPDX-License-Identifier: GPL-2.0-only
/*
 * Oplus scheduler QoS policy for the SM8250 WALT/EAS scheduler.
 *
 * This policy deliberately consumes existing task signals.  FrameBoost and
 * SchedAssist remain authoritative; QoS only keeps an abnormal CFS task off
 * the maximum-capacity cluster when a lower cluster has safe spare capacity.
 * Mode 1 is observation-only, so the feature can be enabled on a production
 * kernel before placement changes are tested.
 */
#include <linux/cpumask.h>
#include <linux/sched.h>
#include <linux/sched/qos_sched.h>
#include <linux/sysctl.h>

#define CREATE_TRACE_POINTS
#include <trace/events/qos_sched.h>

#include "sched.h"
#include "walt.h"

#ifdef OPLUS_FEATURE_SCHED_ASSIST
#include <linux/sched_assist/sched_assist_common.h>
#endif

#ifdef CONFIG_OPLUS_FEATURE_FRAME_BOOST
#include <linux/tuning/frame_group.h>
#endif

#ifdef CONFIG_OPLUS_FEATURE_ABNORMAL_FLAG
#include "../../drivers/soc/oplus/oplus_overload/task_overload.h"
#endif

/* 0=off, 1=trace-only, 2=placement, 3=reserved for preemption. */
int sysctl_sched_qos_enable;
int sysctl_sched_qos_mode = 1;
int sysctl_sched_qos_debug;
int sysctl_sched_qos_safety_margin = 90;

#define QOS_SCHED_MARGIN_MIN 50
#define QOS_SCHED_MARGIN_MAX 100

struct qos_sched_candidate {
	int cpu;
	unsigned long capacity;
	unsigned long spare;
	bool idle;
	bool same_cluster;
};

static bool qos_sched_active(void)
{
	return READ_ONCE(sysctl_sched_qos_enable) &&
		READ_ONCE(sysctl_sched_qos_mode) > 0;
}

enum qos_sched_level qos_sched_task_level(struct task_struct *task)
{
	if (!task)
		return QOS_LEVEL_NORMAL;

#ifdef CONFIG_OPLUS_FEATURE_FRAME_BOOST
	if (frame_boost_enabled() && is_fbg_task(task))
		return QOS_LEVEL_CRITICAL;
#endif

#ifdef OPLUS_FEATURE_SCHED_ASSIST
	if (test_task_ux(task))
		return QOS_LEVEL_HIGH;
#endif

#ifdef CONFIG_OPLUS_FEATURE_ABNORMAL_FLAG
	if (task->abnormal_flag > ABNORMAL_THRESHOLD)
		return QOS_LEVEL_LOW;
#endif

	return QOS_LEVEL_NORMAL;
}

static unsigned int qos_sched_margin(void)
{
	int margin = READ_ONCE(sysctl_sched_qos_safety_margin);

	if (margin < QOS_SCHED_MARGIN_MIN)
		return QOS_SCHED_MARGIN_MIN;
	if (margin > QOS_SCHED_MARGIN_MAX)
		return QOS_SCHED_MARGIN_MAX;
	return margin;
}

static unsigned long qos_sched_cpu_util_without(int cpu,
						struct task_struct *task)
{
	unsigned long util = cpu_util(cpu);
	unsigned long task_util_value;

	if (cpu != task_cpu(task))
		return util;

#ifdef CONFIG_SCHED_WALT
	/* WALT has no useful blocked-util decay while a task is waking. */
	if (READ_ONCE(task->state) == TASK_WAKING)
		return util;
#endif

	task_util_value = task_util(task);
	if (util > task_util_value)
		return util - task_util_value;
	return 0;
}

static bool qos_sched_same_cluster(int cpu, int task_cpu_id)
{
#ifdef CONFIG_SCHED_WALT
	return same_cluster(cpu, task_cpu_id);
#else
	return cpus_share_cache(cpu, task_cpu_id);
#endif
}

static bool qos_sched_candidate_fits(struct task_struct *task, int cpu,
					     unsigned long *capacity,
					     unsigned long *spare)
{
	unsigned long cpu_capacity = capacity_of(cpu);
	unsigned long cpu_util_value = qos_sched_cpu_util_without(cpu, task);
	unsigned long task_util_value = task_util_est(task);
	u64 projected_util = (u64)cpu_util_value + task_util_value;
	u64 safe_capacity = (u64)cpu_capacity * qos_sched_margin();

	if (!cpu_capacity || projected_util * QOS_SCHED_MARGIN_MAX >
		safe_capacity)
		return false;

	*capacity = cpu_capacity;
	*spare = cpu_capacity > cpu_util_value ?
		cpu_capacity - cpu_util_value : 0;
	return true;
}

static bool qos_sched_candidate_better(const struct qos_sched_candidate *candidate,
					       const struct qos_sched_candidate *best)
{
	if (candidate->idle != best->idle)
		return candidate->idle;
	if (candidate->spare != best->spare)
		return candidate->spare > best->spare;
	if (candidate->same_cluster != best->same_cluster)
		return candidate->same_cluster;

	/* Keep the low-priority task on the lowest-capacity safe cluster. */
	if (candidate->capacity != best->capacity)
		return candidate->capacity < best->capacity;
	return candidate->cpu < best->cpu;
}

static int qos_sched_find_lower_cpu(struct task_struct *task)
{
	struct qos_sched_candidate best = {
		.cpu = -1,
	};
	int task_cpu_id = task_cpu(task);
	int cpu;

	for_each_cpu(cpu, &task->cpus_allowed) {
		struct qos_sched_candidate candidate;
		unsigned long capacity, spare;

		if (!cpu_active(cpu) || cpu_isolated(cpu))
			continue;
		if (is_max_capacity_cpu(cpu))
			continue;

		if (!qos_sched_candidate_fits(task, cpu, &capacity, &spare))
			continue;

		candidate.cpu = cpu;
		candidate.capacity = capacity;
		candidate.spare = spare;
		candidate.idle = available_idle_cpu(cpu);
		candidate.same_cluster = qos_sched_same_cluster(cpu, task_cpu_id);

		if (best.cpu < 0 || qos_sched_candidate_better(&candidate, &best))
			best = candidate;
	}

	return best.cpu;
}

bool qos_sched_skip_cpu(struct task_struct *task, int cpu)
{
	if (!qos_sched_active() || READ_ONCE(sysctl_sched_qos_mode) < 2)
		return false;

	if (qos_sched_task_level(task) != QOS_LEVEL_LOW)
		return false;

	return cpu >= 0 && cpu < nr_cpu_ids && is_max_capacity_cpu(cpu) &&
		qos_sched_find_lower_cpu(task) >= 0;
}

void qos_sched_adjust_target(struct task_struct *task, int eas_cpu,
			     int *target_cpu)
{
	enum qos_sched_level level;
	int final_cpu;
	int reason = QOS_REASON_EAS_KEEP;
	int mode;
	unsigned long qos_task_util;
	unsigned long final_util = 0;
	unsigned long final_capacity = 0;
	unsigned long final_spare = 0;

	if (!task || !target_cpu || !qos_sched_active())
		return;

	qos_task_util = task_util_est(task);
	mode = READ_ONCE(sysctl_sched_qos_mode);
	level = qos_sched_task_level(task);
	final_cpu = *target_cpu;

#ifdef CONFIG_OPLUS_FEATURE_FRAME_BOOST
	if (level == QOS_LEVEL_CRITICAL) {
		reason = QOS_REASON_FBG_BYPASS;
		goto trace;
	}
#endif

#ifdef OPLUS_FEATURE_SCHED_ASSIST
	if (level == QOS_LEVEL_HIGH) {
		reason = QOS_REASON_UX_BYPASS;
		goto trace;
	}
#endif

	if (level == QOS_LEVEL_LOW && mode >= 2 &&
		qos_sched_skip_cpu(task, final_cpu)) {
		int lower_cpu = qos_sched_find_lower_cpu(task);

		if (lower_cpu >= 0) {
			final_cpu = lower_cpu;
			reason = QOS_REASON_LOW_AVOID_MAX;
		} else {
			reason = QOS_REASON_NO_VALID_CPU;
		}
	}

	*target_cpu = final_cpu;
	if (final_cpu >= 0 && final_cpu < nr_cpu_ids) {
		final_util = qos_sched_cpu_util_without(final_cpu, task);
		final_capacity = capacity_of(final_cpu);
		final_spare = final_capacity > final_util ?
			final_capacity - final_util : 0;
	}

trace:
	if (READ_ONCE(sysctl_sched_qos_debug) || level != QOS_LEVEL_NORMAL ||
		final_cpu != eas_cpu)
		trace_qos_sched_decision(task, level, eas_cpu, final_cpu,
				reason, mode, qos_task_util, final_util,
				final_capacity, final_spare);
}
