/* room - cgroup v2 setup and per-box cgroup operations. */
#ifndef ROOM_CG_H
#define ROOM_CG_H

#include <stdbool.h>

/*
 * Finds the cgroup v2 hierarchy and makes sure room has a delegated
 * subtree with the memory and pids controllers. Without ROOM_CG_ROOT that
 * can remount the cgroup filesystem read-write and move processes into
 * room/main. Safe to call on every invocation. Must run before any other
 * cg_ function. Dies with a message that says what to change when the
 * environment can't support it.
 */
void cg_init(void);

/*
 * A box has two cgroups. box-N exists from --init to --cleanup, holds no
 * processes and is the claim on the box number: it is made with a single
 * mkdir(), so two room processes can never get the same number, whatever
 * their box roots. box-N/run is made new for every run and is where the
 * program lives, and where the limits and the measurements are.
 *
 * The functions below act on the box in box_id, which may change between
 * calls. Apart from cg_box_exists(), they die on any failure they don't
 * mention.
 */

/*
 * Claims the box number by creating box-N. Returns false if it already
 * existed, which means the number is taken.
 */
bool cg_box_claim(void);

/*
 * -b auto: returns the lowest number at or above from that has no box-N
 * in room/ (or ROOM_CG_ROOT), INT_MAX if there is none. It reads the
 * directory once, however many boxes exist, and ignores every name that
 * isn't box-N. Doesn't depend on box_id. The result is only a hint:
 * another room process may claim the number first, so the caller still
 * locks and calls cg_box_claim().
 */
int cg_box_first_free(int from);

/*
 * --init with a given number: makes sure box-N exists and that no run
 * cgroup is left in it, killing what is still running there. It never
 * removes box-N, so the claim is never dropped.
 */
void cg_box_create(void);

/* True if box-N exists, which is the case between --init and --cleanup. */
bool cg_box_exists(void);

/* Kills whatever is left in the box and removes both cgroups. Fine if they are already gone. */
void cg_box_remove(void);

/*
 * --run, before the program starts: gives the box a fresh run cgroup and
 * sets the limits for this run. A fresh cgroup means cg_box_cpu_ms() and
 * cg_box_stats() cover this run only. mem_kb 0 means no memory limit.
 * max_procs 0 means no limit.
 *
 * cpus is a cpuset list such as "0-3,6", or NULL to leave the CPUs alone.
 * Dies if the cpuset controller isn't available or if some of the CPUs
 * aren't available to the container.
 *
 * cg_box_fd(), cg_box_cpu_ms() and cg_box_stats() need the run cgroup
 * this makes.
 */
void cg_box_prepare(long long mem_kb, int max_procs, const char *cpus);

/*
 * Returns a new O_CLOEXEC fd of the run cgroup directory, for clone3() with
 * CLONE_INTO_CGROUP. The caller closes it.
 */
int cg_box_fd(void);

/*
 * Kills every process in the run cgroup and waits until it is empty.
 * Does nothing if the cgroup doesn't exist. Dies if it isn't empty after
 * 5 seconds.
 */
void cg_box_kill(void);

/* CPU time used in the run cgroup since cg_box_prepare(), in milliseconds. */
long long cg_box_cpu_ms(void);

struct cg_stats {
    long long mem_peak_kb; /* peak memory of this run, -1 without memory.peak (before 5.19) */
    bool oom_killed;       /* the OOM killer fired in the box during this run */
};
void cg_box_stats(struct cg_stats *st);

#endif
