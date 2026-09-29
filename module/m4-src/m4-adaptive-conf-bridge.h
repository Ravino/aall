/* momentum4: the aptx_adaptive SPA codec's
 * ONE entry point into the shared momentum4.conf parser (common/m4-conf.c,
 * copied verbatim into this build tree by prepare-tree.sh -- see that
 * script's own "copy shared common/ config files" step). Replaces the
 * ad hoc per-call-site `getenv()` + m4_ah_user_config (m4-adaptive-config.h)
 * reads a2dp-codec-aptx.c used to do directly (the helper's socket path
 * was specified twice, etc.) with ONE resolved snapshot covering
 * every [adaptive]/[helper]/[stats] key in common/m4-conf-keys.def.
 * SPDX-License-Identifier: MIT
 *
 * Layering (identical to common/m4-conf.h's own doc comment, this file adds
 * nothing new here except the legacy step):
 *   1. compiled-in defaults (common/m4-conf-keys.def)
 *   2. @datadir@/momentum4/momentum4.conf
 *   3. /etc/momentum4/momentum4.conf
 *   4. $XDG_CONFIG_HOME/momentum4/momentum4.conf, else
 *      $HOME/.config/momentum4/momentum4.conf ($MOMENTUM4_CONFIG replaces
 *      this step's path -- tests only, see below)
 * 5. LEGACY FALLBACK (this file's own addition; "legacy config is
 *      read if there is no new one"): if, after step 4,
 *      EVERY [adaptive] key in the registry is still at its "default" source
 *      (i.e. neither the /etc nor the ~/.config momentum4.conf -- if either
 *      even exists -- ever set an [adaptive] key), the OLD flat, no-sections
 *      ~/.config/momentum4/aptx-adaptive.conf (m4-adaptive-config.h's
 *      grammar, unchanged, still read with THAT file's own parser so nothing
 *      about how it is read changes) is loaded and every field it actually
 *      set is applied on top via m4_conf_set(), each with source label
 *      "legacy:<path>" -- so `momentum4 config show`-equivalent diagnostics
 *      (and this file's own m4_ah_effective_config.used_legacy_file below)
 *      can tell a value came from there. One warning is logged (by the
 *      CALLER, via the `log`/`legacy_path` fields below -- this file itself
 *      has no spa_log dependency, matching m4-adaptive-config.c's existing
 *      "dependency-free" contract) the first time this happens per process,
 *      not once per codec_init() (aptx-adaptive.conf is re-read on every
 *      reconnect already, like everything else in this function -- codec
 *      init is not RT -- but a log line on every single reconnect would be
 *      noise for something that, once true, stays true until the user
 *      migrates).
 *   [hfp]/[helper cpu-list etc.]/anything outside [adaptive]/[helper]/
 *   [stats] is irrelevant to this codec and simply ignored (common/m4-conf.c
 *   loads and validates the WHOLE file regardless -- a typo in an unrelated
 *   [hfp] key still only produces one warning entry, never blocks this
 *   codec's own keys).
 *
 * $MOMENTUM4_CONFIG is intentionally the ONLY environment-variable knob this
 * file itself consults (via common/m4-conf.c's own m4_conf_load_layers()) --
 * every per-key M4_ADAPTIVE_..., M4_AH_..., M4_HFP_... environment variable
 * a2dp-codec-aptx.c/m4-adaptive-bridge.c used to read directly is GONE:
 * environment overrides are for tests only; $MOMENTUM4_CONFIG pointed at a
 * throwaway temp file *is* that test-only mechanism, exactly as
 * common/m4-conf.h already documents for the
 * daemon side).
 */
#ifndef SPA_BLUEZ5_M4_ADAPTIVE_CONF_BRIDGE_H_
#define SPA_BLUEZ5_M4_ADAPTIVE_CONF_BRIDGE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "m4-adaptive-heartbeat.h" /* enum m4_ah_mode */

#ifdef __cplusplus
extern "C" {
#endif

#define M4_AH_ECFG_PATH_MAX 512

struct m4_ah_effective_config {
	/* --- [adaptive] --- */
	enum m4_ah_mode mode;
	int32_t latency_adjust_ms;
	bool heartbeat_enabled;
	int32_t resync_period_min;
	int32_t io_rt_priority;          /* 0..99, 0 = SCHED_OTHER */
	bool helper_mode_is_socket;      /* false = spawn */
	int32_t reconnect_grace_ms;
	double stall_ms;
	int32_t warmup_blocks;
	double btq_threshold_ms;
	bool fade_in_enabled;
	int32_t fade_in_frames;
	int32_t ring_slots;              /* 1..M4_AH_RING_SLOTS_MAX (see m4-adaptive-bridge.h) */
	int32_t ring_target_frames;
	double ring_trim_sustain_ms;
	double ring_trim_hard_cap_ms;
	int32_t ring_quiet_amp;
	int32_t helper_frame_timeout_ms;
	int32_t helper_startup_timeout_ms;
	int32_t helper_connect_timeout_ms;
	int32_t helper_restart_backoff_min_ms;
	int32_t helper_restart_backoff_max_ms;
	bool helper_autorestart;
	int32_t helper_stop_grace_ms;
	int32_t idle_poll_ms;
	bool enabled;                    /* [adaptive] enable -- false disables the whole codec */

	/* allow-devices -- already parsed via
	 * m4_device_policy_parse_allow_devices() (m4-device-policy.h), NOT the
	 * raw string: `allow_devices_mode` is one of M4_DEVICE_POLICY_ANY,
	 * M4_DEVICE_POLICY_FINGERPRINT or M4_DEVICE_POLICY_MAC_LIST (never OFF/
	 * IDENTITY -- those only apply to [hfp] keys); `allow_devices_mac_csv`
	 * is only meaningful when allow_devices_mode == M4_DEVICE_POLICY_MAC_LIST. */
	int allow_devices_mode; /* enum m4_device_policy_mode, kept as `int` here so this
				  * dependency-free-ish header does not have to include
				  * m4-device-policy.h just for the enum tag (matches this
				  * file's existing style of keeping enums it does not
				  * otherwise need out of its own #include list) -- callers
				  * cast to/compare against enum m4_device_policy_mode. */
	char allow_devices_mac_csv[M4_AH_ECFG_PATH_MAX];

	/* --- [helper] --- */
	char helper_socket_path[M4_AH_ECFG_PATH_MAX];
	char helper_qemu_path[M4_AH_ECFG_PATH_MAX];
	char helper_loader_path[M4_AH_ECFG_PATH_MAX];
	char helper_capi_path[M4_AH_ECFG_PATH_MAX];
	int32_t helper_rt_priority;      /* SPAWN mode only */
	int32_t helper_nice;             /* SPAWN mode only */
	char helper_cpu_list[128];       /* SPAWN mode only, "" = do not set affinity */
	int32_t helper_start_mute_pkts;
	char helper_loader_env[256];     /* "" = none, debug/RE only */
	bool helper_debug_log;

	/* --- [stats] --- */
	bool stats_enable;
	char stats_path[M4_AH_ECFG_PATH_MAX];
	char stats_history_path[M4_AH_ECFG_PATH_MAX];
	int32_t stats_interval_ms;
	int32_t stats_history_lines;
	int32_t stats_history_rotate_every;

	/* diagnostics, not consumed by encode/bridge logic */
	bool used_legacy_file;
	char legacy_path[M4_AH_ECFG_PATH_MAX]; /* only meaningful if used_legacy_file */
	int n_warnings;   /* common/m4-conf.c warnings (unknown key/section/out-of-range) */
};

/* Loads the full layered config (see this header's own doc comment above)
 * into `out`. Never fails -- `out` is always fully populated with, at worst,
 * every compiled-in default from common/m4-conf-keys.def. Not RT-safe (does
 * file I/O -- fopen/fread on up to 4 files); every existing call site in
 * a2dp-codec-aptx.c already only calls the equivalent of this at
 * codec_init()/codec_fill_caps() time, never from encode(). */
void m4_ah_effective_config_load(struct m4_ah_effective_config *out);

#ifdef __cplusplus
}
#endif

#endif /* SPA_BLUEZ5_M4_ADAPTIVE_CONF_BRIDGE_H_ */
