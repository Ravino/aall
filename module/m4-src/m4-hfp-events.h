/* momentum4 :
 * best-effort notification of HFP telephony AT commands MOMENTUM 4 sends
 * while there is no ModemManager to forward them to.
 * SPDX-License-Identifier: MIT
 *
 * This is NOT upstream PipeWire code. Out-of-tree addition only, built as
 * part of libspa-bluez5.so (spa/plugins/bluez5/meson.build,
 * bluez5-backend-hfp-native sources -- same library backend-native.c is
 * part of). Must never be loaded by a production WirePlumber without an
 * explicit, reviewed decision .
 *
 * Background: in HFP mode, MOMENTUM 4's own gesture->AT-command
 * mapping (swipe back = "end call" etc., per the Sennheiser manual) sends
 * plain HFP AT commands (AT+CHUP, ATA, AT+BLDN, and -- if the AG ever
 * advertises VOICE_RECOG -- AT+BVRA) over the RFCOMM channel BlueZ hands
 * entirely to PipeWire (Profile1.NewConnection). backend-native.c's
 * rfcomm_hfp_ag() is the only code that ever sees these bytes; upstream, if
 * no ModemManager is registered (this machine never has one), it answers
 * every such command with "+CME ERROR: 1" and does nothing else -- no SPA
 * event, no D-Bus signal, no node property. Catching the gesture without
 * touching PipeWire is only possible by scraping the WirePlumber log
 * . This
 * module is variant B: hook the same rfcomm_hfp_ag() branch directly and
 * fan the event out over a UNIX datagram socket, at sub-5-ms latency.
 */
#ifndef M4_HFP_EVENTS_H
#define M4_HFP_EVENTS_H

#include <spa/support/log.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One event: MOMENTUM 4 sent an AT command backend-native.c has no modem to
 * forward, and patch 0006 answered OK instead of the usual +CME ERROR.
 *
 * `verb` is the AT command with any "AT+" prefix and "=..." argument
 * stripped, e.g. "CHUP", "ATA", "BLDN", "BVRA" -- never NULL, never empty
 * (callers must check before calling; see backend-native.c's call site).
 * `bdaddr` is the device address as BlueZ/PipeWire's spa_bt_device reports
 * it (e.g. "00:1B:66:00:00:01"), or NULL if unknown -- still sent, with a
 * placeholder field, so the daemon side always gets a well-formed line.
 *
 * Called from PipeWire's SPA *main loop* thread (not the RT data thread,
 * but a thread that must still never block -- a stall here delays every
 * other bluez5 D-Bus/RFCOMM callback, including the ones that keep MOMENTUM
 * 4's media transport alive). Guarantees, all of them load-bearing for
 * that reason:
 *
 *   - never blocks: the socket is created once with SOCK_DGRAM |
 *     SOCK_NONBLOCK | SOCK_CLOEXEC, and sendto() additionally passes
 *     MSG_DONTWAIT (redundant with SOCK_NONBLOCK, kept because the task
 *     that requested this module named it explicitly). A full receive
 *     buffer, a stuck/slow momentum4d, or momentum4d not running at all
 *     all resolve to a non-blocking EAGAIN/ENOBUFS/ENOENT/ECONNREFUSED,
 *     never a wait;
 *   - never raises SIGPIPE: this socket is never connect()ed (a
 *     connectionless AF_UNIX SOCK_DGRAM sendto() to a nonexistent or
 *     unbound path just fails with ENOENT/ECONNREFUSED, it cannot SIGPIPE
 *     the process the way a write() to a dead connected stream socket
 *     could), and MSG_NOSIGNAL is set on top for defense-in-depth;
 *   - never logs above spa_log_debug(): "momentum4d is not running" /
 *     "hfp-events.sock does not exist yet" is expected steady state (e.g.
 *     right after a WirePlumber restart, before momentum4d has bound the
 *     socket), not a bug worth a warn-level line on every single AT
 *     command;
 *   - never aborts/crashes: every syscall return value is checked, nothing
 *     here is assumed to succeed. If the *first* socket() call ever fails
 *     (only realistic cause: EMFILE/ENFILE), every subsequent call becomes
 *     a single branch (no retry storm, no repeated failing syscalls).
 *
 * Target path: "$XDG_RUNTIME_DIR/momentum4/hfp-events.sock", falling back
 * to "/run/user/<uid>/momentum4/hfp-events.sock" if $XDG_RUNTIME_DIR is
 * unset (same fallback rule as every other momentum4 socket default, see
 * m4_ah_user_config_default_socket_path() in m4-adaptive-config.c). If the
 * daemon has not bound that path (not running, or running under a
 * different $XDG_RUNTIME_DIR), the datagram is silently dropped -- SOCK_DGRAM
 * needs no connect()/reconnect state machine, so the path is simply
 * re-resolved on every call, at zero extra cost once momentum4d does start.
 */
void m4_hfp_events_notify(struct spa_log *log, const char *verb, const char *bdaddr);

/* Overrides the socket path both m4_hfp_events_notify()
 * and m4_hfp_events_notify_raw() send to, from [hfp] events-socket
 * (momentum4.conf, common/m4-conf-keys.def) -- the daemon side
 * (ipc/hfp_events.c) reads the SAME key, so the two must be able to agree
 * on a non-default path together. `path` is copied (safe to pass a
 * stack/temporary string); NULL or "" clears the override, reverting to the
 * built-in "$XDG_RUNTIME_DIR/momentum4/hfp-events.sock" default. Meant to
 * be called from backend-native.c once per resolved m4_hfp_config_get()
 * (main loop thread only, same as every other caller in this module --
 * no lock, matches this whole module's "one native backend instance per
 * process" singleton style). */
void m4_hfp_events_set_socket_path(const char *path);

/* momentum4: diagnostic extension for the "mic-toggle tap"
 * follow-up. We do NOT yet know whether MOMENTUM 4's
 * touch-panel single tap (as opposed to the physical multi-function button,
 * which -- observed on a live M4 -- mutes the microphone entirely LOCALLY,
 * with
 * NO AT/RFCOMM command reaching the AG at all) sends ANY AT command while in
 * a call, and if it does, which one. m4_hfp_events_notify() above only ever
 * fires for the four NAMED verbs it already recognises (CHUP/ATA/BLDN/BVRA);
 * this function is called instead, for every OTHER AT command that would
 * otherwise just fall through to the silent "+CME ERROR: 1" branch, so a
 * live capture can show exactly what (if anything) arrives. The AG's reply
 * to the headset is NOT changed by this call (still the usual ERROR) --
 * unlike the four named verbs, we do not know this command's semantics well
 * enough to claim OK.
 *
 * `raw_at` is the AT command exactly as received (e.g. "AT+CLCC",
 * "AT+XAPL=..."), NUL-terminated, arbitrary length -- this function copies
 * and sanitises it itself (truncates to a small fixed cap, replaces any
 * byte that is not a printable, non-space ASCII character with '_', so the
 * result is always a single whitespace-free token that fits the existing
 * "<VERB> <bdaddr> <ts_ms>"-style wire format used elsewhere in this
 * module). Length-limited (the sanitised/truncated copy) AND rate-limited
 * (drops -- silently, spa_log_debug only -- any call arriving less than
 * `min_interval_ms` ([hfp] raw-min-interval-ms, resolved by the caller --
 * see m4-hfp-config.h -- this file stays config-loader-free) after the last
 * one actually sent, same non-blocking/never-retry/never-crash guarantees as
 * m4_hfp_events_notify() above: this exists purely so a buggy or chatty peer
 * sending AT commands in a tight loop cannot flood hfp-events.sock/
 * momentum4d's log). Wire format: "RAW <sanitised-token> <bdaddr> <ts_ms>"
 * -- a distinct first word from the named-verb "<VERB> <bdaddr> <ts_ms>"
 * lines, so ipc/hfp_events.c's parser can tell them apart unambiguously. */
void m4_hfp_events_notify_raw(struct spa_log *log, const char *raw_at, const char *bdaddr,
		int min_interval_ms);

#ifdef __cplusplus
}
#endif

#endif /* M4_HFP_EVENTS_H */
