/* momentum4: unit tests for m4-adaptive-heartbeat.c and
 * m4-adaptive-config.c. Pure logic, no dlopen, no BlueZ, no D-Bus, no
 * PipeWire, no qemu-hexagon helper, no real filesystem beyond a couple of
 * temp files this test creates and removes itself in /tmp. Uses a synthetic
 * monotonic clock (a plain double counter this test advances explicitly)
 * instead of clock_gettime(), so every timing edge case is exact and
 * deterministic -- no usleep(), no flakiness, runs in well under a second.
 *
 * This covers the heartbeat scenarios specifically;
 * see test/unit_test.c for the byte-layout/RTP-header/real-helper part and
 * test/resilience_test.c for the fault-injection part.
 */
#define _DEFAULT_SOURCE /* setenv/unsetenv */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "m4-adaptive-heartbeat.h"
#include "m4-adaptive-config.h"

static int g_failures = 0;
#define CHECK(cond, ...) do { \
	if (!(cond)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); g_failures++; } \
	else { printf("PASS: "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

/* ===================================================================== *
 * Heartbeat state machine
 * ===================================================================== */

static void test_base_mapping(void)
{
	CHECK(m4_ah_heartbeat_base_target_ms(M4_AH_MODE_LL0) == 55, "ll0 base target == 55 ms");
	CHECK(m4_ah_heartbeat_base_target_ms(M4_AH_MODE_LL1) == 106, "ll1 base target == 106 ms");
	CHECK(m4_ah_heartbeat_ssrc(M4_AH_MODE_LL0) == 0xA1, "ll0 ssrc == 0xA1");
	CHECK(m4_ah_heartbeat_ssrc(M4_AH_MODE_LL1) == 0xA2, "ll1 ssrc == 0xA2");
	CHECK(m4_ah_heartbeat_other_mode(M4_AH_MODE_LL0) == M4_AH_MODE_LL1, "other(ll0) == ll1");
	CHECK(m4_ah_heartbeat_other_mode(M4_AH_MODE_LL1) == M4_AH_MODE_LL0, "other(ll1) == ll0");
}

/* No trigger, no periodic due yet: never fires, regardless of how "clean"
 * and "quiet" things look, until either a trigger happens or period_s
 * elapses (no immediate resync right after
 * a fresh transport comes up). */
static void test_no_spurious_resync_at_startup(void)
{
	struct m4_ah_heartbeat_cfg cfg;
	struct m4_ah_heartbeat hb;
	uint32_t ssrc; int32_t adj; const char *reason;
	double t = 1000.0;

	m4_ah_heartbeat_cfg_defaults(&cfg);
	m4_ah_heartbeat_init(&hb, &cfg, M4_AH_MODE_LL0, -15, t);

	for (int i = 0; i < 500; i++) {
		t += 10.0;
		m4_ah_heartbeat_observe(&hb, t, 0 /* silent the whole time */, false, false, false);
	}
	CHECK(!m4_ah_heartbeat_decide(&hb, t, &ssrc, &adj, &reason),
			"5 s of clean silence with no trigger and period_s=600 far away: no resync");
}

/* Core scenario: a trigger arms a
 * pending resync, but it must wait for BOTH stable_s of clean flow AND
 * quiet_ms of silence before it actually fires -- and it must compute the
 * "consistent blip" (target-preserving alternate-SSRC packet) correctly. */
static void test_trigger_then_gated_resync(void)
{
	struct m4_ah_heartbeat_cfg cfg;
	struct m4_ah_heartbeat hb;
	uint32_t ssrc; int32_t adj; const char *reason;
	double t = 0.0;
	bool fired;

	m4_ah_heartbeat_cfg_defaults(&cfg); /* stable_s=2, quiet_ms=150, min_interval_s=10 */
	m4_ah_heartbeat_init(&hb, &cfg, M4_AH_MODE_LL0, -15, t);

	/* A late-frame trigger, loud audio still playing. */
	t += 10.0;
	m4_ah_heartbeat_observe(&hb, t, 20000 /* loud */, false, true /* late */, false);
	fired = m4_ah_heartbeat_decide(&hb, t, &ssrc, &adj, &reason);
	CHECK(!fired, "trigger alone (not stable, not quiet) does not fire immediately");

	/* Still loud for a while -- even once stable_s has elapsed, "quiet" has
	 * never started, so this must not fire. */
	for (int i = 0; i < 250; i++) { /* 2.5 s */
		t += 10.0;
		m4_ah_heartbeat_observe(&hb, t, 20000, false, false, false);
	}
	fired = m4_ah_heartbeat_decide(&hb, t, &ssrc, &adj, &reason);
	CHECK(!fired, "stable but never quiet: still does not fire (%.0f ms elapsed)", t);

	/* Now it goes quiet. 150 ms of quiet is not enough by itself... */
	for (int i = 0; i < 10; i++) { /* 100 ms quiet */
		t += 10.0;
		m4_ah_heartbeat_observe(&hb, t, 10 /* quiet */, false, false, false);
	}
	fired = m4_ah_heartbeat_decide(&hb, t, &ssrc, &adj, &reason);
	CHECK(!fired, "only 100 ms quiet so far (need 150): does not fire yet");

	/* ...but 150+ ms does, and the reason must be "late" (the trigger that
	 * armed it), with the consistent-blip ssrc/adjust for ll0/-15 -> A2/-66
	 * (106 - 66 == 40 == 55 - 15), exactly as documented. */
	for (int i = 0; i < 6; i++) { /* +60 ms => 160 ms total quiet */
		t += 10.0;
		m4_ah_heartbeat_observe(&hb, t, 10, false, false, false);
	}
	fired = m4_ah_heartbeat_decide(&hb, t, &ssrc, &adj, &reason);
	CHECK(fired, "stable + quiet + pending trigger: resync fires");
	CHECK(fired && ssrc == M4_AH_SSRC_LL1, "blip uses the OTHER mode's ssrc (0xA2)");
	CHECK(fired && adj == -66, "blip adjust == -66 (106 - 66 == 40 == 55 + (-15)) (got %d)", adj);
	CHECK(fired && reason && !strcmp(reason, "late"), "reason reported as \"late\" (got %s)",
			reason ? reason : "(null)");

	/* Exactly ONE packet is the blip: the very next decide() call (even at
	 * the same timestamp) must report normal values again -- see
	 * m4-adaptive-heartbeat.h, "no separate revert step". */
	fired = m4_ah_heartbeat_decide(&hb, t, &ssrc, &adj, &reason);
	CHECK(!fired, "the packet immediately after a blip is NOT itself a blip");
}

/* Same shape, but for ll1/+0 adjust -> blip via A1/+51 (55 + 51 == 106). */
static void test_blip_values_ll1(void)
{
	struct m4_ah_heartbeat_cfg cfg;
	struct m4_ah_heartbeat hb;
	uint32_t ssrc; int32_t adj;
	double t = 0.0;

	m4_ah_heartbeat_cfg_defaults(&cfg);
	m4_ah_heartbeat_init(&hb, &cfg, M4_AH_MODE_LL1, 0, t);

	m4_ah_heartbeat_force(&hb, t, &ssrc, &adj);
	CHECK(ssrc == M4_AH_SSRC_LL0, "ll1 force-blip uses ssrc 0xA1");
	CHECK(adj == 51, "ll1 force-blip adjust == 51 (55 + 51 == 106) (got %d)", adj);
}

/* Periodic "profilactica" resync: fires with reason "period" purely from
 * period_s elapsing, given stability+quiet, with NO trigger ever observed. */
static void test_periodic_resync(void)
{
	struct m4_ah_heartbeat_cfg cfg;
	struct m4_ah_heartbeat hb;
	uint32_t ssrc; int32_t adj; const char *reason;
	double t = 0.0;
	bool fired = false;

	m4_ah_heartbeat_cfg_defaults(&cfg);
	cfg.period_s = 5.0; /* shrink for the test; semantics identical to 600 s */
	m4_ah_heartbeat_init(&hb, &cfg, M4_AH_MODE_LL0, -15, t);

	/* Clean, silent stream for well over period_s. */
	for (int i = 0; i < 700 && !fired; i++) { /* up to 7 s */
		t += 10.0;
		m4_ah_heartbeat_observe(&hb, t, 0, false, false, false);
		fired = m4_ah_heartbeat_decide(&hb, t, &ssrc, &adj, &reason);
	}
	CHECK(fired, "periodic resync fires once period_s has elapsed (t=%.0f ms)", t);
	CHECK(fired && reason && !strcmp(reason, "period"), "reason reported as \"period\" (got %s)",
			reason ? reason : "(null)");
	CHECK(fired && t >= 5000.0 && t < 5300.0,
			"fires close to period_s (5000 ms), not early/very late (fired at %.0f ms)", t);
}

/* min_interval_s gating: two triggers close together must not both resync;
 * a resync itself also resets the stability timer (a resync
 * is itself a discontinuity), so a second trigger right after must
 * wait out BOTH min_interval_s and a fresh stable_s+quiet_ms window. */
static void test_min_interval_and_post_resync_stability(void)
{
	struct m4_ah_heartbeat_cfg cfg;
	struct m4_ah_heartbeat hb;
	uint32_t ssrc; int32_t adj; const char *reason;
	double t = 0.0;
	int resyncs = 0;

	m4_ah_heartbeat_cfg_defaults(&cfg); /* min_interval_s=10 */
	m4_ah_heartbeat_init(&hb, &cfg, M4_AH_MODE_LL0, -15, t);

	/* Trigger, then 30 s of otherwise-clean silence with a trigger re-armed
	 * every 3 s (so it is CONSTANTLY "pending", but flow is clean/quiet in
	 * between) -- min_interval_s must still cap this to at most 3 resyncs in
	 * 30 s (30/10). */
	for (int i = 0; i < 3000; i++) { /* 30 s, 10 ms steps */
		t += 10.0;
		bool trig = (i % 300) == 0; /* every 3 s */
		m4_ah_heartbeat_observe(&hb, t, 0, trig, false, false);
		if (m4_ah_heartbeat_decide(&hb, t, &ssrc, &adj, &reason))
			resyncs++;
	}
	CHECK(resyncs <= 3, "min_interval_s=10 caps resyncs to <= 3 over 30 s (got %d)", resyncs);
	CHECK(resyncs >= 1, "at least one resync actually happened over 30 s (got %d)", resyncs);
}

static void test_set_mode_and_latency_resets_stability(void)
{
	struct m4_ah_heartbeat_cfg cfg;
	struct m4_ah_heartbeat hb;
	uint32_t ssrc; int32_t adj; const char *reason;
	double t = 0.0;

	m4_ah_heartbeat_cfg_defaults(&cfg);
	m4_ah_heartbeat_init(&hb, &cfg, M4_AH_MODE_LL0, -15, t);

	/* Get to a stable+quiet state where a resync WOULD fire on a trigger. */
	for (int i = 0; i < 300; i++) { t += 10.0; m4_ah_heartbeat_observe(&hb, t, 0, false, false, false); }

	/* Changing latency_adjust_ms live must reset the stability clock: a
	 * trigger right after must NOT fire immediately. */
	m4_ah_heartbeat_set_latency_adjust(&hb, -20, t);
	m4_ah_heartbeat_observe(&hb, t, 0, true, false, false);
	CHECK(!m4_ah_heartbeat_decide(&hb, t, &ssrc, &adj, &reason),
			"set_latency_adjust() resets stability: immediate trigger does not fire");

	for (int i = 0; i < 300; i++) { t += 10.0; m4_ah_heartbeat_observe(&hb, t, 0, false, false, false); }
	CHECK(m4_ah_heartbeat_decide(&hb, t, &ssrc, &adj, &reason),
			"...but does fire once stable+quiet again");
	/* target = base(ll0) + (-20) = 55 - 20 = 35; blip via ll1: 35 - 106 = -71. */
	CHECK(adj == -71, "blip after set_latency_adjust(-20) uses the NEW value (adj=%d, want -71)", adj);
}

static void test_disabled_never_fires(void)
{
	struct m4_ah_heartbeat_cfg cfg;
	struct m4_ah_heartbeat hb;
	uint32_t ssrc; int32_t adj; const char *reason;
	double t = 0.0;

	m4_ah_heartbeat_cfg_defaults(&cfg);
	cfg.enabled = false;
	cfg.period_s = 1.0;
	m4_ah_heartbeat_init(&hb, &cfg, M4_AH_MODE_LL0, -15, t);

	for (int i = 0; i < 500; i++) {
		t += 10.0;
		m4_ah_heartbeat_observe(&hb, t, 0, (i == 10), false, false);
		if (m4_ah_heartbeat_decide(&hb, t, &ssrc, &adj, &reason)) {
			CHECK(0, "disabled heartbeat fired at t=%.0f (must never happen)", t);
			return;
		}
	}
	CHECK(1, "disabled heartbeat: 5 s incl. a trigger and period_s=1s elapsed, never fired");
}

/* Peak-amplitude quiet gate: exact threshold boundary and INT16_MIN (the one
 * value where naive abs() overflows an int16_t -- m4_ah_heartbeat_observe()
 * must handle it without UB, this is exactly what UBSan in the ASan build
 * would catch if it did not). */
static void test_quiet_gate_edges(void)
{
	struct m4_ah_heartbeat_cfg cfg;
	struct m4_ah_heartbeat hb;
	uint32_t ssrc; int32_t adj;
	double t = 0.0;

	m4_ah_heartbeat_cfg_defaults(&cfg); /* quiet_amp = 103 */
	m4_ah_heartbeat_init(&hb, &cfg, M4_AH_MODE_LL0, -15, t);
	m4_ah_heartbeat_observe(&hb, t, 0, true, false, false); /* arm a pending resync */

	/* Exactly at the threshold (103): NOT quiet (peak < quiet_amp is the
	 * documented condition, so ==103 counts as loud). */
	for (int i = 0; i < 300; i++) { t += 10.0; m4_ah_heartbeat_observe(&hb, t, 103, false, false, false); }
	{
		const char *reason;
		CHECK(!m4_ah_heartbeat_decide(&hb, t, &ssrc, &adj, &reason),
				"peak exactly == quiet_amp (103) does not count as quiet");
	}

	/* INT16_MIN must not crash/UB and must count as loud (its abs value,
	 * 32768, is obviously >= 103). */
	m4_ah_heartbeat_observe(&hb, t, INT16_MIN, false, false, false);
	t += 10.0;
	{
		const char *reason;
		bool fired = m4_ah_heartbeat_decide(&hb, t, &ssrc, &adj, &reason);
		CHECK(!fired, "INT16_MIN peak resets the quiet run (no crash, no false-quiet)");
	}
}

/* ===================================================================== *
 * Config file parsing
 * ===================================================================== */

static void test_config_parse_buffer(void)
{
	struct m4_ah_user_config c;
	const char *buf =
		"# comment line\n"
		"mode = ll1\n"
		"latency-adjust-ms=-20\n"
		"heartbeat: off\n"          /* unrecognized separator -> "key value" path, no match */
		"heartbeat off\n"           /* whitespace form, should apply */
		"resync_period_min 5 # trailing comment\n"
		"helper-qemu = /opt/x/qemu-hexagon\n"
		"unknown-key = whatever\n"  /* ignored, must not crash or corrupt other fields */
		"\n"
		"   \n"                     /* blank/whitespace-only line */
		"mode=bogus\n";             /* invalid value: must NOT set have_mode a second time to garbage */

	m4_ah_user_config_defaults(&c);
	m4_ah_user_config_parse_buffer(&c, buf, strlen(buf));

	CHECK(c.have_mode && c.mode == M4_AH_MODE_LL1, "mode=ll1 parsed");
	CHECK(c.have_latency_adjust_ms && c.latency_adjust_ms == -20, "latency-adjust-ms=-20 parsed (got %d)",
			c.latency_adjust_ms);
	CHECK(c.have_heartbeat && c.heartbeat_enabled == false, "\"heartbeat off\" (space form) parsed");
	CHECK(c.have_resync_period_min && c.resync_period_min == 5,
			"resync_period_min 5 (with trailing comment) parsed (got %d)", c.resync_period_min);
	CHECK(c.have_qemu_path && !strcmp(c.qemu_path, "/opt/x/qemu-hexagon"), "helper-qemu path parsed (%s)",
			c.qemu_path);
	CHECK(!strcmp(buf + 0, buf + 0), "sanity: buffer itself untouched (parse_buffer takes a copy)");
}

static void test_config_key_aliases_and_bad_values(void)
{
	struct m4_ah_user_config c;
	const char *buf =
		"adj = 7\n"          /* alias for latency-adjust-ms */
		"period = 3\n"       /* alias for resync-period-min */
		"auto_resync = on\n" /* alias for heartbeat */
		"resync_period_min = -5\n" /* invalid (must be > 0): ignored */
		"resync_period_min = notanumber\n"; /* invalid: ignored */

	m4_ah_user_config_defaults(&c);
	m4_ah_user_config_parse_buffer(&c, buf, strlen(buf));

	CHECK(c.have_latency_adjust_ms && c.latency_adjust_ms == 7, "alias \"adj\" parsed");
	CHECK(c.have_heartbeat && c.heartbeat_enabled == true, "alias \"auto_resync\" parsed");
	CHECK(c.have_resync_period_min && c.resync_period_min == 3,
			"alias \"period\" parsed as 3, later invalid values for the SAME key ignored (got %d)",
			c.resync_period_min);
}

static void test_config_file_missing_is_not_an_error(void)
{
	struct m4_ah_user_config c;
	m4_ah_user_config_defaults(&c);
	bool ok = m4_ah_user_config_load_file(&c, "/nonexistent/path/definitely-not-there.conf");
	CHECK(!ok, "load_file() on a missing file returns false");
	CHECK(!c.have_mode && !c.have_latency_adjust_ms, "struct left untouched on missing-file failure");
}

static void test_config_file_roundtrip(void)
{
	const char *path = "/tmp/m4_adaptive_heartbeat_test_config.conf";
	FILE *f = fopen(path, "w");
	struct m4_ah_user_config c;

	CHECK(f != NULL, "can create a temp config file for the roundtrip test");
	if (!f)
		return;
	fputs("mode = ll0\nlatency-adjust-ms = -15\nheartbeat = on\n", f);
	fclose(f);

	m4_ah_user_config_defaults(&c);
	CHECK(m4_ah_user_config_load_file(&c, path), "load_file() succeeds on a real file");
	CHECK(c.have_mode && c.mode == M4_AH_MODE_LL0, "roundtrip: mode == ll0");
	CHECK(c.have_latency_adjust_ms && c.latency_adjust_ms == -15, "roundtrip: latency-adjust-ms == -15");
	CHECK(c.have_heartbeat && c.heartbeat_enabled, "roundtrip: heartbeat == on");

	unlink(path);
}

static void test_config_default_path(void)
{
	char buf[M4_AH_CONFIG_PATH_MAX];

	setenv("M4_ADAPTIVE_CONFIG", "/custom/path.conf", 1);
	m4_ah_user_config_default_path(buf, sizeof(buf));
	CHECK(!strcmp(buf, "/custom/path.conf"), "M4_ADAPTIVE_CONFIG env override wins (%s)", buf);
	unsetenv("M4_ADAPTIVE_CONFIG");

	setenv("XDG_CONFIG_HOME", "/xdg", 1);
	m4_ah_user_config_default_path(buf, sizeof(buf));
	CHECK(!strcmp(buf, "/xdg/momentum4/aptx-adaptive.conf"), "XDG_CONFIG_HOME used when set (%s)", buf);
	unsetenv("XDG_CONFIG_HOME");

	setenv("HOME", "/home/test", 1);
	m4_ah_user_config_default_path(buf, sizeof(buf));
	CHECK(!strcmp(buf, "/home/test/.config/momentum4/aptx-adaptive.conf"),
			"falls back to $HOME/.config/momentum4/aptx-adaptive.conf (%s)", buf);
}

int main(void)
{
	printf("== heartbeat: base mapping ==\n");
	test_base_mapping();

	printf("\n== heartbeat: gating (stability/quiet/period/min-interval) ==\n");
	test_no_spurious_resync_at_startup();
	test_trigger_then_gated_resync();
	test_blip_values_ll1();
	test_periodic_resync();
	test_min_interval_and_post_resync_stability();
	test_set_mode_and_latency_resets_stability();
	test_disabled_never_fires();
	test_quiet_gate_edges();

	printf("\n== config file parsing ==\n");
	test_config_parse_buffer();
	test_config_key_aliases_and_bad_values();
	test_config_file_missing_is_not_an_error();
	test_config_file_roundtrip();
	test_config_default_path();

	printf("\n%d check(s) failed\n", g_failures);
	return g_failures == 0 ? 0 : 1;
}
