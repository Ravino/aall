/* core/callfsm.h -- manual/auto call-mode state machine for Momentum 4.
 * Scope:
 *   - the M4 card profile only: a2dp-sink <-> headset-head-unit (mSBC),
 *     falling back to headset-head-unit-cvsd;
 *   - reason manual|auto (never auto-off a manual
 *     call -- callfsm itself does not enforce that, core/automode.c does;
 *     callfsm just records/returns whatever reason it is given);
 *   - call-state persistence for rollback (via callfsm_ops, see below).
 * Explicitly NOT here: clock.force-quantum, moving other clients'
 * streams (retarget_speech_streams), speech-dispatcher health/repair,
 * default sink/source. No earcon playback either -- `announce` in
 * m4_callfsm_result_t is a HINT for the caller to play one (or not, e.g.
 * "--quiet"), callfsm never calls pw-play itself.
 *
 * All graph/file I/O goes through the abstract m4_callfsm_ops_t
 * (callfsm_ops.h -- deliberately narrower than core/graph_ops.h's struct
 * graph_ops, see that file's own header comment) -- see
 * core/callfsm_fake.c for the deterministic test double used by unit
 * tests, and callfsm_ops.h for why the calls are synchronous.
 *
 * Return codes: 0 = success
 * (including idempotent no-ops), 2 = could not do what was asked. A
 * bad-arguments case does not apply here -- that is a CLI-layer
 * concern, out of scope for this pure core.
 */
#ifndef MOMENTUM4_CORE_CALLFSM_H
#define MOMENTUM4_CORE_CALLFSM_H

#include <stddef.h>

#include "callfsm_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Profile name COMPILED DEFAULTS -- exactly
 * common/m4-conf-keys.def's own [call] profile-msbc/
 * profile-cvsd/profile-music defaults .
 * callfsm.c itself has NO common/m4-conf dependency (this header's own top
 * comment: "no PipeWire/spa/systemd dependency on purpose ... a plain
 * `cc -std=gnu11` unit, testable in isolation and differential-tested"), so
 * these three macros stay the one, single-source-of-truth COMPILED
 * fallback; m4_callfsm_set_profile_names() below is how a config-aware
 * caller (cli/daemon_cmd.c, reading momentum4.conf's [call] section)
 * overrides them for the life of the process WITHOUT changing callfsm.c's
 * own pure, synchronous, ops-only function signatures (m4_callfsm_on/off/
 * status/reason/is_call_profile all stay exactly as before -- no test call
 * site anywhere in tests/ needed to change for this). */
#define M4_CALLFSM_PROFILE_MSBC "headset-head-unit"
#define M4_CALLFSM_PROFILE_CVSD "headset-head-unit-cvsd"
#define M4_CALLFSM_PROFILE_MUSIC_DEFAULT "a2dp-sink"

/* Overrides the three profile names above for the rest of the process
 * (every m4_callfsm_on/off/status/reason/is_call_profile call afterwards,
 * from ANY m4_callfsm_ops_t) -- NULL or "" leaves that one name unchanged.
 * Called exactly once, at daemon startup, by cli/daemon_cmd.c from
 * momentum4.conf's `[call] profile-msbc/profile-cvsd/profile-music`
 * (default values identical to the compiled ones above, so calling this
 * with the registry's own defaults, e.g. because the key was never
 * customised, is a harmless no-op). NOT thread-safe (momentum4d is a single
 * sd-event loop; never called concurrently with an in-flight
 * m4_callfsm_on/off/status/reason call in practice). */
void m4_callfsm_set_profile_names(const char *msbc, const char *cvsd, const char *music_default);

#define M4_CALLFSM_PROFILE_LEN 32
#define M4_CALLFSM_REASON_LEN 16
#define M4_CALLFSM_CARD_LEN 64
#define M4_CALLFSM_MESSAGE_LEN 320

typedef enum {
	M4_CALLFSM_RC_OK = 0,
	M4_CALLFSM_RC_FAILED = 2,
} m4_callfsm_rc_t;

typedef enum {
	M4_CALLFSM_ANNOUNCE_NONE = 0, /* --quiet, or not on a success/transition path */
	M4_CALLFSM_ANNOUNCE_ON,       /* call turned on, an announce is due */
	M4_CALLFSM_ANNOUNCE_OFF,      /* call turned off, an announce is due */
} m4_callfsm_announce_t;

typedef struct {
	m4_callfsm_rc_t rc;
	char message[M4_CALLFSM_MESSAGE_LEN]; /* human-readable outcome, for logging/CLI display */
	m4_callfsm_announce_t announce;
	char codec_label[8];       /* "mSBC"/"CVSD"/"" (only set by m4_callfsm_on() on success) */
	int cvsd_fallback_warning; /* 1 if mSBC was unavailable and CVSD was used instead */
	/* Bug 2 (see callfsm_ops.h's fix_mic_volume_if_needed
	 * doc comment): mirrors that op's own return value verbatim (0 by
	 * default, including on every path other than m4_callfsm_on()'s
	 * success path -- e.g. idempotent no-ops, off(), failures never touch
	 * this field). */
	int mic_volume_fix;
} m4_callfsm_result_t;

/* `on [--quiet]`, reason is "manual" or "auto" (automode passes
 * "auto"; a manual CLI/gesture-driven call passes "manual"). */
m4_callfsm_result_t m4_callfsm_on(const m4_callfsm_ops_t *ops, int quiet, const char *reason);

/* `off [--quiet]`. */
m4_callfsm_result_t m4_callfsm_off(const m4_callfsm_ops_t *ops, int quiet);

typedef struct {
	int m4_present;
	char active_profile[M4_CALLFSM_PROFILE_LEN]; /* "" if m4_present == 0 */
	int call_active;                              /* m4_callfsm_is_call_profile(active_profile) */
	int state_present;
	char state_saved_profile[M4_CALLFSM_PROFILE_LEN]; /* only valid if state_present */
	char state_reason[M4_CALLFSM_REASON_LEN];         /* only valid if state_present */
	/* Set on either mismatch: state_present && !call_active, OR
	 * call_active && !state_present. */
	int stale_warning;
} m4_callfsm_status_t;

m4_callfsm_status_t m4_callfsm_status(const m4_callfsm_ops_t *ops);

/* "none"/"auto"/"manual" (never fails; writes at most
 * outlen-1 bytes + NUL). Old call-state files without a REASON field are
 * treated as "manual" for backward compatibility. */
void m4_callfsm_reason(const m4_callfsm_ops_t *ops, char *out, size_t outlen);

/* True for M4_CALLFSM_PROFILE_MSBC or
 * M4_CALLFSM_PROFILE_CVSD, false otherwise (including NULL/""). */
int m4_callfsm_is_call_profile(const char *profile);

#ifdef __cplusplus
}
#endif

#endif /* MOMENTUM4_CORE_CALLFSM_H */
