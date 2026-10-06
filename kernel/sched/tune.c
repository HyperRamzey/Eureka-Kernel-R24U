#include <linux/cgroup.h>
#include <linux/err.h>
#include <linux/percpu.h>
#include <linux/printk.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>

#include "sched.h"
#include "tune.h"

unsigned int sysctl_sched_cfs_boost __read_mostly;

/*
 * System energy normalization constants, computed from the platform
 * energy model at late init (EAS). While min/max are zero the payoff
 * computation falls back to the raw energy delta.
 */
struct target_nrg schedtune_target_nrg;

/*
 * Performance/Energy threshold hysteresis (EAS SchedTune). Each entry
 * maps a boost range to a (nrg_gain, cap_gain) pair used by the payoff
 * computation in __schedtune_accept_deltas().
 */
struct threshold_params {
	int nrg_gain;
	int cap_gain;
};

static struct threshold_params
threshold_gains[] = {
	{ 0, 5 }, /*   < 10% */
	{ 1, 5 }, /*   < 20% */
	{ 2, 5 }, /*   < 30% */
	{ 3, 5 }, /*   < 40% */
	{ 4, 5 }, /*   < 50% */
	{ 5, 4 }, /*   < 60% */
	{ 5, 3 }, /*   < 70% */
	{ 5, 2 }, /*   < 80% */
	{ 5, 1 }, /*   < 90% */
	{ 5, 0 }  /* <= 100% */
};

/*
 * Evaluate an (energy, capacity) variation pair. A positive payoff is the
 * accept condition for the candidate scheduling decision.
 */
static int
__schedtune_accept_deltas(int nrg_delta, int cap_delta,
			  int perf_boost_idx, int perf_constrain_idx)
{
	int payoff = -INT_MAX;
	int gain_idx = -1;

	/* Performance Boost (B) region */
	if (nrg_delta >= 0 && cap_delta > 0)
		gain_idx = perf_boost_idx;
	/* Performance Constraint (C) region */
	else if (nrg_delta < 0 && cap_delta <= 0)
		gain_idx = perf_constrain_idx;

	/* Default: reject the schedule candidate */
	if (gain_idx == -1)
		return payoff;

	/* payoff = (cap_delta * nrg_gain) - (cap_gain * nrg_delta) */
	payoff  = cap_delta * threshold_gains[gain_idx].nrg_gain;
	payoff -= nrg_delta * threshold_gains[gain_idx].cap_gain;

	return payoff;
}

#ifdef CONFIG_CGROUP_SCHEDTUNE

bool schedtune_initialized = false;

/*
 * EAS scheduler tunables for task groups.
 */

/* SchdTune tunables for a group of tasks */
struct schedtune {
	/* SchedTune CGroup subsystem */
	struct cgroup_subsys_state css;

	/* Boost group allocated ID */
	int idx;

	/* Boost value for tasks on that SchedTune CGroup */
	int boost;

	/* Performance Boost (B) region threshold index */
	int perf_boost_idx;

	/* Performance Constraint (C) region threshold index */
	int perf_constrain_idx;

	/* Bias placement of this group towards idle CPUs */
	int prefer_idle;
};

static inline struct schedtune *css_st(struct cgroup_subsys_state *css)
{
	return css ? container_of(css, struct schedtune, css) : NULL;
}

static inline struct schedtune *task_schedtune(struct task_struct *tsk)
{
	return css_st(task_css(tsk, schedtune_cgrp_id));
}

static inline struct schedtune *parent_st(struct schedtune *st)
{
	return css_st(st->css.parent);
}

/*
 * SchedTune root control group
 * The root control group is used to defined a system-wide boosting tuning,
 * which is applied to all tasks in the system.
 * Task specific boost tuning could be specified by creating and
 * configuring a child control group under the root one.
 * By default, system-wide boosting is disabled, i.e. no boosting is applied
 * to tasks which are not into a child control group.
 */
static struct schedtune
root_schedtune = {
	.boost	= 0,
	.perf_boost_idx = 0,
	.perf_constrain_idx = 0,
	.prefer_idle = 0,
};

/*
 * Maximum number of boost groups to support
 * When per-task boosting is used we still allow only limited number of
 * boost groups for two main reasons:
 * 1. on a real system we usually have only few classes of workloads which
 *    make sense to boost with different values (e.g. background vs foreground
 *    tasks, interactive vs low-priority tasks)
 * 2. a limited number allows for a simpler and more memory/time efficient
 *    implementation especially for the computation of the per-CPU boost
 *    value
 */
#define BOOSTGROUPS_COUNT 8

/* Array of configured boostgroups */
static struct schedtune *allocated_group[BOOSTGROUPS_COUNT] = {
	&root_schedtune,
	NULL,
};

/* SchedTune boost groups
 * Keep track of all the boost groups which impact on CPU, for example when a
 * CPU has two RUNNABLE tasks belonging to two different boost groups and thus
 * likely with different boost values.
 * Since on each system we expect only a limited number of boost groups, here
 * we use a simple array to keep track of the metrics required to compute the
 * maximum per-CPU boosting value.
 */
struct boost_groups {
	/* Maximum boost value for all RUNNABLE tasks on a CPU */
	int boost_max;
	struct {
		/* The boost for tasks on that boost group */
		int boost;
		/* Count of RUNNABLE tasks on that boost group */
		unsigned tasks;
	} group[BOOSTGROUPS_COUNT];
};

/* Boost groups affecting each CPU in the system */
DEFINE_PER_CPU(struct boost_groups, cpu_boost_groups);

static void
schedtune_cpu_update(int cpu)
{
	struct boost_groups *bg;
	int boost_max;
	int idx;

	bg = &per_cpu(cpu_boost_groups, cpu);

	/* The root boost group is always active */
	boost_max = bg->group[0].boost;
	for (idx = 0; idx < BOOSTGROUPS_COUNT; ++idx) {
		/*
		 * A boost group affects a CPU only if it has
		 * RUNNABLE tasks on that CPU
		 */
		if (bg->group[idx].tasks == 0)
			continue;
		boost_max = max(boost_max, bg->group[idx].boost);
	}

	bg->boost_max = boost_max;
}

static int
schedtune_boostgroup_update(int idx, int boost)
{
	struct boost_groups *bg;
	int cur_boost_max;
	int old_boost;
	int cpu;

	/* Update per CPU boost groups */
	for_each_possible_cpu(cpu) {
		bg = &per_cpu(cpu_boost_groups, cpu);

		/*
		 * Keep track of current boost values to compute the per CPU
		 * maximum only when it has been affected by the new value of
		 * the updated boost group
		 */
		cur_boost_max = bg->boost_max;
		old_boost = bg->group[idx].boost;

		/* Update the boost value of this boost group */
		bg->group[idx].boost = boost;

		/* Check if this update increase current max */
		if (boost > cur_boost_max && bg->group[idx].tasks) {
			bg->boost_max = boost;
			continue;
		}

		/* Check if this update has decreased current max */
		if (cur_boost_max == old_boost && old_boost > boost)
			schedtune_cpu_update(cpu);
	}

	return 0;
}

#define ENQUEUE_TASK  1
#define DEQUEUE_TASK -1

static inline void
schedtune_tasks_update(struct task_struct *p, int cpu, int idx, int task_count)
{
	struct boost_groups *bg;
	int tasks;

	bg = &per_cpu(cpu_boost_groups, cpu);

	/* Update boosted tasks count while avoiding to make it negative */
	if (task_count < 0 && bg->group[idx].tasks <= -task_count)
		bg->group[idx].tasks = 0;
	else
		bg->group[idx].tasks += task_count;

	/* Boost group activation or deactivation on that RQ */
	tasks = bg->group[idx].tasks;
	if (tasks == 1 || tasks == 0)
		schedtune_cpu_update(cpu);
}

/*
 * NOTE: This function must be called while holding the lock on the CPU RQ
 */
void schedtune_enqueue_task(struct task_struct *p, int cpu)
{
	struct schedtune *st;
	int idx;

	if (!unlikely(schedtune_initialized))
		return;

	/*
	 * When a task is marked PF_EXITING by do_exit() it's going to be
	 * dequeued and enqueued multiple times in the exit path.
	 * Thus we avoid any further update, since we do not want to change
	 * CPU boosting while the task is exiting.
	 */
	if (p->flags & PF_EXITING)
		return;

	/* Get task boost group */
	rcu_read_lock();
	st = task_schedtune(p);
	idx = st->idx;
	rcu_read_unlock();

	schedtune_tasks_update(p, cpu, idx, ENQUEUE_TASK);
}

/*
 * NOTE: This function must be called while holding the lock on the CPU RQ
 */
void schedtune_dequeue_task(struct task_struct *p, int cpu)
{
	struct schedtune *st;
	int idx;

	if (!unlikely(schedtune_initialized))
		return;

	/*
	 * When a task is marked PF_EXITING by do_exit() it's going to be
	 * dequeued and enqueued multiple times in the exit path.
	 * Thus we avoid any further update, since we do not want to change
	 * CPU boosting while the task is exiting.
	 * The last dequeue will be done by cgroup exit() callback.
	 */
	if (p->flags & PF_EXITING)
		return;

	/* Get task boost group */
	rcu_read_lock();
	st = task_schedtune(p);
	idx = st->idx;
	rcu_read_unlock();

	schedtune_tasks_update(p, cpu, idx, -1);
}

void schedtune_exit_task(struct task_struct *tsk)
{
	struct schedtune *st;
	unsigned long irq_flags;
	unsigned int cpu;
	struct rq *rq;
	int idx;

	if (!unlikely(schedtune_initialized))
		return;

	rq = lock_rq_of(tsk, &irq_flags);
	rcu_read_lock();

	cpu = cpu_of(rq);
	st = task_schedtune(tsk);
	idx = st->idx;
	schedtune_tasks_update(tsk, cpu, idx, DEQUEUE_TASK);

	rcu_read_unlock();
	unlock_rq_of(rq, tsk, &irq_flags);
}

int schedtune_cpu_boost(int cpu)
{
	struct boost_groups *bg;

	bg = &per_cpu(cpu_boost_groups, cpu);
	return bg->boost_max;
}

static inline int schedtune_adj_ta(struct task_struct *p)
{
	struct schedtune *st;
	char name_buf[NAME_MAX + 1];
	int adj = p->signal->oom_score_adj;

	/* We only care about adj == 0 */
	if (adj != 0)
		return 0;

	/* Don't touch kthreads */
	if (p->flags & PF_KTHREAD)
		return 0;

	st = task_schedtune(p);
	cgroup_name(st->css.cgroup, name_buf, sizeof(name_buf));
	if (!strncmp(name_buf, "top-app", strlen("top-app"))) {
		pr_debug("top app is %s with adj %i\n", p->comm, adj);
		return 1;
	}

	return 0;
}

int schedtune_task_boost(struct task_struct *p)
{
	struct schedtune *st;
	int task_boost;

	if (!unlikely(schedtune_initialized))
		return 0;

	/* Get task boost value */
	rcu_read_lock();
	st = task_schedtune(p);
	task_boost = max(st->boost, schedtune_adj_ta(p));
	rcu_read_unlock();

	return task_boost;
}

int schedtune_prefer_idle(struct task_struct *p)
{
	struct schedtune *st;
	int prefer_idle;

	if (!unlikely(schedtune_initialized))
		return 0;

	rcu_read_lock();
	st = task_schedtune(p);
	prefer_idle = st->prefer_idle;
	rcu_read_unlock();

	return prefer_idle;
}

/*
 * EAS: accept or reject a candidate scheduling decision expressed as an
 * (energy, capacity) variation pair, using the boost group's configured
 * performance/energy thresholds.
 */
int
schedtune_accept_deltas(int nrg_delta, int cap_delta,
			struct task_struct *task)
{
	struct schedtune *ct;
	int perf_boost_idx;
	int perf_constrain_idx;

	/* Optimal (O) region */
	if (nrg_delta < 0 && cap_delta > 0)
		return INT_MAX;

	/* Suboptimal (S) region */
	if (nrg_delta > 0 && cap_delta < 0)
		return -INT_MAX;

	rcu_read_lock();
	ct = task_schedtune(task);
	perf_boost_idx = ct->perf_boost_idx;
	perf_constrain_idx = ct->perf_constrain_idx;
	rcu_read_unlock();

	return __schedtune_accept_deltas(nrg_delta, cap_delta,
			perf_boost_idx, perf_constrain_idx);
}

static s64
boost_read(struct cgroup_subsys_state *css, struct cftype *cft)
{
	struct schedtune *st = css_st(css);

	return st->boost;
}

static int
boost_write(struct cgroup_subsys_state *css, struct cftype *cft,
	    s64 boost)
{
	struct schedtune *st = css_st(css);

	if (!strcmp(css->cgroup->kn->name, "top-app"))
		boost = 1;

	if (boost < -100 || boost > 100)
		return -EINVAL;

	if (boost <= 10)
		boost = 0;

	st->boost = boost;
	if (css == &root_schedtune.css)
		sysctl_sched_cfs_boost = boost;

	/*
	 * Keep the performance/energy payoff thresholds in step with the
	 * boost: one threshold_gains[] bucket per 10% of boost. perf_boost and
	 * perf_constrain can still be overridden individually afterwards.
	 */
	st->perf_boost_idx = clamp((int)boost, 0, 99) / 10;
	st->perf_constrain_idx = st->perf_boost_idx;

	/* Update CPU boost */
	schedtune_boostgroup_update(st->idx, st->boost);

	return 0;
}

static u64 prefer_high_cap_read(struct cgroup_subsys_state *css,
				struct cftype *cft)
{
	return 0;
}

static int prefer_high_cap_write(struct cgroup_subsys_state *css,
				 struct cftype *cft, u64 prefer_high_cap)
{
	return 0;
}

static u64
prefer_idle_read(struct cgroup_subsys_state *css, struct cftype *cft)
{
	struct schedtune *st = css_st(css);

	return st->prefer_idle;
}

static int
prefer_idle_write(struct cgroup_subsys_state *css, struct cftype *cft,
	    u64 prefer_idle)
{
	struct schedtune *st = css_st(css);

	if (prefer_idle > 1)
		return -EINVAL;

	st->prefer_idle = prefer_idle;

	return 0;
}

static u64
perf_boost_read(struct cgroup_subsys_state *css, struct cftype *cft)
{
	struct schedtune *st = css_st(css);

	return st->perf_boost_idx;
}

static int
perf_boost_write(struct cgroup_subsys_state *css, struct cftype *cft,
	    u64 perf_boost)
{
	struct schedtune *st = css_st(css);

	if (perf_boost >= ARRAY_SIZE(threshold_gains))
		return -EINVAL;

	st->perf_boost_idx = (int)perf_boost;

	return 0;
}

static u64
perf_constrain_read(struct cgroup_subsys_state *css, struct cftype *cft)
{
	struct schedtune *st = css_st(css);

	return st->perf_constrain_idx;
}

static int
perf_constrain_write(struct cgroup_subsys_state *css, struct cftype *cft,
	    u64 perf_constrain)
{
	struct schedtune *st = css_st(css);

	if (perf_constrain >= ARRAY_SIZE(threshold_gains))
		return -EINVAL;

	st->perf_constrain_idx = (int)perf_constrain;

	return 0;
}

static struct cftype files[] = {
	{
		.name = "boost",
		.read_s64 = boost_read,
		.write_s64 = boost_write,
	},
	{
		.name = "prefer_idle",
		.read_u64 = prefer_idle_read,
		.write_u64 = prefer_idle_write,
	},
	{
		.name = "perf_boost",
		.read_u64 = perf_boost_read,
		.write_u64 = perf_boost_write,
	},
	{
		.name = "perf_constrain",
		.read_u64 = perf_constrain_read,
		.write_u64 = perf_constrain_write,
	},
	{
		.name = "prefer_high_cap",
		.read_u64 = prefer_high_cap_read,
		.write_u64 = prefer_high_cap_write,
	},
	{ }	/* terminate */
};

static int
schedtune_boostgroup_init(struct schedtune *st)
{
	struct boost_groups *bg;
	int cpu;

	/* Keep track of allocated boost groups */
	allocated_group[st->idx] = st;

	/* Initialize the per CPU boost groups */
	for_each_possible_cpu(cpu) {
		bg = &per_cpu(cpu_boost_groups, cpu);
		bg->group[st->idx].boost = 0;
		bg->group[st->idx].tasks = 0;
	}

	return 0;
}

static int
schedtune_init(void)
{
	struct boost_groups *bg;
	int cpu;

	/* Initialize the per CPU boost groups */
	for_each_possible_cpu(cpu) {
		bg = &per_cpu(cpu_boost_groups, cpu);
		memset(bg, 0, sizeof(struct boost_groups));
	}

	pr_info("  schedtune configured to support %d boost groups\n",
		BOOSTGROUPS_COUNT);

	schedtune_initialized = true;

	return 0;
}

static struct cgroup_subsys_state *
schedtune_css_alloc(struct cgroup_subsys_state *parent_css)
{
	struct schedtune *st;
	int idx;

	if (!parent_css) {
		schedtune_init();
		return &root_schedtune.css;
	}

	/* Allow only single level hierachies */
	if (parent_css != &root_schedtune.css) {
		pr_err("Nested SchedTune boosting groups not allowed\n");
		return ERR_PTR(-ENOMEM);
	}

	/* Allow only a limited number of boosting groups */
	for (idx = 1; idx < BOOSTGROUPS_COUNT; ++idx)
		if (!allocated_group[idx])
			break;
	if (idx == BOOSTGROUPS_COUNT) {
		pr_err("Trying to create more than %d SchedTune boosting groups\n",
		       BOOSTGROUPS_COUNT);
		return ERR_PTR(-ENOSPC);
	}

	st = kzalloc(sizeof(*st), GFP_KERNEL);
	if (!st)
		goto out;

	/* Initialize per CPUs boost group support */
	st->idx = idx;
	if (schedtune_boostgroup_init(st))
		goto release;

	return &st->css;

release:
	kfree(st);
out:
	return ERR_PTR(-ENOMEM);
}

static void
schedtune_boostgroup_release(struct schedtune *st)
{
	/* Reset this boost group */
	schedtune_boostgroup_update(st->idx, 0);

	/* Keep track of allocated boost groups */
	allocated_group[st->idx] = NULL;
}

static void
schedtune_css_free(struct cgroup_subsys_state *css)
{
	struct schedtune *st = css_st(css);

	schedtune_boostgroup_release(st);
	kfree(st);
}

struct cgroup_subsys schedtune_cgrp_subsys = {
	.css_alloc	= schedtune_css_alloc,
	.css_free	= schedtune_css_free,
	.legacy_cftypes	= files,
	.early_init	= 1,
};

#else /* CONFIG_CGROUP_SCHEDTUNE */

/*
 * System-wide only boosting: evaluate the energy/performance tradeoff
 * with the default (index 0) threshold gains.
 */
int
schedtune_accept_deltas(int nrg_delta, int cap_delta,
			struct task_struct *task)
{
	/* Optimal (O) region */
	if (nrg_delta < 0 && cap_delta > 0)
		return INT_MAX;

	/* Suboptimal (S) region */
	if (nrg_delta > 0 && cap_delta < 0)
		return -INT_MAX;

	return __schedtune_accept_deltas(nrg_delta, cap_delta, 0, 0);
}

#endif /* CONFIG_CGROUP_SCHEDTUNE */

#ifdef CONFIG_SMP
#include <linux/sched_energy.h>

/*
 * Accumulate a cluster's and its CPUs' energy into the system-wide
 * normalization constants (EAS).
 */
static void
schedtune_add_cluster_nrg(struct sched_group *sg,
		struct target_nrg *ste)
{
	struct sched_domain *sd2;
	struct sched_group *sg2;
	const struct cpumask *cluster_cpus;
	char str[32];
	unsigned long min_pwr;
	unsigned long max_pwr;
	int cpu;

	cluster_cpus = sched_group_cpus(sg);
	snprintf(str, 32, "CLUSTER[%*pbl]", cpumask_pr_args(cluster_cpus));

	min_pwr = sg->sge->idle_states[sg->sge->nr_idle_states - 1].power;
	max_pwr = sg->sge->cap_states[sg->sge->nr_cap_states - 1].power;
	pr_info("schedtune: %-17s min_pwr: %5lu max_pwr: %5lu\n",
		str, min_pwr, max_pwr);

	ste->min_power += min_pwr;
	ste->max_power += max_pwr;

	for_each_cpu(cpu, cluster_cpus) {
		for_each_domain(cpu, sd2) {
			sg2 = sd2->groups;

			/* Skip non-CPU groups (i.e. cluster groups) */
			if (sg2->group_weight != 1)
				continue;
			if (!sg2->sge)
				continue;

			min_pwr = sg2->sge->idle_states[sg2->sge->nr_idle_states - 1].power;
			max_pwr = sg2->sge->cap_states[sg2->sge->nr_cap_states - 1].power;

			ste->min_power += min_pwr;
			ste->max_power += max_pwr;

			snprintf(str, 32, "CPU[%d]", cpu);
			pr_info("schedtune: %-17s min_pwr: %5lu max_pwr: %5lu\n",
				str, min_pwr, max_pwr);
			break;
		}
	}
}

/*
 * Initialize the constants required to compute normalized energy. The
 * values depend on the energy model of the target system and its topology,
 * so this runs at late_initcall when the sched domains and the model
 * binding are guaranteed to be in place.
 */
static int __init
schedtune_init_nrg(void)
{
	struct target_nrg *ste = &schedtune_target_nrg;
	unsigned long delta_pwr = 0;
	struct sched_domain *sd;
	struct sched_group *sg;

	ste->max_power = 0;
	ste->min_power = 0;

	if (!sched_energy_present) {
		pr_info("schedtune: no energy model data, EAS energy normalization disabled\n");
		return 0;
	}

	pr_info("schedtune: init normalization constants...\n");

	rcu_read_lock();

	/* With EAS in use we always have a pointer to the highest SD
	 * which provides energy model data. */
	sd = rcu_dereference(per_cpu(sd_ea, cpumask_first(cpu_online_mask)));
	if (!sd) {
		pr_info("schedtune: no energy model data\n");
		goto nodata;
	}

	sg = sd->groups;
	do {
		if (sg->sge)
			schedtune_add_cluster_nrg(sg, ste);
	} while (sg = sg->next, sg != sd->groups);

	rcu_read_unlock();

	pr_info("schedtune: %-17s min_pwr: %5lu max_pwr: %5lu\n",
		"SYSTEM", ste->min_power, ste->max_power);

	delta_pwr = ste->max_power - ste->min_power;
	if (delta_pwr)
		ste->rdiv = reciprocal_value(delta_pwr);
	pr_info("schedtune: using normalization constants mul: %u sh1: %u sh2: %u\n",
		ste->rdiv.m, ste->rdiv.sh1, ste->rdiv.sh2);

	return 0;

nodata:
	pr_warn("schedtune: energy normalization disabled\n");
	rcu_read_unlock();
	return -EINVAL;
}
late_initcall(schedtune_init_nrg);
#endif /* CONFIG_SMP */

#ifdef CONFIG_FREQVAR_SCHEDTUNE
static struct freqvar_boost_state freqvar_boost_state[CONFIG_NR_CPUS];

int schedtune_freqvar_boost(int cpu)
{
	if (!freqvar_boost_state[cpu].enabled)
		return 0;

	return freqvar_boost_state[cpu].ratio;
}

/* update freqvar_boost ratio matched current frequency */
static void schedtune_freqvar_update_boost_ratio(int cpu, int new_freq)
{
	struct freqvar_boost_table *pos = freqvar_boost_state[cpu].table;

	for (; pos->frequency != CPUFREQ_TABLE_END; pos++)
		if (new_freq == pos->frequency) {
			freqvar_boost_state[cpu].ratio = pos->boost;
			break;
		}

	return;
}

/* when cpu frequency scaled, this callback called on each cpu */
static int schedtune_freqvar_cpufreq_callback(struct notifier_block *nb,
					unsigned long val, void *data)
{
	struct cpufreq_freqs *freq = data;

	if (freq->flags & CPUFREQ_CONST_LOOPS)
		return NOTIFY_OK;

	if (val != CPUFREQ_POSTCHANGE)
		return NOTIFY_OK;

	if (freqvar_boost_state[freq->cpu].enabled)
		schedtune_freqvar_update_boost_ratio(freq->cpu, freq->new);

	return 0;
}

static int schedtune_freqvar_find_node(struct device_node **dn,
					struct cpufreq_policy *policy)
{
	const char *buf;
	cpumask_t shared_mask;
	int ret;

	while ((*dn = of_find_node_by_type(*dn, "schedtune-freqvar"))) {
		/*
		 * shared-cpus includes cpus scaling at the sametime.
		 * it is called "sibling cpus" in the CPUFreq and
		 * masked on the realated_cpus of the policy
		 */
		ret = of_property_read_string(*dn, "shared-cpus", &buf);
		if (ret)
			return ret;
		cpumask_clear(&shared_mask);
		cpulist_parse(buf, &shared_mask);
		cpumask_and(&shared_mask, &shared_mask, cpu_possible_mask);
		if (cpumask_weight(&shared_mask) == 0)
			return -ENODEV;
		if (cpumask_equal(&shared_mask, policy->related_cpus)) {
			return 0;
		}
	}

	return -EINVAL;
}

/*
 * update freqvar_boost table from src table .
 * src table is array of frequency and ratio and has to use ascending order.
 * src table examples: 12 546000 10 650000 8 858000 4 1274000 0
 * dst table examples:
 *      Freq   Ratio
 *    1274000	 0
 *    1170000	 4
 *    1066000	 4
 *     962000	 4
 *     858000	 4
 *     754000	 8
 *     650000	 8
 *     546000	10
 *     442000	12
 * ratio unit is 1%.
 */
int schedtune_freqvar_update_table(unsigned int *src, int src_size,
					struct freqvar_boost_table *dst)
{
	struct freqvar_boost_table *pos, *last_pos = dst;
	unsigned int ratio = 0, freq = 0;
	int i;

	for (i = src_size - 1; i >= 0; i--) {
		ratio = src[i] * SCHEDTUNE_LOAD_BOOST_UTIT;
		freq  = (i - 1 < 0) ? 0 : src[i - 1];

		for (pos = last_pos; pos->frequency != CPUFREQ_TABLE_END; pos++)
			if (pos->frequency >= freq) {
				pos->boost = ratio;
			} else {
				last_pos = pos;
				break;
			}
	}

	return 0;
}

static int schedtune_freqvar_parse_dt(struct device_node *dn,
				struct freqvar_boost_data *data)
{
	int size;
	unsigned int *table;

	/* get the boost table from dts files */
	size = of_property_count_u32_elems(dn, "table");
	table = kzalloc(sizeof(unsigned int) * size, GFP_KERNEL);
	of_property_read_u32_array(dn, "table", (unsigned int *)table, size);
	if (!table)
		return -ENOMEM;

	/* update freqvar_boost table from dt */
	schedtune_freqvar_update_table(table, size, data->table);

	kfree(table);

	return 0;
}

static int schedtune_freqvar_init_table(struct cpufreq_policy *policy,
					struct freqvar_boost_data *data)
{
	struct cpufreq_frequency_table *cpufreq_table, *pos;
	struct freqvar_boost_table *boost_table;
	int size = 0, index;

	cpufreq_table = cpufreq_frequency_get_table(policy->cpu);
	if (unlikely(!cpufreq_table)) {
		pr_debug("%s: Unable to find frequency table\n", __func__);
		return -ENOENT;
	}

	/*
	 * HACK
	 * get max index of cpufreq table
	 * need to more smart and simple way
	 */
	cpufreq_for_each_valid_entry(pos, cpufreq_table)
		size++;

	/*
	 * freqvar_boost table is allocated with POLICY INIT
	 * It is deallocated with POLICY EXIT
	 */
	boost_table = kzalloc(sizeof(struct freqvar_boost_table)
					* (size + 1), GFP_KERNEL);
	if (boost_table == NULL) {
		pr_err("%s: failed to allocate memory\n", __func__);
		return -ENOMEM;
	}

	/* copy cpu frequency table */
	index = 0;
	cpufreq_for_each_valid_entry(pos, cpufreq_table) {
		boost_table[index].frequency = pos->frequency;
		boost_table[index].boost = 0;	/* default is 0, it is no effect */
		index++;
	}
	boost_table[index].frequency = CPUFREQ_TABLE_END;
	boost_table[index].boost = 0;	/* default is 0, it is no effect */

	/* freqvar_boost data is initialized */
	data->table = boost_table;

	return 0;
}

void schedtune_freqvar_boost_enable(int cpu, int index,
				struct freqvar_boost_data *data, bool enabled)
{
	if (enabled) {
		freqvar_boost_state[cpu].ratio = data->table[index].boost;
		freqvar_boost_state[cpu].table = data->table;
		freqvar_boost_state[cpu].enabled = true;
	} else {
		freqvar_boost_state[cpu].enabled = false;
		freqvar_boost_state[cpu].ratio = 0;
	}
	return;
}

int schedtune_freqvar_boost_init(struct cpufreq_policy *policy,
					struct freqvar_boost_data *data)
{
	struct device_node *dn = NULL;
	int cur_index = cpufreq_frequency_table_get_index(policy, policy->cur);
	int cpu;

	if (!freqvar_boost_state[policy->cpu].table) {
		/* find device node */
		if (schedtune_freqvar_find_node(&dn, policy))
			return 0;
		/* copy cpu frequency table */
		if (schedtune_freqvar_init_table(policy, data))
			return 0;
		/* update boost value from dt */
		if (schedtune_freqvar_parse_dt(dn, data))
			goto free;
	} else {
		data->table = freqvar_boost_state[policy->cpu].table;
	}
	/* enable freqvar boost */
	for_each_cpu(cpu, policy->related_cpus)
		schedtune_freqvar_boost_enable(cpu, cur_index, data, true);

	return 0;

free:
	pr_err("SchedTune: faile to initialize\n");
	kfree(data->table);

	return 0;
}

int schedtune_freqvar_boost_exit(struct cpufreq_policy *policy,
					struct freqvar_boost_data *data)
{
	int cpu;

	for_each_cpu(cpu, policy->related_cpus)
		schedtune_freqvar_boost_enable(cpu, 0, data, false);

	return 0;
}

static struct notifier_block schedtune_freqvar_cpufreq_notifier = {
	.notifier_call  = schedtune_freqvar_cpufreq_callback,
};

static int __init schedtune_freqvar_register_cpufreq_noti(void)
{
	return cpufreq_register_notifier(&schedtune_freqvar_cpufreq_notifier,
				CPUFREQ_TRANSITION_NOTIFIER);
}
core_initcall(schedtune_freqvar_register_cpufreq_noti);
#endif	/* CONFIG_FREQVAR_SCHEDTUNE */

int
sysctl_sched_cfs_boost_handler(struct ctl_table *table, int write,
			       void __user *buffer, size_t *lenp,
			       loff_t *ppos)
{
	int ret = proc_dointvec_minmax(table, write, buffer, lenp, ppos);

	if (ret || !write)
		return ret;

	return 0;
}
