/* util/wp_overlay.c -- see wp_overlay.h. */
#include "wp_overlay.h"

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../../../common/m4-conf.h"
#include "../../../common/m4-i18n.h"

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
	return 0; /* final component (the file itself) is created by the caller */
}

const char *m4_wp_overlay_path(char *out, size_t out_len)
{
	const char *home = getenv("HOME");

	if (home == NULL || home[0] == '\0' || out == NULL || out_len == 0)
		return NULL;
	snprintf(out, out_len, "%s/.config/systemd/user/wireplumber.service.d/99-m4-adaptive-overlay.conf",
		 home);
	return out;
}

const char *m4_no_suspend_path(char *out, size_t out_len)
{
	const char *home = getenv("HOME");

	if (home == NULL || home[0] == '\0' || out == NULL || out_len == 0)
		return NULL;
	snprintf(out, out_len, "%s/.config/wireplumber/wireplumber.conf.d/51-m4-no-suspend.conf", home);
	return out;
}

static int copy_file(const char *src, const char *dst, char *err, size_t err_len)
{
	FILE *in, *out;
	char buf[65536];
	size_t n;
	int rc = 0;

	in = fopen(src, "rbe");
	if (in == NULL) {
		if (err != NULL && err_len > 0)
			snprintf(err, err_len, "%s: %s", src, strerror(errno));
		return -1;
	}
	out = fopen(dst, "wbe");
	if (out == NULL) {
		if (err != NULL && err_len > 0)
			snprintf(err, err_len, "%s: %s", dst, strerror(errno));
		fclose(in);
		return -1;
	}
	while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
		if (fwrite(buf, 1, n, out) != n) {
			rc = -1;
			break;
		}
	}
	if (ferror(in))
		rc = -1;
	fclose(in);
	if (fclose(out) != 0)
		rc = -1;
	if (rc != 0) {
		if (err != NULL && err_len > 0)
			snprintf(err, err_len, M4_T("%s: write error",
"%s: ошибка записи"), dst);
		remove(dst);
	}
	return rc;
}

/* @datadir@/momentum4/wireplumber/<name> -- resolved via
 * common/m4-conf.c's compiled M4_CONF_DATADIR (m4_conf_expand(), a raw
 * substitution function, independent of momentum4.conf's own layers -- it
 * cannot be pointed at a test's own scratch directory through
 * $MOMENTUM4_CONFIG the way the rest of this project's test-only overrides
 * work). $MOMENTUM4_DATADIR (test-only, mirrors $MOMENTUM4_MAC/
 * $MOMENTUM4_BT_ADAPTER in util/config.h -- never documented for real use)
 * lets tests/test_lifecycle_cli.sh point this at its own tmpdir instead of
 * requiring a real, matching-prefix `make install` to have already
 * happened wherever `meson test` runs. */
static void datadir_template_path(const char *name, char *out, size_t out_len)
{
	const char *override = getenv("MOMENTUM4_DATADIR");

	if (override != NULL && override[0] != '\0') {
		snprintf(out, out_len, "%s/momentum4/wireplumber/%s", override, name);
	} else {
		char tmpl[300];

		snprintf(tmpl, sizeof(tmpl), "@datadir@/momentum4/wireplumber/%s", name);
		m4_conf_expand(tmpl, out, out_len);
	}
}

int m4_wp_overlay_install(char *err, size_t err_len)
{
	char src[768];
	char dst[768];

	datadir_template_path("99-m4-adaptive-overlay.conf", src, sizeof(src));
	if (m4_wp_overlay_path(dst, sizeof(dst)) == NULL) {
		if (err != NULL && err_len > 0)
			snprintf(err, err_len, M4_T("$HOME is not set",
"$HOME не задан"));
		return -1;
	}
	mkdir_parents(dst);
	return copy_file(src, dst, err, err_len);
}

int m4_wp_overlay_remove(char *err, size_t err_len)
{
	char path[768];

	if (m4_wp_overlay_path(path, sizeof(path)) == NULL)
		return 0;
	if (unlink(path) != 0 && errno != ENOENT) {
		if (err != NULL && err_len > 0)
			snprintf(err, err_len, "%s: %s", path, strerror(errno));
		return -1;
	}
	return 0;
}

/* Substitutes every literal occurrence of "@M4_ADDRESS_UNDERSCORE@" in `in`
 * with `mac_underscored`, writing at most out_len-1 bytes to `out` (always
 * NUL-terminated). Deliberately NOT common/m4-conf.c's m4_conf_expand()
 * (that function only knows about @prefix@/@libdir@/@libexecdir@/@datadir@
 * and ${...} env vars, both resolved at compile/process-start time -- this
 * one substitution is resolved from `[device] address`, a config VALUE,
 * not a build-time constant, so it does not belong in common/, see that
 * template's own header comment). */
static void substitute_mac(const char *in, const char *mac_underscored, char *out, size_t out_len)
{
	static const char needle[] = "@M4_ADDRESS_UNDERSCORE@";
	const size_t needle_len = sizeof(needle) - 1;
	size_t oi = 0;

	if (out_len == 0)
		return;
	while (*in != '\0' && oi + 1 < out_len) {
		if (strncmp(in, needle, needle_len) == 0) {
			size_t n = strlen(mac_underscored);

			if (n > out_len - 1 - oi)
				n = out_len - 1 - oi;
			memcpy(out + oi, mac_underscored, n);
			oi += n;
			in += needle_len;
		} else {
			out[oi++] = *in++;
		}
	}
	out[oi] = '\0';
}

int m4_no_suspend_install(const char *mac_address, char *err, size_t err_len)
{
	char src[768];
	char dst[768];
	char mac_us[64];
	FILE *in, *out;
	char line[4096];
	size_t i;

	if (mac_address == NULL || mac_address[0] == '\0') {
		if (err != NULL && err_len > 0)
			snprintf(err, err_len, M4_T("M4 MAC address unknown (`[device] address` is not set/not "
						"detected) -- 51-m4-no-suspend.conf not installed, WirePlumber's "
						"normal 5-second suspend stays in effect.",
"MAC-адрес M4 неизвестен (`[device] address` не задан/не "
						"обнаружен) -- 51-m4-no-suspend.conf не установлен, обычный "
						"5-секундный suspend WirePlumber останется в силе."));
		return -1;
	}
	for (i = 0; mac_address[i] != '\0' && i + 1 < sizeof(mac_us); i++)
		mac_us[i] = (mac_address[i] == ':') ? '_' : (char)toupper((unsigned char)mac_address[i]);
	mac_us[i] = '\0';

	datadir_template_path("51-m4-no-suspend.conf.in", src, sizeof(src));
	if (m4_no_suspend_path(dst, sizeof(dst)) == NULL) {
		if (err != NULL && err_len > 0)
			snprintf(err, err_len, M4_T("$HOME is not set",
"$HOME не задан"));
		return -1;
	}
	mkdir_parents(dst);

	in = fopen(src, "re");
	if (in == NULL) {
		if (err != NULL && err_len > 0)
			snprintf(err, err_len, "%s: %s", src, strerror(errno));
		return -1;
	}
	out = fopen(dst, "we");
	if (out == NULL) {
		if (err != NULL && err_len > 0)
			snprintf(err, err_len, "%s: %s", dst, strerror(errno));
		fclose(in);
		return -1;
	}
	while (fgets(line, sizeof(line), in) != NULL) {
		char resolved[4096];

		substitute_mac(line, mac_us, resolved, sizeof(resolved));
		if (fputs(resolved, out) == EOF) {
			if (err != NULL && err_len > 0)
				snprintf(err, err_len, M4_T("%s: write error",
"%s: ошибка записи"), dst);
			fclose(in);
			fclose(out);
			remove(dst);
			return -1;
		}
	}
	fclose(in);
	fclose(out);
	return 0;
}

int m4_no_suspend_remove(char *err, size_t err_len)
{
	char path[768];

	if (m4_no_suspend_path(path, sizeof(path)) == NULL)
		return 0;
	if (unlink(path) != 0 && errno != ENOENT) {
		if (err != NULL && err_len > 0)
			snprintf(err, err_len, "%s: %s", path, strerror(errno));
		return -1;
	}
	return 0;
}
