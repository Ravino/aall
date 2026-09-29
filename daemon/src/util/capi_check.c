/* util/capi_check.c -- see capi_check.h. */
#include "capi_check.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#ifndef M4_SOURCE_DATA_DIR
#define M4_SOURCE_DATA_DIR ""
#endif

const char *m4_capi_sha256_list_path(char *buf, size_t buf_cap)
{
	const char *candidates[4];
	const char *xdg_data = getenv("XDG_DATA_HOME");
	const char *home = getenv("HOME");
	char xdg_path[512];
	char home_path[512];
	char devpath[512];
	size_t n = 0;
	size_t i;

	if (xdg_data != NULL && xdg_data[0] != '\0') {
		snprintf(xdg_path, sizeof(xdg_path), "%s/momentum4/capi-sha256.txt", xdg_data);
		candidates[n++] = xdg_path;
	} else if (home != NULL && home[0] != '\0') {
		snprintf(home_path, sizeof(home_path), "%s/.local/share/momentum4/capi-sha256.txt", home);
		candidates[n++] = home_path;
	}
	candidates[n++] = "/usr/share/momentum4/capi-sha256.txt";
	if (M4_SOURCE_DATA_DIR[0] != '\0') {
		snprintf(devpath, sizeof(devpath), "%s/capi-sha256.txt", M4_SOURCE_DATA_DIR);
		candidates[n++] = devpath;
	}

	for (i = 0; i < n; i++) {
		struct stat st;

		if (stat(candidates[i], &st) == 0 && S_ISREG(st.st_mode)) {
			snprintf(buf, buf_cap, "%s", candidates[i]);
			return buf;
		}
	}
	return NULL;
}

int m4_capi_sha256_lookup(const char *list_path, const char *hex_hash, char *out_desc, size_t out_desc_cap)
{
	FILE *f = fopen(list_path, "re");
	char line[512];

	if (f == NULL)
		return -1;
	while (fgets(line, sizeof(line), f) != NULL) {
		char *p = line;
		char *hash_start;
		size_t hash_len;

		while (*p == ' ' || *p == '\t')
			p++;
		if (*p == '\0' || *p == '\n' || *p == '#')
			continue;
		hash_start = p;
		hash_len = 0;
		while (p[hash_len] != '\0' && !isspace((unsigned char)p[hash_len]))
			hash_len++;
		if (hash_len != strlen(hex_hash)) {
			continue;
		}
		if (strncasecmp(hash_start, hex_hash, hash_len) != 0)
			continue;
		if (out_desc != NULL && out_desc_cap > 0) {
			char *rest = hash_start + hash_len;
			size_t rlen;

			while (*rest == ' ' || *rest == '\t')
				rest++;
			rlen = strlen(rest);
			while (rlen > 0 && (rest[rlen - 1] == '\n' || rest[rlen - 1] == '\r'))
				rlen--;
			if (rlen >= out_desc_cap)
				rlen = out_desc_cap - 1;
			memcpy(out_desc, rest, rlen);
			out_desc[rlen] = '\0';
		}
		fclose(f);
		return 1;
	}
	fclose(f);
	return 0;
}
