/* util/build_info.c -- see build_info.h. */
#include "build_info.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_field(struct m4_build_info *out, const char *key, const char *value)
{
	if (strcmp(key, "pipewire_version") == 0)
		snprintf(out->pipewire_version, sizeof(out->pipewire_version), "%s", value);
	else if (strcmp(key, "pipewire_tarball_sha256") == 0)
		snprintf(out->pipewire_tarball_sha256, sizeof(out->pipewire_tarball_sha256), "%s", value);
	else if (strcmp(key, "patches_sha256") == 0)
		snprintf(out->patches_sha256, sizeof(out->patches_sha256), "%s", value);
	else if (strcmp(key, "m4_src_sha256") == 0)
		snprintf(out->m4_src_sha256, sizeof(out->m4_src_sha256), "%s", value);
	else if (strcmp(key, "prepared_at_utc") == 0)
		snprintf(out->prepared_at_utc, sizeof(out->prepared_at_utc), "%s", value);
	else if (strcmp(key, "patches_count") == 0)
		out->patches_count = atol(value);
	else if (strcmp(key, "m4_src_files_count") == 0)
		out->m4_src_files_count = atol(value);
	/* prepared_host_pipewire_version and any future/unknown key: ignored
	 * on purpose -- not part of the enable/disable decision (see
	 * cli/wp_env.c), forward-compatible with new fields. */
}

bool m4_build_info_load(const char *path, struct m4_build_info *out)
{
	FILE *f;
	char line[512];

	memset(out, 0, sizeof(*out));
	out->patches_count = -1;
	out->m4_src_files_count = -1;

	if (path == NULL || path[0] == '\0')
		return false;
	f = fopen(path, "re");
	if (f == NULL)
		return false;

	while (fgets(line, sizeof(line), f) != NULL) {
		char *eq;
		char *key_end;
		char *value;
		size_t vlen;

		/* strip trailing newline(s) */
		vlen = strlen(line);
		while (vlen > 0 && (line[vlen - 1] == '\n' || line[vlen - 1] == '\r'))
			line[--vlen] = '\0';

		eq = strchr(line, '=');
		if (eq == NULL)
			continue; /* not a key=value line -- ignore, do not fail the whole file */
		*eq = '\0';
		key_end = eq;
		while (key_end > line && key_end[-1] == ' ')
			*--key_end = '\0';

		value = eq + 1;
		while (*value == ' ')
			value++;

		if (line[0] == '\0')
			continue;

		set_field(out, line, value);
	}
	fclose(f);
	return true;
}
