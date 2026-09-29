/* pw/callfsm_graph_adapter.h -- the thin synchronous adapter that
 * reconciles core/callfsm_ops.h's narrow synchronous m4_callfsm_ops_t
 * (core/callfsm.c's own I/O interface) with core/graph_ops.h's wider,
 * callback-driven struct graph_ops (pw/graph_pw.c's m4_graph_ops_pw, the
 * ONLY write path to PipeWire, I11/I12): build exactly this adapter, do
 * NOT merge the two interfaces and do NOT change callfsm.c.
 *
 * Design:
 *  - Shares the caller's ALREADY-OPEN `struct graph_ops` handle (opened
 *    once by daemon/daemon.c against the same `sd_event`, alongside its own
 *    on_state_changed() tracking for status/mode decisions) -- this adapter
 *    does not open a second PipeWire connection (a connection was left
 *    open for graph_ops/micdetect; here, for graph_ops/callfsm specifically,
 *    sharing was straightforward because both live in the daemon's own
 *    process and the adapter never needs its own on_state_changed callback,
 *    only get_state()'s cache plus set_profile()).
 *  - card_present()/get_active_profile()/set_profile_confirm() all read
 *    from graph_ops's cached get_state() -- instant, no round trip -- except
 *    set_profile_confirm(), which issues set_profile() (fire-and-forget)
 *    and then WAITS for get_state()'s profile_name to catch up, by calling
 *    sd_event_run() on the SHARED event loop in a bounded poll (profile
 *    confirmation with a timeout is callfsm_ops's job, not
 *    callfsm.c's). This is a nested/reentrant use of the daemon's own
 *    sd_event -- see daemon/daemon.c's top comment for the "at most one
 *    call/mode action in flight" guard that exists specifically to bound
 *    how deep this nesting can ever go.
 * CORRECTION: sd_event_run() does not
 *    nest -- inside any sd-event callback (every live call path) it
 *    returns -EBUSY at once, so this wait gave up immediately. The poll now
 *    goes through pw/graph_wait.h: nested -> pump only the graph's own
 *    PipeWire loop (graph_ops->iterate); top level -> sd_event_run() as
 *    before.
 *  - state_{write,present,read,remove} go straight to core/state.c's
 *    call-state file (see core/state.h for the shared, tool-agnostic
 *    KEY=value format).
 *  - helper_alive() reads $runtime_dir's stats.json via metrics/stats.c
 *    (best-effort -- matches callfsm_ops.h's documented contract: never
 *    fatal, result only logged).
 *  - card_buf: the M4 has exactly one card in this whole project,
 *    so this adapter always writes the fixed opaque token "m4" into
 *    card_buf and ignores the `card` argument passed back into
 *    get_active_profile()/set_profile_confirm() -- there is nothing to
 *    parse or look up, unlike a shell-based approach that has to grep
 *    `pactl list cards short` for a name string it then has to pass back
 *    into every other pactl invocation.
 */
#ifndef MOMENTUM4_PW_CALLFSM_GRAPH_ADAPTER_H
#define MOMENTUM4_PW_CALLFSM_GRAPH_ADAPTER_H

#include <systemd/sd-event.h>

#include "../bt/bt_ops.h"
#include "../core/callfsm_ops.h"
#include "../core/graph_ops.h"
#include "../core/state.h"

#ifdef __cplusplus
extern "C" {
#endif

struct m4_callfsm_graph_adapter;

/* Wires callfsm_ops.h's repair_profile_if_missing
 * to pw/repair_a2dp.c, reusing the SAME shared graph_ops handle/event this
 * adapter already has (no second PipeWire connection, same reasoning as
 * this file's own top comment on sharing graph_handle). Optional: pass NULL
 * to m4_callfsm_graph_adapter_new() below to disable repair entirely (the
 * resulting ops.repair_profile_if_missing() always returns 0 without
 * attempting anything) -- every existing caller/test that does not care
 * about repair-a2dp keeps working unchanged by passing NULL here. */
struct m4_callfsm_graph_adapter_repair_config {
	const struct m4_bt_ops *bt_ops; /* m4_bt_ops_sdbus (production) or a fake (tests) --
					  * must not be NULL if this struct itself is passed */
	const char *mac_address;
	const char *bt_adapter;         /* NULL -> repair_a2dp.c's own "hci0" default */
	int connect_timeout_ms;         /* <=0 -> repair_a2dp.c's own default */
	int enum_wait_ms;                /* <=0 -> repair_a2dp.c's own default (task: 10000) */
};

/* "mic-on-enter" task (`[call] mic-on-enter`, common/m4-conf-keys.def):
 * configures WHAT callfsm_ops.h's fix_mic_volume_if_needed() targets at
 * every confirmed HFP entry (core/callfsm.c's m4_callfsm_on(), called
 * unconditionally regardless of `quiet` -- so this applies uniformly to
 * BOTH a manual/ctl.sock call-on AND an automode-triggered one, unlike
 * daemon.c's own earcon-scheduling code, which only runs on the manual
 * path). Pass NULL to m4_callfsm_graph_adapter_new() below for the
 * pre-existing behaviour (M4_MIC_ON_ENTER_ON, i.e. its own "on" is
 * value-for-value what every caller already got before this struct
 * existed) -- every pre-existing test/production call site that does not
 * care about this feature keeps working unchanged by passing NULL. */
struct m4_callfsm_graph_adapter_mic_config {
	m4_mic_on_enter_t mode;
	/* `[paths] state-dir` (util/config.h's m4_config_state_dir()) -- only
	 * consulted for M4_MIC_ON_ENTER_REMEMBER (core/state.h's
	 * m4_mic_state_read()'s own `state_dir` argument). May be NULL/"" (the
	 * reboot-survival half of remember mode is then simply unavailable --
	 * a daemon-restart-only fallback via `runtime_dir`, this adapter's own
	 * constructor argument, still works). Copied. */
	const char *state_dir;
	/* Same threshold daemon.c's own `[daemon] mic-mute-threshold` uses
	 * (d->mic_mute_threshold) -- passed in explicitly, rather than a
	 * second compiled constant here, so both places agree on what "close
	 * enough to muted/unmuted" means. <=0 -> this file's own compiled
	 * default (M4_CALLFSM_MIC_QUIET_EPS, same value/rationale as
	 * daemon.c's M4_DAEMON_DEFAULT_MIC_ROUTE_QUIET_EPS). */
	float mute_threshold;
};

/* `event`/`ops`/`graph_handle` must all outlive the returned adapter (none
 * of them are owned/closed by it -- graph_handle in particular is the
 * caller's, typically the daemon's own long-lived m4_graph_ops_pw handle).
 * `runtime_dir`/`stats_path` are copied. `confirm_timeout_ms` bounds
 * set_profile_confirm()'s wait (the default is a
 * few seconds -- see daemon.h's own confirm_timeout_ms comment). `repair_cfg`
 * (may be NULL, see its own doc comment above) is copied -- the strings it
 * points to do NOT need to outlive this call. Returns NULL on OOM only --
 * never fails on bad paths (those just make the corresponding ops calls
 * fail later, exactly like a missing $XDG_RUNTIME_DIR would in the
 * shell). */
struct m4_callfsm_graph_adapter *m4_callfsm_graph_adapter_new(
	sd_event *event, const struct graph_ops *ops, void *graph_handle,
	const char *runtime_dir, const char *stats_path, int confirm_timeout_ms,
	const struct m4_callfsm_graph_adapter_repair_config *repair_cfg,
	const struct m4_callfsm_graph_adapter_mic_config *mic_cfg);

void m4_callfsm_graph_adapter_free(struct m4_callfsm_graph_adapter *a);

/* `momentum4 reload` (daemon.c's handle_reload()): `[call] mic-on-enter` is
 * baked into the adapter at construction time (above) for every OTHER
 * caller's sake (a plain, unchanging field read on every HFP entry) -- this
 * setter is the one place that value is allowed to change for the rest of
 * the running adapter's life, mirroring core/callfsm.c's own
 * m4_callfsm_set_profile_names() precedent (module state written once, from
 * a fresh momentum4.conf read, never concurrently with an in-flight
 * m4_callfsm_on()/off() call -- momentum4d is a single sd-event loop). Does
 * NOT touch `state_dir`/`mute_threshold` (paths/thresholds are not
 * genuinely reload-able the way a three-way enum is; unchanged since
 * the earlier "some knobs need a restart" precedent, e.g.
 * `player`). */
void m4_callfsm_graph_adapter_set_mic_on_enter(struct m4_callfsm_graph_adapter *a, m4_mic_on_enter_t mode);

/* Returns an m4_callfsm_ops_t bound to `a` via ctx -- pass straight into
 * m4_callfsm_on()/off()/status()/reason(). `a` must outlive the returned
 * ops (mirrors core/callfsm_fake.c's m4_callfsm_fake_ops()). */
m4_callfsm_ops_t m4_callfsm_graph_adapter_ops(struct m4_callfsm_graph_adapter *a);

#ifdef __cplusplus
}
#endif

#endif /* MOMENTUM4_PW_CALLFSM_GRAPH_ADAPTER_H */
