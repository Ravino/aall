/* util/legacy_conf.c -- see legacy_conf.h. Moved verbatim out of
 * cli/config.c's cmd_migrate() implementation, unchanged in behaviour --
 * see that file's git history for the original single-file version.
 */
#include "legacy_conf.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define M4_LEGACY_CONF_MAX_FILE (64 * 1024)

const struct m4_legacy_map_entry m4_legacy_momentum4_conf_map[] = {
	{ "mode", "daemon", "mode" },
	{ "player", "control", "player" },
	{ "player-status", "control", "player-status" },
	{ "player-resync", "control", "player-resync" },
	{ "gesture-play", "control", "gesture-play" },
	{ "gesture-pause", "control", "gesture-pause" },
	{ "gesture-playpause", "control", "gesture-playpause" },
	{ "gesture-next", "control", "gesture-next" },
	{ "gesture-previous", "control", "gesture-previous" },
	{ "gesture-stop", "control", "gesture-stop" },
	{ "gesture-cooldown-ms", "control", "gesture-cooldown-ms" },
	{ "earcon", "earcon", "enable" },
	{ "earcon-dir", "earcon", "dir" },
	{ "earcon-delay-on-ms", "earcon", "delay-on-ms" },
	{ "earcon-delay-off-ms", "earcon", "delay-off-ms" },
	{ "hfp-chup", "control", "hfp-chup" },
	{ "hfp-ata", "control", "hfp-ata" },
	{ "hfp-bldn", "control", "hfp-bldn" },
	{ "hfp-bvra", "control", "hfp-bvra" },
	{ "hfp-sco-drop", "control", "hfp-sco-drop" },
};
const size_t m4_legacy_momentum4_conf_map_count =
	sizeof(m4_legacy_momentum4_conf_map) / sizeof(m4_legacy_momentum4_conf_map[0]);

const struct m4_legacy_map_entry m4_legacy_aptx_adaptive_conf_map[] = {
	{ "mode", "adaptive", "mode" },
	{ "latency-adjust-ms", "adaptive", "latency-adjust-ms" },
	{ "heartbeat", "adaptive", "heartbeat" },
	{ "resync-period-min", "adaptive", "resync-period-min" },
	{ "rt-priority", "adaptive", "io-rt-priority" },
	{ "helper-mode", "adaptive", "helper-mode" },
	{ "helper-socket", "helper", "socket" },
	{ "helper-qemu", "helper", "qemu" },
	{ "helper-loader", "helper", "loader" },
	{ "helper-capi", "helper", "capi" },
};
const size_t m4_legacy_aptx_adaptive_conf_map_count =
	sizeof(m4_legacy_aptx_adaptive_conf_map) / sizeof(m4_legacy_aptx_adaptive_conf_map[0]);

static char *legacy_trim(char *p)
{
	char *end;

	while (*p != '\0' && isspace((unsigned char)*p))
		p++;
	end = p + strlen(p);
	while (end > p && isspace((unsigned char)end[-1]))
		*--end = '\0';
	return p;
}

int m4_legacy_conf_load(const char *path, const struct m4_legacy_map_entry *map, size_t n_map, m4_conf_t *out,
			 const char *source_prefix,
			 void (*on_unmapped)(const char *key, const char *value, void *ud), void *ud)
{
	FILE *f;
	char *buf;
	size_t n;
	char *p;
	int lineno = 0;
	int applied = 0;

	f = fopen(path, "re");
	if (f == NULL)
		return (errno == ENOENT) ? -1 : -2;

	buf = malloc(M4_LEGACY_CONF_MAX_FILE + 1); /* one-shot CLI/startup path,
						     * not RT/plugin code -- the
						     * "no malloc" rule is
						     * common/'s, not this file's
						     * (see m4-conf.h's own
						     * header comment). */
	if (buf == NULL) {
		fclose(f);
		return -2;
	}
	n = fread(buf, 1, M4_LEGACY_CONF_MAX_FILE, f);
	buf[n] = '\0';
	fclose(f);

	p = buf;
	while (*p != '\0') {
		char *nl = strchr(p, '\n');
		char line[512];
		size_t len = nl != NULL ? (size_t)(nl - p) : strlen(p);
		char *l, *eq, *key, *value, *hash;
		size_t i;

		lineno++;
		if (len >= sizeof(line)) {
			p = nl != NULL ? nl + 1 : p + len;
			continue;
		}
		memcpy(line, p, len);
		line[len] = '\0';
		p = nl != NULL ? nl + 1 : p + len;

		l = legacy_trim(line);
		if (*l == '\0' || *l == '#' || *l == ';')
			continue;
		if (*l == '[') {
			free(buf);
			return -3; /* already sectioned -- not the old flat format */
		}

		eq = strchr(l, '=');
		if (eq == NULL)
			continue;
		*eq = '\0';
		key = legacy_trim(l);
		value = legacy_trim(eq + 1);
		for (hash = value; (hash = strchr(hash, '#')) != NULL; hash++) {
			if (hash > value && isspace((unsigned char)hash[-1])) {
				*hash = '\0';
				value = legacy_trim(value);
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
		if (*key == '\0')
			continue;

		for (i = 0; i < n_map; i++) {
			if (strcmp(map[i].legacy_key, key) == 0) {
				char source[256];

				snprintf(source, sizeof(source), "%s:%s:%d",
					 source_prefix != NULL ? source_prefix : "legacy", path, lineno);
				if (m4_conf_set(out, map[i].new_section, map[i].new_key, value, source))
					applied++;
				break;
			}
		}
		if (i == n_map && on_unmapped != NULL)
			on_unmapped(key, value, ud);
	}
	free(buf);
	return applied;
}
