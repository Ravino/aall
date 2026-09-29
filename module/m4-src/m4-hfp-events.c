/* momentum4: implementation, see m4-hfp-events.h for the full rationale.
 * SPDX-License-Identifier: MIT
 */
#include "m4-hfp-events.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

/* Resolves "$XDG_RUNTIME_DIR/momentum4/hfp-events.sock" (or the
 * "/run/user/<uid>/..." fallback) into `buf`. Deliberately duplicated
 * rather than shared with m4_ah_user_config_default_socket_path() in
 * m4-adaptive-config.c (same rule, different socket name): that function
 * lives in the aptX Adaptive codec library (spa-codec-bluez5-aptx.so) and
 * pulls in config-file parsing this module -- which lives in the *native
 * backend* library (spa-bluez5.so) instead, a different shared object --
 * has no other reason to depend on. Keeping this self-contained also makes
 * it trivial to unit-test without linking the rest of bluez5. */
/* [hfp] events-socket override, set by
 * m4_hfp_events_set_socket_path() -- empty means "no override, use the
 * built-in XDG default below". Sized to sockaddr_un.sun_path exactly (not
 * some larger round number): every caller of hfp_events_socket_path() below
 * only ever has a buffer of that same size to fill anyway (see
 * m4_hfp_events_notify()/_raw()'s own `char path[sizeof(addr.sun_path)]`),
 * and a longer override could never be used regardless -- both callers
 * already drop the event if the resolved path does not fit. Matching the
 * sizes exactly (rather than a larger buffer truncated at copy time) also
 * avoids a spurious -Wformat-truncation from a stricter optimization level
 * being unable to prove the copy below never loses data. */
static char g_socket_path_override[108 /* sizeof(((struct sockaddr_un *)0)->sun_path) */];

void m4_hfp_events_set_socket_path(const char *path)
{
	if (path == NULL || *path == '\0') {
		g_socket_path_override[0] = '\0';
		return;
	}
	snprintf(g_socket_path_override, sizeof(g_socket_path_override), "%s", path);
}

static void hfp_events_socket_path(char *buf, size_t buf_size)
{
	const char *xdg_runtime;

	if (buf_size == 0)
		return;
	buf[0] = '\0';

	if (g_socket_path_override[0] != '\0') {
		snprintf(buf, buf_size, "%s", g_socket_path_override);
		return;
	}

	xdg_runtime = getenv("XDG_RUNTIME_DIR");
	if (xdg_runtime && *xdg_runtime) {
		snprintf(buf, buf_size, "%s/momentum4/hfp-events.sock", xdg_runtime);
		return;
	}
	snprintf(buf, buf_size, "/run/user/%u/momentum4/hfp-events.sock", (unsigned)getuid());
}

/* Lazily opened, process-wide singleton fd -- there is exactly one bluez5
 * native backend instance per WirePlumber process, so a single static is
 * equivalent to (and simpler than) threading a field through struct impl.
 * Never explicitly closed: process exit closes it, and the native backend
 * is never torn down and re-created within one WirePlumber process
 * lifetime (module.c loads spa.bluez5.native once, see bluez5-dbus.c).
 * g_state: 0 = not attempted yet, 1 = g_fd usable, -1 = socket() failed
 * once, permanently given up (avoids retrying a syscall that already told
 * us it will not succeed, e.g. EMFILE, on every single incoming AT
 * command). */
static int g_fd = -1;
static int g_state;

static int ensure_socket(struct spa_log *log)
{
	if (g_state != 0)
		return g_state == 1;

	g_fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (g_fd < 0) {
		spa_log_debug(log, "momentum4: hfp-events: socket() failed: %s",
				strerror(errno));
		g_state = -1;
		return 0;
	}
	g_state = 1;
	return 1;
}

void m4_hfp_events_notify(struct spa_log *log, const char *verb, const char *bdaddr)
{
	struct sockaddr_un addr;
	char path[sizeof(addr.sun_path)];
	char msg[192];
	struct timespec ts;
	long long ts_ms;
	size_t path_len;
	int len;

	if (verb == NULL || *verb == '\0')
		return;
	if (!ensure_socket(log))
		return;

	hfp_events_socket_path(path, sizeof(path));
	path_len = strlen(path);
	if (path_len == 0)
		return;
	if (path_len >= sizeof(addr.sun_path)) {
		/* AF_UNIX path length limit (~108 bytes) -- fail closed rather
		 * than silently truncate into some other, wrong path. Cannot
		 * happen with the fixed "<runtime dir>/momentum4/hfp-events.sock"
		 * suffix unless $XDG_RUNTIME_DIR itself is unusually long. */
		spa_log_debug(log, "momentum4: hfp-events: socket path too long (%zu bytes), dropping event",
				path_len);
		return;
	}

	/* CLOCK_REALTIME, milliseconds: correlation/debug aid for the daemon
	 * and its logs, not used for any timing decision on this side. */
	clock_gettime(CLOCK_REALTIME, &ts);
	ts_ms = (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;

	len = snprintf(msg, sizeof(msg), "%s %s %lld", verb,
			(bdaddr && *bdaddr) ? bdaddr : "00:00:00:00:00:00", ts_ms);
	if (len <= 0)
		return;
	if ((size_t)len >= sizeof(msg))
		len = (int)sizeof(msg) - 1; /* truncated, still a well-formed line; never happens with our fixed verbs */

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	memcpy(addr.sun_path, path, path_len);

	if (sendto(g_fd, msg, (size_t)len, MSG_DONTWAIT | MSG_NOSIGNAL,
			(struct sockaddr *)&addr, sizeof(addr)) < 0) {
		/* ENOENT/ECONNREFUSED (momentum4d not running / stale socket
		 * file) and EAGAIN/ENOBUFS (receiver not draining fast enough)
		 * are expected steady state, not a bug -- debug only, never
		 * warn, never retry (SOCK_DGRAM has no connection state to
		 * retry: the very next call just tries again on its own). */
		spa_log_debug(log, "momentum4: hfp-events: sendto(%s) failed: %s",
				path, strerror(errno));
	}
}

/* Length: see m4-hfp-events.h's doc comment on m4_hfp_events_notify_raw() --
 * a fixed, small cap, independent of whatever `raw_at` the peer sent (no
 * unbounded copy of attacker/device-controlled input, ever). */
#define M4_HFP_EVENTS_RAW_MAX_TOKEN 48
/* CLOCK_MONOTONIC milliseconds, wrapping (int64 -- does not matter, only
 * ever used as a difference between two nearby readings). */
static long long raw_now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Process-wide (same reasoning as g_fd/g_state above: one native backend
 * instance per WirePlumber process) -- 0 means "never sent yet", so the
 * very first raw event is never itself rate-limited. */
static long long g_raw_last_sent_ms;

void m4_hfp_events_notify_raw(struct spa_log *log, const char *raw_at, const char *bdaddr,
		int min_interval_ms)
{
	struct sockaddr_un addr;
	char path[sizeof(addr.sun_path)];
	char token[M4_HFP_EVENTS_RAW_MAX_TOKEN + 1];
	char msg[192];
	struct timespec ts;
	long long ts_ms, now_ms;
	size_t path_len, i, n;
	int len;

	if (raw_at == NULL || *raw_at == '\0')
		return;
	if (min_interval_ms < 0)
		min_interval_ms = 0;

	now_ms = raw_now_ms();
	if (g_raw_last_sent_ms != 0 && now_ms - g_raw_last_sent_ms < min_interval_ms) {
		spa_log_debug(log, "momentum4: hfp-events: raw AT capture rate-limited, dropping: %s", raw_at);
		return;
	}

	if (!ensure_socket(log))
		return;

	hfp_events_socket_path(path, sizeof(path));
	path_len = strlen(path);
	if (path_len == 0 || path_len >= sizeof(addr.sun_path))
		return;

	/* Sanitise: printable, non-space ASCII only, '_' for everything else
	 * (control bytes, whitespace, high-bit-set garbage) -- guarantees the
	 * wire line ipc/hfp_events.c's parser reads back is always exactly
	 * one whitespace-free token, regardless of what the peer actually
	 * sent (a real AT command never contains whitespace, but this is not
	 * trusted input -- backend-native.c already NUL-terminates `buf`
	 * before this is ever called, see its own RFCOMM line-framing, but
	 * its *content* is whatever bytes arrived over Bluetooth). */
	n = 0;
	for (i = 0; raw_at[i] != '\0' && n < M4_HFP_EVENTS_RAW_MAX_TOKEN; i++) {
		unsigned char c = (unsigned char)raw_at[i];

		token[n++] = (c > 0x20 && c < 0x7f) ? (char)c : '_';
	}
	token[n] = '\0';
	if (n == 0)
		return;

	clock_gettime(CLOCK_REALTIME, &ts);
	ts_ms = (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;

	len = snprintf(msg, sizeof(msg), "RAW %s %s %lld", token,
			(bdaddr && *bdaddr) ? bdaddr : "00:00:00:00:00:00", ts_ms);
	if (len <= 0)
		return;
	if ((size_t)len >= sizeof(msg))
		len = (int)sizeof(msg) - 1;

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	memcpy(addr.sun_path, path, path_len);

	if (sendto(g_fd, msg, (size_t)len, MSG_DONTWAIT | MSG_NOSIGNAL,
			(struct sockaddr *)&addr, sizeof(addr)) < 0) {
		spa_log_debug(log, "momentum4: hfp-events: sendto(%s) failed (raw): %s",
				path, strerror(errno));
		return; /* do NOT update g_raw_last_sent_ms on a failed send */
	}
	g_raw_last_sent_ms = now_ms;
}
