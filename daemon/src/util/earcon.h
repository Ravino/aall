/* util/earcon.h -- short "call on"/"call off" sounds, played with
 * `timeout 6 pw-play FILE`. callfsm itself only returns an `announce`
 * hint (core/callfsm.h); this is what actually plays it.
 *
 * Fully detached: fork -> setsid -> fork -> execv, the intermediate child is
 * reaped at once, so nothing blocks and no zombie is left. The child only
 * uses async-signal-safe calls (the daemon has PipeWire threads): paths are
 * resolved in the parent. Best-effort: any failure is logged, never
 * propagated.
 *
 * Sounds directory search order: `dir_override` (config `earcon-dir`),
 * $MOMENTUM4_SOUNDS_DIR, $XDG_DATA_HOME/momentum4/sounds (or
 * ~/.local/share/momentum4/sounds), /usr/share/momentum4/sounds, then the
 * build-time repo path data/sounds (M4_SOURCE_SOUNDS_DIR) -- the first
 * that contains <kind>.wav wins.
 */
#ifndef MOMENTUM4_UTIL_EARCON_H
#define MOMENTUM4_UTIL_EARCON_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define M4_EARCON_CALL_ON "call-on"
#define M4_EARCON_CALL_OFF "call-off"
/* Played on a failed call on/off, and when a
 * call action was requested but blocked by observe mode or safe-latch (NOT
 * when it is merely suppressed by the gesture/HFP cooldown -- that is
 * expected debounce, not a failure). Ships in data/sounds/call-error.wav
 * like the others; if a deployment is missing this file (custom
 * earcon-dir override that does not provide it, for example) this just
 * logs "not found" and stays silent, same as any other missing/
 * misconfigured earcon (util/earcon.c's m4_earcon_play(), never a hard
 * failure). */
#define M4_EARCON_CALL_ERROR "call-error"

/* tap = mic: mic mute/unmute, played immediately
 * (daemon.c's daemon_schedule_earcon() with delay_ms<=0 -- no participant
 * recreation happens for a Route mute, unlike a profile switch, so there is
 * nothing to wait for). Ships in data/sounds/{mic-off,mic-on}.wav like
 * the others. */
#define M4_EARCON_MIC_OFF "mic-off"
#define M4_EARCON_MIC_ON "mic-on"

/* Writes the full path of <kind>.wav into buf; NULL if not found. */
const char *m4_earcon_resolve(const char *dir_override, const char *kind, char *buf, size_t cap);

/* Plays <kind>.wav; returns 0 if a player process was started, -1 otherwise. */
int m4_earcon_play(const char *dir_override, const char *kind);

#ifdef __cplusplus
}
#endif

#endif /* MOMENTUM4_UTIL_EARCON_H */
