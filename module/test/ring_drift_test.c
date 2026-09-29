/* momentum4: "Latency drift in the output ring" -- synthetic
 * reproduction of a LIVE bug (metrics showed ring_out
 * stepping 1 -> 3 -> 4 -> 5 and never coming back down, lining up with
 * helper stall/late/restart events) and proof that the
 * ring-trim fix in m4_ah_bridge_take_frame()/io_thread_steady_state()
 * (m4-adaptive-bridge.c) actually converges back to
 * M4_AH_RING_TARGET_FRAMES_DEFAULT within a bounded time, WITHOUT ever blocking a
 * caller.
 *
 * Drives the bridge at a realistic ~10 ms cadence (the real RT thread's
 * cadence, see m4-adaptive-bridge.h) using test/evil_helper.c's "stall"
 * mode: normal, immediate replies for every block EXCEPT one, which is
 * artificially delayed by EVIL_HELPER_STALL_MS -- exactly the "helper
 * round-trip stall" scenario from the file header (the helper delivers a
 * burst after a delay). While the IO thread is blocked in that one delayed
 * round trip, the test keeps calling submit_pcm()/take_frame() at the
 * normal cadence (as the real RT thread would), which is exactly what
 * causes the input ring to back up and then, once the stalled reply
 * finally arrives, the output ring to receive a rapid burst of frames.
 *
 * Verifies, per task requirement:
 *   1. No single submit_pcm()/take_frame() call ever takes more than a
 *      bounded time (5 ms, same threshold resilience_test.c already uses)
 *      -- including the exact call that happens to land while the IO
 *      thread is mid-stall. This is the core RT-safety contract; the
 *      whole point of the asynchronous bridge design is that a helper-side
 *      stall of up to hundreds of ms is invisible to the RT thread.
 *   2. Output ring occupancy returns to <= M4_AH_RING_TARGET_FRAMES_DEFAULT within
 *      1 s of first exceeding it ("not a single extra frame
 *      longer than ~1s" bound, i.e. M4_AH_RING_TRIM_HARD_CAP_MS_DEFAULT from
 *      m4-adaptive-bridge.c) -- checked for BOTH silent PCM content
 *      (expected to converge much faster, around
 *      M4_AH_RING_TRIM_SUSTAIN_MS_DEFAULT) and loud/continuous content (expected
 *      to converge only once the hard cap forces it).
 *   3. The loop never hangs/crashes and keeps producing frames after
 *      recovery (a stand-in for "sound is continuous": the bridge never
 *      fabricates fake silence gaps beyond what take_frame() legitimately
 *      returning 0 already means, and resumes normal delivery once caught
 *      up).
 *
 * Uses SPAWN mode (like ring_burst_test.c/resilience_test.c) against the
 * evil_helper stand-in -- no qemu-hexagon involved, so this test is fast
 * and deterministic regardless of host load.
 */
#define _GNU_SOURCE
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

/* m4_ah_bridge_get_stats() is deliberately best-effort about ring occupancy
 * (bounded to a trylock per ring, "skip -- leave at 0 -- rather than block a
 * caller that must stay RT-safe", see m4-adaptive-bridge.h): under an ASan/
 * UBSan build, the IO thread's own brief lock hold (a memcpy of up to
 * M4_AH_MAX_FRAME_BYTES) is measurably slower due to instrumentation, which
 * made this test's single-shot sampling race the trylock and occasionally
 * observe a stale/zero occupancy even while the real ring was genuinely
 * backed up (confirmed: max_call_ms/restart_count/continuity all still
 * passed correctly in that run -- only the DIAGNOSTIC read was affected, not
 * the bridge's actual behaviour). Retries the same public, real API a
 * bounded, small number of times to avoid missing a real reading purely due
 * to a momentary lock miss -- this changes nothing about what is being
 * tested, only how patiently this test observes it. */
static void get_stats_robust(struct m4_ah_bridge *b, struct m4_ah_bridge_stats *out)
{
	for (int attempt = 0; attempt < 20; attempt++) {
		m4_ah_bridge_get_stats(b, out);
		if (out->out_ring_count > 0 || out->in_ring_count > 0)
			return;
		if (attempt < 19)
			usleep(200);
	}
}

/* `silent`: fill with all-zero samples (below M4_AH_RING_QUIET_AMP_DEFAULT, so the
 * ring-trim's silence-preferred path can fire). Otherwise a full-scale
 * square-ish tone (always >= quiet_amp), forcing convergence to rely on the
 * hard cap alone. First 4 bytes are NOT overwritten with a marker here
 * (unlike ring_burst_test.c) -- this test only cares about ring occupancy
 * and call latency, not per-block identity. */
static void fill_pcm(uint8_t *pcm, bool silent)
{
	int16_t *s = (int16_t *)pcm;
	size_t n = M4_AH_PCM_SAMPLES_PER_BLOCK * M4_AH_PCM_CHANNELS;
	for (size_t i = 0; i < n; i++)
		s[i] = silent ? 0 : (int16_t)((i & 1) ? 20000 : -20000);
}

/* Runs one stall scenario end-to-end. `stall_ms` is injected via
 * EVIL_HELPER_STALL_MS; `silent` controls whether the driven PCM is quiet
 * (exercises the sustain+silent trim path) or loud (exercises the hard-cap
 * fallback only). `total_ms` bounds how long the whole drive loop runs
 * after the stall (must comfortably exceed M4_AH_RING_TRIM_HARD_CAP_MS_DEFAULT so
 * convergence has a real chance to be observed either way). */
static void scenario_stall(const char *evil, const char *dummy_loader,
		double stall_ms, bool silent)
{
	printf("\n-- scenario: synthetic helper stall of %.0f ms, %s PCM --\n",
			stall_ms, silent ? "silent" : "loud");

	char stall_ms_buf[32];
	snprintf(stall_ms_buf, sizeof(stall_ms_buf), "%.0f", stall_ms);
	setenv("EVIL_HELPER_MODE", "stall", 1);
	setenv("EVIL_HELPER_STALL_AT_BLOCK", "20", 1); /* after priming settles */
	setenv("EVIL_HELPER_STALL_MS", stall_ms_buf, 1);

	struct m4_ah_bridge_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.qemu_path = evil;
	cfg.loader_path = dummy_loader;
	cfg.capi_path = dummy_loader;
	cfg.strip_ota_header = true;
	/* Generous vs. the injected stall so the IO thread's round-trip timeout
	 * itself never fires and marks the bridge dead -- this test is about
	 * the RING, not the auto-restart path (see resilience_test.c for that). */
	cfg.helper_timeout_ms = (uint32_t)(stall_ms * 4.0 + 2000.0);

	struct m4_ah_bridge *b = m4_ah_bridge_start(NULL, &cfg);
	CHECK(b != NULL, "bridge starts against evil_helper 'stall' mode (stall=%.0f ms)",
			stall_ms);
	if (!b) return;

	double max_call_ms = 0.0;
	double first_over_target_ms = -1.0;
	double converged_at_ms = -1.0;
	double peak_out_ring = 0;
	bool ever_alive_after_stall = false;

	double t0 = now_ms();
	/* Run for a few seconds of simulated real-time cadence: long enough to
	 * (a) prime, (b) hit the stall at block 20 (~200 ms in), (c) observe the
	 * ring spike once the stalled reply finally arrives, and (d) watch it
	 * converge, with real margin past M4_AH_RING_TRIM_HARD_CAP_MS_DEFAULT (1000 ms). */
	double run_ms = stall_ms + 3000.0;
	unsigned i = 0;
	double next_tick_ms = t0 + 10.0;
	while (now_ms() - t0 < run_ms) {
		uint8_t pcm[M4_AH_PCM_BLOCK_BYTES];
		uint8_t frame[M4_AH_MAX_FRAME_BYTES];
		fill_pcm(pcm, silent);

		double c0 = now_ms();
		bool accepted = m4_ah_bridge_submit_pcm(b, pcm, sizeof(pcm));
		double c1 = now_ms();
		size_t len = m4_ah_bridge_take_frame(b, frame, sizeof(frame));
		double c2 = now_ms();

		(void)accepted; /* a handful of rejections during the worst of the
			* backlog are expected and fine -- see M4_AH_RING_SLOTS_DEFAULT; this
			* test is about latency-bound convergence, not zero loss (that
			* is ring_burst_test.c's job for the SHORT, self-resolving
			* burst case). */
		(void)len;

		double call1 = c1 - c0, call2 = c2 - c1;
		if (call1 > max_call_ms) max_call_ms = call1;
		if (call2 > max_call_ms) max_call_ms = call2;

		struct m4_ah_bridge_stats st;
		get_stats_robust(b, &st);
		if (st.out_ring_count > peak_out_ring)
			peak_out_ring = st.out_ring_count;

		double now = now_ms();
		if (st.out_ring_count > M4_AH_RING_TARGET_FRAMES_DEFAULT) {
			if (first_over_target_ms < 0.0)
				first_over_target_ms = now;
			converged_at_ms = -1.0; /* not converged (yet/again) */
		} else if (first_over_target_ms >= 0.0 && converged_at_ms < 0.0) {
			converged_at_ms = now;
		}

		if (m4_ah_bridge_is_alive(b))
			ever_alive_after_stall = true;

		i++;
		/* Deadline-paced ~10 ms cadence (not a fixed usleep(10000) after the
		 * work above): keeps the loop's actual cadence close to real-time
		 * regardless of how long get_stats_robust()'s retries or ASan/UBSan
		 * instrumentation overhead take per iteration (both confirmed to
		 * stay well under 10 ms -- max_call_ms itself is checked above), so
		 * this test's OWN pacing does not itself change how much backlog the
		 * synthetic stall produces between build configurations. */
		next_tick_ms += 10.0;
		double remaining_ms = next_tick_ms - now_ms();
		if (remaining_ms > 0.0)
			usleep((useconds_t)(remaining_ms * 1000.0));
		else
			next_tick_ms = now_ms(); /* fell behind by more than one tick; resync */
	}

	CHECK(ever_alive_after_stall, "bridge is alive at some point during the run");
	CHECK(max_call_ms < 5.0, "no single submit_pcm()/take_frame() call ever took "
			">= 5 ms, even the one(s) racing the %.0f ms helper stall (max "
			"observed %.3f ms)", stall_ms, max_call_ms);
	CHECK(peak_out_ring > M4_AH_RING_TARGET_FRAMES_DEFAULT, "the stall actually produced "
			"a real backlog above target to converge from (peak out_ring=%.0f, "
			"target=%u) -- otherwise this scenario would not be testing anything",
			peak_out_ring, (unsigned)M4_AH_RING_TARGET_FRAMES_DEFAULT);

	if (first_over_target_ms >= 0.0 && converged_at_ms >= 0.0) {
		double drain_ms = converged_at_ms - first_over_target_ms;
		/* +100 ms margin over the "~1 s" target / the bridge's own 1000 ms
		 * hard cap: this test's own drive loop only samples ring occupancy
		 * once per ~10 ms usleep()-paced iteration (not a real RT thread),
		 * so detecting "back at target" can lag the bridge's own internal
		 * timestamp by up to one iteration. */
		CHECK(drain_ms <= 1100.0, "output ring returned to <= %u within ~1 s of "
				"first exceeding it (took %.0f ms, stall=%.0f ms, %s PCM)",
				(unsigned)M4_AH_RING_TARGET_FRAMES_DEFAULT, drain_ms, stall_ms,
				silent ? "silent" : "loud");
		if (silent) {
			/* Silent content should converge much sooner than the hard cap,
			 * via the sustain+silent path (M4_AH_RING_TRIM_SUSTAIN_MS_DEFAULT =
			 * 200 ms in m4-adaptive-bridge.c) -- a looser bound than the
			 * hard-cap check above, just to confirm the "prefer silence"
			 * half of the design is actually doing something, not merely
			 * riding the hard cap the whole time. */
			CHECK(drain_ms <= 500.0, "silent-PCM convergence used the "
					"sustain+silent trim path, not just the 1 s hard cap "
					"(took %.0f ms)", drain_ms);
		}
	} else {
		CHECK(false, "ring occupancy both exceeded target AND returned to "
				"target within the run window (first_over=%.0f converged=%.0f)",
				first_over_target_ms, converged_at_ms);
	}

	/* Sound-continuity stand-in: after recovery, the bridge keeps producing
	 * real frames again at the normal cadence (not stuck starved forever). */
	{
		unsigned got = 0;
		double deadline = now_ms() + 500.0;
		while (got < 5 && now_ms() < deadline) {
			uint8_t pcm[M4_AH_PCM_BLOCK_BYTES], frame[M4_AH_MAX_FRAME_BYTES];
			fill_pcm(pcm, false);
			m4_ah_bridge_submit_pcm(b, pcm, sizeof(pcm));
			if (m4_ah_bridge_take_frame(b, frame, sizeof(frame)) > 0)
				got++;
			usleep(10000);
		}
		CHECK(got >= 5, "bridge keeps delivering real frames after recovery "
				"(sound continuity: got %u/5 in a fresh 5-block probe)", got);
	}

	struct m4_ah_bridge_stats final_st;
	m4_ah_bridge_get_stats(b, &final_st);
	printf("INFO: stall=%.0fms silent=%d peak_out_ring=%.0f trimmed_out=%u "
			"trimmed_in_ms=%u restarts=%u\n", stall_ms, (int)silent,
			peak_out_ring, (unsigned)final_st.output_ring_trimmed_frames,
			(unsigned)final_st.input_ring_trimmed_ms,
			m4_ah_bridge_restart_count(b));
	CHECK(m4_ah_bridge_restart_count(b) == 0, "the transient stall never made "
			"the bridge think the helper had died (restart_count == 0, got %u)",
			m4_ah_bridge_restart_count(b));

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

	signal(SIGPIPE, SIG_IGN);

	/* Explicit range: "synthetic helper stall, 50-200ms". */
	scenario_stall(evil, dummy_loader, 50.0, true);
	scenario_stall(evil, dummy_loader, 50.0, false);
	scenario_stall(evil, dummy_loader, 100.0, true);
	scenario_stall(evil, dummy_loader, 100.0, false);
	scenario_stall(evil, dummy_loader, 200.0, true);
	scenario_stall(evil, dummy_loader, 200.0, false);

	printf("\n%d check(s) failed\n", g_failures);
	return g_failures == 0 ? 0 : 1;
}
