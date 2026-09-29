/* ipc/ctl.h -- $runtime_dir/ctl.sock control protocol: a ctl socket with
 * status, call on|off, mode auto|manual|observe, get commands.
 *
 * Wire protocol (deliberately the simplest thing that could work -- an
 * earlier design sketch suggested JSON-lines; this project has no JSON *writer* dependency
 * anywhere yet, spa/utils/json.h (already used by metrics/stats.c) is a
 * parser only, and every other IPC-ish thing in this codebase, including
 * core/state.h's own call-state file, is plain KEY=value text -- so this follows
 * that convention instead of adding one): ONE request per connection.
 *   - Client connects, writes one line ("STATUS\n" / "GET\n" / "CALL ON\n" /
 *     "CALL OFF\n" / "MODE AUTO\n" / "MODE MANUAL\n" / "MODE OBSERVE\n" /
 * "REPAIR_A2DP\n" -- gated exactly like `CALL
 *     ON`/`CALL OFF`, see daemon/daemon.h's write-gating matrix comment),
 *     then shuts down its write side (or just stops writing -- the server
 *     reads up to the first '\n' or M4_CTL_MAX_LINE bytes, whichever comes
 *     first).
 *   - Server responds with "OK\n" or "ERR <message>\n", optionally followed
 *     by zero or more "key=value\n" lines (STATUS/GET only), then closes
 *     the connection -- end of response is end of connection, the client
 *     just reads until EOF.
 *   - "GET" returns machine-oriented key=value lines only (mode=.../
 *     call_active=.../call_reason=.../mic_active=.../frozen=.../
 *     m4_present=.../m4_connected=.../profile=.../switches_in_window=.../
 *     safe_latched=...) for scripts/tests. "STATUS" is meant for a human
 *     (Russian text, one summary line) -- see daemon/daemon.c's
 *     m4_daemon_handle_request() for exactly what each produces; this
 *     header only defines the transport, not the semantics of any specific
 *     command (deliberately -- ipc/ctl.c has zero daemon-logic
 *     dependencies, same separation as pw/loop_glue.c).
 *
 * Concurrency model (explicit limitation, not an oversight): the server
 * handles ONE ctl.sock connection at a time, synchronously, inside whatever
 * cb the caller supplied -- accept() -> SO_PEERCRED uid check -> blocking
 * read (bounded by SO_RCVTIMEO) -> cb() -> blocking write (bounded by
 * SO_SNDTIMEO) -> close. This is safe and simple for a local control socket
 * with a handful of trusted CLI-process clients issuing short requests, and
 * matters here specifically because cb() (in practice, daemon/daemon.c's
 * dispatcher) may itself pump the SAME sd_event reentrantly while a `call
 * on/off` is waiting for profile confirmation (pw/callfsm_graph_adapter.c) --
 * a second ctl.sock connection accepted mid-wait would nest another cb()
 * call, which is why daemon/daemon.c additionally guards with its own
 * "one action in flight" flag (see that file) rather than relying on this
 * transport to serialize anything on its own.
 */
#ifndef MOMENTUM4_IPC_CTL_H
#define MOMENTUM4_IPC_CTL_H

#include <stdbool.h>
#include <stddef.h>

#include <systemd/sd-event.h>

#ifdef __cplusplus
extern "C" {
#endif

#define M4_CTL_MAX_LINE 512
#define M4_CTL_MAX_RESPONSE 4096

/* `request` is NUL-terminated, trailing '\n'/'\r' already stripped, never
 * empty (the server does not invoke cb for a blank line -- responds "ERR
 * empty request" itself). Must write a complete response (see this header's
 * protocol description) into `response` (capacity `response_cap`,
 * NUL-terminated on return) -- if cb does not write anything, the server
 * sends "ERR empty response" instead of an empty reply, treated as a
 * server-side bug, not a client error. */
typedef void (*m4_ctl_request_cb)(void *user_data, const char *request,
				   char *response, size_t response_cap);

struct m4_ctl_server;

/* Binds and listens on `socket_path` (AF_UNIX, SOCK_STREAM), removing a
 * stale socket file at that path first if present (same "we own this path,
 * a leftover file from a crashed previous instance is not a conflict"
 * assumption as every socket-activated unit in this project -- this daemon
 * is NOT socket-activated itself, it owns creating this file directly,
 * protected by the flock in daemon/daemon.c so only one instance ever gets
 * this far). Integrates into `event` (must outlive the returned server).
 * Returns NULL on failure (path too long, bind/listen failed, ...) --
 * check errno/the journal for why, m4_log() already logged a WARN. */
struct m4_ctl_server *m4_ctl_server_open(sd_event *event, const char *socket_path,
					   m4_ctl_request_cb cb, void *user_data);

/* Closes the listening socket and unlinks socket_path. Safe with NULL. */
void m4_ctl_server_close(struct m4_ctl_server *srv);

/* Client side (used by the cli/ subcommands): connects to `socket_path` (POSIX SOCK_STREAM
 * connect() itself has no adjustable timeout for AF_UNIX -- it either
 * succeeds, or fails immediately with ECONNREFUSED/ENOENT if nothing is
 * listening, which is the only failure mode that matters here: "is the
 * daemon running"), sends `request_line` + "\n", reads the response (up to
 * response_cap-1 bytes, until EOF), NUL-terminates it into `response`.
 * `timeout_ms` bounds the read/write (SO_RCVTIMEO/SO_SNDTIMEO on the
 * client's own socket) -- protects the CLI from a hung/misbehaving daemon,
 * matching I9's spirit (the CLI polls on its own if the daemon's socket has
 * not answered within 1s) even though *this* task keeps the actual I9 fallback logic in
 * cli/status.c, not here. Returns 0 on success, -1 on any failure (daemon
 * not running, timeout, oversized response, ...) -- callers only need to
 * distinguish "got a response" from "could not talk to the daemon at all",
 * matching cli/call.c's/cli/mode.c's contract (report
 * an error with a hint when the daemon is unreachable). */
int m4_ctl_client_request(const char *socket_path, const char *request_line,
			   char *response, size_t response_cap, int timeout_ms);

/* Shared response-parsing helper (used by cli/call.c, cli/mode.c,
 * cli/status.c -- all three read a "key=value\n" line out of a response
 * this transport delivered). Finds the FIRST line starting with "key=" in
 * `response` and copies whatever follows up to the next '\n' (or end of
 * string) into `out`, NUL-terminated. Returns true on a match, false if
 * the key was not found (out left untouched) or `out_cap` was too small
 * for the value (out left untouched -- responses here are at most
 * M4_CTL_MAX_RESPONSE bytes of trusted, self-generated text, not user
 * input, so truncation would only ever hide a real bug rather than handle
 * one gracefully). */
bool m4_ctl_response_field(const char *response, const char *key, char *out, size_t out_cap);

#ifdef __cplusplus
}
#endif

#endif /* MOMENTUM4_IPC_CTL_H */
