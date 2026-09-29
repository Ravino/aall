/* util/config.c -- see config.h. */
#include "config.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../bt/bt_discover.h"
#include "../util/log.h"
#include "../../../common/m4-i18n.h"
#include "legacy_conf.h"

/* ------------------------------------------------------------------ */
/* m4_config(): the one process-wide m4_conf_t, loaded lazily.         */
/* ------------------------------------------------------------------ */

static m4_conf_t g_conf;
static bool g_conf_loaded = false;

/* Applies the OLD flat momentum4.conf on top of `c` if the user's config
 * file exists and is still in that pre-[section] format (backward
 * compatibility). m4_conf_load_layers() already tried to parse this same
 * file as the NEW format first (as its layer 4) -- for a flat file that
 * produces one "unknown key" warning per line (every key line, having no
 * preceding [section], lands in the unnamed section, which is not in the
 * registry). Harmless but noisy; this function overrides every one of
 * those keys with its CORRECTLY validated, mapped value afterwards, then
 * appends one clear explanatory warning. Idempotent/no-op once the file has
 * been migrated (m4_legacy_conf_load() returns -3 for an already-sectioned
 * file, or -1 if there is no user file at all). */
static void apply_legacy_momentum4_conf(m4_conf_t *c)
{
	char path[512];
	int rc;

	if (m4_config_file_path(path, sizeof(path)) == NULL)
		return;
	rc = m4_legacy_conf_load(path, m4_legacy_momentum4_conf_map, m4_legacy_momentum4_conf_map_count, c,
				  "legacy", NULL, NULL);
	if (rc <= 0)
		return; /* -1 no file, -2 read error (already warned by the
			  * earlier m4_conf_load_layers() attempt), -3 already
			  * sectioned, 0 flat file existed but mapped nothing */
	if (c->n_warnings < M4_CONF_MAX_WARNINGS) {
		char *w = c->warnings[c->n_warnings];
		int n1;

		n1 = snprintf(w, M4_CONF_MAX_WARNING_MSG, M4_T("%.100s: old sectionless format (%d ",
"%.100s: старый формат без секций (%d "), path, rc);
		if (n1 > 0 && (size_t)n1 < M4_CONF_MAX_WARNING_MSG)
			snprintf(w + n1, M4_CONF_MAX_WARNING_MSG - (size_t)n1,
				 M4_T("key(s) migrated on the fly) -- run `momentum4 config migrate --apply`",
"ключей перенесено на лету) -- запустите `momentum4 config migrate --apply`"));
	}
	c->n_warnings++;
	m4_log(M4_LOG_WARN, M4_T("momentum4d: config %s: old sectionless format -- run "
			     "`momentum4 config migrate`",
"momentum4d: конфиг %s: старый формат без секций -- запустите "
			     "`momentum4 config migrate`"),
	       path);
}

static void config_load_now(void)
{
	m4_conf_load_layers(&g_conf);
	apply_legacy_momentum4_conf(&g_conf);
	g_conf_loaded = true;
}

const m4_conf_t *m4_config(void)
{
	if (!g_conf_loaded)
		config_load_now();
	return &g_conf;
}

/* ------------------------------------------------------------------ */
/* [device] address/adapter=auto (bt/bt_discover.h)                    */
/* ------------------------------------------------------------------ */

#define M4_CONFIG_AUTO_MAC_LEN 32
#define M4_CONFIG_AUTO_ADAPTER_LEN 64

static bool g_auto_tried = false;
static char g_auto_mac[M4_CONFIG_AUTO_MAC_LEN] = "";
static char g_auto_adapter[M4_CONFIG_AUTO_ADAPTER_LEN] = "";

static void run_auto_discovery_once(void)
{
	char err[200];
	const char *name_match;

	if (g_auto_tried)
		return;
	g_auto_tried = true;

	name_match = m4_conf_get_string(m4_config(), "device", "name-match");
	if (m4_bt_discover_auto(name_match, g_auto_mac, sizeof(g_auto_mac), g_auto_adapter,
				 sizeof(g_auto_adapter), err, sizeof(err)) != 0) {
		m4_log(M4_LOG_WARN,
		       M4_T("momentum4d: [device] address/adapter = auto: BlueZ discovery failed (%s) -- "
		       "set the MAC explicitly in momentum4.conf ([device] address) or run `momentum4 install`",
"momentum4d: [device] address/adapter = auto: BlueZ-обнаружение не удалось (%s) -- "
		       "укажите MAC явно в momentum4.conf ([device] address) или запустите `momentum4 install`"),
		       err);
		g_auto_mac[0] = '\0';
		g_auto_adapter[0] = '\0';
	}
}

void m4_config_reload(void)
{
	g_conf_loaded = false;
	g_auto_tried = false;
	g_auto_mac[0] = '\0';
	g_auto_adapter[0] = '\0';
}

const char *m4_config_mac_address(void)
{
	const char *env = getenv("MOMENTUM4_MAC");
	const char *conf_val;

	if (env != NULL && env[0] != '\0')
		return env;

	conf_val = m4_conf_get_string(m4_config(), "device", "address");
	if (conf_val != NULL && strcmp(conf_val, "auto") != 0)
		return conf_val;

	run_auto_discovery_once();
	return g_auto_mac; /* "" if discovery failed -- never NULL */
}

const char *m4_config_bt_adapter(void)
{
	const char *env = getenv("MOMENTUM4_BT_ADAPTER");
	const char *conf_val;

	if (env != NULL && env[0] != '\0')
		return env;

	conf_val = m4_conf_get_string(m4_config(), "device", "adapter");
	if (conf_val != NULL && strcmp(conf_val, "auto") != 0)
		return conf_val;

	run_auto_discovery_once();
	return g_auto_adapter[0] != '\0' ? g_auto_adapter : "hci0";
}

/* ------------------------------------------------------------------ */
/* paths/stats (already ${XDG_...}-expanded by common/m4-conf.c)       */
/* ------------------------------------------------------------------ */

const char *m4_config_runtime_dir(char *buf, size_t buf_cap)
{
	const char *override = getenv("MOMENTUM4_RUNTIME_DIR");
	const char *v;
	int n;

	if (override != NULL && override[0] != '\0') {
		if (strlen(override) + 1 > buf_cap)
			return NULL;
		strcpy(buf, override);
		return buf;
	}

	v = m4_conf_get_string(m4_config(), "paths", "runtime-dir");
	if (v == NULL || v[0] == '\0')
		return NULL;
	n = snprintf(buf, buf_cap, "%s", v);
	if (n < 0 || (size_t)n >= buf_cap)
		return NULL;
	return buf;
}

const char *m4_config_ctl_socket_path(char *buf, size_t buf_cap)
{
	char runtime_dir[480];
	int n;

	if (m4_config_runtime_dir(runtime_dir, sizeof(runtime_dir)) == NULL)
		return NULL;
	n = snprintf(buf, buf_cap, "%s/ctl.sock", runtime_dir);
	if (n < 0 || (size_t)n >= buf_cap)
		return NULL;
	return buf;
}

const char *m4_config_state_dir(char *buf, size_t buf_cap)
{
	const char *v;
	int n;

	v = m4_conf_get_string(m4_config(), "paths", "state-dir");
	if (v == NULL || v[0] == '\0')
		return NULL;
	n = snprintf(buf, buf_cap, "%s", v);
	if (n < 0 || (size_t)n >= buf_cap)
		return NULL;
	return buf;
}

const char *m4_config_stats_path(char *buf, size_t buf_cap)
{
	const char *override = getenv("M4_ADAPTIVE_STATS_PATH");
	const char *v;
	int n;

	if (override != NULL && override[0] != '\0') {
		if (strlen(override) + 1 > buf_cap)
			return NULL;
		strcpy(buf, override);
		return buf;
	}

	v = m4_conf_get_string(m4_config(), "stats", "path");
	if (v == NULL || v[0] == '\0')
		return NULL;
	n = snprintf(buf, buf_cap, "%s", v);
	if (n < 0 || (size_t)n >= buf_cap)
		return NULL;
	return buf;
}

const char *m4_config_file_path(char *buf, size_t buf_cap)
{
	const char *v = getenv("MOMENTUM4_CONFIG");
	int n;

	if (v != NULL && v[0] != '\0') {
		n = snprintf(buf, buf_cap, "%s", v);
	} else if ((v = getenv("XDG_CONFIG_HOME")) != NULL && v[0] != '\0') {
		n = snprintf(buf, buf_cap, "%s/momentum4/momentum4.conf", v);
	} else if ((v = getenv("HOME")) != NULL && v[0] != '\0') {
		n = snprintf(buf, buf_cap, "%s/.config/momentum4/momentum4.conf", v);
	} else {
		return NULL;
	}
	if (n < 0 || (size_t)n >= buf_cap)
		return NULL;
	return buf;
}
