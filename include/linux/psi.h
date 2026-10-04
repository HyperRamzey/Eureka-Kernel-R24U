#ifndef _LINUX_PSI_H
#define _LINUX_PSI_H

#include <linux/jump_label.h>
#include <linux/psi_types.h>
#include <linux/sched.h>
#include <linux/poll.h>

struct seq_file;

#ifdef CONFIG_PSI

extern struct static_key_false psi_disabled;

void psi_init(void);

void psi_task_change(struct task_struct *task, int clear, int set);

void psi_memstall_tick(struct task_struct *task, int cpu);
void psi_memstall_enter(unsigned long *flags);
void psi_memstall_leave(unsigned long *flags);

int psi_show(struct seq_file *s, struct psi_group *group, enum psi_res res);

/*
 * Trigger interface. Upstream guards these with CONFIG_CGROUPS because
 * triggers are only reachable through a cgroup file. This port is
 * system-level only - the triggers live on the three files under
 * /proc/pressure - so they are
 * unconditional. libpsi opens each of those paths O_WRONLY and writes
 * "some <threshold_us> <window_us>", then waits for EPOLLPRI via poll(2).
 */
struct psi_trigger *psi_trigger_create(struct psi_group *group,
			char *buf, size_t nbytes, enum psi_res res);
void psi_trigger_replace(void **trigger_ptr, struct psi_trigger *t);

unsigned int psi_trigger_poll(void **trigger_ptr, struct file *file,
			      poll_table *wait);

#else /* CONFIG_PSI */

static inline void psi_init(void) {}

static inline void psi_memstall_enter(unsigned long *flags) {}
static inline void psi_memstall_leave(unsigned long *flags) {}

#endif /* CONFIG_PSI */

#endif /* _LINUX_PSI_H */