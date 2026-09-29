/* momentum4: resilience tests for m4-adaptive-bridge.c.
 *
 * Exercises three fault scenarios the task explicitly asks for:
 *   1. kill the helper mid-stream (SIGKILL)
 *   2. helper does not respond within its timeout (>100 ms)
 *   3. helper returns garbage (an absurd frame length)
 * plus two related ones that fell out of the same bridge logic:
 *   4. helper exits normally/unexpectedly mid-stream (no signal)
 *   5. helper never answers the startup handshake at all
 *
 * In every case the required outcome is the same: no crash, no hang
 * (bounded wall-clock time for every call), m4_ah_bridge_is_alive()
 * eventually reflects reality, and m4_ah_bridge_take_frame() keeps
 * returning 0 (silence/skip) forever after rather than fabricating data.
 *
 * Uses test/evil_helper.c as a fast, deterministic stand-in for
 * qemu-hexagon (see that file's header for the mode protocol) -- this
 * keeps these tests from depending on real qemu-hexagon's jitter/timing.
 */
#define _GNU_SOURCE
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

static void fill_dummy_pcm(uint8_t *pcm)
{
	memset(pcm, 0, M4_AH_PCM_BLOCK_BYTES);
}

/* Bounded submit+take round, timing both RT-facing calls individually.
 * Returns the max of the two call durations in ms. */
static double timed_round(struct m4_ah_bridge *b, uint8_t *frame_out, size_t *frame_len_out)
{
	uint8_t pcm[M4_AH_PCM_BLOCK_BYTES];
	fill_dummy_pcm(pcm);

	double t0 = now_ms();
	m4_ah_bridge_submit_pcm(b, pcm, sizeof(pcm));
	double t1 = now_ms();
	*frame_len_out = m4_ah_bridge_take_frame(b, frame_out, M4_AH_MAX_FRAME_BYTES);
	double t2 = now_ms();

	double submit_ms = t1 - t0, take_ms = t2 - t1;
	return submit_ms > take_ms ? submit_ms : take_ms;
}

static void scenario_sigkill_midstream(const char *evil, const char *dummy_loader)
{
	printf("\n-- scenario: SIGKILL the helper mid-stream --\n");
	struct m4_ah_bridge_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.qemu_path = evil;
	cfg.loader_path = dummy_loader;
	setenv("EVIL_HELPER_MODE", "normal", 1);
	cfg.capi_path = dummy_loader;
	cfg.strip_ota_header = true;
	cfg.helper_timeout_ms = 100;

	struct m4_ah_bridge *b = m4_ah_bridge_start(NULL, &cfg);
	CHECK(b != NULL, "bridge starts against evil_helper 'normal' mode");
	if (!b) return;

	uint8_t frame[M4_AH_MAX_FRAME_BYTES];
	size_t len = 0;
	double max_ms = 0;
	for (int i = 0; i < 5; i++) {
		double d = timed_round(b, frame, &len);
		if (d > max_ms) max_ms = d;
		usleep(15000);
	}
	CHECK(len > 0, "helper produced frames while alive (last len=%zu)", len);
	CHECK(m4_ah_bridge_is_alive(b), "bridge reports alive before the kill");

	pid_t child = m4_ah_bridge_test_child_pid(b);
	CHECK(child > 0, "bridge exposes a valid child pid for the test (%d)", (int)child);
	if (child > 0)
		kill(child, SIGKILL);

	/* Give the IO thread up to ~2x the helper timeout to notice (its next
	 * write()/read() to the now-dead pipes must fail). */
	double deadline = now_ms() + 2 * cfg.helper_timeout_ms + 200;
	bool observed_dead = false;
	while (now_ms() < deadline) {
		double d = timed_round(b, frame, &len);
		if (d > max_ms) max_ms = d;
		CHECK(d < 20.0, "submit/take stay bounded even while the child is dying (%.2f ms)", d);
		if (!m4_ah_bridge_is_alive(b)) { observed_dead = true; break; }
		usleep(20000);
	}
	CHECK(observed_dead, "bridge eventually marks itself dead after SIGKILL");
	CHECK(len == 0 || !observed_dead, "no frames are fabricated once dead");

	/* Reap the zombie so it doesn't linger past the test (best effort). */
	if (child > 0) waitpid(child, NULL, WNOHANG);

	m4_ah_bridge_stop(b); /* must return promptly, not hang */
	printf("INFO: max observed submit/take call time in this scenario: %.3f ms\n", max_ms);
}

static void scenario_hang(const char *evil, const char *dummy_loader)
{
	printf("\n-- scenario: helper accepts startup but never answers a block (>100 ms) --\n");
	struct m4_ah_bridge_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.qemu_path = evil;
	cfg.loader_path = dummy_loader;
	setenv("EVIL_HELPER_MODE", "hang", 1);
	cfg.capi_path = dummy_loader;
	cfg.strip_ota_header = true;
	cfg.helper_timeout_ms = 100; /* the explicit ">100 ms" scenario */

	double t_start = now_ms();
	struct m4_ah_bridge *b = m4_ah_bridge_start(NULL, &cfg);
	CHECK(b != NULL, "bridge starts (startup handshake still answered by 'hang' mode)");
	if (!b) return;

	uint8_t frame[M4_AH_MAX_FRAME_BYTES];
	size_t len = 0;
	double submit_call_ms = 0;
	{
		double t0 = now_ms();
		m4_ah_bridge_submit_pcm(b, "\0", 0); /* wrong size on purpose, must be rejected */
		submit_call_ms = now_ms() - t0;
	}
	CHECK(submit_call_ms < 5.0, "submit_pcm() with a bad size still returns instantly (%.3f ms)",
			submit_call_ms);

	uint8_t pcm[M4_AH_PCM_BLOCK_BYTES];
	fill_dummy_pcm(pcm);
	double t0 = now_ms();
	bool accepted = m4_ah_bridge_submit_pcm(b, pcm, sizeof(pcm));
	CHECK(accepted, "a correctly sized block is accepted into the mailbox");

	/* The IO thread is now blocked (from its own point of view) waiting on
	 * the child for up to helper_timeout_ms; take_frame() on the RT side
	 * must NEVER block regardless. Poll it repeatedly across that window. */
	double max_take_ms = 0;
	double deadline = now_ms() + cfg.helper_timeout_ms + 300;
	bool observed_dead = false;
	while (now_ms() < deadline) {
		double tt0 = now_ms();
		len = m4_ah_bridge_take_frame(b, frame, sizeof(frame));
		double tt = now_ms() - tt0;
		if (tt > max_take_ms) max_take_ms = tt;
		CHECK(tt < 5.0, "take_frame() stays sub-5ms while the IO thread is stuck (%.3f ms)", tt);
		if (!m4_ah_bridge_is_alive(b)) { observed_dead = true; break; }
		usleep(10000);
	}
	CHECK(observed_dead, "bridge marks itself dead after the %u ms helper timeout elapses "
			"(total wait %.0f ms)", cfg.helper_timeout_ms, now_ms() - t0);
	CHECK(len == 0, "no frame is ever produced for a block the helper never answered");

	double stop_t0 = now_ms();
	m4_ah_bridge_stop(b);
	double stop_ms = now_ms() - stop_t0;
	CHECK(stop_ms < 2000.0, "bridge_stop() returns promptly even against a hung child "
			"(%.0f ms, includes SIGKILL grace period)", stop_ms);
	(void)t_start;
}

static void scenario_no_startup(const char *evil, const char *dummy_loader)
{
	printf("\n-- scenario: helper never answers the startup handshake --\n");
	struct m4_ah_bridge_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.qemu_path = evil;
	cfg.loader_path = dummy_loader;
	setenv("EVIL_HELPER_MODE", "no-startup", 1);
	cfg.capi_path = dummy_loader;
	cfg.strip_ota_header = true;
	cfg.helper_timeout_ms = 100;
	cfg.startup_timeout_ms = 300; /* keep the test fast */

	double t0 = now_ms();
	struct m4_ah_bridge *b = m4_ah_bridge_start(NULL, &cfg);
	double waited = now_ms() - t0;
	CHECK(b == NULL, "bridge_start() fails cleanly when startup never arrives");
	CHECK(waited < cfg.startup_timeout_ms + 500,
			"bridge_start() gives up within its startup_timeout_ms budget (%.0f ms)", waited);
	if (b) m4_ah_bridge_stop(b);
}

static void scenario_startup_fail(const char *evil, const char *dummy_loader)
{
	printf("\n-- scenario: helper reports a nonzero startup status --\n");
	struct m4_ah_bridge_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.qemu_path = evil;
	cfg.loader_path = dummy_loader;
	setenv("EVIL_HELPER_MODE", "startup-fail", 1);
	cfg.capi_path = dummy_loader;
	cfg.strip_ota_header = true;
	cfg.helper_timeout_ms = 100;

	struct m4_ah_bridge *b = m4_ah_bridge_start(NULL, &cfg);
	CHECK(b == NULL, "bridge_start() fails cleanly on a nonzero startup status");
	if (b) m4_ah_bridge_stop(b);
}

static void scenario_garbage_len(const char *evil, const char *dummy_loader)
{
	printf("\n-- scenario: helper returns a garbage (huge) frame length --\n");
	struct m4_ah_bridge_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.qemu_path = evil;
	cfg.loader_path = dummy_loader;
	setenv("EVIL_HELPER_MODE", "garbage-len", 1);
	cfg.capi_path = dummy_loader;
	cfg.strip_ota_header = true;
	cfg.helper_timeout_ms = 200;

	struct m4_ah_bridge *b = m4_ah_bridge_start(NULL, &cfg);
	CHECK(b != NULL, "bridge starts against evil_helper 'garbage-len' mode");
	if (!b) return;

	uint8_t frame[M4_AH_MAX_FRAME_BYTES];
	size_t len = 0;
	uint8_t pcm[M4_AH_PCM_BLOCK_BYTES];
	fill_dummy_pcm(pcm);
	m4_ah_bridge_submit_pcm(b, pcm, sizeof(pcm));

	double deadline = now_ms() + cfg.helper_timeout_ms + 500;
	bool observed_dead = false;
	while (now_ms() < deadline) {
		len = m4_ah_bridge_take_frame(b, frame, sizeof(frame));
		CHECK(len == 0, "no frame body is ever read/returned for the bogus length");
		if (!m4_ah_bridge_is_alive(b)) { observed_dead = true; break; }
		usleep(10000);
	}
	CHECK(observed_dead, "bridge marks itself dead instead of trying to read "
			"an absurd frame length");
	m4_ah_bridge_stop(b);
}

static void scenario_die_midstream(const char *evil, const char *dummy_loader)
{
	printf("\n-- scenario: helper process exits unexpectedly (no signal) --\n");
	struct m4_ah_bridge_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.qemu_path = evil;
	cfg.loader_path = dummy_loader;
	setenv("EVIL_HELPER_MODE", "die-1", 1);
	cfg.capi_path = dummy_loader;
	cfg.strip_ota_header = true;
	cfg.helper_timeout_ms = 200;

	struct m4_ah_bridge *b = m4_ah_bridge_start(NULL, &cfg);
	CHECK(b != NULL, "bridge starts against evil_helper 'die-1' mode");
	if (!b) return;

	uint8_t frame[M4_AH_MAX_FRAME_BYTES];
	size_t len = 0;
	uint8_t pcm[M4_AH_PCM_BLOCK_BYTES];
	fill_dummy_pcm(pcm);

	m4_ah_bridge_submit_pcm(b, pcm, sizeof(pcm)); /* block 1: answered normally */
	usleep(30000);
	len = m4_ah_bridge_take_frame(b, frame, sizeof(frame));
	CHECK(len > 0, "first block is answered before the child exits");

	m4_ah_bridge_submit_pcm(b, pcm, sizeof(pcm)); /* block 2: child exits instead of replying */

	double deadline = now_ms() + cfg.helper_timeout_ms + 500;
	bool observed_dead = false;
	while (now_ms() < deadline) {
		double d0 = now_ms();
		(void)m4_ah_bridge_take_frame(b, frame, sizeof(frame));
		CHECK(now_ms() - d0 < 5.0, "take_frame() stays bounded after the child exits unexpectedly");
		if (!m4_ah_bridge_is_alive(b)) { observed_dead = true; break; }
		usleep(10000);
	}
	CHECK(observed_dead, "bridge notices the unexpected child exit (EOF on the pipe)");
	m4_ah_bridge_stop(b); /* also exercises reap_child() on an already-exited pid */
}

/* "restart the helper on a silent failure instead of a crash".
 * Confirms: (1) while the helper is dead, submit/take stay RT-bounded and
 * report silence (never fabricate a frame, never hang) -- same contract as
 * every other scenario above -- and (2) once it comes back, is_alive() flips
 * back to true, m4_ah_bridge_restart_count() increments, and the restarted
 * helper actually produces real frames again (not just "reports alive"). */
static void scenario_auto_restart(const char *evil, const char *dummy_loader)
{
	printf("\n-- scenario: auto_restart brings a dead helper back, silence in between --\n");
	struct m4_ah_bridge_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.qemu_path = evil;
	cfg.loader_path = dummy_loader;
	setenv("EVIL_HELPER_MODE", "die-1", 1);
	cfg.capi_path = dummy_loader;
	cfg.strip_ota_header = true;
	cfg.helper_timeout_ms = 200;
	cfg.auto_restart = true; /* the setting under test */

	struct m4_ah_bridge *b = m4_ah_bridge_start(NULL, &cfg);
	CHECK(b != NULL, "bridge starts (auto_restart=true)");
	if (!b) return;
	CHECK(m4_ah_bridge_restart_count(b) == 0, "restart_count() starts at 0");

	uint8_t frame[M4_AH_MAX_FRAME_BYTES];
	size_t len = 0;
	uint8_t pcm[M4_AH_PCM_BLOCK_BYTES];
	fill_dummy_pcm(pcm);

	m4_ah_bridge_submit_pcm(b, pcm, sizeof(pcm));
	usleep(30000);
	len = m4_ah_bridge_take_frame(b, frame, sizeof(frame));
	CHECK(len > 0, "first block is answered before the child dies");

	m4_ah_bridge_submit_pcm(b, pcm, sizeof(pcm)); /* block 2: this generation exits (die-1) */

	double deadline = now_ms() + cfg.helper_timeout_ms + 500;
	bool observed_dead = false;
	while (now_ms() < deadline) {
		double d0 = now_ms();
		(void)m4_ah_bridge_take_frame(b, frame, sizeof(frame));
		CHECK(now_ms() - d0 < 5.0, "take_frame() stays bounded while the helper is dead/restarting");
		if (!m4_ah_bridge_is_alive(b)) { observed_dead = true; break; }
		usleep(10000);
	}
	CHECK(observed_dead, "bridge notices the dead helper");

	/* Switch the NEXT spawned generation to a mode that keeps answering, so
	 * we can assert on a stable post-restart run instead of a restart loop
	 * (also correct, just harder to assert on deterministically here).
	 * fork() captures the environment at (re)spawn time, so this only
	 * affects generations from this point on. */
	setenv("EVIL_HELPER_MODE", "normal", 1);

	/* Initial backoff is 500 ms (M4_AH_RESTART_BACKOFF_MIN_MS_DEFAULT in
	 * m4-adaptive-bridge.c); give it generous margin. */
	double restart_deadline = now_ms() + 3000.0;
	bool restarted = false;
	while (now_ms() < restart_deadline) {
		if (m4_ah_bridge_is_alive(b)) { restarted = true; break; }
		usleep(20000);
	}
	CHECK(restarted, "bridge auto-restarts within ~3 s of backoff");
	CHECK(m4_ah_bridge_restart_count(b) >= 1, "restart_count() reflects the restart (got %u)",
			(unsigned)m4_ah_bridge_restart_count(b));

	if (restarted) {
		int got = 0;
		for (int i = 0; i < 10; i++) {
			m4_ah_bridge_submit_pcm(b, pcm, sizeof(pcm));
			usleep(15000);
			if (m4_ah_bridge_take_frame(b, frame, sizeof(frame)) > 0)
				got++;
		}
		CHECK(got > 0, "the restarted helper produces real frames again (%d/10 blocks)", got);
	}

	m4_ah_bridge_stop(b);
}

/* Regression guard for the OLD (pre-auto-restart) behaviour, still the
 * default (auto_restart defaults to false via {0}-initialization, exactly as
 * every scenario above already relies on): a helper that dies with
 * auto_restart=false must stay dead forever, never spontaneously come back,
 * and restart_count() must stay 0. */
static void scenario_no_auto_restart_stays_dead(const char *evil, const char *dummy_loader)
{
	printf("\n-- scenario: auto_restart=false (default) never comes back on its own --\n");
	struct m4_ah_bridge_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.qemu_path = evil;
	cfg.loader_path = dummy_loader;
	setenv("EVIL_HELPER_MODE", "die-1", 1);
	cfg.capi_path = dummy_loader;
	cfg.strip_ota_header = true;
	cfg.helper_timeout_ms = 200;
	cfg.auto_restart = false; /* explicit, though this is also the {0} default */

	struct m4_ah_bridge *b = m4_ah_bridge_start(NULL, &cfg);
	CHECK(b != NULL, "bridge starts (auto_restart=false)");
	if (!b) return;

	uint8_t frame[M4_AH_MAX_FRAME_BYTES];
	uint8_t pcm[M4_AH_PCM_BLOCK_BYTES];
	fill_dummy_pcm(pcm);
	m4_ah_bridge_submit_pcm(b, pcm, sizeof(pcm));
	usleep(30000);
	m4_ah_bridge_take_frame(b, frame, sizeof(frame));
	m4_ah_bridge_submit_pcm(b, pcm, sizeof(pcm)); /* this generation exits */

	double deadline = now_ms() + cfg.helper_timeout_ms + 500;
	while (now_ms() < deadline && m4_ah_bridge_is_alive(b))
		usleep(10000);
	CHECK(!m4_ah_bridge_is_alive(b), "helper is dead after die-1");

	/* Switch to "normal" mode: if auto_restart were (incorrectly) active,
	 * the next generation would come up healthy and we would catch it. */
	setenv("EVIL_HELPER_MODE", "normal", 1);
	usleep(2000000); /* well past the auto-restart scenario's ~500 ms-3 s window */
	CHECK(!m4_ah_bridge_is_alive(b), "still dead 2 s later: no spontaneous restart");
	CHECK(m4_ah_bridge_restart_count(b) == 0, "restart_count() stays 0 (got %u)",
			(unsigned)m4_ah_bridge_restart_count(b));

	m4_ah_bridge_stop(b);
}

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: %s <evil_helper binary> <dummy loader path (any file)>\n", argv[0]);
		return 2;
	}
	const char *evil = argv[1];
	const char *dummy_loader = argv[2];

	/* Block SIGPIPE in the *test* process too: if the per-thread-mask fix
	 * in m4-adaptive-bridge.c's IO thread ever regresses, a dead evil_helper
	 * pipe would otherwise take the whole test process down with it instead
	 * of failing a single CHECK() -- fail loud in the test, not silently
	 * pass because the process never got the chance to print a FAIL. */
	signal(SIGPIPE, SIG_IGN);

	scenario_sigkill_midstream(evil, dummy_loader);
	scenario_hang(evil, dummy_loader);
	scenario_no_startup(evil, dummy_loader);
	scenario_startup_fail(evil, dummy_loader);
	scenario_garbage_len(evil, dummy_loader);
	scenario_die_midstream(evil, dummy_loader);
	scenario_auto_restart(evil, dummy_loader);
	scenario_no_auto_restart_stays_dead(evil, dummy_loader);

	printf("\n%d check(s) failed\n", g_failures);
	return g_failures == 0 ? 0 : 1;
}
