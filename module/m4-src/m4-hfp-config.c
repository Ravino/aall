/* momentum4: implementation, see m4-hfp-config.h.
 * SPDX-License-Identifier: MIT
 */
#include "m4-hfp-config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "m4-conf.h"

/* Mirrors common/m4-conf.c's own m4_conf_load_layers() path resolution for
 * the ONE layer that can actually change at runtime without a process
 * restart in any way that matters here (a user hand-editing their own
 * momentum4.conf, or a package upgrade touching /etc's) -- @datadir@'s
 * fully-commented reference copy never sets real values, so its
 * mtime is not worth tracking. Uses the same public m4_conf_expand() the
 * daemon/plugin config loader itself uses, so the ${XDG_CONFIG_HOME}
 * fallback-to-$HOME/.config rule can never drift between the two. */
static void resolve_watch_paths(char *etc_path, size_t etc_cap, char *user_path, size_t user_cap)
{
	const char *override;

	snprintf(etc_path, etc_cap, "/etc/momentum4/momentum4.conf");

	override = getenv("MOMENTUM4_CONFIG");
	if (override != NULL && override[0] != '\0')
		snprintf(user_path, user_cap, "%s", override);
	else
		m4_conf_expand("${XDG_CONFIG_HOME}/momentum4/momentum4.conf", user_path, user_cap);
}

/* 0 if the file does not exist (or stat() fails for any other reason --
 * treated the same: "not present", never an error worth surfacing here). */
static long stat_mtime(const char *path)
{
	struct stat st;

	if (path == NULL || *path == '\0' || stat(path, &st) != 0)
		return 0;
	return (long)st.st_mtime;
}

static struct m4_hfp_config g_cfg;
static bool g_have_cfg;
static long g_etc_mtime = -1;
static long g_user_mtime = -1;

static void reload(void)
{
	m4_conf_t c;
	const char *s;

	m4_conf_init(&c);
	m4_conf_load_layers(&c);

	memset(&g_cfg, 0, sizeof(g_cfg));

	s = m4_conf_get_string(&c, "hfp", "call-control");
	g_cfg.call_control_mode = m4_device_policy_parse_hfp_mode(s);
	g_cfg.raw_at_capture = m4_conf_get_bool(&c, "hfp", "raw-at-capture", false);
	g_cfg.raw_min_interval_ms = (int)m4_conf_get_int(&c, "hfp", "raw-min-interval-ms", 200);

	s = m4_conf_get_string(&c, "hfp", "sco-restore");
	g_cfg.sco_restore_mode = m4_device_policy_parse_hfp_mode(s);
	g_cfg.sco_timing.first_delay_ms = (int)m4_conf_get_int(&c, "hfp", "sco-restore-first-delay-ms", 250);
	g_cfg.sco_timing.retry_delay_ms = (int)m4_conf_get_int(&c, "hfp", "sco-restore-retry-delay-ms", 1000);
	g_cfg.sco_timing.connect_timeout_ms = (int)m4_conf_get_int(&c, "hfp", "sco-restore-connect-timeout-ms", 2000);
	g_cfg.sco_timing.max_retries = (int)m4_conf_get_int(&c, "hfp", "sco-restore-max-retries", 1);
	g_cfg.sco_timing.window_ms = (int)m4_conf_get_int(&c, "hfp", "sco-restore-window-ms", 30000);
	g_cfg.sco_timing.max_episodes = (int)m4_conf_get_int(&c, "hfp", "sco-restore-max-episodes", 4);

	snprintf(g_cfg.name_match, sizeof(g_cfg.name_match), "%s",
			m4_conf_get_string(&c, "device", "name-match"));
	snprintf(g_cfg.device_address, sizeof(g_cfg.device_address), "%s",
			m4_conf_get_string(&c, "device", "address"));
	snprintf(g_cfg.events_socket, sizeof(g_cfg.events_socket), "%s",
			m4_conf_get_string(&c, "hfp", "events-socket"));

	g_have_cfg = true;
}

const struct m4_hfp_config *m4_hfp_config_get(void)
{
	char etc_path[512], user_path[512];
	long etc_mtime, user_mtime;

	resolve_watch_paths(etc_path, sizeof(etc_path), user_path, sizeof(user_path));
	etc_mtime = stat_mtime(etc_path);
	user_mtime = stat_mtime(user_path);

	if (!g_have_cfg || etc_mtime != g_etc_mtime || user_mtime != g_user_mtime) {
		reload();
		g_etc_mtime = etc_mtime;
		g_user_mtime = user_mtime;
	}
	return &g_cfg;
}
