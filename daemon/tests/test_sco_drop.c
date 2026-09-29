/* tests/test_sco_drop.c -- M4 closes the HFP audio link
 * (double tap in HFP), patch 0008 in our libspa-bluez5 restores it and sends
 * SCODROP / SCOUP / SCOFAIL over hfp-events.sock; momentum4d applies the
 * `hfp-sco-drop` policy (restore | call-off | mic-toggle | none).
 *
 * Covers: the pure decision table (control/gesture.c), the setting
 * (util/settings.c), the wire parser (ipc/hfp_events.c) and the daemon
 * reaction against the fake graph (core/graph_ops_fake.c) -- same fixture
 * style as tests/test_gestures.c. No PipeWire, no D-Bus, no Bluetooth. */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <systemd/sd-event.h>

#include "control/gesture.h"
#include "control/passthrough.h"
#include "core/graph_ops_fake.h"
#include "core/state.h"
#include "daemon/daemon.h"
#include "ipc/hfp_events.h"
#include "util/settings.h"

static int failures = 0;
static int checks = 0;

#define CHECK(cond, msg)                                                          \
	do {                                                                      \
		checks++;                                                         \
		if (!(cond)) {                                                    \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, (msg)); \
			failures++;                                               \
		}                                                                 \
	} while (0)

#define M4_TEST_MAC "00:1B:66:00:00:01"

/* --- pure parts ------------------------------------------------------------ */

static void test_decide_table(void)
{
	/* restore (default) */
	CHECK(m4_sco_drop_decide(M4_SCO_DROP_RESTORE, M4_SCO_EVENT_DROP) == M4_SCO_STEP_NOTHING, "restore/drop");
	CHECK(m4_sco_drop_decide(M4_SCO_DROP_RESTORE, M4_SCO_EVENT_UP) == M4_SCO_STEP_EARCON_RESTORED, "restore/up");
	CHECK(m4_sco_drop_decide(M4_SCO_DROP_RESTORE, M4_SCO_EVENT_FAIL) == M4_SCO_STEP_CALL_OFF_FALLBACK,
	      "restore/fail -> fallback");
	/* call-off */
	CHECK(m4_sco_drop_decide(M4_SCO_DROP_CALL_OFF, M4_SCO_EVENT_DROP) == M4_SCO_STEP_CALL_OFF, "call-off/drop");
	CHECK(m4_sco_drop_decide(M4_SCO_DROP_CALL_OFF, M4_SCO_EVENT_UP) == M4_SCO_STEP_NOTHING, "call-off/up");
	CHECK(m4_sco_drop_decide(M4_SCO_DROP_CALL_OFF, M4_SCO_EVENT_FAIL) == M4_SCO_STEP_CALL_OFF_FALLBACK,
	      "call-off/fail");
	/* mic-toggle */
	CHECK(m4_sco_drop_decide(M4_SCO_DROP_MIC_TOGGLE, M4_SCO_EVENT_DROP) == M4_SCO_STEP_NOTHING, "mic/drop");
	CHECK(m4_sco_drop_decide(M4_SCO_DROP_MIC_TOGGLE, M4_SCO_EVENT_UP) == M4_SCO_STEP_MIC_TOGGLE, "mic/up");
	CHECK(m4_sco_drop_decide(M4_SCO_DROP_MIC_TOGGLE, M4_SCO_EVENT_FAIL) == M4_SCO_STEP_CALL_OFF_FALLBACK,
	      "mic/fail");
	/* none */
	CHECK(m4_sco_drop_decide(M4_SCO_DROP_NONE, M4_SCO_EVENT_DROP) == M4_SCO_STEP_NOTHING, "none/drop");
	CHECK(m4_sco_drop_decide(M4_SCO_DROP_NONE, M4_SCO_EVENT_UP) == M4_SCO_STEP_NOTHING, "none/up");
	CHECK(m4_sco_drop_decide(M4_SCO_DROP_NONE, M4_SCO_EVENT_FAIL) == M4_SCO_STEP_NOTHING, "none/fail");
}

static void test_policy_names_and_settings(void)
{
	m4_sco_drop_policy_t p;
	struct m4_settings s;

	for (int i = 0; i < M4_SCO_DROP_COUNT; i++) {
		CHECK(m4_sco_drop_policy_parse(m4_sco_drop_policy_name((m4_sco_drop_policy_t)i), &p) == 0 &&
			      p == (m4_sco_drop_policy_t)i,
		      "name round-trip");
	}
	CHECK(m4_sco_drop_policy_parse("CALL_OFF", &p) == 0 && p == M4_SCO_DROP_CALL_OFF, "case + underscore");
	CHECK(m4_sco_drop_policy_parse("call-toggle", &p) < 0, "call-toggle is not a sco policy");
	CHECK(m4_sco_drop_policy_parse("", &p) < 0, "empty");
	CHECK(m4_sco_drop_policy_parse(NULL, &p) < 0, "NULL");
	CHECK(m4_sco_drop_policy_parse("restore-restore-restore-restore-x", &p) < 0, "too long");

	m4_settings_defaults(&s);
	CHECK(s.hfp_sco_drop == M4_SCO_DROP_RESTORE, "default restore");
	m4_settings_parse(&s, "hfp-sco-drop = call-off\n", "t");
	CHECK(s.hfp_sco_drop == M4_SCO_DROP_CALL_OFF && s.n_warnings == 0, "hfp-sco-drop = call-off");
	m4_settings_parse(&s, "hfp_sco_drop = mic-toggle # comment\n", "t");
	CHECK(s.hfp_sco_drop == M4_SCO_DROP_MIC_TOGGLE && s.n_warnings == 0, "underscore key + comment");
	m4_settings_parse(&s, "hfp-sco-drop = bogus\n", "t");
	CHECK(s.hfp_sco_drop == M4_SCO_DROP_MIC_TOGGLE && s.n_warnings == 1, "invalid value: warning, kept");
}

static void test_wire_parse(void)
{
	struct m4_hfp_event ev;
	m4_hfp_verb_t v;

	CHECK(m4_hfp_event_parse("SCODROP " M4_TEST_MAC " 1759100139974", &ev) == 0 &&
		      ev.verb == M4_HFP_VERB_SCODROP && strcmp(ev.bdaddr, M4_TEST_MAC) == 0 &&
		      ev.wire_realtime_usec == 1759100139974ULL * 1000ULL,
	      "SCODROP parsed");
	CHECK(m4_hfp_event_parse("SCOUP " M4_TEST_MAC " 1\n", &ev) == 0 && ev.verb == M4_HFP_VERB_SCOUP,
	      "SCOUP parsed");
	CHECK(m4_hfp_event_parse("SCOFAIL " M4_TEST_MAC " 2", &ev) == 0 && ev.verb == M4_HFP_VERB_SCOFAIL,
	      "SCOFAIL parsed");
	CHECK(m4_hfp_event_parse("SCODROP " M4_TEST_MAC, &ev) < 0, "missing ts rejected");
	CHECK(m4_hfp_event_parse("SCODROP " M4_TEST_MAC " 1 extra", &ev) < 0, "extra field rejected");
	CHECK(m4_hfp_event_parse("SCOSOMETHING " M4_TEST_MAC " 1", &ev) < 0, "unknown verb rejected");
	CHECK(m4_hfp_verb_is_sco(M4_HFP_VERB_SCODROP) && m4_hfp_verb_is_sco(M4_HFP_VERB_SCOUP) &&
		      m4_hfp_verb_is_sco(M4_HFP_VERB_SCOFAIL) && !m4_hfp_verb_is_sco(M4_HFP_VERB_CHUP) &&
		      !m4_hfp_verb_is_sco(M4_HFP_VERB_RAW),
	      "is_sco");
	CHECK(m4_hfp_verb_from_name("SCOFAIL", &v) == 0 && v == M4_HFP_VERB_SCOFAIL, "from_name");
	CHECK(m4_hfp_verb_from_name("scofail", &v) < 0, "from_name is exact");
	CHECK(strcmp(m4_hfp_verb_name(M4_HFP_VERB_SCOUP), "SCOUP") == 0, "name");
}

/* --- daemon, fake graph ------------------------------------------------------ */

struct fake_earcon {
	int on, off, error, mic_on, mic_off;
};

static void fake_earcon(void *ctx, const char *dir, const char *kind)
{
	struct fake_earcon *e = ctx;

	(void)dir;
	if (strcmp(kind, "call-on") == 0)
		e->on++;
	else if (strcmp(kind, "call-off") == 0)
		e->off++;
	else if (strcmp(kind, "call-error") == 0)
		e->error++;
	else if (strcmp(kind, "mic-on") == 0)
		e->mic_on++;
	else if (strcmp(kind, "mic-off") == 0)
		e->mic_off++;
}

static int fake_forward(void *ctx, const char *method, char *target, size_t cap)
{
	(void)ctx;
	(void)method;
	snprintf(target, cap, "none");
	return -1;
}

struct fixture {
	sd_event *event;
	m4_graph_ops_fake_t fake;
	char runtime_dir[128];
	char config_path[192];
	struct m4_settings settings;
	struct m4_passthrough_ops pt_ops;
	struct fake_earcon earcon;
	struct m4_daemon *d;
};

static void fixture_setup_mode(struct fixture *fx, const char *settings_text, m4_daemon_mode_t mode)
{
	struct m4_daemon_config cfg = {0};
	char combined[1024];

	memset(fx, 0, sizeof(*fx));
	CHECK(sd_event_new(&fx->event) >= 0, "sd_event_new");
	m4_graph_ops_fake_init(&fx->fake);
	snprintf(fx->runtime_dir, sizeof(fx->runtime_dir), "/tmp/m4d-test-sco.XXXXXX");
	CHECK(mkdtemp(fx->runtime_dir) != NULL, "mkdtemp");
	snprintf(fx->config_path, sizeof(fx->config_path), "%s/momentum4.conf", fx->runtime_dir);

	/* synchronous call-on/off earcons, like test_gestures.c's fixture */
	snprintf(combined, sizeof(combined), "earcon-delay-on-ms = 0\nearcon-delay-off-ms = 0\n%s",
		 settings_text ? settings_text : "");
	m4_settings_defaults(&fx->settings);
	m4_settings_parse(&fx->settings, combined, "fixture");

	fx->pt_ops.ctx = NULL;
	fx->pt_ops.forward = fake_forward;

	cfg.graph_ops = m4_graph_ops_fake(&fx->fake);
	cfg.mac_address = M4_TEST_MAC;
	cfg.runtime_dir = fx->runtime_dir;
	cfg.stats_path = "";
	cfg.confirm_timeout_ms = 300;
	cfg.has_initial_mode = true;
	cfg.initial_mode = mode;
	cfg.settings = &fx->settings;
	cfg.config_path = fx->config_path;
	cfg.passthrough = &fx->pt_ops;
	cfg.earcon = fake_earcon;
	cfg.earcon_ctx = &fx->earcon;

	fx->d = m4_daemon_new(fx->event, &cfg);
	CHECK(fx->d != NULL, "daemon_new");
}

static void fixture_setup(struct fixture *fx, const char *settings_text)
{
	fixture_setup_mode(fx, settings_text, M4_DAEMON_MODE_MANUAL);
}

static void fixture_teardown(struct fixture *fx)
{
	char cmd[256];

	m4_daemon_free(fx->d);
	sd_event_unref(fx->event);
	snprintf(cmd, sizeof(cmd), "rm -rf -- '%s'", fx->runtime_dir);
	if (system(cmd) != 0)
		fprintf(stderr, "warn: cleanup of %s failed\n", fx->runtime_dir);
}

static void hfp(struct fixture *fx, m4_hfp_verb_t verb, const char *mac, double now)
{
	struct m4_hfp_event ev;

	memset(&ev, 0, sizeof(ev));
	ev.verb = verb;
	snprintf(ev.bdaddr, sizeof(ev.bdaddr), "%s", mac);
	ev.wire_realtime_usec = 1759100000ULL * 1000000ULL;
	ev.realtime_usec = ev.wire_realtime_usec;
	ev.monotonic_s = now;
	m4_daemon_hfp_event(fx->d, &ev);
}

static const char *profile(struct fixture *fx)
{
	return fx->fake.profile_name;
}

/* call on through ctl.sock (the way `momentum4 call on` does it) */
static void call_on(struct fixture *fx)
{
	char resp[512];

	m4_daemon_handle_request(fx->d, "CALL ON", resp, sizeof(resp));
	CHECK(strcmp(profile(fx), "headset-head-unit") == 0, "call on -> headset-head-unit");
}

static void pump_until(struct fixture *fx, const int *counter, int want)
{
	for (int i = 0; i < 400 && *counter < want; i++)
		sd_event_run(fx->event, 5 * 1000);
}

/* default policy: drop -> nothing (patch restores), up -> earcon, profile untouched */
static void test_restore_default(void)
{
	struct fixture fx;
	int calls;

	fixture_setup(&fx, NULL);
	call_on(&fx);
	calls = fx.fake.set_profile_calls;
	fx.earcon.on = 0;

	hfp(&fx, M4_HFP_VERB_SCODROP, M4_TEST_MAC, 10.0);
	CHECK(strcmp(profile(&fx), "headset-head-unit") == 0, "drop: still HFP");
	CHECK(fx.fake.set_profile_calls == calls, "drop: no profile write");
	CHECK(fx.earcon.on == 0 && fx.earcon.error == 0 && fx.earcon.off == 0, "drop: no earcon");

	hfp(&fx, M4_HFP_VERB_SCOUP, M4_TEST_MAC, 10.3);
	CHECK(fx.earcon.on == 0, "up: earcon not synchronous (150 ms settle)");
	pump_until(&fx, &fx.earcon.on, 1);
	CHECK(fx.earcon.on == 1, "up: call-on earcon = audio back");
	CHECK(strcmp(profile(&fx), "headset-head-unit") == 0, "up: still HFP");
	CHECK(fx.fake.set_profile_calls == calls, "up: no profile write");

	fixture_teardown(&fx);
}

/* restore failed -> fallback to A2DP so sound comes back */
static void test_restore_fail_fallback(void)
{
	struct fixture fx;

	fixture_setup(&fx, NULL);
	call_on(&fx);
	fx.earcon.error = 0;
	fx.earcon.off = 0;

	hfp(&fx, M4_HFP_VERB_SCODROP, M4_TEST_MAC, 10.0);
	hfp(&fx, M4_HFP_VERB_SCOFAIL, M4_TEST_MAC, 13.3);
	CHECK(strcmp(profile(&fx), "a2dp-sink") == 0, "fail: back to a2dp-sink");
	CHECK(fx.earcon.off == 1, "fail: call-off earcon");
	CHECK(fx.earcon.error == 0, "fail: no error tone into a dead link");

	fixture_teardown(&fx);
}

/* hfp-sco-drop = call-off: the double tap leaves call mode at once;
 * a late SCOFAIL (link was released by the switch) must not do anything
 * harmful -- it lands in the cooldown or finds no call. */
static void test_policy_call_off(void)
{
	struct fixture fx;
	int calls;

	fixture_setup(&fx, "hfp-sco-drop = call-off\n");
	call_on(&fx);

	hfp(&fx, M4_HFP_VERB_SCODROP, M4_TEST_MAC, 10.0);
	CHECK(strcmp(profile(&fx), "a2dp-sink") == 0, "call-off: drop -> a2dp-sink");
	calls = fx.fake.set_profile_calls;
	hfp(&fx, M4_HFP_VERB_SCOUP, M4_TEST_MAC, 10.2);
	CHECK(fx.fake.set_profile_calls == calls && strcmp(profile(&fx), "a2dp-sink") == 0,
	      "call-off: stray up does nothing");
	hfp(&fx, M4_HFP_VERB_SCOFAIL, M4_TEST_MAC, 10.4);
	CHECK(strcmp(profile(&fx), "a2dp-sink") == 0, "call-off: stray fail keeps a2dp-sink");

	fixture_teardown(&fx);
}

/* CHUP (swipe back) and the SCO drop M4 may send around it share the cooldown */
static void test_call_off_policy_shares_cooldown_with_chup(void)
{
	struct fixture fx;
	int calls;

	fixture_setup(&fx, "hfp-sco-drop = call-off\n");
	call_on(&fx);
	hfp(&fx, M4_HFP_VERB_CHUP, M4_TEST_MAC, 20.0);
	CHECK(strcmp(profile(&fx), "a2dp-sink") == 0, "CHUP -> a2dp-sink");
	calls = fx.fake.set_profile_calls;
	hfp(&fx, M4_HFP_VERB_SCODROP, M4_TEST_MAC, 20.05);
	CHECK(fx.fake.set_profile_calls == calls, "SCODROP right after CHUP: debounced");

	fixture_teardown(&fx);
}

/* hfp-sco-drop = mic-toggle: restored link + mic toggled */
static void test_policy_mic_toggle(void)
{
	struct fixture fx;
	int before;

	fixture_setup(&fx, "hfp-sco-drop = mic-toggle\n");
	call_on(&fx);
	m4_graph_ops_fake_set_mic_route(&fx.fake, true, 1.0f, false);
	before = fx.fake.set_mic_volume_calls;

	hfp(&fx, M4_HFP_VERB_SCODROP, M4_TEST_MAC, 30.0);
	CHECK(fx.fake.set_mic_volume_calls == before, "mic: nothing on drop");
	hfp(&fx, M4_HFP_VERB_SCOUP, M4_TEST_MAC, 30.3);
	CHECK(fx.fake.set_mic_volume_calls == before + 1, "mic: toggled once on up");
	CHECK(fx.fake.last_set_mic_mute == true, "mic: muted");
	CHECK(strcmp(profile(&fx), "headset-head-unit") == 0, "mic: still HFP");
	hfp(&fx, M4_HFP_VERB_SCOFAIL, M4_TEST_MAC, 40.0);
	CHECK(strcmp(profile(&fx), "a2dp-sink") == 0, "mic: fail -> fallback a2dp-sink");

	fixture_teardown(&fx);
}

/* hfp-sco-drop = none: log only */
static void test_policy_none(void)
{
	struct fixture fx;
	int calls;

	fixture_setup(&fx, "hfp-sco-drop = none\n");
	call_on(&fx);
	calls = fx.fake.set_profile_calls;
	fx.earcon.on = fx.earcon.off = fx.earcon.error = 0;

	hfp(&fx, M4_HFP_VERB_SCODROP, M4_TEST_MAC, 10.0);
	hfp(&fx, M4_HFP_VERB_SCOUP, M4_TEST_MAC, 10.3);
	hfp(&fx, M4_HFP_VERB_SCOFAIL, M4_TEST_MAC, 20.0);
	for (int i = 0; i < 60; i++)
		sd_event_run(fx.event, 5 * 1000);
	CHECK(fx.fake.set_profile_calls == calls, "none: no profile write");
	CHECK(fx.earcon.on == 0 && fx.earcon.off == 0 && fx.earcon.error == 0, "none: silent");

	fixture_teardown(&fx);
}

/* observe mode: the fallback is a write -> blocked like every other one */
static void test_observe_blocks_fallback(void)
{
	struct fixture fx;
	int calls;

	fixture_setup_mode(&fx, NULL, M4_DAEMON_MODE_MANUAL);
	call_on(&fx);
	{
		char resp[512];
		m4_daemon_handle_request(fx.d, "MODE observe", resp, sizeof(resp));
	}
	calls = fx.fake.set_profile_calls;
	hfp(&fx, M4_HFP_VERB_SCOFAIL, M4_TEST_MAC, 50.0);
	CHECK(fx.fake.set_profile_calls == calls, "observe: fallback not executed");
	CHECK(strcmp(profile(&fx), "headset-head-unit") == 0, "observe: still HFP");

	fixture_teardown(&fx);
}

/* another headset's link (patch in mode "all") is not ours */
static void test_foreign_device_ignored(void)
{
	struct fixture fx;
	int calls;

	fixture_setup(&fx, NULL);
	call_on(&fx);
	calls = fx.fake.set_profile_calls;
	hfp(&fx, M4_HFP_VERB_SCOFAIL, "00:11:22:33:44:55", 60.0);
	CHECK(fx.fake.set_profile_calls == calls, "foreign: ignored");
	hfp(&fx, M4_HFP_VERB_SCOFAIL, "00:1b:66:00:00:01", 61.0);
	CHECK(strcmp(profile(&fx), "a2dp-sink") == 0, "own MAC, lower case: handled");

	fixture_teardown(&fx);
}

int main(void)
{
	test_decide_table();
	test_policy_names_and_settings();
	test_wire_parse();
	test_restore_default();
	test_restore_fail_fallback();
	test_policy_call_off();
	test_call_off_policy_shares_cooldown_with_chup();
	test_policy_mic_toggle();
	test_policy_none();
	test_observe_blocks_fallback();
	test_foreign_device_ignored();

	printf("test_sco_drop: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
