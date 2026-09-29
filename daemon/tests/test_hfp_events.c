/* tests/test_hfp_events.c -- ipc/hfp_events.c (patch 0006 and earcon
 * timing): the pure wire-format
 * parser (no I/O at all), and the receiving end of hfp-events.sock as a
 * real AF_UNIX SOCK_DGRAM socket in a throwaway mkdtemp() directory (same
 * pattern as test_ctl.c's real-socket tests) -- no PipeWire, no
 * WirePlumber, no SPA plugin: m4-hfp-events.c's SENDING side is a separate
 * build (module/), this file only re-implements the same
 * tiny wire format on the client side by hand to drive the receiver.
 *
 * "no unbounded blocking": test_drain_is_bounded() below
 * proves the io callback never processes an unbounded number of pending
 * datagrams in one sd_event_run() dispatch (the receiver's own
 * non-blocking guarantee); the SENDER side's non-blocking guarantee
 * (m4_hfp_events_notify() in the SPA plugin, MSG_DONTWAIT / no listener /
 * full receive buffer) is covered separately in
 * module/test/hfp_events_test.c, which links that file
 * directly.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <systemd/sd-event.h>

#include "ipc/hfp_events.h"

static int failures = 0;

#define CHECK(cond, msg)                                                          \
	do {                                                                       \
		if (!(cond)) {                                                     \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, (msg)); \
			failures++;                                                \
		}                                                                   \
	} while (0)

/* ------------------------------------------------------------------ */
/* pure parser */

static void test_parse_valid(void)
{
	struct m4_hfp_event ev;

	CHECK(m4_hfp_event_parse("CHUP 00:1B:66:00:00:01 1700000000000", &ev) == 0, "CHUP parses");
	CHECK(ev.verb == M4_HFP_VERB_CHUP, "verb CHUP");
	CHECK(strcmp(ev.bdaddr, "00:1B:66:00:00:01") == 0, "bdaddr");
	CHECK(ev.wire_realtime_usec == 1700000000000ULL * 1000ULL, "ts_ms -> usec *1000");
	CHECK(ev.realtime_usec == 0 && ev.monotonic_s == 0.0, "receive-time fields left at 0 by the parser itself");

	CHECK(m4_hfp_event_parse("ATA 00:00:00:00:00:00 0", &ev) == 0, "ATA parses");
	CHECK(ev.verb == M4_HFP_VERB_ATA, "verb ATA");
	CHECK(ev.wire_realtime_usec == 0, "ts_ms=0 -> usec 0");

	CHECK(m4_hfp_event_parse("BLDN 00:1B:66:00:00:01 123", &ev) == 0, "BLDN parses");
	CHECK(ev.verb == M4_HFP_VERB_BLDN, "verb BLDN");

	CHECK(m4_hfp_event_parse("BVRA 00:1B:66:00:00:01 123", &ev) == 0, "BVRA parses");
	CHECK(ev.verb == M4_HFP_VERB_BVRA, "verb BVRA");

	/* a trailing newline (patch 0006 never sends one, but tolerate it --
	 * sscanf's %s conversions already stop at any whitespace) */
	CHECK(m4_hfp_event_parse("CHUP 00:1B:66:00:00:01 1\n", &ev) == 0, "trailing newline tolerated");
}

static void test_parse_invalid(void)
{
	struct m4_hfp_event ev;

	CHECK(m4_hfp_event_parse(NULL, &ev) == -1, "NULL line");
	CHECK(m4_hfp_event_parse("CHUP 00:1B:66:00:00:01 1", NULL) == -1, "NULL out");
	CHECK(m4_hfp_event_parse("", &ev) == -1, "empty line");
	CHECK(m4_hfp_event_parse("CHUP", &ev) == -1, "missing fields");
	CHECK(m4_hfp_event_parse("CHUP 00:1B:66:00:00:01", &ev) == -1, "missing timestamp");
	CHECK(m4_hfp_event_parse("CHUP 00:1B:66:00:00:01 1 extra", &ev) == -1, "extra 4th field rejected");
	CHECK(m4_hfp_event_parse("FROB 00:1B:66:00:00:01 1", &ev) == -1, "unknown verb rejected");
	CHECK(m4_hfp_event_parse("chup 00:1B:66:00:00:01 1", &ev) == -1, "verb is case-sensitive (exact match)");
	CHECK(m4_hfp_event_parse("CHUP 00:1B:66:00:00:01 -1", &ev) == -1, "negative timestamp rejected");
	CHECK(m4_hfp_event_parse("CHUP 00:1B:66:00:00:01 notanumber", &ev) == -1, "non-numeric timestamp rejected");

	/* out is fully zeroed on failure, never partially filled */
	memset(&ev, 0xAA, sizeof(ev));
	CHECK(m4_hfp_event_parse("garbage", &ev) == -1, "garbage rejected");
	CHECK(ev.verb == 0 && ev.bdaddr[0] == '\0' && ev.wire_realtime_usec == 0,
	      "out zeroed on failure, not left at 0xAA");
}

/* patch 0007 ("tap = mic"): "RAW <token>
 * <bdaddr> <ts_ms>" -- 4 fields, distinct shape from the named-verb lines
 * above (module/m4-src/m4-hfp-events.c's
 * m4_hfp_events_notify_raw() is this wire format's only real sender). */
static void test_parse_raw_valid(void)
{
	struct m4_hfp_event ev;

	CHECK(m4_hfp_event_parse("RAW AT+CLCC 00:1B:66:00:00:01 1700000000000", &ev) == 0, "RAW parses");
	CHECK(ev.verb == M4_HFP_VERB_RAW, "verb RAW");
	CHECK(strcmp(ev.raw_text, "AT+CLCC") == 0, "raw_text");
	CHECK(strcmp(ev.bdaddr, "00:1B:66:00:00:01") == 0, "bdaddr");
	CHECK(ev.wire_realtime_usec == 1700000000000ULL * 1000ULL, "ts_ms -> usec *1000");

	/* a named-verb event's raw_text stays empty (memset(out, 0, ...) at
	 * the top of m4_hfp_event_parse() covers this, but assert it
	 * explicitly since daemon.c's m4_daemon_hfp_event() branches on
	 * verb == M4_HFP_VERB_RAW specifically to decide whether to read it) */
	CHECK(m4_hfp_event_parse("CHUP 00:1B:66:00:00:01 1", &ev) == 0 && ev.raw_text[0] == '\0',
	      "named verb: raw_text empty");
}

static void test_parse_raw_invalid(void)
{
	struct m4_hfp_event ev;

	CHECK(m4_hfp_event_parse("RAW", &ev) == -1, "RAW missing fields");
	CHECK(m4_hfp_event_parse("RAW AT+CLCC 00:1B:66:00:00:01", &ev) == -1, "RAW missing timestamp");
	CHECK(m4_hfp_event_parse("RAW AT+CLCC 00:1B:66:00:00:01 1 extra", &ev) == -1, "RAW extra 5th field rejected");
	CHECK(m4_hfp_event_parse("RAW AT+CLCC 00:1B:66:00:00:01 -1", &ev) == -1, "RAW negative timestamp rejected");
	/* only ever 3 fields (no token) -- must NOT be mistaken for a
	 * named-verb line just because "RAW" is not one of them */
	CHECK(m4_hfp_event_parse("RAW 00:1B:66:00:00:01 1", &ev) == -1, "RAW without a token rejected");
}

static void test_verb_name(void)
{
	CHECK(strcmp(m4_hfp_verb_name(M4_HFP_VERB_CHUP), "CHUP") == 0, "name CHUP");
	CHECK(strcmp(m4_hfp_verb_name(M4_HFP_VERB_ATA), "ATA") == 0, "name ATA");
	CHECK(strcmp(m4_hfp_verb_name(M4_HFP_VERB_BLDN), "BLDN") == 0, "name BLDN");
	CHECK(strcmp(m4_hfp_verb_name(M4_HFP_VERB_BVRA), "BVRA") == 0, "name BVRA");
	CHECK(strcmp(m4_hfp_verb_name(M4_HFP_VERB_RAW), "RAW") == 0, "name RAW");
}

/* ------------------------------------------------------------------ */
/* real socket */

static void make_tmp_runtime_dir(char *out, size_t out_len)
{
	snprintf(out, out_len, "/tmp/m4d-test-hfp-events.XXXXXX");
	CHECK(mkdtemp(out) != NULL, "mkdtemp");
}

static void send_raw(const char *sock_path, const char *line)
{
	int fd;
	struct sockaddr_un addr;

	fd = socket(AF_UNIX, SOCK_DGRAM, 0);
	CHECK(fd >= 0, "client socket()");
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock_path);
	CHECK(sendto(fd, line, strlen(line), 0, (struct sockaddr *)&addr, sizeof(addr)) >= 0,
	      "client sendto()");
	close(fd);
}

static int g_cb_calls;
static struct m4_hfp_event g_last_ev;

static void test_cb(void *user_data, const struct m4_hfp_event *ev)
{
	(void)user_data;
	g_cb_calls++;
	g_last_ev = *ev;
}

static double monotonic_s_now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void test_roundtrip_and_fills_receive_clocks(void)
{
	char runtime_dir[192], sock_path[256];
	sd_event *event;
	struct m4_hfp_events *h;
	double before;

	make_tmp_runtime_dir(runtime_dir, sizeof(runtime_dir));
	snprintf(sock_path, sizeof(sock_path), "%s/hfp-events.sock", runtime_dir);
	CHECK(sd_event_new(&event) >= 0, "sd_event_new");

	g_cb_calls = 0;
	h = m4_hfp_events_open(event, runtime_dir, test_cb, NULL);
	CHECK(h != NULL, "open");

	before = monotonic_s_now();
	send_raw(sock_path, "CHUP 00:1B:66:00:00:01 42");
	for (int i = 0; i < 20 && g_cb_calls == 0; i++)
		sd_event_run(event, 20 * 1000);

	CHECK(g_cb_calls == 1, "cb invoked once for one well-formed datagram");
	CHECK(g_last_ev.verb == M4_HFP_VERB_CHUP, "verb delivered");
	CHECK(strcmp(g_last_ev.bdaddr, "00:1B:66:00:00:01") == 0, "bdaddr delivered");
	CHECK(g_last_ev.wire_realtime_usec == 42000, "wire timestamp delivered (42ms -> 42000us)");
	/* realtime_usec/monotonic_s are filled by the RECEIVER at actual
	 * receive time (hfp_events.h's doc comment) -- monotonic_s must be a
	 * real, recent CLOCK_MONOTONIC reading, not the (unrelated) wire
	 * timestamp and not 0. */
	CHECK(g_last_ev.realtime_usec > 0, "realtime_usec filled in by receiver");
	CHECK(g_last_ev.monotonic_s >= before, "monotonic_s filled in by receiver, not before send");

	m4_hfp_events_close(h);
	sd_event_unref(event);
}

static void test_malformed_datagram_dropped(void)
{
	char runtime_dir[192], sock_path[256];
	sd_event *event;
	struct m4_hfp_events *h;

	make_tmp_runtime_dir(runtime_dir, sizeof(runtime_dir));
	snprintf(sock_path, sizeof(sock_path), "%s/hfp-events.sock", runtime_dir);
	CHECK(sd_event_new(&event) >= 0, "sd_event_new");

	g_cb_calls = 0;
	h = m4_hfp_events_open(event, runtime_dir, test_cb, NULL);
	CHECK(h != NULL, "open");

	send_raw(sock_path, "not a valid line at all");
	send_raw(sock_path, "FROB 00:1B:66:00:00:01 1"); /* unknown verb */
	/* followed by one good one, to prove the bad ones did not wedge
	 * anything and were each independently dropped, not just delayed */
	send_raw(sock_path, "ATA 00:1B:66:00:00:01 1");

	for (int i = 0; i < 20 && g_cb_calls == 0; i++)
		sd_event_run(event, 20 * 1000);

	CHECK(g_cb_calls == 1, "only the one well-formed datagram was delivered");
	CHECK(g_last_ev.verb == M4_HFP_VERB_ATA, "it was the ATA one");

	m4_hfp_events_close(h);
	sd_event_unref(event);
}

/* (tests for no unbounded blocking): the io callback must
 * never drain an unbounded number of pending datagrams in a single
 * dispatch -- proven here by queuing well over the internal bound (see
 * ipc/hfp_events.c's M4_HFP_EVENTS_MAX_DRAIN) BEFORE the event loop is ever
 * run, then observing that one sd_event_run() iteration alone does not
 * already deliver all of them, while enough iterations eventually do
 * (level-triggered EPOLLIN keeps re-firing until the socket is drained --
 * this is "bounded per dispatch", not "some events are lost"). */
#define FLOOD_N 200

static void test_drain_is_bounded(void)
{
	char runtime_dir[192], sock_path[256];
	sd_event *event;
	struct m4_hfp_events *h;
	int iterations;

	make_tmp_runtime_dir(runtime_dir, sizeof(runtime_dir));
	snprintf(sock_path, sizeof(sock_path), "%s/hfp-events.sock", runtime_dir);
	CHECK(sd_event_new(&event) >= 0, "sd_event_new");

	g_cb_calls = 0;
	h = m4_hfp_events_open(event, runtime_dir, test_cb, NULL);
	CHECK(h != NULL, "open");

	for (int i = 0; i < FLOOD_N; i++)
		send_raw(sock_path, "ATA 00:1B:66:00:00:01 1");

	/* One single sd_event_run() call dispatches (at most) one readable
	 * event, i.e. exactly one call to the io callback -- which itself
	 * must not have drained everything if the bound is respected. */
	CHECK(sd_event_run(event, 200 * 1000) >= 0, "first sd_event_run()");
	CHECK(g_cb_calls > 0, "first dispatch delivered at least one event");
	CHECK(g_cb_calls < FLOOD_N, "first dispatch did NOT drain all queued datagrams in one go "
				     "(drain bound respected)");

	iterations = 1;
	while (g_cb_calls < FLOOD_N && iterations < 10000) {
		sd_event_run(event, 20 * 1000);
		iterations++;
	}
	CHECK(g_cb_calls == FLOOD_N, "all datagrams eventually delivered (level-triggered EPOLLIN)");
	CHECK(iterations > 1, "took more than one dispatch -- confirms the bound actually did something");

	m4_hfp_events_close(h);
	sd_event_unref(event);
}
#undef FLOOD_N

static void test_open_close_null_safe(void)
{
	CHECK(m4_hfp_events_open(NULL, "/tmp", test_cb, NULL) == NULL, "NULL event -> NULL");
	CHECK(m4_hfp_events_open((sd_event *)1, NULL, test_cb, NULL) == NULL, "NULL runtime_dir -> NULL");
	CHECK(m4_hfp_events_open((sd_event *)1, "", test_cb, NULL) == NULL, "empty runtime_dir -> NULL");
	m4_hfp_events_close(NULL); /* must not crash */
}

int main(void)
{
	test_parse_valid();
	test_parse_invalid();
	test_parse_raw_valid();
	test_parse_raw_invalid();
	test_verb_name();
	test_roundtrip_and_fills_receive_clocks();
	test_malformed_datagram_dropped();
	test_drain_is_bounded();
	test_open_close_null_safe();

	if (failures > 0) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("hfp_events: all checks passed\n");
	return 0;
}
