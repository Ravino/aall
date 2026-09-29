/* util/rt_status.c -- see rt_status.h. */
#define _GNU_SOURCE
#include "rt_status.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/resource.h>
#include <unistd.h>

pid_t m4_rt_status_find_helper_pid(const char *qemu_path)
{
	char resolved_target[PATH_MAX];
	DIR *procdir;
	struct dirent *de;
	pid_t found = -1;

	if (qemu_path == NULL || qemu_path[0] == '\0')
		return -1;
	if (realpath(qemu_path, resolved_target) == NULL)
		return -1; /* configured path does not even exist right now */

	procdir = opendir("/proc");
	if (procdir == NULL)
		return -1;

	while (found < 0 && (de = readdir(procdir)) != NULL) {
		char exe_link[32];
		char exe_target[PATH_MAX];
		ssize_t n;
		pid_t pid;
		const char *p;

		/* /proc/<pid> only -- de->d_name is all-digits for a pid entry,
		 * anything else (self, thread-self, version, ...) is skipped. */
		for (p = de->d_name; *p != '\0'; p++)
			if (!isdigit((unsigned char)*p))
				break;
		if (*p != '\0' || de->d_name[0] == '\0')
			continue;
		pid = (pid_t)atol(de->d_name);
		if (pid <= 0)
			continue;

		snprintf(exe_link, sizeof(exe_link), "/proc/%ld/exe", (long)pid);
		n = readlink(exe_link, exe_target, sizeof(exe_target) - 1);
		if (n <= 0)
			continue; /* gone, kernel thread, no permission, ... -- not our helper */
		exe_target[n] = '\0';

		if (strcmp(exe_target, resolved_target) == 0)
			found = pid;
	}
	closedir(procdir);
	return found;
}

void m4_rt_status_query_pid(pid_t pid, struct m4_rt_proc_status *out)
{
	struct sched_param sp = {0};
	cpu_set_t set;

	memset(out, 0, sizeof(*out));
	if (pid <= 0)
		return;

	out->have_pid = true;
	out->pid = pid;

	out->sched_policy = sched_getscheduler(pid);
	if (sched_getparam(pid, &sp) == 0)
		out->rt_priority = sp.sched_priority;

	/* getpriority()'s own -1-is-a-valid-return quirk: only errno tells
	 * failure apart from a genuine nice value of -1. */
	errno = 0;
	out->nice = getpriority(PRIO_PROCESS, pid);
	if (!(out->nice == -1 && errno != 0))
		out->have_nice = true;

	if (sched_getaffinity(pid, sizeof(set), &set) == 0) {
		out->have_affinity = true;
		out->affinity_count = CPU_COUNT(&set);
	}
}

const char *m4_rt_status_policy_name(int policy)
{
	switch (policy) {
	case SCHED_OTHER: return "other";
	case SCHED_FIFO: return "fifo";
	case SCHED_RR: return "rr";
#ifdef SCHED_BATCH
	case SCHED_BATCH: return "batch";
#endif
#ifdef SCHED_IDLE
	case SCHED_IDLE: return "idle";
#endif
#ifdef SCHED_DEADLINE
	case SCHED_DEADLINE: return "deadline";
#endif
	default: return "unknown";
	}
}

bool m4_rt_status_matches(int configured_priority, const char *actual_policy_name, int actual_prio)
{
	if (actual_policy_name == NULL || actual_policy_name[0] == '\0')
		return false;
	if (configured_priority <= 0)
		return strcasecmp(actual_policy_name, "other") == 0;
	return strcasecmp(actual_policy_name, "fifo") == 0 && actual_prio == configured_priority;
}
