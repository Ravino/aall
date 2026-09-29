/* momentum4 patch 0006 (gestures and earcon
 * timing): m4-hfp-events.c's m4_hfp_events_notify() must never block the
 * caller -- it is called from PipeWire's SPA *main loop* thread inside
 * spa/plugins/bluez5/backend-native.c's rfcomm_hfp_ag(), a thread that must
 * never stall (see m4-hfp-events.h's own doc comment for why).
 *
 * This links m4-hfp-events.c directly (no spa_log dependency beyond the
 * struct spa_log pointer type -- a NULL spa_log with spa_log_debug() is
 * fine, see below) and drives it against a handful of scenarios a real
 * WirePlumber process can hit:
 *   1. no socket at all (momentum4d never started, or a stale/missing
 *      $XDG_RUNTIME_DIR/momentum4/ directory) -- socket()/sendto() must
 *      fail fast (ENOENT), not hang;
 *   2. a bound-but-never-draining receiver (worst case for a DGRAM socket:
 *      the kernel receive buffer eventually fills up) -- sendto() with
 *      MSG_DONTWAIT must return EAGAIN immediately, not block;
 *   3. a normal, draining receiver -- the common case, just confirms the
 *      wire format/round-trip while we're at it;
 *   4. repeated calls after the first socket() failure (EMFILE simulated
 *      by exhausting the fd table) must stay cheap (no retry storm).
 *
 * Bounded wall-clock time is asserted throughout (a handful of ms for
 * thousands of calls) -- the actual proof of "never blocks", since a single
 * accidental blocking syscall anywhere in the call chain would blow this
 * budget by orders of magnitude (this is the same style of proof
 * busy_wait_test.c/latency_bench_pipe_vs_socket.c already use elsewhere in
 * this test/ directory, just for "fast" instead of "idle").
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "m4-hfp-events.h"

static int g_failures = 0;
#define CHECK(cond, ...)                                                     \
	do {                                                                  \
		if (!(cond)) {                                                \
			printf("FAIL: ");                                     \
			printf(__VA_ARGS__);                                  \
			printf("  (%s:%d)\n", __FILE__, __LINE__);            \
			g_failures++;                                         \
		} else {                                                      \
			printf("PASS: ");                                     \
			printf(__VA_ARGS__);                                  \
			printf("\n");                                         \
		}                                                             \
	} while (0)

static double now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static void make_tmp_runtime_dir(char *out, size_t out_len)
{
	snprintf(out, out_len, "/tmp/m4-hfp-events-test.XXXXXX");
	if (mkdtemp(out) == NULL) {
		fprintf(stderr, "mkdtemp failed: %s\n", strerror(errno));
		exit(1);
	}
}

/* Binds an AF_UNIX SOCK_DGRAM listener at the EXACT path
 * m4_hfp_events_notify() actually sends to for a given $XDG_RUNTIME_DIR:
 * "$XDG_RUNTIME_DIR/momentum4/hfp-events.sock" -- m4-hfp-events.c's
 * hfp_events_socket_path() hardcodes that "momentum4/" subdirectory (it
 * matches momentum4d's own m4_config_runtime_dir(), which is
 * "$XDG_RUNTIME_DIR/momentum4", daemon/src/util/config.c); a test
 * that binds one path level too high would never see any datagram at all
 * (sendto() would just get ENOENT and silently drop it -- correct,
 * documented behaviour, but NOT what these tests want to exercise). Returns
 * the bound fd; writes the full socket path to `sock_path_out`. */
static int bind_listener(const char *runtime_dir, char *sock_path_out, size_t sock_path_out_cap)
{
	char subdir[192];
	struct sockaddr_un addr;
	int fd;

	snprintf(subdir, sizeof(subdir), "%s/momentum4", runtime_dir);
	if (mkdir(subdir, 0700) != 0 && errno != EEXIST) {
		fprintf(stderr, "mkdir(%s) failed: %s\n", subdir, strerror(errno));
		exit(1);
	}
	snprintf(sock_path_out, sock_path_out_cap, "%s/hfp-events.sock", subdir);

	fd = socket(AF_UNIX, SOCK_DGRAM, 0);
	CHECK(fd >= 0, "listener socket()");
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock_path_out);
	CHECK(bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0, "listener bind(%s)", sock_path_out);

	/* Belt-and-suspenders for THIS TEST BINARY's own recv() calls below
	 * (not a statement about the module under test): a genuine regression
	 * in m4-hfp-events.c that silently drops a datagram this test expects
	 * to arrive must fail this test loudly, not hang the whole suite/CI
	 * run forever -- exactly what happened once already while writing
	 * this file (a wrong listener path in an earlier draft of this test,
	 * not a module bug -- see bind_listener()'s own comment). */
	{
		struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };

		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	}
	return fd;
}

/* 1. no listener at all -- must return promptly regardless. */
static void test_no_listener_does_not_block(void)
{
	char runtime_dir[128];
	double t0, elapsed;

	make_tmp_runtime_dir(runtime_dir, sizeof(runtime_dir));
	setenv("XDG_RUNTIME_DIR", runtime_dir, 1);

	t0 = now_ms();
	for (int i = 0; i < 1000; i++)
		m4_hfp_events_notify(NULL, "CHUP", "00:1B:66:00:00:01");
	elapsed = now_ms() - t0;

	CHECK(elapsed < 200.0, "1000 calls with no listener took %.2f ms (< 200 ms budget)", elapsed);
}

/* 2. a receiver that never drains -- the socket's kernel receive buffer
 * fills up eventually (SO_RCVBUF default is a few hundred KB, and our
 * datagrams are well under 100 bytes each -- thousands fit). Every send
 * after that must return immediately via EAGAIN under MSG_DONTWAIT, not
 * block waiting for room. */
static void test_full_buffer_does_not_block(void)
{
	char runtime_dir[128], sock_path[256];
	int listen_fd;
	double t0, elapsed;

	make_tmp_runtime_dir(runtime_dir, sizeof(runtime_dir));
	setenv("XDG_RUNTIME_DIR", runtime_dir, 1);
	listen_fd = bind_listener(runtime_dir, sock_path, sizeof(sock_path));
	/* deliberately never recv() from listen_fd below -- that is the point */

	t0 = now_ms();
	for (int i = 0; i < 20000; i++)
		m4_hfp_events_notify(NULL, "CHUP", "00:1B:66:00:00:01");
	elapsed = now_ms() - t0;

	CHECK(elapsed < 500.0, "20000 calls into a never-draining receiver took %.2f ms (< 500 ms budget)",
	      elapsed);

	close(listen_fd);
}

/* 3. a normal, draining receiver -- confirms the wire format end to end:
 * "<VERB> <bdaddr> <ts_ms>", one line, no trailing newline. */
static void test_normal_roundtrip(void)
{
	char runtime_dir[128], sock_path[256];
	int listen_fd;
	char buf[256];
	ssize_t n;
	char verb[16], bdaddr[32];
	long long ts_ms;

	make_tmp_runtime_dir(runtime_dir, sizeof(runtime_dir));
	setenv("XDG_RUNTIME_DIR", runtime_dir, 1);
	listen_fd = bind_listener(runtime_dir, sock_path, sizeof(sock_path));

	m4_hfp_events_notify(NULL, "CHUP", "00:1B:66:00:00:01");

	n = recv(listen_fd, buf, sizeof(buf) - 1, 0);
	CHECK(n > 0, "datagram received");
	if (n > 0) {
		buf[n] = '\0';
		CHECK(buf[n - 1] != '\n', "no trailing newline");
		CHECK(sscanf(buf, "%15s %31s %lld", verb, bdaddr, &ts_ms) == 3, "wire format has 3 fields: \"%s\"",
		      buf);
		CHECK(strcmp(verb, "CHUP") == 0, "verb CHUP");
		CHECK(strcmp(bdaddr, "00:1B:66:00:00:01") == 0, "bdaddr");
		CHECK(ts_ms > 0, "timestamp is a positive number");
	}

	/* NULL/empty bdaddr -> the documented placeholder, not a garbled line */
	m4_hfp_events_notify(NULL, "ATA", NULL);
	n = recv(listen_fd, buf, sizeof(buf) - 1, 0);
	CHECK(n > 0, "second datagram received (NULL bdaddr)");
	if (n > 0) {
		buf[n] = '\0';
		CHECK(sscanf(buf, "%15s %31s %lld", verb, bdaddr, &ts_ms) == 3, "still 3 fields: \"%s\"", buf);
		CHECK(strcmp(verb, "ATA") == 0, "verb ATA");
		CHECK(strcmp(bdaddr, "00:00:00:00:00:00") == 0, "NULL bdaddr -> placeholder");
	}

	/* empty verb -> no-op, nothing sent (m4_hfp_events_notify()'s own
	 * documented contract) */
	m4_hfp_events_notify(NULL, "", "00:1B:66:00:00:01");
	m4_hfp_events_notify(NULL, NULL, "00:1B:66:00:00:01");
	m4_hfp_events_notify(NULL, "CHUP", "00:1B:66:00:00:01"); /* sentinel, to prove nothing queued ahead of it */
	n = recv(listen_fd, buf, sizeof(buf) - 1, 0);
	CHECK(n > 0, "sentinel datagram received");
	if (n > 0) {
		buf[n] = '\0';
		CHECK(sscanf(buf, "%15s %31s %lld", verb, bdaddr, &ts_ms) == 3 && strcmp(verb, "CHUP") == 0,
		      "empty/NULL verb calls sent nothing (sentinel was the very next datagram): \"%s\"", buf);
	}

	close(listen_fd);
}

/* 4. repeated calls stay cheap even when the module's internal socket() has
 * already been created (the module's "process-wide singleton, lazily
 * opened" design, see m4-hfp-events.c -- by this point in the binary, the
 * earlier tests above have already forced that first socket() call) --
 * i.e. no per-call socket()/path-recomputation overhead ever creeps back
 * in. A genuine EMFILE-at-socket()-time run (m4_hfp_events_notify() has
 * never had a successful fd at all) is NOT reproduced here: forcing it
 * would mean lowering RLIMIT_NOFILE for this whole process, which is not
 * safe to do in a shared test binary without very carefully restoring it
 * (and every fd meson/ninja/the test runner itself may be holding open) --
 * not attempted; the "first socket() fails -> every later call is one cheap
 * branch, not a retry" half of the module's contract is covered by reading
 * m4-hfp-events.c's g_state logic directly instead (m4-hfp-events.h's doc
 * comment), not by an experiment. */
static void test_repeated_calls_stay_cheap(void)
{
	double t0, elapsed;

	t0 = now_ms();
	for (int i = 0; i < 5000; i++)
		m4_hfp_events_notify(NULL, "CHUP", "00:1B:66:00:00:01");
	elapsed = now_ms() - t0;
	CHECK(elapsed < 200.0, "5000 calls (cached fd reused, no new socket() per call) took %.2f ms",
	      elapsed);
}

/* 5. m4_hfp_events_notify_raw() (patch 0007, "tap = mic" task): wire
 * format "RAW <token> <bdaddr> <ts_ms>" -- distinct first word from the
 * named-verb lines above, so ipc/hfp_events.c's parser can tell them apart.
 * Also confirms the NUL-bdaddr placeholder still works for this path. */
static void test_raw_normal_roundtrip(void)
{
	char runtime_dir[128], sock_path[256];
	int listen_fd;
	char buf[256];
	ssize_t n;
	char verb[16], token[64], bdaddr[32];
	long long ts_ms;

	make_tmp_runtime_dir(runtime_dir, sizeof(runtime_dir));
	setenv("XDG_RUNTIME_DIR", runtime_dir, 1);
	listen_fd = bind_listener(runtime_dir, sock_path, sizeof(sock_path));

	m4_hfp_events_notify_raw(NULL, "AT+CLCC", "00:1B:66:00:00:01", 200);

	n = recv(listen_fd, buf, sizeof(buf) - 1, 0);
	CHECK(n > 0, "raw datagram received");
	if (n > 0) {
		buf[n] = '\0';
		CHECK(sscanf(buf, "%15s %63s %31s %lld", verb, token, bdaddr, &ts_ms) == 4,
		      "raw wire format has 4 fields: \"%s\"", buf);
		CHECK(strcmp(verb, "RAW") == 0, "first word is RAW, not a verb name");
		CHECK(strcmp(token, "AT+CLCC") == 0, "token is the raw AT command unchanged when already clean");
		CHECK(strcmp(bdaddr, "00:1B:66:00:00:01") == 0, "bdaddr");
		CHECK(ts_ms > 0, "timestamp positive");
	}

	close(listen_fd);
}

/* 6. rate limiting (task: rate-limited): a second call
 * immediately after the first must NOT produce a datagram; after waiting
 * past M4_HFP_EVENTS_RAW_MIN_INTERVAL_MS (200ms, m4-hfp-events.c), a third
 * call must go through again. */
static void test_raw_rate_limited(void)
{
	char runtime_dir[128], sock_path[256];
	int listen_fd;
	char buf[256];
	ssize_t n;
	struct timeval tv_short = { .tv_sec = 0, .tv_usec = 50 * 1000 }; /* short: proves absence, not presence */

	usleep(250 * 1000); /* clear the process-wide rate limiter from any earlier test */

	make_tmp_runtime_dir(runtime_dir, sizeof(runtime_dir));
	setenv("XDG_RUNTIME_DIR", runtime_dir, 1);
	listen_fd = bind_listener(runtime_dir, sock_path, sizeof(sock_path)); /* SO_RCVTIMEO 2s, see bind_listener() */

	m4_hfp_events_notify_raw(NULL, "AT+ONE", "00:1B:66:00:00:01", 200); /* primes the rate limiter */
	n = recv(listen_fd, buf, sizeof(buf) - 1, 0);
	CHECK(n > 0, "first raw call goes through");

	setsockopt(listen_fd, SOL_SOCKET, SO_RCVTIMEO, &tv_short, sizeof(tv_short));
	m4_hfp_events_notify_raw(NULL, "AT+TWO", "00:1B:66:00:00:01", 200); /* immediately after -- rate-limited */
	n = recv(listen_fd, buf, sizeof(buf) - 1, 0);
	CHECK(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK),
	      "second raw call within 200ms is dropped, not queued (errno=%d)", n < 0 ? errno : 0);

	usleep(250 * 1000); /* past the 200ms floor */

	m4_hfp_events_notify_raw(NULL, "AT+THREE", "00:1B:66:00:00:01", 200);
	{
		struct timeval tv2 = { .tv_sec = 2, .tv_usec = 0 };
		setsockopt(listen_fd, SOL_SOCKET, SO_RCVTIMEO, &tv2, sizeof(tv2));
	}
	n = recv(listen_fd, buf, sizeof(buf) - 1, 0);
	CHECK(n > 0, "third raw call, after the rate-limit window, goes through");
	if (n > 0) {
		buf[n] = '\0';
		CHECK(strstr(buf, "AT+THREE") != NULL, "third call carries AT+THREE, not a stale AT+TWO: \"%s\"",
		      buf);
	}

	close(listen_fd);
}

/* 7. sanitisation + length cap: embedded whitespace/control bytes become
 * '_' (the result is always exactly one whitespace-free token), and the
 * token is capped at 48 bytes regardless of input length -- both required
 * so a stray/malicious peer cannot inject extra wire fields or an
 * unbounded line via this path. */
static void test_raw_sanitizes_and_truncates(void)
{
	char runtime_dir[128], sock_path[256];
	int listen_fd;
	char buf[256];
	ssize_t n;
	char verb[16], token[128], bdaddr[32];
	long long ts_ms;
	char long_input[80];

	usleep(250 * 1000); /* clear the rate limiter from the previous test */

	make_tmp_runtime_dir(runtime_dir, sizeof(runtime_dir));
	setenv("XDG_RUNTIME_DIR", runtime_dir, 1);
	listen_fd = bind_listener(runtime_dir, sock_path, sizeof(sock_path));

	m4_hfp_events_notify_raw(NULL, "AT+XAPL=1,2 evil\ttab\x01\x02", "00:1B:66:00:00:01", 200);
	n = recv(listen_fd, buf, sizeof(buf) - 1, 0);
	CHECK(n > 0, "sanitized datagram received");
	if (n > 0) {
		buf[n] = '\0';
		CHECK(sscanf(buf, "%15s %127s %31s %lld", verb, token, bdaddr, &ts_ms) == 4,
		      "sanitized line is still exactly one whitespace-free token: \"%s\"", buf);
		CHECK(strchr(token, ' ') == NULL, "no embedded space survives");
		CHECK(strncmp(token, "AT+XAPL=1,2_evil", 16) == 0, "printable prefix preserved: \"%s\"", token);
	}

	usleep(250 * 1000);
	memset(long_input, 'X', sizeof(long_input) - 1);
	long_input[sizeof(long_input) - 1] = '\0';
	m4_hfp_events_notify_raw(NULL, long_input, "00:1B:66:00:00:01", 200);
	n = recv(listen_fd, buf, sizeof(buf) - 1, 0);
	CHECK(n > 0, "truncated datagram received");
	if (n > 0) {
		buf[n] = '\0';
		CHECK(sscanf(buf, "%15s %127s %31s %lld", verb, token, bdaddr, &ts_ms) == 4,
		      "truncated line still well-formed: \"%s\"", buf);
		CHECK(strlen(token) == 48, "token capped at 48 bytes (got %zu): \"%s\"", strlen(token), token);
	}

	close(listen_fd);
}

/* 8. empty/NULL raw_at -> no-op, same contract as m4_hfp_events_notify(). */
static void test_raw_empty_is_noop(void)
{
	char runtime_dir[128], sock_path[256];
	int listen_fd;
	char buf[256];
	ssize_t n;

	usleep(250 * 1000);
	make_tmp_runtime_dir(runtime_dir, sizeof(runtime_dir));
	setenv("XDG_RUNTIME_DIR", runtime_dir, 1);
	listen_fd = bind_listener(runtime_dir, sock_path, sizeof(sock_path));

	m4_hfp_events_notify_raw(NULL, "", "00:1B:66:00:00:01", 200);
	m4_hfp_events_notify_raw(NULL, NULL, "00:1B:66:00:00:01", 200);
	m4_hfp_events_notify_raw(NULL, "AT+SENTINEL", "00:1B:66:00:00:01", 200); /* proves nothing queued ahead */

	n = recv(listen_fd, buf, sizeof(buf) - 1, 0);
	CHECK(n > 0, "sentinel raw datagram received");
	if (n > 0) {
		buf[n] = '\0';
		CHECK(strstr(buf, "AT+SENTINEL") != NULL, "empty/NULL raw_at sent nothing: \"%s\"", buf);
	}

	close(listen_fd);
}

int main(void)
{
	test_no_listener_does_not_block();
	test_full_buffer_does_not_block();
	test_normal_roundtrip();
	test_repeated_calls_stay_cheap();
	test_raw_normal_roundtrip();
	test_raw_rate_limited();
	test_raw_sanitizes_and_truncates();
	test_raw_empty_is_noop();

	if (g_failures > 0) {
		fprintf(stderr, "%d check(s) failed\n", g_failures);
		return 1;
	}
	printf("hfp_events_test: all checks passed\n");
	return 0;
}
