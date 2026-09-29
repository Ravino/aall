/* daemon/daemon.h -- momentum4d core: wires graph_ops + micdetect + automode
 * + callfsm (via pw/callfsm_graph_adapter.c) + ipc/ctl.c into one sd-event
 * loop. Mode/safe-latch write-gating matrix (this daemon's central safety
 * property):
 *
 *              | ctl.sock `call on/off`      | automode-triggered CALL_ON/OFF
 *   -----------+------------------------------+--------------------------------
 *   OBSERVE    | rejected (ERR), no write     | computed + logged, no write
 *   (default)  |                              | (the daemon computes
 *              |                              | everything but never calls
 *              |                              | set_profile)
 *   MANUAL     | writes (via callfsm)         | computed + logged, no write
 *   AUTO       | writes (via callfsm)         | writes (via callfsm)
 *
 *   safe-latch (`$runtime_dir/safe-latch` exists): overrides ALL of the
 *   above to "no write", in every mode, unconditionally, until
 *   `momentum4 resume` removes the file (checked fresh -- stat(), not
 *   cached -- before every single write attempt, both kinds).
 *
 * The ctl.sock `REPAIR_A2DP` command (an explicit `momentum4
 * repair-a2dp` command) is gated EXACTLY like `call on/off` above (same
 * row: rejected in OBSERVE, writes in MANUAL/AUTO, safe-latch overrides
 * always) -- it performs its own PipeWire write (pw/repair_a2dp.c's
 * set_profile("a2dp-sink", true), via the SAME shared graph_ops handle) and
 * a D-Bus ConnectProfile call, both manual/explicit, never automode-
 * triggered from this table's own automode column (automode's own path to
 * repair-a2dp is core/callfsm.c's repair_profile_if_missing(), invoked
 * indirectly from m4_callfsm_off() under the SAME automode-column gating
 * this table already documents -- see daemon.c's daemon_process_automode_decision()).
 *
 * This matrix is exactly the "should I act" gate (daemon_allow_manual_write()/
 * daemon_allow_auto_write() in daemon.c) -- nothing else in this file, or in
 * pw/graph_pw.c's m4_graph_ops_pw, writes to PipeWire at all (I11/I12): the
 * ONLY call site of `ops->set_profile()` anywhere in momentum4d is
 * pw/callfsm_graph_adapter.c's set_profile_confirm(), invoked only from
 * core/callfsm.c's m4_callfsm_on()/off(), invoked only from the two gated
 * call sites in daemon.c (handle_call() for ctl.sock, daemon_process_
 * automode_decision() for automode). tests/test_daemon.c (a test on the
 * fake graph) asserts this matrix directly against
 * core/graph_ops_fake.c's set_profile_calls counter.
 *
 * Time is always passed in explicitly (double, seconds -- same convention
 * as core/automode.h) rather than read from the clock inside this module,
 * so tests can drive debounce/rate-limit scenarios with synthetic
 * timestamps instead of sleeping in real time; only the small amount of
 * "real" wiring in main.c/cli/daemon_cmd.c (the live sd-event timer
 * callback, the live micdetect callback) ever calls the real clock.
 */
#ifndef MOMENTUM4_DAEMON_DAEMON_H
#define MOMENTUM4_DAEMON_DAEMON_H

#include <stdbool.h>
#include <stddef.h>

#include <systemd/sd-event.h>

#include "../bt/bt_ops.h"
#include "../control/gesture.h"
#include "../control/mpris_player.h"
#include "../control/passthrough.h"
#include "../core/graph_ops.h"
#include "../core/state.h"
#include "../ipc/hfp_events.h"
#include "../pw/micdetect.h"
#include "../util/settings.h"

#ifdef __cplusplus
extern "C" {
#endif

struct m4_daemon_config {
	const struct graph_ops *graph_ops; /* m4_graph_ops_pw (real) or
					      * m4_graph_ops_fake()'s vtable
					      * (tests) -- never NULL. */
	const char *mac_address;
	const char *runtime_dir;  /* call-state/safe-latch/mode -- core/state.h */
	/* mic-on-enter: `[paths] state-dir` (util/config.h's
	 * m4_config_state_dir()) -- only used for `[call] mic-on-enter =
	 * remember`'s reboot-survival half (core/state.h's m4_mic_state_write()/
	 * _read(), via the callfsm adapter's mic_cfg, m4_daemon_new()). NULL/""
	 * -> that half is simply unavailable (a daemon-restart-only fallback via
	 * `runtime_dir` above still works) -- never fatal. */
	const char *state_dir;
	const char *stats_path;   /* helper_alive(), "" / NULL disables it */
	int confirm_timeout_ms;   /* 0 -> callfsm_graph_adapter's own default */

	/* `[daemon] tick-ms`/`graph-retry-s` (configurable). <=0 -> this
	 * file's own compiled default (250ms/5.0s, matching m4-conf-keys.def's
	 * own default for both). */
	int tick_interval_ms;
	double graph_retry_s;

	/* `[daemon] mic-mute-threshold` (configurable, "M4 button =
	 * mute"). <=0 -> this file's own compiled default
	 * (0.02f, M4_MIC_ROUTE_QUIET_EPS's own comment on why). A caller that
	 * genuinely wants 0.0 as the threshold (never a realistic momentum4.conf
	 * value -- see m4-conf-keys.def's own range 0..1 default "0.02") has no
	 * way to request it via this field; same accepted trade-off this
	 * codebase already makes for every other <=0-sentinel int field above. */
	double mic_mute_threshold;

	/* "repair-a2dp" . NULL -> repair-a2dp disabled
	 * entirely: core/callfsm.c's automatic repair path
	 * (repair_profile_if_missing) always no-ops, and the ctl.sock
	 * REPAIR_A2DP command responds ERR. Production: &m4_bt_ops_sdbus
	 * (cli/daemon_cmd.c). Tests: a fake, or simply omitted (every
	 * existing struct m4_daemon_config {0} fixture already leaves this
	 * NULL, so repair stays off by default with zero test changes
	 * required elsewhere). */
	const struct m4_bt_ops *bt_ops;
	const char *bt_adapter;            /* NULL -> "hci0" (repair_a2dp.c's own default) */
	int repair_connect_timeout_ms;     /* <=0 -> repair_a2dp.c's own default */
	int repair_enum_wait_ms;           /* <=0 -> repair_a2dp.c's own default (task: 10000) */
	int repair_a2dp_confirm_timeout_ms; /* <=0 -> falls back to confirm_timeout_ms above
					      * (`[daemon] repair-confirm-timeout-ms`,
					      * configurable) */
	/* `[daemon] repair-a2dp` = auto|manual (configurable). false (every
	 * existing {0} test fixture -- UNAFFECTED, same behaviour as before
	 * this field existed): whenever `bt_ops` is non-NULL, core/callfsm.c's
	 * automatic repair_profile_if_missing() path (invoked from
	 * m4_callfsm_off()) fires when a2dp-sink is confirmed missing from
	 * EnumProfile. true (`[daemon] repair-a2dp = manual`, set by
	 * cli/daemon_cmd.c) -- that automatic path is disabled; `bt_ops` is
	 * still used for the EXPLICIT ctl.sock/`momentum4 repair-a2dp` command
	 * (handle_repair_a2dp() in daemon.c does not consult this flag at all). */
	bool repair_a2dp_manual_only;

	/* `[automode]` (configurable). <=0 (every existing {0} fixture)
	 * -> core/automode.h's own m4_automode_config_default() (0.3s/2.5s/6/
	 * 300s) -- this file never duplicates those four numbers itself. */
	double automode_on_debounce_s;
	double automode_off_debounce_s;
	int automode_max_switches;
	double automode_rate_window_s;

	bool enable_micdetect;    /* false in tests -- drive m4_daemon_mic_active_changed()
				    * directly instead */
	struct m4_micdetect_config micdetect_cfg; /* only read if enable_micdetect */

	bool enable_ctl;          /* false in tests that only need m4_daemon_handle_request() */
	const char *socket_path;  /* only read if enable_ctl */

	/* enable listening on
	 * $runtime_dir/hfp-events.sock (ipc/hfp_events.h) -- false in tests
	 * that only need m4_daemon_hfp_event() called directly. */
	bool enable_hfp_events;

	bool enable_tick_timer;   /* false in tests -- drive m4_daemon_tick() directly */

	/* momentum4.conf + M4 gestures.
	 * has_initial_mode: start in `initial_mode` (from momentum4.conf,
	 * default manual) and write it to $runtime_dir/mode; false (every
	 * older test fixture, {0}) -> the older behaviour, read
	 * $runtime_dir/mode (missing -> observe). */
	bool has_initial_mode;
	m4_daemon_mode_t initial_mode;
	const struct m4_settings *settings;   /* copied; NULL -> m4_settings_defaults() */
	const char *config_path;              /* RELOAD re-reads it; NULL -> RELOAD answers ERR */
	const struct m4_passthrough_ops *passthrough; /* copied; NULL -> passthrough only logs */
	/* Earcon player (util/earcon.h in production, a counter in tests);
	 * NULL -> no sounds. `dir` is settings.earcon_dir ("" = search list). */
	void (*earcon)(void *ctx, const char *dir, const char *kind);
	void *earcon_ctx;
	/* `[earcon] delay-sco-up-ms` . <=0 ->
	 * this file's own compiled default (150ms). */
	int earcon_delay_sco_up_ms;
};

struct m4_daemon;

/* Never fails except on OOM (NULL). graph_ops->open()/micdetect_open()
 * failing (e.g. PipeWire not running yet) is NOT fatal here -- see this
 * file's top comment and daemon.c's reconnect-on-tick logic for graph_ops;
 * a daemon constructed while PipeWire is down starts up fine and reports
 * m4_present=false until it reconnects. `cfg` is only read during this
 * call (nothing is retained by pointer except graph_ops/mac_address, which
 * ARE retained -- `cfg->graph_ops` and the string pointed to by
 * `cfg->mac_address` must outlive the returned daemon; every other string
 * field is copied). */
struct m4_daemon *m4_daemon_new(sd_event *event, const struct m4_daemon_config *cfg);

void m4_daemon_free(struct m4_daemon *d);

/* Feeds a mic-activity transition into automode (live wiring: the real
 * micdetect callback calls this with the wall clock; tests call it
 * directly with synthetic `now`). Any resulting CALL_ON/CALL_OFF decision
 * is gated and, if allowed, executed synchronously before this returns
 * (pw/callfsm_graph_adapter.c's set_profile_confirm() may block for up to
 * confirm_timeout_ms pumping the shared event loop -- see that file). */
void m4_daemon_mic_active_changed(struct m4_daemon *d, bool active, double now);

/* Bug 1 fix: feeds
 * the SAME kind of transition, but with known long-lived holder apps
 * excluded (pw/micdetect.h's hold_active/m4_mic_decide_hold()) -- see
 * core/automode.h's m4_automode_on_mic_hold_changed() doc comment for the
 * exact semantics (only ever arms/cancels the exit debounce of an already
 * active auto call; never arms entry). Live wiring calls this alongside
 * m4_daemon_mic_active_changed() on every micdetect callback (both, always,
 * regardless of which one actually changed -- both are individually
 * idempotent on a repeated value, so this is safe and avoids any ordering
 * ambiguity between the two signals). */
void m4_daemon_mic_hold_changed(struct m4_daemon *d, bool hold_active, double now);

/* Drives automode's debounce/rate-limit timers (live wiring: a periodic
 * sd-event timer set up by m4_daemon_new() when cfg->enable_tick_timer,
 * calling this with the wall clock every ~250ms; tests call it directly).
 * Also where graph_ops reconnection is retried if the last open() attempt
 * failed (see this file's top comment). */
void m4_daemon_tick(struct m4_daemon *d, double now);

/* Test/CLI-internal entry point: dispatches one ctl.sock request WITHOUT a
 * real socket (ipc/ctl.c's m4_ctl_server, when enabled, calls this too, via
 * the cb it was given -- both paths run the exact same code). `request` is
 * one line, already trimmed (no trailing \n/\r), never empty. Always
 * writes a complete response (protocol: ipc/ctl.h) into `response`. */
void m4_daemon_handle_request(struct m4_daemon *d, const char *request,
			       char *response, size_t response_cap);

struct m4_daemon_status {
	m4_daemon_mode_t mode;
	bool safe_latched;

	bool m4_present;
	bool m4_connected;
	bool have_profile;
	char profile_name[64];

	bool frozen;
	bool mic_active;
	bool call_active;
	char call_reason[16];
	int switches_in_window;
};

/* Snapshot of cached state, no I/O beyond a stat() for safe_latched (same
 * cost core/state.c's m4_state_safe_latched() always has) -- exposed for
 * tests so they do not have to parse ctl.sock's text protocol just to
 * assert on internal state (m4_daemon_handle_request()'s GET/STATUS text
 * output is built from exactly this same data, see daemon.c). */
void m4_daemon_get_status(struct m4_daemon *d, struct m4_daemon_status *out);

/* the daemon's own MPRIS player (control/mpris_player.h),
 * created by the caller (cli/daemon_cmd.c) AFTER m4_daemon_new() because
 * its command callback needs the daemon pointer. NULL detaches. Not owned --
 * the caller frees it (before m4_daemon_free()). */
void m4_daemon_attach_player(struct m4_daemon *d, struct m4_mpris_player *player);

/* One command from M4 (via BlueZ -> our MPRIS player; tests call it
 * directly). Logs it (info, with wall-clock time), updates the reported
 * PlaybackStatus per `player-status`, then runs the mapped action:
 *   none                 -> log only;
 *   call-toggle/on/off   -> the SAME gated path as ctl.sock `call on/off`
 *                          (observe/safe-latch -> no write, logged), plus a
 *                          cooldown (gesture-cooldown-ms after the previous
 *                          call action FINISHED) and an earcon on success;
 *   mic-toggle           -> tap = mic: toggles the
 *                          M4 mic Route's mute (own cooldown timer, separate
 *                          from call-on/off's -- see daemon.c), no-ops
 *                          (+call-error earcon) when there is no mic route
 *                          right now (i.e. not in a call);
 *   passthrough(-exact)  -> passthrough ops (session MPRIS player).
 * `ev->monotonic_s` is used as "now" (tests pass synthetic values). */
void m4_daemon_player_event(struct m4_daemon *d, const struct m4_player_event *ev);

struct m4_daemon_gesture_status {
	m4_player_status_policy_t status_policy;
	m4_player_status_policy_t player_status; /* concrete, what M4 is told */
	bool have_last;
	m4_player_cmd_t last_cmd;
	m4_gesture_action_t last_action;
	char last_result[160];
	int events;         /* commands received */
	int call_actions;   /* call on/off actually executed (either result) */
	int passthroughs;   /* forward() calls */
	int ignored;        /* cooldown/busy/observe/safe-latch */
};

void m4_daemon_get_gesture_status(struct m4_daemon *d, struct m4_daemon_gesture_status *out);

/* One HFP call-control AT command forwarded by patch 0006 (ipc/hfp_events.h;
 * live wiring: hfp-events.sock's io callback; tests call it directly with a
 * synthetic `ev`). ev->verb == M4_HFP_VERB_RAW (patch 0007,
 * "tap = mic" -- an AT command backend-native.c could not
 * name) is handled FIRST and separately: logged at INFO with the raw text
 * (ev->raw_text) for live diagnosis, does NOT consult settings.hfp_* or run
 * any action (semantics unknown), does NOT touch the gesture/HFP cooldown.
 * For every named verb, looks up the action for `ev->verb` in settings
 * (hfp-chup/hfp-ata/hfp-bldn/hfp-bvra), logs it, then runs the mapped
 * action:
 *   none                 -> log only;
 *   call-toggle/on/off   -> the SAME gated path as ctl.sock `call on/off`
 *                          and gestures (observe/safe-latch -> no write,
 *                          logged), sharing the SAME cooldown timer/state as
 *                          gestures (gesture-cooldown-ms after the previous
 *                          call action of EITHER kind finished) and an
 *                          earcon on success;
 *   mic-toggle           -> same mic-toggle path as m4_daemon_player_event()
 *                          above (own cooldown timer);
 *   passthrough(-exact)  -> not meaningful for a telephony AT command,
 *                          logged as a misconfiguration warning, no action.
 * `ev->monotonic_s` is used as "now" (tests pass synthetic values, same
 * convention as m4_daemon_player_event()). */
void m4_daemon_hfp_event(struct m4_daemon *d, const struct m4_hfp_event *ev);

struct m4_daemon_hfp_status {
	bool have_last;
	m4_hfp_verb_t last_verb;
	m4_gesture_action_t last_action;
	char last_result[160];
	int events;        /* datagrams received and accepted (known verb, right uid) */
	int call_actions;  /* call on/off actually executed (either result) */
	int ignored;       /* cooldown/busy/observe/safe-latch/unsupported action */
	/* patch 0007 ("tap = mic"): diagnostic
	 * M4_HFP_VERB_RAW events (an AT command backend-native.c could not
	 * name) -- a convenient subtotal of `events` above (RAW events are
	 * accepted datagrams too, so they already count there as well) plus
	 * the last raw command text seen. */
	int raw_events;
	char last_raw_text[64];
};

void m4_daemon_get_hfp_status(struct m4_daemon *d, struct m4_daemon_hfp_status *out);

/* tap = mic: explicit
 * mic on/off/toggle, the ctl.sock `MIC ON`/`MIC OFF`/`MIC TOGGLE` backend
 * (cli/mic.c) -- same gated path as handle_call() (safe-latch/observe/
 * action_in_progress -- NOT the gesture/HFP cooldown, matching handle_call()
 * not applying gesture-cooldown-ms to ctl.sock `call on/off` either), and
 * the SAME underlying write + earcon as the gesture/HFP mic-toggle path
 * (core/graph_ops.h's set_mic_volume(), the same Route as the Bug-2 fix).
 * `want_mute`: true = "mic off" (mute), false = "mic on" (unmute); ignored
 * when `toggle` is true (flips whatever the currently cached state is).
 * Returns true and writes `out` (if non-NULL) with the resulting mic status
 * on success; false (with `out`, if non-NULL, left at the PRE-call cached
 * state) if the write was rejected by a gate, or there is no mic route right
 * now (not in a call) -- `m4_daemon_get_mic_status()` below always reflects
 * the last-known state regardless of which of these two functions was
 * called last. */
struct m4_daemon_mic_status {
	bool have_route;   /* mic Route currently exists (i.e. a call is active) */
	bool mute;         /* only meaningful while have_route */
	float volume;      /* linear 0..1, only meaningful while have_route */
	int actions;       /* mic on/off/toggle actually executed (either result) */
	int ignored;       /* cooldown/busy/observe/safe-latch/no-route */
	char last_result[160];
};

bool m4_daemon_mic_set(struct m4_daemon *d, bool want_mute, bool toggle, const char *origin,
			struct m4_daemon_mic_status *out);

void m4_daemon_get_mic_status(struct m4_daemon *d, struct m4_daemon_mic_status *out);

/* $runtime_dir/daemon.lock, flock(LOCK_EX|LOCK_NB):
 * a single-instance lock via flock. Returns an fd to keep open for the process's entire
 * lifetime (closing it, including via exit or being killed, releases the
 * lock automatically -- nothing to explicitly unlock) on success, -1 if
 * another instance already holds it or on any I/O error (both cases
 * logged; callers must not proceed to m4_daemon_new()/sd_notify() after
 * -1). Creates `runtime_dir` (mkdir 0700) if missing. Deliberately a
 * free function, not part of m4_daemon_new(), so tests can construct a
 * daemon against a fake graph without ever touching a real lock file (the
 * fake-graph tests do not call this at all). */
int m4_daemon_acquire_lock(const char *runtime_dir);

/* sd_notify(READY=1), sd_event_set_watchdog(true) (WatchdogSec from the
 * unit -> WATCHDOG_USEC env var -- a no-op, not an error, if momentum4d is
 * not running under systemd, e.g. interactively for manual testing), then
 * sd_event_loop(event) until sd_event_exit() is called (there is no such
 * call anywhere in this daemon under normal operation -- it runs until
 * killed/SIGTERM'd, matching Restart=always). Returns sd_event_loop()'s
 * result (negative errno on a genuine event-loop failure). */
int m4_daemon_run(struct m4_daemon *d);

#ifdef __cplusplus
}
#endif

#endif /* MOMENTUM4_DAEMON_DAEMON_H */
