/* util/settings.c -- see settings.h. Rebased on common/m4-conf:
 * every constant is now only a default sourced from the registry:
 * every numeric/enum default this file used to duplicate as its own
 * `#define` (M4_SETTINGS_DEFAULT_COOLDOWN_MS 1500, EARCON_DELAY_ON_MS 400,
 * ...) now comes from common/m4-conf-keys.def's [control]/[earcon]/[daemon]
 * entries -- this file has exactly ONE remaining literal default anywhere
 * (M4_DAEMON_MODE_MANUAL in m4_settings_defaults(), which is not a
 * momentum4.conf VALUE but the enum m4_conf_get_string() itself already
 * guarantees for daemon.mode when nothing else was ever loaded).
 *
 * m4_settings_parse()/m4_settings_load() still accept BOTH the OLD flat
 * ("key = value", no [section]) grammar originally
 * documented in settings.h AND the NEW sectioned grammar
 * (common/m4-conf.h) -- m4_settings_load(path) tells them apart by peeking
 * the file (util/legacy_conf.h's own detection, reused from
 * cli/config.c's `migrate`), so an un-migrated live momentum4.conf keeps
 * working exactly as before (with one added warning suggesting `momentum4
 * config migrate`) while a migrated one is read through the shared
 * common/m4-conf.c parser instead. m4_settings_parse() itself (in-memory
 * text, no path) is UNCHANGED flat-grammar-only -- test_settings.c and
 * every daemon test fixture call it directly with flat text on purpose
 * (a fast, no-file-I/O way to drive one gesture/earcon knob at a time), and
 * nothing about the new sectioned format changes that contract.
 */
#include "settings.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "../../../common/m4-conf.h"
#include "../../../common/m4-i18n.h"
#include "legacy_conf.h"
#include "log.h"

#define M4_SETTINGS_MAX_FILE (64 * 1024)

/* Fills every field m4-conf owns (everything except n_warnings/first_warning,
 * set by the two callers below from their own m4_conf_t/flat-parse warning
 * counts respectively) from a fully-loaded m4_conf_t `c` -- shared by
 * m4_settings_defaults() (an empty, defaults-only `c`) and
 * m4_settings_load()'s new-sectioned-format path (a `c` with one file layer
 * applied on top of defaults). m4_settings_parse() (flat-only) does NOT use
 * this -- it keeps setting `s` fields directly, as before. */
static void fill_from_conf(struct m4_settings *s, const m4_conf_t *c)
{
	m4_daemon_mode_t mode;
	m4_player_status_policy_t status;
	m4_gesture_action_t a;
	m4_sco_drop_policy_t drop;
	const char *v;
	static const struct {
		m4_player_cmd_t cmd;
		const char *key;
	} gesture_keys[] = {
		{ M4_PLAYER_CMD_PLAY, "gesture-play" },
		{ M4_PLAYER_CMD_PAUSE, "gesture-pause" },
		{ M4_PLAYER_CMD_PLAYPAUSE, "gesture-playpause" },
		{ M4_PLAYER_CMD_NEXT, "gesture-next" },
		{ M4_PLAYER_CMD_PREVIOUS, "gesture-previous" },
		{ M4_PLAYER_CMD_STOP, "gesture-stop" },
	};

	if (m4_daemon_mode_parse(m4_conf_get_string(c, "daemon", "mode"), &mode) == 0)
		s->mode = mode;
	s->player_enabled = m4_conf_get_bool(c, "control", "player", true);
	if (m4_player_status_policy_parse(m4_conf_get_string(c, "control", "player-status"), &status) == 0)
		s->player_status = status;
	{
		m4_player_resync_t rs;

		if (m4_player_resync_parse(m4_conf_get_string(c, "control", "player-resync"), &rs) == 0)
			s->player_resync = rs;
	}

	for (size_t i = 0; i < sizeof(gesture_keys) / sizeof(gesture_keys[0]); i++) {
		v = m4_conf_get_string(c, "control", gesture_keys[i].key);
		if (v != NULL && v[0] != '\0' && m4_gesture_action_parse(v, &a) == 0)
			s->gesture[gesture_keys[i].cmd] = a;
	}
	/* gesture-play/-pause default to gesture-playpause (registry default
	 * "" for both -- see m4-conf-keys.def's own comment): fill AFTER
	 * gesture-playpause itself is set, same inheritance settings.h always
	 * documented. */
	v = m4_conf_get_string(c, "control", "gesture-play");
	if (v == NULL || v[0] == '\0')
		s->gesture[M4_PLAYER_CMD_PLAY] = s->gesture[M4_PLAYER_CMD_PLAYPAUSE];
	v = m4_conf_get_string(c, "control", "gesture-pause");
	if (v == NULL || v[0] == '\0')
		s->gesture[M4_PLAYER_CMD_PAUSE] = s->gesture[M4_PLAYER_CMD_PLAYPAUSE];

	s->gesture_cooldown_ms = (int)m4_conf_get_int(c, "control", "gesture-cooldown-ms", s->gesture_cooldown_ms);
	s->earcon = m4_conf_get_bool(c, "earcon", "enable", true);
	v = m4_conf_get_string(c, "earcon", "dir");
	if (v != NULL)
		snprintf(s->earcon_dir, sizeof(s->earcon_dir), "%s", v);
	s->earcon_delay_on_ms = (int)m4_conf_get_int(c, "earcon", "delay-on-ms", s->earcon_delay_on_ms);
	s->earcon_delay_off_ms = (int)m4_conf_get_int(c, "earcon", "delay-off-ms", s->earcon_delay_off_ms);

	if (m4_gesture_action_parse(m4_conf_get_string(c, "control", "hfp-chup"), &a) == 0)
		s->hfp_chup = a;
	if (m4_gesture_action_parse(m4_conf_get_string(c, "control", "hfp-ata"), &a) == 0)
		s->hfp_ata = a;
	if (m4_gesture_action_parse(m4_conf_get_string(c, "control", "hfp-bldn"), &a) == 0)
		s->hfp_bldn = a;
	if (m4_gesture_action_parse(m4_conf_get_string(c, "control", "hfp-bvra"), &a) == 0)
		s->hfp_bvra = a;
	if (m4_sco_drop_policy_parse(m4_conf_get_string(c, "control", "hfp-sco-drop"), &drop) == 0)
		s->hfp_sco_drop = drop;

	/* mic-on-enter: `[call] mic-on-enter` / `[earcon]
	 * mic-off-chain-delay-ms` -- see this struct field's own doc comment
	 * (settings.h) for why two different modern sections land here. */
	{
		m4_mic_on_enter_t moe;

		if (m4_mic_on_enter_parse(m4_conf_get_string(c, "call", "mic-on-enter"), &moe) == 0)
			s->mic_on_enter = moe;
	}
	s->mic_off_chain_delay_ms =
		(int)m4_conf_get_int(c, "earcon", "mic-off-chain-delay-ms", s->mic_off_chain_delay_ms);
}

void m4_settings_defaults(struct m4_settings *s)
{
	m4_conf_t c;

	memset(s, 0, sizeof(*s));
	m4_conf_init(&c); /* registry compiled-in defaults, no file layer at all */
	fill_from_conf(s, &c);
}

__attribute__((format(printf, 4, 5)))
static void warn(struct m4_settings *s, const char *origin, int lineno, const char *fmt, ...)
{
	char msg[160];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);

	if (s->n_warnings == 0)
		snprintf(s->first_warning, sizeof(s->first_warning), "%s:%d: %s", origin, lineno, msg);
	s->n_warnings++;
	m4_log(M4_LOG_WARN, M4_T("config %s:%d: %s -- default value kept",
"конфиг %s:%d: %s -- значение по умолчанию оставлено"), origin, lineno, msg);
}

int m4_settings_parse_bool(const char *v)
{
	if (strcasecmp(v, "1") == 0 || strcasecmp(v, "yes") == 0 || strcasecmp(v, "on") == 0 ||
	    strcasecmp(v, "true") == 0)
		return 1;
	if (strcasecmp(v, "0") == 0 || strcasecmp(v, "no") == 0 || strcasecmp(v, "off") == 0 ||
	    strcasecmp(v, "false") == 0)
		return 0;
	return -1;
}

static char *trim(char *p)
{
	char *end;

	while (*p != '\0' && isspace((unsigned char)*p))
		p++;
	end = p + strlen(p);
	while (end > p && isspace((unsigned char)end[-1]))
		*--end = '\0';
	return p;
}

static int gesture_key_to_cmd(const char *key, m4_player_cmd_t *out)
{
	static const char prefix[] = "gesture-";

	if (strncmp(key, prefix, sizeof(prefix) - 1) != 0)
		return -1;
	key += sizeof(prefix) - 1;
	for (int i = 0; i < M4_PLAYER_CMD_COUNT; i++) {
		if (strcmp(key, m4_player_cmd_key((m4_player_cmd_t)i)) == 0) {
			*out = (m4_player_cmd_t)i;
			return 0;
		}
	}
	return -1;
}

/* M4_SETTINGS_MAX_COOLDOWN_MS/M4_SETTINGS_MAX_EARCON_DELAY_MS (settings.h):
 * these two upper bounds are the ONE place this file still duplicates a
 * number also present in common/m4-conf-keys.def ([control]
 * gesture-cooldown-ms / [earcon] delay-on-ms/-off-ms all cap at the same
 * values) -- unavoidable as long as m4_settings_parse() keeps its own,
 * m4-conf-independent flat-text grammar (this file's own top comment);
 * kept as named constants (not bare literals) specifically so a future
 * registry range change is at least grep-able from here. */
static void apply(struct m4_settings *s, const char *origin, int lineno, const char *key,
		  const char *value, bool *set_play, bool *set_pause)
{
	m4_player_cmd_t cmd;
	int b;

	if (strcmp(key, "mode") == 0) {
		m4_daemon_mode_t m;

		if (m4_daemon_mode_parse(value, &m) < 0)
			warn(s, origin, lineno, M4_T("mode = \"%s\": expected observe|manual|auto",
"mode = \"%s\": ожидается observe|manual|auto"), value);
		else
			s->mode = m;
	} else if (strcmp(key, "player") == 0) {
		if ((b = m4_settings_parse_bool(value)) < 0)
			warn(s, origin, lineno, M4_T("player = \"%s\": expected on|off",
"player = \"%s\": ожидается on|off"), value);
		else
			s->player_enabled = b;
	} else if (strcmp(key, "player-status") == 0) {
		m4_player_status_policy_t p;

		if (m4_player_status_policy_parse(value, &p) < 0)
			warn(s, origin, lineno, M4_T("player-status = \"%s\": expected stopped|playing|paused|follow",
"player-status = \"%s\": ожидается stopped|playing|paused|follow"),
			     value);
		else
			s->player_status = p;
	} else if (strcmp(key, "player-resync") == 0) {
		m4_player_resync_t rs;

		if (m4_player_resync_parse(value, &rs) < 0)
			warn(s, origin, lineno, M4_T("player-resync = \"%s\": expected on|connect|off",
"player-resync = \"%s\": ожидается on|connect|off"), value);
		else
			s->player_resync = rs;
	} else if (gesture_key_to_cmd(key, &cmd) == 0) {
		m4_gesture_action_t a;

		if (m4_gesture_action_parse(value, &a) < 0) {
			warn(s, origin, lineno,
			     M4_T("%s = \"%s\": expected none|call-toggle|call-on|call-off|passthrough|passthrough-exact|mic-toggle",
"%s = \"%s\": ожидается none|call-toggle|call-on|call-off|passthrough|passthrough-exact|mic-toggle"),
			     key, value);
			return;
		}
		s->gesture[cmd] = a;
		if (cmd == M4_PLAYER_CMD_PLAY)
			*set_play = true;
		else if (cmd == M4_PLAYER_CMD_PAUSE)
			*set_pause = true;
	} else if (strcmp(key, "gesture-cooldown-ms") == 0) {
		char *end = NULL;
		long v;

		errno = 0;
		v = strtol(value, &end, 10);
		if (errno != 0 || end == value || *end != '\0' || v < 0 || v > M4_SETTINGS_MAX_COOLDOWN_MS)
			warn(s, origin, lineno, M4_T("gesture-cooldown-ms = \"%s\": expected 0..%d",
"gesture-cooldown-ms = \"%s\": ожидается 0..%d"), value,
			     M4_SETTINGS_MAX_COOLDOWN_MS);
		else
			s->gesture_cooldown_ms = (int)v;
	} else if (strcmp(key, "earcon-delay-on-ms") == 0 || strcmp(key, "earcon-delay-off-ms") == 0) {
		char *end = NULL;
		long v;

		errno = 0;
		v = strtol(value, &end, 10);
		if (errno != 0 || end == value || *end != '\0' || v < 0 || v > M4_SETTINGS_MAX_EARCON_DELAY_MS) {
			warn(s, origin, lineno, M4_T("%s = \"%s\": expected 0..%d",
"%s = \"%s\": ожидается 0..%d"), key, value,
			     M4_SETTINGS_MAX_EARCON_DELAY_MS);
		} else if (strcmp(key, "earcon-delay-on-ms") == 0) {
			s->earcon_delay_on_ms = (int)v;
		} else {
			s->earcon_delay_off_ms = (int)v;
		}
	} else if (strcmp(key, "earcon") == 0) {
		if ((b = m4_settings_parse_bool(value)) < 0)
			warn(s, origin, lineno, M4_T("earcon = \"%s\": expected on|off",
"earcon = \"%s\": ожидается on|off"), value);
		else
			s->earcon = b;
	} else if (strcmp(key, "earcon-dir") == 0) {
		if (strlen(value) >= sizeof(s->earcon_dir))
			warn(s, origin, lineno, M4_T("earcon-dir: path too long",
"earcon-dir: слишком длинный путь"));
		else
			snprintf(s->earcon_dir, sizeof(s->earcon_dir), "%s", value);
	} else if (strcmp(key, "hfp-chup") == 0 || strcmp(key, "hfp-ata") == 0 ||
		   strcmp(key, "hfp-bldn") == 0 || strcmp(key, "hfp-bvra") == 0) {
		m4_gesture_action_t a;

		if (m4_gesture_action_parse(value, &a) < 0) {
			warn(s, origin, lineno,
			     M4_T("%s = \"%s\": expected none|call-toggle|call-on|call-off|passthrough|passthrough-exact|mic-toggle",
"%s = \"%s\": ожидается none|call-toggle|call-on|call-off|passthrough|passthrough-exact|mic-toggle"),
			     key, value);
			return;
		}
		if (strcmp(key, "hfp-chup") == 0)
			s->hfp_chup = a;
		else if (strcmp(key, "hfp-ata") == 0)
			s->hfp_ata = a;
		else if (strcmp(key, "hfp-bldn") == 0)
			s->hfp_bldn = a;
		else
			s->hfp_bvra = a;
	} else if (strcmp(key, "hfp-sco-drop") == 0) {
		m4_sco_drop_policy_t p;

		if (m4_sco_drop_policy_parse(value, &p) < 0)
			warn(s, origin, lineno, M4_T("hfp-sco-drop = \"%s\": expected restore|call-off|mic-toggle|none",
"hfp-sco-drop = \"%s\": ожидается restore|call-off|mic-toggle|none"),
			     value);
		else
			s->hfp_sco_drop = p;
	} else if (strcmp(key, "mic-on-enter") == 0) {
		m4_mic_on_enter_t moe;

		if (m4_mic_on_enter_parse(value, &moe) < 0)
			warn(s, origin, lineno, M4_T("mic-on-enter = \"%s\": expected on|off|remember",
"mic-on-enter = \"%s\": ожидается on|off|remember"), value);
		else
			s->mic_on_enter = moe;
	} else if (strcmp(key, "mic-off-chain-delay-ms") == 0) {
		char *end = NULL;
		long v;

		errno = 0;
		v = strtol(value, &end, 10);
		if (errno != 0 || end == value || *end != '\0' || v < 0 || v > M4_SETTINGS_MAX_EARCON_DELAY_MS)
			warn(s, origin, lineno, M4_T("mic-off-chain-delay-ms = \"%s\": expected 0..%d",
"mic-off-chain-delay-ms = \"%s\": ожидается 0..%d"), value,
			     M4_SETTINGS_MAX_EARCON_DELAY_MS);
		else
			s->mic_off_chain_delay_ms = (int)v;
	} else {
		warn(s, origin, lineno, M4_T("unknown key \"%s\"",
"неизвестный ключ \"%s\""), key);
	}
}

void m4_settings_parse(struct m4_settings *s, const char *text, const char *origin)
{
	const char *p = text;
	int lineno = 0;
	bool set_play = false, set_pause = false;

	if (origin == NULL)
		origin = "momentum4.conf";

	while (p != NULL && *p != '\0') {
		const char *nl = strchr(p, '\n');
		size_t len = nl != NULL ? (size_t)(nl - p) : strlen(p);
		char line[512];
		char *l, *eq, *key, *value, *hash;

		lineno++;
		if (len >= sizeof(line)) {
			warn(s, origin, lineno, M4_T("line too long",
"слишком длинная строка"));
			p = nl != NULL ? nl + 1 : NULL;
			continue;
		}
		memcpy(line, p, len);
		line[len] = '\0';
		p = nl != NULL ? nl + 1 : NULL;

		l = trim(line);
		if (*l == '\0' || *l == '#' || *l == ';')
			continue;

		eq = strchr(l, '=');
		if (eq == NULL) {
			warn(s, origin, lineno, M4_T("no '=' in line \"%s\"",
"нет '=' в строке \"%s\""), l);
			continue;
		}
		*eq = '\0';
		key = trim(l);
		value = trim(eq + 1);

		/* trailing " # comment" (only after whitespace, so '#' inside a
		 * path without a preceding space survives) */
		for (hash = value; (hash = strchr(hash, '#')) != NULL; hash++) {
			if (hash > value && isspace((unsigned char)hash[-1])) {
				*hash = '\0';
				value = trim(value);
				break;
			}
		}
		if (value[0] == '"' && strlen(value) >= 2 && value[strlen(value) - 1] == '"') {
			value[strlen(value) - 1] = '\0';
			value++;
		}

		for (char *k = key; *k != '\0'; k++) {
			*k = (char)tolower((unsigned char)*k);
			if (*k == '_')
				*k = '-';
		}
		if (*key == '\0') {
			warn(s, origin, lineno, M4_T("empty key",
"пустой ключ"));
			continue;
		}
		apply(s, origin, lineno, key, value, &set_play, &set_pause);
	}

	/* gesture-play / gesture-pause default to gesture-playpause */
	if (!set_play)
		s->gesture[M4_PLAYER_CMD_PLAY] = s->gesture[M4_PLAYER_CMD_PLAYPAUSE];
	if (!set_pause)
		s->gesture[M4_PLAYER_CMD_PAUSE] = s->gesture[M4_PLAYER_CMD_PLAYPAUSE];
}

/* Peeks `path`'s first non-blank/non-comment line: '[' means the NEW
 * sectioned format (common/m4-conf.h) -- everything else (including a file
 * that does not exist, which the caller re-discovers itself right after via
 * fopen()) means "treat as the OLD flat format", matching
 * util/legacy_conf.c's own m4_legacy_conf_load() detection (-3 return) but
 * without needing an m4_conf_t just to ask the question. */
static bool looks_sectioned(const char *path)
{
	FILE *f = fopen(path, "re");
	char line[256];
	bool sectioned = false;

	if (f == NULL)
		return false;
	while (fgets(line, sizeof(line), f) != NULL) {
		char *l = trim(line);

		if (*l == '\0' || *l == '#' || *l == ';')
			continue;
		sectioned = (*l == '[');
		break;
	}
	fclose(f);
	return sectioned;
}

int m4_settings_load(struct m4_settings *s, const char *path)
{
	m4_conf_t c;

	m4_settings_defaults(s);
	if (path == NULL || path[0] == '\0')
		return 1;

	if (looks_sectioned(path)) {
		int rc;

		/* m4_conf_load_file() loads ON TOP of `c` and does not initialise
		 * it: without this, keys absent from the file were read from
		 * uninitialised stack memory (found by tests/test_settings.c
		 * test_player_resync(): a second load inherited the first one's
		 * value). */
		m4_conf_init(&c);
		rc = m4_conf_load_file(&c, path); /* m4_conf_t.c contract: 1 read, 0 ENOENT, -1 error */

		if (rc <= 0) {
			m4_settings_parse(s, "", path); /* applies play/pause inheritance, no other change */
			return rc == 0 ? 1 : -1;
		}
		fill_from_conf(s, &c);
		s->n_warnings = c.n_warnings;
		if (c.n_warnings > 0)
			snprintf(s->first_warning, sizeof(s->first_warning), "%s", c.warnings[0]);
		return 0;
	}

	{
		FILE *f;
		char *buf;
		size_t n;

		f = fopen(path, "re");
		if (f == NULL) {
			if (errno == ENOENT) {
				m4_settings_parse(s, "", path);
				return 1;
			}
			m4_log(M4_LOG_WARN, M4_T("config %s: could not be read (%s) -- using defaults",
"конфиг %s: не читается (%s) -- значения по умолчанию"), path,
			       strerror(errno));
			m4_settings_parse(s, "", path);
			return -1;
		}
		buf = malloc(M4_SETTINGS_MAX_FILE + 1);
		if (buf == NULL) {
			fclose(f);
			m4_settings_parse(s, "", path);
			return -1;
		}
		n = fread(buf, 1, M4_SETTINGS_MAX_FILE, f);
		buf[n] = '\0';
		if (!feof(f))
			m4_log(M4_LOG_WARN, M4_T("config %s: more than %d bytes, the rest was ignored",
"конфиг %s: больше %d байт, хвост проигнорирован"), path,
			       M4_SETTINGS_MAX_FILE);
		fclose(f);

		m4_settings_parse(s, buf, path);
		free(buf);
		return 0;
	}
}

/* Old-flat-format startup compat wrapper (the old flat
 * momentum4.conf is still read, with a warning) -- used ONLY
 * by cli/daemon_cmd.c at normal daemon startup (NOT by m4_settings_load()
 * itself, whose own contract/tests above are unchanged): loads `path`
 * exactly like m4_settings_load() when it is flat, but additionally appends
 * one clear "run config migrate" warning (Russian text) so the operator notices
 * *once*, at startup, rather than only when running `momentum4 config show`
 * by hand. A no-op (same as m4_settings_load()) for a missing file or an
 * already-sectioned one. */
int m4_settings_load_with_migration_hint(struct m4_settings *s, const char *path)
{
	int rc = m4_settings_load(s, path);

	if (path != NULL && path[0] != '\0' && rc == 0 && !looks_sectioned(path)) {
		s->n_warnings++;
		m4_log(M4_LOG_WARN,
		       M4_T("momentum4d: config %s: old sectionless format -- run `momentum4 config migrate`",
"momentum4d: конфиг %s: старый формат без секций -- запустите `momentum4 config migrate`"),
		       path);
		if (s->n_warnings == 1) {
			int n1 = snprintf(s->first_warning, sizeof(s->first_warning), "%.100s: ", path);

			if (n1 > 0 && (size_t)n1 < sizeof(s->first_warning))
				snprintf(s->first_warning + n1, sizeof(s->first_warning) - (size_t)n1,
					 M4_T("old sectionless format -- run `momentum4 config migrate`",
"старый формат без секций -- запустите `momentum4 config migrate`"));
		}
	}
	return rc;
}
