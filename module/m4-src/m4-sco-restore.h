/* momentum4 patch 0008: restore the HFP
 * audio link (SCO/eSCO) in place after MOMENTUM 4 closes it from its side.
 * SPDX-License-Identifier: MIT
 *
 * This is NOT upstream PipeWire code. Out-of-tree addition, built into
 * libspa-bluez5.so next to backend-native.c (same library as
 * m4-hfp-events.c). Pure policy only -- no sockets, no loops, no SPA types --
 * so it can be unit-tested standalone (test/sco_restore_test.c). The glue
 * that actually reconnects the socket lives in backend-native.c (patch 0008),
 * because it needs that file's static sco_do_connect()/sco_start_source()/
 * sco_ready().
 *
 * Background: in HFP, a double tap on the M4 touch panel makes
 * the headset release the eSCO link itself (HCI Disconnection Complete,
 * reason 0x13, no AT command before or after) -- QCC HFP "transfer audio to
 * AG". PipeWire 1.4.2 (and upstream master) then only sets the transport to
 * IDLE and closes the fd; sco-sink/sco-source stop their I/O but the nodes
 * stay RUNNING, the transport stays acquired, and nothing ever reconnects
 * while the nodes keep running (our combine sink keeps them running forever).
 * Result: silence until the profile is switched.
 *
 * Policy: after such a drop, if the transport is still in use, reconnect
 * after a short delay; on connect failure retry once; limit the number of
 * restore episodes per time window so a headset that keeps dropping the link
 * (or refuses it) cannot make us loop. When we give up, backend-native.c
 * tells momentum4d (hfp-events.sock "SCOFAIL"), which falls back to A2DP.
 *
 * This file used to ALSO own mode-string
 * parsing ("m4"|"all"|"off"|"0"|"no") and the MOMENTUM-4-name-prefix check
 * (m4_sco_restore_mode_parse()/m4_sco_restore_device_is_target()) -- both
 * moved out to m4-device-policy.h (m4_device_policy_parse_hfp_mode() /
 * m4_device_policy_identity_matches()), which is shared with [hfp]
 * call-control (patch 0006/0013) and the aptx_adaptive codec's own
 * allow-devices gate (patch 0009/0012/0014), so all three read the SAME
 * [device] name-match/address config the same way instead of three
 * independently-hardcoded copies of "MOMENTUM 4". This file now only owns
 * the episode/retry/rate-limit timing state machine below, configurable via
 * struct m4_sco_restore_timing (previously fixed #define constants -- see
 * m4_sco_restore_timing_defaults() for what those used to be, still the
 * compiled-in default when the caller has no [hfp] config to apply).
 */
#ifndef M4_SCO_RESTORE_H
#define M4_SCO_RESTORE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Compiled-in defaults, matching common/m4-conf-keys.def's [hfp]
 * sco-restore-* defaults byte for byte (these used to be the only
 * values, now they are just what m4_sco_restore_timing_defaults() returns
 * when nothing overrides them). Kept as named constants (not inlined into
 * the defaults function) so test/sco_restore_test.c can keep comparing
 * against them by name, same as before this file became configurable. */
#define M4_SCO_RESTORE_FIRST_DELAY_MS 250      /* let the HF finish its own state change */
#define M4_SCO_RESTORE_RETRY_DELAY_MS 1000     /* after a failed connect */
#define M4_SCO_RESTORE_CONNECT_TIMEOUT_MS 2000 /* eSCO setup watchdog per attempt */
#define M4_SCO_RESTORE_MAX_RETRIES 1           /* failed connects per episode before giving up */
#define M4_SCO_RESTORE_WINDOW_MS 30000
#define M4_SCO_RESTORE_MAX_EPISODES 4          /* restore episodes allowed per window, ring-buffered below */

/* Runtime timing, populated from [hfp] sco-restore-first-delay-ms /
 * -retry-delay-ms / -connect-timeout-ms / -max-retries / -window-ms /
 * -max-episodes (common/m4-conf-keys.def). `max_episodes` MUST be
 * <= M4_SCO_RESTORE_MAX_EPISODES (the registry's own range caps it at 1000,
 * well above this compiled ring-buffer bound of 4 -- see m4_sco_restore_init()
 * for how an out-of-range value is handled: clamped, not rejected, same
 * "never fatal" contract as the rest of this plugin's config surface). */
struct m4_sco_restore_timing {
	int first_delay_ms;
	int retry_delay_ms;
	int connect_timeout_ms;
	int max_retries;
	int window_ms;
	int max_episodes;
};

void m4_sco_restore_timing_defaults(struct m4_sco_restore_timing *t);

enum m4_sco_restore_verdict {
	M4_SCO_RESTORE_DONE = 0,   /* nothing (more) to do */
	M4_SCO_RESTORE_SCHEDULE,   /* arm the timer for *delay_ms, then attempt */
	M4_SCO_RESTORE_GIVE_UP,    /* stop trying, report SCOFAIL */
};

struct m4_sco_restore {
	struct m4_sco_restore_timing timing;
	uint64_t episode_ms[M4_SCO_RESTORE_MAX_EPISODES]; /* start times, ring */
	unsigned n_episodes;   /* valid entries in episode_ms (<= timing.max_episodes) */
	unsigned next;         /* ring write position */
	int retries_left;
	bool active;           /* an episode is running (timer armed or connect in flight) */
};

/* Compiled defaults (m4_sco_restore_timing_defaults()) -- unchanged
 * behaviour/signature from before this file's timing became configurable,
 * so every existing call site (and test/sco_restore_test.c) keeps working
 * unmodified. */
void m4_sco_restore_init(struct m4_sco_restore *r);

/* Same, with explicit timing (backend-native.c's real call site, patch
 * 0013) -- `timing->max_episodes` is clamped to
 * [1, M4_SCO_RESTORE_MAX_EPISODES] (the fixed ring-buffer capacity above);
 * every other field is used as given (common/m4-conf-keys.def's own
 * min/max already range-validates them before they ever get here). */
void m4_sco_restore_init_with_timing(struct m4_sco_restore *r, const struct m4_sco_restore_timing *timing);

/* Updates
 * ONLY r->timing, in place -- unlike m4_sco_restore_init_with_timing(), does
 * NOT reset episode/active/retry state. Safe to call every time the caller
 * re-resolves momentum4.conf (e.g. once per drop event, from the main
 * loop), so a config change takes effect on the very next episode without
 * losing in-flight rate-limiting history. max_episodes is clamped the same
 * way m4_sco_restore_init_with_timing() does. */
void m4_sco_restore_set_timing(struct m4_sco_restore *r, const struct m4_sco_restore_timing *timing);

/* An established, in-use link was dropped by the remote side at now_ms
 * (CLOCK_MONOTONIC ms). SCHEDULE (delay in *delay_ms) or GIVE_UP (too many
 * episodes in the window). A drop while an episode is already active
 * (cannot normally happen: the link is not ACTIVE then) is treated as
 * DONE -- the running episode continues. */
enum m4_sco_restore_verdict m4_sco_restore_on_drop(struct m4_sco_restore *r, uint64_t now_ms,
						    int *delay_ms);

/* Result of one reconnect attempt. ok -> DONE (episode over). !ok ->
 * SCHEDULE a retry (*delay_ms) while retries remain, else GIVE_UP. Calling
 * this with no active episode returns DONE and changes nothing. */
enum m4_sco_restore_verdict m4_sco_restore_on_result(struct m4_sco_restore *r, bool ok,
						      int *delay_ms);

/* The attempt was not made because the link is no longer ours to restore
 * (transport released/destroyed, SLC gone, already reconnected by someone
 * else). Ends the episode without counting it as a failure. */
void m4_sco_restore_cancel(struct m4_sco_restore *r);

const char *m4_sco_restore_verdict_name(enum m4_sco_restore_verdict v);

#ifdef __cplusplus
}
#endif

#endif /* M4_SCO_RESTORE_H */
