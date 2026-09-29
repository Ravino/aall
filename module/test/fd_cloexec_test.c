/* momentum4: "explicit FD_CLOEXEC and closing
 * extra fds in the child before exec'ing qemu (close_range)").
 *
 * Verifies experimentally (not just by reading the code) that
 * m4-adaptive-bridge.c's spawn_child() no longer leaks arbitrary open file
 * descriptors from the parent (WirePlumber, in production) into the
 * qemu-hexagon child. Method: the parent opens a handful of "marker" fds at
 * fixed, distinctive numbers (deliberately NOT close-on-exec, to simulate
 * whatever WirePlumber/glib/D-Bus fds might already be open and NOT
 * O_CLOEXEC -- the pessimistic case), then starts a bridge whose "qemu_path"
 * is actually a tiny shell script that dumps its own /proc/self/fd listing
 * to a known file before doing anything else. The test then greps that
 * listing for the marker fd numbers: none of them must appear.
 *
 * Does not depend on real qemu-hexagon/CAPI/loader binaries -- loader_path/
 * capi_path just need to be *some* real, readable file (m4_ah_bridge_start()
 * access()-checks them before ever exec'ing); their content is irrelevant
 * here, so this test's own source file is reused for both.
 *
 * Usage: fd_cloexec_test (no arguments). Exit 0 = pass.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "m4-adaptive-bridge.h"

static int g_failures = 0;

#define CHECK(cond, ...) do { \
	if (!(cond)) { \
		printf("FAIL: "); printf(__VA_ARGS__); printf("  (%s:%d)\n", __FILE__, __LINE__); \
		g_failures++; \
	} else { \
		printf("PASS: "); printf(__VA_ARGS__); printf("\n"); \
	} \
} while (0)

/* Fixed, deliberately-distinctive fd numbers to look for in the child's
 * /proc/self/fd listing. Chosen high enough to not collide with whatever
 * stdio/library fds this small test binary itself happens to have open
 * (checked at runtime below, not just assumed). */
static const int MARKER_FDS[] = { 61, 62, 63 };
#define N_MARKERS (int)(sizeof(MARKER_FDS) / sizeof(MARKER_FDS[0]))

static const char *FD_DUMP_PATH = "/tmp/m4_fd_cloexec_test_dump.txt";
static const char *QEMU_SCRIPT_PATH = "/tmp/m4_fd_cloexec_test_qemu.sh";

int main(void)
{
	int i;

	unlink(FD_DUMP_PATH);

	/* Write the fake "qemu-hexagon" as a shell script: dump /proc/self/fd,
	 * then just exit (no PROTOCOL.h handshake reply -- bridge_start() will
	 * time out and return NULL, which is fine, we only care about what the
	 * child did before exiting). */
	{
		FILE *f = fopen(QEMU_SCRIPT_PATH, "w");
		CHECK(f != NULL, "wrote fake qemu-hexagon script");
		if (!f)
			return 1;
		/* -1: one bare fd-number-or-name per line, no permissions/owner/date
		 * columns to worry about when matching below. */
		fprintf(f, "#!/bin/sh\nls -1 /proc/self/fd > '%s' 2>&1\nexit 0\n", FD_DUMP_PATH);
		fclose(f);
		chmod(QEMU_SCRIPT_PATH, 0755);
	}

	/* Open marker fds at fixed numbers, WITHOUT O_CLOEXEC -- the
	 * pessimistic case this defense-in-depth fix targets (modern
	 * glib/pipewire usually create sockets with SOCK_CLOEXEC
	 * by default, so a mass leak is unlikely, but this was not verified
	 * experimentally -- this test removes that
	 * "probably" for the one thing we control: our own spawn_child()). */
	for (i = 0; i < N_MARKERS; i++) {
		int fd = open("/dev/null", O_RDONLY);
		CHECK(fd >= 0, "opened marker fd source #%d", i);
		if (fd < 0)
			return 1;
		if (fd != MARKER_FDS[i]) {
			if (dup2(fd, MARKER_FDS[i]) < 0) {
				printf("FAIL: dup2(%d -> %d): %s (fd already in use by this test "
						"binary itself? try different MARKER_FDS)\n",
						fd, MARKER_FDS[i], strerror(errno));
				return 1;
			}
			close(fd);
		}
		/* Explicitly clear FD_CLOEXEC in case dup2's target fd inherited one
		 * from something -- we want these fds definitely NOT close-on-exec
		 * before spawn_child() runs, so a pass here is meaningful. */
		fcntl(MARKER_FDS[i], F_SETFD, 0);
	}
	CHECK(1, "opened %d marker fds at fixed numbers %d..%d, all FD_CLOEXEC cleared",
			N_MARKERS, MARKER_FDS[0], MARKER_FDS[N_MARKERS - 1]);

	struct m4_ah_bridge_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.qemu_path = QEMU_SCRIPT_PATH;
	cfg.loader_path = __FILE__;  /* any real, readable file */
	cfg.capi_path = __FILE__;    /* any real, readable file */
	cfg.startup_timeout_ms = 500; /* script never replies; fail fast */

	struct m4_ah_bridge *b = m4_ah_bridge_start(NULL, &cfg);
	/* Expected to be NULL (script never sends the startup handshake) --
	 * that is not what this test checks. If a real qemu-hexagon happened to
	 * be at QEMU_SCRIPT_PATH (impossible, it is a path we just wrote a shell
	 * script to) this would be surprising, but either way we only care
	 * about the fd dump file below. */
	if (b)
		m4_ah_bridge_stop(b);

	/* Give the child a moment to run+exit and flush the dump file (it is a
	 * synchronous shell script, should be near-instant, but avoid any race
	 * against the filesystem). */
	for (i = 0; i < 20 && access(FD_DUMP_PATH, F_OK) != 0; i++)
		usleep(20 * 1000);

	FILE *dump = fopen(FD_DUMP_PATH, "r");
	CHECK(dump != NULL, "fake qemu-hexagon child ran and produced its fd dump");
	if (!dump) {
		printf("FAIL: cannot proceed without the fd dump -- was the child ever exec'd?\n");
		return 1;
	}

	char line[512];
	char all_lines[16384];
	size_t used = 0;
	while (fgets(line, sizeof(line), dump)) {
		size_t l = strlen(line);
		if (used + l < sizeof(all_lines)) {
			memcpy(all_lines + used, line, l);
			used += l;
		}
	}
	all_lines[used] = '\0';
	fclose(dump);

	printf("INFO: child's /proc/self/fd listing (%zu bytes):\n%s\n", used, all_lines);

	/* `ls -1` output: one bare fd number per line (no path prefix). Match a
	 * WHOLE line equal to the marker number, not a numeric substring (e.g.
	 * marker "6" must not "match" a line "61" by substring luck). */
	for (i = 0; i < N_MARKERS; i++) {
		char needle[32];
		int found = 0;
		const char *p = all_lines;
		snprintf(needle, sizeof(needle), "%d", MARKER_FDS[i]);
		size_t needle_len = strlen(needle);
		while ((p = strstr(p, needle)) != NULL) {
			bool at_line_start = (p == all_lines) || p[-1] == '\n';
			char after = p[needle_len];
			bool at_line_end = (after == '\n' || after == '\0');
			if (at_line_start && at_line_end) {
				found = 1;
				break;
			}
			p += needle_len;
		}
		CHECK(!found, "marker fd %d (parent-side, non-CLOEXEC) is NOT present in the "
				"qemu-hexagon child's fd table", MARKER_FDS[i]);
	}

	/* Sanity: the child DID have some fds (0/1/2 at least) -- makes sure the
	 * dump file is not just empty/garbled and the "not found" checks above
	 * are actually meaningful. */
	CHECK(strstr(all_lines, "0\n") != NULL || strstr(all_lines, "1\n") != NULL,
			"sanity: fd dump lists at least the child's own stdio fds");

	for (i = 0; i < N_MARKERS; i++)
		close(MARKER_FDS[i]);
	unlink(FD_DUMP_PATH);
	unlink(QEMU_SCRIPT_PATH);

	printf("\n%d check(s) failed\n", g_failures);
	return g_failures == 0 ? 0 : 1;
}
