/* Unit tests for metrics/stats.c's m4_stats_parse() -- pure, no I/O.
 * The "live" sample below was captured verbatim from this machine's
 * $XDG_RUNTIME_DIR/momentum4/adaptive-stats.json, while the M4 were
 * connected on aptX Adaptive LL. */
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "metrics/stats.h"

static int failures = 0;

#define CHECK(cond, msg) \
	do { \
		if (!(cond)) { \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, (msg)); \
			failures++; \
		} \
	} while (0)

static const char LIVE_SAMPLE[] =
	"{\"ts_unix_ms\":1790575464708,\"uptime_ms\":1100590,\"alive\":true,"
	"\"mode\":\"ll0\",\"latency_adjust_ms\":-19,\"heartbeat_enabled\":true,"
	"\"resync_period_min\":10,\"encode_calls\":110020,\"frames_produced\":110019,"
	"\"input_drops_ring_full\":0,\"input_drops_trylock\":0,\"helper_late\":2,"
	"\"heartbeat_late_triggers\":0,\"stall_events\":0,\"heartbeat_events\":1,"
	"\"output_ring_full_drops\":0,\"helper_restarts\":0,\"ring_in\":1,\"ring_in_cap\":8,"
	"\"ring_out\":1,\"ring_out_cap\":8,\"out_ring_trimmed_frames\":0,"
	"\"in_ring_trimmed_ms\":0,\"out_ring_max_window\":2,\"in_ring_max_window\":1,"
	"\"bridge_rt_policy\":\"fifo\",\"bridge_rt_prio\":70}";

static void test_live_sample(void)
{
	struct m4_stats st;
	int rc = m4_stats_parse(LIVE_SAMPLE, strlen(LIVE_SAMPLE), &st);

	CHECK(rc == 0, "live sample must parse");
	CHECK(st.ts_unix_ms == 1790575464708LL, "ts_unix_ms (exceeds 32-bit int range)");
	CHECK(fabs(st.uptime_ms - 1100590.0) < 0.5, "uptime_ms");
	CHECK(st.alive == true, "alive");
	CHECK(strcmp(st.mode, "ll0") == 0, "mode");
	CHECK(st.latency_adjust_ms == -19, "latency_adjust_ms (negative)");
	CHECK(st.heartbeat_enabled == true, "heartbeat_enabled");
	CHECK(st.resync_period_min == 10, "resync_period_min");
	CHECK(st.encode_calls == 110020, "encode_calls");
	CHECK(st.frames_produced == 110019, "frames_produced");
	CHECK(st.helper_late == 2, "helper_late");
	CHECK(st.helper_restarts == 0, "helper_restarts");
	CHECK(st.have_ring_in && st.ring_in == 1 && st.ring_in_cap == 8, "ring_in/cap");
	CHECK(st.have_ring_out && st.ring_out == 1 && st.ring_out_cap == 8, "ring_out/cap");
	CHECK(st.out_ring_max_window == 2, "out_ring_max_window");
	CHECK(strcmp(st.bridge_rt_policy, "fifo") == 0, "bridge_rt_policy");
	CHECK(st.bridge_rt_prio == 70, "bridge_rt_prio");
}

static void test_null_rings(void)
{
	/* build_stats_json() emits bare `null` for ring_in/ring_out when its
	 * trylock on the ring mutex fails -- must not be mistaken for 0. */
	static const char sample[] =
		"{\"alive\":true,\"ring_in\":null,\"ring_in_cap\":8,"
		"\"ring_out\":null,\"ring_out_cap\":8}";
	struct m4_stats st;
	int rc = m4_stats_parse(sample, strlen(sample), &st);

	CHECK(rc == 0, "null-ring sample must parse");
	CHECK(!st.have_ring_in, "ring_in must be reported as unavailable, not 0");
	CHECK(!st.have_ring_out, "ring_out must be reported as unavailable, not 0");
	CHECK(st.ring_in_cap == 8, "ring_in_cap still parses alongside a null ring_in");
}

static void test_partial_object(void)
{
	/* Forward compatibility: unknown keys ignored, missing keys leave
	 * zero value, does not error out. */
	static const char sample[] = "{\"alive\":false,\"some_future_field\":123,\"mode\":\"hq\"}";
	struct m4_stats st;
	int rc = m4_stats_parse(sample, strlen(sample), &st);

	CHECK(rc == 0, "partial object must still parse");
	CHECK(st.alive == false, "alive false");
	CHECK(strcmp(st.mode, "hq") == 0, "mode still parsed");
	CHECK(st.frames_produced == 0, "missing field defaults to 0");
	CHECK(!st.have_ring_in, "missing ring_in defaults to unavailable");
}

static void test_empty_object(void)
{
	static const char sample[] = "{}";
	struct m4_stats st;
	int rc = m4_stats_parse(sample, strlen(sample), &st);

	CHECK(rc == 0, "empty object is not a parse error");
	CHECK(st.alive == false, "zeroed alive");
	CHECK(st.mode[0] == '\0', "zeroed mode string");
}

static void test_malformed(void)
{
	struct m4_stats st;

	CHECK(m4_stats_parse("not json at all", 16, &st) == -EINVAL,
	      "garbage input rejected");
	CHECK(m4_stats_parse("[1,2,3]", 7, &st) == -EINVAL, "a JSON array is not a stats object");
	CHECK(m4_stats_parse("", 0, &st) == -EINVAL, "empty buffer rejected");
	CHECK(m4_stats_parse(NULL, 0, &st) == -EINVAL, "NULL buffer rejected");
}

static void test_bad_load_path(void)
{
	struct m4_stats st;
	int rc = m4_stats_load("/nonexistent/path/momentum4-test-stats.json", &st);
	CHECK(rc == -ENOENT, "missing file reported as ENOENT, not a parse error");
}

int main(void)
{
	test_live_sample();
	test_null_rings();
	test_partial_object();
	test_empty_object();
	test_malformed();
	test_bad_load_path();

	if (failures > 0) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("stats: all checks passed\n");
	return 0;
}
