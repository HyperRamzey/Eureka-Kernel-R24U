/* SPDX-License-Identifier: GPL-2.0 */
/*
 * PSI scheduler-side accounting hooks (4.9 backport, derp).
 *
 * These drive the PSI task-state accounting:
 *   TSK_RUNNING  - task is runnable on a runqueue
 *   TSK_IOWAIT   - task is sleeping in I/O
 *   TSK_MEMSTALL - task is stalled on memory
 * psi_task_change() folds those counts into the per-CPU PSI buckets.
 *
 * The 4.9 tree keeps these in kernel/sched/stats.h, which is #included from
 * sched.h *before* 'struct rq' and the __task_rq_lock() family exist. This file
 * is included by core.c only, after "sched.h", so no shared header's include
 * order has to change.
 *
 * 4.4 adaptations:
 *  - upstream tests p->flags & PF_MEMSTALL for "inside a stall section". That
 *    bit is unavailable here: 0x01000000 is PF_PERF_CRITICAL in this tree.
 *    PSI_TSK_IN_MEMSTALL in p->psi_flags is used instead. It has to be a bit
 *    of its own, NOT TSK_MEMSTALL: psi_task_change() clears TSK_MEMSTALL when
 *    a task is dequeued for a migration, and psi_enqueue() must still know
 *    afterwards that the task is in a section so it can count it again on the
 *    new CPU. Reading TSK_MEMSTALL here loses the stall on every migration
 *    and underflows the new CPU's counter at psi_memstall_leave().
 *  - 4.4's __task_rq_lock() takes no irqsave flags (only task_rq_lock() does).
 */

#ifndef _LINUX_SCHED_PSI_HOOKS_H
#define _LINUX_SCHED_PSI_HOOKS_H

#include <linux/psi.h>

#ifdef CONFIG_PSI

static inline void psi_enqueue(struct task_struct *p, bool wakeup)
{
	int clear = 0, set = TSK_RUNNING;

	if (static_branch_likely(&psi_disabled))
		return;

	if (!wakeup || p->sched_psi_wake_requeue) {
		/* count it on THIS cpu if it is still inside a stall section */
		if (p->psi_flags & PSI_TSK_IN_MEMSTALL)
			set |= TSK_MEMSTALL;
		if (p->sched_psi_wake_requeue)
			p->sched_psi_wake_requeue = 0;
	} else {
		if (p->in_iowait)
			clear |= TSK_IOWAIT;
	}

	psi_task_change(p, clear, set);
}

static inline void psi_dequeue(struct task_struct *p, bool sleep)
{
	int clear = TSK_RUNNING, set = 0;

	if (static_branch_likely(&psi_disabled))
		return;

	if (!sleep) {
		if (p->psi_flags & TSK_MEMSTALL)
			clear |= TSK_MEMSTALL;
	} else {
		if (p->in_iowait)
			set |= TSK_IOWAIT;
	}

	psi_task_change(p, clear, set);
}

/*
 * Is the task being migrated during a wakeup? Deregister its sleep-persistent
 * PSI states from the old queue and tell psi_enqueue() it has to requeue.
 */
static inline void psi_ttwu_dequeue(struct task_struct *p)
{
	if (static_branch_likely(&psi_disabled))
		return;

	if (unlikely(p->in_iowait || (p->psi_flags & TSK_MEMSTALL))) {
		struct rq *rq;
		int clear = 0;

		if (p->in_iowait)
			clear |= TSK_IOWAIT;
		if (p->psi_flags & TSK_MEMSTALL)
			clear |= TSK_MEMSTALL;

		rq = __task_rq_lock(p);
		psi_task_change(p, clear, 0);
		p->sched_psi_wake_requeue = 1;
		__task_rq_unlock(rq);
	}
}

static inline void psi_task_tick(struct rq *rq)
{
	if (static_branch_likely(&psi_disabled))
		return;

	if (unlikely(rq->curr->psi_flags & TSK_MEMSTALL))
		psi_memstall_tick(rq->curr, cpu_of(rq));
}

#else /* CONFIG_PSI */

static inline void psi_enqueue(struct task_struct *p, bool wakeup) {}
static inline void psi_dequeue(struct task_struct *p, bool sleep) {}
static inline void psi_ttwu_dequeue(struct task_struct *p) {}
static inline void psi_task_tick(struct rq *rq) {}

#endif /* CONFIG_PSI */

#endif /* _LINUX_SCHED_PSI_HOOKS_H */
