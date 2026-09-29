/* momentum4:
 * offline integration test for m4-adaptive-bridge.c's MODE == SOCKET path,
 * exercised against a REAL systemd socket-activation code path --
 * `systemd-socket-activate --accept --inetd` -- rather than a hand-rolled
 * accept()/dup2() stand-in. This is deliberately the same binary systemd
 * itself uses to implement `Accept=yes` + `StandardInput=socket` +
 * `StandardOutput=socket` unit semantics (see systemd-socket-activate(1)),
 * so a pass here is real evidence that m4-adaptive-bridge.c's
 * connect_socket()/try_spawn_and_handshake() interoperate correctly with
 * the exact fd-duplication model systemd/momentum4-helper.socket.in +
 * momentum4-helper@.service will use in production -- WITHOUT touching
 * the real systemd --user manager, ~/.config/systemd/user, or enabling any
 * persistent unit. Everything here is a plain child process bound to a
 * throwaway socket path in a temp directory, killed and cleaned up at exit.
 *
 * `--accept` makes systemd-socket-activate spawn a FRESH instance of the
 * given command per incoming connection (the actual behaviour of a
 * template `momentum4-helper@.service` unit with `Accept=yes` on its
 * paired `.socket`); `--inetd` dup2()s the accepted connection onto the
 * child's stdin AND stdout (the actual behaviour of
 * `StandardInput=socket`/`StandardOutput=socket`) instead of passing it as
 * fd 3 via $LISTEN_FDS. Confirmed empirically that BOTH
 * flags are required together to reproduce our unit's semantics -- with
 * neither, or with only one, the child gets the wrong fd/argv shape (see
 * below for the two failure modes observed: without --accept,
 * the child gets the bare *listening* socket instead of an accepted
 * connection; without --inetd, the connection arrives as fd 3 not
 * stdin/stdout).
 *
 * Reuses test/evil_helper.c (already speaks the exact wire protocol on
 * fd 0/1, see that file's header) as the stand-in "guest", exactly as
 * test/resilience_test.c does for SPAWN mode -- this keeps socket-mode
 * fault injection deterministic and fast, independent of real
 * qemu-hexagon timing, the same rationale resilience_test.c documents for
 * itself.
 *
 * Skips (prints a note, exit 0, not a failure) if `systemd-socket-activate`
 * is not on $PATH -- this is a systemd-provided debug tool, present on this
 * project's Debian 12 dev machine (systemd 252) but not guaranteed on every
 * build environment.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
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

static bool have_systemd_socket_activate(void)
{
	/* Cheap PATH search via access(), avoids depending on `which` being
	 * installed in a minimal CI image. */
	const char *path = getenv("PATH");
	char buf[4096];
	char *saveptr, *dir;

	if (!path)
		return false;
	snprintf(buf, sizeof(buf), "%s", path);
	for (dir = strtok_r(buf, ":", &saveptr); dir; dir = strtok_r(NULL, ":", &saveptr)) {
		char candidate[PATH_MAX];
		snprintf(candidate, sizeof(candidate), "%s/systemd-socket-activate", dir);
		if (access(candidate, X_OK) == 0)
			return true;
	}
	return false;
}

/* Spawns `systemd-socket-activate --accept --inetd -l sock_path -- helper_path
 * mode` in the background (fork(), no shell). Waits (bounded) for sock_path
 * to appear and actually be a socket before returning, since
 * systemd-socket-activate's bind()+listen() happens asynchronously relative
 * to our fork() returning. Returns the activator's pid (caller SIGKILLs +
 * waitpid()s it at teardown; the accepted per-connection helper instances
 * are separate grandchild processes systemd-socket-activate itself reaps).
 * Returns -1 on failure (activator never bound the socket within the
 * timeout). */
static pid_t start_activator(const char *sock_path, const char *helper_path, const char *mode)
{
	pid_t pid = fork();
	if (pid < 0)
		return -1;
	if (pid == 0) {
		/* Child: quiet down systemd-socket-activate's own stderr chatter
		 * ("Listening on ...", "Connection from ...") so test output stays
		 * readable -- none of it is needed for this test's own PASS/FAIL
		 * logic, only the wire-level exchange over the socket is. */
		int devnull = open("/dev/null", O_WRONLY);
		if (devnull >= 0) {
			dup2(devnull, STDERR_FILENO);
			close(devnull);
		}
		/* evil_helper.c picks its behaviour from $EVIL_HELPER_MODE first,
		 * falling back to argv[2] only "for convenient manual/standalone
		 * invocation" (see that file's header) -- argv[2] is not usable
		 * here because systemd-socket-activate execs the target command
		 * with only ONE fixed argv (helper_path becomes argv[0], no room
		 * for a mode argv[1]/[2] the way resilience_test.c's SPAWN-mode
		 * `execl(qemu, qemu, loader, capi, NULL)` has one). Confirmed
		 * empirically that systemd-socket-activate does
		 * NOT pass its own inherited environment through to the spawned
		 * child (it starts the child from a clean environment, the same
		 * way a real systemd service unit would) -- a plain setenv() in
		 * this forked child before exec has no effect on what
		 * systemd-socket-activate's own child (evil_helper) sees.
		 * `--setenv=NAME=VALUE` is the tool's own sanctioned way to inject
		 * a variable into that clean environment (see
		 * systemd-socket-activate --help). */
		char setenv_arg[256];
		snprintf(setenv_arg, sizeof(setenv_arg), "--setenv=EVIL_HELPER_MODE=%s", mode);
		execlp("systemd-socket-activate", "systemd-socket-activate",
				"--accept", "--inetd", setenv_arg, "-l", sock_path, "--",
				helper_path, (char *)NULL);
		_exit(127);
	}

	double deadline = now_ms() + 3000.0;
	struct stat st;
	while (now_ms() < deadline) {
		if (stat(sock_path, &st) == 0 && S_ISSOCK(st.st_mode))
			return pid;
		struct timespec ts = { .tv_sec = 0, .tv_nsec = 10 * 1000 * 1000 };
		nanosleep(&ts, NULL);
	}
	kill(pid, SIGKILL);
	waitpid(pid, NULL, 0);
	return -1;
}

static void stop_activator(pid_t pid)
{
	if (pid <= 0)
		return;
	kill(pid, SIGKILL);
	waitpid(pid, NULL, 0);
}

static void fill_dummy_pcm(uint8_t *pcm)
{
	memset(pcm, 0, M4_AH_PCM_BLOCK_BYTES);
}

static void scenario_available_probe(const char *sock_dir)
{
	printf("\n-- scenario: m4_ah_bridge_socket_available() probe (no connect) --\n");
	char missing[PATH_MAX], regular_file[PATH_MAX];
	snprintf(missing, sizeof(missing), "%s/does-not-exist.sock", sock_dir);
	snprintf(regular_file, sizeof(regular_file), "%s/not-a-socket.txt", sock_dir);

	CHECK(!m4_ah_bridge_socket_available(NULL), "NULL path -> false");
	CHECK(!m4_ah_bridge_socket_available(""), "empty path -> false");
	CHECK(!m4_ah_bridge_socket_available(missing), "nonexistent path -> false");

	FILE *f = fopen(regular_file, "w");
	CHECK(f != NULL, "(setup) created a plain regular file");
	if (f) {
		fputs("not a socket\n", f);
		fclose(f);
	}
	CHECK(!m4_ah_bridge_socket_available(regular_file),
			"regular file at the path (not a socket) -> false, not just \"exists\"");
	unlink(regular_file);
}

static void scenario_normal_roundtrip(const char *sock_path, const char *evil, const char *dummy_loader)
{
	printf("\n-- scenario: real systemd-socket-activate + evil_helper 'normal', full round trip --\n");
	pid_t activator = start_activator(sock_path, evil, "normal");
	CHECK(activator > 0, "systemd-socket-activate bound the socket within budget");
	if (activator <= 0)
		return;

	CHECK(m4_ah_bridge_socket_available(sock_path),
			"m4_ah_bridge_socket_available() sees the real listening socket");

	struct m4_ah_bridge_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.mode = M4_AH_HELPER_MODE_SOCKET;
	cfg.socket_path = sock_path;
	cfg.strip_ota_header = true;
	cfg.helper_timeout_ms = 500;
	cfg.connect_timeout_ms = 500;
	cfg.startup_timeout_ms = 2000;
	(void)dummy_loader;

	struct m4_ah_bridge *b = m4_ah_bridge_start(NULL, &cfg);
	CHECK(b != NULL, "bridge connects + completes the startup handshake over the socket");
	if (b) {
		CHECK(m4_ah_bridge_test_child_pid(b) == -1,
				"test_child_pid() is -1 in socket mode (no local child)");
		int ok_frames = 0;
		for (int i = 0; i < 20; i++) {
			uint8_t pcm[M4_AH_PCM_BLOCK_BYTES];
			fill_dummy_pcm(pcm);
			m4_ah_bridge_submit_pcm(b, pcm, sizeof(pcm));
			struct timespec ts = { .tv_sec = 0, .tv_nsec = 15 * 1000 * 1000 };
			nanosleep(&ts, NULL);
			uint8_t frame[M4_AH_MAX_FRAME_BYTES];
			size_t n = m4_ah_bridge_take_frame(b, frame, sizeof(frame));
			/* evil_helper.c's fake_frame is 12 bytes
			 * {0,0,0,0,0,0,0,0xAD,0x84,0x00,0xAA,0x04}; stripping the first
			 * M4_AH_OTA_HEADER_BYTES==8 bytes (cfg.strip_ota_header=true)
			 * leaves the last 4: {0x84,0x00,0xAA,0x04}. */
			if (n == 4 && frame[0] == 0x84 && frame[1] == 0x00 && frame[2] == 0xAA && frame[3] == 0x04)
				ok_frames++;
		}
		CHECK(ok_frames >= 18, "at least 18/20 blocks round-tripped a real frame over the socket (got %d)", ok_frames);
		CHECK(m4_ah_bridge_is_alive(b), "bridge still alive after 20 blocks");
		m4_ah_bridge_stop(b);
	}
	stop_activator(activator);
}

static void scenario_no_startup_bounded_timeout(const char *sock_path, const char *evil)
{
	printf("\n-- scenario: server accepts the connection but never sends the startup "
			"handshake reply (bounded fail-closed) --\n");
	/* Note: evil_helper.c's "hang" mode DOES send the startup reply
	 * immediately and only hangs on the first block request afterwards (see
	 * that file's header) -- "no-startup" is the one that never writes
	 * anything at all, i.e. the actual startup-handshake-timeout scenario. */
	pid_t activator = start_activator(sock_path, evil, "no-startup");
	CHECK(activator > 0, "systemd-socket-activate bound the socket (no-startup scenario)");
	if (activator <= 0)
		return;

	struct m4_ah_bridge_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.mode = M4_AH_HELPER_MODE_SOCKET;
	cfg.socket_path = sock_path;
	cfg.connect_timeout_ms = 500;
	cfg.startup_timeout_ms = 300; /* small on purpose: bound this test's wall time */
	cfg.helper_timeout_ms = 300;

	double t0 = now_ms();
	struct m4_ah_bridge *b = m4_ah_bridge_start(NULL, &cfg);
	double elapsed = now_ms() - t0;
	CHECK(b == NULL, "bridge_start() fails closed when the server never answers");
	CHECK(elapsed < 1000.0, "bridge_start() returned within a bounded time (%.0f ms, budget 300 ms)", elapsed);
	if (b)
		m4_ah_bridge_stop(b);
	stop_activator(activator);
}

static void scenario_missing_socket(const char *sock_dir)
{
	printf("\n-- scenario: nothing listening at all (no activator started) --\n");
	char sock_path[PATH_MAX];
	snprintf(sock_path, sizeof(sock_path), "%s/nobody-home.sock", sock_dir);

	struct m4_ah_bridge_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.mode = M4_AH_HELPER_MODE_SOCKET;
	cfg.socket_path = sock_path;
	cfg.connect_timeout_ms = 300;
	cfg.startup_timeout_ms = 300;

	double t0 = now_ms();
	struct m4_ah_bridge *b = m4_ah_bridge_start(NULL, &cfg);
	double elapsed = now_ms() - t0;
	CHECK(b == NULL, "bridge_start() fails closed when nothing is listening");
	CHECK(elapsed < 1000.0, "fails fast (%.0f ms), does not wait out the full budget "
			"(m4_ah_bridge_socket_available() pre-check in m4_ah_bridge_start() "
			"rejects it before ever calling connect())", elapsed);
	if (b)
		m4_ah_bridge_stop(b);
}

static void scenario_server_death_auto_restart(const char *sock_path, const char *evil)
{
	printf("\n-- scenario: server instance dies mid-stream, auto_restart reconnects "
			"to a FRESH server instance (real per-connection respawn) --\n");
	pid_t activator = start_activator(sock_path, evil, "die-1");
	CHECK(activator > 0, "systemd-socket-activate bound the socket (die-1 scenario)");
	if (activator <= 0)
		return;

	struct m4_ah_bridge_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.mode = M4_AH_HELPER_MODE_SOCKET;
	cfg.socket_path = sock_path;
	cfg.helper_timeout_ms = 300;
	cfg.connect_timeout_ms = 300;
	cfg.startup_timeout_ms = 1000;
	cfg.auto_restart = true;
	cfg.strip_ota_header = true;

	struct m4_ah_bridge *b = m4_ah_bridge_start(NULL, &cfg);
	CHECK(b != NULL, "bridge connects to the first server instance");
	if (!b)
		goto out;

	{
		/* die-1 answers block 1 normally, then exit(1)s before block 2 --
		 * see evil_helper.c. Mirrors test/resilience_test.c's own die-1
		 * scenarios exactly: submit block 1, sleep, and DRAIN it with
		 * take_frame() before submitting block 2 -- this is what
		 * guarantees block 1 has actually round-tripped through the IO
		 * thread (single-slot mailbox, see m4-adaptive-bridge.h) before
		 * block 2 is handed to it, rather than racing block 2 into
		 * overwriting a not-yet-consumed block 1 in the input mailbox. */
		uint8_t pcm[M4_AH_PCM_BLOCK_BYTES];
		uint8_t frame0[M4_AH_MAX_FRAME_BYTES];
		fill_dummy_pcm(pcm);

		m4_ah_bridge_submit_pcm(b, pcm, sizeof(pcm)); /* block 1: answered normally */
		{
			struct timespec ts = { .tv_sec = 0, .tv_nsec = 100 * 1000 * 1000 };
			nanosleep(&ts, NULL);
		}
		CHECK(m4_ah_bridge_take_frame(b, frame0, sizeof(frame0)) > 0,
				"first block is answered before the server instance dies");

		m4_ah_bridge_submit_pcm(b, pcm, sizeof(pcm)); /* block 2: this instance exits (die-1) */

		double deadline = now_ms() + 5000.0;
		while (m4_ah_bridge_is_alive(b) && now_ms() < deadline) {
			struct timespec ts = { .tv_sec = 0, .tv_nsec = 50 * 1000 * 1000 };
			nanosleep(&ts, NULL);
		}
		CHECK(!m4_ah_bridge_is_alive(b), "bridge notices the server instance died");

		/* The auto-restart backoff will call connect_socket() again;
		 * systemd-socket-activate (--accept) spawns a BRAND NEW evil_helper
		 * instance for that new connection -- this is the real
		 * per-connection instantiation model of `Accept=yes`, not a mock. */
		deadline = now_ms() + 15000.0;
		while (!m4_ah_bridge_is_alive(b) && now_ms() < deadline) {
			struct timespec ts = { .tv_sec = 0, .tv_nsec = 100 * 1000 * 1000 };
			nanosleep(&ts, NULL);
		}
		CHECK(m4_ah_bridge_is_alive(b),
				"bridge auto-reconnects to a fresh server instance within 15 s");
		CHECK(m4_ah_bridge_restart_count(b) >= 1,
				"restart_count() reflects at least one reconnect (got %u)",
				m4_ah_bridge_restart_count(b));

		if (m4_ah_bridge_is_alive(b)) {
			uint8_t frame[M4_AH_MAX_FRAME_BYTES];
			m4_ah_bridge_submit_pcm(b, pcm, sizeof(pcm));
			struct timespec ts = { .tv_sec = 0, .tv_nsec = 100 * 1000 * 1000 };
			nanosleep(&ts, NULL);
			size_t n = m4_ah_bridge_take_frame(b, frame, sizeof(frame));
			CHECK(n == 4, "the reconnected instance produces real frames again (got %zu bytes)", n);
		}
	}

	m4_ah_bridge_stop(b);
out:
	stop_activator(activator);
}

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: %s <evil_helper-path> <dummy-file-path>\n", argv[0]);
		return 2;
	}
	const char *evil = argv[1];
	const char *dummy = argv[2];

	if (!have_systemd_socket_activate()) {
		printf("systemd-socket-activate not found on $PATH -- skipping "
				"socket_bridge_test (see file header: this is a systemd "
				"debug tool, present on the project dev machine but not "
				"guaranteed everywhere). Not a failure.\n");
		return 0;
	}

	char sock_dir[] = "/tmp/m4-socket-bridge-test-XXXXXX";
	if (mkdtemp(sock_dir) == NULL) {
		fprintf(stderr, "mkdtemp failed: %m\n");
		return 1;
	}
	/* AF_UNIX sun_path is capped at ~108 bytes -- keep this short and under
	 * /tmp rather than the project's own (potentially long) scratchpad path. */
	char sock_path[PATH_MAX];
	snprintf(sock_path, sizeof(sock_path), "%s/helper.sock", sock_dir);

	scenario_available_probe(sock_dir);
	scenario_missing_socket(sock_dir);
	scenario_normal_roundtrip(sock_path, evil, dummy);
	unlink(sock_path);
	scenario_no_startup_bounded_timeout(sock_path, evil);
	unlink(sock_path);
	scenario_server_death_auto_restart(sock_path, evil);
	unlink(sock_path);

	rmdir(sock_dir);

	printf("\n%d check(s) failed\n", g_failures);
	return g_failures == 0 ? 0 : 1;
}
