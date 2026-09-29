/* tests/test_gestures.c -- daemon side of M4 gestures: gesture -> action
 * map, the same write gate as ctl.sock
 * `call on/off` (observe / safe-latch), cooldown, earcon, passthrough,
 * player-status policy, PLAYER/RELOAD ctl commands, mode from momentum4.conf,
 * and the nested-sd_event_run() regression (-EBUSY) with an ASYNC fake graph.
 * Fake graph (core/graph_ops_fake.c), fake passthrough, earcon counter --
 * no PipeWire, no D-Bus. */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <systemd/sd-event.h>

#include "control/gesture.h"
#include "control/mpris_player.h"
#include "control/passthrough.h"
#include "core/graph_ops_fake.h"
#include "core/state.h"
#include "daemon/daemon.h"
#include "ipc/ctl.h"
#include "ipc/hfp_events.h"
#include "util/settings.h"

#include "../../common/m4-i18n.h"

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

/* --- fakes ---------------------------------------------------------------- */

struct fake_pt {
	int calls;
	int rc;
	char last_method[32];
};

static int fake_forward(void *ctx, const char *method, char *target, size_t cap)
{
	struct fake_pt *pt = ctx;

	pt->calls++;
	snprintf(pt->last_method, sizeof(pt->last_method), "%s", method);
	snprintf(target, cap, "org.mpris.MediaPlayer2.fake (Playing)");
	return pt->rc;
}

struct fake_earcon {
	int on, off, error;
	int mic_on, mic_off; /* "tap = mic" task */
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

struct fixture {
	sd_event *event;
	m4_graph_ops_fake_t fake;
	char runtime_dir[128];
	char config_path[192];
	struct m4_settings settings;
	struct fake_pt pt;
	struct m4_passthrough_ops pt_ops;
	struct fake_earcon earcon;
	struct m4_daemon *d;
};

static void make_tmpdir(char *out, size_t out_len)
{
	snprintf(out, out_len, "/tmp/m4d-test-gestures.XXXXXX");
	CHECK(mkdtemp(out) != NULL, "mkdtemp");
}

/* settings_text: momentum4.conf content applied on top of defaults (NULL = defaults).
 * earcon-delay-on-ms/off-ms default to 400/500 in
 * production (m4_settings_defaults()) -- forced to 0 here (synchronous
 * earcon, matching this file's pre-existing tests, which all assert an
 * earcon fired the instant the triggering call returns, with no event loop
 * pumping in between) UNLESS `settings_text` itself sets one of those two
 * keys, in which case it wins (later `apply()` call for the same key
 * always overrides an earlier one, see util/settings.c) -- used by
 * test_hfp_call_earcon_delay() below, which deliberately configures a
 * non-zero delay and pumps fx->event to observe it. */
static void fixture_setup_ex(struct fixture *fx, const char *settings_text, bool has_initial_mode,
			     m4_daemon_mode_t initial_mode, bool async_graph)
{
	struct m4_daemon_config cfg = {0};
	char combined_settings[1024];

	memset(fx, 0, sizeof(*fx));
	CHECK(sd_event_new(&fx->event) >= 0, "sd_event_new");
	m4_graph_ops_fake_init(&fx->fake);
	fx->fake.async_set_profile = async_graph;
	fx->fake.iterate_enabled = async_graph;
	make_tmpdir(fx->runtime_dir, sizeof(fx->runtime_dir));
	snprintf(fx->config_path, sizeof(fx->config_path), "%s/momentum4.conf", fx->runtime_dir);

	snprintf(combined_settings, sizeof(combined_settings), "earcon-delay-on-ms = 0\nearcon-delay-off-ms = 0\n%s",
		 settings_text ? settings_text : "");

	m4_settings_defaults(&fx->settings);
	m4_settings_parse(&fx->settings, combined_settings, "fixture");

	fx->pt_ops.ctx = &fx->pt;
	fx->pt_ops.forward = fake_forward;

	cfg.graph_ops = m4_graph_ops_fake(&fx->fake);
	cfg.mac_address = M4_TEST_MAC;
	cfg.runtime_dir = fx->runtime_dir;
	cfg.stats_path = "";
	cfg.confirm_timeout_ms = 300;
	cfg.has_initial_mode = has_initial_mode;
	cfg.initial_mode = initial_mode;
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
	fixture_setup_ex(fx, settings_text, true, M4_DAEMON_MODE_MANUAL, false);
}

static void fixture_teardown(struct fixture *fx)
{
	char path[256];

	m4_daemon_free(fx->d);
	sd_event_unref(fx->event);
	snprintf(path, sizeof(path), "rm -rf -- '%s'", fx->runtime_dir);
	if (system(path) != 0)
		fprintf(stderr, "warn: cleanup of %s failed\n", fx->runtime_dir);
}

static void gesture(struct fixture *fx, m4_player_cmd_t cmd, double now)
{
	struct m4_player_event ev;

	memset(&ev, 0, sizeof(ev));
	ev.cmd = cmd;
	snprintf(ev.sender, sizeof(ev.sender), ":1.42");
	ev.realtime_usec = 1759068000ULL * 1000000ULL;
	ev.monotonic_s = now;
	m4_daemon_player_event(fx->d, &ev);
}

static const char *profile(struct fixture *fx)
{
	return fx->fake.profile_name;
}

/* simulates an
 * hfp-events.sock datagram already parsed+timestamped (m4_daemon_hfp_event()
 * is called directly, same convention as gesture() above for
 * m4_daemon_player_event() -- no real socket in this file, that is
 * test_hfp_events.c's job). */
static void hfp_event(struct fixture *fx, m4_hfp_verb_t verb, double now)
{
	struct m4_hfp_event ev;

	memset(&ev, 0, sizeof(ev));
	ev.verb = verb;
	snprintf(ev.bdaddr, sizeof(ev.bdaddr), "%s", M4_TEST_MAC);
	ev.wire_realtime_usec = 1759068000ULL * 1000000ULL;
	ev.realtime_usec = ev.wire_realtime_usec;
	ev.monotonic_s = now;
	m4_daemon_hfp_event(fx->d, &ev);
}

/* --- mode from momentum4.conf ------------------------------------------- */

static void test_mode_from_config_overrides_runtime_file(void)
{
	struct fixture fx;
	struct m4_daemon_status st;
	char dir[128];

	/* a previous run left "observe" in $runtime_dir/mode */
	make_tmpdir(dir, sizeof(dir));
	CHECK(m4_state_mode_write(dir, M4_DAEMON_MODE_OBSERVE) == 0, "seed runtime mode");
	CHECK(m4_state_mode_read(dir) == M4_DAEMON_MODE_OBSERVE, "seeded");

	fixture_setup_ex(&fx, NULL, true, M4_DAEMON_MODE_MANUAL, false);
	m4_daemon_get_status(fx.d, &st);
	CHECK(st.mode == M4_DAEMON_MODE_MANUAL, "config mode manual wins at start");
	CHECK(m4_state_mode_read(fx.runtime_dir) == M4_DAEMON_MODE_MANUAL, "runtime mode file rewritten");
	fixture_teardown(&fx);

	/* back-compat: without has_initial_mode the runtime file is used */
	fixture_setup_ex(&fx, NULL, false, M4_DAEMON_MODE_MANUAL, false);
	m4_daemon_get_status(fx.d, &st);
	CHECK(st.mode == M4_DAEMON_MODE_OBSERVE, "no initial mode -> old default observe");
	fixture_teardown(&fx);
	rmdir(dir);
}

/* --- call-toggle ----------------------------------------------------------- */

static void test_next_toggles_call_with_cooldown_and_earcon(void)
{
	struct fixture fx;
	struct m4_daemon_gesture_status gs;

	fixture_setup(&fx, NULL); /* defaults: next = call-toggle, mode manual */
	CHECK(strcmp(profile(&fx), "a2dp-sink") == 0, "starts in a2dp-sink");

	gesture(&fx, M4_PLAYER_CMD_NEXT, 100.0);
	CHECK(strcmp(profile(&fx), "headset-head-unit") == 0, "Next -> call on (mSBC)");
	CHECK(fx.earcon.on == 1 && fx.earcon.off == 0, "earcon call-on played once");

	gesture(&fx, M4_PLAYER_CMD_NEXT, 100.2); /* 200ms later, within the 300ms default cooldown */
	CHECK(strcmp(profile(&fx), "headset-head-unit") == 0, "second Next within cooldown ignored");
	m4_daemon_get_gesture_status(fx.d, &gs);
	CHECK(gs.ignored == 1, "counted as ignored");
	CHECK(strstr(gs.last_result, "cooldown") != NULL, "result says cooldown");

	gesture(&fx, M4_PLAYER_CMD_NEXT, 102.0);
	CHECK(strcmp(profile(&fx), "a2dp-sink") == 0, "Next after cooldown -> call off (back to AALL)");
	CHECK(fx.earcon.off == 1, "earcon call-off played once");

	m4_daemon_get_gesture_status(fx.d, &gs);
	CHECK(gs.events == 3 && gs.call_actions == 2, "3 events, 2 executed call actions");
	CHECK(fx.pt.calls == 0, "no passthrough for Next");
	fixture_teardown(&fx);
}

static void test_call_on_off_actions_are_idempotent(void)
{
	struct fixture fx;

	fixture_setup(&fx, "gesture-cooldown-ms = 0\ngesture-play = call-on\ngesture-pause = call-off\n");
	gesture(&fx, M4_PLAYER_CMD_PLAY, 10.0);
	CHECK(strcmp(profile(&fx), "headset-head-unit") == 0, "Play -> call-on");
	gesture(&fx, M4_PLAYER_CMD_PLAY, 11.0);
	CHECK(strcmp(profile(&fx), "headset-head-unit") == 0, "second call-on keeps the call");
	CHECK(fx.earcon.on == 1, "idempotent call-on plays no second earcon");
	gesture(&fx, M4_PLAYER_CMD_PAUSE, 12.0);
	CHECK(strcmp(profile(&fx), "a2dp-sink") == 0, "Pause -> call-off");
	fixture_teardown(&fx);
}

static void test_observe_and_safe_latch_block_gesture_writes(void)
{
	struct fixture fx;
	struct m4_daemon_gesture_status gs;
	char path[256];
	FILE *f;

	fixture_setup_ex(&fx, NULL, true, M4_DAEMON_MODE_OBSERVE, false);
	gesture(&fx, M4_PLAYER_CMD_NEXT, 5.0);
	CHECK(fx.fake.set_profile_calls == 0, "observe: no set_profile");
	m4_daemon_get_gesture_status(fx.d, &gs);
	CHECK(gs.ignored == 1 && strstr(gs.last_result, "observe") != NULL, "observe: logged as not executed");
	CHECK(fx.earcon.on == 0, "observe: no call-on earcon");
	CHECK(fx.earcon.error == 1, "observe: call-error earcon played");
	fixture_teardown(&fx);

	fixture_setup(&fx, NULL);
	snprintf(path, sizeof(path), "%s/safe-latch", fx.runtime_dir);
	f = fopen(path, "w");
	CHECK(f != NULL, "create safe-latch");
	if (f != NULL)
		fclose(f);
	gesture(&fx, M4_PLAYER_CMD_NEXT, 5.0);
	CHECK(fx.fake.set_profile_calls == 0, "safe-latch: no set_profile");
	CHECK(fx.earcon.on == 0, "safe-latch: no earcon");
	CHECK(fx.earcon.error == 1, "safe-latch: call-error earcon played");
	fixture_teardown(&fx);
}

/* (call-error on failure, but not on a cooldown hit): the
 * cooldown-ignored case (already exercised by
 * test_next_toggles_call_with_cooldown_and_earcon() above, which asserts
 * gs.ignored == 1 for it) must NOT play call-error -- it is expected
 * debounce, not a failure. */
static void test_cooldown_does_not_play_error_earcon(void)
{
	struct fixture fx;

	fixture_setup(&fx, NULL); /* defaults: next = call-toggle */
	gesture(&fx, M4_PLAYER_CMD_NEXT, 100.0);
	CHECK(fx.earcon.on == 1 && fx.earcon.error == 0, "first Next: call-on, no error");
	gesture(&fx, M4_PLAYER_CMD_NEXT, 100.1); /* well within the 300ms default cooldown */
	CHECK(fx.earcon.on == 1, "second Next (cooldown): no extra call-on");
	CHECK(fx.earcon.error == 0, "second Next (cooldown): NOT treated as an error");
	fixture_teardown(&fx);
}

/* (the earcon fires after profile confirmation, with a delay
 * -- earcon-delay-on-ms/off-ms, 400/500 by default): the success earcon must
 * NOT fire synchronously when a non-zero delay is configured, and MUST
 * fire once the configured delay has elapsed (pumping fx.event, since
 * daemon_schedule_earcon() uses a real sd-event oneshot timer -- see
 * daemon.c). */
static void test_earcon_delay_is_not_synchronous(void)
{
	struct fixture fx;
	int iterations;

	fixture_setup(&fx, "earcon-delay-on-ms = 50\nearcon-delay-off-ms = 80\ngesture-cooldown-ms = 0\n");

	gesture(&fx, M4_PLAYER_CMD_NEXT, 1.0); /* -> call on */
	CHECK(strcmp(profile(&fx), "headset-head-unit") == 0, "profile switched immediately");
	CHECK(fx.earcon.on == 0, "call-on earcon NOT played synchronously when delay > 0");

	iterations = 0;
	while (fx.earcon.on == 0 && iterations < 2000) {
		sd_event_run(fx.event, 5 * 1000);
		iterations++;
	}
	CHECK(fx.earcon.on == 1, "call-on earcon eventually played after the configured delay");

	gesture(&fx, M4_PLAYER_CMD_NEXT, 2.0); /* -> call off */
	CHECK(strcmp(profile(&fx), "a2dp-sink") == 0, "profile switched immediately");
	CHECK(fx.earcon.off == 0, "call-off earcon NOT played synchronously when delay > 0");

	iterations = 0;
	while (fx.earcon.off == 0 && iterations < 2000) {
		sd_event_run(fx.event, 5 * 1000);
		iterations++;
	}
	CHECK(fx.earcon.off == 1, "call-off earcon eventually played after the configured delay");

	fixture_teardown(&fx);
}

static void test_auto_mode_allows_gesture(void)
{
	struct fixture fx;

	fixture_setup_ex(&fx, NULL, true, M4_DAEMON_MODE_AUTO, false);
	gesture(&fx, M4_PLAYER_CMD_NEXT, 5.0);
	CHECK(strcmp(profile(&fx), "headset-head-unit") == 0, "auto mode: gesture writes too");
	fixture_teardown(&fx);
}

static void test_gesture_toggles_ctl_started_call(void)
{
	struct fixture fx;
	char response[M4_CTL_MAX_RESPONSE];

	fixture_setup(&fx, "gesture-cooldown-ms = 0\n");
	m4_daemon_handle_request(fx.d, "CALL ON", response, sizeof(response));
	CHECK(strncmp(response, "OK", 2) == 0, "ctl CALL ON ok");
	CHECK(fx.earcon.on == 1, "ctl call on plays the earcon too");
	gesture(&fx, M4_PLAYER_CMD_NEXT, 1e9); /* cooldown 0 -> toggles off */
	CHECK(strcmp(profile(&fx), "a2dp-sink") == 0, "gesture toggles a ctl-started call off");
	fixture_teardown(&fx);
}

/* --- HFP call-control events --- */

/* Scenario from the task ("CHUP in HFP -> call off"): a call already active
 * (headset-head-unit, e.g. started by a gesture/ctl the same way
 * test_gesture_toggles_ctl_started_call() above does), M4 sends AT+CHUP,
 * patch 0006 forwards it as a CHUP event -> default hfp-chup=call-off
 * switches back to a2dp-sink. */
static void test_hfp_chup_defaults_to_call_off(void)
{
	struct fixture fx;
	struct m4_daemon_hfp_status hs;

	fixture_setup(&fx, "gesture-cooldown-ms = 0\n");
	CHECK(strcmp(profile(&fx), "a2dp-sink") == 0, "starts in a2dp-sink");

	gesture(&fx, M4_PLAYER_CMD_NEXT, 1.0); /* -> call on, same as a real "answer" gesture would */
	CHECK(strcmp(profile(&fx), "headset-head-unit") == 0, "call on (setup)");
	fx.earcon.on = fx.earcon.off = fx.earcon.error = 0; /* only interested in the CHUP's own effect below */

	hfp_event(&fx, M4_HFP_VERB_CHUP, 2.0);
	CHECK(strcmp(profile(&fx), "a2dp-sink") == 0, "AT+CHUP -> call off (default hfp-chup=call-off)");
	CHECK(fx.earcon.off == 1, "call-off earcon played");

	m4_daemon_get_hfp_status(fx.d, &hs);
	CHECK(hs.events == 1, "one hfp event counted");
	CHECK(hs.call_actions == 1, "one call action executed");
	CHECK(hs.last_verb == M4_HFP_VERB_CHUP, "last_verb CHUP");
	CHECK(hs.last_action == M4_GESTURE_ACTION_CALL_OFF, "last_action call-off");
	CHECK(strstr(hs.last_result, "off -> ok") != NULL, "last_result mentions the outcome");
	fixture_teardown(&fx);
}

/* hfp-ata/hfp-bldn/hfp-bvra default to `none` (settings.h/.c) -- must be
 * logged, but must NOT touch the graph. */
static void test_hfp_other_verbs_default_to_none(void)
{
	struct fixture fx;
	struct m4_daemon_hfp_status hs;

	fixture_setup(&fx, NULL);
	hfp_event(&fx, M4_HFP_VERB_ATA, 1.0);
	CHECK(fx.fake.set_profile_calls == 0, "ATA (default none): no set_profile");
	m4_daemon_get_hfp_status(fx.d, &hs);
	CHECK(hs.last_action == M4_GESTURE_ACTION_NONE, "ATA: action none");
	CHECK(strcmp(hs.last_result, "no action") == 0, "ATA: logged as no action");

	hfp_event(&fx, M4_HFP_VERB_BLDN, 2.0);
	hfp_event(&fx, M4_HFP_VERB_BVRA, 3.0);
	CHECK(fx.fake.set_profile_calls == 0, "BLDN/BVRA (default none): no set_profile");
	m4_daemon_get_hfp_status(fx.d, &hs);
	CHECK(hs.events == 3, "3 hfp events counted");
	CHECK(hs.call_actions == 0, "no call actions executed");
	fixture_teardown(&fx);
}

/* (the same cooldown as gestures): an HFP event right
 * after a gesture-triggered switch is debounced against it too -- the
 * cooldown state (d->last_call_action_end) is shared, not per-source. */
static void test_hfp_shares_cooldown_with_gestures(void)
{
	struct fixture fx;
	struct m4_daemon_hfp_status hs;

	fixture_setup(&fx, NULL); /* default cooldown 300ms, hfp-chup=call-off */
	gesture(&fx, M4_PLAYER_CMD_NEXT, 100.0); /* -> call on */
	CHECK(strcmp(profile(&fx), "headset-head-unit") == 0, "gesture call on");

	hfp_event(&fx, M4_HFP_VERB_CHUP, 100.15); /* 150ms later, within the 300ms cooldown */
	CHECK(strcmp(profile(&fx), "headset-head-unit") == 0, "CHUP within cooldown: ignored, call stays on");
	m4_daemon_get_hfp_status(fx.d, &hs);
	CHECK(hs.ignored == 1, "counted as ignored");
	CHECK(strstr(hs.last_result, "cooldown") != NULL, "result says cooldown");
	CHECK(fx.earcon.error == 0, "cooldown is not an error");

	hfp_event(&fx, M4_HFP_VERB_CHUP, 102.0); /* past the shared cooldown (300ms default) */
	CHECK(strcmp(profile(&fx), "a2dp-sink") == 0, "CHUP after cooldown: call off");
	fixture_teardown(&fx);
}

/* configuring hfp-chup=passthrough (accepted by the parser, per
 * settings.h's own doc comment: "no session-MPRIS analog") must not crash and must not
 * touch the graph -- logged as unsupported instead. */
static void test_hfp_passthrough_unsupported(void)
{
	struct fixture fx;

	fixture_setup(&fx, "hfp-chup = passthrough\n");
	hfp_event(&fx, M4_HFP_VERB_CHUP, 1.0);
	CHECK(fx.fake.set_profile_calls == 0, "passthrough hfp-chup: no set_profile");
	CHECK(fx.pt.calls == 0, "passthrough hfp-chup: no session MPRIS forward either");
	fixture_teardown(&fx);
}

/* --- mic mute toggle ("tap = mic" task) -------------------- */

/* patch 0007 ("tap = mic"): M4_HFP_VERB_RAW is
 * purely diagnostic -- never gated, never executes any action, regardless
 * of mode/safe-latch/cooldown, and does not consume the shared call/mic
 * cooldown either (a live tap test producing a RAW event must not
 * accidentally suppress the NEXT real gesture). */
static void test_hfp_raw_event_is_diagnostic_only(void)
{
	struct fixture fx;
	struct m4_hfp_event ev;
	struct m4_daemon_hfp_status hs;

	fixture_setup(&fx, NULL);

	memset(&ev, 0, sizeof(ev));
	ev.verb = M4_HFP_VERB_RAW;
	snprintf(ev.raw_text, sizeof(ev.raw_text), "AT+CLCC");
	snprintf(ev.bdaddr, sizeof(ev.bdaddr), "%s", M4_TEST_MAC);
	ev.monotonic_s = 1.0;
	m4_daemon_hfp_event(fx.d, &ev);

	CHECK(fx.fake.set_profile_calls == 0, "RAW event never touches Profile");
	CHECK(fx.fake.set_mic_volume_calls == 0, "RAW event never touches the mic Route");
	CHECK(fx.earcon.on == 0 && fx.earcon.off == 0 && fx.earcon.error == 0 && fx.earcon.mic_on == 0 &&
		      fx.earcon.mic_off == 0,
	      "RAW event plays no earcon at all");

	m4_daemon_get_hfp_status(fx.d, &hs);
	CHECK(hs.events == 1 && hs.raw_events == 1, "counted as both an event and a raw event");
	CHECK(strcmp(hs.last_raw_text, "AT+CLCC") == 0, "last_raw_text recorded");
	CHECK(hs.call_actions == 0 && hs.ignored == 0, "not an executed nor an ignored call action");

	/* immediately followed by a real gesture -- must NOT be debounced by
	 * the RAW event (it never touched last_call_action_end/
	 * last_mic_action_end) */
	gesture(&fx, M4_PLAYER_CMD_NEXT, 1.01);
	CHECK(strcmp(profile(&fx), "headset-head-unit") == 0, "call-toggle right after a RAW event still fires");

	fixture_teardown(&fx);
}

static void test_mic_toggle_requires_active_call(void)
{
	struct fixture fx;
	struct m4_daemon_mic_status ms;

	fixture_setup(&fx, "gesture-previous = mic-toggle\n");
	CHECK(strcmp(profile(&fx), "a2dp-sink") == 0, "starts in a2dp-sink -- no mic route");

	gesture(&fx, M4_PLAYER_CMD_PREVIOUS, 1.0);
	CHECK(fx.fake.set_mic_volume_calls == 0, "mic-toggle outside a call never writes the Route");
	CHECK(fx.earcon.error == 1, "call-error earcon played (no route -- not in a call)");
	CHECK(fx.earcon.mic_on == 0 && fx.earcon.mic_off == 0, "no mic earcon either way");

	m4_daemon_get_mic_status(fx.d, &ms);
	CHECK(!ms.have_route, "status: no route");
	CHECK(ms.actions == 1, "counted as an attempted action, same convention as call_actions");

	fixture_teardown(&fx);
}

static void test_mic_toggle_via_gesture_and_hfp_share_cooldown(void)
{
	struct fixture fx;
	struct m4_daemon_mic_status ms;

	fixture_setup(&fx, "gesture-next = call-toggle\ngesture-previous = mic-toggle\nhfp-ata = mic-toggle\n");

	gesture(&fx, M4_PLAYER_CMD_NEXT, 1.0); /* -> call on, headset-head-unit */
	CHECK(strcmp(profile(&fx), "headset-head-unit") == 0, "call on first");
	m4_graph_ops_fake_set_mic_route(&fx.fake, true, 1.0f, false); /* AT+VGM already reported, unmuted */

	gesture(&fx, M4_PLAYER_CMD_PREVIOUS, 100.0); /* mic-toggle: unmuted -> muted */
	CHECK(fx.fake.last_set_mic_mute == true, "first mic-toggle mutes");
	CHECK(fx.fake.last_set_mic_save == true, "saved (WirePlumber store_user_selected_profile_hook parity)");
	CHECK(fx.earcon.mic_off == 1 && fx.earcon.mic_on == 0, "mic-off earcon played once");
	m4_daemon_get_mic_status(fx.d, &ms);
	CHECK(ms.have_route && ms.mute, "status reflects muted");

	/* own cooldown timer, separate from call's: a HFP mic-toggle event
	 * right after the gesture one above is still debounced against it
	 * (same gesture-cooldown-ms, the same
	 * cooldown as gestures). */
	hfp_event(&fx, M4_HFP_VERB_ATA, 100.2);
	CHECK(fx.fake.set_mic_volume_calls == 1, "HFP mic-toggle within cooldown of the gesture one: ignored");

	hfp_event(&fx, M4_HFP_VERB_ATA, 102.0); /* past the 300ms default cooldown */
	CHECK(fx.fake.set_mic_volume_calls == 2, "HFP mic-toggle after cooldown: executed");
	CHECK(fx.fake.last_set_mic_mute == false, "second toggle unmutes");
	CHECK(fx.earcon.mic_on == 1, "mic-on earcon played once");

	/* mic-toggle's cooldown is INDEPENDENT of call on/off's own: a
	 * call-toggle gesture immediately after the HFP mic-toggle above must
	 * NOT be blocked by the mic cooldown. */
	gesture(&fx, M4_PLAYER_CMD_NEXT, 102.05);
	CHECK(strcmp(profile(&fx), "a2dp-sink") == 0, "call-off right after a mic-toggle is NOT debounced by it");

	fixture_teardown(&fx);
}

static void test_mic_ctl_on_off_toggle_status(void)
{
	struct fixture fx;
	char response[M4_CTL_MAX_RESPONSE];

	fixture_setup(&fx, NULL);

	m4_daemon_handle_request(fx.d, "MIC STATUS", response, sizeof(response));
	CHECK(strncmp(response, "OK\n", 3) == 0, "MIC STATUS ok even with no call active");
	CHECK(strstr(response, "have_route=false\n") != NULL, "no route outside a call");

	m4_daemon_handle_request(fx.d, "MIC ON", response, sizeof(response));
	CHECK(strncmp(response, "ERR", 3) == 0, "MIC ON outside a call is rejected (no route)");

	gesture(&fx, M4_PLAYER_CMD_NEXT, 1.0); /* default: next = call-toggle */
	CHECK(strcmp(profile(&fx), "headset-head-unit") == 0, "call on");
	m4_graph_ops_fake_set_mic_route(&fx.fake, true, 1.0f, false);

	m4_daemon_handle_request(fx.d, "MIC OFF", response, sizeof(response));
	CHECK(strncmp(response, "OK\n", 3) == 0 && strstr(response, "muted=true\n") != NULL, "MIC OFF -> muted=true");
	CHECK(fx.earcon.mic_off == 1, "mic-off earcon via ctl too");

	/* ctl.sock MIC is NOT subject to the gesture-cooldown timer (same
	 * convention as ctl.sock `call on/off` vs. handle_call() -- an
	 * explicit one-shot command, not a debounced physical gesture
	 * stream) -- back-to-back MIC ON/OFF/TOGGLE all execute immediately. */
	m4_daemon_handle_request(fx.d, "MIC ON", response, sizeof(response));
	CHECK(strncmp(response, "OK\n", 3) == 0 && strstr(response, "muted=false\n") != NULL, "MIC ON right after");

	m4_daemon_handle_request(fx.d, "MIC TOGGLE", response, sizeof(response));
	CHECK(strstr(response, "muted=true\n") != NULL, "MIC TOGGLE flips from on -> off");

	m4_daemon_handle_request(fx.d, "MIC STATUS", response, sizeof(response));
	CHECK(strstr(response, "have_route=true\n") != NULL && strstr(response, "muted=true\n") != NULL,
	      "MIC STATUS reflects the toggle");

	m4_daemon_handle_request(fx.d, "MIC bogus", response, sizeof(response));
	CHECK(strncmp(response, "ERR", 3) == 0, "unknown MIC argument rejected");

	fixture_teardown(&fx);
}

static void test_call_off_forces_mic_unmute(void)
{
	struct fixture fx;

	fixture_setup(&fx, NULL); /* default: next = call-toggle */

	gesture(&fx, M4_PLAYER_CMD_NEXT, 1.0);
	CHECK(strcmp(profile(&fx), "headset-head-unit") == 0, "call on");
	m4_graph_ops_fake_set_mic_route(&fx.fake, true, 0.3f, true); /* muted, low gain -- e.g. from a stale save */

	gesture(&fx, M4_PLAYER_CMD_NEXT, 3.0); /* call off (past the 300ms default cooldown) */
	CHECK(strcmp(profile(&fx), "a2dp-sink") == 0, "call off");
	CHECK(fx.fake.last_set_mic_mute == false, "call-off forces mute=false");
	CHECK(fx.fake.last_set_mic_save == true, "saved -- next call-on must not inherit a stale mute");
	CHECK(fx.earcon.mic_on == 0 && fx.earcon.mic_off == 0,
	      "task: no EXTRA mic earcon on top of call-off's own -- would be confusing, not reassuring");
	CHECK(fx.earcon.off == 1, "call-off earcon played exactly once, as usual");

	fixture_teardown(&fx);
}

/* M4 button = mute (VGM): the scenario
 * list -- "button -> VGM 0 -> off earcon; button -> VGM 15 ->
 * on earcon; duplicate VGM -> one earcon; call on while muted -> unmute".
 * m4_graph_ops_fake_set_mic_route() (NOT fx.fake.set_mic_volume_calls, which
 * only counts THIS daemon's OWN writes) is the right way to simulate "the M4
 * itself reported a new AT+VGM value" -- e.g. a real physical button press
 * -- since on the real graph both a button press and one of this daemon's
 * own set_mic_volume() writes arrive via the exact same PW_DEVICE_CHANGE_MASK_PARAMS
 * Route refresh (graph_pw.c's corrected comment on graph_start_refresh()),
 * and daemon_check_mic_route_transition() deliberately cannot and need not
 * tell them apart either. */
static void test_mic_route_watch_button_transitions(void)
{
	struct fixture fx;
	struct m4_daemon_mic_status ms;

	fixture_setup(&fx, NULL); /* default: next = call-toggle */

	/* "call on while muted -> unmute" (item 1): the M4 already reported
	 * AT+VGM=0 BEFORE this call-on --
	 * the entry-fix must restore it to 100% as PART of call-on, with only
	 * the call-on earcon (no separate, redundant mic-on chirp -- the watch
	 * only arms AFTER this settles, daemon_execute_call()'s own comment). */
	m4_graph_ops_fake_set_mic_route(&fx.fake, true, 0.0f, false);
	gesture(&fx, M4_PLAYER_CMD_NEXT, 1.0);
	CHECK(strcmp(profile(&fx), "headset-head-unit") == 0, "call on");
	CHECK(fx.fake.set_mic_volume_calls == 1, "entry-fix wrote exactly once (exactly once, not twice)");
	CHECK(fx.fake.last_set_mic_mute == false && fx.fake.last_set_mic_save == true,
	      "entry-fix: unmute, saved");
	m4_daemon_get_mic_status(fx.d, &ms);
	CHECK(ms.have_route && !ms.mute && ms.volume >= 0.99f,
	      "status: mic already shows 100% right after entry (item 4, real state)");
	CHECK(fx.earcon.on == 1, "call-on earcon played");
	CHECK(fx.earcon.mic_on == 0 && fx.earcon.mic_off == 0,
	      "no separate mic earcon for the entry-fix itself (item 1)");

	/* "button -> VGM 0 -> off earcon": nothing in momentum4d wrote this --
	 * fx.fake.set_mic_volume_calls must NOT move. */
	m4_graph_ops_fake_set_mic_route(&fx.fake, true, 0.0f, false);
	CHECK(fx.fake.set_mic_volume_calls == 1, "button press is NOT a daemon write");
	CHECK(fx.earcon.mic_off == 1 && fx.earcon.mic_on == 0, "button mute -> mic-off earcon, once");
	m4_daemon_get_mic_status(fx.d, &ms);
	CHECK(ms.have_route && ms.mute && ms.volume <= 0.02f, "status reflects the button mute (item 4)");

	/* "duplicate VGM -> one earcon": M4 re-sends the SAME AT+VGM=0 (the
	 * example being "M4 sends VGM twice (00, 00) within ~100ms"). Debounce
	 * here is value-based, not time-based -- see
	 * daemon_check_mic_route_transition()'s own comment for why a second,
	 * genuinely DIFFERENT transition within the same 100ms (e.g. a fast
	 * double-tap of the real button) must still play twice, which a
	 * time-window debounce would get wrong. */
	m4_graph_ops_fake_set_mic_route(&fx.fake, true, 0.0f, false);
	CHECK(fx.earcon.mic_off == 1, "duplicate VGM=0: still exactly one mic-off earcon");

	/* "button -> VGM 15 -> on earcon" */
	m4_graph_ops_fake_set_mic_route(&fx.fake, true, 1.0f, false);
	CHECK(fx.earcon.mic_on == 1 && fx.earcon.mic_off == 1, "button unmute -> mic-on earcon, once");
	m4_daemon_get_mic_status(fx.d, &ms);
	CHECK(ms.have_route && !ms.mute && ms.volume >= 0.99f, "status reflects the button unmute");

	/* item 5: mute state is not carried over -- an M4-initiated VGM after the watch is
	 * disarmed (call over) must not carry over into the next session; the
	 * next call-on's own entry-fix baseline (already covered above) is the
	 * only thing that matters again. */
	gesture(&fx, M4_PLAYER_CMD_NEXT, 3.0); /* call off, past the 300ms default cooldown */
	CHECK(strcmp(profile(&fx), "a2dp-sink") == 0, "call off");
	CHECK(fx.earcon.mic_on == 1 && fx.earcon.mic_off == 1,
	      "call-off's own force-unmute is a no-op here (already unmuted) -- no extra mic earcon, counts unchanged");

	fixture_teardown(&fx);
}

/* --- "mic-on-enter" task ------------------------------------------------- */

/* `mic-on-enter = off`: the entry-fix must leave the mic MUTED (not
 * unmuted), call-on still plays as
 * usual, and mic-off chains right after it (task text: play mic-off
 * right after call-on, so the user knows). `mic-off-chain-delay-ms = 0`
 * (with the fixture's own default `earcon-delay-on-ms = 0`) keeps this
 * fully synchronous, same convention as this file's other delay=0 tests. */
static void test_mic_on_enter_off_chains_mic_off_after_call_on(void)
{
	struct fixture fx;

	fixture_setup(&fx, "mic-on-enter = off\nmic-off-chain-delay-ms = 0\n");
	m4_graph_ops_fake_set_mic_route(&fx.fake, true, 1.0f, false); /* M4 reports full gain before the call */

	gesture(&fx, M4_PLAYER_CMD_NEXT, 1.0);
	CHECK(strcmp(profile(&fx), "headset-head-unit") == 0, "call on");
	CHECK(fx.fake.mic_volume <= 0.02f, "entry-fix muted the mic (mic-on-enter=off)");
	CHECK(fx.fake.mic_mute, "muted flag too");
	CHECK(fx.earcon.on == 1, "call-on earcon played");
	CHECK(fx.earcon.mic_off == 1, "mic-off earcon chained right after call-on");
	CHECK(fx.earcon.mic_on == 0, "no mic-on earcon");

	/* Idempotent re-entry (e.g. a second call-on somehow reached this path):
	 * entry-fix has nothing to fix (already muted) -- no repeat mic-off. */
	fx.earcon.on = 0;
	fx.earcon.mic_off = 0;
	m4_graph_ops_fake_set_mic_route(&fx.fake, true, 0.0f, true);
	gesture(&fx, M4_PLAYER_CMD_NEXT, 1.5); /* -> call off (toggle) */
	CHECK(strcmp(profile(&fx), "a2dp-sink") == 0, "call off");
	CHECK(fx.earcon.mic_off == 0, "call-off does not itself chain mic-off");

	fixture_teardown(&fx);
}

/* Same scenario, but with a non-zero `earcon-delay-on-ms` (call-on itself
 * delayed, like test_earcon_delay_is_not_synchronous() above) -- exercises
 * the CHAIN path (daemon_schedule_mic_off_after_call_on()'s "> 0" branch,
 * on_earcon_timer() firing the chain), not the synchronous one. */
static void test_mic_on_enter_off_chain_via_delayed_timer(void)
{
	struct fixture fx;
	int iterations;

	fixture_setup(&fx, "mic-on-enter = off\nearcon-delay-on-ms = 30\nmic-off-chain-delay-ms = 20\n");
	m4_graph_ops_fake_set_mic_route(&fx.fake, true, 1.0f, false);

	gesture(&fx, M4_PLAYER_CMD_NEXT, 1.0);
	CHECK(strcmp(profile(&fx), "headset-head-unit") == 0, "profile switched immediately");
	CHECK(fx.fake.mic_volume <= 0.02f, "entry-fix already muted (not earcon-delayed)");
	CHECK(fx.earcon.on == 0, "call-on earcon not yet played (delay > 0)");
	CHECK(fx.earcon.mic_off == 0, "mic-off not yet played either");

	iterations = 0;
	while (fx.earcon.on == 0 && iterations < 2000) {
		sd_event_run(fx.event, 5 * 1000);
		iterations++;
	}
	CHECK(fx.earcon.on == 1, "call-on eventually played");
	CHECK(fx.earcon.mic_off == 0, "mic-off has NOT fired yet -- it chains only AFTER call-on plays");

	iterations = 0;
	while (fx.earcon.mic_off == 0 && iterations < 2000) {
		sd_event_run(fx.event, 5 * 1000);
		iterations++;
	}
	CHECK(fx.earcon.mic_off == 1, "mic-off eventually chained after call-on");

	fixture_teardown(&fx);
}

/* `mic-on-enter = remember`: no prior record at all -> "on" (the key's own
 * documented safe default) -- entry-fix unmutes, ordinary call-on earcon
 * only, no mic-off chain (mirrors test_mic_on_enter_on_is_unchanged_default()
 * in tests/test_callfsm_graph_adapter.c, at the daemon/earcon level here). */
static void test_mic_on_enter_remember_no_prior_state(void)
{
	struct fixture fx;

	fixture_setup(&fx, "mic-on-enter = remember\n");
	m4_graph_ops_fake_set_mic_route(&fx.fake, true, 0.0f, true);

	gesture(&fx, M4_PLAYER_CMD_NEXT, 1.0);
	CHECK(strcmp(profile(&fx), "headset-head-unit") == 0, "call on");
	CHECK(fx.fake.mic_volume >= 0.99f, "remember, no prior record -- defaults to mic on");
	CHECK(fx.earcon.on == 1 && fx.earcon.mic_off == 0, "only call-on, no mic-off chain");

	fixture_teardown(&fx);
}

/* `mic-on-enter = remember`: restores whatever the mic was at the LAST HFP
 * exit (task text: the state is restored as of the last HFP
 * exit) -- across a full call on -> mute (button) -> off -> on
 * cycle, exercising daemon_persist_mic_state_on_exit()'s own capture (BEFORE
 * daemon_force_mic_unmute()'s unconditional unmute) and the entry-fix's own
 * read of it on the NEXT call-on. */
static void test_mic_on_enter_remember_restores_last_exit_state(void)
{
	struct fixture fx;

	fixture_setup(&fx, "mic-on-enter = remember\n");

	/* first call: mic on entry (no prior record), user mutes with the
	 * physical button mid-call, then hangs up. */
	m4_graph_ops_fake_set_mic_route(&fx.fake, true, 1.0f, false);
	gesture(&fx, M4_PLAYER_CMD_NEXT, 1.0); /* call on */
	CHECK(strcmp(profile(&fx), "headset-head-unit") == 0, "call on (1)");
	m4_graph_ops_fake_set_mic_route(&fx.fake, true, 0.0f, false); /* button mute mid-call */
	gesture(&fx, M4_PLAYER_CMD_NEXT, 1.5); /* call off */
	CHECK(strcmp(profile(&fx), "a2dp-sink") == 0, "call off (1)");
	/* daemon_force_mic_unmute() always runs at exit -- the LIVE Route ends
	 * up unmuted, but the PERSISTED record (captured before that) must
	 * still say "muted", exactly like the file-based memory it models. */
	CHECK(fx.fake.mic_volume >= 0.99f, "live Route force-unmuted at exit (unrelated existing behaviour)");

	/* second call: entry-fix must restore "muted", from the persisted
	 * record, regardless of the CURRENT (just force-unmuted) live state. */
	gesture(&fx, M4_PLAYER_CMD_NEXT, 2.0); /* call on */
	CHECK(strcmp(profile(&fx), "headset-head-unit") == 0, "call on (2)");
	CHECK(fx.fake.mic_volume <= 0.02f, "remember restored \"muted\" from the last exit");
	CHECK(fx.earcon.mic_off == 1, "mic-off earcon chained after this call-on's own entry-fix");

	fixture_teardown(&fx);
}

/* --- none / passthrough ----------------------------------------------------- */

static void test_none_and_passthrough(void)
{
	struct fixture fx;
	struct m4_daemon_gesture_status gs;

	fixture_setup(&fx, NULL); /* previous = none, playpause(+play/pause) = passthrough */
	gesture(&fx, M4_PLAYER_CMD_PREVIOUS, 1.0);
	CHECK(fx.pt.calls == 0 && fx.fake.set_profile_calls == 0, "none: nothing happens");
	m4_daemon_get_gesture_status(fx.d, &gs);
	CHECK(gs.have_last && gs.last_cmd == M4_PLAYER_CMD_PREVIOUS && gs.last_action == M4_GESTURE_ACTION_NONE,
	      "none: still recorded");

	gesture(&fx, M4_PLAYER_CMD_PLAY, 2.0);
	CHECK(fx.pt.calls == 1 && strcmp(fx.pt.last_method, "PlayPause") == 0, "passthrough: Play -> PlayPause");
	gesture(&fx, M4_PLAYER_CMD_PAUSE, 3.0);
	CHECK(fx.pt.calls == 2 && strcmp(fx.pt.last_method, "PlayPause") == 0, "passthrough: Pause -> PlayPause");

	fx.pt.rc = 1;
	gesture(&fx, M4_PLAYER_CMD_PLAYPAUSE, 4.0);
	m4_daemon_get_gesture_status(fx.d, &gs);
	CHECK(strstr(gs.last_result, "no MPRIS") != NULL, "no session player -> logged, nothing else");
	CHECK(fx.fake.set_profile_calls == 0, "passthrough never touches the graph");
	fixture_teardown(&fx);

	fixture_setup(&fx, "gesture-play = passthrough-exact\ngesture-stop = passthrough\n");
	gesture(&fx, M4_PLAYER_CMD_PLAY, 1.0);
	CHECK(strcmp(fx.pt.last_method, "Play") == 0, "passthrough-exact keeps Play");
	gesture(&fx, M4_PLAYER_CMD_STOP, 2.0);
	CHECK(strcmp(fx.pt.last_method, "Stop") == 0, "Stop forwarded as Stop");
	fixture_teardown(&fx);
}

/* --- player-status policy + ctl PLAYER / RELOAD ---------------------------- */

static void test_status_policy_follow(void)
{
	struct fixture fx;
	struct m4_daemon_gesture_status gs;

	fixture_setup(&fx, "player-status = follow\ngesture-playpause = none\n");
	m4_daemon_get_gesture_status(fx.d, &gs);
	CHECK(gs.player_status == M4_PLAYER_STATUS_PLAYING, "follow starts Playing (Smart Pause armed)");
	gesture(&fx, M4_PLAYER_CMD_PAUSE, 1.0);
	m4_daemon_get_gesture_status(fx.d, &gs);
	CHECK(gs.player_status == M4_PLAYER_STATUS_PAUSED, "Pause -> Paused");
	gesture(&fx, M4_PLAYER_CMD_PLAY, 2.0);
	m4_daemon_get_gesture_status(fx.d, &gs);
	CHECK(gs.player_status == M4_PLAYER_STATUS_PLAYING, "Play -> Playing");
	fixture_teardown(&fx);

	fixture_setup(&fx, NULL);
	gesture(&fx, M4_PLAYER_CMD_PAUSE, 1.0);
	m4_daemon_get_gesture_status(fx.d, &gs);
	CHECK(gs.player_status == M4_PLAYER_STATUS_STOPPED, "default stopped never changes");
	fixture_teardown(&fx);
}

static void test_ctl_player_and_reload(void)
{
	struct fixture fx;
	char response[M4_CTL_MAX_RESPONSE];
	struct m4_daemon_gesture_status gs;
	struct m4_daemon_status st;
	FILE *f;

	fixture_setup(&fx, NULL);
	m4_daemon_handle_request(fx.d, "PLAYER", response, sizeof(response));
	CHECK(strncmp(response, "OK\n", 3) == 0, "PLAYER ok");
	CHECK(strstr(response, "gesture-next=call-toggle\n") != NULL, "PLAYER shows the map");
	CHECK(strstr(response, "player_registered=false\n") != NULL, "no player attached in tests");
	CHECK(strstr(response, "player_status=Stopped\n") != NULL, "status Stopped");

	m4_daemon_handle_request(fx.d, "PLAYER STATUS playing", response, sizeof(response));
	CHECK(strstr(response, "player_status=Playing\n") != NULL, "PLAYER STATUS playing");
	m4_daemon_handle_request(fx.d, "PLAYER STATUS loud", response, sizeof(response));
	CHECK(strncmp(response, "ERR", 3) == 0, "bad status rejected");

	f = fopen(fx.config_path, "w");
	CHECK(f != NULL, "write config");
	if (f != NULL) {
		fputs("mode = observe\ngesture-next = none\ngesture-previous = call-toggle\nbogus = 1\n", f);
		fclose(f);
	}
	m4_daemon_handle_request(fx.d, "RELOAD", response, sizeof(response));
	CHECK(strncmp(response, "OK", 2) == 0 && strstr(response, "warnings=1") != NULL, "RELOAD ok, 1 warning");
	m4_daemon_get_status(fx.d, &st);
	CHECK(st.mode == M4_DAEMON_MODE_MANUAL, "RELOAD does not change the running mode");
	m4_daemon_get_gesture_status(fx.d, &gs);
	CHECK(gs.player_status == M4_PLAYER_STATUS_STOPPED, "RELOAD resets player-status to config (stopped)");

	gesture(&fx, M4_PLAYER_CMD_NEXT, 1.0);
	CHECK(fx.fake.set_profile_calls == 0, "after reload Next = none");
	gesture(&fx, M4_PLAYER_CMD_PREVIOUS, 2.0);
	CHECK(strcmp(profile(&fx), "headset-head-unit") == 0, "after reload Previous = call-toggle");
	fixture_teardown(&fx);
}

/* --- regression: call from INSIDE an sd-event callback, async graph ------- */

struct in_loop_ctx {
	struct fixture *fx;
	m4_player_cmd_t cmd;
};

static int on_timer_gesture(sd_event_source *s, uint64_t usec, void *userdata)
{
	struct in_loop_ctx *c = userdata;

	(void)s;
	(void)usec;
	/* exactly the live situation: we are inside sd-event dispatch
	 * (SD_EVENT_RUNNING), a nested sd_event_run() would be -EBUSY */
	CHECK(sd_event_get_state(c->fx->event) == SD_EVENT_RUNNING, "inside dispatch");
	gesture(c->fx, c->cmd, 50.0);
	sd_event_exit(c->fx->event, 0);
	return 0;
}

static void run_gesture_inside_loop(struct fixture *fx, m4_player_cmd_t cmd)
{
	struct in_loop_ctx c = { .fx = fx, .cmd = cmd };
	sd_event_source *src = NULL;

	CHECK(sd_event_add_time_relative(fx->event, &src, CLOCK_MONOTONIC, 1000, 0, on_timer_gesture, &c) >= 0,
	      "arm timer");
	CHECK(sd_event_loop(fx->event) >= 0, "loop");
	sd_event_source_unref(src);
}

static void test_async_graph_inside_dispatch(void)
{
	struct fixture fx;
	struct m4_daemon_gesture_status gs;

	/* iterate() available */
	fixture_setup_ex(&fx, NULL, true, M4_DAEMON_MODE_MANUAL, true);
	CHECK(strcmp(profile(&fx), "a2dp-sink") == 0, "async fixture starts in a2dp-sink");
	run_gesture_inside_loop(&fx, M4_PLAYER_CMD_NEXT);
	CHECK(strcmp(profile(&fx), "headset-head-unit") == 0, "async confirm inside dispatch succeeds");
	CHECK(fx.fake.iterate_calls > 0, "confirmation came through iterate()");
	m4_daemon_get_gesture_status(fx.d, &gs);
	CHECK(strstr(gs.last_result, "-> ok") != NULL, "result ok");
	fixture_teardown(&fx);

	/* same, but the graph cannot iterate: the earlier, unfixed situation. Must
	 * fail FAST (no hang, no double switch), not succeed by accident. */
	fixture_setup_ex(&fx, NULL, true, M4_DAEMON_MODE_MANUAL, true);
	fx.fake.iterate_enabled = false;
	run_gesture_inside_loop(&fx, M4_PLAYER_CMD_NEXT);
	m4_daemon_get_gesture_status(fx.d, &gs);
	CHECK(strstr(gs.last_result, "-> ok") == NULL, "without iterate(): reported as failed");
	CHECK(fx.fake.iterate_calls == 0, "iterate not used");
	fixture_teardown(&fx);
}

/* [general] language = ru (common/m4-i18n.h M4_T()): daemon.c's gesture
 * result text switches to the Russian variant -- the ONE test in this repo
 * that exercises the ru branch of M4_T() end to end (item 4 of the
 * language-support task), everything else in this file runs the default
 * (m4_lang == M4_LANG_EN) that every other test implicitly assumes. Reset
 * m4_lang back to EN unconditionally (even on a CHECK() failure -- other
 * test functions in this same process must not inherit "ru"). */
static void test_ru_language_switches_gesture_result_text(void)
{
	struct fixture fx;
	struct m4_daemon_gesture_status gs;

	m4_lang = M4_LANG_RU;

	fixture_setup(&fx, NULL);
	gesture(&fx, M4_PLAYER_CMD_NEXT, 100.0);
	gesture(&fx, M4_PLAYER_CMD_NEXT, 100.2); /* within the default 300ms cooldown */
	m4_daemon_get_gesture_status(fx.d, &gs);
	CHECK(strstr(gs.last_result, "пауза") != NULL, "ru: result says cooldown");
	fixture_teardown(&fx);

	m4_lang = M4_LANG_EN;
}

int main(void)
{
	test_mode_from_config_overrides_runtime_file();
	test_next_toggles_call_with_cooldown_and_earcon();
	test_call_on_off_actions_are_idempotent();
	test_observe_and_safe_latch_block_gesture_writes();
	test_auto_mode_allows_gesture();
	test_gesture_toggles_ctl_started_call();
	test_cooldown_does_not_play_error_earcon();
	test_earcon_delay_is_not_synchronous();
	test_hfp_chup_defaults_to_call_off();
	test_hfp_other_verbs_default_to_none();
	test_hfp_shares_cooldown_with_gestures();
	test_hfp_passthrough_unsupported();
	test_hfp_raw_event_is_diagnostic_only();
	test_mic_toggle_requires_active_call();
	test_mic_toggle_via_gesture_and_hfp_share_cooldown();
	test_mic_ctl_on_off_toggle_status();
	test_call_off_forces_mic_unmute();
	test_mic_route_watch_button_transitions();
	test_mic_on_enter_off_chains_mic_off_after_call_on();
	test_mic_on_enter_off_chain_via_delayed_timer();
	test_mic_on_enter_remember_no_prior_state();
	test_mic_on_enter_remember_restores_last_exit_state();
	test_none_and_passthrough();
	test_status_policy_follow();
	test_ctl_player_and_reload();
	test_async_graph_inside_dispatch();
	test_ru_language_switches_gesture_result_text();

	printf("test_gestures: %d/%d checks passed\n", checks - failures, checks);
	return failures == 0 ? 0 : 1;
}
