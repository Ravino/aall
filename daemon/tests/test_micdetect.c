/* Unit tests for the pure decision logic in pw/micdetect.c
 * (m4_mic_decide/m4_mic_source_is_m4/m4_mic_binary_excluded/
 * m4_mic_placeholder_capture_name) -- no live PipeWire connection.
 * Scenarios mirror the already-fixed prior implementation and its own
 * test additions:
 * excluded binaries, stream.monitor taps (pavucontrol VU meters),
 * passive links, our own placeholder capture, plain M4 mic use, and the
 * regression case (our own capture + a real app on the same source ->
 * still counts as active).
 */
#include <stdio.h>
#include <string.h>

#include "pw/micdetect.h"

static int failures = 0;

#define CHECK(cond, msg) \
	do { \
		if (!(cond)) { \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, (msg)); \
			failures++; \
		} \
	} while (0)

static struct m4_mic_candidate mk(bool source_is_m4, const char *media_class,
				   const char *node_name, bool passive, bool monitor,
				   const char *app_binary)
{
	struct m4_mic_candidate c;

	memset(&c, 0, sizeof(c));
	c.source_is_m4_mic = source_is_m4;
	if (media_class != NULL)
		snprintf(c.sink_media_class, sizeof(c.sink_media_class), "%s", media_class);
	if (node_name != NULL)
		snprintf(c.sink_node_name, sizeof(c.sink_node_name), "%s", node_name);
	c.sink_passive = passive;
	c.sink_stream_monitor = monitor;
	if (app_binary != NULL)
		snprintf(c.sink_app_binary, sizeof(c.sink_app_binary), "%s", app_binary);
	return c;
}

static void test_source_is_m4(void)
{
	CHECK(m4_mic_source_is_m4("momentum4_mic", "00:1B:66:00:00:01", "momentum4_mic"),
	      "exact placeholder name matches");
	CHECK(!m4_mic_source_is_m4("momentum4_mic_capture", "00:1B:66:00:00:01", "momentum4_mic"),
	      "the placeholder's OWN capture stream is not itself the mic source "
	      "(it's a consumer, not the source)");
	CHECK(m4_mic_source_is_m4("bluez_input.00_1B_66_00_00_01.0", "00:1B:66:00:00:01", "momentum4_mic"),
	      "real HFP source node, prefix match");
	CHECK(m4_mic_source_is_m4("bluez_input.00_1B_66_00_00_01.1", "00:1B:66:00:00:01", NULL),
	      "real HFP source node, no placeholder configured");
	CHECK(!m4_mic_source_is_m4("bluez_input.AA_BB_CC_DD_EE_FF.0", "00:1B:66:00:00:01", "momentum4_mic"),
	      "a different device's bluez_input node is not the M4");
	CHECK(!m4_mic_source_is_m4("capture.studio_mic", "00:1B:66:00:00:01", "momentum4_mic"),
	      "an unrelated capture node");
	CHECK(!m4_mic_source_is_m4(NULL, "00:1B:66:00:00:01", "momentum4_mic"), "NULL node_name");
	CHECK(!m4_mic_source_is_m4("momentum4_mic", NULL, NULL), "no MAC, no placeholder -> never matches");
}

static void test_binary_excluded(void)
{
	CHECK(m4_mic_binary_excluded("pavucontrol"), "pavucontrol excluded");
	CHECK(m4_mic_binary_excluded("pw-mon"), "pw-mon excluded");
	CHECK(m4_mic_binary_excluded("pw-cat"), "pw-cat excluded");
	CHECK(!m4_mic_binary_excluded("firefox"), "an ordinary app is not excluded");
	CHECK(!m4_mic_binary_excluded(NULL), "NULL is not excluded");
	CHECK(!m4_mic_binary_excluded(""), "empty string is not excluded");
}

static void test_placeholder_capture_name(void)
{
	char buf[64];

	m4_mic_placeholder_capture_name("momentum4_mic", buf, sizeof(buf));
	CHECK(strcmp(buf, "momentum4_mic_capture") == 0, "builds \"<name>_capture\"");

	m4_mic_placeholder_capture_name(NULL, buf, sizeof(buf));
	CHECK(buf[0] == '\0', "NULL placeholder -> empty string");

	m4_mic_placeholder_capture_name("", buf, sizeof(buf));
	CHECK(buf[0] == '\0', "empty placeholder -> empty string");

	m4_mic_placeholder_capture_name("momentum4_mic", buf, 5);
	CHECK(strlen(buf) < 5, "truncation stays inside the buffer");
}

static void test_decide_basic(void)
{
	struct m4_mic_candidate c[4];

	/* Not our mic at all. */
	c[0] = mk(false, "Stream/Input/Audio", "some-app-input", false, false, "firefox");
	CHECK(!m4_mic_decide(c, 1, NULL), "source_is_m4_mic=false never counts");

	/* Real use: an ordinary app capturing from the M4/placeholder mic. */
	c[0] = mk(true, "Stream/Input/Audio", "capture.recorder", false, false, "gnome-sound-recorder");
	CHECK(m4_mic_decide(c, 1, "momentum4_mic_capture"), "ordinary app on M4 mic -> active");

	/* Wrong media class (defensive case -- should not happen in practice). */
	c[0] = mk(true, "Stream/Output/Audio", "weird", false, false, "");
	CHECK(!m4_mic_decide(c, 1, NULL), "non-Stream/Input/Audio sink is ignored");
}

static void test_decide_exclusions(void)
{
	struct m4_mic_candidate c[1];

	c[0] = mk(true, "Stream/Input/Audio", "pavucontrol-peak", false, false, "pavucontrol");
	CHECK(!m4_mic_decide(c, 1, NULL), "excluded binary (pavucontrol) does not count");

	c[0] = mk(true, "Stream/Input/Audio", "vu-meter", false, true, "some-mixer");
	CHECK(!m4_mic_decide(c, 1, NULL), "stream.monitor=true (peak meter) does not count");

	c[0] = mk(true, "Stream/Input/Audio", "some-passive-tap", true, false, "");
	CHECK(!m4_mic_decide(c, 1, NULL), "node.passive=true does not count");

	/* Regression: our own placeholder capture must not
	 * count as "mic in use" (else automode would never leave HFP). */
	c[0] = mk(true, "Stream/Input/Audio", "momentum4_mic_capture", true, false, "");
	CHECK(!m4_mic_decide(c, 1, "momentum4_mic_capture"),
	      "our own placeholder capture (passive AND by name) does not count");

	/* Defense-in-depth: name match alone (hypothetically not passive
	 * anymore) still excludes -- matches the documented rationale for
	 * checking both. */
	c[0] = mk(true, "Stream/Input/Audio", "momentum4_mic_capture", false, false, "");
	CHECK(!m4_mic_decide(c, 1, "momentum4_mic_capture"),
	      "our own placeholder capture excluded by name alone");
}

static void test_decide_regression_own_capture_plus_real_app(void)
{
	/* Regression test: our own passive placeholder capture
	 * AND a real application on the SAME source must still be reported
	 * as "mic active" (the exclusion is per-link, not "if our capture is
	 * present, ignore everything"). */
	struct m4_mic_candidate c[2];

	c[0] = mk(true, "Stream/Input/Audio", "momentum4_mic_capture", true, false, "");
	c[1] = mk(true, "Stream/Input/Audio", "capture.discord", false, false, "discord");
	CHECK(m4_mic_decide(c, 2, "momentum4_mic_capture"),
	      "our own capture + a real app on the same source -> still active");

	/* Order must not matter. */
	{
		struct m4_mic_candidate swapped[2] = { c[1], c[0] };
		CHECK(m4_mic_decide(swapped, 2, "momentum4_mic_capture"), "order-independent");
	}
}

static void test_decide_edge_cases(void)
{
	CHECK(!m4_mic_decide(NULL, 0, NULL), "NULL candidates, 0 count -> not active");
	CHECK(!m4_mic_decide(NULL, 5, NULL), "NULL candidates with nonzero count still safe");
}

/* Bug 1 fix --
 * m4_mic_is_holder()/m4_mic_decide_hold(). */
static void test_is_holder(void)
{
	CHECK(m4_mic_is_holder("signal-desktop"), "signal-desktop is a known holder");
	CHECK(!m4_mic_is_holder("librewolf"), "an ordinary calling app is not a holder");
	CHECK(!m4_mic_is_holder(NULL), "NULL is not a holder");
	CHECK(!m4_mic_is_holder(""), "empty string is not a holder");
}

static void test_decide_hold_excludes_holder_but_not_decide(void)
{
	struct m4_mic_candidate c[1];

	c[0] = mk(true, "Stream/Input/Audio", "signal-desktop_capture", false, false, "signal-desktop");
	CHECK(m4_mic_decide(c, 1, NULL), "m4_mic_decide(): holder alone still counts (ENTRY, unchanged)");
	CHECK(!m4_mic_decide_hold(c, 1, NULL), "m4_mic_decide_hold(): holder alone does NOT count");
}

static void test_decide_hold_multi_app(void)
{
	/* multi_app_mic.scn's shape, at the pure
	 * decide() level: signal-desktop (holder) + librewolf (real call) on
	 * the mic at the same time -> hold_active must be true (librewolf
	 * alone is enough), mic_active also true. */
	struct m4_mic_candidate c[2];

	c[0] = mk(true, "Stream/Input/Audio", "signal-desktop_capture", false, false, "signal-desktop");
	c[1] = mk(true, "Stream/Input/Audio", "librewolf_capture", false, false, "librewolf");
	CHECK(m4_mic_decide(c, 2, NULL), "both present -> mic_active true");
	CHECK(m4_mic_decide_hold(c, 2, NULL), "librewolf (non-holder) -> hold_active true");

	/* librewolf leaves, only the holder remains -- mic_active stays true
	 * (the bug's own symptom, unchanged by design, see this header's own
	 * "Bug 1" comment), hold_active must now be false (the fix). */
	struct m4_mic_candidate only_holder[1] = { c[0] };
	CHECK(m4_mic_decide(only_holder, 1, NULL), "holder alone -> mic_active STILL true");
	CHECK(!m4_mic_decide_hold(only_holder, 1, NULL), "holder alone -> hold_active false");
}

static void test_decide_hold_edge_cases(void)
{
	CHECK(!m4_mic_decide_hold(NULL, 0, NULL), "NULL candidates, 0 count -> not hold-active");
	CHECK(!m4_mic_decide_hold(NULL, 5, NULL), "NULL candidates with nonzero count still safe");

	/* Existing exclusions (tool binaries, monitor, passive, our own
	 * placeholder capture) still apply on top of the holder filter --
	 * m4_mic_decide_hold() is a thin filter in front of m4_mic_decide(),
	 * not a replacement for its own exclusion logic. */
	struct m4_mic_candidate c[1];
	c[0] = mk(true, "Stream/Input/Audio", "pavucontrol-peak", false, false, "pavucontrol");
	CHECK(!m4_mic_decide_hold(c, 1, NULL), "non-holder exclusions still apply under decide_hold()");
}

int main(void)
{
	test_source_is_m4();
	test_binary_excluded();
	test_placeholder_capture_name();
	test_decide_basic();
	test_decide_exclusions();
	test_decide_regression_own_capture_plus_real_app();
	test_decide_edge_cases();
	test_is_holder();
	test_decide_hold_excludes_holder_but_not_decide();
	test_decide_hold_multi_app();
	test_decide_hold_edge_cases();

	if (failures > 0) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("micdetect: all checks passed\n");
	return 0;
}
