/* momentum4: implementation, see m4-adaptive-conf-bridge.h.
 * SPDX-License-Identifier: MIT
 */
#include "m4-adaptive-conf-bridge.h"

#include <stdio.h>
#include <string.h>
#include <strings.h> /* strcasecmp */

#include "m4-conf.h"
#include "m4-device-policy.h"
#include "m4-adaptive-config.h" /* legacy aptx-adaptive.conf parser, m4_ah_user_config */

/* True if NOTHING in [adaptive] was ever set by a real momentum4.conf layer
 * (i.e. every registry entry in that section is still at its compiled-in
 * "default" source) -- see this file's header doc, step 5 ("LEGACY
 * FALLBACK"). The @datadir@ reference copy is fully commented (every key
 * line starts with "#"), so it never actually sets anything even when it
 * exists and gets "loaded" -- only /etc and $XDG_CONFIG_HOME/$HOME
 * momentum4.conf can flip this to false. */
static bool adaptive_section_is_all_default(const m4_conf_t *c)
{
	size_t i;

	for (i = 0; i < m4_conf_registry_count; i++) {
		if (strcmp(m4_conf_registry[i].section, "adaptive") != 0)
			continue;
		if (strcmp(c->entries[i].source, "default") != 0)
			return false;
	}
	return true;
}

/* True if (section,key) is still at its compiled-in default -- i.e. no real
 * momentum4.conf layer (/etc or user, INCLUDING a $MOMENTUM4_CONFIG test
 * override) set it. Guards every single m4_conf_set() call below: the
 * LEGACY FILE AS A WHOLE is only ever consulted when [adaptive] overall
 * looks untouched (adaptive_section_is_all_default(), checked once by the
 * caller before this function is even invoked), but once it IS consulted,
 * an individual key the new momentum4.conf DID set (e.g. a test's
 * $MOMENTUM4_CONFIG setting ONLY `[helper] qemu` to a deliberately-broken
 * path, while leaving every [adaptive] key untouched so the legacy-fallback
 * trigger still fires) must not be silently overwritten by that same
 * legacy file's own value for the SAME key -- per-key precedence, not just
 * per-file. */
static bool still_default(const m4_conf_t *c, const char *section, const char *key)
{
	int idx = m4_conf_find(section, key);
	return idx < 0 || strcmp(c->entries[idx].source, "default") == 0;
}

/* Applies every `have_*` field the legacy m4_ah_user_config parser set onto
 * `c`'s [adaptive]/[helper] entries via m4_conf_set(), each labelled
 * "legacy:<path>" -- but ONLY for a key still_default() (see above). Mirrors
 * the "rt-priority -> io-rt-priority" rename (the OLD file's
 * single "rt-priority" key becomes [adaptive] io-rt-priority in the new
 * schema; [helper] rt-priority is a DIFFERENT, unrelated key the old file
 * never had a way to set at all). */
static void apply_legacy_overlay(m4_conf_t *c, const struct m4_ah_user_config *legacy, const char *path)
{
	char label[M4_AH_ECFG_PATH_MAX + 8];
	char buf[64];

	snprintf(label, sizeof(label), "legacy:%s", path);

	if (legacy->have_mode && still_default(c, "adaptive", "mode"))
		m4_conf_set(c, "adaptive", "mode",
				legacy->mode == M4_AH_MODE_LL1 ? "ll1" : "ll0", label);
	if (legacy->have_latency_adjust_ms && still_default(c, "adaptive", "latency-adjust-ms")) {
		snprintf(buf, sizeof(buf), "%d", legacy->latency_adjust_ms);
		m4_conf_set(c, "adaptive", "latency-adjust-ms", buf, label);
	}
	if (legacy->have_heartbeat && still_default(c, "adaptive", "heartbeat"))
		m4_conf_set(c, "adaptive", "heartbeat", legacy->heartbeat_enabled ? "on" : "off", label);
	if (legacy->have_resync_period_min && still_default(c, "adaptive", "resync-period-min")) {
		snprintf(buf, sizeof(buf), "%d", legacy->resync_period_min);
		m4_conf_set(c, "adaptive", "resync-period-min", buf, label);
	}
	if (legacy->have_io_rt_priority && still_default(c, "adaptive", "io-rt-priority")) {
		snprintf(buf, sizeof(buf), "%d", legacy->io_rt_priority);
		m4_conf_set(c, "adaptive", "io-rt-priority", buf, label);
	}
	if (legacy->have_helper_mode && still_default(c, "adaptive", "helper-mode"))
		m4_conf_set(c, "adaptive", "helper-mode",
				legacy->helper_mode_is_socket ? "socket" : "spawn", label);
	if (legacy->have_allow_devices && still_default(c, "adaptive", "allow-devices"))
		m4_conf_set(c, "adaptive", "allow-devices",
				legacy->allow_devices_any ? "any" : "adk-stereo", label);

	if (legacy->have_socket_path && still_default(c, "helper", "socket"))
		m4_conf_set(c, "helper", "socket", legacy->socket_path, label);
	if (legacy->have_qemu_path && still_default(c, "helper", "qemu"))
		m4_conf_set(c, "helper", "qemu", legacy->qemu_path, label);
	if (legacy->have_loader_path && still_default(c, "helper", "loader"))
		m4_conf_set(c, "helper", "loader", legacy->loader_path, label);
	if (legacy->have_capi_path && still_default(c, "helper", "capi"))
		m4_conf_set(c, "helper", "capi", legacy->capi_path, label);
}

static int allow_devices_resolve(const m4_conf_t *c, struct m4_ah_effective_config *out)
{
	const char *raw = m4_conf_get_string(c, "adaptive", "allow-devices");
	bool valid;
	enum m4_device_policy_mode mode = m4_device_policy_parse_allow_devices(
			raw, out->allow_devices_mac_csv, sizeof(out->allow_devices_mac_csv), &valid);

	out->allow_devices_mode = (int)mode;
	return valid ? 0 : -1;
}

void m4_ah_effective_config_load(struct m4_ah_effective_config *out)
{
	m4_conf_t c;
	struct m4_ah_user_config legacy;
	char legacy_path[M4_AH_ECFG_PATH_MAX];
	const char *s;

	memset(out, 0, sizeof(*out));

	m4_conf_init(&c);
	m4_conf_load_layers(&c);

	m4_ah_user_config_defaults(&legacy);
	m4_ah_user_config_default_path(legacy_path, sizeof(legacy_path));
	if (adaptive_section_is_all_default(&c) &&
			m4_ah_user_config_load_file(&legacy, legacy_path)) {
		apply_legacy_overlay(&c, &legacy, legacy_path);
		out->used_legacy_file = true;
		snprintf(out->legacy_path, sizeof(out->legacy_path), "%s", legacy_path);
	}

	/* [adaptive] */
	s = m4_conf_get_string(&c, "adaptive", "mode");
	out->mode = (s && !strcasecmp(s, "ll1")) ? M4_AH_MODE_LL1 : M4_AH_MODE_LL0;
	out->latency_adjust_ms = (int32_t)m4_conf_get_int(&c, "adaptive", "latency-adjust-ms", -15);
	out->heartbeat_enabled = m4_conf_get_bool(&c, "adaptive", "heartbeat", true);
	out->resync_period_min = (int32_t)m4_conf_get_int(&c, "adaptive", "resync-period-min", 10);
	out->io_rt_priority = (int32_t)m4_conf_get_int(&c, "adaptive", "io-rt-priority", 70);
	s = m4_conf_get_string(&c, "adaptive", "helper-mode");
	out->helper_mode_is_socket = !(s && !strcasecmp(s, "spawn"));
	out->reconnect_grace_ms = (int32_t)m4_conf_get_int(&c, "adaptive", "reconnect-grace-ms", 1500);
	out->stall_ms = m4_conf_get_double(&c, "adaptive", "stall-ms", 50.0);
	out->warmup_blocks = (int32_t)m4_conf_get_int(&c, "adaptive", "warmup-blocks", 20);
	out->btq_threshold_ms = m4_conf_get_double(&c, "adaptive", "btq-threshold-ms", 30.0);
	out->fade_in_enabled = m4_conf_get_bool(&c, "adaptive", "fade-in", true);
	out->fade_in_frames = (int32_t)m4_conf_get_int(&c, "adaptive", "fade-in-frames", 240);
	out->ring_slots = (int32_t)m4_conf_get_int(&c, "adaptive", "ring-slots", 8);
	out->ring_target_frames = (int32_t)m4_conf_get_int(&c, "adaptive", "ring-target-frames", 1);
	out->ring_trim_sustain_ms = m4_conf_get_double(&c, "adaptive", "ring-trim-sustain-ms", 200.0);
	out->ring_trim_hard_cap_ms = m4_conf_get_double(&c, "adaptive", "ring-trim-hard-cap-ms", 1000.0);
	out->ring_quiet_amp = (int32_t)m4_conf_get_int(&c, "adaptive", "ring-quiet-amp", 103);
	out->helper_frame_timeout_ms = (int32_t)m4_conf_get_int(&c, "adaptive", "helper-frame-timeout-ms", 200);
	out->helper_startup_timeout_ms = (int32_t)m4_conf_get_int(&c, "adaptive", "helper-startup-timeout-ms", 5000);
	out->helper_connect_timeout_ms = (int32_t)m4_conf_get_int(&c, "adaptive", "helper-connect-timeout-ms", 1000);
	out->helper_restart_backoff_min_ms = (int32_t)m4_conf_get_int(&c, "adaptive", "helper-restart-backoff-min-ms", 500);
	out->helper_restart_backoff_max_ms = (int32_t)m4_conf_get_int(&c, "adaptive", "helper-restart-backoff-max-ms", 10000);
	out->helper_autorestart = m4_conf_get_bool(&c, "adaptive", "helper-autorestart", true);
	out->helper_stop_grace_ms = (int32_t)m4_conf_get_int(&c, "adaptive", "helper-stop-grace-ms", 300);
	out->idle_poll_ms = (int32_t)m4_conf_get_int(&c, "adaptive", "idle-poll-ms", 200);
	out->enabled = m4_conf_get_bool(&c, "adaptive", "enable", true);
	allow_devices_resolve(&c, out);

	/* [helper] */
	snprintf(out->helper_socket_path, sizeof(out->helper_socket_path), "%s",
			m4_conf_get_string(&c, "helper", "socket"));
	snprintf(out->helper_qemu_path, sizeof(out->helper_qemu_path), "%s",
			m4_conf_get_string(&c, "helper", "qemu"));
	snprintf(out->helper_loader_path, sizeof(out->helper_loader_path), "%s",
			m4_conf_get_string(&c, "helper", "loader"));
	snprintf(out->helper_capi_path, sizeof(out->helper_capi_path), "%s",
			m4_conf_get_string(&c, "helper", "capi"));
	out->helper_rt_priority = (int32_t)m4_conf_get_int(&c, "helper", "rt-priority", 10);
	out->helper_nice = (int32_t)m4_conf_get_int(&c, "helper", "nice", -5);
	snprintf(out->helper_cpu_list, sizeof(out->helper_cpu_list), "%s",
			m4_conf_get_string(&c, "helper", "cpu-list"));
	out->helper_start_mute_pkts = (int32_t)m4_conf_get_int(&c, "helper", "start-mute-pkts", 0);
	snprintf(out->helper_loader_env, sizeof(out->helper_loader_env), "%s",
			m4_conf_get_string(&c, "helper", "loader-env"));
	out->helper_debug_log = m4_conf_get_bool(&c, "helper", "debug-log", false);

	/* [stats] */
	out->stats_enable = m4_conf_get_bool(&c, "stats", "enable", true);
	snprintf(out->stats_path, sizeof(out->stats_path), "%s",
			m4_conf_get_string(&c, "stats", "path"));
	snprintf(out->stats_history_path, sizeof(out->stats_history_path), "%s",
			m4_conf_get_string(&c, "stats", "history-path"));
	out->stats_interval_ms = (int32_t)m4_conf_get_int(&c, "stats", "interval-ms", 10000);
	out->stats_history_lines = (int32_t)m4_conf_get_int(&c, "stats", "history-lines", 360);
	out->stats_history_rotate_every = (int32_t)m4_conf_get_int(&c, "stats", "history-rotate-every", 30);

	out->n_warnings = c.n_warnings;
}
