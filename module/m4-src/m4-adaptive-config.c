/* momentum4: aptX Adaptive user config file.
 * SPDX-License-Identifier: MIT
 * See m4-adaptive-config.h.
 */
#include "m4-adaptive-config.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* strcasecmp */
#include <unistd.h>  /* getuid() */

void m4_ah_user_config_defaults(struct m4_ah_user_config *c)
{
	memset(c, 0, sizeof(*c));
}

/* Case/separator-insensitive key match: 'a' and 'b' compare equal if they
 * are the same after lowercasing and treating '-' and '_' as identical. */
static bool key_eq(const char *a, const char *b)
{
	for (;; a++, b++) {
		char ca = *a, cb = *b;
		if (ca == '-') ca = '_';
		if (cb == '-') cb = '_';
		ca = (char)tolower((unsigned char)ca);
		cb = (char)tolower((unsigned char)cb);
		if (ca != cb)
			return false;
		if (ca == '\0')
			return true;
	}
}

static void rstrip(char *s)
{
	size_t n = strlen(s);
	while (n > 0 && isspace((unsigned char)s[n - 1]))
		s[--n] = '\0';
}

static char *lstrip(char *s)
{
	while (*s && isspace((unsigned char)*s))
		s++;
	return s;
}

static bool parse_bool(const char *v, bool *out)
{
	if (!strcasecmp(v, "on") || !strcasecmp(v, "1") || !strcasecmp(v, "true") ||
			!strcasecmp(v, "yes")) {
		*out = true;
		return true;
	}
	if (!strcasecmp(v, "off") || !strcasecmp(v, "0") || !strcasecmp(v, "false") ||
			!strcasecmp(v, "no")) {
		*out = false;
		return true;
	}
	return false;
}

static bool parse_i32(const char *v, int32_t *out)
{
	char *end;
	long val = strtol(v, &end, 0);
	if (end == v || *end != '\0')
		return false;
	if (val < INT32_MIN || val > INT32_MAX)
		return false;
	*out = (int32_t)val;
	return true;
}

static void apply_kv(struct m4_ah_user_config *c, char *key, char *value)
{
	rstrip(key);
	rstrip(value);
	value = lstrip(value);
	if (*key == '\0' || *value == '\0')
		return;

	if (key_eq(key, "mode")) {
		if (!strcasecmp(value, "ll0")) {
			c->have_mode = true;
			c->mode = M4_AH_MODE_LL0;
		} else if (!strcasecmp(value, "ll1")) {
			c->have_mode = true;
			c->mode = M4_AH_MODE_LL1;
		}
	} else if (key_eq(key, "latency_adjust_ms") || key_eq(key, "latency") ||
			key_eq(key, "adj")) {
		int32_t v;
		if (parse_i32(value, &v)) {
			c->have_latency_adjust_ms = true;
			c->latency_adjust_ms = v;
		}
	} else if (key_eq(key, "heartbeat") || key_eq(key, "auto_resync")) {
		bool v;
		if (parse_bool(value, &v)) {
			c->have_heartbeat = true;
			c->heartbeat_enabled = v;
		}
	} else if (key_eq(key, "resync_period_min") || key_eq(key, "period")) {
		int32_t v;
		if (parse_i32(value, &v) && v > 0) {
			c->have_resync_period_min = true;
			c->resync_period_min = v;
		}
	} else if (key_eq(key, "helper_qemu")) {
		c->have_qemu_path = true;
		snprintf(c->qemu_path, sizeof(c->qemu_path), "%s", value);
	} else if (key_eq(key, "helper_loader")) {
		c->have_loader_path = true;
		snprintf(c->loader_path, sizeof(c->loader_path), "%s", value);
	} else if (key_eq(key, "helper_capi")) {
		c->have_capi_path = true;
		snprintf(c->capi_path, sizeof(c->capi_path), "%s", value);
	} else if (key_eq(key, "helper_mode")) {
		if (!strcasecmp(value, "socket")) {
			c->have_helper_mode = true;
			c->helper_mode_is_socket = true;
		} else if (!strcasecmp(value, "spawn")) {
			c->have_helper_mode = true;
			c->helper_mode_is_socket = false;
		}
		/* Unrecognized value (neither "socket" nor "spawn"): silently
		 * ignored, same "best-effort convenience file" policy as every
		 * other key in this parser -- the caller's own default wins. */
	} else if (key_eq(key, "helper_socket")) {
		c->have_socket_path = true;
		snprintf(c->socket_path, sizeof(c->socket_path), "%s", value);
	} else if (key_eq(key, "rt_priority")) {
		int32_t v;
		/* 0..99: Linux SCHED_FIFO/SCHED_RR priority range
		 * (sched_get_priority_min/max(SCHED_FIFO) == 1..99). 0 is a valid,
		 * meaningful value here too ("disabled" -- see m4-adaptive-config.h),
		 * so it is accepted, unlike resync_period_min's `v > 0` above.
		 * Anything outside 0..99 is silently ignored (best-effort file,
		 * same policy as every other key in this parser) rather than
		 * clamped, so a typo cannot silently turn into some other valid-
		 * looking priority. */
		if (parse_i32(value, &v) && v >= 0 && v <= 99) {
			c->have_io_rt_priority = true;
			c->io_rt_priority = v;
		}
	} else if (key_eq(key, "allow_devices")) {
		if (!strcasecmp(value, "m4")) {
			c->have_allow_devices = true;
			c->allow_devices_any = false;
		} else if (!strcasecmp(value, "any")) {
			c->have_allow_devices = true;
			c->allow_devices_any = true;
		}
		/* Unrecognized value: silently ignored, same policy as every other
		 * key in this parser -- the caller's own default ("m4") wins. */
	}
	/* Unknown key: ignored, per file header ("best-effort convenience"). */
}

void m4_ah_user_config_parse_buffer(struct m4_ah_user_config *c, const char *buf, size_t len)
{
	char *copy = malloc(len + 1);
	char *line, *saveptr;

	if (!copy)
		return;
	memcpy(copy, buf, len);
	copy[len] = '\0';

	line = strtok_r(copy, "\n", &saveptr);
	while (line) {
		char *hash = strchr(line, '#');
		char *eq;
		if (hash)
			*hash = '\0';

		{
			char *p = lstrip(line);
			if (*p == '\0') {
				line = strtok_r(NULL, "\n", &saveptr);
				continue;
			}
			eq = strchr(p, '=');
			if (eq) {
				*eq = '\0';
				apply_kv(c, p, eq + 1);
			} else {
				/* "key value" (whitespace-separated) form. */
				char *sp = p;
				while (*sp && !isspace((unsigned char)*sp))
					sp++;
				if (*sp != '\0') {
					*sp = '\0';
					apply_kv(c, p, sp + 1);
				}
			}
		}
		line = strtok_r(NULL, "\n", &saveptr);
	}

	free(copy);
}

bool m4_ah_user_config_load_file(struct m4_ah_user_config *c, const char *path)
{
	FILE *f;
	char *buf;
	long size;
	size_t n;

	if (!path || *path == '\0')
		return false;

	f = fopen(path, "rb");
	if (!f)
		return false;

	if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
		fclose(f);
		return false;
	}
	/* Sanity cap: this is a small hand-edited config file, not arbitrary
	 * input. 64 KiB is already absurdly generous. */
	if (size > 64 * 1024) {
		fclose(f);
		return false;
	}

	buf = malloc((size_t)size);
	if (!buf) {
		fclose(f);
		return false;
	}
	n = fread(buf, 1, (size_t)size, f);
	fclose(f);

	m4_ah_user_config_parse_buffer(c, buf, n);
	free(buf);
	return true;
}

void m4_ah_user_config_default_path(char *buf, size_t buf_size)
{
	const char *env, *xdg, *home;

	if (buf_size == 0)
		return;
	buf[0] = '\0';

	env = getenv("M4_ADAPTIVE_CONFIG");
	if (env && *env) {
		snprintf(buf, buf_size, "%s", env);
		return;
	}

	xdg = getenv("XDG_CONFIG_HOME");
	if (xdg && *xdg) {
		snprintf(buf, buf_size, "%s/momentum4/aptx-adaptive.conf", xdg);
		return;
	}

	home = getenv("HOME");
	if (home && *home)
		snprintf(buf, buf_size, "%s/.config/momentum4/aptx-adaptive.conf", home);
}

void m4_ah_user_config_default_socket_path(char *buf, size_t buf_size)
{
	const char *xdg_runtime;

	if (buf_size == 0)
		return;
	buf[0] = '\0';

	xdg_runtime = getenv("XDG_RUNTIME_DIR");
	if (xdg_runtime && *xdg_runtime) {
		snprintf(buf, buf_size, "%s/momentum4/helper.sock", xdg_runtime);
		return;
	}

	/* systemd's `%t` specifier falls back to /run/user/<uid> when
	 * $XDG_RUNTIME_DIR is unset -- mirrored here so this default matches
	 * ListenStream=%t/momentum4/helper.sock even in an environment that
	 * (unusually, e.g. a bare test harness) does not export the variable. */
	snprintf(buf, buf_size, "/run/user/%u/momentum4/helper.sock", (unsigned)getuid());
}
