/*
 * Pressure stall information for CPU, memory and IO
 *
 * Copyright (c) 2018 Facebook, Inc.
 * Author: Johannes Weiner <hannes@cmpxchg.org>
 *
 * Polling support by Suren Baghdasaryan <surenb@google.com>
 * Copyright (c) 2018 Google, Inc.
 *
 * When CPU, memory and IO are contended, tasks experience delays that
 * reduce throughput and introduce latencies into the workload. Memory
 * and IO contention, in addition, can cause a full loss of forward
 * progress in which the CPU goes idle.
 *
 * This code aggregates individual task delays into resource pressure
 * metrics that indicate problems with both workload health and
 * resource utilization.
 *
 *			Model
 *
 * The time in which a task can execute on a CPU is our baseline for
 * productivity. Pressure expresses the amount of time in which this
 * potential cannot be realized due to resource contention.
 *
 * This concept of productivity has two components: the workload and
 * the CPU. To measure the impact of pressure on both, we define two
 * contention states for a resource: SOME and FULL.
 *
 * In the SOME state of a given resource, one or more tasks are
 * delayed on that resource. This affects the workload's ability to
 * perform work, but the CPU may still be executing other tasks.
 *
 * In the FULL state of a given resource, all non-idle tasks are
 * delayed on that resource such that nobody is advancing and the CPU
 * goes idle. This leaves both workload and CPU unproductive.
 *
 * (Naturally, the FULL state doesn't exist for the CPU resource.)
 *
 *	SOME = nr_delayed_tasks != 0
 *	FULL = nr_delayed_tasks != 0 && nr_running_tasks == 0
 *
 * The percentage of wallclock time spent in those compound stall
 * states gives pressure numbers between 0 and 100 for each resource,
 * where the SOME percentage indicates workload slowdowns and the FULL
 * percentage indicates reduced CPU utilization:
 *
 *	%SOME = time(SOME) / period
 *	%FULL = time(FULL) / period
 *
 *			Multiple CPUs
 *
 * The more tasks and available CPUs there are, the more work can be
 * performed concurrently. This means that the potential that can go
 * unrealized due to resource contention *also* scales with non-idle
 * tasks and CPUs.
 *
 * Consider a scenario where 257 number crunching tasks are trying to
 * run concurrently on 256 CPUs. If we simply aggregated the task
 * states, we would have to conclude a CPU SOME pressure number of
 * 100%, since *somebody* is waiting on a runqueue at all
 * times. However, that is clearly not the amount of contention the
 * workload is experiencing: only one out of 256 possible exceution
 * threads will be contended at any given time, or about 0.4%.
 *
 * Conversely, consider a scenario of 4 tasks and 4 CPUs where at any
 * given time *one* of the tasks is delayed due to a lack of memory.
 * Again, looking purely at the task state would yield a memory FULL
 * pressure number of 0%, since *somebody* is always making forward
 * progress. But again this wouldn't capture the amount of execution
 * potential lost, which is 1 out of 4 CPUs, or 25%.
 *
 * To calculate wasted potential (pressure) with multiple processors,
 * we have to base our calculation on the number of non-idle tasks in
 * conjunction with the number of available CPUs, which is the number
 * of potential execution threads. SOME becomes then the proportion of
 * delayed tasks to possibe threads, and FULL is the share of possible
 * threads that are unproductive due to delays:
 *
 *	threads = min(nr_nonidle_tasks, nr_cpus)
 *	   SOME = min(nr_delayed_tasks / threads, 1)
 *	   FULL = (threads - min(nr_running_tasks, threads)) / threads
 *
 * For the 257 number crunchers on 256 CPUs, this yields:
 *
 *	threads = min(257, 256)
 *	   SOME = min(1 / 256, 1)             = 0.4%
 *	   FULL = (256 - min(257, 256)) / 256 = 0%
 *
 * For the 1 out of 4 memory-delayed tasks, this yields:
 *
 *	threads = min(4, 4)
 *	   SOME = min(1 / 4, 1)               = 25%
 *	   FULL = (4 - min(3, 4)) / 4         = 25%
 *
 * [ Substitute nr_cpus with 1, and you can see that it's a natural
 *   extension of the single-CPU model. ]
 *
 *			Implementation
 *
 * To assess the precise time spent in each such state, we would have
 * to freeze the system on task changes and start/stop the state
 * clocks accordingly. Obviously that doesn't scale in practice.
 *
 * Because the scheduler aims to distribute the compute load evenly
 * among the available CPUs, we can track task state locally to each
 * CPU and, at much lower frequency, extrapolate the global state for
 * the cumulative stall times and the running averages.
 *
 * For each runqueue, we track:
 *
 *	   tSOME[cpu] = time(nr_delayed_tasks[cpu] != 0)
 *	   tFULL[cpu] = time(nr_delayed_tasks[cpu] && !nr_running_tasks[cpu])
 *	tNONIDLE[cpu] = time(nr_nonidle_tasks[cpu] != 0)
 *
 * and then periodically aggregate:
 *
 *	tNONIDLE = sum(tNONIDLE[i])
 *
 *	   tSOME = sum(tSOME[i] * tNONIDLE[i]) / tNONIDLE
 *	   tFULL = sum(tFULL[i] * tNONIDLE[i]) / tNONIDLE
 *
 *	   %SOME = tSOME / period
 *	   %FULL = tFULL / period
 *
 * This gives us an approximation of pressure that is practical
 * cost-wise, yet way more sensitive and accurate than periodic
 * sampling of the aggregate task states would be.
 */

#include <linux/seq_file.h>
#include <linux/proc_fs.h>
#include <linux/seqlock.h>
#include <linux/uaccess.h>
#include <linux/module.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/sched.h>
#include <linux/ctype.h>
#include <linux/file.h>
#include <linux/poll.h>
#include <linux/psi.h>
#include "sched.h"

static int psi_bug __read_mostly;

/*
 * Last task that ran psi_avgs_work(). 4.4 has no wq_worker_last_func(),
 * which upstream uses in psi_task_change() to avoid re-arming the
 * aggregation clock when the aggregation worker itself goes to sleep
 * (otherwise the work ping-pongs forever and never shuts off when idle).
 * Remembering the worker's task gives the same persistent answer:
 * workqueue workers are long-lived and may run other works, but
 * suppressing the kick for that one task is harmless, exactly like
 * upstream's persistent last-func test.
 */
static struct task_struct *psi_avgs_task __read_mostly;

/*
 * TEMPORARY poll-path diagnostics, exported through /proc/pressure/enable.
 * Remove once the trigger path is confirmed working.
 */
static atomic_long_t psi_dbg_sched;
static atomic_long_t psi_dbg_runs;
static atomic_long_t psi_dbg_trigs;
static int psi_dbg_task_nonnull;
static int psi_dbg_list_empty;
static int psi_dbg_curwork_nonnull;

DEFINE_STATIC_KEY_FALSE(psi_disabled);

#ifdef CONFIG_PSI_DEFAULT_DISABLED
static bool psi_enable;
#else
static bool psi_enable = true;
#endif
static int __init setup_psi(char *str)
{
	return kstrtobool(str, &psi_enable) == 0;
}
__setup("psi=", setup_psi);

/* Running averages - we need to be higher-res than loadavg */
#define PSI_FREQ	(2*HZ+1)	/* 2 sec intervals */
#define EXP_10s		1677		/* 1/exp(2s/10s) as fixed-point */
#define EXP_60s		1981		/* 1/exp(2s/60s) */
#define EXP_300s	2034		/* 1/exp(2s/300s) */

/*
 * Fixed-point load-average math, vendored from kernel/sched/loadavg.c.
 *
 * 4.9 lifted FSHIFT/FIXED_1/calc_load()/calc_load_n()/LOAD_INT/LOAD_FRAC into
 * include/linux/sched/loadavg.h; this 4.4 tree keeps them private to
 * loadavg.c and has no such header. Vendoring the exact same expressions
 * keeps the reported avgs numerically identical to upstream. fixed_power_int()
 * below is byte-identical to the 4.4 copy in kernel/sched/loadavg.c:231.
 */
#define PSI_FSHIFT	11		/* nr of bits of precision */
#define PSI_FIXED_1	(1<<PSI_FSHIFT)/* 1.0 as fixed-point */

#define LOAD_INT(x) ((x) >> PSI_FSHIFT)
#define LOAD_FRAC(x) LOAD_INT(((x) & (PSI_FIXED_1-1)) * 100)

/* a1 = a0 * e + a * (1 - e) */
static inline unsigned long psi_calc_load(unsigned long load,
					  unsigned long exp,
					  unsigned long active)
{
	unsigned long newload;

	newload = load * exp + active * (PSI_FIXED_1 - exp);
	if (active >= load)
		newload += PSI_FIXED_1 - 1;

	return newload / PSI_FIXED_1;
}

/* x^n in O(log n) time; verbatim from kernel/sched/loadavg.c */
static unsigned long psi_fixed_power_int(unsigned long x,
					 unsigned int frac_bits,
					 unsigned int n)
{
	unsigned long result = 1UL << frac_bits;

	if (n) {
		for (;;) {
			if (n & 1) {
				result *= x;
				result += 1UL << (frac_bits - 1);
				result >>= frac_bits;
			}
			n >>= 1;
			if (!n)
				break;
			x *= x;
			x += 1UL << (frac_bits - 1);
			x >>= frac_bits;
		}
	}

	return result;
}

static inline unsigned long psi_calc_load_n(unsigned long load,
					    unsigned long exp,
					    unsigned long active,
					    unsigned int n)
{
	return psi_calc_load(load, psi_fixed_power_int(exp, PSI_FSHIFT, n),
			     active);
}

/* PSI trigger definitions */
#define WINDOW_MIN_US 500000	/* Min window size is 500ms */
#define WINDOW_MAX_US 10000000	/* Max window size is 10s */
#define UPDATES_PER_WINDOW 10	/* 10 updates per window */

/* Sampling frequency in nanoseconds */
static u64 psi_period __read_mostly;

/* System-level pressure and stall tracking */
static DEFINE_PER_CPU(struct psi_group_cpu, system_group_pcpu);
static struct psi_group psi_system = {
	.pcpu = &system_group_pcpu,
};

static void psi_avgs_work(struct work_struct *work);
static void psi_poll_fn(struct psi_group *group);
static void psi_poll_work(struct kthread_work *work);
static void psi_poll_timer_fn(unsigned long data);


static void group_init(struct psi_group *group)
{
	int cpu;

	for_each_possible_cpu(cpu)
		seqcount_init(&per_cpu_ptr(group->pcpu, cpu)->seq);
	/*
	 * Upstream v5.4 seeds only avg_next_update and deliberately leaves
	 * avg_last_update at 0: update_averages() derives missed_periods
	 * from avg_next_update and the sample period from avg_last_update,
	 * so seeding it here would distort the first sample.
	 */
	group->avg_next_update = sched_clock() + psi_period;
	INIT_DELAYED_WORK(&group->avgs_work, psi_avgs_work);
	mutex_init(&group->avgs_lock);
	/*
	 * Poll side: upstream's kthread_worker, which 4.4 does have. Only the
	 * worker bookkeeping is set up here; the psimon THREAD is created
	 * lazily in psi_trigger_create(), which is where upstream v5.4 calls
	 * kthread_create_worker(0, "psimon"). Creating it in psi_init() would
	 * start a kernel thread from inside sched_init() and bootloop, which
	 * two earlier workqueue-based attempts of this port both did.
	 */
	kthread_init_worker(&group->poll_kworker);
	atomic_set(&group->poll_scheduled, 0);
	mutex_init(&group->trigger_lock);
	INIT_LIST_HEAD(&group->triggers);
	memset(group->nr_triggers, 0, sizeof(group->nr_triggers));
	group->poll_states = 0;
	group->poll_min_period = U32_MAX;
	memset(group->polling_total, 0, sizeof(group->polling_total));
	group->polling_next_update = ULLONG_MAX;
	group->polling_until = 0;
}

void __init psi_init(void)
{
	/*
	 * The group is initialised UNCONDITIONALLY, unlike upstream, which
	 * returns early when PSI is disabled at boot. We have a runtime
	 * switch (/proc/pressure/enable), and without an initialised group
	 * there would be nothing to switch on later.
	 *
	 * No thread and no workqueue is created here, deliberately.
	 * psi_init() runs at the end of sched_init(), far too early to start
	 * anything: both kthread_create() and alloc_workqueue() (the latter via
	 * kthread_create_on_node(), kernel/workqueue.c:1823) start a kernel
	 * thread, and doing that from inside sched_init() hangs the device on
	 * the bootlogo. It was measured here, twice. Upstream avoids it the
	 * same way: its only worker (psimon) is created lazily in
	 * psi_trigger_create(), long after boot, which is what this port does
	 * too.
	 */
	psi_period = jiffies_to_nsecs(PSI_FREQ);

	group_init(&psi_system);

	if (!psi_enable)
		static_branch_enable(&psi_disabled);
}

static bool test_state(unsigned int *tasks, enum psi_states state)
{
	switch (state) {
	case PSI_IO_SOME:
		return tasks[NR_IOWAIT];
	case PSI_IO_FULL:
		return tasks[NR_IOWAIT] && !tasks[NR_RUNNING];
	case PSI_MEM_SOME:
		return tasks[NR_MEMSTALL];
	case PSI_MEM_FULL:
		return tasks[NR_MEMSTALL] && !tasks[NR_RUNNING];
	case PSI_CPU_SOME:
		return tasks[NR_RUNNING] > 1;
	case PSI_NONIDLE:
		return tasks[NR_IOWAIT] || tasks[NR_MEMSTALL] ||
			tasks[NR_RUNNING];
	default:
		return false;
	}
}

static void get_recent_times(struct psi_group *group, int cpu,
			     enum psi_aggregators aggregator, u32 *times,
			     u32 *pchanged_states)
{
	struct psi_group_cpu *groupc = per_cpu_ptr(group->pcpu, cpu);
	u64 now, state_start;
	enum psi_states s;
	unsigned int seq;
	u32 state_mask;

	*pchanged_states = 0;

	/* Snapshot a coherent view of the CPU state */
	do {
		seq = read_seqcount_begin(&groupc->seq);
		now = cpu_clock(cpu);
		memcpy(times, groupc->times, sizeof(groupc->times));
		state_mask = groupc->state_mask;
		state_start = groupc->state_start;
	} while (read_seqcount_retry(&groupc->seq, seq));

	/* Calculate state time deltas against the previous snapshot */
	for (s = 0; s < NR_PSI_STATES; s++) {
		u32 delta;
		/*
		 * In addition to already concluded states, we also
		 * incorporate currently active states on the CPU,
		 * since states may last for many sampling periods.
		 *
		 * This way we keep our delta sampling buckets small
		 * (u32) and our reported pressure close to what's
		 * actually happening.
		 */
		if (state_mask & (1 << s))
			times[s] += now - state_start;

		delta = times[s] - groupc->times_prev[aggregator][s];
		groupc->times_prev[aggregator][s] = times[s];

		times[s] = delta;
		if (delta)
			*pchanged_states |= (1 << s);
	}
}

static void calc_avgs(unsigned long avg[3], int missed_periods,
		      u64 time, u64 period)
{
	unsigned long pct;

	/* Fill in zeroes for periods of no activity */
	if (missed_periods) {
		avg[0] = psi_calc_load_n(avg[0], EXP_10s, 0, missed_periods);
		avg[1] = psi_calc_load_n(avg[1], EXP_60s, 0, missed_periods);
		avg[2] = psi_calc_load_n(avg[2], EXP_300s, 0, missed_periods);
	}

	/* Sample the most recent active period */
	pct = div_u64(time * 100, period);
	pct *= FIXED_1;
	avg[0] = psi_calc_load(avg[0], EXP_10s, pct);
	avg[1] = psi_calc_load(avg[1], EXP_60s, pct);
	avg[2] = psi_calc_load(avg[2], EXP_300s, pct);
}

static void collect_percpu_times(struct psi_group *group,
				 enum psi_aggregators aggregator,
				 u32 *pchanged_states)
{
	u64 deltas[NR_PSI_STATES - 1] = { 0, };
	unsigned long nonidle_total = 0;
	u32 changed_states = 0;
	int cpu;
	int s;

	/*
	 * Collect the per-cpu time buckets and average them into a
	 * single time sample that is normalized to wallclock time.
	 *
	 * For averaging, each CPU is weighted by its non-idle time in
	 * the sampling period. This eliminates artifacts from uneven
	 * loading, or even entirely idle CPUs.
	 */
	for_each_possible_cpu(cpu) {
		u32 times[NR_PSI_STATES];
		u32 nonidle;
		u32 cpu_changed_states;

		get_recent_times(group, cpu, aggregator, times,
				&cpu_changed_states);
		changed_states |= cpu_changed_states;

		nonidle = nsecs_to_jiffies(times[PSI_NONIDLE]);
		nonidle_total += nonidle;

		for (s = 0; s < PSI_NONIDLE; s++)
			deltas[s] += (u64)times[s] * nonidle;
	}

	/*
	 * Integrate the sample into the running statistics that are
	 * reported to userspace: the cumulative stall times and the
	 * decaying averages.
	 *
	 * Pressure percentages are sampled at PSI_FREQ. We might be
	 * called more often when the user polls more frequently than
	 * that; we might be called less often when there is no task
	 * activity, thus no data, and clock ticks are sporadic. The
	 * below handles both.
	 */

	/* total= */
	for (s = 0; s < NR_PSI_STATES - 1; s++)
		group->total[aggregator][s] +=
				div_u64(deltas[s], max(nonidle_total, 1UL));

	if (pchanged_states)
		*pchanged_states = changed_states;
}

static u64 update_averages(struct psi_group *group, u64 now)
{
	unsigned long missed_periods = 0;
	u64 expires, period;
	u64 avg_next_update;
	int s;

	/* avgX= */
	expires = group->avg_next_update;
	if (now - expires >= psi_period)
		missed_periods = div_u64(now - expires, psi_period);

	/*
	 * The periodic clock tick can get delayed for various
	 * reasons, especially on loaded systems. To avoid clock
	 * drift, we schedule the clock in fixed psi_period intervals.
	 * But the deltas we sample out of the per-cpu buckets above
	 * are based on the actual time elapsing between clock ticks.
	 */
	avg_next_update = expires + ((1 + missed_periods) * psi_period);
	period = now - (group->avg_last_update + (missed_periods * psi_period));
	group->avg_last_update = now;

	for (s = 0; s < NR_PSI_STATES - 1; s++) {
		u32 sample;

		sample = group->total[PSI_AVGS][s] - group->avg_total[s];
		/*
		 * Due to the lockless sampling of the time buckets,
		 * recorded time deltas can slip into the next period,
		 * which under full pressure can result in samples in
		 * excess of the period length.
		 *
		 * We don't want to report non-sensical pressures in
		 * excess of 100%, nor do we want to drop such events
		 * on the floor. Instead we punt any overage into the
		 * future until pressure subsides. By doing this we
		 * don't underreport the occurring pressure curve, we
		 * just report it delayed by one period length.
		 *
		 * The error isn't cumulative. As soon as another
		 * delta slips from a period P to P+1, by definition
		 * it frees up its time T in P.
		 */
		if (sample > period)
			sample = period;
		group->avg_total[s] += sample;
		calc_avgs(group->avg[s], missed_periods, sample, period);
	}

	return avg_next_update;
}

static void psi_avgs_work(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct psi_group *group = container_of(dwork, struct psi_group, avgs_work);
	u32 changed_states;
	bool nonidle;
	u64 now;

	/* See psi_task_change(): remember who we are for the idle-kick guard. */
	WRITE_ONCE(psi_avgs_task, current);

	mutex_lock(&group->avgs_lock);

	now = sched_clock();

	collect_percpu_times(group, PSI_AVGS, &changed_states);
	nonidle = changed_states & (1 << PSI_NONIDLE);
	/*
	 * If there is task activity, periodically fold the per-cpu
	 * times and feed samples into the running averages. If things
	 * are idle and there is no data to process, stop the clock.
	 * Once restarted, we'll catch up the running averages in one
	 * go - see calc_avgs() and missed_periods.
	 */
	if (now >= group->avg_next_update)
		group->avg_next_update = update_averages(group, now);

	if (nonidle) {
		schedule_delayed_work(dwork, nsecs_to_jiffies(
				group->avg_next_update - now) + 1);
	}

	mutex_unlock(&group->avgs_lock);
}

/* Trigger tracking window manupulations */
static void window_reset(struct psi_window *win, u64 now, u64 value,
			 u64 prev_growth)
{
	win->start_time = now;
	win->start_value = value;
	win->prev_growth = prev_growth;
}

/*
 * PSI growth tracking window update and growth calculation routine.
 *
 * This approximates a sliding tracking window by interpolating
 * partially elapsed windows using historical growth data from the
 * previous intervals. This minimizes memory requirements (by not storing
 * all the intermediate values in the previous window) and simplifies
 * the calculations. It works well because PSI signal changes only in
 * positive direction and over relatively small window sizes the growth
 * is close to linear.
 */
static u64 window_update(struct psi_window *win, u64 now, u64 value)
{
	u64 elapsed;
	u64 growth;

	elapsed = now - win->start_time;
	growth = value - win->start_value;
	/*
	 * After each tracking window passes win->start_value and
	 * win->start_time get reset and win->prev_growth stores
	 * the average per-window growth of the previous window.
	 * win->prev_growth is then used to interpolate additional
	 * growth from the previous window assuming it was linear.
	 */
	if (elapsed > win->size)
		window_reset(win, now, value, growth);
	else {
		u32 remaining;

		remaining = win->size - elapsed;
		growth += div64_u64(win->prev_growth * remaining, win->size);
	}

	return growth;
}

static void init_triggers(struct psi_group *group, u64 now)
{
	struct psi_trigger *t;

	list_for_each_entry(t, &group->triggers, node)
		window_reset(&t->win, now,
				group->total[PSI_POLL][t->state], 0);
	memcpy(group->polling_total, group->total[PSI_POLL],
		   sizeof(group->polling_total));
	group->polling_next_update = now + group->poll_min_period;
}

static u64 update_triggers(struct psi_group *group, u64 now)
{
	struct psi_trigger *t;
	bool new_stall = false;
	u64 *total = group->total[PSI_POLL];

	/*
	 * On subsequent updates, calculate growth deltas and let
	 * watchers know when their specified thresholds are exceeded.
	 */
	list_for_each_entry(t, &group->triggers, node) {
		u64 growth;

		/* Check for stall activity */
		if (group->polling_total[t->state] == total[t->state])
			continue;

		/*
		 * Multiple triggers might be looking at the same state,
		 * remember to update group->polling_total[] once we've
		 * been through all of them. Also remember to extend the
		 * polling time if we see new stall activity.
		 */
		new_stall = true;

		/* Calculate growth since last update */
		growth = window_update(&t->win, now, total[t->state]);
		if (growth < t->threshold)
			continue;

		/* Limit event signaling to once per window */
		if (now < t->last_event_time + t->win.size)
			continue;

		/* Generate an event */
		if (cmpxchg(&t->event, 0, 1) == 0)
			wake_up_interruptible(&t->event_wait);
		t->last_event_time = now;
	}

	if (new_stall)
		memcpy(group->polling_total, total,
				sizeof(group->polling_total));

	return now + group->poll_min_period;
}

/*
 * Schedule polling if it's not already scheduled. It's safe to call even from
 * hotpath because even though schedule_delayed_work takes the workqueue
 * pool lock that lock is never contended, because the poll runs on its own
 * preventing such competition.
 */
/*
 * Both PSI workers run on dedicated kthreads parked on a waitqueue.
 *
 * Upstream 4.9 runs them as kthread_delayed_work bound to a kthread_worker,
 * because the whole point of commit 9f2a5b3d9ad9 was that PSI's own timing
 * must not queue behind unrelated workqueue consumers. This 4.4 tree has no
 * kthread_worker / kthread_delayed_work at all, and pushing PSI onto the
 * normal workqueue would reintroduce exactly that delay - which matters on a
 * 3 GB device doing zram writeback, i.e. precisely when lmkd needs PSI to be
 * accurate.
 *
 * So each psi_group owns two kthreads and two waitqueues. The scheduling
 * entry points stay callable from the hot path: one WRITE_ONCE, one cmpxchg,
 * one wake_up, no sleeping lock.
 */
static void psi_schedule_poll_work(struct psi_group *group, unsigned long delay)
{
	/*
	 * poll_task is NULL until the first trigger is registered, which is
	 * exactly upstream's window (psi_schedule_poll_work() checks
	 * poll_kworker for the same reason).
	 */
	/* Do not reschedule if already scheduled (upstream v5.4) */
	if (atomic_cmpxchg(&group->poll_scheduled, 0, 1) != 0)
		return;

	/*
	 * poll_task is NULL until the first trigger is registered, which is
	 * exactly upstream's window; it clears the flag so a later task change
	 * retries.
	 */
	if (unlikely(!group->poll_task)) {
		atomic_set(&group->poll_scheduled, 0);
		return;
	}

	atomic_long_inc(&psi_dbg_sched);

	/*
	 * Diagnostic: what did insert_kthread_work() see? It only calls
	 * wake_up_process() when current_work is NULL, and 4.4's
	 * kthread_worker_fn sets worker->task itself on its first
	 * instruction - so a NULL here means the wake is skipped.
	 */
	psi_dbg_task_nonnull = (group->poll_kworker.task != NULL);
	psi_dbg_list_empty = list_empty(&group->poll_kworker.work_list);
	psi_dbg_curwork_nonnull = (group->poll_kworker.current_work != NULL);

	/*
	 * Called from the scheduler hot path with rq->lock held, via
	 * enqueue_task -> psi_enqueue -> psi_task_change. Only leaf-spinlock
	 * primitives are legal here. kthread_queue_work() takes
	 * poll_kworker.lock and calls wake_up_process(); that is precisely the
	 * property upstream relies on ("safe to call even from hotpath ...
	 * takes worker->lock spinlock"). A workqueue's mod_delayed_work() is
	 * NOT: it takes a pool lock and, on a WQ_MEM_RECLAIM pool, pulls in the
	 * rescuer machinery. That version hard-hung the whole device here.
	 *
	 * delay 0 or 1 means "as soon as possible" (psi_task_change passes 1),
	 * so it queues directly. Only the longer re-arm delay from
	 * psi_poll_fn() goes through the timer.
	 */
	/*
	 * Always defer through the timer, including delay == 1.
	 *
	 * Upstream's kthread_queue_delayed_work() takes an immediate
	 * kthread_queue_work() branch only when delay == 0, and no PSI caller
	 * ever passes 0: psi_task_change() passes 1 and psi_poll_work() passes
	 * nsecs_to_jiffies(...) + 1. So upstream ALWAYS defers, which matters:
	 * kthread_queue_work() only calls wake_up_process() when worker->task is
	 * already set, and worker->task is set by psimon's own first
	 * instruction in kthread_worker_fn(). Queueing immediately can beat
	 * that, the wake is silently skipped, and the work never runs -
	 * measured on #37 as sched=1 with runs=0 and task_nonnull=0.
	 *
	 * A one-jiffy timer is enough: by the time it fires, psimon is parked
	 * and worker->task is valid.
	 */
	mod_timer(&group->poll_timer, jiffies + delay);
}

static void psi_poll_work(struct kthread_work *work)
{
	struct psi_group *group = container_of(work, struct psi_group, poll_work);

	/* Same ordering as upstream: clear before doing any work. */
	atomic_set(&group->poll_scheduled, 0);

	atomic_long_inc(&psi_dbg_runs);
	psi_poll_fn(group);
}

/*
 * Timer callback standing in for kthread_queue_delayed_work(), which 4.4 lacks.
 * Runs in softirq; kthread_queue_work() takes only poll_kworker.lock and calls
 * wake_up_process(), so it is atomic-safe.
 */
static void psi_poll_timer_fn(unsigned long data)
{
	struct psi_group *group = (struct psi_group *)data;

	kthread_queue_work(&group->poll_kworker, &group->poll_work);
}

static void psi_poll_fn(struct psi_group *group)
{
	u32 changed_states;
	u64 now;

	atomic_long_inc(&psi_dbg_runs);
	mutex_lock(&group->trigger_lock);

	now = sched_clock();

	collect_percpu_times(group, PSI_POLL, &changed_states);

	if (changed_states & group->poll_states) {
		/* Initialize trigger windows when entering polling mode */
		if (now > group->polling_until)
			init_triggers(group, now);

		/*
		 * Keep the monitor active for at least the duration of the
		 * minimum tracking window as long as monitor states are
		 * changing.
		 */
		group->polling_until = now +
			group->poll_min_period * UPDATES_PER_WINDOW;
	}

	if (now > group->polling_until) {
		group->polling_next_update = ULLONG_MAX;
		goto out;
	}

	if (now >= group->polling_next_update) {
		atomic_long_inc(&psi_dbg_trigs);
		group->polling_next_update = update_triggers(group, now);
	}

	psi_schedule_poll_work(group,
		nsecs_to_jiffies(group->polling_next_update - now) + 1);

out:
	mutex_unlock(&group->trigger_lock);
}

static void record_times(struct psi_group_cpu *groupc, int cpu,
			 bool memstall_tick)
{
	u32 delta;
	u64 now;

	now = cpu_clock(cpu);
	delta = now - groupc->state_start;
	groupc->state_start = now;

	if (groupc->state_mask & (1 << PSI_IO_SOME)) {
		groupc->times[PSI_IO_SOME] += delta;
		if (groupc->state_mask & (1 << PSI_IO_FULL))
			groupc->times[PSI_IO_FULL] += delta;
	}

	if (groupc->state_mask & (1 << PSI_MEM_SOME)) {
		groupc->times[PSI_MEM_SOME] += delta;
		if (groupc->state_mask & (1 << PSI_MEM_FULL))
			groupc->times[PSI_MEM_FULL] += delta;
		else if (memstall_tick) {
			u32 sample;
			/*
			 * Since we care about lost potential, a
			 * memstall is FULL when there are no other
			 * working tasks, but also when the CPU is
			 * actively reclaiming and nothing productive
			 * could run even if it were runnable.
			 *
			 * When the timer tick sees a reclaiming CPU,
			 * regardless of runnable tasks, sample a FULL
			 * tick (or less if it hasn't been a full tick
			 * since the last state change).
			 */
			sample = min(delta, (u32)jiffies_to_nsecs(1));
			groupc->times[PSI_MEM_FULL] += sample;
		}
	}

	if (groupc->state_mask & (1 << PSI_CPU_SOME))
		groupc->times[PSI_CPU_SOME] += delta;

	if (groupc->state_mask & (1 << PSI_NONIDLE))
		groupc->times[PSI_NONIDLE] += delta;
}

static u32 psi_group_change(struct psi_group *group, int cpu,
			    unsigned int clear, unsigned int set)
{
	struct psi_group_cpu *groupc;
	unsigned int t, m;
	enum psi_states s;
	u32 state_mask = 0;

	groupc = per_cpu_ptr(group->pcpu, cpu);

	/*
	 * First we assess the aggregate resource states this CPU's
	 * tasks have been in since the last change, and account any
	 * SOME and FULL time these may have resulted in.
	 *
	 * Then we update the task counts according to the state
	 * change requested through the @clear and @set bits.
	 */
	write_seqcount_begin(&groupc->seq);

	record_times(groupc, cpu, false);

	for (t = 0, m = clear; m; m &= ~(1 << t), t++) {
		if (!(m & (1 << t)))
			continue;
		if (groupc->tasks[t] == 0 && !psi_bug) {
			printk_deferred(KERN_ERR "psi: task underflow! cpu=%d t=%d tasks=[%u %u %u] clear=%x set=%x\n",
					cpu, t, groupc->tasks[0],
					groupc->tasks[1], groupc->tasks[2],
					clear, set);
			psi_bug = 1;
		}
		groupc->tasks[t]--;
	}

	for (t = 0; set; set &= ~(1 << t), t++)
		if (set & (1 << t))
			groupc->tasks[t]++;

	/* Calculate state mask representing active states */
	for (s = 0; s < NR_PSI_STATES; s++) {
		if (test_state(groupc->tasks, s))
			state_mask |= (1 << s);
	}
	groupc->state_mask = state_mask;

	write_seqcount_end(&groupc->seq);

	return state_mask;
}


void psi_task_change(struct task_struct *task, int clear, int set)
{
	int cpu = task_cpu(task);
	struct psi_group *group = &psi_system;
	bool wake_clock = true;
	u32 state_mask;

	if (!task->pid)
		return;

	if (((task->psi_flags & set) ||
	     (task->psi_flags & clear) != clear) &&
	    !psi_bug) {
		printk_deferred(KERN_ERR "psi: inconsistent task state! task=%d:%s cpu=%d psi_flags=%x clear=%x set=%x\n",
				task->pid, task->comm, cpu,
				task->psi_flags, clear, set);
		psi_bug = 1;
	}

	task->psi_flags &= ~clear;
	task->psi_flags |= set;

	/*
	 * Periodic aggregation shuts off if there is a period of no task
	 * changes, so we wake it back up if necessary. However, don't do this
	 * if the task change is the aggregation worker itself going to sleep,
	 * or we'll ping-pong forever.
	 *
	 * Upstream asks the workqueue "which function did this worker last
	 * run?" via wq_worker_last_func(), which 4.4 does not have. The
	 * equivalent here is a persistent task pointer recorded by
	 * psi_avgs_work(): by the time the worker dequeues to sleep, its
	 * current_func has already been cleared, so only a remembered task
	 * identity catches the ping-pong case. A worker task that once ran
	 * psi_avgs_work simply never kicks the clock again, which is benign -
	 * upstream's last-func test has the same permanent effect.
	 */
	if (unlikely((clear & TSK_RUNNING) &&
		     (task->flags & PF_WQ_WORKER) &&
		     task == READ_ONCE(psi_avgs_task)))
		wake_clock = false;

	state_mask = psi_group_change(group, cpu, clear, set);

	if (state_mask & group->poll_states)
		psi_schedule_poll_work(group, 1);

	/*
	 * Do not queue PSI's averaging worker until the workqueue that
	 * schedule_delayed_work() actually uses exists.
	 *
	 * include/linux/workqueue.h:592 makes schedule_delayed_work() call
	 * queue_delayed_work_on(cpu, system_power_efficient_wq, ...), NOT
	 * system_wq - this tree redirects the global delayed-work helpers at
	 * the WQ_POWER_EFFICIENT pool. The pointer that must be non-NULL here
	 * is therefore system_power_efficient_wq.
	 *
	 * There is no workqueue_init_early() here, so all of these are created
	 * by early_initcall(init_workqueues) (kernel/workqueue.c), which runs
	 * from do_initcalls() inside kernel_init_freeable() - after rest_init()
	 * has already forked kernel_init and kthreadd, the first tasks with a
	 * non-zero ->pid and therefore the first callers of psi_task_change().
	 *
	 * init_workqueues() creates them in this order:
	 *   system_wq                            (non-NULL from here on)
	 *   system_highpri_wq / system_long_wq / system_unbound_wq
	 *   system_freezable_wq
	 *   system_power_efficient_wq            <-- still NULL during the above
	 *
	 * Each alloc_workqueue() wakes its own first worker, and that
	 * kthread_create_on_node() -> try_to_wake_up() -> activate_task() runs
	 * psi_enqueue() -> psi_task_change() on this very CPU while the pointer
	 * for the pool being created is not yet assigned. Measured stack from
	 * the kernel that hung - three boots, byte-identical offsets:
	 *
	 *   __queue_delayed_work+0xb8      WARN_ON_ONCE(!wq)
	 *   psi_task_change+0x380
	 *   activate_task+0x13c
	 *   try_to_wake_up+0x348
	 *   kthread_create_on_node+0xd0
	 *   create_worker+0xe0
	 *   apply_wqattrs_prepare+0x28c
	 *   apply_workqueue_attrs_locked+0x34
	 *   apply_workqueue_attrs+0x38
	 *   __alloc_workqueue_key+0x26c
	 *   init_workqueues+0x30c
	 *
	 * queue_delayed_work_on() does not fault there: __queue_delayed_work()
	 * only WARNs, stores dwork->wq = NULL and arms the timer. That NULL is
	 * dereferenced PSI_FREQ = 2*HZ+1 = 1001 jiffies (2.002 s at
	 * CONFIG_HZ=500) later, in delayed_work_timer_fn() -> __queue_work(),
	 * at wq->flags - which is the actual hang.
	 *
	 * NOTE: guarding on system_wq, or on keventd_up(), does NOT work here.
	 * system_wq is created first, so it is already non-NULL during the
	 * window that matters, and keventd_up() (workqueue.h:623) reports true
	 * while system_power_efficient_wq is still NULL. An earlier attempt at
	 * this fix guarded system_wq, compiled cleanly, and still hung with the
	 * identical stack: the check was on a different variable from the one
	 * schedule_delayed_work() passes.
	 *
	 * Upstream v5.4 cannot hit any of this: workqueue_init_early() runs in
	 * start_kernel() right after sched_init(), so the pools exist before any
	 * task can change scheduler state. It is also why enabling PSI from
	 * userspace has always worked - by the time /proc/pressure/enable can be
	 * written, init_workqueues has finished.
	 *
	 * Nothing can read /proc/pressure before initcalls, so skipping the kick
	 * in this window loses no observable behaviour: WORK_STRUCT_PENDING_BIT
	 * is left clear, so the first task change after the pool exists queues
	 * the worker normally.
	 */
	if (wake_clock && system_power_efficient_wq &&
	    !delayed_work_pending(&group->avgs_work))
		schedule_delayed_work(&group->avgs_work, PSI_FREQ);
}

void psi_memstall_tick(struct task_struct *task, int cpu)
{
	struct psi_group_cpu *groupc = per_cpu_ptr(psi_system.pcpu, cpu);

	write_seqcount_begin(&groupc->seq);
	record_times(groupc, cpu, true);
	write_seqcount_end(&groupc->seq);
}

/**
 * psi_memstall_enter - mark the beginning of a memory stall section
 * @flags: flags to handle nested sections
 *
 * Marks the calling task as being stalled due to a lack of memory,
 * such as waiting for a refault or performing reclaim.
 */
void psi_memstall_enter(unsigned long *flags)
{
	unsigned long rf;
	struct rq *rq;

	if (static_branch_likely(&psi_disabled))
		return;

	/*
	 * Nesting guard.
	 *
	 * Upstream tests current->flags & PF_MEMSTALL, but that bit is
	 * PF_PERF_CRITICAL in this 4.4 tree. PSI_TSK_IN_MEMSTALL in
	 * task->psi_flags plays that role. It must not be TSK_MEMSTALL itself:
	 * that one is the per-CPU "currently counted" state and is cleared on
	 * every dequeue for migration, see psi_types.h.
	 */
	*flags = current->psi_flags & PSI_TSK_IN_MEMSTALL;
	if (*flags)
		return;

	/*
	 * The accounting needs to be atomic wrt changes to the task's
	 * scheduling state, otherwise we can race with CPU migration and
	 * credit the stall to the wrong per-CPU bucket.
	 *
	 * 4.4 has no this_rq_lock_irq() and no struct rq_flags. task_rq_lock()
	 * is the direct equivalent: it takes pi_lock and the runqueue lock
	 * with interrupts disabled.
	 */
	rq = task_rq_lock(current, &rf);

	psi_task_change(current, 0, TSK_MEMSTALL);
	current->psi_flags |= PSI_TSK_IN_MEMSTALL;

	task_rq_unlock(rq, current, &rf);
}

/**
 * psi_memstall_leave - mark the end of an memory stall section
 * @flags: flags to handle nested memdelay sections
 *
 * Marks the calling task as no longer stalled due to lack of memory.
 */
void psi_memstall_leave(unsigned long *flags)
{
	unsigned long rf;
	struct rq *rq;

	if (static_branch_likely(&psi_disabled))
		return;

	if (*flags)
		return;

	rq = task_rq_lock(current, &rf);

	psi_task_change(current, TSK_MEMSTALL, 0);
	current->psi_flags &= ~PSI_TSK_IN_MEMSTALL;

	task_rq_unlock(rq, current, &rf);
}


int psi_show(struct seq_file *m, struct psi_group *group, enum psi_res res)
{
	int full;
	u64 now;

	if (static_branch_likely(&psi_disabled))
		return -EOPNOTSUPP;

	/* Update averages before reporting them */
	mutex_lock(&group->avgs_lock);
	now = sched_clock();
	collect_percpu_times(group, PSI_AVGS, NULL);
	if (now >= group->avg_next_update)
		group->avg_next_update = update_averages(group, now);
	mutex_unlock(&group->avgs_lock);

	for (full = 0; full < 2 - (res == PSI_CPU); full++) {
		unsigned long avg[3];
		u64 total;
		int w;

		for (w = 0; w < 3; w++)
			avg[w] = group->avg[res * 2 + full][w];
		total = div_u64(group->total[PSI_AVGS][res * 2 + full],
				NSEC_PER_USEC);

		seq_printf(m, "%s avg10=%lu.%02lu avg60=%lu.%02lu avg300=%lu.%02lu total=%llu\n",
			   full ? "full" : "some",
			   LOAD_INT(avg[0]), LOAD_FRAC(avg[0]),
			   LOAD_INT(avg[1]), LOAD_FRAC(avg[1]),
			   LOAD_INT(avg[2]), LOAD_FRAC(avg[2]),
			   total);
	}

	return 0;
}

static int psi_io_show(struct seq_file *m, void *v)
{
	return psi_show(m, &psi_system, PSI_IO);
}

static int psi_memory_show(struct seq_file *m, void *v)
{
	return psi_show(m, &psi_system, PSI_MEM);
}

static int psi_cpu_show(struct seq_file *m, void *v)
{
	return psi_show(m, &psi_system, PSI_CPU);
}

static int psi_io_open(struct inode *inode, struct file *file)
{
	return single_open(file, psi_io_show, NULL);
}

static int psi_memory_open(struct inode *inode, struct file *file)
{
	return single_open(file, psi_memory_show, NULL);
}

static int psi_cpu_open(struct inode *inode, struct file *file)
{
	return single_open(file, psi_cpu_show, NULL);
}

struct psi_trigger *psi_trigger_create(struct psi_group *group,
			char *buf, size_t nbytes, enum psi_res res)
{
	struct psi_trigger *t;
	enum psi_states state;
	u32 threshold_us;
	u32 window_us;

	if (static_branch_likely(&psi_disabled))
		return ERR_PTR(-EOPNOTSUPP);

	if (sscanf(buf, "some %u %u", &threshold_us, &window_us) == 2)
		state = PSI_IO_SOME + res * 2;
	else if (sscanf(buf, "full %u %u", &threshold_us, &window_us) == 2)
		state = PSI_IO_FULL + res * 2;
	else
		return ERR_PTR(-EINVAL);

	if (state >= PSI_NONIDLE)
		return ERR_PTR(-EINVAL);

	if (window_us < WINDOW_MIN_US ||
		window_us > WINDOW_MAX_US)
		return ERR_PTR(-EINVAL);

	/* Check threshold */
	if (threshold_us == 0 || threshold_us > window_us)
		return ERR_PTR(-EINVAL);

	t = kmalloc(sizeof(*t), GFP_KERNEL);
	if (!t)
		return ERR_PTR(-ENOMEM);

	t->group = group;
	t->state = state;
	t->threshold = threshold_us * NSEC_PER_USEC;
	t->win.size = window_us * NSEC_PER_USEC;
	window_reset(&t->win, 0, 0, 0);

	t->event = 0;
	t->last_event_time = 0;
	init_waitqueue_head(&t->event_wait);
	kref_init(&t->refcount);

	mutex_lock(&group->trigger_lock);

	/*
	 * Same placement as upstream's kthread_create_worker(0, "psimon"): lazily
	 * on the first trigger, never at init time.
	 */
	if (unlikely(!group->poll_task)) {
		struct sched_param param = {
			.sched_priority = 1,
		};

		kthread_init_work(&group->poll_work, psi_poll_work);
		setup_timer(&group->poll_timer, psi_poll_timer_fn,
			    (unsigned long)group);

		group->poll_task = kthread_create(kthread_worker_fn,
						  &group->poll_kworker,
						  "psimon");
		if (IS_ERR(group->poll_task)) {
			del_timer(&group->poll_timer);
			kfree(t);
			mutex_unlock(&group->trigger_lock);
			return ERR_CAST(group->poll_task);
		}
		/*
		 * Adopt the task into the worker and WAKE IT.
		 *
		 * create_kthread() goes through kernel_thread(), which leaves the
		 * new task parked; only kthread() itself wakes it. Nothing in this
		 * port did, so psimon never reached kthread_worker_fn(): it never
		 * set worker->task and never drained work_list. Measured on #37 as
		 * psi_dbg_sched climbing 1->3 with psi_dbg_runs stuck at 0 and
		 * "ps -A -T | grep psimon" returning nothing.
		 *
		 * Setting .task first is what makes insert_kthread_work()'s
		 * `likely(worker->task)` gate pass, so a work queued before the
		 * thread's first instruction still gets its wake_up_process().
		 * This is upstream v4.9's kthread_create_worker(), which does
		 * exactly these two things.
		 */
		group->poll_kworker.task = group->poll_task;
		wake_up_process(group->poll_task);

		/* Upstream does exactly this to its psimon task. */
		sched_setscheduler_nocheck(group->poll_task, SCHED_FIFO,
					   &param);
	}

	list_add(&t->node, &group->triggers);
	group->poll_min_period = min(group->poll_min_period,
		div_u64(t->win.size, UPDATES_PER_WINDOW));
	group->nr_triggers[t->state]++;
	group->poll_states |= (1 << t->state);

	mutex_unlock(&group->trigger_lock);

	return t;
}

static void psi_trigger_destroy(struct kref *ref)
{
	struct psi_trigger *t = container_of(ref, struct psi_trigger, refcount);
	struct psi_group *group = t->group;
	struct task_struct *poll_task_to_stop = NULL;

	if (static_branch_likely(&psi_disabled))
		return;

	/*
	 * Wakeup waiters to stop polling. Can happen if cgroup is deleted
	 * from under a polling process.
	 */
	wake_up_interruptible(&t->event_wait);

	mutex_lock(&group->trigger_lock);

	if (!list_empty(&t->node)) {
		struct psi_trigger *tmp;
		u64 period = ULLONG_MAX;

		list_del(&t->node);
		group->nr_triggers[t->state]--;
		if (!group->nr_triggers[t->state])
			group->poll_states &= ~(1 << t->state);
		/* reset min update period for the remaining triggers */
		list_for_each_entry(tmp, &group->triggers, node)
			period = min(period, div_u64(tmp->win.size,
					UPDATES_PER_WINDOW));
		group->poll_min_period = period;
		/* Destroy psimon when the last trigger is destroyed */
		if (group->poll_states == 0) {
			group->polling_until = 0;
			poll_task_to_stop = group->poll_task;
			group->poll_task = NULL;
		}
	}

	mutex_unlock(&group->trigger_lock);

	/*
	 * Wait for *trigger_ptr readers from psi_trigger_replace to leave
	 * their RCU read-side critical section before destroying the trigger.
	 */
	synchronize_rcu();
	/*
	 * Destroy psimon after releasing trigger_lock, to prevent a deadlock
	 * while waiting for psi_poll_fn() to acquire trigger_lock.
	 */
	if (poll_task_to_stop) {
		del_timer_sync(&group->poll_timer);
		kthread_cancel_work_sync(&group->poll_work);
		atomic_set(&group->poll_scheduled, 0);
		kthread_stop(poll_task_to_stop);
	}
	kfree(t);
}

void psi_trigger_replace(void **trigger_ptr, struct psi_trigger *new)
{
	struct psi_trigger *old = *trigger_ptr;

	if (static_branch_likely(&psi_disabled))
		return;

	rcu_assign_pointer(*trigger_ptr, new);
	if (old)
		kref_put(&old->refcount, psi_trigger_destroy);
}

unsigned int psi_trigger_poll(void **trigger_ptr, struct file *file,
			      poll_table *wait)
{
	unsigned int ret = DEFAULT_POLLMASK;
	struct psi_trigger *t;

	if (static_branch_likely(&psi_disabled))
		return DEFAULT_POLLMASK | POLLERR | POLLPRI;

	rcu_read_lock();

	t = rcu_dereference(*(void __rcu __force **)trigger_ptr);
	if (!t) {
		rcu_read_unlock();
		return DEFAULT_POLLMASK | POLLERR | POLLPRI;
	}
	kref_get(&t->refcount);

	rcu_read_unlock();

	poll_wait(file, &t->event_wait, wait);

	if (cmpxchg(&t->event, 1, 0) == 1)
		ret |= POLLPRI;

	kref_put(&t->refcount, psi_trigger_destroy);

	return ret;
}

static ssize_t psi_write(struct file *file, const char __user *user_buf,
			 size_t nbytes, enum psi_res res)
{
	char buf[32];
	size_t buf_size;
	struct seq_file *seq;
	struct psi_trigger *new;

	if (static_branch_likely(&psi_disabled))
		return -EOPNOTSUPP;

	if (!nbytes)
		return -EINVAL;

	buf_size = min(nbytes, sizeof(buf));
	if (copy_from_user(buf, user_buf, buf_size))
		return -EFAULT;

	buf[buf_size - 1] = '\0';

	new = psi_trigger_create(&psi_system, buf, nbytes, res);
	if (IS_ERR(new))
		return PTR_ERR(new);

	seq = file->private_data;
	/* Take seq->lock to protect seq->private from concurrent writes */
	mutex_lock(&seq->lock);
	psi_trigger_replace(&seq->private, new);
	mutex_unlock(&seq->lock);

	return nbytes;
}

static ssize_t psi_io_write(struct file *file, const char __user *user_buf,
			    size_t nbytes, loff_t *ppos)
{
	return psi_write(file, user_buf, nbytes, PSI_IO);
}

static ssize_t psi_memory_write(struct file *file, const char __user *user_buf,
				size_t nbytes, loff_t *ppos)
{
	return psi_write(file, user_buf, nbytes, PSI_MEM);
}

static ssize_t psi_cpu_write(struct file *file, const char __user *user_buf,
			     size_t nbytes, loff_t *ppos)
{
	return psi_write(file, user_buf, nbytes, PSI_CPU);
}

static unsigned int psi_fop_poll(struct file *file, poll_table *wait)
{
	struct seq_file *seq = file->private_data;

	return psi_trigger_poll(&seq->private, file, wait);
}

static int psi_fop_release(struct inode *inode, struct file *file)
{
	struct seq_file *seq = file->private_data;

	psi_trigger_replace(&seq->private, NULL);
	return single_release(inode, file);
}

static const struct file_operations psi_io_fops = {
	.open           = psi_io_open,
	.read           = seq_read,
	.llseek         = seq_lseek,
	.write          = psi_io_write,
	.poll           = psi_fop_poll,
	.release        = psi_fop_release,
};

static const struct file_operations psi_memory_fops = {
	.open           = psi_memory_open,
	.read           = seq_read,
	.llseek         = seq_lseek,
	.write          = psi_memory_write,
	.poll           = psi_fop_poll,
	.release        = psi_fop_release,
};

static const struct file_operations psi_cpu_fops = {
	.open           = psi_cpu_open,
	.read           = seq_read,
	.llseek         = seq_lseek,
	.write          = psi_cpu_write,
	.poll           = psi_fop_poll,
	.release        = psi_fop_release,
};

/*
 * Runtime switch: /proc/pressure/enable
 *
 * Non-standard, and deliberately so. libpsi only ever touches the three
 * /proc/pressure/{io,memory,cpu} files, so adding a fourth file cannot confuse
 * it. This exists because psi=1 on the kernel command line is unusable on this
 * device: the bootloader owns /proc/cmdline and boot.img's cmdline field is
 * ignored, so CONFIG_PSI_DEFAULT_DISABLED can never be overridden at boot.
 *
 *   echo 1 > /proc/pressure/enable   - start stall accounting now
 *   echo 0 > /proc/pressure/enable   - stop it
 *
 * Enabling clears the psi_disabled branch and kicks the averages work.
 * Disabling raises it again and cancels the work. Disabling is best-effort: a
 * task already inside psi_task_change() may finish its accounting, which is
 * harmless (it only bumps counters) but means the transition is not instantly
 * quiescent.
 */
static ssize_t psi_enable_read(struct file *file, char __user *buf,
			       size_t count, loff_t *ppos)
{
	char s[2];

	char d[80];
	int n;

	n = scnprintf(d, sizeof(d), "%d %ld %ld %ld %d %d %d\n",
		      static_branch_unlikely(&psi_disabled) ? 0 : 1,
		      atomic_long_read(&psi_dbg_sched),
		      atomic_long_read(&psi_dbg_runs),
		      atomic_long_read(&psi_dbg_trigs),
		      psi_dbg_task_nonnull,
		      psi_dbg_list_empty,
		      psi_dbg_curwork_nonnull);
	(void)s;
	return simple_read_from_buffer(buf, count, ppos, d, n);
}

static ssize_t psi_enable_write(struct file *file, const char __user *ubuf,
				size_t count, loff_t *ppos)
{
	char s[2];
	bool on;

	if (count < 1)
		return -EINVAL;
	if (copy_from_user(s, ubuf, 1))
		return -EFAULT;
	on = (s[0] == '1');
	if (s[0] != '0' && s[0] != '1')
		return -EINVAL;

	if (on) {
		static_branch_disable(&psi_disabled);
		/* Start folding per-cpu stall buckets into the averages. */
		if (!delayed_work_pending(&psi_system.avgs_work))
			schedule_delayed_work(&psi_system.avgs_work,
					      PSI_FREQ);
	} else {
		static_branch_enable(&psi_disabled);
		cancel_delayed_work_sync(&psi_system.avgs_work);
	}

	return count;
}

static const struct file_operations psi_enable_fops = {
	.open	= simple_open,
	.read	= psi_enable_read,
	.write	= psi_enable_write,
	.llseek	= noop_llseek,
};

static int __init psi_proc_init(void)
{
	proc_mkdir("pressure", NULL);
	proc_create("pressure/io", 0, NULL, &psi_io_fops);
	proc_create("pressure/memory", 0, NULL, &psi_memory_fops);
	proc_create("pressure/cpu", 0, NULL, &psi_cpu_fops);
	proc_create("pressure/enable", 0644, NULL, &psi_enable_fops);
	return 0;
}
module_init(psi_proc_init);
