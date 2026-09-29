/* tests/scenarios/scenario_runner.c -- scenario-level integration test bench
 * for momentum4d, built ONLY on top of the daemon's
 * public API (daemon/daemon.h) plus its existing test doubles
 * (core/graph_ops_fake.c, bt/bt_ops_fake.c) -- exactly like tests/test_daemon.c,
 * tests/test_repair_a2dp.c, tests/test_callfsm_graph_adapter.c already do,
 * just driven from a small text DSL instead of hand-written C per scenario,
 * so new scenarios can be added as .scn files without touching this file.
 *
 * Scope note: this harness does NOT touch pw/micdetect.c's live PipeWire
 * glue or core/automode.c internals directly -- it exercises
 * them only through daemon.c's existing public entry points
 * (m4_daemon_mic_active_changed()/m4_daemon_tick()/m4_daemon_handle_request()),
 * exactly like tests/test_daemon.c. Multi-app microphone aggregation
 * (signal-desktop holds it continuously, librewolf only during the call,
 * pw-record once) is modelled by calling micdetect.c's OWN pure decision function,
 * m4_mic_decide() (pw/micdetect.h), against a synthetic candidate list built
 * from `mic <binary> start|stop [passive] [monitor]` DSL lines -- the exact
 * same function the live PipeWire glue in micdetect.c calls, so this is a
 * faithful (not reimplemented) model of the aggregation logic, just fed
 * synthetic candidates instead of a live registry.
 *
 * DSL (see tests/scenarios/README.md and the *.scn files for full syntax):
 *   optional first line: "CONFIG key=value ..."
 *   every other non-blank, non-'#' line: "<time_s> <command> [args...]"
 *   time_s is an absolute simulated seconds-since-start float, monotonically
 *   non-decreasing; every line first calls m4_daemon_tick(d, time_s) (a
 *   no-op if automode has no pending deadline at or before time_s) and THEN
 *   executes the command/expectation.
 *
 * Real-wall-clock note: automode's own debounce/rate-limit timers are
 * purely a function of the `time_s` this harness passes in (see
 * core/automode.h) -- fully virtual, instant to run. BUT
 * pw/callfsm_graph_adapter.c's set_profile_confirm() and pw/repair_a2dp.c's
 * poll_until() are bounded by REAL wall-clock timeouts
 * (confirm_timeout_ms/enum_wait_ms/connect_timeout_ms, via sd_event_now()),
 * because that is what the real adapter/repair code do against real
 * PipeWire too -- CONFIG's *_ms knobs default small (see below) specifically
 * so `hfp_delay`/`reappear_profile` scenarios finish in well under a second
 * of real time while still exercising the exact same timeout-vs-confirmed
 * race the observed 0-12s live delay describes, just time-scaled.
 */
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <systemd/sd-event.h>

#include "bt/bt_ops_fake.h"
#include "core/graph_ops_fake.h"
#include "core/state.h"
#include "daemon/daemon.h"
#include "ipc/ctl.h"
#include "ipc/hfp_events.h"
#include "pw/micdetect.h"
#include "util/settings.h"

#define M4_TEST_MAC "00:1B:66:00:00:01"
#define MAX_APPS 16
#define MAX_LINE 512

struct mic_app {
	bool used;
	char binary[64];
	bool active;
	bool passive;
	bool monitor;
};

struct fixture {
	sd_event *event;
	m4_graph_ops_fake_t fake;
	m4_bt_ops_fake_t bt_fake;
	char runtime_dir[128];
	/* mic-on-enter: `[paths] state-dir` (survives a "reboot" -- see
	 * the `mic_wipe_runtime` DSL command below). */
	char state_dir[128];
	struct m4_daemon *d;

	int confirm_timeout_ms;
	int repair_connect_timeout_ms;
	int repair_enum_wait_ms;
	bool use_repair; /* whether cfg.bt_ops is wired up */
	/* mic-on-enter: `[call] mic-on-enter` (CONFIG mic_on_enter=on|
	 * off|remember) -- fed into cfg.settings below, since it is not an int
	 * knob like the other CONFIG keys (handle_config_line()'s own special
	 * case for it). */
	m4_mic_on_enter_t mic_on_enter;

	struct mic_app apps[MAX_APPS];
	/* delayed-event timers still pending at the end of a scenario (armed
	 * but never fired) -- unref'd before sd_event_unref() so ASan's leak
	 * check stays clean */
	sd_event_source *timers[32];
	int n_timers;
	bool last_mic_active;
	bool have_last_mic_active;
	bool last_hold_active; /* Bug 1 fix */
	bool have_last_hold_active;

	int fails;
	int checks;
	const char *scenario_name;
	char last_response[M4_CTL_MAX_RESPONSE];
};

/* ------------------------------------------------------------------ */
/* small helpers mirroring tests/test_repair_a2dp.c's own local helpers
 * (not exported from graph_ops_fake.h -- struct fields are public on
 * purpose, see that header, so re-implementing this two-liner locally is
 * the established convention rather than a shortcut). */

static void remove_profile_by_name(m4_graph_ops_fake_t *fake, const char *name)
{
	size_t w = 0;
	for (size_t i = 0; i < fake->n_profiles; i++) {
		if (strcmp(fake->profiles[i].name, name) != 0)
			fake->profiles[w++] = fake->profiles[i];
	}
	fake->n_profiles = w;
}

static void add_profile_if_absent(m4_graph_ops_fake_t *fake, const char *name)
{
	for (size_t i = 0; i < fake->n_profiles; i++)
		if (strcmp(fake->profiles[i].name, name) == 0)
			return;
	if (fake->n_profiles >= M4_GRAPH_MAX_PROFILES)
		return;
	struct m4_graph_profile *p = &fake->profiles[fake->n_profiles++];
	memset(p, 0, sizeof(*p));
	p->index = (uint32_t)fake->n_profiles - 1;
	snprintf(p->name, sizeof(p->name), "%s", name);
	p->available = true;
}

/* ------------------------------------------------------------------ */
/* delayed-event timers: models both "HFP confirmation arrives late" (the
 * live CALL_ON bug) and "a2dp-sink reappears in
 * EnumProfile some seconds after ConnectProfile" (repair-a2dp's own
 * pattern, tests/test_repair_a2dp.c's test_missing_then_reappears). Both
 * only fire because the adapter's/repair's OWN bounded poll loops pump this
 * same sd_event via sd_event_run() while waiting -- see this file's top
 * comment. */

struct delayed_profile_apply {
	m4_graph_ops_fake_t *fake;
	char profile[64];
	bool add_only; /* true: add_profile_if_absent (repair scenario);
			  false: set_profile_external (HFP confirm-delay scenario) */
};

static int on_delayed_profile_apply(sd_event_source *s, uint64_t usec, void *userdata)
{
	struct delayed_profile_apply *ctx = userdata;
	(void)s;
	(void)usec;
	if (ctx->add_only)
		add_profile_if_absent(ctx->fake, ctx->profile);
	else
		m4_graph_ops_fake_set_profile_external(ctx->fake, ctx->profile);
	/* ctx is freed by the source's destroy callback (arm_delayed_profile) */
	return 0;
}

static void arm_delayed_profile(struct fixture *fx, const char *profile, int delay_ms, bool add_only)
{
	struct delayed_profile_apply *ctx = calloc(1, sizeof(*ctx));
	sd_event_source *timer = NULL;

	ctx->fake = &fx->fake;
	snprintf(ctx->profile, sizeof(ctx->profile), "%s", profile);
	ctx->add_only = add_only;
	/* Kept in fx->timers and unref'd at the end of the scenario (a
	 * floating source that never fires would pin the event loop and leak
	 * both). ctx is owned by the source (destroy callback), so it is freed
	 * whether or not the timer fired. */
	if (fx->n_timers >= (int)(sizeof(fx->timers) / sizeof(fx->timers[0])) ||
	    sd_event_add_time_relative(fx->event, &timer, CLOCK_MONOTONIC,
					(uint64_t)delay_ms * 1000, 1000,
					on_delayed_profile_apply, ctx) < 0) {
		free(ctx);
		return;
	}
	sd_event_source_set_destroy_callback(timer, free);
	fx->timers[fx->n_timers++] = timer;
}

/* ------------------------------------------------------------------ */
/* mic aggregation: same struct/function pw/micdetect.c's live glue itself
 * calls (m4_mic_decide(), pw/micdetect.h) -- see this file's top comment. */

static void mic_recompute_and_feed(struct fixture *fx, double now)
{
	struct m4_mic_candidate candidates[MAX_APPS];
	size_t n = 0;
	bool new_active, new_hold_active;

	for (int i = 0; i < MAX_APPS; i++) {
		struct mic_app *a = &fx->apps[i];
		struct m4_mic_candidate *c;

		if (!a->used || !a->active)
			continue;
		c = &candidates[n++];
		memset(c, 0, sizeof(*c));
		c->source_is_m4_mic = true;
		snprintf(c->sink_media_class, sizeof(c->sink_media_class), "%s", "Stream/Input/Audio");
		snprintf(c->sink_node_name, sizeof(c->sink_node_name), "%s_capture", a->binary);
		c->sink_passive = a->passive;
		c->sink_stream_monitor = a->monitor;
		snprintf(c->sink_app_binary, sizeof(c->sink_app_binary), "%s", a->binary);
	}

	new_active = m4_mic_decide(candidates, n, NULL);
	/* Bug 1 fix: same aggregation, mirroring
	 * daemon.c's on_micdetect_active_changed() -- both fed on every
	 * recompute, unconditionally, exactly like the live daemon does
	 * (both are individually idempotent on a repeated value). */
	new_hold_active = m4_mic_decide_hold(candidates, n, NULL);

	if (!fx->have_last_mic_active || new_active != fx->last_mic_active) {
		fx->have_last_mic_active = true;
		fx->last_mic_active = new_active;
		m4_daemon_mic_active_changed(fx->d, new_active, now);
	}
	if (!fx->have_last_hold_active || new_hold_active != fx->last_hold_active) {
		fx->have_last_hold_active = true;
		fx->last_hold_active = new_hold_active;
		m4_daemon_mic_hold_changed(fx->d, new_hold_active, now);
	}
}

static struct mic_app *find_or_alloc_app(struct fixture *fx, const char *binary)
{
	int free_slot = -1;
	for (int i = 0; i < MAX_APPS; i++) {
		if (fx->apps[i].used && strcmp(fx->apps[i].binary, binary) == 0)
			return &fx->apps[i];
		if (!fx->apps[i].used && free_slot < 0)
			free_slot = i;
	}
	if (free_slot < 0)
		return NULL;
	fx->apps[free_slot].used = true;
	snprintf(fx->apps[free_slot].binary, sizeof(fx->apps[free_slot].binary), "%s", binary);
	return &fx->apps[free_slot];
}

/* ------------------------------------------------------------------ */

static void report(struct fixture *fx, bool cond, const char *fmt, ...)
{
	va_list ap;
	fx->checks++;
	if (cond)
		return;
	fx->fails++;
	fprintf(stderr, "FAIL [%s]: ", fx->scenario_name);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fprintf(stderr, "\n");
}

static void daemon_recreate(struct fixture *fx)
{
	struct m4_daemon_config cfg = {0};
	struct m4_settings settings;

	if (fx->d != NULL)
		m4_daemon_free(fx->d);

	cfg.graph_ops = m4_graph_ops_fake(&fx->fake); /* re-arms the fake's static open() slot */
	cfg.mac_address = M4_TEST_MAC;
	cfg.runtime_dir = fx->runtime_dir;
	cfg.state_dir = fx->state_dir; /* "mic-on-enter" task */
	cfg.stats_path = "";
	cfg.confirm_timeout_ms = fx->confirm_timeout_ms;
	if (fx->use_repair) {
		cfg.bt_ops = m4_bt_ops_fake(&fx->bt_fake);
		cfg.bt_adapter = "hci0";
		cfg.repair_connect_timeout_ms = fx->repair_connect_timeout_ms;
		cfg.repair_enum_wait_ms = fx->repair_enum_wait_ms;
	}
	cfg.enable_micdetect = false;
	cfg.enable_ctl = false;
	cfg.enable_tick_timer = false;

	/* mic-on-enter: CONFIG mic_on_enter=... (default M4_MIC_ON_ENTER_ON,
	 * fx->mic_on_enter's own zero-value) -- every OTHER settings.h field
	 * stays at m4_settings_defaults(), same as this function's pre-existing
	 * cfg.settings == NULL behaviour (m4_daemon_new() itself would call
	 * m4_settings_defaults() for us, but we need a live struct here to
	 * override one field first). */
	m4_settings_defaults(&settings);
	settings.mic_on_enter = fx->mic_on_enter;
	cfg.settings = &settings;

	fx->d = m4_daemon_new(fx->event, &cfg);
	/* mic state is NOT persisted across a restart (real behaviour: nothing
	 * in core/state.h models "mic currently active" -- only call-state/
	 * mode/safe-latch are on disk) -- a fresh daemon always starts
	 * believing no mic is active until the next mic event, matching
	 * daemon.h's own doc comment on automode's cold-start default. */
	fx->have_last_mic_active = false;
	fx->have_last_hold_active = false; /* Bug 1 fix */
}

/* ------------------------------------------------------------------ */

static char *trim(char *s)
{
	while (isspace((unsigned char)*s))
		s++;
	char *end = s + strlen(s);
	while (end > s && isspace((unsigned char)end[-1]))
		*--end = '\0';
	return s;
}

static void handle_config_line(struct fixture *fx, char *rest)
{
	char *tok = strtok(rest, " \t");
	while (tok != NULL) {
		char *eq = strchr(tok, '=');
		if (eq != NULL) {
			*eq = '\0';
			/* mic-on-enter: the one CONFIG key that is a string
			 * enum, not an int -- checked before the generic atoi()
			 * path below (atoi() on "off"/"remember" would silently be
			 * 0, which happens to also be M4_MIC_ON_ENTER_ON -- wrong
			 * for "off"/"remember", so this must be its own branch,
			 * not folded into the int one). */
			if (strcmp(tok, "mic_on_enter") == 0) {
				m4_mic_on_enter_t v;

				if (m4_mic_on_enter_parse(eq + 1, &v) == 0)
					fx->mic_on_enter = v;
				else
					fprintf(stderr, "WARN [%s]: CONFIG mic_on_enter=%s: expected on|off|remember\n",
						fx->scenario_name, eq + 1);
			} else {
				int val = atoi(eq + 1);

				if (strcmp(tok, "confirm_timeout_ms") == 0)
					fx->confirm_timeout_ms = val;
				else if (strcmp(tok, "repair_connect_timeout_ms") == 0)
					fx->repair_connect_timeout_ms = val;
				else if (strcmp(tok, "repair_enum_wait_ms") == 0)
					fx->repair_enum_wait_ms = val;
				else if (strcmp(tok, "use_repair") == 0)
					fx->use_repair = val != 0;
				else
					fprintf(stderr, "WARN [%s]: unknown CONFIG key '%s'\n", fx->scenario_name, tok);
			}
		}
		tok = strtok(NULL, " \t");
	}
}

static void process_command(struct fixture *fx, double now, char *cmd, char *rest)
{
	char *response = fx->last_response;

	if (strcmp(cmd, "present") == 0) {
		char *a1 = strtok(rest, " \t");
		char *a2 = strtok(NULL, " \t");
		bool present = a1 != NULL && atoi(a1) != 0;
		bool connected = a2 != NULL ? atoi(a2) != 0 : present;
		m4_graph_ops_fake_set_present(&fx->fake, present, connected);
	} else if (strcmp(cmd, "profile") == 0) {
		char *name = strtok(rest, " \t");
		m4_graph_ops_fake_set_profile_external(&fx->fake, name != NULL ? name : "");
	} else if (strcmp(cmd, "remove_profile") == 0) {
		char *name = strtok(rest, " \t");
		if (name != NULL)
			remove_profile_by_name(&fx->fake, name);
	} else if (strcmp(cmd, "add_profile") == 0) {
		char *name = strtok(rest, " \t");
		if (name != NULL)
			add_profile_if_absent(&fx->fake, name);
	} else if (strcmp(cmd, "mode") == 0) {
		char *word = strtok(rest, " \t");
		char req[32];
		snprintf(req, sizeof(req), "MODE %s", word != NULL ? word : "");
		m4_daemon_handle_request(fx->d, req, response, M4_CTL_MAX_RESPONSE);
	} else if (strcmp(cmd, "safe_latch") == 0) {
		char *word = strtok(rest, " \t");
		char path[256];
		snprintf(path, sizeof(path), "%s/safe-latch", fx->runtime_dir);
		if (word != NULL && strcmp(word, "on") == 0) {
			FILE *f = fopen(path, "w");
			if (f != NULL)
				fclose(f);
		} else {
			unlink(path);
		}
	} else if (strcmp(cmd, "call") == 0) {
		char *word = strtok(rest, " \t");
		bool on = word != NULL && strcmp(word, "on") == 0;
		m4_daemon_handle_request(fx->d, on ? "CALL ON" : "CALL OFF", response, M4_CTL_MAX_RESPONSE);
	} else if (strcmp(cmd, "mic_ctl") == 0) {
		/* tap = mic: "mic_ctl on|off|toggle|status"
		 * -- ctl.sock MIC (cli/mic.c's backend, daemon.c's handle_mic()).
		 * Named "mic_ctl", not "mic" (already the mic-CAPTURE-APPLICATION
		 * holder simulation above, automode's input -- unrelated). */
		char *word = strtok(rest, " \t");
		char req[24];

		snprintf(req, sizeof(req), "MIC %s", word != NULL ? word : "STATUS");
		for (char *p = req; *p != '\0'; p++)
			*p = (char)toupper((unsigned char)*p);
		m4_daemon_handle_request(fx->d, req, response, M4_CTL_MAX_RESPONSE);
	} else if (strcmp(cmd, "repair") == 0) {
		m4_daemon_handle_request(fx->d, "REPAIR_A2DP", response, M4_CTL_MAX_RESPONSE);
	} else if (strcmp(cmd, "hfp") == 0) {
		/* "hfp <VERB>" -- patch
		 * 0006: simulates a hfp-events.sock datagram already
		 * parsed+timestamped, exactly like tests/test_gestures.c's
		 * hfp_event() helper (no real socket here either -- that is
		 * tests/test_hfp_events.c's job, see this file's own scope
		 * note at the top). */
		char *verb_word = strtok(rest, " \t");
		struct m4_hfp_event ev;
		m4_hfp_verb_t verb;

		memset(&ev, 0, sizeof(ev));
		if (verb_word == NULL) {
			fprintf(stderr, "WARN [%s]: hfp command missing verb\n", fx->scenario_name);
		} else if (strcmp(verb_word, "CHUP") == 0) {
			verb = M4_HFP_VERB_CHUP;
		} else if (strcmp(verb_word, "ATA") == 0) {
			verb = M4_HFP_VERB_ATA;
		} else if (strcmp(verb_word, "BLDN") == 0) {
			verb = M4_HFP_VERB_BLDN;
		} else if (strcmp(verb_word, "BVRA") == 0) {
			verb = M4_HFP_VERB_BVRA;
		} else if (m4_hfp_verb_from_name(verb_word, &verb) == 0 && m4_hfp_verb_is_sco(verb)) {
			/* SCODROP/SCOUP/SCOFAIL -- patch 0008 */
		} else {
			fprintf(stderr, "WARN [%s]: hfp unknown verb '%s'\n", fx->scenario_name, verb_word);
			verb_word = NULL;
		}
		if (verb_word != NULL) {
			ev.verb = verb;
			snprintf(ev.bdaddr, sizeof(ev.bdaddr), "%s", M4_TEST_MAC);
			ev.realtime_usec = (uint64_t)(now * 1e6);
			ev.monotonic_s = now;
			m4_daemon_hfp_event(fx->d, &ev);
		}
	} else if (strcmp(cmd, "mic_route") == 0) {
		/* Bug 2: "mic_route <have 0|1> <volume 0.1> <mute 0|1>" */
		char *have = strtok(rest, " \t");
		char *vol = strtok(NULL, " \t");
		char *mute = strtok(NULL, " \t");
		m4_graph_ops_fake_set_mic_route(&fx->fake, have != NULL && atoi(have) != 0,
						 vol != NULL ? (float)atof(vol) : 0.0f,
						 mute != NULL && atoi(mute) != 0);
	} else if (strcmp(cmd, "mic") == 0) {
		char *binary = strtok(rest, " \t");
		char *action = strtok(NULL, " \t");
		struct mic_app *a = binary != NULL ? find_or_alloc_app(fx, binary) : NULL;
		if (a == NULL) {
			fprintf(stderr, "WARN [%s]: mic command missing/unknown binary\n", fx->scenario_name);
		} else {
			bool start = action != NULL && strcmp(action, "start") == 0;
			a->active = start;
			a->passive = false;
			a->monitor = false;
			for (char *flag = strtok(NULL, " \t"); flag != NULL; flag = strtok(NULL, " \t")) {
				if (strcmp(flag, "passive") == 0)
					a->passive = true;
				else if (strcmp(flag, "monitor") == 0)
					a->monitor = true;
			}
			mic_recompute_and_feed(fx, now);
		}
	} else if (strcmp(cmd, "mic_wipe_runtime") == 0) {
		/* mic-on-enter: models a REBOOT (not just a daemon
		 * restart) for `[call] mic-on-enter = remember`'s own two-
		 * location persistence -- $XDG_RUNTIME_DIR is typically tmpfs
		 * and does not survive one, $XDG_STATE_HOME does. Only removes
		 * the mic-last-state file (leaves call-state/mode/safe-latch
		 * alone -- this DSL command is specifically about the mic-state
		 * fallback path, core/state.h's m4_mic_state_read()'s own doc
		 * comment). Use right before `restart` to exercise it. */
		char path[256];

		snprintf(path, sizeof(path), "%s/mic-last-state", fx->runtime_dir);
		unlink(path);
	} else if (strcmp(cmd, "restart") == 0) {
		daemon_recreate(fx);
	} else if (strcmp(cmd, "bt_result") == 0) {
		char *rc = strtok(rest, " \t");
		char *msg = strtok(NULL, "");
		fx->bt_fake.result = rc != NULL ? atoi(rc) : 0;
		snprintf(fx->bt_fake.result_message, sizeof(fx->bt_fake.result_message), "%s",
			 msg != NULL ? msg : "");
	} else if (strcmp(cmd, "hfp_delay") == 0) {
		char *profile = strtok(rest, " \t");
		char *ms = strtok(NULL, " \t");
		if (profile != NULL && ms != NULL) {
			fx->fake.fail_next_set_profile = true;
			arm_delayed_profile(fx, profile, atoi(ms), false);
		}
	} else if (strcmp(cmd, "reappear_profile") == 0) {
		char *name = strtok(rest, " \t");
		char *ms = strtok(NULL, " \t");
		if (name != NULL && ms != NULL)
			arm_delayed_profile(fx, name, atoi(ms), true);
	} else if (strcmp(cmd, "expect_set_profile_calls") == 0) {
		char *n = strtok(rest, " \t");
		report(fx, n != NULL && fx->fake.set_profile_calls == atoi(n),
		       "expect_set_profile_calls %s, got %d", n, fx->fake.set_profile_calls);
	} else if (strcmp(cmd, "expect_profile") == 0) {
		char *name = strtok(rest, " \t");
		report(fx, name != NULL && strcmp(fx->fake.profile_name, name) == 0,
		       "expect_profile %s, got %s", name, fx->fake.profile_name);
	} else if (strcmp(cmd, "expect_mode") == 0) {
		char *word = strtok(rest, " \t");
		struct m4_daemon_status st;
		m4_daemon_get_status(fx->d, &st);
		const char *got = m4_daemon_mode_name(st.mode);
		report(fx, word != NULL && strcmp(got, word) == 0, "expect_mode %s, got %s", word, got);
	} else if (strcmp(cmd, "expect_repair_calls") == 0) {
		char *n = strtok(rest, " \t");
		report(fx, n != NULL && fx->bt_fake.connect_profile_calls == atoi(n),
		       "expect_repair_calls %s, got %d", n, fx->bt_fake.connect_profile_calls);
	} else if (strcmp(cmd, "expect_call_active") == 0) {
		char *n = strtok(rest, " \t");
		struct m4_daemon_status st;
		m4_daemon_get_status(fx->d, &st);
		report(fx, n != NULL && st.call_active == (atoi(n) != 0),
		       "expect_call_active %s, got %d", n, st.call_active);
	} else if (strcmp(cmd, "expect_mic_active") == 0) {
		char *n = strtok(rest, " \t");
		struct m4_daemon_status st;
		m4_daemon_get_status(fx->d, &st);
		report(fx, n != NULL && st.mic_active == (atoi(n) != 0),
		       "expect_mic_active %s, got %d", n, st.mic_active);
	} else if (strcmp(cmd, "expect_frozen") == 0) {
		char *n = strtok(rest, " \t");
		struct m4_daemon_status st;
		m4_daemon_get_status(fx->d, &st);
		report(fx, n != NULL && st.frozen == (atoi(n) != 0), "expect_frozen %s, got %d", n, st.frozen);
	} else if (strcmp(cmd, "expect_mic_volume") == 0) {
		/* Bug 2 */
		char *n = strtok(rest, " \t");
		double want = n != NULL ? atof(n) : -1.0;
		report(fx, n != NULL && fabs((double)fx->fake.mic_volume - want) < 0.001,
		       "expect_mic_volume %s, got %f", n, (double)fx->fake.mic_volume);
	} else if (strcmp(cmd, "expect_mic_mute") == 0) {
		char *n = strtok(rest, " \t");
		report(fx, n != NULL && fx->fake.mic_mute == (atoi(n) != 0),
		       "expect_mic_mute %s, got %d", n, fx->fake.mic_mute);
	} else if (strcmp(cmd, "expect_set_mic_volume_calls") == 0) {
		char *n = strtok(rest, " \t");
		report(fx, n != NULL && fx->fake.set_mic_volume_calls == atoi(n),
		       "expect_set_mic_volume_calls %s, got %d", n, fx->fake.set_mic_volume_calls);
	} else if (strcmp(cmd, "expect_open_calls") == 0) {
		char *n = strtok(rest, " \t");
		report(fx, n != NULL && fx->fake.open_calls == atoi(n), "expect_open_calls %s, got %d", n,
		       fx->fake.open_calls);
	} else if (strcmp(cmd, "expect_response_ok") == 0) {
		report(fx, strncmp(response, "OK", 2) == 0, "expect_response_ok, got '%.40s'", response);
	} else if (strcmp(cmd, "expect_response_err") == 0) {
		report(fx, strncmp(response, "ERR", 3) == 0, "expect_response_err, got '%.40s'", response);
	} else {
		fprintf(stderr, "WARN [%s]: unknown command '%s'\n", fx->scenario_name, cmd);
	}
}

int main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: %s <scenario.scn>\n", argv[0]);
		return 2;
	}

	FILE *f = fopen(argv[1], "r");
	if (f == NULL) {
		fprintf(stderr, "cannot open %s\n", argv[1]);
		return 2;
	}

	struct fixture fx = {0};
	fx.scenario_name = argv[1];
	fx.confirm_timeout_ms = 200;
	fx.repair_connect_timeout_ms = 100;
	fx.repair_enum_wait_ms = 300;
	fx.use_repair = true;

	if (sd_event_new(&fx.event) < 0) {
		fprintf(stderr, "sd_event_new failed\n");
		return 2;
	}
	m4_graph_ops_fake_init(&fx.fake);
	m4_bt_ops_fake_init(&fx.bt_fake);
	snprintf(fx.runtime_dir, sizeof(fx.runtime_dir), "/tmp/m4d-scn.XXXXXX");
	if (mkdtemp(fx.runtime_dir) == NULL) {
		fprintf(stderr, "mkdtemp failed\n");
		return 2;
	}
	/* mic-on-enter: separate from runtime_dir on purpose -- models
	 * `[paths] state-dir` (survives a "reboot", see the `mic_wipe_runtime`
	 * DSL command). */
	snprintf(fx.state_dir, sizeof(fx.state_dir), "/tmp/m4d-scn-state.XXXXXX");
	if (mkdtemp(fx.state_dir) == NULL) {
		fprintf(stderr, "mkdtemp (state_dir) failed\n");
		return 2;
	}

	char line[MAX_LINE];
	bool first_line = true;
	bool daemon_created = false;

	while (fgets(line, sizeof(line), f) != NULL) {
		char *l = trim(line);
		if (l[0] == '\0' || l[0] == '#')
			continue;

		if (first_line && strncmp(l, "CONFIG", 6) == 0) {
			handle_config_line(&fx, l + 6);
			first_line = false;
			continue;
		}
		first_line = false;

		if (!daemon_created) {
			daemon_recreate(&fx);
			daemon_created = true;
		}

		char *save = NULL;
		char *time_tok = strtok_r(l, " \t", &save);
		char *cmd = strtok_r(NULL, " \t", &save);
		if (time_tok == NULL || cmd == NULL) {
			fprintf(stderr, "WARN [%s]: malformed line '%s'\n", fx.scenario_name, line);
			continue;
		}
		double now = atof(time_tok);
		char *rest = save; /* remainder of the line, mutated by strtok inside process_command */

		m4_daemon_tick(fx.d, now);
		process_command(&fx, now, cmd, rest);
	}
	fclose(f);

	printf("%s: %d/%d checks passed\n", fx.scenario_name, fx.checks - fx.fails, fx.checks);

	if (fx.d != NULL)
		m4_daemon_free(fx.d);
	for (int i = 0; i < fx.n_timers; i++)
		sd_event_source_unref(fx.timers[i]);
	sd_event_unref(fx.event);

	return fx.fails > 0 ? 1 : 0;
}
