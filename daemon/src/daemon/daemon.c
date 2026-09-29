/* daemon/daemon.c -- see daemon.h. */
#include "daemon.h"

#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <systemd/sd-daemon.h>

#include "../core/automode.h"
#include "../core/callfsm.h"
#include "../core/profile_rules.h"
#include "../ipc/ctl.h"
#include "../pw/callfsm_graph_adapter.h"
#include "../pw/repair_a2dp.h"
#include "../util/config.h"
#include "../util/earcon.h"
#include "../util/log.h"

#include "../../../common/m4-i18n.h"

/* "daemon.tick-ms"/"daemon.graph-retry-s":
 * compiled DEFAULTS ONLY (identical to common/m4-conf-keys.def's own
 * defaults for both) -- m4_daemon_new() below copies cfg->tick_interval_ms/
 * graph_retry_s into per-instance d->tick_interval_ms/graph_retry_s
 * whenever the caller supplies a positive value (cli/daemon_cmd.c, from
 * momentum4.conf), falling back to these two macros otherwise (every
 * existing {0} test fixture -- unaffected, same numbers as before). */
#define M4_DAEMON_DEFAULT_TICK_INTERVAL_MS 250 /* automode.h: "every 0.2-0.5s" */
#define M4_DAEMON_DEFAULT_GRAPH_RETRY_SECONDS 5.0

/* M4 button = mute (VGM): the M4 mic Route's volume is
 * quantised in 1/15 steps by AT+VGM's own protocol (spa/plugins/bluez5/
 * backend-native.c: "AT+VGM=value: value between 0 and 15" -- confirmed by
 * reading it), so the only value that ever means "off" is exactly 0.0f; the
 * next possible step is 1/15 ~= 0.067f. 0.02f sits cleanly below that with
 * generous margin for float rounding in spa_bt_volume_hw_to_linear()'s own
 * division. Deliberately volume-only (see daemon_check_mic_route_transition()
 * and daemon_mic_fill_status() below for why PipeWire's separate
 * SPA_PROP_mute/`mic_mute` soft flag is NOT part of this decision).
 * `[daemon] mic-mute-threshold` (configurable) -- compiled default only,
 * see d->mic_mute_threshold's own doc comment above. */
#define M4_DAEMON_DEFAULT_MIC_ROUTE_QUIET_EPS 0.02f
#define M4_DAEMON_DEFAULT_EARCON_DELAY_SCO_UP_MS 150 /* `[earcon] delay-sco-up-ms` */

struct m4_daemon {
	sd_event *event;
	const struct graph_ops *graph_ops;
	char mac_address[32];
	char runtime_dir[512];
	char state_dir[512]; /* "mic-on-enter" task -- see daemon.h's own doc comment */
	char stats_path[512];
	int confirm_timeout_ms;
	int tick_interval_ms;      /* daemon.h's own doc comment; >0, always resolved
				     * to a positive value by m4_daemon_new() below */
	double graph_retry_s;      /* same, >0 */
	float mic_mute_threshold;  /* same value the M4_DAEMON_DEFAULT_MIC_ROUTE_QUIET_EPS
				     * macro used to be for every instance, now a
				     * per-instance field (daemon.h's own doc comment) */
	int earcon_delay_sco_up_ms;

	/* "repair-a2dp" -- see daemon.h's own doc
	 * comment on struct m4_daemon_config's bt_ops field for what NULL
	 * means. Only used by handle_repair_a2dp() (ctl.sock REPAIR_A2DP,
	 * this file) -- the automode/callfsm path to repair-a2dp goes
	 * through the adapter (m4_callfsm_graph_adapter_new()'s repair_cfg
	 * argument, set up once in m4_daemon_new() below from these same
	 * fields). */
	const struct m4_bt_ops *bt_ops;
	char bt_adapter[16];
	int repair_connect_timeout_ms;
	int repair_enum_wait_ms;
	int repair_a2dp_confirm_timeout_ms;
	bool repair_a2dp_manual_only;

	void *graph_handle; /* NULL if not currently connected -- retried from
			      * m4_daemon_tick() every graph_retry_s (above) */
	double last_graph_open_attempt;
	struct m4_graph_state graph_state;
	bool present_seen; /* graph_state.m4_present as observed by the last
			     * m4_daemon_tick() call -- see
			     * daemon_reevaluate_mic_after_reappear()'s doc
			     * comment for
			     * why this is tracked separately from
			     * graph_state.m4_present itself (which updates
			     * immediately/async on every on_graph_state_changed()). */

	bool enable_micdetect;
	struct m4_micdetect_config micdetect_cfg;
	char micdetect_mac[32];
	char micdetect_placeholder[64];
	void *micdetect_handle;

	struct m4_callfsm_graph_adapter *adapter;
	m4_callfsm_ops_t callfsm_ops;

	m4_automode_core_t *automode;

	struct m4_ctl_server *ctl;
	sd_event_source *tick_source;

	/* patch 0006's hfp-events.sock consumer */
	struct m4_hfp_events *hfp_events;
	struct m4_daemon_hfp_status hstat;

	m4_daemon_mode_t mode;
	bool action_in_progress; /* see ipc/ctl.h's concurrency-model comment
				   * and this file's top-of-daemon.h comment */

	/* momentum4.conf + gestures */
	struct m4_settings settings;
	char config_path[512];
	bool have_passthrough;
	struct m4_passthrough_ops passthrough;
	void (*earcon)(void *ctx, const char *dir, const char *kind);
	void *earcon_ctx;
	/* Delayed call-on/call-off earcon (see
	 * daemon_schedule_earcon() below). A single shared oneshot timer is
	 * enough: at most one call-on/off switch is ever in flight at a time
	 * (action_in_progress already serialises all of handle_call()/
	 * daemon_gesture_call()/daemon_hfp_call()), so a second schedule
	 * request can only arrive after the first earcon already fired or was
	 * superseded -- re-arming the same source (like on_tick_timer()/
	 * mpris_player.c's arm_timer() already do elsewhere in this codebase)
	 * is correct, not just convenient. */
	sd_event_source *earcon_timer;
	const char *earcon_pending_kind; /* static-duration string (M4_EARCON_CALL_*), NULL = none armed */
	/* mic-on-enter: a SECOND earcon queued to fire right after
	 * `earcon_pending_kind` actually plays (if the mic ends up
	 * muted, play mic-off right after call-on) -- see
	 * daemon_schedule_mic_off_after_call_on()/on_earcon_timer() below for
	 * why this cannot just be a second, independent daemon_schedule_earcon()
	 * call (the shared timer only has room for one pending kind at a time). */
	const char *earcon_chain_kind;
	int earcon_chain_delay_ms;
	struct m4_mpris_player *player; /* not owned */
	m4_player_status_policy_t player_status; /* concrete */
	double last_call_action_end; /* "now" time base of the caller; <0 = never */
	struct m4_daemon_gesture_status gstat;

	/* tap = mic: own
	 * cooldown timer, deliberately separate from last_call_action_end
	 * above (a mic tap and a call on/off swipe are different physical
	 * gestures -- sharing one timer would make a call-off right after a
	 * mic-toggle, or vice versa, spuriously ignored). */
	double last_mic_action_end;
	struct m4_daemon_mic_status mstat;

	/* M4 button = mute (VGM): real-time observation of
	 * the M4 mic Route DURING an already-established call (item 2) --
	 * distinct from `mstat` above (which is only the last-known snapshot
	 * for MIC STATUS) and from pw/micdetect.h's UNRELATED "is some local
	 * PipeWire stream capturing the mic" mic_active/hold_active (automode's
	 * own HFP-entry trigger). `_armed` gates the whole feature: false
	 * whenever there is no established baseline to compare against yet
	 * (route absent, or a call-on's own entry-fix, item 1, is still
	 * settling) -- see daemon_check_mic_route_transition()/
	 * daemon_arm_mic_route_watch() below for exactly when it flips.
	 * `_off` is the last-announced effective mute state (d->mic_mute_threshold
	 * threshold on Route volume alone, see that macro's own comment for why
	 * not the soft SPA_PROP_mute flag too). */
	bool mic_route_watch_armed;
	bool mic_route_watch_off;
};

/* M4 button = mute (VGM): `state->have_mic_route` ALONE is NOT a
 * safe proxy for "a call is active" on this device -- confirmed live
 * (`pw-dump` against the running M4 card): spa/plugins/bluez5/bluez5-device.c's
 * build_route() deliberately keeps SPA_PARAM_Route index=ROUTE_INPUT(0)
 * populated whenever the underlying HFP SLC + mic Source node exist
 * (`profile_direction_mask(..., hfp_input_for_a2dp=true)`), REGARDLESS of
 * whether the profile actually STREAMING is A2DP or HFP -- this M4 keeps
 * that SLC connected continuously (it is how AT+VGM keeps working at all
 * while listening to music in AALL), so have_mic_route reads true ALL THE
 * TIME, not just during an mSBC/CVSD call. graph_ops.h's/callfsm_ops.h's own
 * doc comments ("have_mic_route is false whenever the current profile has no
 * input direction... AALL/a2dp-sink") were WRONG on this point (an
 * unverified assumption from an earlier task) -- everything in this file
 * that means "is the user currently in a call, is it meaningful to touch/
 * announce the mic" (item 2's "during a call", item 3's "not in a call?"
 * gate, item 4's status) must additionally require the ACTIVE profile to be
 * a call profile, not just have_mic_route by itself. */
static bool daemon_mic_route_usable(const struct m4_graph_state *state)
{
	return state->have_mic_route && state->have_profile &&
	       m4_callfsm_is_call_profile(state->profile_name);
}

/* Forward declarations -- see their definitions (further down, alongside the
 * rest of the mic-toggle code) for the full
 * rationale; needed here because on_graph_state_changed() (right below) is
 * the earliest place in the file a Route change is observed. */
static void daemon_schedule_earcon(struct m4_daemon *d, const char *kind, int delay_ms);
static void mic_result(struct m4_daemon *d, enum m4_log_level lvl, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));
static void daemon_check_mic_route_transition(struct m4_daemon *d, const struct m4_graph_state *state);
static void daemon_arm_mic_route_watch(struct m4_daemon *d);
static void daemon_persist_mic_state_on_exit(struct m4_daemon *d); /* "mic-on-enter" task */

static double monotonic_now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* ------------------------------------------------------------------ */
/* write-gating matrix (daemon.h's top comment) */

static bool daemon_allow_manual_write(struct m4_daemon *d)
{
	if (d->mode == M4_DAEMON_MODE_OBSERVE)
		return false;
	return !m4_state_safe_latched(d->runtime_dir);
}

static bool daemon_allow_auto_write(struct m4_daemon *d)
{
	if (d->mode != M4_DAEMON_MODE_AUTO)
		return false;
	return !m4_state_safe_latched(d->runtime_dir);
}

/* ------------------------------------------------------------------ */
/* graph_ops wiring */

static void on_graph_state_changed(void *user_data, const struct m4_graph_state *state)
{
	struct m4_daemon *d = user_data;

	/* "M4 button = mute (VGM)": unlike the reappear-edge
	 * handling below, this one is deliberately done HERE, synchronously,
	 * not deferred to the next tick() -- item 2's own "no delay"
	 * requirement (an earcon right when the button is pressed, not up to
	 * ~250ms later) and daemon_check_mic_route_transition() itself takes
	 * no `now`/wall-clock argument at all (it only ever compares `state`
	 * against the last-armed baseline), so there is no time-base mismatch
	 * risk here the way there is for automode's debounce timers (see the
	 * comment below, unchanged). Called BEFORE d->graph_state is
	 * overwritten -- it does not read d->graph_state at all, only the
	 * fresh incoming `state` and its own d->mic_route_watch_* baseline, so
	 * the order relative to the assignment below does not matter, but
	 * "observe the change, then commit the cache" reads more naturally. */
	daemon_check_mic_route_transition(d, state);

	d->graph_state = *state;
	/* The false->true "M4 reappeared" edge is detected and handled inside
	 * m4_daemon_tick() (daemon_reevaluate_mic_after_reappear(), below),
	 * NOT here -- this callback (struct m4_graph_callbacks.on_state_changed,
	 * graph_ops.h) carries no `now`, and this project's automode timers are
	 * ALWAYS driven by an explicit, caller-supplied `now` (tests and
	 * scenario_runner.c use a fully synthetic timeline that has no
	 * relationship to monotonic_now() -- calling monotonic_now() from here
	 * would silently arm a debounce deadline in the wrong time base and
	 * never fire in any test). m4_daemon_tick() already runs periodically
	 * in production (~every d->tick_interval_ms) and is called
	 * before every scenario/test event, so detecting the edge there costs
	 * at most one tick interval of extra latency and stays entirely within
	 * the one time base every caller already uses. */
}

static void daemon_try_open_graph(struct m4_daemon *d, double now)
{
	static const struct m4_graph_callbacks cb = { .on_state_changed = on_graph_state_changed };

	d->last_graph_open_attempt = now;
	d->graph_handle = d->graph_ops->open(d->event, d->mac_address, &cb, d);
	if (d->graph_handle == NULL)
		m4_log(M4_LOG_WARN, M4_T("momentum4d: PipeWire unavailable, retrying in %.0f s",
"momentum4d: PipeWire недоступен, повторю попытку через %.0f с"),
		       d->graph_retry_s);
}

/* ------------------------------------------------------------------ */
/* automode decision execution (the two gated call sites, daemon.h's matrix) */

static void note_reason_from_state(struct m4_daemon *d, double now)
{
	char reason[M4_CALLFSM_REASON_LEN];

	m4_callfsm_reason(&d->callfsm_ops, reason, sizeof(reason));
	(void)m4_automode_note_external_call_state(d->automode, reason, now);
}

static void daemon_process_automode_decision(struct m4_daemon *d,
					       const m4_automode_decision_t *decision, double now)
{
	for (size_t i = 0; i < decision->action_count; i++) {
		m4_automode_action_t action = decision->actions[i];
		bool want_on;
		m4_callfsm_result_t r;
		int ok;

		if (action == M4_AUTOMODE_ACTION_ANNOUNCE_FREEZE) {
			m4_log(M4_LOG_WARN, M4_T("automode: frozen (looks like switch flapping)",
"automode: заморожен (похоже на флаппинг переключений)"));
			continue;
		}
		if (action != M4_AUTOMODE_ACTION_CALL_ON && action != M4_AUTOMODE_ACTION_CALL_OFF)
			continue;

		want_on = (action == M4_AUTOMODE_ACTION_CALL_ON);

		if (!daemon_allow_auto_write(d)) {
			/* Observe mode: compute + log what WOULD
			 * happen, never call callfsm. Deliberately does NOT
			 * call m4_automode_note_call_result() here -- there is
			 * nothing to roll back, automode's own optimistic
			 * call_active bookkeeping (set inside tick()/
			 * on_mic_activity_changed() before the decision was
			 * even returned to us) is exactly what "as if it had
			 * acted" should look like while observing. */
			m4_log(M4_LOG_INFO,
			       M4_T("automode (mode %s%s): would do %s, but NOT changing the profile",
"automode (режим %s%s): сделал бы %s, но НЕ меняю профиль"),
			       m4_daemon_mode_name(d->mode),
			       m4_state_safe_latched(d->runtime_dir) ? ", safe-latch" : "",
			       want_on ? "CALL_ON" : "CALL_OFF");
			continue;
		}

		if (d->action_in_progress) {
			/* Known limitation: a busy
			 * collision with a concurrent ctl.sock action drops
			 * this automode decision outright rather than
			 * queueing/retrying it -- automode's own has_pending_
			 * on/off was already cleared by tick()/
			 * on_mic_activity_changed() before returning this
			 * decision, so it will not be retried automatically;
			 * the next mic transition re-arms it. */
			m4_log(M4_LOG_WARN,
			       M4_T("automode: %s skipped -- another action is already in progress",
"automode: %s пропущено -- другое действие уже выполняется"),
			       want_on ? "CALL_ON" : "CALL_OFF");
			continue;
		}

		/* Every automode decision is logged (info) -- logged
		 * HERE, at the moment automode's decision is actually about
		 * to be executed (the "would have done X, but NOT changing" branch
		 * above already covers the observe-mode case of this same
		 * requirement), separately from the result+timing line below
		 * (every profile switch with its actual result and
		 * confirmation time). */
		m4_log(M4_LOG_INFO, M4_T("automode (mode %s): decision %s -- executing",
"automode (режим %s): решение %s -- выполняю"), m4_daemon_mode_name(d->mode),
		       want_on ? "CALL_ON" : "CALL_OFF");

		double t0 = monotonic_now();

		/* mic-on-enter: capture the pre-exit mic state for an
		 * automode-triggered CALL_OFF too (daemon_persist_mic_state_on_exit()'s
		 * own doc comment on why BOTH exit paths need this) -- BEFORE
		 * m4_callfsm_off() runs, same ordering rationale as
		 * daemon_execute_call()'s manual path (the graph state observed
		 * right now is still "as of this call", automode's own quiet=1
		 * path never calls daemon_force_mic_unmute() at all today, so
		 * there is no later overwrite to race here either way). */
		if (!want_on)
			daemon_persist_mic_state_on_exit(d);

		d->action_in_progress = true;
		r = want_on ? m4_callfsm_on(&d->callfsm_ops, 1, "auto")
			    : m4_callfsm_off(&d->callfsm_ops, 1);
		d->action_in_progress = false;

		ok = (r.rc == M4_CALLFSM_RC_OK);
		m4_automode_note_call_result(d->automode, action, ok, now);
		m4_log(ok ? M4_LOG_INFO : M4_LOG_WARN, M4_T("automode: %s -> %s (%.3f s)",
"automode: %s -> %s (%.3f с)"),
		       want_on ? "CALL_ON" : "CALL_OFF", r.message, monotonic_now() - t0);
		if (ok)
			note_reason_from_state(d, now);
	}
}

/* Bug 1 gap fix: replays whichever of
 * mic_active/hold_active is CURRENTLY true as a fresh false->true edge, so
 * automode re-arms the attempt it lost when M4 disappeared mid-debounce.
 * Called from m4_daemon_tick() (below) on a false->true `present` edge --
 * simply having mic_active/hold_active still be "true" produces no
 * automatic action on its own: M4 disappearing mid-debounce fails the
 * in-flight callfsm attempt (card_present() returns 0), which rolls
 * call_active back via note_call_result() -- but automode's OWN
 * mic_active/hold_active fields are untouched by any of that (they are not
 * "call state", see core/automode.h), so on_mic_activity_changed()/
 * on_mic_hold_changed() see no CHANGE in value and stay silent (idempotent
 * no-ops) unless something replays a fresh edge, which is exactly what this
 * function does. Each replay's own false-edge half is a correctly-idempotent
 * no-op on
 * everything that ISN'T the field being replayed (e.g. replaying
 * mic_active never touches hold_active's pending state), and produces no
 * action itself (Decision.actions is always empty for the arm-side of
 * on_mic_activity_changed()/on_mic_hold_changed() -- only tick() ever
 * executes CALL_ON/CALL_OFF) -- daemon_process_automode_decision() is
 * still called for both, for symmetry with every other call site in this
 * file and because it is a documented no-op on an empty decision. */
static void daemon_reevaluate_mic_after_reappear(struct m4_daemon *d, double now)
{
	m4_automode_status_t ast;
	m4_automode_decision_t dec;

	m4_automode_status(d->automode, now, &ast);

	if (ast.mic_active) {
		dec = m4_automode_on_mic_activity_changed(d->automode, false, now);
		daemon_process_automode_decision(d, &dec, now);
		dec = m4_automode_on_mic_activity_changed(d->automode, true, now);
		daemon_process_automode_decision(d, &dec, now);
	}
	if (ast.hold_active) {
		dec = m4_automode_on_mic_hold_changed(d->automode, false, now);
		daemon_process_automode_decision(d, &dec, now);
		dec = m4_automode_on_mic_hold_changed(d->automode, true, now);
		daemon_process_automode_decision(d, &dec, now);
	}
}

static void on_micdetect_active_changed(void *user_data, bool active, bool hold_active)
{
	struct m4_daemon *d = user_data;
	double now = monotonic_now();

	/* Both, always, regardless of which one(s) actually changed -- see
	 * daemon.h's m4_daemon_mic_hold_changed() doc comment for why this is
	 * safe (both idempotent on a repeated value). */
	m4_daemon_mic_active_changed(d, active, now);
	m4_daemon_mic_hold_changed(d, hold_active, now);
}

void m4_daemon_mic_active_changed(struct m4_daemon *d, bool active, double now)
{
	m4_automode_decision_t decision = m4_automode_on_mic_activity_changed(d->automode, active, now);

	daemon_process_automode_decision(d, &decision, now);
}

void m4_daemon_mic_hold_changed(struct m4_daemon *d, bool hold_active, double now)
{
	m4_automode_decision_t decision = m4_automode_on_mic_hold_changed(d->automode, hold_active, now);

	daemon_process_automode_decision(d, &decision, now);
}

void m4_daemon_tick(struct m4_daemon *d, double now)
{
	m4_automode_decision_t decision;
	bool present_now;

	if (d->graph_handle == NULL && now - d->last_graph_open_attempt >= d->graph_retry_s)
		daemon_try_open_graph(d, now);

	/* Bug 1 gap fix: detect the
	 * false->true "M4 reappeared" edge HERE, using this call's own `now`
	 * -- NOT in on_graph_state_changed(), which has no `now` at all (see
	 * that function's comment). `present_now` is read from graph_state,
	 * which on_graph_state_changed() already keeps up to date
	 * asynchronously/immediately; `present_seen` is this function's own
	 * once-per-tick snapshot used only to detect the edge. */
	present_now = d->graph_state.m4_present;
	if (present_now && !d->present_seen)
		daemon_reevaluate_mic_after_reappear(d, now);
	d->present_seen = present_now;

	/* M4 button = mute (VGM): safety net for daemon_arm_mic_route_watch()
	 * below, which is normally armed explicitly, right as a call-on
	 * finishes settling (daemon_execute_call()). Two cases that misses:
	 * momentum4d restarting mid-call (a fresh on_graph_state_changed()
	 * burst arrives with have_mic_route already true, but no call-on ever
	 * ran in THIS process to arm it), and the card being switched into a
	 * call profile by something other than callfsm (core/callfsm.c's
	 * m4_callfsm_on() own "manual call mode with no saved state"
	 * branch -- it returns before ever reaching fix_mic_volume_if_needed()/
	 * announcing, so daemon_execute_call() never arms the watch for that
	 * path either). Cheap and safe to check every tick: no I/O, just two
	 * cached bools compared (graph_ops.h's get_state()/on_state_changed()
	 * contract is "always instant" already) -- and this can never race the
	 * item-1 entry-fix settling window the way arming unconditionally from
	 * on_graph_state_changed() itself would: that settling only
	 * ever happens synchronously inside m4_callfsm_on(), which itself only
	 * ever runs from a ctl.sock/gesture/HFP/automode call site, NEVER from
	 * inside m4_daemon_tick(). daemon_mic_route_usable(), not raw
	 * have_mic_route: that field alone stays true even outside a call on
	 * this device (that function's own comment) -- arming unconditionally
	 * on it would arm the watch during ordinary AALL listening too, and
	 * then fire mic-on/off earcons over music the first time the M4's
	 * background HFP SLC gain happens to change for any reason. */
	if (!d->mic_route_watch_armed && daemon_mic_route_usable(&d->graph_state))
		daemon_arm_mic_route_watch(d);

	decision = m4_automode_tick(d->automode, now);
	daemon_process_automode_decision(d, &decision, now);
}

static int on_tick_timer(sd_event_source *s, uint64_t usec, void *userdata)
{
	struct m4_daemon *d = userdata;

	(void)usec;
	m4_daemon_tick(d, monotonic_now());
	sd_event_source_set_time_relative(s, (uint64_t)d->tick_interval_ms * 1000);
	sd_event_source_set_enabled(s, SD_EVENT_ONESHOT);
	return 0;
}

/* ------------------------------------------------------------------ */
/* ctl.sock request handlers */

static int on_earcon_timer(sd_event_source *s, uint64_t usec, void *userdata)
{
	struct m4_daemon *d = userdata;

	(void)s;
	(void)usec;
	if (d->earcon != NULL && d->earcon_pending_kind != NULL)
		d->earcon(d->earcon_ctx, d->settings.earcon_dir, d->earcon_pending_kind);
	d->earcon_pending_kind = NULL;

	/* mic-on-enter: the kind that just played may have a chained
	 * follow-up queued (daemon_schedule_mic_off_after_call_on() below) --
	 * fire it as a fresh, independent daemon_schedule_earcon() call now
	 * that the timer slot it was riding along on is free again. */
	if (d->earcon_chain_kind != NULL) {
		const char *chain_kind = d->earcon_chain_kind;
		int chain_delay_ms = d->earcon_chain_delay_ms;

		d->earcon_chain_kind = NULL;
		daemon_schedule_earcon(d, chain_kind, chain_delay_ms);
	}
	return 0;
}

/* Plays `kind` `delay_ms` after this call, WITHOUT blocking the caller or
 * the event loop (an sd-event oneshot timer, exactly like on_tick_timer()
 * and mpris_player.c's own retry timer -- never a sleep()/busy-wait). The
 * profile switch itself has already completed by the time this is called
 * (daemon_execute_call() calls it only after m4_callfsm_on()/off() already
 * returned OK) -- only the SOUND is delayed, so the switch's own latency is
 * completely unaffected.
 *
 * Why a fixed delay from config instead of waiting for "M4's node reaches
 * SPA_NODE_STATE_RUNNING": core/graph_ops.h (this daemon's only PipeWire
 * abstraction) tracks the M4 DEVICE's Profile, not any Node's runtime
 * state at all -- there is currently no plumbing anywhere in momentum4d
 * that could tell this function "the M4 node just started
 * producing/consuming buffers" without first extending
 * graph_ops.h/graph_pw.c with a new subscription (Node info + state
 * changes) it does not have today. Given the explicit defaults chosen
 * here (on=400ms, off=500ms, chosen to cover the combine-stream
 * participant recreation + aptX Adaptive helper cold-start), the
 * fixed-delay version already closes the "sound plays into nothing" bug
 * this fixes; a RUNNING-event version would be a strictly-better
 * follow-up, not attempted here.
 *
 * `delay_ms <= 0` plays immediately (synchronously, from this call) --
 * used for the call-error earcon (daemon_play_earcon_error() below), where
 * nothing is "becoming ready" to wait for, and kept as a fast path so
 * `earcon-delay-*-ms = 0` behaves exactly like the previous, undelayed
 * code (no timer round-trip at all -- this is also what lets old tests that assert an
 * earcon fired synchronously keep working unmodified when they explicitly
 * set the delay to 0, see tests/test_gestures.c). */
static void daemon_schedule_earcon(struct m4_daemon *d, const char *kind, int delay_ms)
{
	uint64_t usec;

	/* mic-on-enter: ANY fresh call here invalidates a previously
	 * queued chain (daemon_schedule_mic_off_after_call_on() below) -- a
	 * chain is only ever meaningful relative to the specific call-on
	 * schedule that queued it; a NEW, unrelated earcon (a later call-off,
	 * call-error, or a second call-on before the first one's chain fired)
	 * must not let a stale mic-off fire on top of it. on_earcon_timer()'s
	 * own re-arm for the chain clears this field itself right before
	 * calling back in here, so this is a no-op on that path. */
	d->earcon_chain_kind = NULL;

	if (!d->settings.earcon || d->earcon == NULL || kind == NULL)
		return;

	if (delay_ms <= 0) {
		d->earcon(d->earcon_ctx, d->settings.earcon_dir, kind);
		return;
	}
	usec = (uint64_t)delay_ms * 1000ULL;

	d->earcon_pending_kind = kind; /* supersedes any earlier still-pending earcon */
	if (d->earcon_timer == NULL) {
		if (sd_event_add_time_relative(d->event, &d->earcon_timer, CLOCK_MONOTONIC, usec, 1000,
						on_earcon_timer, d) < 0) {
			m4_log(M4_LOG_WARN, M4_T("earcon: failed to arm the delay timer -- playing immediately",
"earcon: не удалось завести таймер задержки -- играю сразу"));
			d->earcon(d->earcon_ctx, d->settings.earcon_dir, kind);
			d->earcon_pending_kind = NULL;
		}
		return;
	}
	sd_event_source_set_time_relative(d->earcon_timer, usec);
	sd_event_source_set_enabled(d->earcon_timer, SD_EVENT_ONESHOT);
}

static void daemon_play_earcon(struct m4_daemon *d, m4_callfsm_announce_t announce)
{
	int delay_ms;
	const char *kind;

	if (announce == M4_CALLFSM_ANNOUNCE_NONE)
		return;
	if (announce == M4_CALLFSM_ANNOUNCE_ON) {
		kind = M4_EARCON_CALL_ON;
		delay_ms = d->settings.earcon_delay_on_ms;
	} else {
		kind = M4_EARCON_CALL_OFF;
		delay_ms = d->settings.earcon_delay_off_ms;
	}
	daemon_schedule_earcon(d, kind, delay_ms);
}

/* mic-on-enter: queues M4_EARCON_MIC_OFF to play strictly AFTER
 * call-on's own sound (if the mic ends up
 * muted, play mic-off right after call-on, so the user knows).
 * Called from daemon_execute_call()'s on-branch below, ONLY when call-on
 * was actually just scheduled (r.announce == ANNOUNCE_ON, earcon enabled)
 * AND the mic-route watch's freshly-armed baseline (daemon_arm_mic_route_watch(),
 * called right before daemon_play_earcon() in that same branch -- see its
 * own comment there for why the ORDER matters here) shows the mic ended up
 * muted (`d->mic_route_watch_off`), regardless of WHICH mic-on-enter mode
 * caused that (off, or remember with a muted last-known state) -- this
 * function itself does not need to know which.
 *
 * `on_earcon_delay_ms` is exactly the delay daemon_play_earcon() just used
 * for call-on (0 = it already played synchronously, an instant ago, from
 * the very same call stack -- nothing is occupying the shared earcon timer
 * right now). Two cases:
 *   - on_earcon_delay_ms <= 0: schedule mic-off as a normal, independent
 *     daemon_schedule_earcon() call (chain-delay from now).
 *   - on_earcon_delay_ms > 0: call-on is still pending in the shared timer
 *     (one pending-kind slot, see daemon_schedule_earcon()'s own top
 *     comment) -- queue mic-off as a CHAIN instead of a second, immediately
 *     competing daemon_schedule_earcon() call, which would just overwrite/
 *     cancel the still-pending call-on request. on_earcon_timer() fires it
 *     right after call-on actually plays. */
static void daemon_schedule_mic_off_after_call_on(struct m4_daemon *d, int on_earcon_delay_ms)
{
	int chain_ms = d->settings.mic_off_chain_delay_ms; /* already validated 0..30000 by settings.c */

	if (!d->settings.earcon || d->earcon == NULL)
		return;
	if (on_earcon_delay_ms <= 0) {
		daemon_schedule_earcon(d, M4_EARCON_MIC_OFF, chain_ms);
		return;
	}
	d->earcon_chain_kind = M4_EARCON_MIC_OFF;
	d->earcon_chain_delay_ms = chain_ms;
}

/* call-error on failure, in observe mode, and on a
 * safe-latch, but NOT on a cooldown hit): always immediate (delay_ms=0) -- there is no
 * profile switch in flight to wait for in any of these cases (a failed
 * switch has already finished failing; a blocked-by-mode/safe-latch action
 * never started one at all). Called from THREE kinds of call sites, all
 * sharing the same daemon.h write-gating matrix: daemon_execute_call()'s
 * own r.rc != OK path (covers handle_call()/daemon_gesture_call()/
 * daemon_hfp_call() uniformly, since all three funnel through it) for
 * a failure, and directly from handle_call()/daemon_gesture_call()/
 * daemon_hfp_call()'s own safe-latch/observe-mode gate checks for the other
 * two cases (deliberately NOT from their cooldown checks, and NOT from
 * their action_in_progress/busy checks -- neither is in this feature's list,
 * and both are handled by the underlying condition simply persisting for
 * the (typically sub-second) time the earlier action needs to finish). */
static void daemon_play_earcon_error(struct m4_daemon *d)
{
	daemon_schedule_earcon(d, M4_EARCON_CALL_ERROR, 0);
}

/* tap = mic: on call off,
 * or on exiting HFP, the mic is always unmuted (so the next
 * call does not start muted). Called from daemon_execute_call() below, only
 * after a successful call-OFF -- fire-and-forget (same contract as
 * core/graph_ops.h's set_mic_volume() itself), NOT gated by cooldown/
 * action_in_progress/mode (it rides along inside an already-gated,
 * already-in-flight call-off, not a new user-visible action of its own), and
 * deliberately does NOT play a mic-on earcon (call-off already has its own
 * earcon; a second beep right on top of it would be confusing, not
 * reassuring). Silently a no-op if there is no
 * graph handle at all; otherwise ALWAYS issues the write (idempotent, a
 * single cheap async pw_device_set_param() -- matching the literal
 * requirement of "always", not just "when we last observed it as muted"), but only logs
 * when the cached state shows it actually mattered, to avoid a log line on
 * every single ordinary call-off. */
/* "mic-on-enter" task (`[call] mic-on-enter = remember`): persists the M4
 * mic Route's CURRENT mute state (core/state.h's m4_mic_state_write(), both
 * `d->runtime_dir` and `d->state_dir`) as "the state at the last HFP exit".
 * Called from BOTH exit paths -- daemon_execute_call()'s off-branch (manual/
 * ctl.sock/gesture/HFP call-off) below, and daemon_process_automode_decision()'s
 * own CALL_OFF branch (automode-triggered) -- so remember mode's memory
 * stays accurate regardless of WHICH path ended the call, matching the
 * registry key's own "at the last HFP exit", not "the last
 * MANUAL exit". Read-only wrt the graph (only reads the already-cached
 * `d->graph_state`, the same snapshot daemon_force_mic_unmute() itself reads
 * right after this in the manual path) -- never blocks, never retries; a
 * failed write is only logged (best-effort, matches every other persistence
 * call in this file, e.g. m4_state_mode_write()'s own call site). No-op if
 * the Route was not observed at all right now (nothing meaningful to record --
 * does NOT overwrite a possibly still-valid earlier record with a guess). */
static void daemon_persist_mic_state_on_exit(struct m4_daemon *d)
{
	bool muted;

	if (!d->graph_state.have_mic_route)
		return;
	muted = d->graph_state.mic_volume <= d->mic_mute_threshold;
	if (m4_mic_state_write(d->runtime_dir, d->state_dir, muted) != 0)
		m4_log(M4_LOG_WARN, M4_T("mic: failed to save microphone state (mic-on-enter=remember)",
"mic: не удалось сохранить состояние микрофона (mic-on-enter=remember)"));
}

static void daemon_force_mic_unmute(struct m4_daemon *d)
{
	bool was_muted;

	if (d->graph_handle == NULL || d->graph_ops->set_mic_volume == NULL)
		return;
	/* M4 button = mute (VGM): volume-only, like everywhere else in
	 * this file now (d->mic_mute_threshold's own comment) -- the soft
	 * SPA_PROP_mute flag this used to check is never set by M4 itself. */
	was_muted = d->graph_state.have_mic_route && d->graph_state.mic_volume <= d->mic_mute_threshold;
	d->graph_ops->set_mic_volume(d->graph_handle, 1.0f, false, true);
	if (was_muted)
		m4_log(M4_LOG_INFO, M4_T("mic: automatically unmuted on leaving the call",
"mic: автоматически размьючен при выходе из звонка"));
}

/* The one place a MANUAL (ctl.sock or gesture) call on/off is executed --
 * callers have already applied the write gate (daemon.h's matrix) and the
 * action_in_progress check. `origin` only goes into the log line. */
static m4_callfsm_result_t daemon_execute_call(struct m4_daemon *d, bool on, const char *origin,
						double *elapsed_out)
{
	m4_callfsm_result_t r;
	double t0 = monotonic_now();

	d->action_in_progress = true;
	r = on ? m4_callfsm_on(&d->callfsm_ops, 0, "manual") : m4_callfsm_off(&d->callfsm_ops, 0);
	d->action_in_progress = false;
	*elapsed_out = monotonic_now() - t0;

	/* Every profile
	 * switch, with its actual result and confirmation time, is logged
	 * (info). This is the request-level summary (one line for the
	 * whole call on/off, including any mSBC->CVSD fallback and/or
	 * repair-a2dp within it); pw/callfsm_graph_adapter.c additionally
	 * logs each individual set_profile_confirm() attempt on its own. */
	m4_log(r.rc == M4_CALLFSM_RC_OK ? M4_LOG_INFO : M4_LOG_WARN,
	       M4_T("call %s (%s): %s (%.3f s)",
"call %s (%s): %s (%.3f с)"), on ? "on" : "off", origin, r.message, *elapsed_out);

	if (r.rc == M4_CALLFSM_RC_OK) {
		note_reason_from_state(d, monotonic_now());
		if (on) {
			/* "M4 button = mute (VGM)": arm the
			 * real-time mic-route watch ONLY here, AFTER
			 * m4_callfsm_on() (and its own fix_mic_volume_if_needed()
			 * entry-fix) has ALREADY fully run and settled -- NOT from
			 * on_graph_state_changed() itself the instant have_mic_route
			 * first becomes true. Arming any earlier would race the
			 * entry-fix's OWN set_mic_volume(...) write: that write
			 * flows through the exact same on_graph_state_changed()
			 * path a real button press does (both are just a Route
			 * PARAMS change from callfsm_graph_adapter.c's point of
			 * view), so an already-armed watch would see it as a
			 * spurious "mic on/off" transition and fire a SECOND
			 * earcon right on top of the call-on one -- exactly the
			 * kind of doubling item 3 asks NOT to do. Only announced
			 * transitions from here on (a real button press, or an
			 * explicit `momentum4 mic on/off/toggle`) get an earcon.
			 *
			 * mic-on-enter: moved to BEFORE daemon_play_earcon()
			 * below (used to come after it) -- arming captures the
			 * SETTLED post-entry-fix state into `d->mic_route_watch_off`
			 * (daemon_arm_mic_route_watch()'s own doc comment), which
			 * daemon_schedule_mic_off_after_call_on() right below needs
			 * to decide whether a mic-off chain is warranted at all;
			 * this reordering has no effect on WHEN anything is written
			 * to the graph (only on daemon-local bookkeeping order, both
			 * strictly after m4_callfsm_on() has already fully returned). */
			daemon_arm_mic_route_watch(d);
			daemon_play_earcon(d, r.announce);
			if (r.announce == M4_CALLFSM_ANNOUNCE_ON && d->mic_route_watch_off)
				daemon_schedule_mic_off_after_call_on(d, d->settings.earcon_delay_on_ms);
		} else {
			/* mic-on-enter: capture the mic Route's state as it
			 * stood AT THE MOMENT OF THIS EXIT -- strictly BEFORE
			 * daemon_force_mic_unmute() below overwrites it -- so
			 * `[call] mic-on-enter = remember`'s next HFP entry can
			 * restore exactly this. Best-effort/read-only wrt the
			 * graph (only a small file write); always attempted,
			 * regardless of the CURRENTLY configured mic-on-enter mode,
			 * so switching TO remember later already has fresh history
			 * (the state is restored as of the LAST HFP exit,
			 * not the last exit while already in remember mode). */
			daemon_persist_mic_state_on_exit(d);
			daemon_play_earcon(d, r.announce);
			daemon_force_mic_unmute(d); /* task: call off/HFP exit always unmutes */
			/* item 5: mute state is not carried over -- the next
			 * call-on re-arms from scratch (daemon_check_mic_route_
			 * transition() below also disarms on have_mic_route
			 * dropping to false, e.g. an M4 disconnect mid-call; this
			 * is the immediate, "call off succeeded" half of the same
			 * rule). */
			d->mic_route_watch_armed = false;
		}
	} else {
		daemon_play_earcon_error(d); /* call-error on failure */
	}
	return r;
}

static void handle_call(struct m4_daemon *d, bool on, char *response, size_t cap)
{
	m4_callfsm_result_t r;
	double elapsed_s;

	if (m4_state_safe_latched(d->runtime_dir)) {
		snprintf(response, cap, M4_T("ERR safe-latch is active -- run 'momentum4 resume'\n",
"ERR safe-latch активна -- выполните 'momentum4 resume'\n"));
		daemon_play_earcon_error(d); /* call-error on safe-latch */
		return;
	}
	if (!daemon_allow_manual_write(d)) {
		snprintf(response, cap,
			 M4_T("ERR mode %s -- run 'momentum4 mode manual' or 'momentum4 mode auto'\n",
"ERR режим %s -- выполните 'momentum4 mode manual' или 'momentum4 mode auto'\n"),
			 m4_daemon_mode_name(d->mode));
		daemon_play_earcon_error(d); /* call-error in observe mode */
		return;
	}
	if (d->action_in_progress) {
		snprintf(response, cap, M4_T("ERR another action is already in progress, try again\n",
"ERR другое действие уже выполняется, повторите\n"));
		return;
	}

	r = daemon_execute_call(d, on, M4_T("manual",
"ручной"), &elapsed_s);
	d->last_call_action_end = monotonic_now();

	if (r.rc == M4_CALLFSM_RC_OK)
		snprintf(response, cap, "OK\nmessage=%s\n", r.message);
	else
		snprintf(response, cap, "ERR %s\n", r.message);
}

/* ctl.sock REPAIR_A2DP (an explicit `momentum4
 * repair-a2dp` command): same write-gating as handle_call() above (daemon.h's
 * matrix comment) -- this is a manual, explicit action, never
 * automode-triggered (automode's own path to repair is
 * core/callfsm.c's repair_profile_if_missing(), reached only from inside
 * m4_callfsm_off(), already gated by the SAME daemon_allow_*_write() the
 * normal CALL_OFF path uses). */
static void handle_repair_a2dp(struct m4_daemon *d, char *response, size_t cap)
{
	struct m4_repair_a2dp_config rcfg = {0};
	struct m4_repair_a2dp_result r;

	if (m4_state_safe_latched(d->runtime_dir)) {
		snprintf(response, cap, M4_T("ERR safe-latch is active -- run 'momentum4 resume'\n",
"ERR safe-latch активна -- выполните 'momentum4 resume'\n"));
		return;
	}
	if (!daemon_allow_manual_write(d)) {
		snprintf(response, cap,
			 M4_T("ERR mode %s -- run 'momentum4 mode manual' or 'momentum4 mode auto'\n",
"ERR режим %s -- выполните 'momentum4 mode manual' или 'momentum4 mode auto'\n"),
			 m4_daemon_mode_name(d->mode));
		return;
	}
	if (d->bt_ops == NULL) {
		snprintf(response, cap, M4_T("ERR repair-a2dp unavailable (the daemon is running without bt_ops)\n",
"ERR repair-a2dp недоступен (демон запущен без bt_ops)\n"));
		return;
	}
	if (d->graph_handle == NULL) {
		snprintf(response, cap, M4_T("ERR PipeWire is currently unavailable to the daemon\n",
"ERR PipeWire сейчас недоступен демону\n"));
		return;
	}
	if (d->action_in_progress) {
		snprintf(response, cap, M4_T("ERR another action is already in progress, try again\n",
"ERR другое действие уже выполняется, повторите\n"));
		return;
	}

	rcfg.graph_ops = d->graph_ops;
	rcfg.graph_handle = d->graph_handle;
	rcfg.bt_ops = d->bt_ops;
	rcfg.event = d->event;
	rcfg.mac_address = d->mac_address;
	rcfg.bt_adapter = d->bt_adapter;
	rcfg.connect_timeout_ms = d->repair_connect_timeout_ms;
	rcfg.enum_wait_ms = d->repair_enum_wait_ms;
	rcfg.confirm_timeout_ms =
		d->repair_a2dp_confirm_timeout_ms > 0 ? d->repair_a2dp_confirm_timeout_ms : d->confirm_timeout_ms;

	d->action_in_progress = true;
	r = m4_repair_a2dp_run(&rcfg);
	d->action_in_progress = false;

	if (r.status == M4_REPAIR_A2DP_OK)
		snprintf(response, cap, "OK\nmessage=%s\n", r.message);
	else
		snprintf(response, cap, "ERR %s\n", r.message);
}

static void handle_mode(struct m4_daemon *d, const char *arg, char *response, size_t cap)
{
	m4_daemon_mode_t new_mode;

	if (m4_daemon_mode_parse(arg, &new_mode) < 0) {
		snprintf(response, cap, M4_T("ERR unknown mode: %s (expected observe|manual|auto)\n",
"ERR неизвестный режим: %s (ожидается observe|manual|auto)\n"),
			 arg);
		return;
	}
	d->mode = new_mode;
	if (m4_state_mode_write(d->runtime_dir, new_mode) < 0)
		m4_log(M4_LOG_WARN, M4_T("momentum4d: failed to save the mode to disk",
"momentum4d: не удалось сохранить режим на диск"));
	m4_log(M4_LOG_INFO, M4_T("momentum4d: mode changed to %s",
"momentum4d: режим изменён на %s"), m4_daemon_mode_name(new_mode));
	snprintf(response, cap, "OK\nmode=%s\n", m4_daemon_mode_name(new_mode));
}

static void handle_get(struct m4_daemon *d, char *response, size_t cap)
{
	m4_automode_status_t ast;

	m4_automode_status(d->automode, monotonic_now(), &ast);
	snprintf(response, cap,
		 "OK\n"
		 "mode=%s\n"
		 "safe_latched=%s\n"
		 "m4_present=%s\n"
		 "m4_connected=%s\n"
		 "profile=%s\n"
		 "frozen=%s\n"
		 "mic_active=%s\n"
		 "call_active=%s\n"
		 "call_reason=%s\n"
		 "switches_in_window=%d\n",
		 m4_daemon_mode_name(d->mode),
		 m4_state_safe_latched(d->runtime_dir) ? "true" : "false",
		 d->graph_state.m4_present ? "true" : "false",
		 d->graph_state.m4_connected ? "true" : "false",
		 d->graph_state.have_profile ? d->graph_state.profile_name : "",
		 ast.frozen ? "true" : "false",
		 ast.mic_active ? "true" : "false",
		 ast.call_active ? "true" : "false",
		 ast.call_active_reason,
		 ast.switches_in_window);
}

static void handle_status(struct m4_daemon *d, char *response, size_t cap)
{
	m4_automode_status_t ast;
	enum m4_mode mode = d->graph_state.have_profile ? m4_profile_mode(d->graph_state.profile_name)
							  : M4_MODE_UNKNOWN;
	int n = 0;

	m4_automode_status(d->automode, monotonic_now(), &ast);

	n += snprintf(response + n, cap - (size_t)n, "OK\n");
	if (!d->graph_state.m4_present)
		n += snprintf(response + n, cap - (size_t)n,
			      M4_T("text=M4: not found in PipeWire\n",
"text=M4: не найдены в PipeWire\n"));
	else if (!d->graph_state.m4_connected)
		n += snprintf(response + n, cap - (size_t)n,
			      M4_T("text=M4: card present, but the headset is not connected\n",
"text=M4: карта есть, но наушники не подключены\n"));
	else
		n += snprintf(response + n, cap - (size_t)n, M4_T("text=M4: connected, mode %s\n",
"text=M4: подключены, режим %s\n"),
			      m4_mode_human(mode));
	n += snprintf(response + n, cap - (size_t)n,
		      M4_T("text=Daemon: mode %s%s, call %s (%s)%s\n",
"text=Демон: режим %s%s, звонок %s (%s)%s\n"),
		      m4_daemon_mode_name(d->mode),
		      m4_state_safe_latched(d->runtime_dir) ? M4_T(", safe-latch LATCHED",
", ЗАЩЁЛКА safe-latch") : "",
		      ast.call_active ? M4_T("active",
"активен") : M4_T("not active",
"не активен"), ast.call_active_reason,
		      ast.frozen ? M4_T(", FROZEN (looks like flapping)",
", ЗАМОРОЖЕН (похоже на флаппинг)") : "");
	/* "tap = mic" task (volume-only, fixed by "M4 button =
	 * mute (VGM)" task): shown only while actually in a call
	 * (daemon_mic_route_usable(), NOT raw have_mic_route -- that field
	 * alone stays true even in AALL on this device, see its own comment) --
	 * matches m4_daemon_get_mic_status()'s own gating/volume-based `mute`
	 * semantics. */
	if (daemon_mic_route_usable(&d->graph_state))
		snprintf(response + n, cap - (size_t)n, M4_T("text=Microphone: %s\n",
"text=Микрофон: %s\n"),
			 d->graph_state.mic_volume <= d->mic_mute_threshold ? M4_T("off",
"выключен") : M4_T("on",
"включён"));
}

/* Forward declaration: "tap = mic" task -- the gated
 * mic-toggle entry point is defined further down (after the HFP section,
 * which it is also shared with), but both m4_daemon_player_event() (gesture
 * path, right below) and m4_daemon_hfp_event() (HFP path) need to call it. */
static void daemon_mic_toggle_gated(struct m4_daemon *d, const char *origin, double now);

/* ------------------------------------------------------------------ */
/* gestures */

static void format_wallclock(uint64_t realtime_usec, char *buf, size_t cap)
{
	time_t sec = (time_t)(realtime_usec / 1000000ULL);
	struct tm tm;

	if (realtime_usec == 0 || localtime_r(&sec, &tm) == NULL) {
		snprintf(buf, cap, "?");
		return;
	}
	snprintf(buf, cap, "%02d:%02d:%02d.%03u", tm.tm_hour, tm.tm_min, tm.tm_sec,
		 (unsigned)((realtime_usec / 1000ULL) % 1000ULL));
}

static void gesture_result(struct m4_daemon *d, enum m4_log_level lvl, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));

static void gesture_result(struct m4_daemon *d, enum m4_log_level lvl, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(d->gstat.last_result, sizeof(d->gstat.last_result), fmt, ap);
	va_end(ap);
	m4_log(lvl, M4_T("gesture: %s",
"жест: %s"), d->gstat.last_result);
}

static void daemon_gesture_call(struct m4_daemon *d, m4_gesture_action_t action, m4_player_cmd_t cmd,
				 double now)
{
	double cooldown_s = (double)d->settings.gesture_cooldown_ms / 1000.0;
	double since = now - d->last_call_action_end;
	m4_callfsm_result_t r;
	char origin[48];
	double elapsed_s;
	bool on;

	if (d->last_call_action_end >= 0 && since >= 0 && since < cooldown_s) {
		d->gstat.ignored++;
		gesture_result(d, M4_LOG_INFO,
			       M4_T("%s ignored -- cooldown after the last switch (%.2f s < %.2f s)",
"%s проигнорирован -- пауза после прошлого переключения (%.2f с < %.2f с)"),
			       m4_gesture_action_name(action), since, cooldown_s);
		return;
	}
	if (m4_state_safe_latched(d->runtime_dir)) {
		d->gstat.ignored++;
		gesture_result(d, M4_LOG_WARN, M4_T("%s NOT executed -- safe-latch (momentum4 resume)",
"%s НЕ выполнен -- safe-latch (momentum4 resume)"),
			       m4_gesture_action_name(action));
		daemon_play_earcon_error(d); /* call-error on safe-latch */
		return;
	}
	if (!daemon_allow_manual_write(d)) {
		d->gstat.ignored++;
		gesture_result(d, M4_LOG_INFO, M4_T("%s NOT executed -- mode %s (need manual or auto)",
"%s НЕ выполнен -- режим %s (нужен manual или auto)"),
			       m4_gesture_action_name(action), m4_daemon_mode_name(d->mode));
		daemon_play_earcon_error(d); /* call-error in observe mode */
		return;
	}
	if (d->action_in_progress) {
		d->gstat.ignored++;
		gesture_result(d, M4_LOG_WARN, M4_T("%s NOT executed -- another action is already in progress",
"%s НЕ выполнен -- другое действие уже выполняется"),
			       m4_gesture_action_name(action));
		return;
	}

	if (action == M4_GESTURE_ACTION_CALL_ON) {
		on = true;
	} else if (action == M4_GESTURE_ACTION_CALL_OFF) {
		on = false;
	} else {
		m4_callfsm_status_t st = m4_callfsm_status(&d->callfsm_ops);

		on = !st.call_active;
	}

	snprintf(origin, sizeof(origin), M4_T("gesture %s",
"жест %s"), m4_player_cmd_method(cmd));
	d->gstat.call_actions++;
	r = daemon_execute_call(d, on, origin, &elapsed_s);
	/* measured from the END of the (blocking) switch: commands M4 sent
	 * while we were busy are dispatched right after and must not toggle
	 * straight back */
	d->last_call_action_end = now + elapsed_s;
	gesture_result(d, r.rc == M4_CALLFSM_RC_OK ? M4_LOG_INFO : M4_LOG_WARN, M4_T("call %s -> %s (%.3f s)",
"call %s -> %s (%.3f с)"),
		       on ? "on" : "off", r.rc == M4_CALLFSM_RC_OK ? "ok" : r.message, elapsed_s);
}

static void daemon_gesture_passthrough(struct m4_daemon *d, m4_gesture_action_t action, m4_player_cmd_t cmd)
{
	const char *method = m4_gesture_passthrough_method(action, cmd);
	char target[200];
	int rc;

	if (!d->have_passthrough || d->passthrough.forward == NULL) {
		gesture_result(d, M4_LOG_INFO, M4_T("passthrough %s -- forwarding unavailable in this process",
"passthrough %s -- пересылка недоступна в этом процессе"), method);
		return;
	}
	d->gstat.passthroughs++;
	rc = d->passthrough.forward(d->passthrough.ctx, method, target, sizeof(target));
	if (rc == 0)
		gesture_result(d, M4_LOG_INFO, "passthrough %s -> %s", method, target);
	else if (rc == 1)
		gesture_result(d, M4_LOG_INFO, M4_T("passthrough %s -- no MPRIS player in the session, doing nothing",
"passthrough %s -- в сессии нет MPRIS-плеера, ничего не делаю"),
			       method);
	else
		gesture_result(d, M4_LOG_WARN, M4_T("passthrough %s failed: %s",
"passthrough %s не удался: %s"), method, target);
}

void m4_daemon_attach_player(struct m4_daemon *d, struct m4_mpris_player *player)
{
	d->player = player;
	if (player != NULL)
		m4_mpris_player_set_status(player, d->player_status);
}

void m4_daemon_player_event(struct m4_daemon *d, const struct m4_player_event *ev)
{
	m4_gesture_action_t action;
	m4_player_status_policy_t new_status;
	char when[32];

	if ((unsigned)ev->cmd >= M4_PLAYER_CMD_COUNT)
		return;
	action = d->settings.gesture[ev->cmd];
	format_wallclock(ev->realtime_usec, when, sizeof(when));

	d->gstat.events++;
	d->gstat.have_last = true;
	d->gstat.last_cmd = ev->cmd;
	d->gstat.last_action = action;
	d->gstat.last_result[0] = '\0';

	m4_log(M4_LOG_INFO, M4_T("gesture: command %s from the headset in %s (from %s) -> action %s",
"жест: команда %s от наушников в %s (от %s) -> действие %s"),
	       m4_player_cmd_method(ev->cmd), when, ev->sender[0] ? ev->sender : "?",
	       m4_gesture_action_name(action));

	new_status = m4_player_status_after(d->settings.player_status, d->player_status, ev->cmd);
	if (new_status != d->player_status) {
		m4_log(M4_LOG_INFO, M4_T("gesture: player status for the M4 %s -> %s",
"жест: статус плеера для M4 %s -> %s"),
		       m4_player_status_mpris(d->player_status), m4_player_status_mpris(new_status));
		d->player_status = new_status;
		m4_mpris_player_set_status(d->player, new_status);
	}
	/* player status after reconnect: M4 has just
	 * flipped its own play/pause belief; with a FIXED status nothing we
	 * report changed, so it would never hear back from us -- and M4 sends
	 * swipes (Next/Previous) only while it believes "playing". */
	if (m4_player_resync_after_cmd(d->settings.player_resync, d->settings.player_status, ev->cmd)) {
		char why[64];

		snprintf(why, sizeof(why), M4_T("after %s from the M4",
"после %s от M4"), m4_player_cmd_method(ev->cmd));
		m4_mpris_player_resync(d->player, why);
	}

	switch (action) {
	case M4_GESTURE_ACTION_CALL_TOGGLE:
	case M4_GESTURE_ACTION_CALL_ON:
	case M4_GESTURE_ACTION_CALL_OFF:
		daemon_gesture_call(d, action, ev->cmd, ev->monotonic_s);
		break;
	case M4_GESTURE_ACTION_MIC_TOGGLE: {
		char origin[48];

		snprintf(origin, sizeof(origin), M4_T("gesture %s (mic)",
"жест %s (mic)"), m4_player_cmd_method(ev->cmd));
		daemon_mic_toggle_gated(d, origin, ev->monotonic_s);
		break;
	}
	case M4_GESTURE_ACTION_PASSTHROUGH:
	case M4_GESTURE_ACTION_PASSTHROUGH_EXACT:
		daemon_gesture_passthrough(d, action, ev->cmd);
		break;
	case M4_GESTURE_ACTION_NONE:
	case M4_GESTURE_ACTION_COUNT:
	default:
		snprintf(d->gstat.last_result, sizeof(d->gstat.last_result), M4_T("no action",
"нет действия"));
		break;
	}
}

void m4_daemon_get_gesture_status(struct m4_daemon *d, struct m4_daemon_gesture_status *out)
{
	*out = d->gstat;
	out->status_policy = d->settings.player_status;
	out->player_status = d->player_status;
}

/* ------------------------------------------------------------------ */
/* HFP call-control events (patch 0006) */

static void hfp_result(struct m4_daemon *d, enum m4_log_level lvl, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));

static void hfp_result(struct m4_daemon *d, enum m4_log_level lvl, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(d->hstat.last_result, sizeof(d->hstat.last_result), fmt, ap);
	va_end(ap);
	m4_log(lvl, "hfp: %s", d->hstat.last_result);
}

/* Same 4 write-gating checks and the same cooldown state
 * (d->last_call_action_end/d->settings.gesture_cooldown_ms) as
 * daemon_gesture_call() above -- (the same cooldown as
 * gestures). Deliberately shares the SAME timer/state with the gesture path
 * rather than keeping a separate one, so an HFP AT event arriving right
 * after a gesture- or ctl-triggered switch (or vice versa) is debounced
 * against it too, not just against other HFP events. `action` is never
 * NONE/PASSTHROUGH(_EXACT)/COUNT here -- m4_daemon_hfp_event() below only
 * calls this for CALL_TOGGLE/CALL_ON/CALL_OFF. */
static void daemon_hfp_call(struct m4_daemon *d, m4_gesture_action_t action, m4_hfp_verb_t verb, double now)
{
	double cooldown_s = (double)d->settings.gesture_cooldown_ms / 1000.0;
	double since = now - d->last_call_action_end;
	m4_callfsm_result_t r;
	char origin[48];
	double elapsed_s;
	bool on;

	if (d->last_call_action_end >= 0 && since >= 0 && since < cooldown_s) {
		d->hstat.ignored++;
		hfp_result(d, M4_LOG_INFO,
			   M4_T("%s (%s) ignored -- cooldown after the last switch (%.2f s < %.2f s)",
"%s (%s) проигнорирован -- пауза после прошлого переключения (%.2f с < %.2f с)"),
			   m4_gesture_action_name(action), m4_hfp_verb_name(verb), since, cooldown_s);
		return;
	}
	if (m4_state_safe_latched(d->runtime_dir)) {
		d->hstat.ignored++;
		hfp_result(d, M4_LOG_WARN, M4_T("%s (%s) NOT executed -- safe-latch (momentum4 resume)",
"%s (%s) НЕ выполнен -- safe-latch (momentum4 resume)"),
			   m4_gesture_action_name(action), m4_hfp_verb_name(verb));
		daemon_play_earcon_error(d); /* call-error on safe-latch */
		return;
	}
	if (!daemon_allow_manual_write(d)) {
		d->hstat.ignored++;
		hfp_result(d, M4_LOG_INFO, M4_T("%s (%s) NOT executed -- mode %s (need manual or auto)",
"%s (%s) НЕ выполнен -- режим %s (нужен manual или auto)"),
			   m4_gesture_action_name(action), m4_hfp_verb_name(verb), m4_daemon_mode_name(d->mode));
		daemon_play_earcon_error(d); /* call-error in observe mode */
		return;
	}
	if (d->action_in_progress) {
		d->hstat.ignored++;
		hfp_result(d, M4_LOG_WARN, M4_T("%s (%s) NOT executed -- another action is already in progress",
"%s (%s) НЕ выполнен -- другое действие уже выполняется"),
			   m4_gesture_action_name(action), m4_hfp_verb_name(verb));
		return;
	}

	if (action == M4_GESTURE_ACTION_CALL_ON) {
		on = true;
	} else if (action == M4_GESTURE_ACTION_CALL_OFF) {
		on = false;
	} else {
		m4_callfsm_status_t st = m4_callfsm_status(&d->callfsm_ops);

		on = !st.call_active;
	}

	snprintf(origin, sizeof(origin), "HFP %s", m4_hfp_verb_name(verb));
	d->hstat.call_actions++;
	r = daemon_execute_call(d, on, origin, &elapsed_s);
	/* Same reasoning as daemon_gesture_call(): measured from the END of
	 * the (blocking) switch, so events queued while we were busy do not
	 * toggle straight back. */
	d->last_call_action_end = now + elapsed_s;
	hfp_result(d, r.rc == M4_CALLFSM_RC_OK ? M4_LOG_INFO : M4_LOG_WARN, M4_T("call %s -> %s (%.3f s)",
"call %s -> %s (%.3f с)"),
		   on ? "on" : "off", r.rc == M4_CALLFSM_RC_OK ? "ok" : r.message, elapsed_s);
}

/* patch 0008: M4 closed the HFP audio link (double tap
 * in HFP = QCC "transfer audio to AG"); our libspa-bluez5 reconnects it in
 * place and reports SCODROP, then SCOUP or SCOFAIL. The daemon only adds the
 * hfp-sco-drop policy on top (control/gesture.h's m4_sco_drop_decide()):
 * earcon on restore, call-off (A2DP) as the fallback so sound never stays
 * dead, or the double tap as a call-off / mic-toggle gesture. Call-off goes
 * through daemon_hfp_call(), i.e. the same observe/safe-latch/cooldown gates
 * as AT+CHUP. */
static void daemon_sco_event(struct m4_daemon *d, const struct m4_hfp_event *ev)
{
	m4_sco_event_t sev;
	m4_sco_step_t step;
	char origin[48];

	if (d->mac_address[0] != '\0' && ev->bdaddr[0] != '\0' &&
	    strcasecmp(ev->bdaddr, d->mac_address) != 0) {
		hfp_result(d, M4_LOG_DEBUG, M4_T("%s from %s -- not the M4, ignoring",
"%s от %s -- не M4, игнорирую"), m4_hfp_verb_name(ev->verb), ev->bdaddr);
		return;
	}

	switch (ev->verb) {
	case M4_HFP_VERB_SCODROP: sev = M4_SCO_EVENT_DROP; break;
	case M4_HFP_VERB_SCOUP:   sev = M4_SCO_EVENT_UP; break;
	case M4_HFP_VERB_SCOFAIL: sev = M4_SCO_EVENT_FAIL; break;
	default: return;
	}
	step = m4_sco_drop_decide(d->settings.hfp_sco_drop, sev);
	m4_log(sev == M4_SCO_EVENT_UP ? M4_LOG_INFO : M4_LOG_WARN,
	       M4_T("hfp: HFP audio channel: %s from %s (patch 0008) -> hfp-sco-drop=%s: %s",
"hfp: звуковой канал HFP: %s от %s (patch 0008) -> hfp-sco-drop=%s: %s"),
	       m4_hfp_verb_name(ev->verb), ev->bdaddr[0] != '\0' ? ev->bdaddr : "?",
	       m4_sco_drop_policy_name(d->settings.hfp_sco_drop), m4_sco_step_name(step));

	switch (step) {
	case M4_SCO_STEP_EARCON_RESTORED:
		/* link already up and both nodes restarted their I/O; a short
		 * settle so the tone is not cut by the first packets */
		daemon_schedule_earcon(d, M4_EARCON_CALL_ON, d->earcon_delay_sco_up_ms);
		hfp_result(d, M4_LOG_INFO, M4_T("call audio restored (SCO reconnected)",
"звук звонка восстановлен (SCO переподключён)"));
		break;
	case M4_SCO_STEP_MIC_TOGGLE:
		snprintf(origin, sizeof(origin), "HFP %s (mic)", m4_hfp_verb_name(ev->verb));
		daemon_mic_toggle_gated(d, origin, ev->monotonic_s);
		break;
	case M4_SCO_STEP_CALL_OFF:
	case M4_SCO_STEP_CALL_OFF_FALLBACK:
		/* No separate error tone for the fallback: the SCO link is down,
		 * it would only reach the other combine member; the call-off
		 * earcon (after the switch, earcon-delay-off-ms) says "AALL". */
		daemon_hfp_call(d, M4_GESTURE_ACTION_CALL_OFF, ev->verb, ev->monotonic_s);
		break;
	case M4_SCO_STEP_NOTHING:
	default:
		snprintf(d->hstat.last_result, sizeof(d->hstat.last_result), M4_T("%s: no action",
"%s: нет действия"),
			 m4_hfp_verb_name(ev->verb));
		break;
	}
}

void m4_daemon_hfp_event(struct m4_daemon *d, const struct m4_hfp_event *ev)
{
	m4_gesture_action_t action;

	if ((unsigned)ev->verb >= (unsigned)M4_HFP_VERB_COUNT)
		return;

	d->hstat.events++;
	d->hstat.have_last = true;
	d->hstat.last_verb = ev->verb;
	d->hstat.last_result[0] = '\0';

	/* patch 0008, not AT */
	if (m4_hfp_verb_is_sco(ev->verb)) {
		daemon_sco_event(d, ev);
		return;
	}

	/* patch 0007 ("tap = mic"): diagnostic-
	 * only -- logged at <5ms latency (this event arrived via
	 * hfp-events.sock, not the ~300ms journal-scrape path) so a live tap
	 * test shows exactly what AT command (if any) M4 sent, WITHOUT
	 * mapping to any hfp-*=... action (we do not know this command's
	 * semantics -- see m4_hfp_events_notify_raw()'s own doc comment). Not
	 * counted in hstat.call_actions/ignored (neither applies -- nothing
	 * was gated or executed), but IS counted in hstat.events above and
	 * hstat.raw_events below, and updates hstat.last_raw_text for
	 * `momentum4 status`-style inspection. */
	if (ev->verb == M4_HFP_VERB_RAW) {
		d->hstat.raw_events++;
		snprintf(d->hstat.last_raw_text, sizeof(d->hstat.last_raw_text), "%s", ev->raw_text);
		hfp_result(d, M4_LOG_INFO, M4_T("raw AT command \"%s\" from %s (patch 0007, diagnostics, "
			   "not mapped to any action)",
"сырая AT-команда \"%s\" от %s (patch 0007, диагностика, "
			   "не сопоставлена ни с каким действием)"),
			   ev->raw_text, ev->bdaddr[0] != '\0' ? ev->bdaddr : "?");
		return;
	}

	switch (ev->verb) {
	case M4_HFP_VERB_CHUP: action = d->settings.hfp_chup; break;
	case M4_HFP_VERB_ATA:  action = d->settings.hfp_ata;  break;
	case M4_HFP_VERB_BLDN: action = d->settings.hfp_bldn; break;
	case M4_HFP_VERB_BVRA: action = d->settings.hfp_bvra; break;
	default:               action = M4_GESTURE_ACTION_NONE; break;
	}
	d->hstat.last_action = action;

	m4_log(M4_LOG_INFO, M4_T("hfp: event %s from %s (AT without a modem, patch 0006) -> action %s",
"hfp: событие %s от %s (AT без модема, patch 0006) -> действие %s"),
	       m4_hfp_verb_name(ev->verb), ev->bdaddr[0] != '\0' ? ev->bdaddr : "?",
	       m4_gesture_action_name(action));

	switch (action) {
	case M4_GESTURE_ACTION_CALL_TOGGLE:
	case M4_GESTURE_ACTION_CALL_ON:
	case M4_GESTURE_ACTION_CALL_OFF:
		daemon_hfp_call(d, action, ev->verb, ev->monotonic_s);
		break;
	case M4_GESTURE_ACTION_MIC_TOGGLE: {
		char origin[48];

		snprintf(origin, sizeof(origin), "HFP %s (mic)", m4_hfp_verb_name(ev->verb));
		daemon_mic_toggle_gated(d, origin, ev->monotonic_s);
		break;
	}
	case M4_GESTURE_ACTION_PASSTHROUGH:
	case M4_GESTURE_ACTION_PASSTHROUGH_EXACT:
		/* No session-MPRIS analog for a telephony AT command -- treated
		 * as none, but logged so a config mistake is visible rather
		 * than silently doing nothing. */
		d->hstat.ignored++;
		hfp_result(d, M4_LOG_WARN, M4_T("%s is not supported for hfp-* -- action not executed",
"%s не поддерживается для hfp-* -- действие не выполнено"),
			   m4_gesture_action_name(action));
		break;
	case M4_GESTURE_ACTION_NONE:
	case M4_GESTURE_ACTION_COUNT:
	default:
		snprintf(d->hstat.last_result, sizeof(d->hstat.last_result), M4_T("no action",
"нет действия"));
		break;
	}
}

void m4_daemon_get_hfp_status(struct m4_daemon *d, struct m4_daemon_hfp_status *out)
{
	*out = d->hstat;
}

static void on_hfp_event(void *user_data, const struct m4_hfp_event *ev)
{
	m4_daemon_hfp_event((struct m4_daemon *)user_data, ev);
}

/* ------------------------------------------------------------------ */
/* mic mute toggle ("tap = mic" task
 * follow-up): mutes/unmutes the M4 mic Route via graph_ops (the same
 * SPA_PARAM_Route object the earlier Bug 2 volume fix already targets,
 * core/graph_ops.h's set_mic_volume() -- confirmed by reading
 * spa/plugins/bluez5/bluez5-device.c's node_set_mute()/apply_device_props():
 * this is a purely local PipeWire Props event, no AT/RFCOMM round trip to
 * M4 at all, unlike the mic GAIN Bug 2 already has to fight (AT+VGM, a
 * SEPARATE prop) -- so it is as fast as a single async
 * pw_device_set_param() can be, matching the "no delay" requirement).
 * mic_result()/daemon_check_mic_route_transition()/daemon_arm_mic_route_watch()
 * are already forward-declared above on_graph_state_changed(), which needs
 * the latter two before either is defined -- see that block's own comment. */

static void mic_result(struct m4_daemon *d, enum m4_log_level lvl, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(d->mstat.last_result, sizeof(d->mstat.last_result), fmt, ap);
	va_end(ap);
	m4_log(lvl, "mic: %s", d->mstat.last_result);
}

/* "M4 button = mute (VGM)": seeds d->mic_route_watch_off from
 * the CURRENT d->graph_state (the caller's job to ensure it is fresh --
 * daemon_execute_call()'s on-success path calls this right after
 * m4_callfsm_on() has fully returned, i.e. after fix_mic_volume_if_needed()'s
 * own confirm-poll has already settled d->graph_state; m4_daemon_tick()'s
 * safety net calls it once daemon_mic_route_usable() is observed true with
 * no confirm poll in flight at all, so d->graph_state is trivially current
 * either way)
 * WITHOUT announcing anything -- this is a baseline, not a transition. Safe
 * to call repeatedly/idempotently (e.g. tick() calling it again next tick
 * would be a no-op in practice since `armed` is already true by then). */
static void daemon_arm_mic_route_watch(struct m4_daemon *d)
{
	d->mic_route_watch_armed = true;
	d->mic_route_watch_off = d->graph_state.have_mic_route &&
				  d->graph_state.mic_volume <= d->mic_mute_threshold;
}

/* "M4 button = mute (VGM)" (docstring: "Debounce: M4 sends VGM
 * twice (00, 00) within ~100ms -- the earcon must play only once"): called
 * from on_graph_state_changed() on EVERY graph update while a call is
 * established. Debounce here is purely VALUE-based, not a timer -- two
 * identical AT+VGM=00 messages 100ms apart both settle `state->mic_volume`
 * to the exact same 0.0f, so the `now_off == d->mic_route_watch_off` check
 * below suppresses the second one on its own (this is also reinforced one
 * layer down: pw/graph_pw.c's graph_on_core_done() does not even invoke
 * on_state_changed() at all unless the COMMITTED state actually differs from
 * before -- see its own comment -- so in the real graph this function is
 * often not even called a second time for a genuine duplicate). No separate
 * cooldown/timer is needed or wanted: item 2 wants the user to hear the
 * result immediately, and a real second button press within 100ms of the first
 * (a fast double-tap of the physical mute button, as opposed to M4 merely
 * repeating the same AT+VGM) is exactly a case where BOTH transitions are
 * real and should both be heard -- a time-based debounce would incorrectly
 * eat the second one too.
 *
 * Volume-only (d->mic_mute_threshold's own comment): `state->mic_mute` is
 * PipeWire's own separate SPA_PROP_mute/softMute flag -- confirmed by reading
 * spa/plugins/bluez5/bluez5-device.c's node_set_mute(), it only ever changes
 * in response to OUR OWN prior set_mic_volume(..., mute=X, ...) writes (this
 * daemon's or some other PipeWire client's, e.g. `pactl set-source-mute`) and
 * is NEVER touched by an incoming AT+VGM from the M4 itself (node_set_mute()
 * does not call spa_bt_transport_set_volume() at all, unlike node_set_volume()
 * -- so a stale `mic_mute=true` left over from an earlier explicit `momentum4
 * mic off` would otherwise permanently mask a later, real, hardware-level
 * unmute via the physical button, which only ever moves the volume). Using
 * volume alone matches both the requirement's own literal wording ("Route volume:
 * 0 -> mic-off, >0 -> mic-on") and physical reality. */
static void daemon_check_mic_route_transition(struct m4_daemon *d, const struct m4_graph_state *state)
{
	bool now_off;

	if (!daemon_mic_route_usable(state)) {
		/* item 5: mute state is not carried over -- call over (or never
		 * started -- daemon_mic_route_usable()'s own comment: raw
		 * have_mic_route alone would stay true even in AALL on this
		 * device) means there is nothing left to watch; the next
		 * call-on re-arms from scratch. */
		d->mic_route_watch_armed = false;
		return;
	}
	if (!d->mic_route_watch_armed)
		return; /* no baseline yet -- see daemon_arm_mic_route_watch()'s
			 * own comment for exactly when one gets seeded. */

	now_off = state->mic_volume <= d->mic_mute_threshold;
	if (now_off == d->mic_route_watch_off)
		return;

	d->mic_route_watch_off = now_off;
	mic_result(d, M4_LOG_INFO, M4_T("M4: microphone %s (Route observation, button/AT+VGM)",
"M4: микрофон %s (наблюдение Route, кнопка/AT+VGM)"),
		   now_off ? M4_T("off",
"выключен") : M4_T("on",
"включён"));
	daemon_schedule_earcon(d, now_off ? M4_EARCON_MIC_OFF : M4_EARCON_MIC_ON, 0 /* item 2: no delay */);
}

/* The one place a mic on/off/toggle is actually executed -- callers
 * (m4_daemon_mic_set() for ctl.sock, daemon_mic_toggle_gated() for gesture/
 * HFP below) have already applied their own write gate and
 * action_in_progress check, exactly mirroring daemon_execute_call()'s own
 * split between itself and handle_call()/daemon_gesture_call()/
 * daemon_hfp_call(). Plays the mic-on/off earcon on success, call-error on
 * failure -- same convention as daemon_execute_call(). Returns true/false;
 * never touches d->action_in_progress or any cooldown state itself (callers
 * own both, since ctl.sock's MIC command and the gesture/HFP path use
 * DIFFERENT cooldown rules, see m4_daemon_mic_set()'s and
 * daemon_mic_toggle_gated()'s own comments). */
static bool daemon_execute_mic(struct m4_daemon *d, bool want_mute, const char *origin)
{
	float volume;
	enum m4_graph_status rc;

	d->mstat.actions++;

	if (d->graph_handle == NULL || !d->graph_state.m4_present) {
		mic_result(d, M4_LOG_WARN, M4_T("%s -- the M4 is unavailable to the daemon",
"%s -- M4 недоступны демону"), origin);
		daemon_play_earcon_error(d);
		return false;
	}
	if (!daemon_mic_route_usable(&d->graph_state)) {
		/* daemon_mic_route_usable(), not raw have_mic_route: on this
		 * device the Route exists even outside a call (see that
		 * function's own comment) -- without this, `momentum4 mic off`
		 * would have silently muted the physical mic gain while just
		 * listening to music in AALL, nowhere near a real call. */
		mic_result(d, M4_LOG_WARN, M4_T("%s -- no microphone route (not in a call?)",
"%s -- нет маршрута микрофона (не в звонке?)"), origin);
		daemon_play_earcon_error(d);
		return false;
	}

	/* "M4 button = mute (VGM)": `momentum4 mic toggle|on|off`
	 * sets Route volume to 0 or 100 -- i.e. the same +VGM as on M4. Was:
	 * leave `volume` UNCHANGED and only flip the soft `mute` flag --
	 * confirmed by reading bluez5-device.c's node_set_mute() (see
	 * d->mic_mute_threshold's/daemon_check_mic_route_transition()'s own
	 * comments) that this NEVER reached the transport at all: a `momentum4
	 * mic off` under the old code set only a local PipeWire flag that has
	 * no effect whatsoever on the M4's actual microphone gain -- BlueZ
	 * never sent AT+VGM=0, the physical mic kept capturing audio
	 * regardless of what `momentum4 mic status` reported. Writing volume
	 * 0.0f/1.0f is the SAME wire effect the physical mute button has
	 * (rfcomm_send_reply(rfcomm, "+VGM: 0"/"+VGM: 15") from
	 * spa_bt_transport_set_volume(), node_set_volume()'s own path) --
	 * which is exactly why daemon_check_mic_route_transition() (the single
	 * place that now plays the mic-on/off earcon, see below) cannot tell
	 * this write apart from a real button press, and does not need to. The
	 * `mute` argument is still passed along in lockstep (harmless,
	 * existing tests/status still read it) but is no longer load-bearing
	 * for anything real. */
	volume = want_mute ? 0.0f : 1.0f;
	rc = d->graph_ops->set_mic_volume(d->graph_handle, volume, want_mute, true);
	if (rc != M4_GRAPH_OK) {
		mic_result(d, M4_LOG_WARN, M4_T("%s -- set_mic_volume failed (%d)",
"%s -- set_mic_volume не удался (%d)"), origin, (int)rc);
		daemon_play_earcon_error(d);
		return false;
	}

	/* Bounded confirm, same pattern pw/callfsm_graph_adapter.c's
	 * adapter_fix_mic_volume_if_needed() already uses
	 * -- best-effort: a graph_ops with no iterate() (most test fixtures)
	 * just skips straight to "fire-and-forget succeeded", the write above
	 * is still correct, only the wait-for-observed-effect part is
	 * skipped. Volume-only condition, matching the write above. */
	if (d->graph_ops->iterate != NULL) {
		double deadline = monotonic_now() + 0.3;

		while (monotonic_now() < deadline) {
			struct m4_graph_state st;
			bool reached;

			if (d->graph_ops->get_state(d->graph_handle, &st) == M4_GRAPH_OK && st.have_mic_route) {
				reached = want_mute ? st.mic_volume <= d->mic_mute_threshold
						    : st.mic_volume >= 1.0f - d->mic_mute_threshold;
				if (reached)
					break;
			}
			if (d->graph_ops->iterate(d->graph_handle, 20) < 0)
				break;
		}
	}

	mic_result(d, M4_LOG_INFO, "%s -> %s", origin, want_mute ? M4_T("off",
"выключен") : M4_T("on",
"включён"));
	/* the earcon plays from the observation path (item 2), not twice --
	 * NO earcon call here anymore. The set_mic_volume() write above flows
	 * through on_graph_state_changed() -> daemon_check_mic_route_transition()
	 * exactly like a physical button press does (see that function's own
	 * comment on why it cannot and need not tell the two apart), which
	 * plays the mic-on/off earcon exactly once, whichever of the two
	 * caused it. */
	return true;
}

static void daemon_mic_fill_status(struct m4_daemon *d, struct m4_daemon_mic_status *out)
{
	struct m4_graph_state live;
	const struct m4_graph_state *st = &d->graph_state;

	*out = d->mstat;
	/* "M4 button = mute (VGM)": real state, not a
	 * cache -- read straight from graph_ops's own get_state() rather than
	 * d->graph_state, which is a SECOND copy of the same data, refreshed
	 * only when on_graph_state_changed() happens to have already run for
	 * this event-loop turn (graph_ops.h's own get_state() doc comment:
	 * "no round-trip to the server -- always instant" -- it is exactly as
	 * cheap as reading d->graph_state, so there is no cost to preferring
	 * it). Falls back to the cached copy if there is currently no open
	 * graph handle at all (same as every other d->graph_state read in this
	 * file in that case -- get_state() itself would just report
	 * m4_present=false anyway, but avoid a NULL-handle call into graph_ops
	 * entirely here). */
	if (d->graph_handle != NULL && d->graph_ops->get_state(d->graph_handle, &live) == M4_GRAPH_OK)
		st = &live;

	/* daemon_mic_route_usable(), not raw st->have_mic_route: that field
	 * alone stays true even outside a call on this device (its own
	 * comment) -- `momentum4 mic status` outside a call must still say
	 * "no route (not in a call)", matching the CLI's/every gate's own
	 * "have_route == in a call" contract (mic.h/callfsm_ops.h's own doc
	 * comments), not the raw SPA fact that the Route object exists. */
	out->have_route = daemon_mic_route_usable(st);
	/* Volume-only, like everywhere else in this file now
	 * (d->mic_mute_threshold's own comment) -- NOT st->mic_mute (the soft
	 * PipeWire-only flag that never reflects the M4's own physical mute
	 * button, and could otherwise show a stale "muted" long after the
	 * volume -- the only thing the M4 itself actually reports -- says
	 * otherwise). */
	out->mute = out->have_route && st->mic_volume <= d->mic_mute_threshold;
	out->volume = out->have_route ? st->mic_volume : 0.0f;
}

/* Gated entry for gesture/HFP-triggered M4_GESTURE_ACTION_MIC_TOGGLE -- own
 * cooldown timer (d->last_mic_action_end, NOT d->last_call_action_end, see
 * struct m4_daemon's field comment above), reusing the SAME
 * gesture-cooldown-ms value (a 300ms cooldown was accepted).
 * `origin` only goes into the log line, same convention as
 * daemon_gesture_call()/daemon_hfp_call(). */
static void daemon_mic_toggle_gated(struct m4_daemon *d, const char *origin, double now)
{
	double cooldown_s = (double)d->settings.gesture_cooldown_ms / 1000.0;
	double since = now - d->last_mic_action_end;
	bool want_mute;

	if (d->last_mic_action_end >= 0 && since >= 0 && since < cooldown_s) {
		d->mstat.ignored++;
		mic_result(d, M4_LOG_INFO, M4_T("%s ignored -- cooldown after the last switch (%.2f s < %.2f s)",
"%s проигнорирован -- пауза после прошлого переключения (%.2f с < %.2f с)"),
			   origin, since, cooldown_s);
		return;
	}
	if (m4_state_safe_latched(d->runtime_dir)) {
		d->mstat.ignored++;
		mic_result(d, M4_LOG_WARN, M4_T("%s NOT executed -- safe-latch (momentum4 resume)",
"%s НЕ выполнен -- safe-latch (momentum4 resume)"), origin);
		daemon_play_earcon_error(d);
		return;
	}
	if (!daemon_allow_manual_write(d)) {
		d->mstat.ignored++;
		mic_result(d, M4_LOG_INFO, M4_T("%s NOT executed -- mode %s (need manual or auto)",
"%s НЕ выполнен -- режим %s (нужен manual или auto)"), origin,
			   m4_daemon_mode_name(d->mode));
		daemon_play_earcon_error(d);
		return;
	}
	if (d->action_in_progress) {
		d->mstat.ignored++;
		mic_result(d, M4_LOG_WARN, M4_T("%s NOT executed -- another action is already in progress",
"%s НЕ выполнен -- другое действие уже выполняется"), origin);
		return;
	}

	want_mute = !(daemon_mic_route_usable(&d->graph_state) &&
			      d->graph_state.mic_volume <= d->mic_mute_threshold); /* volume-only + in-call,
			      "currently effectively off" -> toggle unmutes. daemon_mic_route_usable(),
			      not raw have_mic_route (its own comment) -- harmless if this ever
			      disagrees, since daemon_execute_mic() re-checks have_route/usable()
			      itself and rejects outside a call either way, but this keeps the
			      TOGGLE decision itself meaningful too, not just the write gate. */

	d->action_in_progress = true;
	daemon_execute_mic(d, want_mute, origin);
	d->action_in_progress = false;
	d->last_mic_action_end = now;
}

/* ctl.sock `MIC ON`/`MIC OFF`/`MIC TOGGLE` backend (cli/mic.c), and the
 * public entry point tests use directly (see daemon.h). Deliberately the
 * SAME gate set as handle_call() (safe-latch/observe/action_in_progress)
 * and NOT the gesture-cooldown timer above -- matching handle_call() itself
 * not applying gesture-cooldown-ms to ctl.sock `call on/off` either (that
 * cooldown exists specifically to debounce a *physical* gesture/AT
 * stream, not an explicit one-shot CLI command). */
bool m4_daemon_mic_set(struct m4_daemon *d, bool want_mute, bool toggle, const char *origin,
			struct m4_daemon_mic_status *out)
{
	bool ok;

	if (m4_state_safe_latched(d->runtime_dir)) {
		d->mstat.ignored++;
		mic_result(d, M4_LOG_WARN, M4_T("%s NOT executed -- safe-latch (momentum4 resume)",
"%s НЕ выполнен -- safe-latch (momentum4 resume)"), origin);
		if (out != NULL)
			daemon_mic_fill_status(d, out);
		return false;
	}
	if (!daemon_allow_manual_write(d)) {
		d->mstat.ignored++;
		mic_result(d, M4_LOG_INFO, M4_T("%s NOT executed -- mode %s (need manual or auto)",
"%s НЕ выполнен -- режим %s (нужен manual или auto)"), origin,
			   m4_daemon_mode_name(d->mode));
		if (out != NULL)
			daemon_mic_fill_status(d, out);
		return false;
	}
	if (d->action_in_progress) {
		d->mstat.ignored++;
		mic_result(d, M4_LOG_WARN, M4_T("%s NOT executed -- another action is already in progress",
"%s НЕ выполнен -- другое действие уже выполняется"), origin);
		if (out != NULL)
			daemon_mic_fill_status(d, out);
		return false;
	}

	if (toggle)
		want_mute = !(daemon_mic_route_usable(&d->graph_state) &&
			      d->graph_state.mic_volume <= d->mic_mute_threshold); /* volume-only + in-call,
			      "currently effectively off" -> toggle unmutes. daemon_mic_route_usable(),
			      not raw have_mic_route (its own comment) -- harmless if this ever
			      disagrees, since daemon_execute_mic() re-checks have_route/usable()
			      itself and rejects outside a call either way, but this keeps the
			      TOGGLE decision itself meaningful too, not just the write gate. */

	d->action_in_progress = true;
	ok = daemon_execute_mic(d, want_mute, origin);
	d->action_in_progress = false;

	if (out != NULL)
		daemon_mic_fill_status(d, out);
	return ok;
}

void m4_daemon_get_mic_status(struct m4_daemon *d, struct m4_daemon_mic_status *out)
{
	daemon_mic_fill_status(d, out);
}

static void handle_player(struct m4_daemon *d, const char *arg, char *response, size_t cap)
{
	int n = 0;

	if (arg != NULL) {
		m4_player_status_policy_t p;

		while (*arg == ' ')
			arg++;
		if (strcasecmp(arg, "RESYNC") == 0) {
			/* explicit: works even with player-resync = off */
			if (d->player == NULL) {
				snprintf(response, cap, M4_T("ERR player is not running (player = off, or no system bus)\n",
"ERR плеер не запущен (player = off или нет системной шины)\n"));
				return;
			}
			m4_mpris_player_resync(d->player, "momentum4 player resync");
			m4_log(M4_LOG_INFO, M4_T("gesture: player status resend requested for the M4 (%s)",
"жест: запрошена переотправка статуса плеера для M4 (%s)"),
			       m4_player_status_mpris(d->player_status));
		} else if (strncasecmp(arg, "STATUS ", 7) != 0 || m4_player_status_policy_parse(arg + 7, &p) < 0) {
			snprintf(response, cap,
				 M4_T("ERR expected: PLAYER STATUS stopped|playing|paused|follow | PLAYER RESYNC\n",
"ERR ожидается: PLAYER STATUS stopped|playing|paused|follow | PLAYER RESYNC\n"));
			return;
		} else {
			d->settings.player_status = p;
			d->player_status = m4_player_status_initial(p);
			m4_mpris_player_set_status(d->player, d->player_status);
			m4_log(M4_LOG_INFO, M4_T("gesture: player-status = %s (until the daemon restarts), the M4 sees %s",
"жест: player-status = %s (до перезапуска демона), M4 видит %s"),
			       m4_player_status_policy_name(p), m4_player_status_mpris(d->player_status));
			/* same value as before -> BlueZ would not forward it */
			if (d->settings.player_resync != M4_PLAYER_RESYNC_OFF)
				m4_mpris_player_resync(d->player, "momentum4 player status");
		}
	}

#define APPEND(...)                                                                   \
	do {                                                                          \
		if ((size_t)n < cap)                                                  \
			n += snprintf(response + n, cap - (size_t)n, __VA_ARGS__);    \
	} while (0)

	APPEND("OK\n");
	APPEND("player_enabled=%s\n", d->settings.player_enabled ? "true" : "false");
	APPEND("player_attached=%s\n", d->player != NULL ? "true" : "false");
	APPEND("player_registered=%s\n", m4_mpris_player_is_registered(d->player) ? "true" : "false");
	APPEND("player_status_policy=%s\n", m4_player_status_policy_name(d->settings.player_status));
	APPEND("player_status=%s\n", m4_player_status_mpris(d->player_status));
	APPEND("player_resync=%s\n", m4_player_resync_name(d->settings.player_resync));
	APPEND("player_resync_count=%d\n", m4_mpris_player_resync_count(d->player));
	for (int i = 0; i < M4_PLAYER_CMD_COUNT; i++)
		APPEND("gesture-%s=%s\n", m4_player_cmd_key((m4_player_cmd_t)i),
		       m4_gesture_action_name(d->settings.gesture[i]));
	APPEND("gesture_cooldown_ms=%d\n", d->settings.gesture_cooldown_ms);
	APPEND("earcon=%s\n", d->settings.earcon ? "on" : "off");
	APPEND("events=%d\n", d->gstat.events);
	APPEND("last_command=%s\n", d->gstat.have_last ? m4_player_cmd_method(d->gstat.last_cmd) : "");
	APPEND("last_action=%s\n", d->gstat.have_last ? m4_gesture_action_name(d->gstat.last_action) : "");
	APPEND("last_result=%s\n", d->gstat.last_result);
#undef APPEND
}

static void handle_reload(struct m4_daemon *d, char *response, size_t cap)
{
	struct m4_settings fresh;
	int rc;

	/* [general] language (common/m4-i18n.h): m4_config() is the separate
	 * process-wide layered singleton (util/config.h) `d->settings` below
	 * does NOT go through -- force it to re-read every layer, then re-pick
	 * m4_lang from the fresh result, so a language change takes effect on
	 * `momentum4 reload` without a daemon restart, same guarantee as every
	 * other key documented in m4-i18n.h's own header comment. Ignored on
	 * every code path below that returns early (ERR): the language change
	 * itself has still happened, only the settings the rest of this
	 * function govern have not been re-read. */
	m4_config_reload();
	m4_i18n_init_from_conf(m4_config());

	if (d->config_path[0] == '\0') {
		snprintf(response, cap, M4_T("ERR path to momentum4.conf is unknown\n",
"ERR путь к momentum4.conf неизвестен\n"));
		return;
	}
	rc = m4_settings_load(&fresh, d->config_path);
	if (rc < 0) {
		snprintf(response, cap, M4_T("ERR %s could not be read -- settings unchanged\n",
"ERR %s не читается -- настройки не изменены\n"), d->config_path);
		return;
	}
	/* mode is NOT reloaded (runtime `momentum4 mode` wins until restart);
	 * player on/off needs a restart */
	fresh.mode = d->settings.mode;
	if (fresh.player_enabled != d->settings.player_enabled)
		m4_log(M4_LOG_WARN, M4_T("reload: player = %s takes effect after the daemon restarts",
"reload: player = %s вступит в силу после перезапуска демона"),
		       fresh.player_enabled ? "on" : "off");
	fresh.player_enabled = d->settings.player_enabled;
	/* mic-on-enter: unlike mode/player above, this one CAN take
	 * effect without a restart -- the adapter's own setter
	 * (m4_callfsm_graph_adapter_set_mic_on_enter(), callfsm_graph_adapter.h's
	 * own doc comment on why a setter exists at all) keeps it in sync with
	 * `d->settings.mic_on_enter` below. */
	if (fresh.mic_on_enter != d->settings.mic_on_enter)
		m4_callfsm_graph_adapter_set_mic_on_enter(d->adapter, fresh.mic_on_enter);
	d->settings = fresh;
	d->player_status = m4_player_status_initial(fresh.player_status);
	m4_mpris_player_set_status(d->player, d->player_status);
	m4_mpris_player_set_auto_resync(d->player, fresh.player_resync != M4_PLAYER_RESYNC_OFF);
	m4_log(M4_LOG_INFO, M4_T("reload: %s re-read (%s), %d warning(s)",
"reload: %s перечитан (%s), предупреждений %d"), d->config_path,
	       rc == 1 ? M4_T("no file -- default values",
"файла нет -- значения по умолчанию") : "ok", fresh.n_warnings);
	snprintf(response, cap, "OK\nconfig=%s\nwarnings=%d\nfirst_warning=%s\n", d->config_path,
		 fresh.n_warnings, fresh.first_warning);
}

/* ctl.sock `MIC ON`/`MIC OFF`/`MIC TOGGLE`/`MIC STATUS` (cli/mic.c). STATUS
 * (also the no-arg default, same convention as PLAYER above) is read-only,
 * no gating -- just m4_daemon_get_mic_status()'s current cached state. */
static void handle_mic(struct m4_daemon *d, const char *arg, char *response, size_t cap)
{
	struct m4_daemon_mic_status st;
	bool want_mute, toggle = false;
	bool ok;

	if (arg != NULL) {
		while (*arg == ' ')
			arg++;
	}
	if (arg == NULL || *arg == '\0' || strcasecmp(arg, "STATUS") == 0) {
		m4_daemon_get_mic_status(d, &st);
		snprintf(response, cap,
			 "OK\n"
			 "have_route=%s\n"
			 "muted=%s\n"
			 "volume=%.2f\n"
			 "actions=%d\n"
			 "ignored=%d\n",
			 st.have_route ? "true" : "false", st.have_route && st.mute ? "true" : "false",
			 (double)st.volume, st.actions, st.ignored);
		return;
	}
	if (strcasecmp(arg, "ON") == 0) {
		want_mute = false;
	} else if (strcasecmp(arg, "OFF") == 0) {
		want_mute = true;
	} else if (strcasecmp(arg, "TOGGLE") == 0) {
		want_mute = false; /* overridden by toggle=true inside m4_daemon_mic_set() */
		toggle = true;
	} else {
		snprintf(response, cap, M4_T("ERR expected: MIC on|off|toggle|status\n",
"ERR ожидается: MIC on|off|toggle|status\n"));
		return;
	}

	ok = m4_daemon_mic_set(d, want_mute, toggle, "MIC (ctl)", &st);
	if (ok)
		snprintf(response, cap, "OK\nmuted=%s\n", st.mute ? "true" : "false");
	else
		snprintf(response, cap, "ERR %s\n", d->mstat.last_result);
}

static void daemon_dispatch(void *user_data, const char *request, char *response, size_t response_cap)
{
	struct m4_daemon *d = user_data;

	if (strcmp(request, "STATUS") == 0)
		handle_status(d, response, response_cap);
	else if (strcmp(request, "GET") == 0)
		handle_get(d, response, response_cap);
	else if (strcmp(request, "CALL ON") == 0)
		handle_call(d, true, response, response_cap);
	else if (strcmp(request, "CALL OFF") == 0)
		handle_call(d, false, response, response_cap);
	else if (strncmp(request, "MODE ", 5) == 0)
		handle_mode(d, request + 5, response, response_cap);
	else if (strcmp(request, "REPAIR_A2DP") == 0)
		handle_repair_a2dp(d, response, response_cap);
	else if (strcmp(request, "PLAYER") == 0)
		handle_player(d, NULL, response, response_cap);
	else if (strncmp(request, "PLAYER ", 7) == 0)
		handle_player(d, request + 7, response, response_cap);
	else if (strcmp(request, "RELOAD") == 0)
		handle_reload(d, response, response_cap);
	else if (strcmp(request, "MIC") == 0)
		handle_mic(d, NULL, response, response_cap);
	else if (strncmp(request, "MIC ", 4) == 0)
		handle_mic(d, request + 4, response, response_cap);
	else
		snprintf(response, response_cap, M4_T("ERR unknown command: %s\n",
"ERR неизвестная команда: %s\n"), request);
}

void m4_daemon_handle_request(struct m4_daemon *d, const char *request, char *response,
			       size_t response_cap)
{
	daemon_dispatch(d, request, response, response_cap);
}

/* ------------------------------------------------------------------ */

void m4_daemon_get_status(struct m4_daemon *d, struct m4_daemon_status *out)
{
	m4_automode_status_t ast;

	memset(out, 0, sizeof(*out));
	m4_automode_status(d->automode, monotonic_now(), &ast);

	out->mode = d->mode;
	out->safe_latched = m4_state_safe_latched(d->runtime_dir);
	out->m4_present = d->graph_state.m4_present;
	out->m4_connected = d->graph_state.m4_connected;
	out->have_profile = d->graph_state.have_profile;
	snprintf(out->profile_name, sizeof(out->profile_name), "%s", d->graph_state.profile_name);
	out->frozen = ast.frozen;
	out->mic_active = ast.mic_active;
	out->call_active = ast.call_active;
	snprintf(out->call_reason, sizeof(out->call_reason), "%s", ast.call_active_reason);
	out->switches_in_window = ast.switches_in_window;
}

struct m4_daemon *m4_daemon_new(sd_event *event, const struct m4_daemon_config *cfg)
{
	struct m4_daemon *d = calloc(1, sizeof(*d));
	char reason[M4_CALLFSM_REASON_LEN];

	if (d == NULL)
		return NULL;

	d->event = event;
	d->graph_ops = cfg->graph_ops;
	snprintf(d->mac_address, sizeof(d->mac_address), "%s", cfg->mac_address ? cfg->mac_address : "");
	snprintf(d->runtime_dir, sizeof(d->runtime_dir), "%s", cfg->runtime_dir ? cfg->runtime_dir : "");
	snprintf(d->state_dir, sizeof(d->state_dir), "%s", cfg->state_dir ? cfg->state_dir : "");
	snprintf(d->stats_path, sizeof(d->stats_path), "%s", cfg->stats_path ? cfg->stats_path : "");
	d->confirm_timeout_ms = cfg->confirm_timeout_ms;
	d->tick_interval_ms = cfg->tick_interval_ms > 0 ? cfg->tick_interval_ms : M4_DAEMON_DEFAULT_TICK_INTERVAL_MS;
	d->graph_retry_s = cfg->graph_retry_s > 0 ? cfg->graph_retry_s : M4_DAEMON_DEFAULT_GRAPH_RETRY_SECONDS;
	d->mic_mute_threshold =
		cfg->mic_mute_threshold > 0 ? (float)cfg->mic_mute_threshold : M4_DAEMON_DEFAULT_MIC_ROUTE_QUIET_EPS;
	d->earcon_delay_sco_up_ms =
		cfg->earcon_delay_sco_up_ms > 0 ? cfg->earcon_delay_sco_up_ms : M4_DAEMON_DEFAULT_EARCON_DELAY_SCO_UP_MS;

	d->bt_ops = cfg->bt_ops;
	snprintf(d->bt_adapter, sizeof(d->bt_adapter), "%s", cfg->bt_adapter ? cfg->bt_adapter : "hci0");
	d->repair_connect_timeout_ms = cfg->repair_connect_timeout_ms;
	d->repair_enum_wait_ms = cfg->repair_enum_wait_ms;
	d->repair_a2dp_confirm_timeout_ms = cfg->repair_a2dp_confirm_timeout_ms;
	d->repair_a2dp_manual_only = cfg->repair_a2dp_manual_only;

	if (cfg->settings != NULL)
		d->settings = *cfg->settings;
	else
		m4_settings_defaults(&d->settings);
	snprintf(d->config_path, sizeof(d->config_path), "%s", cfg->config_path ? cfg->config_path : "");
	if (cfg->passthrough != NULL) {
		d->passthrough = *cfg->passthrough;
		d->have_passthrough = true;
	}
	d->earcon = cfg->earcon;
	d->earcon_ctx = cfg->earcon_ctx;
	d->player_status = m4_player_status_initial(d->settings.player_status);
	d->last_call_action_end = -1.0;
	d->last_mic_action_end = -1.0;

	if (cfg->has_initial_mode) {
		/* item 4: momentum4.conf `mode` (default manual)
		 * is authoritative at start -- survives restarts and reboots.
		 * Written to $runtime_dir/mode too so status/older tools agree. */
		d->mode = cfg->initial_mode;
		if (d->runtime_dir[0] != '\0' && m4_state_mode_write(d->runtime_dir, d->mode) < 0)
			m4_log(M4_LOG_WARN, M4_T("momentum4d: failed to write the mode to %s",
"momentum4d: не удалось записать режим в %s"), d->runtime_dir);
	} else {
		d->mode = m4_state_mode_read(d->runtime_dir);
	}

	daemon_try_open_graph(d, monotonic_now());
	/* daemon_try_open_graph() may have synchronously delivered an initial
	 * on_graph_state_changed() already (e.g. core/graph_ops_fake.c's
	 * open(), which calls notify() before returning -- tests/scenario_runner.c)
	 * -- seed present_seen from whatever graph_state ended up as, so the
	 * FIRST m4_daemon_tick() call never sees a spurious false->true edge. */
	d->present_seen = d->graph_state.m4_present;

	{
		struct m4_callfsm_graph_adapter_repair_config repair_cfg = {0};
		const struct m4_callfsm_graph_adapter_repair_config *repair_cfg_ptr = NULL;
		/* mic-on-enter: `d->settings.mic_on_enter` is already set
		 * (from `cfg->settings` above) by the time this block runs. */
		struct m4_callfsm_graph_adapter_mic_config mic_cfg = {0};

		if (d->bt_ops != NULL && !d->repair_a2dp_manual_only) {
			repair_cfg.bt_ops = d->bt_ops;
			repair_cfg.mac_address = d->mac_address;
			repair_cfg.bt_adapter = d->bt_adapter;
			repair_cfg.connect_timeout_ms = d->repair_connect_timeout_ms;
			repair_cfg.enum_wait_ms = d->repair_enum_wait_ms;
			repair_cfg_ptr = &repair_cfg;
		}
		mic_cfg.mode = d->settings.mic_on_enter;
		mic_cfg.state_dir = d->state_dir;
		mic_cfg.mute_threshold = d->mic_mute_threshold;
		d->adapter = m4_callfsm_graph_adapter_new(event, d->graph_ops, d->graph_handle,
							   d->runtime_dir, d->stats_path,
							   d->confirm_timeout_ms, repair_cfg_ptr, &mic_cfg);
	}
	if (d->adapter == NULL) {
		m4_daemon_free(d);
		return NULL;
	}
	d->callfsm_ops = m4_callfsm_graph_adapter_ops(d->adapter);

	{
		/* `[automode]` (configurable) -- m4_automode_config_default()
		 * supplies the default four numbers; a positive cfg-> value
		 * overrides just that one field (every existing {0} fixture keeps
		 * the pure default, unaffected -- automode.c itself, the frozen/
		 * golden-traced module, is NOT touched by this). */
		m4_automode_config_t automode_cfg;

		m4_automode_config_default(&automode_cfg);
		if (cfg->automode_on_debounce_s > 0)
			automode_cfg.on_debounce_s = cfg->automode_on_debounce_s;
		if (cfg->automode_off_debounce_s > 0)
			automode_cfg.off_debounce_s = cfg->automode_off_debounce_s;
		if (cfg->automode_max_switches > 0)
			automode_cfg.max_switches = cfg->automode_max_switches;
		if (cfg->automode_rate_window_s > 0)
			automode_cfg.rate_window_s = cfg->automode_rate_window_s;
		d->automode = m4_automode_new(&automode_cfg);
	}
	if (d->automode == NULL) {
		m4_daemon_free(d);
		return NULL;
	}
	m4_callfsm_reason(&d->callfsm_ops, reason, sizeof(reason));
	(void)m4_automode_start(d->automode, monotonic_now(), reason);

	d->enable_micdetect = cfg->enable_micdetect;
	if (d->enable_micdetect) {
		snprintf(d->micdetect_mac, sizeof(d->micdetect_mac), "%s",
			 cfg->micdetect_cfg.mac_address ? cfg->micdetect_cfg.mac_address : "");
		snprintf(d->micdetect_placeholder, sizeof(d->micdetect_placeholder), "%s",
			 cfg->micdetect_cfg.placeholder_name ? cfg->micdetect_cfg.placeholder_name : "");
		d->micdetect_cfg.mac_address = d->micdetect_mac;
		d->micdetect_cfg.placeholder_name =
			d->micdetect_placeholder[0] != '\0' ? d->micdetect_placeholder : NULL;
		d->micdetect_handle = m4_micdetect_open(event, &d->micdetect_cfg,
							 on_micdetect_active_changed, d);
		if (d->micdetect_handle == NULL)
			m4_log(M4_LOG_WARN, M4_T("momentum4d: failed to connect to PipeWire (micdetect)",
"momentum4d: не удалось подключиться к PipeWire (micdetect)"));
	}

	if (cfg->enable_ctl && cfg->socket_path != NULL) {
		d->ctl = m4_ctl_server_open(event, cfg->socket_path, daemon_dispatch, d);
		if (d->ctl == NULL) {
			m4_daemon_free(d);
			return NULL;
		}
	}

	if (cfg->enable_hfp_events) {
		d->hfp_events = m4_hfp_events_open(event, d->runtime_dir, on_hfp_event, d);
		if (d->hfp_events == NULL)
			m4_log(M4_LOG_WARN,
			       M4_T("momentum4d: hfp-events.sock unavailable -- HFP gestures (AT+CHUP etc.) will not be caught",
"momentum4d: hfp-events.sock недоступен -- жесты HFP (AT+CHUP и т.п.) не будут пойманы"));
	}

	if (cfg->enable_tick_timer) {
		if (sd_event_add_time_relative(event, &d->tick_source, CLOCK_MONOTONIC,
						(uint64_t)d->tick_interval_ms * 1000, 10 * 1000,
						on_tick_timer, d) < 0) {
			m4_daemon_free(d);
			return NULL;
		}
	}

	return d;
}

void m4_daemon_free(struct m4_daemon *d)
{
	if (d == NULL)
		return;
	if (d->tick_source != NULL)
		sd_event_source_unref(d->tick_source);
	if (d->earcon_timer != NULL)
		sd_event_source_unref(d->earcon_timer);
	if (d->ctl != NULL)
		m4_ctl_server_close(d->ctl);
	if (d->hfp_events != NULL)
		m4_hfp_events_close(d->hfp_events);
	if (d->micdetect_handle != NULL)
		m4_micdetect_close(d->micdetect_handle);
	if (d->automode != NULL)
		m4_automode_free(d->automode);
	if (d->adapter != NULL)
		m4_callfsm_graph_adapter_free(d->adapter);
	if (d->graph_handle != NULL && d->graph_ops != NULL)
		d->graph_ops->close(d->graph_handle);
	free(d);
}

int m4_daemon_acquire_lock(const char *runtime_dir)
{
	char path[600];
	int fd;

	if (runtime_dir == NULL || runtime_dir[0] == '\0')
		return -1;
	if (mkdir(runtime_dir, 0700) < 0 && errno != EEXIST) {
		m4_log(M4_LOG_ERR, M4_T("momentum4d: failed to create %s: %s",
"momentum4d: не удалось создать %s: %s"), runtime_dir, strerror(errno));
		return -1;
	}
	if (snprintf(path, sizeof(path), "%s/daemon.lock", runtime_dir) >= (int)sizeof(path))
		return -1;

	fd = open(path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
	if (fd < 0) {
		m4_log(M4_LOG_ERR, M4_T("momentum4d: failed to open %s: %s",
"momentum4d: не удалось открыть %s: %s"), path, strerror(errno));
		return -1;
	}
	if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
		if (errno == EWOULDBLOCK)
			m4_log(M4_LOG_ERR, M4_T("momentum4d: already running (lock %s is held)",
"momentum4d: уже запущен (lock %s занят)"), path);
		else
			m4_log(M4_LOG_ERR, M4_T("momentum4d: flock(%s) failed: %s",
"momentum4d: flock(%s) не удался: %s"), path, strerror(errno));
		close(fd);
		return -1;
	}
	return fd;
}

int m4_daemon_run(struct m4_daemon *d)
{
	sd_notify(0, "READY=1");
	sd_event_set_watchdog(d->event, 1);
	return sd_event_loop(d->event);
}
