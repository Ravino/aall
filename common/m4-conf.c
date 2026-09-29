/* common/m4-conf.c -- see m4-conf.h. No malloc, C99, no dependency beyond
 * the standard library . */
#include "m4-conf.h"
#include "m4-i18n.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* strcasecmp/strncasecmp (POSIX, not C99 -- available
			* on every platform this repo targets, glibc/musl/BSD) */

/* --- Registry (built from m4-conf-keys.def) ------------------------------ */

#define M4_CONF_KEY(sec, key, type, def, mn, mx, en, com_en, com_ru) \
	{ (sec), (key), (type), (def), (mn), (mx), (en), (com_en), (com_ru) },

const m4_conf_key_def_t m4_conf_registry[] = {
#include "m4-conf-keys.def"
};

#undef M4_CONF_KEY

const size_t m4_conf_registry_count = sizeof(m4_conf_registry) / sizeof(m4_conf_registry[0]);

/* m4_conf_t.entries[] is a fixed array (m4-conf.h) -- grow it there if this
 * ever fires (registry currently ~115 keys, see m4-conf-keys.def's own
 * header comment). Kept as a plain runtime check (not _Static_assert)
 * because m4_conf_registry_count is a `const size_t` initialized from a
 * sizeof of an array whose size depends on an #include -- portable across
 * the compilers this repo targets (gcc/clang, C99/C11) without relying on
 * _Static_assert accepting a non-ICE-looking-but-actually-constant
 * expression in every one of them. m4_conf_init() is always the first call
 * on any m4_conf_t (both real callers and every test), so checking there
 * catches this immediately, in every consumer, without a separate build step. */
#define M4_CONF_ENTRIES_CAP (sizeof(((m4_conf_t *)0)->entries) / sizeof(((m4_conf_t *)0)->entries[0]))

/* --- Build-time install paths (see m4-conf.h's "@prefix@ @libdir@ ..." doc) */
#ifndef M4_CONF_PREFIX
#define M4_CONF_PREFIX "/usr/local"
#endif
#ifndef M4_CONF_LIBDIR
#define M4_CONF_LIBDIR M4_CONF_PREFIX "/lib"
#endif
#ifndef M4_CONF_LIBEXECDIR
#define M4_CONF_LIBEXECDIR M4_CONF_PREFIX "/libexec"
#endif
#ifndef M4_CONF_DATADIR
#define M4_CONF_DATADIR M4_CONF_PREFIX "/share"
#endif
/* [paths] spa-system-dir's build-time default :
 * the consumer meson.build (currently only daemon/meson.build)
 * resolves this from `pkg-config --variable=plugindir libspa-0.2` and
 * passes it as -DM4_CONF_SPA_SYSTEM_DIR. Falls back to "" (not a guess)
 * when libspa-0.2 could not be probed at build time -- an empty string is
 * the one value `momentum4 wp-env` (cli/wp_env.c) is guaranteed to treat
 * as "do not touch SPA_PLUGIN_DIR", never as a real path (see that file's
 * own header comment for why guessing here would be unsafe: a wrong
 * non-empty path there crashes WirePlumber).
 */
#ifndef M4_CONF_SPA_SYSTEM_DIR
#define M4_CONF_SPA_SYSTEM_DIR ""
#endif
/* [paths] systemd-user-unit-dir's build-time default: the consumer
 * meson.build files (top-level meson.build and daemon/meson.build,
 * independently -- see either one's own comment) resolve this from
 * `get_option('prefix')` using the SAME prefix-dependent rule (systemd
 * --user's real UnitPath has a "lib/systemd/user" entry only under /usr
 * and /usr/local; any other prefix is searched under
 * "PREFIX/share/systemd/user" instead) and pass it as
 * -DM4_CONF_SYSTEMD_USER_UNIT_DIR. The fallback below mirrors that same
 * rule for a build that did not define the macro (e.g. a manual `cc`
 * invocation without meson) -- it is a reasonable guess, not a promise;
 * `momentum4 doctor`/`install` still verify the resolved path is one
 * systemd --user actually searches. */
#ifndef M4_CONF_SYSTEMD_USER_UNIT_DIR
#define M4_CONF_SYSTEMD_USER_UNIT_DIR M4_CONF_PREFIX "/share/systemd/user"
#endif

/* --- small string helpers ------------------------------------------------ */

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

/* case-insensitive, '_' == '-' (same convention as util/settings.c) */
static bool key_equal(const char *a, const char *b)
{
	for (;;) {
		unsigned char ca = (unsigned char)*a++;
		unsigned char cb = (unsigned char)*b++;

		if (ca == '_')
			ca = '-';
		if (cb == '_')
			cb = '-';
		ca = (unsigned char)tolower(ca);
		cb = (unsigned char)tolower(cb);
		if (ca != cb)
			return false;
		if (ca == '\0')
			return true;
	}
}

static bool section_equal(const char *a, const char *b)
{
	return strcasecmp(a, b) == 0;
}

static void lower_inplace(char *s)
{
	for (; *s != '\0'; s++)
		*s = (char)tolower((unsigned char)*s);
}

/* --- registry lookup ------------------------------------------------------ */

int m4_conf_find(const char *section, const char *key)
{
	size_t i;

	if (section == NULL || key == NULL)
		return -1;
	for (i = 0; i < m4_conf_registry_count; i++) {
		if (section_equal(m4_conf_registry[i].section, section) &&
		    key_equal(m4_conf_registry[i].key, key))
			return (int)i;
	}
	return -1;
}

/* --- placeholder expansion ------------------------------------------------ */

struct env_var {
	const char *name;
	const char *fallback_suffix; /* appended to $HOME if the var itself is unset/empty, or NULL */
};

static const struct env_var env_vars[] = {
	{ "XDG_RUNTIME_DIR", NULL },       /* no safe fallback -- left empty, same as util/config.c */
	{ "XDG_CONFIG_HOME", "/.config" },
	{ "XDG_DATA_HOME", "/.local/share" },
	{ "XDG_STATE_HOME", "/.local/state" },
	{ "HOME", NULL },
};

static void resolve_env_var(const char *name, char *out, size_t out_cap)
{
	const char *v = getenv(name);
	size_t i;

	out[0] = '\0';
	if (v != NULL && v[0] != '\0') {
		snprintf(out, out_cap, "%s", v);
		return;
	}
	for (i = 0; i < sizeof(env_vars) / sizeof(env_vars[0]); i++) {
		if (strcmp(env_vars[i].name, name) == 0 && env_vars[i].fallback_suffix != NULL) {
			const char *home = getenv("HOME");

			if (home != NULL && home[0] != '\0')
				snprintf(out, out_cap, "%s%s", home, env_vars[i].fallback_suffix);
			return;
		}
	}
}

struct build_var {
	const char *name;
	const char *value;
};

static const struct build_var build_vars[] = {
	{ "prefix", M4_CONF_PREFIX },
	{ "libdir", M4_CONF_LIBDIR },
	{ "libexecdir", M4_CONF_LIBEXECDIR },
	{ "datadir", M4_CONF_DATADIR },
	{ "spa_system_dir", M4_CONF_SPA_SYSTEM_DIR },
	{ "systemd_user_unit_dir", M4_CONF_SYSTEMD_USER_UNIT_DIR },
};

/* appends up to `n` bytes of `src` to *cursor (bounded by *remaining),
 * advancing both -- shared by every placeholder-expansion branch below. */
static void append_bounded(char **cursor, size_t *remaining, const char *src, size_t n)
{
	if (*remaining == 0)
		return;
	if (n >= *remaining)
		n = *remaining - 1;
	memcpy(*cursor, src, n);
	*cursor += n;
	*remaining -= n;
	**cursor = '\0';
}

void m4_conf_expand(const char *in, char *out, size_t out_cap)
{
	char *cursor = out;
	size_t remaining = out_cap;

	if (out_cap == 0)
		return;
	out[0] = '\0';
	if (in == NULL)
		return;

	while (*in != '\0') {
		if (in[0] == '$' && in[1] == '{') {
			const char *close = strchr(in + 2, '}');

			if (close != NULL && (size_t)(close - (in + 2)) < 64) {
				char name[64];
				size_t len = (size_t)(close - (in + 2));
				char val[M4_CONF_MAX_VALUE];

				memcpy(name, in + 2, len);
				name[len] = '\0';
				resolve_env_var(name, val, sizeof(val));
				append_bounded(&cursor, &remaining, val, strlen(val));
				in = close + 1;
				continue;
			}
		} else if (in[0] == '%' && in[1] == 'h') {
			char val[M4_CONF_MAX_VALUE];

			resolve_env_var("HOME", val, sizeof(val));
			append_bounded(&cursor, &remaining, val, strlen(val));
			in += 2;
			continue;
		} else if (in[0] == '@') {
			const char *close = strchr(in + 1, '@');

			if (close != NULL && close > in + 1 && (size_t)(close - (in + 1)) < 32) {
				char name[32];
				size_t len = (size_t)(close - (in + 1));
				size_t i;
				bool matched = false;

				memcpy(name, in + 1, len);
				name[len] = '\0';
				for (i = 0; i < sizeof(build_vars) / sizeof(build_vars[0]); i++) {
					if (strcmp(build_vars[i].name, name) == 0) {
						append_bounded(&cursor, &remaining, build_vars[i].value,
								strlen(build_vars[i].value));
						matched = true;
						break;
					}
				}
				if (matched) {
					in = close + 1;
					continue;
				}
			}
		}
		append_bounded(&cursor, &remaining, in, 1);
		in++;
	}
}

/* --- validation + storage -------------------------------------------------- */

__attribute__((format(printf, 3, 4)))
static void add_warning(m4_conf_t *c, bool count_only, const char *fmt, ...)
{
	va_list ap;

	c->n_warnings++;
	if (count_only || c->n_warnings > M4_CONF_MAX_WARNINGS)
		return;
	va_start(ap, fmt);
	vsnprintf(c->warnings[c->n_warnings - 1], M4_CONF_MAX_WARNING_MSG, fmt, ap);
	va_end(ap);
}

bool m4_conf_parse_bool(const char *v, bool *ok)
{
	if (ok != NULL)
		*ok = true;
	if (strcasecmp(v, "1") == 0 || strcasecmp(v, "yes") == 0 || strcasecmp(v, "on") == 0 ||
	    strcasecmp(v, "true") == 0)
		return true;
	if (strcasecmp(v, "0") == 0 || strcasecmp(v, "no") == 0 || strcasecmp(v, "off") == 0 ||
	    strcasecmp(v, "false") == 0)
		return false;
	if (ok != NULL)
		*ok = false;
	return false;
}

/* Validates+stores `raw_value` into entries[idx], per m4_conf_registry[idx]'s
 * declared type. `origin`/`lineno` are for warning text only ("" origin ->
 * used as-is, e.g. "default"). Returns true on success. On failure, leaves
 * entries[idx] untouched and appends one warning (unless `warnings_only_count`,
 * used by m4_conf_init() to size-check the registry itself without ever
 * having a non-empty warnings array at that point). */
static bool apply_value(m4_conf_t *c, size_t idx, const char *raw_value, const char *origin, int lineno)
{
	const m4_conf_key_def_t *def = &m4_conf_registry[idx];
	char expanded[M4_CONF_MAX_VALUE];

	switch (def->type) {
	case M4_CONF_T_BOOL: {
		bool ok, v;

		v = m4_conf_parse_bool(raw_value, &ok);
		if (!ok) {
			add_warning(c, false, M4_T("%s:%d: %s.%s = \"%s\": expected on|off",
"%s:%d: %s.%s = \"%s\": ожидается on|off"), origin, lineno,
				    def->section, def->key, raw_value);
			return false;
		}
		snprintf(c->entries[idx].value, M4_CONF_MAX_VALUE, "%s", v ? "on" : "off");
		break;
	}
	case M4_CONF_T_INT: {
		char *end = NULL;
		long v;

		v = strtol(raw_value, &end, 10);
		if (end == raw_value || *end != '\0' || (double)v < def->min || (double)v > def->max) {
			add_warning(c, false, M4_T("%s:%d: %s.%s = \"%s\": expected an integer %g..%g",
"%s:%d: %s.%s = \"%s\": ожидается целое %g..%g"), origin, lineno,
				    def->section, def->key, raw_value, def->min, def->max);
			return false;
		}
		snprintf(c->entries[idx].value, M4_CONF_MAX_VALUE, "%ld", v);
		break;
	}
	case M4_CONF_T_DOUBLE: {
		char *end = NULL;
		double v;

		v = strtod(raw_value, &end);
		/* strtod() itself accepts "nan"/"inf"/"infinity" (C99 7.20.1.3) --
		 * every comparison against a NaN is false, so `v < min || v > max`
		 * would silently ACCEPT "nan" as being "in range". isnan()/isinf()
		 * reject both explicitly; no key in the registry is meant to hold
		 * either. */
		if (end == raw_value || *end != '\0' || isnan(v) || isinf(v) || v < def->min || v > def->max) {
			add_warning(c, false, M4_T("%s:%d: %s.%s = \"%s\": expected a number %g..%g",
"%s:%d: %s.%s = \"%s\": ожидается число %g..%g"), origin, lineno,
				    def->section, def->key, raw_value, def->min, def->max);
			return false;
		}
		snprintf(c->entries[idx].value, M4_CONF_MAX_VALUE, "%.6g", v);
		break;
	}
	case M4_CONF_T_STRING: {
		if (strlen(raw_value) >= sizeof(expanded)) {
			add_warning(c, false, M4_T("%s:%d: %s.%s: value longer than %d bytes",
"%s:%d: %s.%s: значение длиннее %d байт"), origin, lineno,
				    def->section, def->key, M4_CONF_MAX_VALUE - 1);
			return false;
		}
		m4_conf_expand(raw_value, expanded, sizeof(expanded));
		snprintf(c->entries[idx].value, M4_CONF_MAX_VALUE, "%s", expanded);
		break;
	}
	case M4_CONF_T_ENUM: {
		/* empty is always accepted (means "unset" -- e.g. control.gesture-play/
		 * -pause default to control.gesture-playpause, see m4-conf-keys.def) */
		if (raw_value[0] == '\0') {
			c->entries[idx].value[0] = '\0';
			break;
		}
		if (def->enum_values != NULL) {
			const char *p = def->enum_values;
			bool matched = false;

			while (*p != '\0') {
				const char *bar = strchr(p, '|');
				size_t len = bar != NULL ? (size_t)(bar - p) : strlen(p);

				if (len == strlen(raw_value) && strncasecmp(p, raw_value, len) == 0) {
					snprintf(c->entries[idx].value, M4_CONF_MAX_VALUE, "%.*s", (int)len, p);
					matched = true;
					break;
				}
				p = bar != NULL ? bar + 1 : p + len;
			}
			if (!matched) {
				add_warning(c, false, M4_T("%s:%d: %s.%s = \"%s\": expected one of (%s)",
"%s:%d: %s.%s = \"%s\": ожидается одно из (%s)"), origin,
					    lineno, def->section, def->key, raw_value, def->enum_values);
				return false;
			}
		} else {
			snprintf(c->entries[idx].value, M4_CONF_MAX_VALUE, "%s", raw_value);
		}
		break;
	}
	default:
		return false;
	}

	snprintf(c->entries[idx].source, M4_CONF_MAX_SOURCE, "%s", origin);
	c->entries[idx].source_line = lineno;
	return true;
}

/* --- init / one line / buffer / file / layers ------------------------------ */

void m4_conf_init(m4_conf_t *c)
{
	size_t i;

	memset(c, 0, sizeof(*c));
	/* Defensive: if the registry ever outgrows entries[], every index past
	 * this point would corrupt m4_conf_t itself (this is the ONE runtime
	 * check this whole malloc-free, fixed-buffer file relies on -- see the
	 * M4_CONF_ENTRIES_CAP comment above). Both m4-conf.c's own unit tests
	 * (common/tests/test_m4_conf.c) and any regular build immediately
	 * exercise this via m4_conf_init(), so it cannot silently ship broken. */
	if (m4_conf_registry_count > M4_CONF_ENTRIES_CAP) {
		fprintf(stderr,
			"m4-conf: BUG: registry has %zu keys, m4_conf_t.entries[] holds only %zu -- "
			"grow the array in common/m4-conf.h\n",
			m4_conf_registry_count, (size_t)M4_CONF_ENTRIES_CAP);
		abort();
	}
	for (i = 0; i < m4_conf_registry_count; i++)
		apply_value(c, i, m4_conf_registry[i].default_value, "default", 0);
}

bool m4_conf_set(m4_conf_t *c, const char *section, const char *key, const char *value,
		  const char *source_label)
{
	int idx = m4_conf_find(section, key);

	if (idx < 0) {
		add_warning(c, false, M4_T("%s: unknown key %s.%s",
"%s: неизвестный ключ %s.%s"), source_label != NULL ? source_label : "set",
			    section, key);
		return false;
	}
	return apply_value(c, (size_t)idx, value,
			    source_label != NULL ? source_label : "set", 0);
}

/* One already-trimmed, non-empty, non-comment line: either "[section]" or
 * "key = value". `cur_section`/`cur_section_cap` persist across calls for
 * one load (buffer or file). */
static void handle_line(m4_conf_t *c, char *line, const char *origin, int lineno, char *cur_section,
			 size_t cur_section_cap)
{
	size_t len = strlen(line);

	if (line[0] == '[' && line[len - 1] == ']') {
		size_t n = len - 2;

		if (n >= cur_section_cap)
			n = cur_section_cap - 1;
		memcpy(cur_section, line + 1, n);
		cur_section[n] = '\0';
		lower_inplace(cur_section);
		return;
	}

	{
		char *eq = strchr(line, '=');
		char *key, *value;
		int idx;

		if (eq == NULL) {
			add_warning(c, false, M4_T("%s:%d: no '=' in line \"%s\"",
"%s:%d: нет '=' в строке \"%s\""), origin, lineno, line);
			return;
		}
		*eq = '\0';
		key = trim(line);
		value = trim(eq + 1);
		if (value[0] == '"' && strlen(value) >= 2 && value[strlen(value) - 1] == '"') {
			value[strlen(value) - 1] = '\0';
			value++;
		}
		if (*key == '\0') {
			add_warning(c, false, M4_T("%s:%d: empty key",
"%s:%d: пустой ключ"), origin, lineno);
			return;
		}
		if (cur_section[0] == '\0') {
			add_warning(c, false, M4_T("%s:%d: \"%s\" outside a [device]/[adaptive]/... section -- skipped",
"%s:%d: \"%s\" вне секции [device]/[adaptive]/... -- пропущено"),
				    origin, lineno, key);
			return;
		}
		idx = m4_conf_find(cur_section, key);
		if (idx < 0) {
			add_warning(c, false, M4_T("%s:%d: unknown key [%s] %s",
"%s:%d: неизвестный ключ [%s] %s"), origin, lineno, cur_section, key);
			return;
		}
		apply_value(c, (size_t)idx, value, origin, lineno);
	}
}

void m4_conf_load_buffer(m4_conf_t *c, const char *text, const char *source_label)
{
	const char *p = text != NULL ? text : "";
	int lineno = 0;
	char cur_section[M4_CONF_MAX_SECTION] = "";
	const char *origin = source_label != NULL ? source_label : "<buffer>";

	while (*p != '\0') {
		const char *nl = strchr(p, '\n');
		size_t len = nl != NULL ? (size_t)(nl - p) : strlen(p);
		char line[512];
		char *l;

		lineno++;
		if (len >= sizeof(line)) {
			add_warning(c, false, M4_T("%s:%d: line too long",
"%s:%d: слишком длинная строка"), origin, lineno);
			p = nl != NULL ? nl + 1 : p + len;
			continue;
		}
		memcpy(line, p, len);
		line[len] = '\0';
		p = nl != NULL ? nl + 1 : p + len;

		l = trim(line);
		if (*l == '\0' || *l == '#' || *l == ';')
			continue;
		handle_line(c, l, origin, lineno, cur_section, sizeof(cur_section));
	}
}

int m4_conf_load_file(m4_conf_t *c, const char *path)
{
	FILE *f;
	char cur_section[M4_CONF_MAX_SECTION] = "";
	char line[512];
	int lineno = 0;

	if (path == NULL || path[0] == '\0')
		return 0;

	f = fopen(path, "re");
	if (f == NULL) {
		if (errno == ENOENT)
			return 0;
		add_warning(c, false, M4_T("%s: could not be read (%s)",
"%s: не читается (%s)"), path, strerror(errno));
		return -1;
	}

	while (fgets(line, sizeof(line), f) != NULL) {
		char *nl = strchr(line, '\n');
		char *l;

		lineno++;
		if (nl == NULL && !feof(f)) {
			/* line longer than sizeof(line) -- warn once, then resync by
			 * reading (and discarding) up to the next '\n' before
			 * resuming normal line-at-a-time parsing. */
			int ch;

			add_warning(c, false, M4_T("%s:%d: line too long",
"%s:%d: слишком длинная строка"), path, lineno);
			while ((ch = fgetc(f)) != EOF && ch != '\n')
				;
			continue;
		}
		if (nl != NULL)
			*nl = '\0';
		l = trim(line);
		if (*l == '\0' || *l == '#' || *l == ';')
			continue;
		handle_line(c, l, path, lineno, cur_section, sizeof(cur_section));
	}
	fclose(f);
	return 1;
}

int m4_conf_load_layers(m4_conf_t *c)
{
	char path[M4_CONF_MAX_VALUE];
	int n = 0;
	const char *override;

	m4_conf_init(c);

	/* layer 2: @datadir@/momentum4/momentum4.conf -- $MOMENTUM4_DATADIR
	 * (test-only, see m4-conf.h's "Layering" doc above) replaces @datadir@
	 * itself, so a test can point this layer at an empty scratch directory
	 * instead of depending on whether a real install happens to have left
	 * a reference copy at the compiled-in @datadir@ on whatever machine
	 * runs the test suite. */
	override = getenv("MOMENTUM4_DATADIR");
	if (override != NULL && override[0] != '\0')
		snprintf(path, sizeof(path), "%s/momentum4/momentum4.conf", override);
	else
		m4_conf_expand("@datadir@/momentum4/momentum4.conf", path, sizeof(path));
	if (m4_conf_load_file(c, path) == 1)
		n++;

	/* layer 3: /etc/momentum4/momentum4.conf -- a plain literal, not
	 * @-substituted, so $MOMENTUM4_DATADIR above cannot reach it.
	 * $MOMENTUM4_ETC_CONF (test-only) replaces the whole path, the same
	 * way $MOMENTUM4_CONFIG below replaces layer 4's whole path. */
	override = getenv("MOMENTUM4_ETC_CONF");
	if (m4_conf_load_file(c, (override != NULL && override[0] != '\0')
					  ? override
					  : "/etc/momentum4/momentum4.conf") == 1)
		n++;

	override = getenv("MOMENTUM4_CONFIG");
	if (override != NULL && override[0] != '\0')
		snprintf(path, sizeof(path), "%s", override);
	else
		m4_conf_expand("${XDG_CONFIG_HOME}/momentum4/momentum4.conf", path, sizeof(path));
	if (m4_conf_load_file(c, path) == 1)
		n++;

	c->n_files_loaded = n;
	return n;
}

/* --- accessors -------------------------------------------------------------- */

bool m4_conf_get_bool_i(const m4_conf_t *c, int idx, bool fallback)
{
	bool ok, v;

	if (idx < 0 || (size_t)idx >= m4_conf_registry_count)
		return fallback;
	v = m4_conf_parse_bool(c->entries[idx].value, &ok);
	return ok ? v : fallback;
}

long m4_conf_get_int_i(const m4_conf_t *c, int idx, long fallback)
{
	char *end = NULL;
	long v;

	if (idx < 0 || (size_t)idx >= m4_conf_registry_count)
		return fallback;
	v = strtol(c->entries[idx].value, &end, 10);
	if (end == c->entries[idx].value || *end != '\0')
		return fallback;
	return v;
}

double m4_conf_get_double_i(const m4_conf_t *c, int idx, double fallback)
{
	char *end = NULL;
	double v;

	if (idx < 0 || (size_t)idx >= m4_conf_registry_count)
		return fallback;
	v = strtod(c->entries[idx].value, &end);
	if (end == c->entries[idx].value || *end != '\0')
		return fallback;
	return v;
}

const char *m4_conf_get_string_i(const m4_conf_t *c, int idx)
{
	if (idx < 0 || (size_t)idx >= m4_conf_registry_count)
		return NULL;
	return c->entries[idx].value;
}

bool m4_conf_get_bool(const m4_conf_t *c, const char *section, const char *key, bool fallback)
{
	return m4_conf_get_bool_i(c, m4_conf_find(section, key), fallback);
}

long m4_conf_get_int(const m4_conf_t *c, const char *section, const char *key, long fallback)
{
	return m4_conf_get_int_i(c, m4_conf_find(section, key), fallback);
}

double m4_conf_get_double(const m4_conf_t *c, const char *section, const char *key, double fallback)
{
	return m4_conf_get_double_i(c, m4_conf_find(section, key), fallback);
}

const char *m4_conf_get_string(const m4_conf_t *c, const char *section, const char *key)
{
	return m4_conf_get_string_i(c, m4_conf_find(section, key));
}

/* --- config show ------------------------------------------------------------ */

__attribute__((format(printf, 4, 5)))
static void show_append(char *out, size_t out_cap, size_t *used, const char *fmt, ...)
{
	va_list ap;
	size_t off = *used < out_cap ? *used : out_cap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(out + off, out_cap - off, fmt, ap);
	va_end(ap);
	if (n > 0)
		*used += (size_t)n;
}

/* --- reference file generation (m4_conf_write_reference(), see m4-conf.h) - */

/* Short blurb printed once, right after each "[section]" header -- purely
 * cosmetic, keep in sync with m4-conf-keys.def's own section grouping/order
 * by hand; an unknown section (should never happen) just gets no blurb. */
struct m4_conf_section_blurb {
	const char *section;
	const char *text_en;
	const char *text_ru;
};

static const struct m4_conf_section_blurb m4_conf_section_blurbs[] = {
	{ "general", "Interface language.", "Язык интерфейса." },
	{ "device", "How to find and recognize the Momentum 4.", "Как найти и опознать Momentum 4." },
	{ "adaptive", "aptX Adaptive: the SPA codec and the bridge to the helper (qemu-hexagon).",
	  "aptX Adaptive: SPA-кодек и мост к helper'у (qemu-hexagon)." },
	{ "helper", "The helper process (qemu-hexagon + CAPI encoder) -- `momentum4 helper-exec`.",
	  "Helper-процесс (qemu-hexagon + CAPI-энкодер) -- `momentum4 helper-exec`." },
	{ "hfp", "HFP endpoint patches (gestures/AT commands/SCO restore).",
	  "Патчи HFP-эндпоинта (жесты/AT-команды/восстановление SCO)." },
	{ "stats", "Live bridge metrics (adaptive-stats.json).", "Живые метрики моста (adaptive-stats.json)." },
	{ "daemon", "momentum4d core: mode, timings, repair-a2dp.", "Ядро momentum4d: режим, тайминги, repair-a2dp." },
	{ "call", "PipeWire/BlueZ profiles used by call mode.", "Профили PipeWire/BlueZ, используемые режимом звонка." },
	{ "automode", "Automatic call detection from microphone activity.",
	  "Автоматическое определение звонка по активности микрофона." },
	{ "control", "M4 gestures (tap/swipe/on-head sensor) and HFP AT commands -> actions.",
	  "Жесты M4 (тап/свайп/датчик ношения) и HFP AT-команды -> действия." },
	{ "earcon", "Sound notifications (.wav via pw-play).", "Звуковые уведомления (.wav через pw-play)." },
	{ "graph", "`momentum4 graph-host`: combined sink, microphone placeholder, quantum.",
	  "`momentum4 graph-host`: комбинированный sink, заглушка микрофона, quantum." },
	{ "paths", "Install/runtime paths (usually no need to touch).",
	  "Пути установки/рантайма (обычно трогать не нужно)." },
	{ "cli", "Timeouts of `momentum4 ...` commands (rarely needs changing).",
	  "Таймауты команд `momentum4 ...` (редко нужно менять)." },
};

static const char *m4_conf_blurb_for(const char *section)
{
	size_t i;

	for (i = 0; i < sizeof(m4_conf_section_blurbs) / sizeof(m4_conf_section_blurbs[0]); i++)
		if (strcmp(m4_conf_section_blurbs[i].section, section) == 0)
			return M4_T(m4_conf_section_blurbs[i].text_en, m4_conf_section_blurbs[i].text_ru);
	return NULL;
}

static void m4_conf_print_comment_block(FILE *out, const char *comment)
{
	const char *p = comment;

	while (p != NULL && *p != '\0') {
		const char *nl = strchr(p, '\n');
		size_t len = nl != NULL ? (size_t)(nl - p) : strlen(p);

		fprintf(out, "# %.*s\n", (int)len, p);
		p = nl != NULL ? nl + 1 : p + len;
	}
}

static void m4_conf_print_type_hint(FILE *out, const m4_conf_key_def_t *def)
{
	switch (def->type) {
	case M4_CONF_T_BOOL:
		fprintf(out, M4_T("# type: on|off\n", "# тип: on|off\n"));
		break;
	case M4_CONF_T_INT:
		fprintf(out, M4_T("# type: integer, %g..%g\n", "# тип: целое, %g..%g\n"), def->min, def->max);
		break;
	case M4_CONF_T_DOUBLE:
		fprintf(out, M4_T("# type: number, %g..%g\n", "# тип: число, %g..%g\n"), def->min, def->max);
		break;
	case M4_CONF_T_ENUM:
		fprintf(out, M4_T("# type: one of (%s)\n", "# тип: одно из (%s)\n"),
			def->enum_values != NULL ? def->enum_values : "");
		break;
	case M4_CONF_T_STRING:
	default:
		fprintf(out, M4_T("# type: string\n", "# тип: строка\n"));
		break;
	}
}

void m4_conf_write_reference(FILE *out)
{
	size_t i;
	char last_section[M4_CONF_MAX_SECTION] = "";

	fprintf(out,
		M4_T(
		"# momentum4.conf -- generated by common/gen-conf.c from common/m4-conf-keys.def,\n"
		"# DO NOT EDIT this file directly under @datadir@/momentum4/ -- it will be\n"
		"# overwritten by the next install. Copy the sections/keys you need into\n"
		"# /etc/momentum4/momentum4.conf (system overrides) or\n"
		"# ~/.config/momentum4/momentum4.conf (per-user) and uncomment them --\n"
		"# layers: compiled-in < this file < /etc/momentum4/momentum4.conf < ~/.config/....\n"
		"# Value substitutions: ${XDG_RUNTIME_DIR} ${XDG_CONFIG_HOME}\n"
		"# ${XDG_DATA_HOME} ${XDG_STATE_HOME} ${HOME} %%h (= $HOME).\n"
		"# `momentum4 config show` prints each key's current EFFECTIVE value and\n"
		"# the file it came from; `momentum4 config validate` checks a file for\n"
		"# errors without applying it.\n"
		"#\n"
		"# Every key below is commented out and shows its default value --\n"
		"# uncomment and change it to override.\n",
		"# momentum4.conf -- сгенерировано common/gen-conf.c из common/m4-conf-keys.def,\n"
		"# НЕ РЕДАКТИРУЙТЕ этот файл напрямую в @datadir@/momentum4/ -- он будет\n"
		"# перезаписан при следующей установке. Скопируйте нужные секции/ключи в\n"
		"# /etc/momentum4/momentum4.conf (системные переопределения) или\n"
		"# ~/.config/momentum4/momentum4.conf (пользовательские) и раскомментируйте --\n"
		"# слои: код < этот файл < /etc/momentum4/momentum4.conf < ~/.config/....\n"
		"# Подстановки в значениях: ${XDG_RUNTIME_DIR} ${XDG_CONFIG_HOME}\n"
		"# ${XDG_DATA_HOME} ${XDG_STATE_HOME} ${HOME} %%h (= $HOME).\n"
		"# `momentum4 config show` печатает текущее ЭФФЕКТИВНОЕ значение каждого\n"
		"# ключа и файл, из которого оно взято; `momentum4 config validate` проверяет\n"
		"# файл на ошибки без применения.\n"
		"#\n"
		"# Каждый ключ ниже закомментирован и показывает значение по умолчанию --\n"
		"# раскомментируйте и измените, чтобы переопределить.\n"));

	for (i = 0; i < m4_conf_registry_count; i++) {
		const m4_conf_key_def_t *def = &m4_conf_registry[i];

		if (strcmp(last_section, def->section) != 0) {
			const char *blurb = m4_conf_blurb_for(def->section);

			fprintf(out, "\n[%s]\n", def->section);
			if (blurb != NULL)
				fprintf(out, "# %s\n", blurb);
			snprintf(last_section, sizeof(last_section), "%s", def->section);
		}
		fprintf(out, "\n");
		m4_conf_print_comment_block(out, M4_T(def->comment_en, def->comment_ru));
		m4_conf_print_type_hint(out, def);
		fprintf(out, "# %s = %s\n", def->key, def->default_value);
	}
}

int m4_conf_format_show(const m4_conf_t *c, char *out, size_t out_cap)
{
	size_t used = 0;
	size_t i;
	char last_section[M4_CONF_MAX_SECTION] = "";

	if (out_cap > 0)
		out[0] = '\0';

	for (i = 0; i < m4_conf_registry_count; i++) {
		const m4_conf_key_def_t *def = &m4_conf_registry[i];

		if (!section_equal(last_section, def->section)) {
			show_append(out, out_cap, &used, "%s[%s]\n", i == 0 ? "" : "\n", def->section);
			snprintf(last_section, sizeof(last_section), "%s", def->section);
		}
		if (c->entries[i].source_line > 0)
			show_append(out, out_cap, &used, "%-28s = %-24s # %s:%d\n", def->key, c->entries[i].value,
				    c->entries[i].source, c->entries[i].source_line);
		else
			show_append(out, out_cap, &used, "%-28s = %-24s # %s\n", def->key, c->entries[i].value,
				    c->entries[i].source);
	}
	return (int)used;
}
