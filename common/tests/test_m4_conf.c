/* common/tests/test_m4_conf.c -- common/m4-conf.c unit tests. No
 * framework, same convention as every other test directory in this repo
 * (daemon/tests, module): a plain
 * main() that exits non-zero (message on stderr) on the first CHECK()
 * failure.
 */
#define _POSIX_C_SOURCE 200809L /* setenv/unsetenv (glibc hides them under
				  * plain -std=c99 without this -- m4-conf.c
				  * itself needs no such define, only this
				  * test does) */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "../m4-conf.h"
#include "../m4-i18n.h"

static int failures = 0;
static int checks = 0;

#define CHECK(cond, msg)                                                            \
	do {                                                                          \
		checks++;                                                             \
		if (!(cond)) {                                                        \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, (msg)); \
			failures++;                                                   \
		}                                                                     \
	} while (0)

/* --- registry self-check --------------------------------------------------- */

static void test_registry_self_consistent(void)
{
	size_t i;

	CHECK(m4_conf_registry_count > 100, "registry has >100 keys (expect roughly 110)");

	for (i = 0; i < m4_conf_registry_count; i++) {
		const m4_conf_key_def_t *def = &m4_conf_registry[i];

		CHECK(def->section != NULL && def->section[0] != '\0', "every key has a section");
		CHECK(def->key != NULL && def->key[0] != '\0', "every key has a name");
		CHECK(def->default_value != NULL, "every key has a default (possibly \"\")");
		CHECK(def->comment_en != NULL && def->comment_en[0] != '\0', "every key is documented (en)");
		CHECK(def->comment_ru != NULL && def->comment_ru[0] != '\0', "every key is documented (ru)");
		CHECK(strlen(def->section) < M4_CONF_MAX_SECTION, "section name fits M4_CONF_MAX_SECTION");
		CHECK(strlen(def->key) < M4_CONF_MAX_KEY, "key name fits M4_CONF_MAX_KEY");
		CHECK(strlen(def->default_value) < M4_CONF_MAX_VALUE, "default fits M4_CONF_MAX_VALUE");

		if (def->type == M4_CONF_T_ENUM && def->enum_values != NULL && def->default_value[0] != '\0') {
			/* the default itself, if non-empty, must be one of enum_values
			 * (gesture-play/-pause are the only two with an intentionally
			 * empty default -- see m4-conf-keys.def's own header comment) */
			const char *p = def->enum_values;
			bool matched = false;

			while (*p != '\0' && !matched) {
				const char *bar = strchr(p, '|');
				size_t len = bar != NULL ? (size_t)(bar - p) : strlen(p);

				if (len == strlen(def->default_value) &&
				    strncmp(p, def->default_value, len) == 0)
					matched = true;
				p = bar != NULL ? bar + 1 : p + len;
			}
			CHECK(matched, def->key);
		}
	}
}

static void test_no_duplicate_keys(void)
{
	size_t i, j;
	bool dup = false;

	for (i = 0; i < m4_conf_registry_count; i++) {
		for (j = i + 1; j < m4_conf_registry_count; j++) {
			if (strcasecmp(m4_conf_registry[i].section, m4_conf_registry[j].section) == 0 &&
			    strcasecmp(m4_conf_registry[i].key, m4_conf_registry[j].key) == 0) {
				fprintf(stderr, "duplicate key: [%s] %s\n", m4_conf_registry[i].section,
					m4_conf_registry[i].key);
				dup = true;
			}
		}
	}
	CHECK(!dup, "no duplicate (section,key) pairs in the registry");
}

/* every default (except the two intentionally-empty ENUMs) survives its own
 * type's validation unchanged -- guards against a future edit that adds a
 * default outside its own declared min/max or enum set. */
static void test_defaults_all_valid(void)
{
	m4_conf_t c;

	m4_conf_init(&c);
	CHECK(c.n_warnings == 0, "m4_conf_init() on the compiled-in registry produces zero warnings");
}

/* --- find / accessors -------------------------------------------------------- */

static void test_find(void)
{
	CHECK(m4_conf_find("adaptive", "mode") >= 0, "find adaptive.mode");
	CHECK(m4_conf_find("Adaptive", "MODE") >= 0, "find is case-insensitive");
	CHECK(m4_conf_find("adaptive", "helper_mode") == m4_conf_find("adaptive", "helper-mode"),
	      "find treats _ and - the same");
	CHECK(m4_conf_find("nope", "nope") == -1, "unknown section -> -1");
	CHECK(m4_conf_find("adaptive", "nope") == -1, "unknown key -> -1");
}

static void test_accessors_defaults(void)
{
	m4_conf_t c;

	m4_conf_init(&c);
	CHECK(strcmp(m4_conf_get_string(&c, "adaptive", "mode"), "ll0") == 0, "mode default ll0");
	CHECK(m4_conf_get_int(&c, "adaptive", "latency-adjust-ms", -999) == -15, "latency-adjust-ms default -15");
	CHECK(m4_conf_get_bool(&c, "adaptive", "heartbeat", false) == true, "heartbeat default on");
	CHECK(m4_conf_get_double(&c, "adaptive", "stall-ms", -1.0) == 50.0, "stall-ms default 50.0");
	CHECK(m4_conf_get_string(&c, "nope", "nope") == NULL, "unknown key -> NULL string");
	CHECK(m4_conf_get_int(&c, "nope", "nope", 42) == 42, "unknown key -> fallback int");
	CHECK(m4_conf_get_bool(&c, "nope", "nope", true) == true, "unknown key -> fallback bool");
}

/* --- INI parsing: sections, warnings, quoting -------------------------------- */

static void test_sections_and_unknown(void)
{
	m4_conf_t c;
	const char *text = "[adaptive]\n"
			    "mode = ll1\n"
			    "[bogus-section]\n"
			    "x = 1\n"
			    "[adaptive]\n" /* re-opening a section must still work */
			    "resync-period-min = 20\n";

	m4_conf_init(&c);
	m4_conf_load_buffer(&c, text, "t");
	CHECK(strcmp(m4_conf_get_string(&c, "adaptive", "mode"), "ll1") == 0, "mode set inside its section");
	CHECK(m4_conf_get_int(&c, "adaptive", "resync-period-min", -1) == 20,
	      "re-opened section still applies");
	CHECK(c.n_warnings == 1, "exactly one warning (the bogus section's one key)");
}

static void test_key_before_any_section(void)
{
	m4_conf_t c;

	m4_conf_init(&c);
	m4_conf_load_buffer(&c, "mode = ll1\n", "t");
	CHECK(c.n_warnings == 1, "a key before any [section] is one warning");
	CHECK(strcmp(m4_conf_get_string(&c, "adaptive", "mode"), "ll0") == 0, "...and does not apply");
}

static void test_quoting_and_comments(void)
{
	m4_conf_t c;
	const char *text = "; semicolon comment\n"
			    "# hash comment\n"
			    "\n"
			    "   \n" /* blank/whitespace-only line */
			    "[helper]\n"
			    "cpu-list = \"  0,1  \"\n";

	m4_conf_init(&c);
	m4_conf_load_buffer(&c, text, "t");
	CHECK(c.n_warnings == 0, "comments/blank lines produce no warnings");
	CHECK(strcmp(m4_conf_get_string(&c, "helper", "cpu-list"), "  0,1  ") == 0,
	      "quotes strip but preserve inner whitespace verbatim");
}

static void test_missing_equals_and_empty_key(void)
{
	m4_conf_t c;

	m4_conf_init(&c);
	m4_conf_load_buffer(&c, "[device]\nthis-has-no-equals-sign\n", "t");
	CHECK(c.n_warnings == 1, "line without '=' is one warning");

	m4_conf_init(&c);
	m4_conf_load_buffer(&c, "[device]\n = value\n", "t");
	CHECK(c.n_warnings == 1, "empty key is one warning");
}

/* --- type validation ---------------------------------------------------------- */

static void test_bool_validation(void)
{
	m4_conf_t c;

	m4_conf_init(&c);
	CHECK(m4_conf_set(&c, "adaptive", "heartbeat", "off", "t"), "bool off accepted");
	CHECK(m4_conf_get_bool(&c, "adaptive", "heartbeat", true) == false, "...and applied");
	CHECK(m4_conf_set(&c, "adaptive", "heartbeat", "YES", "t"), "bool YES (case-insensitive) accepted");
	CHECK(m4_conf_get_bool(&c, "adaptive", "heartbeat", false) == true, "...and applied");
	CHECK(!m4_conf_set(&c, "adaptive", "heartbeat", "maybe", "t"), "bool \"maybe\" rejected");
	CHECK(m4_conf_get_bool(&c, "adaptive", "heartbeat", false) == true,
	      "...previous valid value kept on rejection");
}

static void test_int_range(void)
{
	m4_conf_t c;

	m4_conf_init(&c);
	CHECK(m4_conf_set(&c, "adaptive", "resync-period-min", "0", "t"), "int at min accepted");
	CHECK(m4_conf_set(&c, "adaptive", "resync-period-min", "1440", "t"), "int at max accepted");
	CHECK(!m4_conf_set(&c, "adaptive", "resync-period-min", "1441", "t"), "int above max rejected");
	CHECK(!m4_conf_set(&c, "adaptive", "resync-period-min", "-1", "t"), "int below min rejected");
	CHECK(!m4_conf_set(&c, "adaptive", "resync-period-min", "5x", "t"), "int with trailing garbage rejected");
	CHECK(!m4_conf_set(&c, "adaptive", "resync-period-min", "", "t"), "empty int rejected");
	CHECK(!m4_conf_set(&c, "adaptive", "resync-period-min", "  ", "t"), "whitespace-only int rejected");
}

static void test_double_range(void)
{
	m4_conf_t c;

	m4_conf_init(&c);
	CHECK(m4_conf_set(&c, "adaptive", "stall-ms", "0.5", "t"), "double accepted");
	CHECK(m4_conf_get_double(&c, "adaptive", "stall-ms", -1) == 0.5, "...and applied");
	CHECK(!m4_conf_set(&c, "adaptive", "stall-ms", "-1", "t"), "double below min rejected");
	CHECK(!m4_conf_set(&c, "adaptive", "stall-ms", "1e9", "t"), "double above max rejected");
	CHECK(!m4_conf_set(&c, "adaptive", "stall-ms", "nan", "t"),
	      "the literal string \"nan\" is out of [0,10000] and rejected");
}

static void test_enum_validation(void)
{
	m4_conf_t c;

	m4_conf_init(&c);
	CHECK(m4_conf_set(&c, "adaptive", "mode", "ll1", "t"), "enum exact match");
	CHECK(m4_conf_set(&c, "adaptive", "mode", "LL0", "t"), "enum case-insensitive match");
	CHECK(strcmp(m4_conf_get_string(&c, "adaptive", "mode"), "ll0") == 0,
	      "...stored as the registry's canonical (lowercase) form");
	CHECK(!m4_conf_set(&c, "adaptive", "mode", "ll", "t"), "enum partial match rejected");
	CHECK(!m4_conf_set(&c, "adaptive", "mode", "ll01", "t"), "enum superset-string rejected");
	CHECK(!m4_conf_set(&c, "adaptive", "mode", "off", "t"), "value from a DIFFERENT key's enum rejected");
}

static void test_enum_empty_special_case(void)
{
	m4_conf_t c;

	m4_conf_init(&c);
	CHECK(strcmp(m4_conf_get_string(&c, "control", "gesture-play"), "") == 0,
	      "gesture-play defaults to empty (\"inherit from gesture-playpause\", by convention)");
	CHECK(m4_conf_set(&c, "control", "gesture-play", "call-on", "t"), "can still be set explicitly");
	CHECK(m4_conf_set(&c, "control", "gesture-play", "", "t"), "and explicitly reset to empty");
}

static void test_string_length_limit(void)
{
	m4_conf_t c;
	char huge[M4_CONF_MAX_VALUE + 10];

	memset(huge, 'x', sizeof(huge) - 1);
	huge[sizeof(huge) - 1] = '\0';

	m4_conf_init(&c);
	CHECK(!m4_conf_set(&c, "helper", "cpu-list", huge, "t"), "over-length string value rejected");
}

/* --- placeholder expansion ---------------------------------------------------- */

static void test_expand(void)
{
	char out[512];

	setenv("XDG_RUNTIME_DIR", "/run/user/4242", 1);
	unsetenv("XDG_CONFIG_HOME");
	setenv("HOME", "/home/tester", 1);

	m4_conf_expand("${XDG_RUNTIME_DIR}/momentum4/helper.sock", out, sizeof(out));
	CHECK(strcmp(out, "/run/user/4242/momentum4/helper.sock") == 0, "${XDG_RUNTIME_DIR} expands");

	m4_conf_expand("${XDG_CONFIG_HOME}/momentum4/momentum4.conf", out, sizeof(out));
	CHECK(strcmp(out, "/home/tester/.config/momentum4/momentum4.conf") == 0,
	      "${XDG_CONFIG_HOME} falls back to $HOME/.config when unset");

	m4_conf_expand("%h/.local/bin/momentum4", out, sizeof(out));
	CHECK(strcmp(out, "/home/tester/.local/bin/momentum4") == 0, "%h expands to $HOME");

	m4_conf_expand("@libdir@/momentum4/spa-plugins", out, sizeof(out));
	CHECK(strstr(out, "/lib/momentum4/spa-plugins") != NULL, "@libdir@ expands (build-time default)");

	m4_conf_expand("no placeholders here", out, sizeof(out));
	CHECK(strcmp(out, "no placeholders here") == 0, "plain text passes through unchanged");

	m4_conf_expand("literal $ and % and @ signs", out, sizeof(out));
	CHECK(strcmp(out, "literal $ and % and @ signs") == 0,
	      "lone $/%%/@ that do not form a recognised placeholder pass through unchanged");

	m4_conf_expand("${UNKNOWN_VAR}/x", out, sizeof(out));
	CHECK(strcmp(out, "/x") == 0, "unset/unknown env var expands to empty");

	/* truncation must never overflow/crash and must stay NUL-terminated */
	m4_conf_expand("${XDG_RUNTIME_DIR}/momentum4/helper.sock", out, 5);
	CHECK(strlen(out) == 4, "truncated to out_cap-1 bytes");

	unsetenv("XDG_RUNTIME_DIR");
	unsetenv("HOME");
}

/* --- file loading -------------------------------------------------------------- */

static void write_file(const char *path, const char *content)
{
	FILE *f = fopen(path, "w");
	fputs(content, f);
	fclose(f);
}

static void test_load_file(void)
{
	m4_conf_t c;
	char path[256];
	int rc;

	snprintf(path, sizeof(path), "/tmp/m4-conf-test-%d.conf", (int)getpid());
	write_file(path, "[adaptive]\nmode = ll1\n");

	m4_conf_init(&c);
	rc = m4_conf_load_file(&c, path);
	CHECK(rc == 1, "load_file returns 1 for an existing, readable file");
	CHECK(strcmp(m4_conf_get_string(&c, "adaptive", "mode"), "ll1") == 0, "...and applies it");
	unlink(path);

	m4_conf_init(&c);
	rc = m4_conf_load_file(&c, path); /* just unlinked -- ENOENT */
	CHECK(rc == 0, "load_file returns 0 (not an error) for ENOENT");
	CHECK(c.n_warnings == 0, "...and produces no warning for ENOENT");

	rc = m4_conf_load_file(&c, NULL);
	CHECK(rc == 0, "load_file(NULL) is a no-op, not a crash");
	rc = m4_conf_load_file(&c, "");
	CHECK(rc == 0, "load_file(\"\") is a no-op, not a crash");
}

static void test_load_file_overlong_line_resyncs(void)
{
	m4_conf_t c;
	char path[256];
	char content[2048];
	size_t off;

	snprintf(path, sizeof(path), "/tmp/m4-conf-test-long-%d.conf", (int)getpid());
	off = (size_t)snprintf(content, sizeof(content), "[helper]\ncpu-list = ");
	memset(content + off, 'x', 700);
	off += 700;
	off += (size_t)snprintf(content + off, sizeof(content) - off, "\nsocket = /tmp/ok.sock\n");
	write_file(path, content);

	m4_conf_init(&c);
	m4_conf_load_file(&c, path);
	CHECK(c.n_warnings == 1, "overlong line is discarded whole (one warning), never partially applied");
	CHECK(strcmp(m4_conf_get_string(&c, "helper", "socket"), "/tmp/ok.sock") == 0,
	      "parsing resumes correctly on the line AFTER an overlong one");
	unlink(path);
}

/* --- layering --------------------------------------------------------------- */

static void test_load_layers_isolated(void)
{
	m4_conf_t c;
	char dir[256], home[300], xdgbase[300], momdir[350];
	int n;

	snprintf(dir, sizeof(dir), "/tmp/m4-conf-test-layers-%d", (int)getpid());
	snprintf(home, sizeof(home), "%s/home", dir);
	/* XDG_CONFIG_HOME is the BASE config dir (like ~/.config) --
	 * m4_conf_load_layers() appends "/momentum4/momentum4.conf" itself. */
	snprintf(xdgbase, sizeof(xdgbase), "%s/xdgconfig", dir);
	snprintf(momdir, sizeof(momdir), "%s/momentum4", xdgbase);
	{
		char cmd[1200];

		snprintf(cmd, sizeof(cmd), "rm -rf '%s' && mkdir -p '%s' '%s'", dir, home, momdir);
		CHECK(system(cmd) == 0, "test scaffolding: mkdir -p");
	}
	{
		char path[400];

		snprintf(path, sizeof(path), "%s/momentum4.conf", momdir);
		write_file(path, "[device]\naddress = 11:22:33:44:55:66\n");
	}

	setenv("HOME", home, 1);
	setenv("XDG_CONFIG_HOME", xdgbase, 1);
	unsetenv("MOMENTUM4_CONFIG");
	/* point the @datadir@ and /etc layers at/under a directory that cannot
	 * possibly contain a real momentum4.conf, so this test is ACTUALLY
	 * isolated from whatever a previous `make install`/package left at the
	 * real, compiled-in @datadir@ or at the real /etc/momentum4/ on the
	 * machine running the test suite (without the env vars below, this was
	 * reproduced failing in an isolated container with a pre-existing
	 * /usr/local/share/momentum4/momentum4.conf on disk). */
	setenv("MOMENTUM4_DATADIR", dir, 1);
	{
		char etc_conf[400];

		snprintf(etc_conf, sizeof(etc_conf), "%s/no-such-etc-momentum4.conf", dir);
		setenv("MOMENTUM4_ETC_CONF", etc_conf, 1);
	}

	n = m4_conf_load_layers(&c);
	CHECK(n == 1, "exactly the user layer was found (datadir/etc isolated by env, not just absent)");
	CHECK(strcmp(m4_conf_get_string(&c, "device", "address"), "11:22:33:44:55:66") == 0,
	      "user layer applied");
	CHECK(c.n_files_loaded == n, "n_files_loaded matches the return value");

	unsetenv("HOME");
	unsetenv("XDG_CONFIG_HOME");
	unsetenv("MOMENTUM4_DATADIR");
	unsetenv("MOMENTUM4_ETC_CONF");
	{
		char cmd[300];

		snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
		system(cmd);
	}
}

/* --- config show -------------------------------------------------------------- */

static void test_format_show(void)
{
	m4_conf_t c;
	char buf[65536];
	int n;

	m4_conf_init(&c);
	m4_conf_set(&c, "adaptive", "mode", "ll1", "test-source");
	n = m4_conf_format_show(&c, buf, sizeof(buf));
	CHECK(n > 0 && (size_t)n < sizeof(buf), "format_show fits in a generous buffer");
	CHECK(strstr(buf, "[adaptive]") != NULL, "output has section headers");
	CHECK(strstr(buf, "mode") != NULL && strstr(buf, "ll1") != NULL, "output shows the overridden value");
	CHECK(strstr(buf, "test-source") != NULL, "output shows the source label");

	/* truncation: tiny buffer must not crash, must stay NUL-terminated, and
	 * must report the WOULD-BE length (snprintf-style) */
	{
		char tiny[8];
		int n2 = m4_conf_format_show(&c, tiny, sizeof(tiny));

		CHECK((size_t)strlen(tiny) < sizeof(tiny), "tiny buffer stays NUL-terminated");
		CHECK(n2 > n - 100, "would-be length is still reported even when truncated");
	}
}

/* --- bool parser -------------------------------------------------------------- */

static void test_parse_bool(void)
{
	bool ok;

	CHECK(m4_conf_parse_bool("1", &ok) == true && ok, "\"1\" -> true");
	CHECK(m4_conf_parse_bool("on", &ok) == true && ok, "\"on\" -> true");
	CHECK(m4_conf_parse_bool("TRUE", &ok) == true && ok, "\"TRUE\" -> true");
	CHECK(m4_conf_parse_bool("0", &ok) == false && ok, "\"0\" -> false");
	CHECK(m4_conf_parse_bool("off", &ok) == false && ok, "\"off\" -> false");
	CHECK(m4_conf_parse_bool("bogus", &ok) == false && !ok, "\"bogus\" -> ok=false");
	(void)m4_conf_parse_bool("bogus", NULL); /* must not crash with ok == NULL */
}

/* common/m4-i18n.h: m4_lang_parse()/m4_lang_name()/M4_T() and the registry's
 * own [general] language default (M4_LANG_EN == 0, m4_lang's own static
 * initializer, checked WITHOUT this test having called any language-
 * selection routine first -- see that header's own "safe by construction"
 * comment). Restores m4_lang to EN at the end regardless of outcome: every
 * other test in this file (and file order is not guaranteed by convention
 * elsewhere in this repo) implicitly assumes the default. */
static void test_i18n(void)
{
	m4_lang_t lang;

	CHECK(m4_lang == M4_LANG_EN, "m4_lang defaults to EN before any language selection");
	CHECK(strcmp(m4_lang_name(M4_LANG_EN), "en") == 0, "m4_lang_name(EN) == \"en\"");
	CHECK(strcmp(m4_lang_name(M4_LANG_RU), "ru") == 0, "m4_lang_name(RU) == \"ru\"");

	CHECK(m4_lang_parse("en", &lang) == 1 && lang == M4_LANG_EN, "parse \"en\"");
	CHECK(m4_lang_parse("EN", &lang) == 1 && lang == M4_LANG_EN, "parse \"EN\" (case-insensitive)");
	CHECK(m4_lang_parse("ru", &lang) == 1 && lang == M4_LANG_RU, "parse \"ru\"");
	CHECK(m4_lang_parse("RU", &lang) == 1 && lang == M4_LANG_RU, "parse \"RU\" (case-insensitive)");
	CHECK(m4_lang_parse("fr", &lang) == 0, "parse \"fr\" -> rejected");
	CHECK(m4_lang_parse("", &lang) == 0, "parse \"\" -> rejected");
	CHECK(m4_lang_parse(NULL, &lang) == 0, "parse NULL -> rejected, does not crash");

	CHECK(strcmp(M4_T("hello", "привет"), "hello") == 0,
	      "M4_T() returns the en branch while m4_lang == EN");
	m4_lang = M4_LANG_RU;
	CHECK(strcmp(M4_T("hello", "привет"), "привет") == 0,
	      "M4_T() returns the ru branch while m4_lang == RU");

	m4_i18n_init_from_conf(NULL);
	CHECK(m4_lang == M4_LANG_RU, "m4_i18n_init_from_conf(NULL) leaves m4_lang untouched");

	{
		m4_conf_t c;
		int idx;

		m4_conf_init(&c);
		idx = m4_conf_find("general", "language");
		CHECK(idx >= 0, "registry has general.language");
		snprintf(c.entries[idx].value, sizeof(c.entries[idx].value), "en");
		m4_i18n_init_from_conf(&c);
		CHECK(m4_lang == M4_LANG_EN, "m4_i18n_init_from_conf() picks up general.language = en");
		snprintf(c.entries[idx].value, sizeof(c.entries[idx].value), "ru");
		m4_i18n_init_from_conf(&c);
		CHECK(m4_lang == M4_LANG_RU, "m4_i18n_init_from_conf() picks up general.language = ru");
	}

	m4_lang = M4_LANG_EN; /* restore -- see this function's own header comment */
}

int main(void)
{
	test_registry_self_consistent();
	test_no_duplicate_keys();
	test_defaults_all_valid();
	test_find();
	test_accessors_defaults();
	test_sections_and_unknown();
	test_key_before_any_section();
	test_quoting_and_comments();
	test_missing_equals_and_empty_key();
	test_bool_validation();
	test_int_range();
	test_double_range();
	test_enum_validation();
	test_enum_empty_special_case();
	test_string_length_limit();
	test_expand();
	test_load_file();
	test_load_file_overlong_line_resyncs();
	test_load_layers_isolated();
	test_format_show();
	test_parse_bool();
	test_i18n();

	fprintf(stderr, "%d/%d checks passed\n", checks - failures, checks);
	return failures == 0 ? 0 : 1;
}
