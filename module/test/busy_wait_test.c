/* momentum4: "RT for the bridge's IO thread" -- verify the bridge's IO
 * thread cannot "eat" CPU: it must block on poll()/eventfd while idle, never
 * busy-wait, EVEN when raised to SCHED_FIFO (cfg.io_thread_rt_priority > 0)
 * and EVEN under host CPU contention. This matters more for a SCHED_FIFO
 * thread than a SCHED_OTHER one: a real busy-wait bug at SCHED_FIFO priority
 * would starve whatever else shares that CPU (including, in the worst case,
 * kernel housekeeping on that core) rather than just being unfair -- so this
 * check specifically exercises the RT-enabled path, not the default.
 *
 * Method: getrusage(RUSAGE_SELF) sums ALL threads of the calling process
 * (Linux, since 2.6.26) and excludes children -- exactly what is needed here
 * (the IO thread is a sibling thread of main(), evil_helper is a separate
 * child process with its own, irrelevant CPU time). Start the bridge, let it
 * go fully idle (no submit_pcm() calls at all) for a couple of seconds while
 * a handful of nice(19) CPU-hog child processes compete for every core (the
 * chosen approach being "stress-ng if available, else your own CPU-hog with nice" --
 * stress-ng is not installed on this machine, so this is the fallback), then
 * assert the process's own consumed CPU time over that window stayed at
 * most a small fraction of the wall-clock window -- consistent with only a
 * handful of cheap poll()/eventfd wakeups (M4_AH_IDLE_POLL_MS_DEFAULT = 200 ms, so
 * ~10 wakeups over a 2 s window), and utterly inconsistent with a busy loop
 * (which would consume ~100% of a core, i.e. approach the full window).
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sched.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "m4-adaptive-bridge.h"

static int g_failures = 0;
#define CHECK(cond, ...) do { \
	if (!(cond)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); g_failures++; } \
	else { printf("PASS: "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static double now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static double rusage_self_cpu_ms(void)
{
	struct rusage ru;
	getrusage(RUSAGE_SELF, &ru);
	return (double)ru.ru_utime.tv_sec * 1000.0 + (double)ru.ru_utime.tv_usec / 1000.0 +
			(double)ru.ru_stime.tv_sec * 1000.0 + (double)ru.ru_stime.tv_usec / 1000.0;
}

/* One nice(19), SCHED_OTHER busy-loop child per (logical) CPU, so every core
 * has real competition during the idle window -- this is the "host under
 * load" half of the requirement ("verify under load"), not just
 * an unloaded-machine sanity check. Deliberately nice(19): a misbehaving
 * *test* process must never be able to degrade the real, live system (which
 * shares this machine) any more than an ordinary background task would. */
static pid_t *spawn_cpu_hogs(int n)
{
	pid_t *pids = calloc((size_t)n, sizeof(pid_t));
	for (int i = 0; i < n; i++) {
		pid_t pid = fork();
		if (pid == 0) {
			nice(19);
			volatile uint64_t x = 0;
			for (;;)
				x++;
			_exit(0);
		}
		pids[i] = pid;
	}
	return pids;
}

static void reap_cpu_hogs(pid_t *pids, int n)
{
	for (int i = 0; i < n; i++) {
		if (pids[i] > 0) {
			kill(pids[i], SIGKILL);
			waitpid(pids[i], NULL, 0);
		}
	}
	free(pids);
}

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: %s <evil_helper_path> <dummy_loader_path>\n", argv[0]);
		return 2;
	}
	const char *evil = argv[1];
	const char *dummy = argv[2];

	long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
	if (ncpu < 1)
		ncpu = 1;
	/* Cap the number of hog processes: the point is "real contention", not
	 * "as many processes as physically possible" -- the machine this runs
	 * on may be sharing real-time audio work with other processes, so
	 * this test deliberately stays small (a handful of nice(19) processes,
	 * short window) rather than trying to saturate every core on a
	 * potentially already-loaded host. nice(19) already means these yield
	 * to literally everything else runnable; the cap is extra margin on
	 * top of that. */
	if (ncpu > 4)
		ncpu = 4;

	printf("-- scenario: IO thread (SCHED_FIFO, prio=1) must not busy-wait "
			"while idle, under %ld nice(19) CPU-hog processes --\n", ncpu);

	struct m4_ah_bridge_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.qemu_path = evil;
	cfg.loader_path = dummy;
	cfg.capi_path = dummy;
	setenv("EVIL_HELPER_MODE", "normal", 1);
	cfg.strip_ota_header = true;
	cfg.helper_timeout_ms = 500;
	cfg.io_thread_rt_priority = 1; /* the RT-enabled path, see file header */

	struct m4_ah_bridge *b = m4_ah_bridge_start(NULL, &cfg);
	CHECK(b != NULL, "bridge starts with io_thread_rt_priority = 1");
	if (!b)
		return 1;

	struct m4_ah_bridge_stats st0;
	m4_ah_bridge_get_stats(b, &st0);
	CHECK(st0.bridge_rt_policy == SCHED_FIFO,
			"IO thread really is SCHED_FIFO for this check to be meaningful (got %d)",
			st0.bridge_rt_policy);

	pid_t *hogs = spawn_cpu_hogs((int)ncpu);
	/* Let the hogs actually ramp up and contend before measuring. */
	usleep(200000);

	const double window_ms = 1200.0;
	double cpu_before = rusage_self_cpu_ms();
	double t0 = now_ms();
	/* Deliberately NO submit_pcm()/take_frame() calls at all during this
	 * window -- the IO thread has nothing to do but wait, exactly the
	 * condition a busy-wait bug would show up in. */
	usleep((useconds_t)(window_ms * 1000.0));
	double elapsed_ms = now_ms() - t0;
	double cpu_after = rusage_self_cpu_ms();
	double cpu_used_ms = cpu_after - cpu_before;

	reap_cpu_hogs(hogs, (int)ncpu);

	printf("INFO: idle window %.1f ms wall-clock under %ld CPU hogs, "
			"process CPU time consumed (all threads, RUSAGE_SELF): %.2f ms\n",
			elapsed_ms, ncpu, cpu_used_ms);

	/* Threshold: a busy-wait bug would consume close to 100% of ONE core for
	 * the whole window (i.e. approach `elapsed_ms`). A correctly-blocking
	 * thread only wakes on M4_AH_IDLE_POLL_MS_DEFAULT=200ms timeouts (~10 times over
	 * this window) plus whatever the main thread's own usleep() costs
	 * (negligible) -- generous but still discriminating: 10% of the window. */
	double threshold_ms = elapsed_ms * 0.10;
	CHECK(cpu_used_ms < threshold_ms,
			"CPU consumed while idle (%.2f ms) stays well under a busy-wait "
			"threshold (%.2f ms = 10%% of the %.0f ms window) -- confirms "
			"poll()/eventfd blocking, not a spin loop", cpu_used_ms, threshold_ms,
			elapsed_ms);

	/* Bridge must still be fully responsive after the idle+contention
	 * window -- confirms the IO thread was genuinely blocked waiting for
	 * work, not stuck/deadlocked in some other way that happens to also
	 * consume no CPU (e.g. permanently parked on the wrong futex). */
	uint8_t pcm[M4_AH_PCM_BLOCK_BYTES];
	uint8_t frame[M4_AH_MAX_FRAME_BYTES];
	memset(pcm, 0, sizeof(pcm));
	unsigned got = 0;
	for (int i = 0; i < 10; i++) {
		m4_ah_bridge_submit_pcm(b, pcm, sizeof(pcm));
		usleep(15000);
		if (m4_ah_bridge_take_frame(b, frame, sizeof(frame)) > 0)
			got++;
	}
	CHECK(got > 0, "bridge resumes producing frames after the idle window (%u/10)", got);

	m4_ah_bridge_stop(b);

	printf("\n%s: %d failure(s)\n", g_failures == 0 ? "ALL PASS" : "SOME FAILED", g_failures);
	return g_failures == 0 ? 0 : 1;
}
