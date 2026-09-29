/* util/rt_status.h -- RT status: what SCHED_* priority
 * the aptX Adaptive bridge's IO thread and the qemu-hexagon HELPER PROCESS
 * actually got, compared against `[adaptive] io-rt-priority`/`[helper]
 * rt-priority` -- `momentum4 doctor`/`momentum4 status` both show this
 * (cli/doctor.c, cli/status.c), so the operator can tell "configured" from
 * "actually running with" without reading journalctl for a
 * sched_setscheduler()/pthread_setschedparam() WARN line (EPERM -- no
 * matching LimitRTPRIO/rtkit -- is silently tolerated by both the bridge
 * and `momentum4 helper-exec`, on purpose, so nothing else ever surfaces
 * this by itself).
 *
 * Pure, host-independent comparison logic (m4_rt_status_matches() below) is
 * separated from live syscalls/proc reads (m4_rt_status_find_helper_pid()/
 * m4_rt_status_query_pid()) precisely so the former is unit-testable
 * without a real qemu-hexagon process running -- this project's existing
 * convention (e.g. core/profile_rules.c's own pure/impure split).
 *
 * The bridge IO thread's OWN actual policy/priority is not read here at
 * all: it already self-reports via `bridge_rt_policy`/`bridge_rt_prio` in
 * stats.json (m4-adaptive-bridge.c's apply_io_thread_rt(), read by
 * metrics/stats.c's struct m4_stats) -- both cli/doctor.c and cli/status.c
 * read that directly and pass it into m4_rt_status_matches() below,
 * avoiding a second, redundant way to learn the same fact (this process,
 * momentum4d/`momentum4`, is not even the same process as the bridge, which
 * lives inside WirePlumber -- there is nothing else this file COULD read
 * for that half). */
#ifndef MOMENTUM4_UTIL_RT_STATUS_H
#define MOMENTUM4_UTIL_RT_STATUS_H

#include <stdbool.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Scans /proc for a running process whose /proc/<pid>/exe resolves to
 * `qemu_path` (both sides realpath()'d, so a symlinked `[helper] qemu`
 * still matches). Read-only (opendir()/readlink() only -- no ptrace, no
 * /proc/<pid>/mem, no systemctl/sd-bus, consistent with cli/doctor.c's own
 * "only reads/observes" policy) -- works for BOTH `[adaptive] helper-mode
 * = socket` (systemd socket-activated, one process per connection) and
 * `= spawn` (fork+exec'd directly by the bridge inside WirePlumber),
 * unlike a systemd GetUnit/MainPID query, which would only ever see the
 * socket-mode case. Returns the PID (>0) of the first match, or -1 if none
 * is currently running (not an error -- the helper is simply idle/not
 * connected right now, or `qemu_path` is NULL/""/does not resolve).
 * O(number of processes on the machine) -- fine for a `doctor`/`status`
 * command run interactively by a human, not a hot path. */
pid_t m4_rt_status_find_helper_pid(const char *qemu_path);

struct m4_rt_proc_status {
	bool have_pid;
	pid_t pid;
	int sched_policy;   /* raw SCHED_* (<sched.h>), -1 if sched_getscheduler() failed */
	int rt_priority;    /* sched_param.sched_priority, only meaningful for FIFO/RR */
	bool have_nice;
	int nice;           /* getpriority(PRIO_PROCESS, pid) */
	bool have_affinity;
	int affinity_count; /* CPU_COUNT() of the process's current affinity mask,
			      * i.e. how many CPUs it is currently allowed to run on
			      * (all of them, if `[helper] cpu-list` was never set) */
};

/* sched_getscheduler()/sched_getparam()/getpriority()/sched_getaffinity()
 * against `pid` -- all read-only, all succeed for any process the CALLER's
 * own uid owns (no CAP_SYS_NICE needed to READ scheduling info, only to
 * raise it -- these never fail with EPERM against our own helper process).
 * `pid <= 0` -> everything left false/zeroed (`out->have_pid = false`),
 * same "no data, not an error" contract as m4_rt_status_find_helper_pid(). */
void m4_rt_status_query_pid(pid_t pid, struct m4_rt_proc_status *out);

/* "fifo"/"other"/"rr"/"batch"/"idle"/"deadline"/"unknown" for a raw
 * <sched.h> SCHED_* value -- the SAME name table m4-adaptive-bridge.c's own
 * sched_policy_name() uses for stats.json's `bridge_rt_policy`, so
 * `momentum4 doctor`/`status` can compare like-for-like strings without a
 * second, independently-maintained (and possibly diverging) name table. */
const char *m4_rt_status_policy_name(int policy);

/* Pure comparison (no I/O): does the ACTUALLY observed policy/priority
 * match what `[adaptive] io-rt-priority`/`[helper] rt-priority` asked for?
 * `configured_priority`: the registry value, 0 meaning "SCHED_OTHER
 * expected" (both registry keys document this convention verbatim: "0 =
 * SCHED_OTHER"). `actual_policy_name`: m4_rt_status_policy_name()'s own
 * output, or stats.json's `bridge_rt_policy` string verbatim (both use the
 * identical name table, see this header's own top comment) -- NULL/""
 * treated as "unknown", never a match. `actual_prio`: the priority number
 * actually in effect (meaningless/ignored unless actual_policy_name is
 * "fifo" or "rr"). Returns true iff they agree: `configured_priority <= 0`
 * requires "other" (a positive `actual_prio` under "other" cannot happen in
 * practice -- SCHED_OTHER's own priority is always 0 -- but is not itself
 * compared, only the policy name is, for that direction); `configured_priority
 * > 0` requires policy "fifo" AND `actual_prio == configured_priority`
 * exactly (`[helper] rt-priority`'s own doc comment: above the unit's
 * LimitRTPRIO, it is clamped with a warning -- a SILENTLY CLAMPED value would
 * show up here as a real, intentional mismatch, which is exactly the point
 * of this whole feature). */
bool m4_rt_status_matches(int configured_priority, const char *actual_policy_name, int actual_prio);

#ifdef __cplusplus
}
#endif

#endif /* MOMENTUM4_UTIL_RT_STATUS_H */
