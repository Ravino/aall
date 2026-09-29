/* core/state.h -- filesystem state momentum4d shares with the rest of the
 * project (state file and latch):
 *   - call-state: $runtime_dir/call-state, a plain KEY=value format
 *     (PROFILE=/SAVED_AT=/REASON=, one per line, tmp+rename atomic write)
 *     -- a state file written by the daemon's callfsm adapter is readable
 *     by any other local tool that understands this same format, and vice
 *     versa, without either side needing to know which one wrote it.
 *   - safe-latch: $runtime_dir/safe-latch. Presence (existence only, content
 *     ignored) means the daemon must not write anything to the PipeWire
 *     graph at all, no matter what `mode` says, until `momentum4 resume`
 *     removes it. Any local tool can set this flag directly (e.g. to force
 *     momentum4d into a safe, read-only state during an emergency audio
 *     rollback) -- momentum4d only needs to RECOGNIZE it; `momentum4 resume`
 *     (cli/resume.c) is what removes it,
 *     and is deliberately a local unlink(), not a daemon command, so it
 *     works even if the daemon is dead.
 *   - daemon mode: $runtime_dir/mode, one line ("observe"/"manual"/"auto").
 *     Persisted so Restart=on-failure/a crash-restart does not silently
 *     revert to the safest mode (observe) *or* silently keep the last
 *     mode -- it deliberately keeps the LAST mode an operator explicitly
 *     set (missing file == first run == observe, matching the daemon's
 *     "observe mode by default"). Lives under $runtime_dir (tmpfs,
 *     usually $XDG_RUNTIME_DIR) on purpose, same tradeoff call-state
 *     already makes: a reboot/logout clears it back to
 *     "unset -> observe", which is the safe direction to fail in.
 *
 * All three live under one directory (`runtime_dir`), passed in by the
 * caller rather than computed here (dependency injection -- see
 * util/config.h's m4_config_runtime_dir() for how main.c/the daemon resolve
 * the real path; tests pass a throwaway tmpdir instead). No function here
 * ever reads XDG_RUNTIME_DIR/MOMENTUM4_RUNTIME_DIR itself.
 */
#ifndef MOMENTUM4_CORE_STATE_H
#define MOMENTUM4_CORE_STATE_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define M4_STATE_PROFILE_LEN 32
#define M4_STATE_REASON_LEN 16

/* mkdir(runtime_dir, 0700) if missing (EEXIST is not an error), then
 * atomically (tmp+rename) writes
 * call-state. `reason` NULL is treated as "manual". Returns 0 on
 * success, -1 on I/O failure (directory not creatable, rename failed, ...). */
int m4_state_write(const char *runtime_dir, const char *profile, const char *reason);

/* True if $runtime_dir/call-state exists (does not validate its content). */
bool m4_state_present(const char *runtime_dir);

/* Reads PROFILE=/REASON= out of $runtime_dir/call-state (first matching
 * "KEY=" line wins --
 * tolerant of a REASON-less legacy file: out_reason is left "" in that
 * case, not
 * an error). Returns 0 if the file exists and PROFILE= was found, -1
 * otherwise (missing file, or no PROFILE= line at all -- an empty/corrupt
 * state file is not distinguishable from "no state" to a caller who only
 * has this return code). */
int m4_state_read(const char *runtime_dir, char *out_profile, size_t out_profile_len,
		   char *out_reason, size_t out_reason_len);

/* unlink($runtime_dir/call-state). Never fails loudly (ENOENT is not an
 * error). */
void m4_state_remove(const char *runtime_dir);

/* stat($runtime_dir/safe-latch) == 0. false on any error (including
 * runtime_dir itself missing) -- "can't tell" is treated as "not latched"
 * only because the daemon ALSO refuses to run at all without a usable
 * runtime_dir (m4_ctl_server_open()/the flock both need it first); this
 * function is never the only thing standing between the daemon and a
 * graph write in a world where runtime_dir is broken. */
bool m4_state_safe_latched(const char *runtime_dir);

typedef enum {
	M4_DAEMON_MODE_OBSERVE = 0, /* default: compute + log
				      * "would do X", never call set_profile,
				      * for EITHER automode-triggered actions OR
				      * ctl.sock `call on/off` -- see
				      * daemon/daemon.c's own top comment for the
				      * full observe/manual/auto matrix and why
				      * `call on/off` is included here too. */
	M4_DAEMON_MODE_MANUAL,       /* only explicit ctl.sock `call on/off`
				      * writes; automode's own mic-driven
				      * decisions are computed (debounce timers
				      * keep ticking) but never executed. */
	M4_DAEMON_MODE_AUTO,         /* both ctl.sock `call on/off` AND
				      * automode-triggered decisions write. */
} m4_daemon_mode_t;

/* "observe"/"manual"/"auto". Never returns NULL. */
const char *m4_daemon_mode_name(m4_daemon_mode_t mode);

/* Case-insensitive parse of "observe"/"manual"/"auto" (also accepts
 * "on"/"off" as aliases for "auto"/"manual" -- shorthand, task text's own
 * ctl.sock command list uses "mode auto|manual|observe" as the canonical
 * spelling, this is only a convenience). Returns 0 and sets *out on a
 * match, -1 (out left untouched) otherwise. */
int m4_daemon_mode_parse(const char *name, m4_daemon_mode_t *out);

/* Atomically (tmp+rename) writes $runtime_dir/mode. Returns 0/-1 like
 * m4_state_write(). */
int m4_state_mode_write(const char *runtime_dir, m4_daemon_mode_t mode);

/* Reads $runtime_dir/mode. Missing file, unreadable file, or unrecognized
 * content all resolve to M4_DAEMON_MODE_OBSERVE -- the safe default, never
 * an error a caller has to handle separately (observe is the safe
 * default). */
m4_daemon_mode_t m4_state_mode_read(const char *runtime_dir);

/* "mic-on-enter" task (`[call] mic-on-enter`, common/m4-conf-keys.def):
 * whether the M4 mic Route is forced on/off at every HFP entry, or restored
 * to whatever it last was when the PREVIOUS call ended. */
typedef enum {
	M4_MIC_ON_ENTER_ON = 0, /* default -- the original
				  * unconditional-unmute behaviour (Bug 2), unchanged. */
	M4_MIC_ON_ENTER_OFF,
	M4_MIC_ON_ENTER_REMEMBER,
} m4_mic_on_enter_t;

/* "on"/"off"/"remember". Never returns NULL. */
const char *m4_mic_on_enter_name(m4_mic_on_enter_t v);

/* Case-insensitive. Returns 0 and sets *out on a match, -1 (out left
 * untouched) otherwise. */
int m4_mic_on_enter_parse(const char *name, m4_mic_on_enter_t *out);

/* mic-last-state: the M4 mic Route's mute state at the moment of the most
 * recent HFP exit (`[call] mic-on-enter = remember`'s own persisted memory).
 * One line "MUTED=0|1", atomically (tmp+rename) written to BOTH
 * `runtime_dir` ($XDG_RUNTIME_DIR/momentum4 -- survives a daemon restart
 * within the same login session/boot) AND `state_dir` ($XDG_STATE_HOME/
 * momentum4 -- survives a reboot too, since $XDG_RUNTIME_DIR is typically
 * tmpfs) -- stored in $XDG_RUNTIME_DIR/momentum4 and in
 * ~/.local/state/momentum4, to survive a daemon restart. Either path
 * may be NULL/"" (skipped); the two writes are independent -- a failure on
 * one does not prevent the other. Returns 0 if at least one write
 * succeeded, -1 if both failed (or both paths were empty). */
int m4_mic_state_write(const char *runtime_dir, const char *state_dir, bool muted);

/* Reads the persisted mic mute state: tries `runtime_dir` first (freshest --
 * survives a daemon restart without a reboot), then `state_dir` (survives a
 * reboot too). Returns 0 and writes *muted_out on the first readable file
 * with a valid MUTED= line, -1 if neither path has one (caller's own
 * "never recorded yet" default applies -- see `[call] mic-on-enter`'s own
 * doc comment on why that default is "mic on", not "mic off"). */
int m4_mic_state_read(const char *runtime_dir, const char *state_dir, bool *muted_out);

#ifdef __cplusplus
}
#endif

#endif /* MOMENTUM4_CORE_STATE_H */
