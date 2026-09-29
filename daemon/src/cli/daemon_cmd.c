/* cli/daemon_cmd.c -- see daemon_cmd.h. */
#include "daemon_cmd.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <systemd/sd-bus.h>
#include <systemd/sd-event.h>

#include "../bt/bt_ops.h"
#include "../core/callfsm.h"
#include "../core/graph_ops.h"
#include "../daemon/daemon.h"
#include "../pw/micdetect.h"
#include "../pw/repair_a2dp.h"
#include "../util/config.h"
#include "../util/earcon.h"
#include "../util/log.h"
#include "../util/settings.h"
#include "../../../common/m4-i18n.h"

/* 5000 ms
 * was too short for an HSP/HFP profile switch -- unlike a naive poll loop
 * (which could check the OPTIMISTIC, synchronously
 * updated SPA_PARAM_Profile value directly via a fresh `pactl`/enum_params
 * call every 0.5s), pw/callfsm_graph_adapter.c's confirm loop only learns
 * about the switch via a PUSHED info event that bluez5-device.c fires only
 * once BlueZ's async HFP codec negotiation (SCO setup + AT+BCS handshake)
 * actually completes -- which can genuinely take longer than a few
 * seconds. Bumped to 10s (was 5s) as a conservative fix; combined with
 * core/callfsm.c's confirm_or_verify() cross-check (same root cause), which
 * catches a switch that lands just after this window elapses without
 * re-issuing a second, competing profile-switch request. Was a compiled
 * #define (10000); now `[daemon] profile-confirm-timeout-ms` -- registry
 * default is the same 10000, so nothing changes unless the operator
 * overrides it. */

static void production_earcon(void *ctx, const char *dir, const char *kind)
{
	(void)ctx;
	(void)m4_earcon_play(dir, kind);
}

static void on_player_event(void *user_data, const struct m4_player_event *ev)
{
	m4_daemon_player_event(user_data, ev);
}

/* our MPRIS player in BlueZ (system bus). Any failure
 * here only disables gestures -- the daemon keeps running. */
static struct m4_mpris_player *start_player(sd_event *event, struct m4_daemon *d,
					    const struct m4_settings *settings, sd_bus **bus_out)
{
	struct m4_mpris_player_config pcfg = {0};
	char adapter_path[64];
	char device_path[128] = "";
	const char *mac = m4_config_mac_address();
	sd_bus *bus = NULL;
	struct m4_mpris_player *player;
	int r;

	*bus_out = NULL;
	r = sd_bus_open_system(&bus);
	if (r < 0) {
		m4_log(M4_LOG_WARN, M4_T("momentum4d: the system D-Bus bus is unavailable (%s) -- M4 gestures disabled",
"momentum4d: системная шина D-Bus недоступна (%s) -- жесты M4 отключены"),
		       strerror(-r));
		return NULL;
	}
	r = sd_bus_attach_event(bus, event, SD_EVENT_PRIORITY_NORMAL);
	if (r < 0) {
		m4_log(M4_LOG_WARN, M4_T("momentum4d: sd_bus_attach_event: %s -- M4 gestures disabled",
"momentum4d: sd_bus_attach_event: %s -- жесты M4 отключены"), strerror(-r));
		sd_bus_flush_close_unref(bus);
		return NULL;
	}
	snprintf(adapter_path, sizeof(adapter_path), "/org/bluez/%s", m4_config_bt_adapter());
	pcfg.bus = bus;
	pcfg.adapter_path = adapter_path;
	pcfg.initial_status = m4_player_status_initial(settings->player_status);
	/* player status after reconnect */
	if (mac != NULL && strlen(mac) == 17) {
		int n = snprintf(device_path, sizeof(device_path), "%s/dev_%s", adapter_path, mac);

		for (int i = n - 17; i < n; i++)
			if (device_path[i] == ':')
				device_path[i] = '_';
	}
	pcfg.device_path = device_path; /* "" -> any device on the adapter */
	pcfg.auto_resync = settings->player_resync != M4_PLAYER_RESYNC_OFF;
	pcfg.cb = on_player_event;
	pcfg.user_data = d;
	player = m4_mpris_player_new(event, &pcfg);
	if (player == NULL) {
		sd_bus_flush_close_unref(bus);
		return NULL;
	}
	*bus_out = bus;
	return player;
}

int m4_cli_daemon_run(int argc, char **argv)
{
	char runtime_dir[480];
	char state_dir[480];
	char stats_path[512];
	char sock_path[512];
	const char *mac = m4_config_mac_address();
	int lock_fd;
	sd_event *event = NULL;
	struct m4_daemon_config cfg = {0};
	struct m4_daemon *d = NULL;
	struct m4_settings settings;
	char config_path[512];
	int settings_rc;
	struct m4_passthrough_sdbus *pt = NULL;
	struct m4_passthrough_ops pt_ops;
	struct m4_mpris_player *player = NULL;
	sd_bus *system_bus = NULL;
	int rc;

	(void)argc;
	(void)argv;

	if (m4_config_runtime_dir(runtime_dir, sizeof(runtime_dir)) == NULL) {
		fprintf(stderr, M4_T("momentum4 daemon: no $XDG_RUNTIME_DIR -- nowhere to write ctl.sock/call-state\n",
"momentum4 daemon: нет $XDG_RUNTIME_DIR -- некуда писать ctl.sock/call-state\n"));
		return 1;
	}
	if (m4_config_ctl_socket_path(sock_path, sizeof(sock_path)) == NULL) {
		fprintf(stderr, M4_T("momentum4 daemon: failed to build the ctl.sock path\n",
"momentum4 daemon: не удалось построить путь ctl.sock\n"));
		return 1;
	}
	if (m4_config_stats_path(stats_path, sizeof(stats_path)) == NULL)
		stats_path[0] = '\0';
	/* mic-on-enter: `[paths] state-dir` -- best-effort, NOT fatal if
	 * unavailable ($XDG_STATE_HOME/$HOME both unset), unlike runtime_dir
	 * above (`remember`'s reboot-survival half is simply unavailable then,
	 * see daemon.h's own doc comment on cfg.state_dir). */
	if (m4_config_state_dir(state_dir, sizeof(state_dir)) == NULL)
		state_dir[0] = '\0';

	lock_fd = m4_daemon_acquire_lock(runtime_dir);
	if (lock_fd < 0)
		return 1; /* m4_daemon_acquire_lock() already logged why */

	if (sd_event_new(&event) < 0) {
		m4_log(M4_LOG_ERR, M4_T("momentum4d: sd_event_new() failed",
"momentum4d: sd_event_new() не удался"));
		return 1;
	}

	{
		/* every one of
		 * these used to be a compiled #define or an unconditional
		 * literal -- see m4-conf-keys.def for each key's own default
		 * (identical to the number/string this file used to hardcode,
		 * so an unconfigured system behaves exactly as before). */
		const m4_conf_t *conf = m4_config();
		const char *placeholder_name = m4_conf_get_string(conf, "graph", "mic-name");
		const char *repair_mode = m4_conf_get_string(conf, "daemon", "repair-a2dp");

		cfg.graph_ops = &m4_graph_ops_pw;
		cfg.mac_address = mac;
		cfg.runtime_dir = runtime_dir;
		cfg.state_dir = state_dir;
		cfg.stats_path = stats_path;
		cfg.confirm_timeout_ms = (int)m4_conf_get_int(conf, "daemon", "profile-confirm-timeout-ms", 10000);
		cfg.tick_interval_ms = (int)m4_conf_get_int(conf, "daemon", "tick-ms", 0);
		cfg.graph_retry_s = m4_conf_get_double(conf, "daemon", "graph-retry-s", 0.0);
		cfg.mic_mute_threshold = m4_conf_get_double(conf, "daemon", "mic-mute-threshold", 0.0);
		cfg.earcon_delay_sco_up_ms = (int)m4_conf_get_int(conf, "earcon", "delay-sco-up-ms", 0);

		cfg.automode_on_debounce_s = m4_conf_get_int(conf, "automode", "on-debounce-ms", 0) / 1000.0;
		cfg.automode_off_debounce_s = m4_conf_get_int(conf, "automode", "off-debounce-ms", 0) / 1000.0;
		cfg.automode_max_switches = (int)m4_conf_get_int(conf, "automode", "max-switches", 0);
		cfg.automode_rate_window_s = m4_conf_get_double(conf, "automode", "rate-window-s", 0.0);
		m4_mic_set_exclude_apps(m4_conf_get_string(conf, "automode", "exclude-apps"));
		m4_mic_set_holder_apps(m4_conf_get_string(conf, "automode", "holder-apps"));

		m4_callfsm_set_profile_names(m4_conf_get_string(conf, "call", "profile-msbc"),
					      m4_conf_get_string(conf, "call", "profile-cvsd"),
					      m4_conf_get_string(conf, "call", "profile-music"));

		/* "repair-a2dp" -- real sd-bus, no fake,
		 * this is the production daemon. Powers BOTH the ctl.sock
		 * REPAIR_A2DP command and core/callfsm.c's automatic
		 * repair_profile_if_missing() path (via the adapter, daemon.c;
		 * gated off entirely by repair_a2dp_manual_only when `[daemon]
		 * repair-a2dp = manual`). 0/<=0 below -> repair_a2dp.c's own
		 * compiled defaults (same numbers as the registry's). */
		cfg.bt_ops = &m4_bt_ops_sdbus;
		cfg.bt_adapter = m4_config_bt_adapter();
		cfg.repair_connect_timeout_ms = (int)m4_conf_get_int(conf, "daemon", "repair-connect-timeout-ms", 0);
		cfg.repair_enum_wait_ms = (int)m4_conf_get_int(conf, "daemon", "repair-enum-wait-ms", 0);
		cfg.repair_a2dp_confirm_timeout_ms =
			(int)m4_conf_get_int(conf, "daemon", "repair-confirm-timeout-ms", 0);
		cfg.repair_a2dp_manual_only = repair_mode != NULL && strcmp(repair_mode, "manual") == 0;

		cfg.enable_micdetect = true;
		cfg.micdetect_cfg.mac_address = mac;
		cfg.micdetect_cfg.placeholder_name =
			placeholder_name != NULL && placeholder_name[0] != '\0' ? placeholder_name : "momentum4_mic";
	}

	cfg.enable_ctl = true;
	cfg.socket_path = sock_path;

	/* hfp-events.sock */
	cfg.enable_hfp_events = true;

	cfg.enable_tick_timer = true;

	/* momentum4.conf (mode, gestures, player, earcon) */
	if (m4_config_file_path(config_path, sizeof(config_path)) == NULL)
		config_path[0] = '\0';
	settings_rc = m4_settings_load_with_migration_hint(&settings, config_path);
	m4_log(M4_LOG_INFO, M4_T("momentum4d: config %s: %s; mode %s, player %s, player-status %s",
"momentum4d: конфиг %s: %s; режим %s, плеер %s, player-status %s"),
	       config_path[0] ? config_path : M4_T("(path unknown)",
"(путь неизвестен)"),
	       settings_rc == 0 ? M4_T("read",
"прочитан") : settings_rc == 1 ? M4_T("no file, using defaults",
"нет файла, значения по умолчанию")
							   : M4_T("read error, using defaults",
"ошибка чтения, значения по умолчанию"),
	       m4_daemon_mode_name(settings.mode), settings.player_enabled ? "on" : "off",
	       m4_player_status_policy_name(settings.player_status));
	cfg.has_initial_mode = true;
	cfg.initial_mode = settings.mode;
	cfg.settings = &settings;
	cfg.config_path = config_path;
	cfg.earcon = production_earcon;
	pt = m4_passthrough_sdbus_new(NULL); /* session bus, opened lazily */
	if (pt != NULL) {
		pt_ops = m4_passthrough_sdbus_ops(pt);
		cfg.passthrough = &pt_ops;
	}

	d = m4_daemon_new(event, &cfg);
	if (d == NULL) {
		m4_log(M4_LOG_ERR, M4_T("momentum4d: failed to initialize",
"momentum4d: не удалось инициализироваться"));
		m4_passthrough_sdbus_free(pt);
		sd_event_unref(event);
		return 1;
	}

	if (settings.player_enabled) {
		player = start_player(event, d, &settings, &system_bus);
		m4_daemon_attach_player(d, player);
	} else {
		m4_log(M4_LOG_INFO, M4_T("momentum4d: player = off -- not registering the MPRIS player in BlueZ",
"momentum4d: player = off -- MPRIS-плеер в BlueZ не регистрирую"));
	}

	{
		struct m4_daemon_status st;

		m4_daemon_get_status(d, &st);
		m4_log(M4_LOG_INFO, M4_T("momentum4d: started, mode %s (from %s)",
"momentum4d: запущен, режим %s (из %s)"), m4_daemon_mode_name(st.mode),
		       settings_rc == 0 ? "momentum4.conf" : M4_T("default values",
"значения по умолчанию"));
	}
	rc = m4_daemon_run(d);

	m4_daemon_attach_player(d, NULL);
	m4_mpris_player_free(player);
	sd_bus_flush_close_unref(system_bus);
	m4_daemon_free(d);
	m4_passthrough_sdbus_free(pt);
	sd_event_unref(event);
	return rc < 0 ? 1 : 0;
}
