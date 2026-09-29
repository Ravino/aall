/* momentum4: standalone MODE == SOCKET client
 * probe, deliberately factored OUT of socket_bridge_test.c so it can be run
 * as its OWN process under a `systemd-run --user -p MemoryDenyWriteExecute=yes`
 * sandbox while the "server" side (systemd-socket-activate + evil_helper, or
 * in principle real qemu-hexagon) runs OUTSIDE that sandbox in a separate,
 * unsandboxed process tree.
 *
 * This is the actual point of the chosen design,
 * "variant 2": WirePlumber keeps MemoryDenyWriteExecute=yes (the hardening
 * that broke qemu-hexagon's TCG code buffer in-process,
 * as found during debugging), while the client-side bridge code -- connect(), a
 * pthread IO thread, two mailboxes, an eventfd -- has NO mmap(PROT_EXEC)/
 * mprotect(PROT_EXEC) dependency of its own and should therefore work
 * completely unaffected under that same seccomp filter. This probe proves
 * exactly that claim in isolation, rather than re-deriving it by inference
 * from "the whole redesign moves qemu out of the sandboxed process" (true,
 * but not the same as directly observing the client succeed under MDWE=yes).
 *
 * Usage: socket_bridge_client_probe <socket_path> [n_blocks]
 * Exit 0 on success (connects, completes the handshake, round-trips at
 * least one real frame), 1 on any failure. Prints one line per check to
 * stdout in the same PASS:/FAIL: style as the other tests here, so a
 * wrapping shell script can just grep for "FAIL" or check the exit code.
 */
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

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s <socket_path> [n_blocks]\n", argv[0]);
		return 1;
	}
	const char *sock_path = argv[1];
	int n_blocks = argc > 2 ? atoi(argv[2]) : 10;

	printf("client probe pid=%d running against socket=%s (checking this process's "
			"own sandboxing has no bearing on the bridge's connect()/pthread/eventfd "
			"calls, which need no PROT_EXEC mappings of their own)\n", (int)getpid(), sock_path);

	CHECK(m4_ah_bridge_socket_available(sock_path),
			"m4_ah_bridge_socket_available(): stat() on the socket path works "
			"under this process's sandbox");

	struct m4_ah_bridge_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.mode = M4_AH_HELPER_MODE_SOCKET;
	cfg.socket_path = sock_path;
	cfg.strip_ota_header = true;
	cfg.connect_timeout_ms = 1000;
	cfg.startup_timeout_ms = 2000;
	cfg.helper_timeout_ms = 500;

	struct m4_ah_bridge *b = m4_ah_bridge_start(NULL, &cfg);
	CHECK(b != NULL, "connect(AF_UNIX) + pthread_create() + startup handshake "
			"all succeed under this process's own sandbox");
	if (b) {
		int ok = 0;
		uint8_t pcm[M4_AH_PCM_BLOCK_BYTES];
		memset(pcm, 0, sizeof(pcm));
		for (int i = 0; i < n_blocks; i++) {
			m4_ah_bridge_submit_pcm(b, pcm, sizeof(pcm));
			struct timespec ts = { .tv_sec = 0, .tv_nsec = 15 * 1000 * 1000 };
			nanosleep(&ts, NULL);
			uint8_t frame[M4_AH_MAX_FRAME_BYTES];
			if (m4_ah_bridge_take_frame(b, frame, sizeof(frame)) > 0)
				ok++;
		}
		CHECK(ok >= n_blocks - 2, "at least %d/%d PCM blocks round-tripped a real "
				"frame while this process ran under its own sandbox (got %d)",
				n_blocks - 2, n_blocks, ok);
		m4_ah_bridge_stop(b);
	}

	printf("%d check(s) failed\n", g_failures);
	return g_failures == 0 ? 0 : 1;
}
