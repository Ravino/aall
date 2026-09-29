/* momentum4: fault-injection stand-in for the real qemu-hexagon
 * helper, used by resilience_test.c to exercise m4-adaptive-bridge.c
 * against a misbehaving child WITHOUT depending on real qemu-hexagon timing
 * (which would make failure-injection tests slow and flaky).
 *
 * m4_ah_bridge_start() execs `qemu_path loader_path capi_path`, and
 * separately access()-checks loader_path/capi_path as real, readable files
 * before ever exec'ing (see m4-adaptive-bridge.c) -- a real safety check we
 * do not want to weaken just for testing. So resilience_test.c passes real
 * (but content-irrelevant) files for loader_path/capi_path, and selects
 * this binary's behaviour via the EVIL_HELPER_MODE environment variable
 * instead (inherited across fork()+exec() like any other env var; falls
 * back to argv[2] for convenient manual/standalone invocation).
 *
 * Speaks the exact wire protocol from ../pipewire-src/spa/plugins/bluez5/
 * m4-adaptive-protocol.h (== loader/PROTOCOL.h).
 *
 * Modes (EVIL_HELPER_MODE env var, or argv[2] if unset):
 *   normal        Always replies immediately with a small fixed 4-byte
 *                  fake "frame" to every PCM block. Used for the "kill the
 *                  helper mid-stream" test (the test process SIGKILLs this
 *                  child externally via m4_ah_bridge_test_child_pid()).
 *   hang          Sends a valid startup reply, then never answers any
 *                  block request. Used for the "helper does not respond
 *                  within N ms" test.
 *   no-startup    Never sends the startup reply at all (also just hangs).
 *                  Used for the bridge-start startup-timeout path.
 *   startup-fail  Sends a startup reply with a nonzero status. Used for
 *                  the "helper reports CAPI init failure" path.
 *   garbage-len   Valid startup; on the first block request, replies with
 *                  frame_bytes = 0xFFFFFFFF (and no frame body). Exercises
 *                  the bridge's oversized-length defense.
 *   die-1         Valid startup; replies normally to the first block, then
 *                  exits(1) before answering the second. Used to test a
 *                  clean/unexpected child exit mid-stream (as opposed to a
 *                  signal-based kill).
 *   echo-seq      Valid startup; each reply's frame body is the first 4
 *                  bytes of the PCM block it was asked to encode (prefixed
 *                  with the same 8 dummy OTA-header bytes as the other
 *                  modes, so strip_ota_header=true leaves exactly those 4
 *                  bytes as the delivered frame). Lets a test tag each PCM
 *                  block with a distinguishing marker (e.g. a block
 *                  sequence number written into the first samples) and then
 *                  verify, end-to-end through the real bridge code, that
 *                  every submitted block is eventually delivered exactly
 *                  once and in order -- used by ring_burst_test.c to prove
 *                  the input/output ring buffers do not drop or reorder
 *                  blocks submitted back-to-back within one "graph cycle"
 *                  (see m4-adaptive-bridge.h file header).
 *   stall         Same echo-seq behaviour (marker = first 4 bytes of the
 *                  PCM block, for ordering/no-duplication checks), EXCEPT
 *                  that on block number EVIL_HELPER_STALL_AT_BLOCK (default
 *                  5, 1-based) it sleeps for EVIL_HELPER_STALL_MS ms
 *                  (default 100) BEFORE replying to THAT ONE block, then
 *                  resumes normal immediate replies for every later block --
 *                  a transient round-trip stall (TCG hiccup, host
 *                  scheduling), not a permanent hang or crash. Models
 *                  exactly the "Latency drift in the output ring" scenario
 *                  (the helper delivers a burst after a delay): the IO thread's
 *                  single blocking round trip for that one block takes a
 *                  while, during which the RT thread keeps submitting new
 *                  blocks at its normal cadence (building up a backlog in
 *                  the input ring), then the IO thread drains that backlog
 *                  in a rapid burst once the stalled reply finally arrives.
 *                  Used by test/ring_drift_test.c.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int write_all(int fd, const void *buf, size_t len)
{
	const unsigned char *p = buf;
	while (len > 0) {
		ssize_t n = write(fd, p, len);
		if (n <= 0)
			return -1;
		p += (size_t)n;
		len -= (size_t)n;
	}
	return 0;
}

static int read_all(int fd, void *buf, size_t len)
{
	unsigned char *p = buf;
	while (len > 0) {
		ssize_t n = read(fd, p, len);
		if (n <= 0)
			return -1;
		p += (size_t)n;
		len -= (size_t)n;
	}
	return 0;
}

int main(int argc, char **argv)
{
	const char *mode = getenv("EVIL_HELPER_MODE");
	if (!mode)
		mode = argc > 2 ? argv[2] : "normal";
	int block_no = 0;

	if (strcmp(mode, "no-startup") == 0) {
		pause(); /* wait forever for a signal; startup handshake never comes */
		return 0;
	}

	if (strcmp(mode, "startup-fail") == 0) {
		uint32_t reply[2] = { 1u /* nonzero status */, 0u };
		write_all(1, reply, sizeof(reply));
		return 0;
	}

	/* All remaining modes send a valid startup reply first. */
	{
		uint32_t reply[2] = { 0u, 0u };
		if (write_all(1, reply, sizeof(reply)) != 0)
			return 1;
	}

	if (strcmp(mode, "hang") == 0) {
		pause();
		return 0;
	}

	for (;;) {
		uint32_t len_hdr;
		unsigned char pcm[64 * 1024];

		if (read_all(0, &len_hdr, sizeof(len_hdr)) != 0)
			return 0; /* EOF: parent asked us to exit, same as the real guest */
		if (len_hdr > sizeof(pcm))
			return 1;
		if (read_all(0, pcm, len_hdr) != 0)
			return 0;

		block_no++;

		if (strcmp(mode, "garbage-len") == 0 && block_no == 1) {
			uint32_t reply[2] = { 0u, 0xFFFFFFFFu };
			write_all(1, reply, sizeof(reply));
			/* Deliberately do NOT send a matching frame body: the bridge
			 * must reject this on frame_bytes alone, without trying to
			 * read an absurd amount of frame data. */
			continue;
		}

		if (strcmp(mode, "die-1") == 0 && block_no >= 2)
			return 1; /* unexpected exit instead of a reply */

		if (strcmp(mode, "echo-seq") == 0) {
			unsigned char fake_frame[12] = {
				0, 0, 0, 0, 0, 0, 0, 0xAD, 0, 0, 0, 0 };
			memcpy(fake_frame + 8, pcm, 4);
			uint32_t reply[2] = { 0u, (uint32_t)sizeof(fake_frame) };
			if (write_all(1, reply, sizeof(reply)) != 0)
				return 0;
			if (write_all(1, fake_frame, sizeof(fake_frame)) != 0)
				return 0;
			continue;
		}

		if (strcmp(mode, "stall") == 0) {
			const char *at_s = getenv("EVIL_HELPER_STALL_AT_BLOCK");
			const char *ms_s = getenv("EVIL_HELPER_STALL_MS");
			int stall_at = at_s ? atoi(at_s) : 5;
			long stall_ms = ms_s ? atol(ms_s) : 100;
			unsigned char fake_frame[12] = {
				0, 0, 0, 0, 0, 0, 0, 0xAD, 0, 0, 0, 0 };
			memcpy(fake_frame + 8, pcm, 4);
			if (block_no == stall_at) {
				struct timespec ts;
				ts.tv_sec = stall_ms / 1000;
				ts.tv_nsec = (stall_ms % 1000) * 1000000L;
				nanosleep(&ts, NULL);
			}
			uint32_t reply[2] = { 0u, (uint32_t)sizeof(fake_frame) };
			if (write_all(1, reply, sizeof(reply)) != 0)
				return 0;
			if (write_all(1, fake_frame, sizeof(fake_frame)) != 0)
				return 0;
			continue;
		}

		{
			/* 8 dummy OTA-header bytes (content irrelevant to these tests)
			 * followed by a 4-byte "codec frame" with a plausible in-band
			 * marker, so the bridge's strip_ota_header=true path (the
			 * default the real codec uses) has something left to return
			 * instead of silently dropping an under-length frame. */
			static const unsigned char fake_frame[12] = {
				0, 0, 0, 0, 0, 0, 0, 0xAD, 0x84, 0x00, 0xAA, 0x04 };
			uint32_t reply[2] = { 0u, (uint32_t)sizeof(fake_frame) };
			if (write_all(1, reply, sizeof(reply)) != 0)
				return 0;
			if (write_all(1, fake_frame, sizeof(fake_frame)) != 0)
				return 0;
		}
	}
}
