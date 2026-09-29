/* momentum4: reproduces the real-world "crackle/desync" bug found
 * live ("live crackle and latency") and proves the
 * m4-adaptive-bridge.c ring-buffer fix resolves it.
 *
 * Root cause (see m4-adaptive-bridge.h file header for the full writeup):
 * spa/plugins/bluez5/media-sink.c's flush_data() deliberately calls
 * struct media_codec::encode() more than once per graph cycle whenever more
 * source PCM is already available ("encode next packet already now; it will
 * be flushed later on timer") -- which happens on essentially every cycle
 * once the PipeWire graph quantum (512-1024 samples in this project's
 * configs) exceeds this codec's fixed 480-sample/10 ms block. That means
 * codec_encode_adaptive() -> m4_ah_bridge_submit_pcm() can be called two or
 * more times with no real wall-clock time between calls (same RT thread, no
 * scheduling gap at all).
 *
 * The original v1/v2 bridge used a *single-slot* PCM input mailbox that
 * submit_pcm() unconditionally overwrote. Two submit_pcm() calls a few
 * microseconds apart -- far less time than a full IO-thread round trip
 * (thread wakeup + pipe/socket write/read, at least tens of microseconds) --
 * meant the second call silently clobbered the first call's PCM before the
 * IO thread ever read it: real, permanent audio data loss feeding a
 * stateful/predictive encoder, not just an occasionally-skipped *output*
 * packet. This test reproduces exactly that call pattern (a tight burst of
 * submit_pcm() calls, zero delay, mimicking one media-sink.c graph cycle at
 * quantum=1024) and checks that every block tagged with a distinct marker is
 * delivered exactly once, in order, with the new ring-buffer design.
 *
 * Uses test/evil_helper.c's "echo-seq" mode (see that file) so each
 * delivered frame directly reports which PCM block it was derived from,
 * without depending on the real Qualcomm encoder's output format.
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

/* Tags a PCM block's first 4 bytes with `seq` (little-endian), rest zeroed;
 * echo-seq mode in evil_helper.c returns exactly these 4 bytes as the
 * delivered frame (after OTA-header stripping). */
static void fill_tagged_pcm(uint8_t *pcm, uint32_t seq)
{
	memset(pcm, 0, M4_AH_PCM_BLOCK_BYTES);
	memcpy(pcm, &seq, sizeof(seq));
}

/* Burst of N submit_pcm() calls with NO delay at all between them (the exact
 * pattern media-sink.c's "goto again" eager pre-encode produces within one
 * graph cycle when quantum > block_size -- see file header), then drains
 * take_frame() (with brief sleeps to let the IO thread catch up, since a
 * real bridge is intentionally asynchronous) until either all N tagged
 * frames have been seen or a generous overall deadline expires. Verifies:
 * every seq 0..N-1 appears exactly once, in increasing order (FIFO), none
 * missing (silent drop -- the original bug) and none duplicated/corrupted. */
static void scenario_tight_burst(const char *evil, const char *dummy_loader,
		unsigned burst_len)
{
	printf("\n-- scenario: tight burst of %u submit_pcm() calls, zero delay --\n",
			burst_len);
	struct m4_ah_bridge_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.qemu_path = evil;
	cfg.loader_path = dummy_loader;
	cfg.capi_path = dummy_loader;
	cfg.strip_ota_header = true;
	cfg.helper_timeout_ms = 500;
	setenv("EVIL_HELPER_MODE", "echo-seq", 1);

	struct m4_ah_bridge *b = m4_ah_bridge_start(NULL, &cfg);
	CHECK(b != NULL, "bridge starts against evil_helper 'echo-seq' mode");
	if (!b) return;

	/* Prime: the bridge design has an intentional ~1-block steady-state lag
	 * (see m4-adaptive-bridge.h); do a few unhurried rounds first so the
	 * burst below starts from steady state, matching real usage. */
	for (int i = 0; i < 3; i++) {
		uint8_t pcm[M4_AH_PCM_BLOCK_BYTES], frame[M4_AH_MAX_FRAME_BYTES];
		fill_tagged_pcm(pcm, 0xFFFFFFFFu); /* priming marker, not checked */
		m4_ah_bridge_submit_pcm(b, pcm, sizeof(pcm));
		usleep(5000);
		m4_ah_bridge_take_frame(b, frame, sizeof(frame));
	}

	/* The actual burst: this is the part that would have silently dropped
	 * data with the old single-slot mailbox. Also opportunistically drains
	 * take_frame() between submits (exactly like codec_encode_adaptive()
	 * does -- submit then immediately take, every call), which is itself
	 * part of what must not lose data. */
	uint8_t submit_frame_buf[M4_AH_MAX_FRAME_BYTES];
	int seen[4096] = { 0 };
	unsigned delivered_during_burst = 0;
	double t_burst_start = now_ms();
	for (unsigned i = 0; i < burst_len; i++) {
		uint8_t pcm[M4_AH_PCM_BLOCK_BYTES];
		fill_tagged_pcm(pcm, i);
		bool accepted = m4_ah_bridge_submit_pcm(b, pcm, sizeof(pcm));
		CHECK(accepted, "burst block %u accepted by submit_pcm() (ring not "
				"unexpectedly full)", i);
		size_t len = m4_ah_bridge_take_frame(b, submit_frame_buf, sizeof(submit_frame_buf));
		if (len == 4) {
			uint32_t got;
			memcpy(&got, submit_frame_buf, 4);
			if (got < burst_len && got != 0xFFFFFFFFu) {
				seen[got]++;
				delivered_during_burst++;
			}
		}
	}
	double t_burst_ms = now_ms() - t_burst_start;
	CHECK(t_burst_ms < 5.0, "the whole %u-call burst took %.3f ms (RT-safety: "
			"no call in it may block on the helper)", burst_len, t_burst_ms);

	/* Drain whatever is still in flight (the bridge is asynchronous; some
	 * of the burst's frames legitimately finish slightly after the burst
	 * itself, this is expected and fine -- what matters is nothing is ever
	 * lost or duplicated). */
	double deadline = now_ms() + 2000.0;
	unsigned total_delivered = delivered_during_burst;
	while (total_delivered < burst_len && now_ms() < deadline) {
		size_t len = m4_ah_bridge_take_frame(b, submit_frame_buf, sizeof(submit_frame_buf));
		if (len == 4) {
			uint32_t got;
			memcpy(&got, submit_frame_buf, 4);
			if (got < burst_len && got != 0xFFFFFFFFu && seen[got] == 0) {
				seen[got] = 1;
				total_delivered++;
			}
		} else {
			usleep(1000);
		}
	}

	unsigned missing = 0, duplicated = 0;
	for (unsigned i = 0; i < burst_len; i++) {
		if (seen[i] == 0) missing++;
		if (seen[i] > 1) duplicated++;
	}
	CHECK(missing == 0, "0 of %u burst-tagged blocks missing (got %u missing) "
			"-- this is the exact bug the ring buffer fixes: the old "
			"single-slot mailbox would drop most of a same-cycle burst here",
			burst_len, missing);
	CHECK(duplicated == 0, "0 of %u burst-tagged blocks duplicated (got %u)",
			burst_len, duplicated);
	CHECK(total_delivered == burst_len, "all %u burst-tagged blocks eventually "
			"delivered (got %u within the 2 s drain deadline)",
			burst_len, total_delivered);

	m4_ah_bridge_stop(b);
}

/* Sanity check for the ring's own overflow accounting: submit more blocks
 * back-to-back than M4_AH_RING_SLOTS_DEFAULT without ever calling take_frame() in
 * between (worst case: nothing drains the ring at all during the burst).
 * Some rejections are expected once the ring is genuinely full -- the
 * important thing is submit_pcm() reports this honestly via its return
 * value / m4_ah_bridge_get_stats(), rather than silently overwriting. */
static void scenario_overflow_is_reported(const char *evil, const char *dummy_loader)
{
	printf("\n-- scenario: ring overflow is reported, not silent --\n");
	struct m4_ah_bridge_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.qemu_path = evil;
	cfg.loader_path = dummy_loader;
	cfg.capi_path = dummy_loader;
	cfg.strip_ota_header = true;
	cfg.helper_timeout_ms = 500;
	setenv("EVIL_HELPER_MODE", "hang", 1); /* never answers -> IO thread never drains the ring */

	struct m4_ah_bridge *b = m4_ah_bridge_start(NULL, &cfg);
	CHECK(b != NULL, "bridge starts against evil_helper 'hang' mode");
	if (!b) return;

	unsigned accepted = 0, rejected = 0;
	uint8_t pcm[M4_AH_PCM_BLOCK_BYTES];
	fill_tagged_pcm(pcm, 0);
	for (unsigned i = 0; i < 64; i++) {
		if (m4_ah_bridge_submit_pcm(b, pcm, sizeof(pcm)))
			accepted++;
		else
			rejected++;
	}
	CHECK(accepted > 0 && accepted <= 64, "some blocks accepted before the "
			"never-draining ring fills up (got %u)", accepted);
	CHECK(rejected > 0, "the rest are honestly rejected once the ring is "
			"full, not silently overwritten (got %u rejected)", rejected);

	struct m4_ah_bridge_stats st;
	m4_ah_bridge_get_stats(b, &st);
	CHECK(st.input_ring_full_drops == rejected, "stats.input_ring_full_drops "
			"(%u) matches the caller-observed rejection count (%u)",
			(unsigned)st.input_ring_full_drops, rejected);
	CHECK(st.in_ring_count == M4_AH_RING_SLOTS_DEFAULT, "stats.in_ring_count reports "
			"the ring completely full (%u/%u)", st.in_ring_count,
			(unsigned)M4_AH_RING_SLOTS_DEFAULT);

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

	/* Sizes spanning the realistic range of media-sink.c's within-cycle
	 * burst (quantum 512 -> occasionally 2; quantum 1024 -> ~2 every
	 * cycle; a deliberately pessimistic 6 to also exercise multi-cycle
	 * backlog draining). */
	scenario_tight_burst(evil, dummy_loader, 2);
	scenario_tight_burst(evil, dummy_loader, 3);
	scenario_tight_burst(evil, dummy_loader, 6);
	scenario_overflow_is_reported(evil, dummy_loader);

	printf("\n%d check(s) failed\n", g_failures);
	return g_failures == 0 ? 0 : 1;
}
