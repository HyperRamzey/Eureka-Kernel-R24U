#ifndef _LINUX_PSI_TYPES_H
#define _LINUX_PSI_TYPES_H

#include <linux/workqueue.h>
#include <linux/seqlock.h>
#include <linux/types.h>
#include <linux/kref.h>
#include <linux/wait.h>
#include <linux/kthread.h>

#ifdef CONFIG_PSI

/*
 * Backport note (derp, 4.4 tree)
 *
 * Upstream 4.9 keeps the per-group polling work on a `struct kthread_worker`
 * created with kthread_create_worker() and bound with
 * kthread_delayed_work_init(). None of that exists in 4.4 - this tree has
 * plain kthread_create() plus the standard delayed_work, and no
 * kthread_worker at all.
 *
 * Rather than degrade the poll work onto the system workqueue - which is
 * exactly the delay kthread_worker was introduced to avoid, and would make
 * PSI miss trigger windows under writeback/zram congestion - each psi_group
 * here gets its own dedicated kthread parked on a waitqueue. That keeps the
 * guarantee upstream added in commit 9f2a5b3d9ad9 ("sched: psi: create
 * kthread worker") while only using 4.4 primitives.
 *
 * 4.4 HAS struct kthread_worker / struct kthread_work / kthread_worker_fn() /
 * kthread_init_work() / kthread_queue_work() / kthread_cancel_work_sync(), so the
 * worker side is a faithful port of upstream's:
 *   struct kthread_worker poll_kworker;		kthread_worker_fn() runs this
 *   struct kthread_work   poll_work;		queued with kthread_queue_work()
 *   struct task_struct  *poll_task;		the psimon thread itself
 *
 * Only the DELAYED variant is missing in 4.4 (no kthread_delayed_work,
 * kthread_queue_delayed_work, kthread_cancel_delayed_work_sync or
 * kthread_destroy_worker), so the delay is a timer_list whose callback calls
 * kthread_queue_work().
 *
 * A dedicated WORKQUEUE was tried here instead and HARD HUNG the device: it made
 * the scheduler hot path call mod_delayed_work() with rq->lock held. Upstream's
 * comment on psi_schedule_poll_work says its hot path is safe only because the
 * primitive takes a single uncontended leaf spinlock; a workqueue pool lock is not
 * that. kthread_queue_work() takes poll_kworker.lock and calls wake_up_process(),
 * which are safe from the hot path.
 *
 * A hand-rolled kthread parked on its own waitqueue was tried first and
 * measurably failed on device: psi_schedule_poll_work() issued exactly one
 * wake_up() and psi_poll_fn() never ran, because nothing re-armed the flag.
 * mod_delayed_work() re-arms natively, so there is no flag to get stuck.
 */

/* Tracked task states */
enum psi_task_count {
	NR_IOWAIT,
	NR_MEMSTALL,
	NR_RUNNING,
	NR_PSI_TASK_COUNTS = 3,
};

/* Task state bitmasks */
#define TSK_IOWAIT	(1 << NR_IOWAIT)
#define TSK_MEMSTALL	(1 << NR_MEMSTALL)
#define TSK_RUNNING	(1 << NR_RUNNING)

/*
 * Private bit in task->psi_flags, deliberately outside the TSK_* count bits:
 * set from psi_memstall_enter() to psi_memstall_leave(). It is the 4.4
 * stand-in for upstream's PF_MEMSTALL (0x01000000 is PF_PERF_CRITICAL in this
 * tree).
 *
 * It must be separate from TSK_MEMSTALL. TSK_MEMSTALL says "this task is
 * currently counted in its CPU's bucket" and is cleared by psi_task_change()
 * whenever the task is dequeued for a migration; "inside a stall section" has
 * to survive that so psi_enqueue() can count the task again on its new CPU.
 * psi_task_change() only touches the bits it is passed, so this one is
 * unaffected by it.
 */
#define PSI_TSK_IN_MEMSTALL	(1 << NR_PSI_TASK_COUNTS)

/* Resources that workloads could be stalled on */
enum psi_res {
	PSI_IO,
	PSI_MEM,
	PSI_CPU,
	NR_PSI_RESOURCES = 3,
};

/*
 * Pressure states for each resource:
 *
 * SOME: Stalled tasks & working tasks
 * FULL: Stalled tasks & no working tasks
 */
enum psi_states {
	PSI_IO_SOME,
	PSI_IO_FULL,
	PSI_MEM_SOME,
	PSI_MEM_FULL,
	PSI_CPU_SOME,
	/* Only per-CPU, to weigh the CPU in the global average: */
	PSI_NONIDLE,
	NR_PSI_STATES = 6,
};

enum psi_aggregators {
	PSI_AVGS = 0,
	PSI_POLL,
	NR_PSI_AGGREGATORS,
};

struct psi_group_cpu {
	/* 1st cacheline updated by the scheduler */

	/* Aggregator needs to know of concurrent changes */
	seqcount_t seq ____cacheline_aligned_in_smp;

	/* States of the tasks belonging to this group */
	unsigned int tasks[NR_PSI_TASK_COUNTS];

	/* Aggregate pressure state derived from the tasks */
	u32 state_mask;

	/* Period time sampling buckets for each state of interest (ns) */
	u32 times[NR_PSI_STATES];

	/* Time of last task change in this group (rq_clock) */
	u64 state_start;

	/* 2nd cacheline updated by the aggregator */

	/* Delta detection against the sampling buckets */
	u32 times_prev[NR_PSI_AGGREGATORS][NR_PSI_STATES]
			____cacheline_aligned_in_smp;
};

/* PSI growth tracking window */
struct psi_window {
	/* Window size in ns */
	u64 size;

	/* Start time of the current window in ns */
	u64 start_time;

	/* Value at the start of the window */
	u64 start_value;

	/* Value growth in the previous window */
	u64 prev_growth;
};

struct psi_trigger {
	/* PSI state being monitored by the trigger */
	enum psi_states state;

	/* User-spacified threshold in ns */
	u64 threshold;

	/* List node inside triggers list */
	struct list_head node;

	/* Backpointer needed during trigger destruction */
	struct psi_group *group;

	/* Wait queue for polling */
	wait_queue_head_t event_wait;

	/* Pending event flag */
	int event;

	/* Tracking window */
	struct psi_window win;

	/*
	 * Time last event was generated. Used for rate-limiting
	 * events to one per window
	 */
	u64 last_event_time;

	/* Refcounting to prevent premature destruction */
	struct kref refcount;
};

struct psi_group {
	/* Protects data used by the aggregator */
	struct mutex avgs_lock;

	/* Per-cpu task state & time tracking */
	struct psi_group_cpu __percpu *pcpu;

	/* Running pressure averages */
	u64 avg_total[NR_PSI_STATES - 1];
	u64 avg_last_update;
	u64 avg_next_update;

	/*
	 * Aggregator work control.
	 *
	 * This is a plain delayed_work on the normal workqueue, which is
	 * exactly what upstream does. An earlier revision of this port gave
	 * it a dedicated kthread instead, on the theory that PSI timing
	 * should not queue behind other workqueue consumers. That is true
	 * and it is also wrong: the dedicated kthread had to be created from
	 * psi_init(), which runs at the end of sched_init(), and creating a
	 * SCHED_FIFO kthread that early in boot hangs the device on the
	 * bootlogo. Upstream keeps the averages work on the workqueue and
	 * only gives the *trigger polling* path a dedicated worker, created
	 * lazily from psi_trigger_create(). Follow that.
	 */
	struct delayed_work avgs_work;

	/* Total stall times and sampled pressure averages */
	u64 total[NR_PSI_AGGREGATORS][NR_PSI_STATES - 1];
	unsigned long avg[NR_PSI_STATES - 1][3];

	/*
	 * Monitor work control. See the backport note at the top of this
	 * header: kthread_worker + kthread_work, plus a timer for the delay
	 * that 4.4's missing kthread_delayed_work would have provided.
	 */
	atomic_t poll_scheduled;
	struct kthread_worker poll_kworker;
	struct kthread_work poll_work;
	struct timer_list poll_timer;
	struct task_struct *poll_task;

	/* Protects data used by the monitor */
	struct mutex trigger_lock;

	/* Configured polling triggers */
	struct list_head triggers;
	u32 nr_triggers[NR_PSI_STATES - 1];
	u32 poll_states;
	u64 poll_min_period;

	/* Total stall times at the start of monitor activation */
	u64 polling_total[NR_PSI_STATES - 1];
	u64 polling_next_update;
	u64 polling_until;
};

#else /* CONFIG_PSI */

struct psi_group { };

#endif /* CONFIG_PSI */

#endif /* _LINUX_PSI_TYPES_H */