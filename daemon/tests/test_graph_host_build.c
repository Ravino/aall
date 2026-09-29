/* tests/test_graph_host_build.c -- pure-logic unit tests for
 * graph/graph_host_build.c. No PipeWire, no live system --
 * every case here checks the generated module-args STRING only. Same
 * plain-main()-exits-nonzero-on-first-failure convention as every other
 * test/ in this tree (see tests/meson.build's own top comment).
 */
#include <stdio.h>
#include <string.h>

#include "graph/graph_host_build.h"

static int g_failures = 0;

#define CHECK(cond, msg) \
	do { \
		if (!(cond)) { \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, (msg)); \
			g_failures++; \
		} \
	} while (0)

static int contains(const char *haystack, const char *needle)
{
	return strstr(haystack, needle) != NULL;
}

static void test_mac_underscore(void)
{
	char out[32];

	CHECK(m4_graph_host_mac_underscore("00:1B:66:00:00:01", out, sizeof(out)) == out,
	      "mac_underscore should succeed on a normal MAC");
	CHECK(strcmp(out, "00_1B_66_00_00_01") == 0, "mac_underscore output mismatch");

	CHECK(m4_graph_host_mac_underscore(NULL, out, sizeof(out)) == NULL,
	      "mac_underscore(NULL) must fail");
	CHECK(m4_graph_host_mac_underscore("", out, sizeof(out)) == NULL,
	      "mac_underscore(\"\") must fail");
	CHECK(m4_graph_host_mac_underscore("00:1B:66:00:00:01", out, 4) == NULL,
	      "mac_underscore must fail when out_cap is too small, not truncate");
}

static void test_combine_basic(void)
{
	struct m4_graph_host_config cfg = {
		.combine_enable = true,
		.combine_name = "m4_combined",
		.combine_members_csv = "",
		.combine_latency_compensate = false,
		.m4_mac = "00:1B:66:00:00:01",
	};
	char buf[M4_GRAPH_HOST_ARGS_BUF_MIN];
	int n;

	n = m4_graph_host_build_combine_args(&cfg, buf, sizeof(buf));
	CHECK(n > 0, "combine builder should succeed with just M4");
	CHECK((size_t)n == strlen(buf), "returned length must match strlen(buf)");
	CHECK(contains(buf, "combine.mode = sink"), "combine.mode missing");
	CHECK(contains(buf, "node.name = \"m4_combined\""), "combine node.name missing");
	CHECK(contains(buf, "combine.latency-compensate = false"), "latency-compensate must be false");
	CHECK(contains(buf, "node.name = \"bluez_output.00_1B_66_00_00_01.1\""),
	      "M4's own combine member pattern missing");
	CHECK(contains(buf, "media.class = \"Audio/Sink\""), "media.class match missing");
}

static void test_combine_members_csv(void)
{
	struct m4_graph_host_config cfg = {
		.combine_enable = true,
		.combine_name = "m4_combined",
		.combine_members_csv = " PRO_X_Wireless_Gaming_Headset , ~alsa_output.*,, ",
		.combine_latency_compensate = true,
		.m4_mac = "00:1B:66:00:00:01",
	};
	char buf[M4_GRAPH_HOST_ARGS_BUF_MIN];
	int n;

	n = m4_graph_host_build_combine_args(&cfg, buf, sizeof(buf));
	CHECK(n > 0, "combine builder should succeed with a members CSV");
	CHECK(contains(buf, "node.name = \"PRO_X_Wireless_Gaming_Headset\""),
	      "literal member token missing (whitespace not trimmed?)");
	CHECK(contains(buf, "node.name = \"~alsa_output.*\""),
	      "regex member token ('~' prefix) missing or mangled");
	CHECK(contains(buf, "node.name = \"bluez_output.00_1B_66_00_00_01.1\""),
	      "M4 pattern missing when a members CSV is also given");
	CHECK(contains(buf, "combine.latency-compensate = true"), "latency-compensate must be true");
	/* empty tokens (",," / trailing comma) must not produce a stray
	 * "node.name = \"\"" match. */
	CHECK(!contains(buf, "node.name = \"\""), "empty CSV token produced an empty match");
}

static void test_combine_no_mac_yet(void)
{
	struct m4_graph_host_config cfg = {
		.combine_enable = true,
		.combine_name = "m4_combined",
		.combine_members_csv = "some_sink",
		.combine_latency_compensate = false,
		.m4_mac = "",
	};
	char buf[M4_GRAPH_HOST_ARGS_BUF_MIN];
	int n;

	n = m4_graph_host_build_combine_args(&cfg, buf, sizeof(buf));
	CHECK(n > 0, "combine builder must still succeed with an unknown MAC");
	CHECK(contains(buf, "node.name = \"some_sink\""), "listed member still present");
	CHECK(!contains(buf, "bluez_output"), "no M4 pattern should be emitted without a MAC");
}

static void test_combine_rejects_quote_injection(void)
{
	struct m4_graph_host_config cfg = {
		.combine_enable = true,
		.combine_name = "m4_combined",
		.combine_members_csv = "evil\" } ] actions = {} } { matches = [ {\"x",
		.m4_mac = "00:1B:66:00:00:01",
	};
	char buf[M4_GRAPH_HOST_ARGS_BUF_MIN];

	CHECK(m4_graph_host_build_combine_args(&cfg, buf, sizeof(buf)) == -1,
	      "a '\"' in a members token must be rejected, not silently embedded");
}

static void test_combine_buffer_too_small(void)
{
	struct m4_graph_host_config cfg = {
		.combine_enable = true,
		.combine_name = "m4_combined",
		.combine_members_csv = "",
		.m4_mac = "00:1B:66:00:00:01",
	};
	char tiny[8];

	CHECK(m4_graph_host_build_combine_args(&cfg, tiny, sizeof(tiny)) == -1,
	      "a too-small buffer must fail, not silently truncate");
}

static void test_combine_null_args(void)
{
	char buf[16];
	CHECK(m4_graph_host_build_combine_args(NULL, buf, sizeof(buf)) == -1, "NULL cfg must fail");
}

static void test_mic_basic(void)
{
	struct m4_graph_host_config cfg = {
		.mic_placeholder_enable = true,
		.mic_name = "momentum4_mic",
		.m4_mac = "00:1B:66:00:00:01",
	};
	char buf[M4_GRAPH_HOST_ARGS_BUF_MIN];
	int n;

	n = m4_graph_host_build_mic_args(&cfg, buf, sizeof(buf));
	CHECK(n > 0, "mic builder should succeed");
	CHECK(contains(buf, "audio.channels = 1"), "mono channel count missing");
	CHECK(contains(buf, "filter.smart = true"), "filter.smart missing");
	CHECK(contains(buf, "\"api.bluez5.address\":\"00:1B:66:00:00:01\""),
	      "filter.smart.target MAC missing (must be strict-JSON, colon-separated)");
	CHECK(contains(buf, "\"media.class\":\"Audio/Source\""), "filter.smart.target media.class missing");
	CHECK(contains(buf, "node.name = \"momentum4_mic_capture\""), "capture node.name missing");
	CHECK(contains(buf, "media.class = Audio/Source"), "playback media.class override missing");
	CHECK(contains(buf, "node.name = \"momentum4_mic\""), "playback (public) node.name missing");
	CHECK(contains(buf, "node.virtual = false"), "node.virtual=false missing (must not look virtual)");
	CHECK(contains(buf, "node.linger = true"), "node.linger=true missing (must survive last unlink)");
}

static void test_mic_no_mac_yet(void)
{
	struct m4_graph_host_config cfg = {
		.mic_placeholder_enable = true,
		.mic_name = "momentum4_mic",
		.m4_mac = "",
	};
	char buf[M4_GRAPH_HOST_ARGS_BUF_MIN];
	int n;

	n = m4_graph_host_build_mic_args(&cfg, buf, sizeof(buf));
	CHECK(n > 0, "mic builder must still succeed without a known MAC (permanently-present placeholder)");
	CHECK(!contains(buf, "filter.smart.target"), "no filter.smart.target without a MAC");
	CHECK(contains(buf, "node.name = \"momentum4_mic\""), "placeholder node itself must still exist");
}

static void test_mic_default_name(void)
{
	struct m4_graph_host_config cfg = {
		.mic_placeholder_enable = true,
		.mic_name = "",
		.m4_mac = "00:1B:66:00:00:01",
	};
	char buf[M4_GRAPH_HOST_ARGS_BUF_MIN];

	CHECK(m4_graph_host_build_mic_args(&cfg, buf, sizeof(buf)) > 0, "mic builder with empty name should use default");
	CHECK(contains(buf, "\"momentum4_mic\""), "empty mic_name must fall back to \"momentum4_mic\"");
}

int main(void)
{
	test_mac_underscore();
	test_combine_basic();
	test_combine_members_csv();
	test_combine_no_mac_yet();
	test_combine_rejects_quote_injection();
	test_combine_buffer_too_small();
	test_combine_null_args();
	test_mic_basic();
	test_mic_no_mac_yet();
	test_mic_default_name();

	if (g_failures > 0) {
		fprintf(stderr, "%d check(s) failed\n", g_failures);
		return 1;
	}
	printf("test_graph_host_build: all checks passed\n");
	return 0;
}
