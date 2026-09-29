/* tests/test_rt_status.c -- util/rt_status.c ("RT status" task).
 * Pure comparison logic is exercised directly (m4_rt_status_matches()/
 * _policy_name()); the /proc-reading halves are exercised live against
 * THIS test process's own PID (always readable, no fake qemu-hexagon
 * binary needed -- see this file's own meson.build comment).
 */
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "util/rt_status.h"

static int failures = 0;
static int checks = 0;

#define CHECK(cond, msg)                                                          \
	do {                                                                        \
		checks++;                                                           \
		if (!(cond)) {                                                      \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, (msg)); \
			failures++;                                                 \
		}                                                                   \
	} while (0)

static void test_policy_name(void)
{
	CHECK(strcmp(m4_rt_status_policy_name(SCHED_OTHER), "other") == 0, "SCHED_OTHER -> \"other\"");
	CHECK(strcmp(m4_rt_status_policy_name(SCHED_FIFO), "fifo") == 0, "SCHED_FIFO -> \"fifo\"");
	CHECK(strcmp(m4_rt_status_policy_name(SCHED_RR), "rr") == 0, "SCHED_RR -> \"rr\"");
	CHECK(strcmp(m4_rt_status_policy_name(9999), "unknown") == 0, "unrecognised -> \"unknown\"");
}

/* [adaptive] io-rt-priority / [helper] rt-priority's own documented "0 =
 * SCHED_OTHER" convention -- see rt_status.h's own doc comment on
 * m4_rt_status_matches() for the exact contract this mirrors. */
static void test_matches(void)
{
	CHECK(m4_rt_status_matches(0, "other", 0), "configured 0 + actual other -> match");
	CHECK(m4_rt_status_matches(-5, "other", 0), "configured negative also means \"expect other\"");
	CHECK(!m4_rt_status_matches(0, "fifo", 1), "configured 0 but actually fifo -> mismatch");
	CHECK(!m4_rt_status_matches(0, "fifo", 0), "configured 0 but actually fifo (even prio 0) -> mismatch");

	CHECK(m4_rt_status_matches(70, "fifo", 70), "configured 70 + actual fifo/70 -> match");
	CHECK(!m4_rt_status_matches(70, "fifo", 50), "configured 70 but actual fifo/50 (clamped) -> mismatch");
	CHECK(!m4_rt_status_matches(70, "other", 0), "configured 70 but actually other -> mismatch");
	CHECK(!m4_rt_status_matches(70, "rr", 70), "configured 70 but actually rr (not fifo) -> mismatch");

	CHECK(m4_rt_status_matches(10, "FIFO", 10), "policy name comparison is case-insensitive");
	CHECK(!m4_rt_status_matches(10, NULL, 10), "NULL actual policy name -> never a match");
	CHECK(!m4_rt_status_matches(10, "", 10), "empty actual policy name -> never a match");
}

/* No configuration knob to compare against is provided/needed here (this
 * function has no config dependency at all) -- just the /proc-reading half
 * of util/rt_status.c, exercised against this test binary's own PID. */
static void test_find_and_query_self(void)
{
	char self_exe[4096];
	ssize_t n;
	pid_t found;
	struct m4_rt_proc_status ps;

	n = readlink("/proc/self/exe", self_exe, sizeof(self_exe) - 1);
	CHECK(n > 0, "readlink(/proc/self/exe)");
	if (n <= 0)
		return;
	self_exe[n] = '\0';

	found = m4_rt_status_find_helper_pid(self_exe);
	CHECK(found == getpid(), "find_helper_pid(own exe) finds THIS process");

	CHECK(m4_rt_status_find_helper_pid(NULL) == -1, "NULL path -> -1, not a crash");
	CHECK(m4_rt_status_find_helper_pid("") == -1, "empty path -> -1");
	CHECK(m4_rt_status_find_helper_pid("/no/such/binary-at-all") == -1,
	      "nonexistent path -> -1 (realpath() itself fails)");

	m4_rt_status_query_pid(getpid(), &ps);
	CHECK(ps.have_pid, "query_pid(getpid()): have_pid true");
	CHECK(ps.pid == getpid(), "query_pid: pid echoed back");
	CHECK(ps.sched_policy >= 0, "sched_getscheduler() succeeded for our own pid (never EPERM on self)");
	CHECK(ps.have_nice, "getpriority() succeeded for our own pid");
	/* affinity: not asserted true/false (sched_getaffinity() can
	 * legitimately be unavailable in some sandboxes) -- only that it never
	 * crashes and, if available, reports at least one CPU. */
	if (ps.have_affinity)
		CHECK(ps.affinity_count >= 1, "at least one CPU in our own affinity mask");

	{
		struct m4_rt_proc_status zero;

		m4_rt_status_query_pid(0, &zero);
		CHECK(!zero.have_pid, "pid<=0 -> have_pid false, no crash");
		m4_rt_status_query_pid(-1, &zero);
		CHECK(!zero.have_pid, "negative pid -> have_pid false, no crash");
	}
}

int main(void)
{
	test_policy_name();
	test_matches();
	test_find_and_query_self();

	if (failures > 0) {
		fprintf(stderr, "%d/%d checks failed\n", failures, checks);
		return 1;
	}
	printf("test_rt_status: %d/%d checks passed\n", checks, checks);
	return 0;
}
