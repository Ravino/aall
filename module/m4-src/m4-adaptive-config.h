/* momentum4: aptX Adaptive user config file (~/.config/momentum4/
 * aptx-adaptive.conf).
 * SPDX-License-Identifier: MIT
 *
 * NOT upstream PipeWire. Dependency-free like m4-adaptive-heartbeat.h (no
 * <spa/...>), so it is directly unit-testable with a bare `gcc -std=gnu11`.
 * a2dp-codec-aptx.c is the only SPA-aware
 * caller: it loads this file at codec_init_adaptive() time (config-thread
 * context, not RT) and applies env-var overrides (M4_ADAPTIVE_MODE,
 * M4_ADAPTIVE_LATENCY_ADJUST_MS, M4_ADAPTIVE_QEMU/LOADER/CAPI,
 * M4_ADAPTIVE_HEARTBEAT, M4_ADAPTIVE_RESYNC_PERIOD_MIN) on top, same
 * precedence order everywhere else in this plugin already uses
 * (hardcoded default < config file < environment variable).
 *
 * File format: one `key = value` (or `key value`) per line, '#' starts a
 * comment (rest of line ignored, including after a value), blank lines
 * ignored, keys case-insensitive, '-' and '_' both accepted as separators.
 * Unknown keys and malformed lines are ignored (this is a best-effort
 * convenience file, never a reason to fail codec_init()).
 *
 * Recognized keys:
 *   mode                  ll0 | ll1
 *   latency-adjust-ms     integer, -32768..32767 (see m4-adaptive-heartbeat.h)
 *   heartbeat             on | off
 *   resync-period-min     positive integer (minutes)
 *   helper-mode spawn | socket (default socket:
 *                         qemu-hexagon runs as a systemd --user socket-
 *                         activated service outside WirePlumber, so
 *                         WirePlumber's own MemoryDenyWriteExecute=yes
 *                         hardening never needs to be relaxed (spawning
 *                         qemu-hexagon in-process broke it -- see
 *                         module/test/socket_bridge_client_probe.c).
 *                         "spawn" is the
 *                         original fork+exec design, kept for
 *                         offline tests and for anyone who does not want to
 *                         install the systemd units.)
 *   helper-socket         path to the AF_UNIX socket (helper-mode = socket
 *                         only), default $XDG_RUNTIME_DIR/momentum4/helper.sock
 *   helper-qemu           path to qemu-hexagon(-static) (helper-mode = spawn only)
 *   helper-loader         path to hx_adaptive_loader (helper-mode = spawn only)
 *   helper-capi           path to capi_v2_aptX_CLHDADV_Encoder.so (helper-mode = spawn only)
 *   rt-priority           integer, 0..99. RT priority for the bridge's own
 *                         IO thread (m4-adaptive-bridge.c's io_thread_main(),
 *                         inside WirePlumber's process -- NOT the
 *                         qemu-hexagon helper, which has its own separate
 *                         M4_ADAPTIVE_HELPER_RTPRIO/CPUSchedulingPriority=
 *                         knobs). 0 (default) = leave the IO thread at
 *                         SCHED_OTHER. > 0 = attempt SCHED_FIFO at that
 *                         priority via pthread_setschedparam(); recommended
 *                         to stay below WirePlumber's own data-loop.0
 *                         priority (module-rt default 88, see rt.prio in
 *                         wireplumber.conf) so the audio graph's own RT
 *                         thread is never starved by this one -- e.g. 70.
 *                         Best-effort: EPERM (no RLIMIT_RTPRIO on
 *                         wireplumber.service) is silently tolerated, the
 *                         thread simply stays SCHED_OTHER (see
 *                         "bridge_rt_policy"/"bridge_rt_prio" in the JSON
 *                         stats file for the actually-achieved outcome).
 *   allow-devices         m4 (default) | any: the aptx_adaptive endpoint is
 *                         registered globally in BlueZ (fill_caps() has no
 *                         per-device context, see its own comment in
 *                         a2dp-codec-aptx.c), so as shipped it would happily
 *                         negotiate with ANY peer that advertises vendor
 *                         0x000000D7 / codec 0x00AD, 48 kHz, JOINT-capable --
 *                         not just MOMENTUM 4. "m4" makes
 *                         codec_select_config_adaptive()/
 *                         codec_validate_config_adaptive() additionally
 *                         require the remote capabilities to byte-for-byte
 *                         match the known MOMENTUM 4 fingerprint (TTP LL/HQ/
 *                         TWS bytes, cap_ext_ver, features, setup_pref, EOC --
 *                         see adaptive_caps_match_m4_fingerprint() and its
 *                         own comment for exactly which bytes and why).
 *                         "any" restores the old, loose vendor/codec/rate/
 *                         channel-mode-only check -- only meant for testing
 *                         against a different aptX Adaptive receiver, not a
 *                         safe default (we do not have another receiver's
 *                         real capabilities to test against).
 */
#ifndef SPA_BLUEZ5_M4_ADAPTIVE_CONFIG_H_
#define SPA_BLUEZ5_M4_ADAPTIVE_CONFIG_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "m4-adaptive-heartbeat.h"

#ifdef __cplusplus
extern "C" {
#endif

#define M4_AH_CONFIG_PATH_MAX 512

struct m4_ah_user_config {
	bool have_mode;
	enum m4_ah_mode mode;

	bool have_latency_adjust_ms;
	int32_t latency_adjust_ms;

	bool have_heartbeat;
	bool heartbeat_enabled;

	bool have_resync_period_min;
	int32_t resync_period_min;

	bool have_qemu_path;
	char qemu_path[M4_AH_CONFIG_PATH_MAX];
	bool have_loader_path;
	char loader_path[M4_AH_CONFIG_PATH_MAX];
	bool have_capi_path;
	char capi_path[M4_AH_CONFIG_PATH_MAX];

	/* have_helper_mode/helper_mode_is_socket avoid
	 * pulling in m4-adaptive-bridge.h's enum m4_ah_helper_mode here (this
	 * file stays dependency-free on purpose, see the file header) -- the
	 * caller (a2dp-codec-aptx.c) maps this bool to the real enum. */
	bool have_helper_mode;
	bool helper_mode_is_socket;

	bool have_socket_path;
	char socket_path[M4_AH_CONFIG_PATH_MAX];

	/* "RT for the bridge's IO thread": 0..99, see the "rt-priority" key doc above.
	 * Deliberately int32_t (not m4_ah_bridge_config's plain `int`) for the
	 * same dependency-free-parsing reasons as latency_adjust_ms above; the
	 * caller (a2dp-codec-aptx.c) narrows/validates the range before handing
	 * it to m4_ah_bridge_config.io_thread_rt_priority. */
	bool have_io_rt_priority;
	int32_t io_rt_priority;

	/* "allow-devices": m4 (default) | any. See m4-adaptive-config.h's own
	 * key doc block above this struct for the full rationale (the
	 * caps-fingerprint filter) -- kept here as a single
	 * bool (not an enum) for the same dependency-free-parsing style as
	 * helper_mode_is_socket above: false = "m4" (only accept a remote SEP
	 * whose aptX Adaptive capabilities byte-for-byte match the known
	 * MOMENTUM 4 fingerprint -- TTP/features/setup_pref/EOC bytes, see
	 * adaptive_caps_match_m4_fingerprint() in a2dp-codec-aptx.c), true =
	 * "any" (the original behaviour: any peer that speaks vendor 0x000000D7/
	 * codec 0x00AD, 48 kHz, JOINT-capable is accepted -- useful for testing
	 * against a different aptX Adaptive receiver, NOT the safe default for
	 * a shipped product since we do not know what other receivers' caps
	 * look like. */
	bool have_allow_devices;
	bool allow_devices_any;
};

/* All `have_*` flags false; caller decides what the actual defaults are for
 * anything left unset (kept out of this module so the "what is the default"
 * decision lives in exactly one place, a2dp-codec-aptx.c). */
void m4_ah_user_config_defaults(struct m4_ah_user_config *c);

/* Parses an in-memory buffer (not necessarily NUL-terminated -- length is
 * authoritative). Only ever SETS fields it recognizes a valid value for;
 * never clears a flag that was already set by an earlier call (so callers
 * can layer multiple sources, e.g. a system-wide file then a user file, by
 * calling this twice on the same struct -- not currently done, but kept
 * consistent with the rest of this module's "compose, don't clobber" style). */
void m4_ah_user_config_parse_buffer(struct m4_ah_user_config *c, const char *buf, size_t len);

/* Reads `path` and parses it. Returns false if the file does not exist or
 * cannot be read -- this is the expected, non-error common case (most users
 * will not have created this file), NOT logged as a failure by this module
 * (caller may log at debug level if it wants to). `c` is left untouched on
 * failure. */
bool m4_ah_user_config_load_file(struct m4_ah_user_config *c, const char *path);

/* Fills buf with $M4_ADAPTIVE_CONFIG if set, else
 * $XDG_CONFIG_HOME/momentum4/aptx-adaptive.conf, else
 * $HOME/.config/momentum4/aptx-adaptive.conf. Never fails; if HOME is also
 * unset, writes an empty string (caller's load_file() will then just report
 * "not found", which is fine -- this whole file is
 * optional). */
void m4_ah_user_config_default_path(char *buf, size_t buf_size);

/* Default AF_UNIX socket path for helper-mode = socket,
 * matching ListenStream=%t/momentum4/helper.sock in
 * systemd/momentum4-helper.socket.in (systemd's `%t` specifier is
 * $XDG_RUNTIME_DIR, falling back to /run/user/<uid> if that env var is
 * unset -- mirrored here so a caller that has no $XDG_RUNTIME_DIR in its
 * environment, e.g. a minimal test harness, still resolves to the same
 * path systemd itself would use). Never fails; if neither $XDG_RUNTIME_DIR
 * nor a resolvable uid is available, writes an empty string (caller's
 * m4_ah_bridge_socket_available()/connect() will then just report "not
 * available", consistent with every other best-effort default in this
 * file). */
void m4_ah_user_config_default_socket_path(char *buf, size_t buf_size);

#ifdef __cplusplus
}
#endif

#endif /* SPA_BLUEZ5_M4_ADAPTIVE_CONFIG_H_ */
