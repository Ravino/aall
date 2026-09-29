/* util/conf_upsert.c -- see conf_upsert.h. */
#include "conf_upsert.h"

#include "../../../common/m4-i18n.h"

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define M4_UPSERT_MAX_KV 16
#define M4_UPSERT_LINE_LEN 4096

static void rstrip(char *s)
{
	size_t n = strlen(s);

	while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) {
		s[--n] = '\0';
	}
}

static bool line_is_section(const char *line, char *out_name, size_t out_len)
{
	const char *p = line;
	const char *close;

	while (*p == ' ' || *p == '\t')
		p++;
	if (*p != '[')
		return false;
	close = strchr(p, ']');
	if (close == NULL)
		return false;
	p++;
	{
		size_t n = (size_t)(close - p);

		if (n >= out_len)
			n = out_len - 1;
		memcpy(out_name, p, n);
		out_name[n] = '\0';
	}
	return true;
}

/* Returns true and fills out_key if `line` looks like "key = value" (or
 * "key=value"), ignoring leading whitespace -- comments ('#'/';') and
 * blank lines are never keys. */
static bool line_is_key(const char *line, char *out_key, size_t out_len)
{
	const char *p = line;
	const char *eq;
	const char *end;

	while (*p == ' ' || *p == '\t')
		p++;
	if (*p == '\0' || *p == '#' || *p == ';' || *p == '[')
		return false;
	eq = strchr(p, '=');
	if (eq == NULL)
		return false;
	end = eq;
	while (end > p && (end[-1] == ' ' || end[-1] == '\t'))
		end--;
	if (end == p)
		return false;
	{
		size_t n = (size_t)(end - p);

		if (n >= out_len)
			n = out_len - 1;
		memcpy(out_key, p, n);
		out_key[n] = '\0';
	}
	return true;
}

/* Extracts and trims the value part of a "key = value" line already
 * confirmed by line_is_key() (same '=' rule). Trailing whitespace only --
 * this project's INI grammar has no trailing-comment-after-value syntax
 * (see common/m4-conf.h), so nothing after the last non-space character is
 * ever stripped as a comment. */
static void line_value(const char *line, char *out, size_t out_len)
{
	const char *eq = strchr(line, '=');
	const char *p;
	const char *end;

	out[0] = '\0';
	if (eq == NULL)
		return;
	p = eq + 1;
	while (*p == ' ' || *p == '\t')
		p++;
	end = p + strlen(p);
	while (end > p && (end[-1] == ' ' || end[-1] == '\t'))
		end--;
	{
		size_t n = (size_t)(end - p);

		if (n >= out_len)
			n = out_len - 1;
		memcpy(out, p, n);
		out[n] = '\0';
	}
}

static int mkdir_parents(const char *path)
{
	char tmp[1024];
	char *p;

	snprintf(tmp, sizeof(tmp), "%s", path);
	for (p = tmp + 1; *p != '\0'; p++) {
		if (*p == '/') {
			*p = '\0';
			mkdir(tmp, 0755);
			*p = '/';
		}
	}
	mkdir(tmp, 0755);
	return 0;
}

/* Writes "key = value\n" for every not-yet-applied kv whose section matches
 * `section` (skipped ones -- NULL/"" value -- are counted as already
 * "applied" from the caller's point of view before this is ever called). */
static void flush_missing_for_section(FILE *out, const char *section, const struct m4_conf_upsert_kv *kvs,
				       size_t n_kvs, bool *applied, int *n_changed)
{
	size_t i;

	for (i = 0; i < n_kvs; i++) {
		if (applied[i])
			continue;
		if (strcmp(kvs[i].section, section) != 0)
			continue;
		fprintf(out, "%s = %s\n", kvs[i].key, kvs[i].value);
		applied[i] = true;
		(*n_changed)++;
	}
}

int m4_conf_upsert(const char *path, const struct m4_conf_upsert_kv *kvs, size_t n_kvs, char *err,
		    size_t err_len)
{
	FILE *in, *out;
	char tmp_path[1200];
	char line[M4_UPSERT_LINE_LEN];
	char current_section[256] = "";
	bool have_section = false;
	bool applied[M4_UPSERT_MAX_KV];
	bool section_seen[M4_UPSERT_MAX_KV];
	int n_changed = 0;
	size_t i;

	if (path == NULL || path[0] == '\0' || kvs == NULL) {
		if (err != NULL && err_len > 0)
			snprintf(err, err_len, M4_T("internal error: empty arguments",
"внутренняя ошибка: пустые аргументы"));
		return -1;
	}
	if (n_kvs > M4_UPSERT_MAX_KV) {
		if (err != NULL && err_len > 0)
			snprintf(err, err_len, M4_T("internal error: too many keys (%zu > %d)",
"внутренняя ошибка: слишком много ключей (%zu > %d)"), n_kvs,
				 M4_UPSERT_MAX_KV);
		return -1;
	}
	for (i = 0; i < n_kvs; i++) {
		applied[i] = (kvs[i].value == NULL || kvs[i].value[0] == '\0');
		section_seen[i] = false;
	}

	{
		char dir[1024];
		char *slash;

		snprintf(dir, sizeof(dir), "%s", path);
		slash = strrchr(dir, '/');
		if (slash != NULL) {
			*slash = '\0';
			mkdir_parents(dir);
		}
	}

	snprintf(tmp_path, sizeof(tmp_path), "%s.tmp-install", path);
	out = fopen(tmp_path, "we");
	if (out == NULL) {
		if (err != NULL && err_len > 0)
			snprintf(err, err_len, "%s: %s", tmp_path, strerror(errno));
		return -1;
	}

	in = fopen(path, "re");
	if (in != NULL) {
		while (fgets(line, sizeof(line), in) != NULL) {
			char stripped[M4_UPSERT_LINE_LEN];
			char name[256];
			char key[256];

			snprintf(stripped, sizeof(stripped), "%s", line);
			rstrip(stripped);

			if (line_is_section(stripped, name, sizeof(name))) {
				if (have_section)
					flush_missing_for_section(out, current_section, kvs, n_kvs, applied,
								   &n_changed);
				fputs(line, out);
				if (line[strlen(line) - 1] != '\n')
					fputc('\n', out);
				snprintf(current_section, sizeof(current_section), "%s", name);
				have_section = true;
				for (i = 0; i < n_kvs; i++)
					if (strcmp(kvs[i].section, name) == 0)
						section_seen[i] = true;
				continue;
			}

			if (have_section && line_is_key(stripped, key, sizeof(key))) {
				bool matched = false;
				bool value_changed = false;

				for (i = 0; i < n_kvs; i++) {
					char current_value[M4_UPSERT_LINE_LEN];

					if (applied[i])
						continue;
					if (strcmp(kvs[i].section, current_section) != 0)
						continue;
					if (strcmp(kvs[i].key, key) != 0)
						continue;
					matched = true;
					applied[i] = true;
					line_value(stripped, current_value, sizeof(current_value));
					if (strcmp(current_value, kvs[i].value) != 0) {
						fprintf(out, "%s = %s\n", kvs[i].key, kvs[i].value);
						n_changed++;
						value_changed = true;
					}
					break;
				}
				if (matched) {
					if (!value_changed) {
						/* Already correct -- keep the line verbatim, do
						 * not touch mtime for a no-op re-run. */
						fputs(line, out);
						if (line[strlen(line) - 1] != '\n')
							fputc('\n', out);
					}
					continue;
				}
			}

			fputs(line, out);
			if (line[strlen(line) - 1] != '\n')
				fputc('\n', out);
		}
		if (have_section)
			flush_missing_for_section(out, current_section, kvs, n_kvs, applied, &n_changed);
		fclose(in);
	}

	/* Any kv whose section never appeared anywhere in the file (including
	 * a brand new/empty file): one new "[section]" block per distinct
	 * remaining section, in the order kvs were given. */
	for (i = 0; i < n_kvs; i++) {
		size_t j;

		if (applied[i] || section_seen[i])
			continue;
		fprintf(out, "\n[%s]\n", kvs[i].section);
		fprintf(out, "%s = %s\n", kvs[i].key, kvs[i].value);
		applied[i] = true;
		section_seen[i] = true;
		n_changed++;
		for (j = i + 1; j < n_kvs; j++) {
			if (!applied[j] && !section_seen[j] && strcmp(kvs[j].section, kvs[i].section) == 0) {
				fprintf(out, "%s = %s\n", kvs[j].key, kvs[j].value);
				applied[j] = true;
				section_seen[j] = true;
				n_changed++;
			}
		}
	}

	if (fflush(out) != 0 || ferror(out) != 0) {
		if (err != NULL && err_len > 0)
			snprintf(err, err_len, M4_T("%s: write error: %s",
"%s: ошибка записи: %s"), tmp_path, strerror(errno));
		fclose(out);
		remove(tmp_path);
		return -1;
	}
	fclose(out);

	if (n_changed == 0) {
		/* Nothing to do -- do not touch the real file's mtime for no reason. */
		remove(tmp_path);
		return 0;
	}
	if (rename(tmp_path, path) != 0) {
		if (err != NULL && err_len > 0)
			snprintf(err, err_len, "rename(%s, %s): %s", tmp_path, path, strerror(errno));
		remove(tmp_path);
		return -1;
	}
	return n_changed;
}
