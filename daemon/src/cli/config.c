/* cli/config.c -- see config.h.
 * `momentum4 config show|validate|migrate|defaults`, built on top of
 * common/m4-conf.{c,h} (the malloc-free, daemon+plugin-shared parser and
 * key registry) and, for `migrate` only, the two OLD, flat (no-[section])
 * file formats this project shipped before this parser: util/settings.h's
 * momentum4.conf grammar and module's aptx-adaptive.conf
 * grammar (m4-adaptive-config.h) -- see legacy_flat_load() below. Neither
 * old grammar is implemented in common/ itself (m4-conf.h's own
 * header comment: "kept out of common/ on purpose -- the legacy key sets
 * are daemon/plugin history, not something the SPA plugin build needs to
 * know about").
 */
#include "config.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../../../common/m4-conf.h"
#include "../../../common/m4-i18n.h"
#include "../util/conf_upsert.h"
#include "../util/config.h"
#include "../util/legacy_conf.h"

static void print_usage(FILE *out)
{
	fprintf(out,
		M4_T("usage: momentum4 config <subcommand>\n"
		"  show                 effective config (every layer applied) + each key's source\n"
		"  get <section>.<key>  print one key's current effective value\n"
		"  set <section>.<key> <value>  write one key into your momentum4.conf\n"
		"                        ($MOMENTUM4_CONFIG, else $XDG_CONFIG_HOME/momentum4/\n"
		"                        momentum4.conf, else ~/.config/momentum4/momentum4.conf)\n"
		"  validate [PATH]      check one file (PATH) or every layer (no PATH); return "
		"code 1 if there are warnings\n"
		"  defaults [--out PATH] fully commented reference with default "
		"values (stdout, or a file)\n"
		"  migrate [--apply]    migrate the old flat momentum4.conf/aptx-adaptive.conf "
		"to the new sectioned format; a dry run by default, changes nothing\n",
"usage: momentum4 config <subcommand>\n"
		"  show                 эффективный конфиг (все слои применены) + источник каждого ключа\n"
		"  get <секция>.<ключ>  печатает текущее эффективное значение одного ключа\n"
		"  set <секция>.<ключ> <значение>  записывает один ключ в ваш momentum4.conf\n"
		"                        ($MOMENTUM4_CONFIG, иначе $XDG_CONFIG_HOME/momentum4/\n"
		"                        momentum4.conf, иначе ~/.config/momentum4/momentum4.conf)\n"
		"  validate [PATH]      проверить один файл (PATH) или все слои (без PATH); код "
		"возврата 1, если есть предупреждения\n"
		"  defaults [--out PATH] полностью закомментированный справочник со значениями по "
		"умолчанию (stdout, либо файл)\n"
		"  migrate [--apply]    перенести старые momentum4.conf/aptx-adaptive.conf (плоские) "
		"в новый секционный формат; по умолчанию сухой прогон, ничего не меняет\n"));
}

/* --- config show ------------------------------------------------------------ */

static int cmd_show(int argc, char **argv)
{
	m4_conf_t c;
	char buf[65536];

	(void)argv;
	if (argc != 0) {
		fprintf(stderr, "usage: momentum4 config show\n");
		return 2;
	}
	m4_conf_load_layers(&c);
	m4_conf_format_show(&c, buf, sizeof(buf));
	fputs(buf, stdout);
	if (c.n_warnings > 0) {
		int i;

		fprintf(stderr, M4_T("\n%d warning%s while reading the config:\n",
"\n%d предупреждени%s при чтении конфига:\n"), c.n_warnings,
			c.n_warnings == 1 ? M4_T("",
"е") : (c.n_warnings < 5 ? M4_T("s",
"я") : M4_T("s",
"й")));
		for (i = 0; i < c.n_warnings && i < M4_CONF_MAX_WARNINGS; i++)
			fprintf(stderr, "  %s\n", c.warnings[i]);
	}
	return 0;
}

/* --- get/set: shared "<section>.<key>" splitter ------------------------------ */

/* Splits `arg` ("section.key") into `section`/`key` (both NUL-terminated,
 * M4_CONF_MAX_SECTION/M4_CONF_MAX_KEY-bounded, same limits the registry
 * itself enforces). Returns 0 on a well-formed split, -1 if `arg` has no
 * '.', starts/ends with one, or either half does not fit -- callers print
 * their own usage-style error on -1, this function never does (shared by
 * both cmd_get and cmd_set, whose error wording differs slightly). */
static int split_section_key(const char *arg, char *section, size_t section_cap, char *key, size_t key_cap)
{
	const char *dot = strchr(arg, '.');
	size_t section_len;

	if (dot == NULL || dot == arg || dot[1] == '\0')
		return -1;
	section_len = (size_t)(dot - arg);
	if (section_len >= section_cap || strlen(dot + 1) >= key_cap)
		return -1;
	snprintf(section, section_cap, "%.*s", (int)section_len, arg);
	snprintf(key, key_cap, "%s", dot + 1);
	return 0;
}

/* --- config get ---------------------------------------------------------------- */

static int cmd_get(int argc, char **argv)
{
	char section[M4_CONF_MAX_SECTION];
	char key[M4_CONF_MAX_KEY];
	int idx;
	m4_conf_t c;
	const char *value;

	if (argc != 1) {
		fprintf(stderr, "usage: momentum4 config get <section>.<key>\n");
		return 2;
	}
	if (split_section_key(argv[0], section, sizeof(section), key, sizeof(key)) < 0) {
		fprintf(stderr, M4_T("momentum4 config get: expected <section>.<key>, got \"%s\"\n",
"momentum4 config get: ожидается <секция>.<ключ>, получено \"%s\"\n"), argv[0]);
		return 2;
	}
	idx = m4_conf_find(section, key);
	if (idx < 0) {
		fprintf(stderr, M4_T("momentum4 config get: unknown key: %s.%s\n",
"momentum4 config get: неизвестный ключ: %s.%s\n"), section, key);
		return 1;
	}
	m4_conf_load_layers(&c);
	value = m4_conf_get_string_i(&c, idx);
	printf("%s\n", value != NULL ? value : "");
	return 0;
}

/* --- config set ---------------------------------------------------------------- */

static int cmd_set(int argc, char **argv)
{
	char section[M4_CONF_MAX_SECTION];
	char key[M4_CONF_MAX_KEY];
	char path[4096];
	struct m4_conf_upsert_kv kv;
	char err[256];
	char needle[M4_CONF_MAX_SECTION + M4_CONF_MAX_KEY + 2];
	m4_conf_t c;
	int idx, i;

	if (argc != 2) {
		fprintf(stderr, "usage: momentum4 config set <section>.<key> <value>\n");
		return 2;
	}
	if (split_section_key(argv[0], section, sizeof(section), key, sizeof(key)) < 0) {
		fprintf(stderr, M4_T("momentum4 config set: expected <section>.<key>, got \"%s\"\n",
"momentum4 config set: ожидается <секция>.<ключ>, получено \"%s\"\n"), argv[0]);
		return 2;
	}
	idx = m4_conf_find(section, key);
	if (idx < 0) {
		fprintf(stderr, M4_T("momentum4 config set: unknown key: %s.%s\n",
"momentum4 config set: неизвестный ключ: %s.%s\n"), section, key);
		return 1;
	}
	if (m4_config_file_path(path, sizeof(path)) == NULL) {
		fprintf(stderr, M4_T("momentum4 config set: could not determine the momentum4.conf path "
				      "(no $HOME/$XDG_CONFIG_HOME)\n",
"momentum4 config set: не удалось определить путь momentum4.conf "
				      "(нет $HOME/$XDG_CONFIG_HOME)\n"));
		return 1;
	}

	kv.section = section;
	kv.key = key;
	kv.value = argv[1];
	if (m4_conf_upsert(path, &kv, 1, err, sizeof(err)) < 0) {
		fprintf(stderr, M4_T("momentum4 config set: %s: %s\n",
"momentum4 config set: %s: %s\n"), path, err);
		return 1;
	}

	/* Re-read every layer (not just `path`) and surface a warning if THIS
	 * key's new value did not validate (out of range / not one of
	 * enum_values / ...) -- the file is still written either way (this
	 * project's "a typo in the config must not keep anything from
	 * starting" contract, common/m4-conf.h's own header comment), the
	 * warning is purely so `config set` itself does not look silently
	 * successful on an obvious mistake. */
	m4_conf_load_layers(&c);
	snprintf(needle, sizeof(needle), "%s.%s", section, key);
	for (i = 0; i < c.n_warnings && i < M4_CONF_MAX_WARNINGS; i++)
		if (strstr(c.warnings[i], needle) != NULL)
			fprintf(stderr, M4_T("WARN %s\n",
"ПРЕДУПРЕЖДЕНИЕ %s\n"), c.warnings[i]);

	printf(M4_T("%s.%s = %s (written to %s)\n",
"%s.%s = %s (записано в %s)\n"), section, key, argv[1], path);
	printf(M4_T("Run `momentum4 reload` (or restart momentum4d) for the daemon to pick this up.\n",
"Выполните `momentum4 reload` (или перезапустите momentum4d), чтобы демон подхватил это.\n"));
	return 0;
}

/* --- config validate --------------------------------------------------------- */

static int cmd_validate(int argc, char **argv)
{
	m4_conf_t c;
	int i;

	if (argc > 1) {
		fprintf(stderr, "usage: momentum4 config validate [PATH]\n");
		return 2;
	}

	if (argc == 1) {
		int rc;

		m4_conf_init(&c);
		rc = m4_conf_load_file(&c, argv[0]);
		if (rc == 0) {
			fprintf(stderr, M4_T("momentum4 config validate: %s: file not found\n",
"momentum4 config validate: %s: файл не найден\n"), argv[0]);
			return 1;
		}
		if (rc < 0) {
			fprintf(stderr, M4_T("momentum4 config validate: %s: could not be read\n",
"momentum4 config validate: %s: не читается\n"), argv[0]);
			return 1;
		}
	} else {
		int n = m4_conf_load_layers(&c);

		printf(M4_T("layers found: %d (@datadir@/momentum4/momentum4.conf, "
		       "/etc/momentum4/momentum4.conf, ~/.config/momentum4/momentum4.conf)\n",
"слоёв найдено: %d (@datadir@/momentum4/momentum4.conf, "
		       "/etc/momentum4/momentum4.conf, ~/.config/momentum4/momentum4.conf)\n"),
		       n);
	}

	if (c.n_warnings == 0) {
		printf(M4_T("OK: no warnings\n",
"OK: предупреждений нет\n"));
		return 0;
	}
	printf(M4_T("%d warning%s:\n",
"%d предупреждени%s:\n"), c.n_warnings, c.n_warnings == 1 ? M4_T("",
"е") : (c.n_warnings < 5 ? M4_T("s",
"я") : M4_T("s",
"й")));
	for (i = 0; i < c.n_warnings && i < M4_CONF_MAX_WARNINGS; i++)
		printf("  %s\n", c.warnings[i]);
	return 1;
}

/* --- config defaults ---------------------------------------------------------- */

/* mkdir -p `path` itself (not just its parents -- despite the name kept
 * for its original call sites, which pass a directory, never a file). */
static int mkdir_parents(const char *path)
{
	char tmp[1024];
	char *p;

	snprintf(tmp, sizeof(tmp), "%s", path);
	for (p = tmp + 1; *p != '\0'; p++) {
		if (*p == '/') {
			*p = '\0';
			mkdir(tmp, 0755); /* ignore EEXIST and every other error here --
					    * the fopen() right after this function is the
					    * real, authoritative failure signal */
			*p = '/';
		}
	}
	mkdir(tmp, 0755); /* the final component itself -- the loop above only
			    * acts on '/' characters, so it creates every
			    * ancestor but never `path` itself. */
	return 0;
}

static int cmd_defaults(int argc, char **argv)
{
	const char *out_path = NULL;

	if (argc == 0) {
		/* stdout */
	} else if (argc == 2 && strcmp(argv[0], "--out") == 0) {
		out_path = argv[1];
	} else {
		fprintf(stderr, "usage: momentum4 config defaults [--out PATH]\n");
		return 2;
	}

	if (out_path == NULL) {
		m4_conf_write_reference(stdout);
		return 0;
	}

	{
		char dir[1024];
		char *slash;
		FILE *f;

		snprintf(dir, sizeof(dir), "%s", out_path);
		slash = strrchr(dir, '/');
		if (slash != NULL) {
			*slash = '\0';
			mkdir_parents(dir);
		}
		f = fopen(out_path, "we");
		if (f == NULL) {
			fprintf(stderr, "momentum4 config defaults: %s: %s\n", out_path, strerror(errno));
			return 1;
		}
		m4_conf_write_reference(f);
		fclose(f);
		printf(M4_T("written: %s\n",
"записано: %s\n"), out_path);
	}
	return 0;
}

/* --- config migrate ------------------------------------------------------------ */

/* Table + parser (m4_legacy_momentum4_conf_map / m4_legacy_aptx_adaptive_conf_map
 * / m4_legacy_conf_load()) moved to util/legacy_conf.{c,h} so
 * util/config.c's m4_config() can apply the SAME momentum4.conf mapping at
 * normal daemon startup (backward compatibility), not just here
 * in the explicit `migrate` command -- see that header's own comment.
 * cmd_migrate() below uses "migrate:%s:%d" as its own source_label prefix
 * (distinct from m4_config()'s "legacy:%s:%d") only so `config show`'s
 * source column can tell a value that came from an explicit `migrate --apply`
 * run apart from one merely read on every startup from a not-yet-migrated
 * file; both go through the exact same table and validation. */

static void print_unmapped(const char *key, const char *value, void *ud)
{
	int *count = ud;

	(*count)++;
	printf(M4_T("  ! not migrated (no match in the new registry, migrate it by hand): %s = %s\n",
"  ! не перенесён (нет соответствия в новом реестре, перенесите вручную): %s = %s\n"), key, value);
}

/* $M4_ADAPTIVE_CONFIG, else $XDG_CONFIG_HOME/momentum4/aptx-adaptive.conf,
 * else $HOME/.config/momentum4/aptx-adaptive.conf -- same precedence
 * m4-adaptive-config.h's own m4_ah_user_config_default_path() documents,
 * duplicated here (not called into module from the daemon --
 * the two components do not link against each other) rather than shared. */
static const char *legacy_aptx_adaptive_conf_path(char *buf, size_t buf_cap)
{
	const char *v = getenv("M4_ADAPTIVE_CONFIG");
	int n;

	if (v != NULL && v[0] != '\0') {
		n = snprintf(buf, buf_cap, "%s", v);
	} else if ((v = getenv("XDG_CONFIG_HOME")) != NULL && v[0] != '\0') {
		n = snprintf(buf, buf_cap, "%s/momentum4/aptx-adaptive.conf", v);
	} else if ((v = getenv("HOME")) != NULL && v[0] != '\0') {
		n = snprintf(buf, buf_cap, "%s/.config/momentum4/aptx-adaptive.conf", v);
	} else {
		return NULL;
	}
	if (n < 0 || (size_t)n >= buf_cap)
		return NULL;
	return buf;
}

/* Copies `src` to `src`+".v0.1" -- but ONLY if that backup does not already
 * exist (never overwrites an earlier backup: the FIRST migrate --apply run
 * is the one whose backup matters for rollback
 * own rollback recipe reads FROM ".v0.1", so a second accidental
 * `migrate --apply` must not silently replace it with already-migrated
 * content). Returns 1 if copied, 0 if skipped (backup already existed, or
 * `src` itself does not exist -- nothing to back up), -1 on a real I/O
 * error. */
static int backup_if_needed(const char *src)
{
	char dst[1200];
	FILE *in, *out;
	char buf[8192];
	size_t n;
	struct stat st;

	if (stat(src, &st) != 0)
		return 0; /* nothing to back up */
	snprintf(dst, sizeof(dst), "%s.v0.1", src);
	if (stat(dst, &st) == 0)
		return 0; /* earlier backup already exists -- do not touch it */

	in = fopen(src, "rb");
	if (in == NULL)
		return -1;
	out = fopen(dst, "wb");
	if (out == NULL) {
		fclose(in);
		return -1;
	}
	while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
		fwrite(buf, 1, n, out);
	fclose(in);
	fclose(out);
	return 1;
}

static int cmd_migrate(int argc, char **argv)
{
	bool apply = false;
	m4_conf_t merged;
	char momentum4_path[512];
	char aptx_path[512];
	int rc_m4, rc_aptx;
	int unmapped_count = 0;
	int total_applied = 0;

	if (argc == 1 && strcmp(argv[0], "--apply") == 0) {
		apply = true;
	} else if (argc != 0) {
		fprintf(stderr, "usage: momentum4 config migrate [--apply]\n");
		return 2;
	}

	if (m4_config_file_path(momentum4_path, sizeof(momentum4_path)) == NULL) {
		fprintf(stderr, M4_T("momentum4 config migrate: failed to determine the momentum4.conf path "
				 "(no $HOME/$XDG_CONFIG_HOME)\n",
"momentum4 config migrate: не удалось определить путь momentum4.conf "
				 "(нет $HOME/$XDG_CONFIG_HOME)\n"));
		return 1;
	}
	if (legacy_aptx_adaptive_conf_path(aptx_path, sizeof(aptx_path)) == NULL) {
		fprintf(stderr, M4_T("momentum4 config migrate: failed to determine the aptx-adaptive.conf path\n",
"momentum4 config migrate: не удалось определить путь aptx-adaptive.conf\n"));
		return 1;
	}

	/* Start from the CURRENT effective config (defaults + whatever @datadir@/
	 * /etc/ already apply) so keys the old files never mentioned keep their
	 * normal layered value -- only keys actually present in the legacy
	 * files are then overridden on top, matching the "config show BEFORE and
	 * AFTER match" (a read-only acceptance criterion): show's output for every
	 * key not touched by either legacy file is unaffected by migrate. */
	m4_conf_load_layers(&merged);

	printf("momentum4.conf: %s\n", momentum4_path);
	rc_m4 = m4_legacy_conf_load(momentum4_path, m4_legacy_momentum4_conf_map,
				     m4_legacy_momentum4_conf_map_count, &merged, "migrate", print_unmapped,
				     &unmapped_count);
	if (rc_m4 == -1) {
		printf(M4_T("  missing -- nothing to migrate\n",
"  отсутствует -- нечего переносить\n"));
	} else if (rc_m4 == -2) {
		fprintf(stderr, M4_T("  could not be read: %s\n",
"  не читается: %s\n"), strerror(errno));
		return 1;
	} else if (rc_m4 == -3) {
		printf(M4_T("  already in the new (sectioned) format -- skipped\n",
"  уже в новом (секционном) формате -- пропущен\n"));
		rc_m4 = 0;
	} else {
		printf(M4_T("  keys migrated: %d\n",
"  перенесено ключей: %d\n"), rc_m4);
		total_applied += rc_m4;
	}

	printf("aptx-adaptive.conf: %s\n", aptx_path);
	rc_aptx = m4_legacy_conf_load(aptx_path, m4_legacy_aptx_adaptive_conf_map,
				       m4_legacy_aptx_adaptive_conf_map_count, &merged, "migrate", print_unmapped,
				       &unmapped_count);
	if (rc_aptx == -1) {
		printf(M4_T("  missing -- nothing to migrate\n",
"  отсутствует -- нечего переносить\n"));
	} else if (rc_aptx == -2) {
		fprintf(stderr, M4_T("  could not be read: %s\n",
"  не читается: %s\n"), strerror(errno));
		return 1;
	} else if (rc_aptx == -3) {
		printf(M4_T("  already in the new (sectioned) format -- skipped\n",
"  уже в новом (секционном) формате -- пропущен\n"));
		rc_aptx = 0;
	} else {
		printf(M4_T("  keys migrated: %d\n",
"  перенесено ключей: %d\n"), rc_aptx);
		total_applied += rc_aptx;
	}

	if (total_applied == 0 && unmapped_count == 0) {
		printf(M4_T("\nNothing to migrate (both files are missing, or already in the new format).\n",
"\nПереносить нечего (оба файла отсутствуют или уже в новом формате).\n"));
		return 0;
	}

	if (!apply) {
		printf(M4_T("\nDRY RUN -- nothing written. Run "
		       "`momentum4 config migrate --apply` to apply it (a backup of "
		       "momentum4.conf.v0.1 / aptx-adaptive.conf.v0.1 will be made).\n",
"\nСУХОЙ ПРОГОН -- ничего не записано. Запустите "
		       "`momentum4 config migrate --apply`, чтобы применить (будет сделан "
		       "бэкап momentum4.conf.v0.1 / aptx-adaptive.conf.v0.1).\n"));
		return 0;
	}

	{
		int rc;

		rc = backup_if_needed(momentum4_path);
		if (rc < 0) {
			fprintf(stderr, M4_T("momentum4 config migrate: failed to create the %s.v0.1 backup: %s\n",
"momentum4 config migrate: не удалось создать бэкап %s.v0.1: %s\n"),
				momentum4_path, strerror(errno));
			return 1;
		}
		printf(M4_T("\nmomentum4.conf backup: %s\n",
"\nбэкап momentum4.conf: %s\n"), rc == 1 ? M4_T("created (.v0.1)",
"создан (.v0.1)") : M4_T("already existed/nothing to back up",
"уже существовал/нечего было"));

		rc = backup_if_needed(aptx_path);
		if (rc < 0) {
			fprintf(stderr, M4_T("momentum4 config migrate: failed to create the %s.v0.1 backup: %s\n",
"momentum4 config migrate: не удалось создать бэкап %s.v0.1: %s\n"), aptx_path,
				strerror(errno));
			return 1;
		}
		printf(M4_T("aptx-adaptive.conf backup: %s\n",
"бэкап aptx-adaptive.conf: %s\n"), rc == 1 ? M4_T("created (.v0.1)",
"создан (.v0.1)") : M4_T("already existed/nothing to back up",
"уже существовал/нечего было"));
	}

	{
		char dir[512];
		char *slash;
		char tmp_path[560];
		FILE *f;
		size_t i;
		char last_section[M4_CONF_MAX_SECTION] = "";

		snprintf(dir, sizeof(dir), "%s", momentum4_path);
		slash = strrchr(dir, '/');
		if (slash != NULL) {
			*slash = '\0';
			mkdir_parents(dir);
		}
		snprintf(tmp_path, sizeof(tmp_path), "%s.tmp-migrate", momentum4_path);
		f = fopen(tmp_path, "we");
		if (f == NULL) {
			fprintf(stderr, "momentum4 config migrate: %s: %s\n", tmp_path, strerror(errno));
			return 1;
		}
		/* Only the keys legacy files actually set, plus a short header --
		 * NOT a full dump of every default (that is `config defaults`'s
		 * job) -- keeps the migrated file small and diffable. Written
		 * directly from the registry (not m4_conf_format_show(), whose
		 * "key = value    # source:line" layout is for human display only
		 * and is NOT valid INI -- the trailing "# source" comment would
		 * become part of `value` on the next load, since this project's new
		 * sectioned format deliberately has no trailing-comment-after-value
		 * syntax, see m4-conf.h's own file-format doc comment). Only
		 * sections that actually gained at least one migrated key are
		 * printed (empty `[section]` headers with nothing under them are
		 * valid but pointless clutter in a migration output). */
		fprintf(f,
			M4_T("# momentum4.conf -- migrated by `momentum4 config migrate --apply` from the old\n"
			"# momentum4.conf/aptx-adaptive.conf (kept alongside as .v0.1). Full\n"
			"# commented reference of every key: `momentum4 config defaults`.\n",
"# momentum4.conf -- перенесено `momentum4 config migrate --apply` из старых\n"
			"# momentum4.conf/aptx-adaptive.conf (сохранены как .v0.1 рядом). Полный\n"
			"# закомментированный справочник всех ключей: `momentum4 config defaults`.\n"));
		for (i = 0; i < m4_conf_registry_count; i++) {
			const m4_conf_key_def_t *def = &m4_conf_registry[i];

			if (strncmp(merged.entries[i].source, "migrate:", 8) != 0)
				continue;
			if (strcmp(last_section, def->section) != 0) {
				fprintf(f, "\n[%s]\n", def->section);
				snprintf(last_section, sizeof(last_section), "%s", def->section);
			}
			fprintf(f, "%s = %s\n", def->key, merged.entries[i].value);
		}
		if (fflush(f) != 0 || ferror(f) != 0) {
			fprintf(stderr, M4_T("momentum4 config migrate: %s: write error: %s\n",
"momentum4 config migrate: %s: ошибка записи: %s\n"), tmp_path,
				strerror(errno));
			fclose(f);
			remove(tmp_path);
			return 1;
		}
		fclose(f);
		if (rename(tmp_path, momentum4_path) != 0) {
			fprintf(stderr, "momentum4 config migrate: rename(%s, %s): %s\n", tmp_path, momentum4_path,
				strerror(errno));
			remove(tmp_path);
			return 1;
		}
	}

	printf(M4_T("\nNew %s written (%d key%s). Rollback: cp %s.v0.1 %s && cp %s.v0.1 %s && momentum4 reload\n",
"\nЗаписан новый %s (%d ключ%s). Откат: cp %s.v0.1 %s && cp %s.v0.1 %s && momentum4 reload\n"),
	       momentum4_path, total_applied, total_applied == 1 ? "" : M4_T("s",
"ей"), momentum4_path, momentum4_path,
	       aptx_path, aptx_path);
	if (unmapped_count > 0)
		printf(M4_T("%d key%s not migrated%s automatically -- see the warnings above.\n",
"%d ключ%s не перенесён%s автоматически -- см. предупреждения выше.\n"), unmapped_count,
		       unmapped_count == 1 ? "" : M4_T("s",
"ей"), unmapped_count == 1 ? "" : M4_T("",
"ы"));
	return 0;
}

/* --- dispatch ------------------------------------------------------------------ */

int m4_cli_config_run(int argc, char **argv)
{
	if (argc < 1) {
		print_usage(stderr);
		return 2;
	}
	if (strcmp(argv[0], "show") == 0)
		return cmd_show(argc - 1, argv + 1);
	if (strcmp(argv[0], "get") == 0)
		return cmd_get(argc - 1, argv + 1);
	if (strcmp(argv[0], "set") == 0)
		return cmd_set(argc - 1, argv + 1);
	if (strcmp(argv[0], "validate") == 0)
		return cmd_validate(argc - 1, argv + 1);
	if (strcmp(argv[0], "defaults") == 0)
		return cmd_defaults(argc - 1, argv + 1);
	if (strcmp(argv[0], "migrate") == 0)
		return cmd_migrate(argc - 1, argv + 1);
	if (strcmp(argv[0], "-h") == 0 || strcmp(argv[0], "--help") == 0) {
		print_usage(stdout);
		return 0;
	}
	fprintf(stderr, M4_T("momentum4 config: unknown subcommand: %s\n",
"momentum4 config: неизвестная подкоманда: %s\n"), argv[0]);
	print_usage(stderr);
	return 2;
}
