/* core/automode.h -- pure (no I/O, no subprocess, no clock) decision logic
 * for auto-switching AALL <-> "call mode" by Momentum 4 microphone activity
 * (it lives in the momentum4d core, not a separate script). Every
 * function here takes the current time as an explicit argument (clock
 * injection) and returns a small decision
 * (list of actions), never performing any of them itself -- the I/O layer
 * (momentum4d's watcher/executor, or a test) is responsible for
 * actually switching the call profile, playing an earcon, etc.
 *
 * This header, and automode.c, have NO PipeWire/spa/systemd dependency on
 * purpose (mirrors core/profile_rules.h) -- both must stay a plain
 * `cc -std=gnu11` unit, testable in isolation and differentially tested
 * against a frozen set of reference traces (see tests/test_automode_diff.c).
 *
 * Design:
 *   - Action / m4_automode_action_t: CALL_ON, CALL_OFF, ANNOUNCE_FREEZE.
 *   - AutoModeConfig / m4_automode_config_t: four fields, with
 *     defaults (on_debounce_s=0.3, off_debounce_s=2.5, max_switches=6,
 *     rate_window_s=300.0).
 *   - AutoModeCore / m4_automode_core_t (opaque, heap-allocated --
 *     m4_automode_new()/m4_automode_free()) with five "input
 *     event" methods (start, note_external_call_state,
 *     on_mic_activity_changed, tick, resume) plus note_call_result()
 *     (void) and status().
 *
 * "reason" strings ("none"/"auto"/"manual") are passed through verbatim as
 * C strings, not validated. Buffers are fixed
 * size (M4_AUTOMODE_REASON_LEN) since only these three short values are
 * ever used anywhere in this codebase (matching core/state.h's own
 * call-state file format); a longer string is silently truncated by
 * snprintf, never a buffer overflow.
 */
#ifndef MOMENTUM4_CORE_AUTOMODE_H
#define MOMENTUM4_CORE_AUTOMODE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define M4_AUTOMODE_REASON_LEN 16

typedef enum {
	M4_AUTOMODE_ACTION_NONE = 0,
	M4_AUTOMODE_ACTION_CALL_ON,       /* call m4_callfsm_on(reason="auto") */
	M4_AUTOMODE_ACTION_CALL_OFF,      /* call m4_callfsm_off() */
	M4_AUTOMODE_ACTION_ANNOUNCE_FREEZE, /* flapping detected, automode frozen */
} m4_automode_action_t;

const char *m4_automode_action_name(m4_automode_action_t action);

/* A single AutoModeCore call never returns more than one action:
 * on_mic_activity_changed()/start()/note_external_call_state()/resume()
 * always return zero or one action, and tick()'s
 * two independent debounce branches are mutually exclusive in practice
 * (setting one of _pending_on_at/_pending_off_at always clears the other,
 * in every code path that touches them) plus an early `return` on freeze --
 * see automode.c top-of-file note for the full argument. 2 is a documented
 * safety margin, not an observed maximum. */
#define M4_AUTOMODE_MAX_ACTIONS 2

typedef struct {
	m4_automode_action_t actions[M4_AUTOMODE_MAX_ACTIONS];
	size_t action_count;
} m4_automode_decision_t;

typedef struct {
	double on_debounce_s;
	double off_debounce_s;
	int max_switches;
	double rate_window_s;
} m4_automode_config_t;

/* Fills *cfg with the default AutoModeConfig values. */
void m4_automode_config_default(m4_automode_config_t *cfg);

/* Opaque -- see automode.c. One instance per automode "session", exactly
 * one AutoModeCore instance per process. */
typedef struct m4_automode_core m4_automode_core_t;

/* cfg == NULL uses m4_automode_config_default(). Returns NULL on OOM. */
m4_automode_core_t *m4_automode_new(const m4_automode_config_t *cfg);
void m4_automode_free(m4_automode_core_t *core);

/* Non-destructive startup reconciliation: adopts whatever call-state
 * already exists, changes nothing itself. */
m4_automode_decision_t m4_automode_start(m4_automode_core_t *core, double now, const char *current_reason);

/* The watcher reports what the current call-state reason actually is
 * right now (the daemon does not fight the user -- manual intervention behind
 * automode's back cancels any of automode's own pending transitions). */
m4_automode_decision_t m4_automode_note_external_call_state(m4_automode_core_t *core, const char *reason, double now);

/* Idempotent on a repeated identical value.
 * Drives ENTRY (arms the on-debounce)
 * and, on going false, also arms the exit debounce (the "stream
 * disappeared" half of the long-lived-holder fix: `active` here already
 * includes long-lived holders like signal-desktop, so a stream
 * disappearing ENTIRELY, holder included, still exits exactly as it always
 * has). */
m4_automode_decision_t m4_automode_on_mic_activity_changed(m4_automode_core_t *core, int active, double now);

/* `hold_active` is
 * the SAME kind of signal as `active` above but with known long-lived
 * holder apps (pw/micdetect.h's M4_MIC_HOLDER_BINARIES, e.g.
 * "signal-desktop") excluded -- see that header's "Bug 1: long-lived
 * holders" section for the full rationale. Only ever affects an ALREADY
 * ACTIVE, reason=="auto" call: while such a call is active, hold_active
 * going false ARMS the exit debounce (exactly like `active` going false
 * does), and hold_active going true CANCELS a pending exit (the call
 * continues). Never affects ENTRY (arms no on-debounce) and never touches
 * a manual call (I4), or a call that isn't active at all -- in both of
 * those cases this is a no-op besides updating the cached value returned by
 * m4_automode_status(). Idempotent on a repeated identical value, same
 * convention as on_mic_activity_changed(). */
m4_automode_decision_t m4_automode_on_mic_hold_changed(m4_automode_core_t *core, int hold_active, double now);

/* Processes expired debounce deadlines, applies the
 * rate limiter, freezes on flapping. Call regularly (e.g. every
 * 0.2-0.5s). */
m4_automode_decision_t m4_automode_tick(m4_automode_core_t *core, double now);

/* Rolls back this
 * core's idea of call_active/call_active_reason on a failed CALL_ON/
 * CALL_OFF, so the next tick/event can retry instead of getting stuck out
 * of sync with reality. */
void m4_automode_note_call_result(m4_automode_core_t *core, m4_automode_action_t action, int ok, double now);

/* `momentum4 mode auto`'s resume path: clears the flapping
 * freeze latch (manual operator action, never automode itself). */
m4_automode_decision_t m4_automode_resume(m4_automode_core_t *core, double now);

typedef struct {
	int frozen;
	int mic_active;
	int hold_active; /* Bug 1 fix, see on_mic_hold_changed()'s doc comment above */
	int call_active;
	char call_active_reason[M4_AUTOMODE_REASON_LEN]; /* "none"/"auto"/"manual" */
	int switches_in_window;
} m4_automode_status_t;

/* pending_on_in_s/pending_off_in_s (float, now-relative, useful only
 * for a human status line) are intentionally not exposed here -- not
 * needed by any C caller yet; add if/when a `momentum4 status` field
 * needs it. */
void m4_automode_status(const m4_automode_core_t *core, double now, m4_automode_status_t *out);

#ifdef __cplusplus
}
#endif

#endif /* MOMENTUM4_CORE_AUTOMODE_H */
