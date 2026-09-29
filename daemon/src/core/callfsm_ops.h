/* core/callfsm_ops.h -- abstract interface core/callfsm.c uses to talk to
 * "the graph" (PipeWire card profile + persisted call-state + helper
 * liveness), so that callfsm.c stays pure/testable.
 *
 * This is a narrow, SYNCHRONOUS request/response interface (set profile,
 * then poll for the resulting state with its own timeout) -- deliberately
 * separate from core/graph_ops.h's struct graph_ops, which is a wider,
 * callback-driven (on_state_changed), sd-event-integrated interface used
 * to drive the daemon's live PipeWire state (EnumProfile, connection
 * status, etc.). pw/callfsm_graph_adapter.c is the reconciliation between
 * the two: a thin synchronous adapter built on top of struct graph_ops
 * (calling m4_graph_ops_pw's set_profile() + polling get_state() with its
 * own timeout, wrapped in a callfsm_ops_t of the shape below).
 *
 * Everything below this point is otherwise unchanged from the original
 * design: every function takes an opaque `ctx` instead of being a method
 * on a concrete type, so callfsm.c never includes pipewire/spa/sd-event or
 * any file-I/O headers. Two implementations exist so far:
 *   - core/callfsm_fake.c: deterministic in-memory fake for unit tests.
 *   - a real implementation (pw/callfsm_graph_adapter.c, adapting
 *     pw/graph_pw.c's struct graph_ops plus a real call-state file under
 *     $XDG_RUNTIME_DIR/momentum4).
 *
 * These calls are deliberately SYNCHRONOUS and may block/retry internally
 * (a `pactl set-card-profile`-style change followed by a poll loop with
 * its own timeout) -- callfsm.c
 * itself never sleeps or loops on wall-clock time; all waiting is the ops
 * implementation's job. callfsm is a decision algorithm, not an I/O loop
 * -- no quantum/thread/speech handling belongs in it.
 */
#ifndef MOMENTUM4_CORE_CALLFSM_OPS_H
#define MOMENTUM4_CORE_CALLFSM_OPS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct m4_callfsm_ops {
	void *ctx;

	/* Is the Momentum 4 card present right now? On success (1), writes an
	 * opaque card identifier into card_buf (implementation-defined
	 * encoding -- callfsm only ever passes it back verbatim to the other
	 * ops, never parses it). Returns 0 if absent, -1 on a hard I/O error
	 * (callfsm treats -1 exactly like 0/absent -- there is no separate
	 * error path for this case). */
	int (*card_present)(void *ctx, char *card_buf, size_t card_buf_len);

	/* Active profile name of `card` into buf. Returns 0 on success
	 * (buf may still be "" if the profile could not be read -- mirrors
	 * active_profile_for_card() returning empty on failure), -1 on error. */
	int (*get_active_profile)(void *ctx, const char *card, char *buf, size_t buflen);

	/* Requests switching `card` to `profile` AND waits for it to be
	 * confirmed (implementation's own timeout). Returns 1 if confirmed,
	 * 0 otherwise (request failed outright, or never confirmed in time). */
	int (*set_profile_confirm)(void *ctx, const char *card, const char *profile);

	/* Persisted call-state (mirrors $XDG_RUNTIME_DIR/momentum4/call-state:
	 * PROFILE=<saved_profile>, REASON=<reason>). */
	int (*state_write)(void *ctx, const char *saved_profile, const char *reason);
	int (*state_present)(void *ctx);
	/* 0 on success (out_profile/out_reason filled), -1 if no state is
	 * present (callers must check state_present() first in practice, but
	 * this must not crash either way). */
	int (*state_read)(void *ctx, char *out_profile, size_t out_profile_len,
			   char *out_reason, size_t out_reason_len);
	void (*state_remove)(void *ctx);

	/* Best-effort, never fatal to the caller -- the result is only
	 * logged on `off`, never changes its exit code or message. Returns
	 * 1 alive, 0 not alive/unknown. */
	int (*helper_alive)(void *ctx);

	/* "repair-a2dp" (works around the live CALL_ON bug that
	 * motivated this field). Called ONLY by m4_callfsm_off() (never on()),
	 * and ONLY as a last resort, AFTER set_profile_confirm()+
	 * get_active_profile() have ALREADY both failed to confirm `profile`
	 * (== M4_CALLFSM_PROFILE_MUSIC_DEFAULT, "a2dp-sink", in every call
	 * site that exists today -- see callfsm.c's confirm_or_repair()).
	 *
	 * a2dp-sink can become structurally ABSENT
	 * from the card's EnumProfile after an HFP call (bluez5-device.c's
	 * build_profile() refuses to even list it once `connected_profiles`
	 * loses the A2DP_SINK bit) -- at that point no amount of
	 * set_profile(a2dp-sink) retries can EVER succeed; BlueZ has to be
	 * asked to reconnect that one profile first
	 * (org.bluez.Device1.ConnectProfile, "momentum4 repair-a2dp").
	 *
	 * Implementations MUST check whether `profile` is ACTUALLY missing
	 * from EnumProfile before doing anything network/D-Bus-visible (the
	 * requirement is "ONLY if a2dp-sink is absent from
	 * EnumProfile", verbatim) -- if it is present (the
	 * earlier failure was for some other reason, e.g. a merely slow
	 * confirm), this must be a fast, side-effect-free no-op returning 0,
	 * NOT a repair attempt that was never needed.
	 *
	 * On an ACTUAL repair attempt (profile confirmed missing): returns 1
	 * if `profile` is now confirmed ACTIVE (the implementation's own
	 * responsibility to verify before returning 1 -- callfsm.c's
	 * confirm_or_repair() trusts a `1` return to mean "already
	 * re-verified", it does not re-issue set_profile_confirm() itself),
	 * 0 if the repair attempt failed or timed out.
	 *
	 * NULL is a valid value for this function pointer (e.g. an ops
	 * implementation with no way to repair, or that intentionally wants
	 * repair disabled) -- callfsm.c checks for NULL before calling it
	 * and simply skips repair in that case, the exact same best-effort
	 * contract as helper_alive() above. */
	int (*repair_profile_if_missing)(void *ctx, const char *card, const char *profile);

	/* M4's own reported HFP mic
	 * gain (AT+VGM) can be 0%/muted right after a (re)connect or a
	 * profile switch into headset-head-unit/-cvsd (bluez5-device.c
	 * mirrors whatever the headset itself reports verbatim -- this is
	 * NOT something WirePlumber's stream-restore can fix, and it is not
	 * a PipeWire bug). Called ONCE by m4_callfsm_on(), right after a
	 * successful (confirmed) mSBC/CVSD switch -- best-effort, NEVER
	 * changes on()'s rc on its own, same contract as helper_alive()/
	 * repair_profile_if_missing() above (NULL is valid -- skip entirely).
	 * `card` is the same opaque token card_present() produced.
	 * Returns: 1 = was muted and/or quiet, has now been fixed AND
	 * re-confirmed (implementation's own job -- callfsm.c does not poll
	 * again); 0 = nothing needed fixing (already unmuted and effectively
	 * full volume), OR no mic route observed at all right now (e.g. an
	 * implementation with no Route support, or a transient gap in the
	 * observation -- see pw/graph_pw.c's own doc comment on why Route
	 * isn't always fresh); -1 = needed fixing but the fix could not be
	 * confirmed (implementation's own timeout). */
	int (*fix_mic_volume_if_needed)(void *ctx, const char *card);
} m4_callfsm_ops_t;

#ifdef __cplusplus
}
#endif

#endif /* MOMENTUM4_CORE_CALLFSM_OPS_H */
