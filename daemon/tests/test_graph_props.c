/* Unit tests for the pure node property helpers
 * in pw/graph_pw.c -- exposed via graph_pw.h precisely so they can be
 * tested here without a live PipeWire connection. */
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "pw/graph_pw.h"

static int failures = 0;

#define CHECK(cond, msg) \
	do { \
		if (!(cond)) { \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, (msg)); \
			failures++; \
		} \
	} while (0)

static void test_build_device_name(void)
{
	char buf[80];

	m4_pw_build_device_name("AA:BB:CC:DD:EE:FF", buf, sizeof(buf));
	CHECK(strcmp(buf, "bluez_card.AA_BB_CC_DD_EE_FF") == 0,
	      "matches the live bluez5-device.c naming (verified against pw-dump)");

	/* truncation must not overflow the buffer nor omit the NUL */
	m4_pw_build_device_name("AA:BB:CC:DD:EE:FF", buf, 5);
	CHECK(strlen(buf) < 5, "truncated output stays inside the buffer");

	m4_pw_build_device_name("", buf, sizeof(buf));
	CHECK(strcmp(buf, "bluez_card.") == 0, "empty MAC still produces the prefix");
}

static void test_media_class_is_sink(void)
{
	CHECK(m4_pw_media_class_is_sink("Audio/Sink"), "plain Audio/Sink");
	CHECK(m4_pw_media_class_is_sink("Audio/Sink/Internal"), "Audio/Sink/Internal (still a sink)");
	CHECK(!m4_pw_media_class_is_sink("Audio/Source"), "Audio/Source is not a sink");
	CHECK(!m4_pw_media_class_is_sink("Audio/Device"), "Audio/Device (the card itself) is not a sink");
	CHECK(!m4_pw_media_class_is_sink(NULL), "NULL is not a sink");
	CHECK(!m4_pw_media_class_is_sink(""), "empty string is not a sink");
}

/* Observed in production (this feature's
 * "repair-a2dp and graph resilience" section): "unknown resource 3 op:2"
 * must be recognized as the benign stale-destroy-ack race, NOT as a sign the
 * whole PipeWire core connection is down. */
static void test_error_is_benign_stale_destroy(void)
{
	CHECK(m4_graph_error_is_benign_stale_destroy(-ENOENT, "unknown resource 3 op:2"),
	      "exact live-bug message, res=-ENOENT -> benign");
	CHECK(m4_graph_error_is_benign_stale_destroy(-ENOENT, "client 0x1234: unknown resource 7 op:0"),
	      "substring match, different id/op -- still benign");
	CHECK(!m4_graph_error_is_benign_stale_destroy(-ENOENT, "no global 3 any more"),
	      "different -ENOENT message shape -- NOT the stale-destroy race, must stay fatal");
	CHECK(!m4_graph_error_is_benign_stale_destroy(-EPIPE, "unknown resource 3 op:2"),
	      "same message text but a different res code -- must NOT match (res must be -ENOENT too)");
	CHECK(!m4_graph_error_is_benign_stale_destroy(-ENOENT, NULL), "NULL message -> not benign, no crash");
	CHECK(!m4_graph_error_is_benign_stale_destroy(-ENOENT, ""), "empty message -> not benign");
	CHECK(!m4_graph_error_is_benign_stale_destroy(0, "unknown resource 3 op:2"),
	      "res=0 (no error at all) -- must NOT match");
}

int main(void)
{
	test_build_device_name();
	test_media_class_is_sink();
	test_error_is_benign_stale_destroy();

	if (failures > 0) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("graph_props: all checks passed\n");
	return 0;
}
