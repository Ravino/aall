/* momentum4: "RT for the bridge's IO thread" -- tests for apply_io_thread_rt() in
 * m4-adaptive-bridge.c (cfg.io_thread_rt_priority / m4_ah_bridge_stats'
 * bridge_rt_policy/bridge_rt_prio).
 *
 * Uses test/evil_helper.c (SPAWN mode) as a fast, deterministic stand-in for
 * qemu-hexagon, same as resilience_test.c -- no real qemu-hexagon involved,
 * so this is safe to run without any special scheduling precautions and
 * cannot interfere with a live helper instance.
 *
 * Scenarios:
 *   1. io_thread_rt_priority == 0 (default/disabled): bridge_rt_policy must
 *      be SCHED_OTHER and bridge_rt_prio 0, unconditionally.
 *   2. io_thread_rt_priority == 1 (a value expected to succeed under this
 *      test's own RLIMIT_RTPRIO, see the shell wrapper/CI note below):
 *      bridge_rt_policy/prio must reflect SCHED_FIFO/1 -- proves the happy
 *      path actually raises the thread's priority, not just "does not
 *      crash".
 *   3. io_thread_rt_priority intentionally set ABOVE this process's own
 *      RLIMIT_RTPRIO soft limit (queried via getrlimit(RLIMIT_RTPRIO) at
 *      runtime, +1): pthread_setschedparam() must fail with EPERM,
 *      m4_ah_bridge_start() must still SUCCEED (never fail closed over
 *      this), and bridge_rt_policy/prio must report the honest fallback
 *      (SCHED_OTHER/0), matching the "silently stay SCHED_OTHER on EPERM"
 *      requirement.
 *   4. Regardless of scenario, submit_pcm()/take_frame() keep working
 *      (a few real encode/decode round trips against evil_helper) -- RT
 *      priority changes must not disturb the existing bridge contract.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sched.h>
#include <sys/resource.h>
#include <unistd.h>

#include "m4-adaptive-bridge.h"

static int g_failures = 0;
#define CHECK(cond, ...) do { \
	if (!(cond)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); g_failures++; } \
	else { printf("PASS: "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static void fill_dummy_pcm(uint8_t *pcm)
{
	memset(pcm, 0, M4_AH_PCM_BLOCK_BYTES);
}

/* A handful of real submit/take round trips against evil_helper's "normal"
 * mode, to confirm the bridge still functions normally regardless of what
 * apply_io_thread_rt() just did to the IO thread's scheduling. Does not
 * assert on frame content (resilience_test.c/unit_test.c already do byte-
 * level checks against evil_helper/the real helper) -- only that frames
 * keep flowing and no call ever blocks. */
static void exercise_rounds(struct m4_ah_bridge *b, const char *label)
{
	uint8_t pcm[M4_AH_PCM_BLOCK_BYTES];
	uint8_t frame[M4_AH_MAX_FRAME_BYTES];
	fill_dummy_pcm(pcm);
	unsigned got_frame = 0;
	for (int i = 0; i < 10; i++) {
		m4_ah_bridge_submit_pcm(b, pcm, sizeof(pcm));
		usleep(5000);
		if (m4_ah_bridge_take_frame(b, frame, sizeof(frame)) > 0)
			got_frame++;
	}
	CHECK(got_frame > 0, "%s: bridge still produces frames after RT setup (%u/10)",
			label, got_frame);
}

static void scenario_disabled(const char *evil, const char *dummy)
{
	printf("\n-- scenario: io_thread_rt_priority = 0 (disabled, default) --\n");
	struct m4_ah_bridge_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.qemu_path = evil;
	cfg.loader_path = dummy;
	cfg.capi_path = dummy;
	setenv("EVIL_HELPER_MODE", "normal", 1);
	cfg.strip_ota_header = true;
	cfg.helper_timeout_ms = 200;
	cfg.io_thread_rt_priority = 0;

	struct m4_ah_bridge *b = m4_ah_bridge_start(NULL, &cfg);
	CHECK(b != NULL, "bridge starts with io_thread_rt_priority = 0");
	if (!b) return;

	struct m4_ah_bridge_stats st;
	m4_ah_bridge_get_stats(b, &st);
	CHECK(st.bridge_rt_policy == SCHED_OTHER,
			"bridge_rt_policy == SCHED_OTHER when disabled (got %d)", st.bridge_rt_policy);
	CHECK(st.bridge_rt_prio == 0,
			"bridge_rt_prio == 0 when disabled (got %d)", st.bridge_rt_prio);

	exercise_rounds(b, "disabled");
	m4_ah_bridge_stop(b);
}

static void scenario_succeeds(const char *evil, const char *dummy, int rtprio)
{
	printf("\n-- scenario: io_thread_rt_priority = %d (expected to succeed under "
			"this process's own RLIMIT_RTPRIO) --\n", rtprio);
	struct m4_ah_bridge_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.qemu_path = evil;
	cfg.loader_path = dummy;
	cfg.capi_path = dummy;
	setenv("EVIL_HELPER_MODE", "normal", 1);
	cfg.strip_ota_header = true;
	cfg.helper_timeout_ms = 200;
	cfg.io_thread_rt_priority = rtprio;

	struct m4_ah_bridge *b = m4_ah_bridge_start(NULL, &cfg);
	CHECK(b != NULL, "bridge starts with io_thread_rt_priority = %d", rtprio);
	if (!b) return;

	struct m4_ah_bridge_stats st;
	m4_ah_bridge_get_stats(b, &st);
	CHECK(st.bridge_rt_policy == SCHED_FIFO,
			"bridge_rt_policy == SCHED_FIFO (got %d) -- RLIMIT_RTPRIO must allow "
			"prio %d in this environment for this check to be meaningful; "
			"see scenario_over_limit() for the EPERM/fallback path",
			st.bridge_rt_policy, rtprio);
	CHECK(st.bridge_rt_prio == rtprio,
			"bridge_rt_prio == %d (got %d)", rtprio, st.bridge_rt_prio);

	exercise_rounds(b, "succeeds");
	m4_ah_bridge_stop(b);
}

static void scenario_over_limit(const char *evil, const char *dummy)
{
	struct rlimit rl;
	int over;

	if (getrlimit(RLIMIT_RTPRIO, &rl) != 0) {
		printf("SKIP: getrlimit(RLIMIT_RTPRIO) failed, cannot compute an "
				"out-of-range priority for this environment\n");
		return;
	}
	/* sched_get_priority_max(SCHED_FIFO) is 99 on Linux; if the soft limit
	 * is already at (or above) that, there is no legal "too high" value to
	 * test with on this machine -- skip rather than report a false FAIL. */
	if (rl.rlim_cur >= 99) {
		printf("SKIP: RLIMIT_RTPRIO soft limit is %lu (>= 99), no out-of-range "
				"SCHED_FIFO priority exists to provoke EPERM on this machine\n",
				(unsigned long)rl.rlim_cur);
		return;
	}
	over = (int)rl.rlim_cur + 50;
	if (over > 99)
		over = 99;

	printf("\n-- scenario: io_thread_rt_priority = %d, ABOVE this process's "
			"RLIMIT_RTPRIO soft limit (%lu) -- must fall back to SCHED_OTHER, "
			"never fail m4_ah_bridge_start() --\n", over, (unsigned long)rl.rlim_cur);

	struct m4_ah_bridge_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.qemu_path = evil;
	cfg.loader_path = dummy;
	cfg.capi_path = dummy;
	setenv("EVIL_HELPER_MODE", "normal", 1);
	cfg.strip_ota_header = true;
	cfg.helper_timeout_ms = 200;
	cfg.io_thread_rt_priority = over;

	struct m4_ah_bridge *b = m4_ah_bridge_start(NULL, &cfg);
	CHECK(b != NULL, "m4_ah_bridge_start() still SUCCEEDS despite an "
			"unattainable RT priority (%d) -- EPERM must be silently tolerated",
			over);
	if (!b) return;

	struct m4_ah_bridge_stats st;
	m4_ah_bridge_get_stats(b, &st);
	CHECK(st.bridge_rt_policy == SCHED_OTHER,
			"bridge_rt_policy honestly reports SCHED_OTHER after the EPERM "
			"fallback (got %d)", st.bridge_rt_policy);
	CHECK(st.bridge_rt_prio == 0,
			"bridge_rt_prio honestly reports 0 after the EPERM fallback (got %d)",
			st.bridge_rt_prio);

	exercise_rounds(b, "over_limit");
	m4_ah_bridge_stop(b);
}

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: %s <evil_helper_path> <dummy_loader_path>\n", argv[0]);
		return 2;
	}
	const char *evil = argv[1];
	const char *dummy = argv[2];

	scenario_disabled(evil, dummy);
	scenario_succeeds(evil, dummy, 1);
	scenario_over_limit(evil, dummy);

	printf("\n%s: %d failure(s)\n", g_failures == 0 ? "ALL PASS" : "SOME FAILED", g_failures);
	return g_failures == 0 ? 0 : 1;
}
