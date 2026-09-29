/* common/gen-conf.c -- CLI wrapper around m4_conf_write_reference(), which
 * generates the fully-commented reference @datadir@/momentum4/momentum4.conf
 * from the same common/m4-conf-keys.def table used everywhere else. The
 * actual generation logic lives in common/m4-conf.c (m4_conf_write_reference()) --
 * shared verbatim with `momentum4 config defaults`
 * (daemon/src/cli/config.c) so the two can never drift; this file
 * is only argv handling + atomic-write-to-a-path.
 *
 * Usage: gen-conf [output-path]
 *   no argument, or "-"  -- print to stdout
 *   any other argument   -- write atomically (temp file + rename) to that
 *                           path; creating parent directories is the
 *                           caller's job (meson's custom_target() always
 *                           has the directory ready).
 */
#include "m4-conf.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
	const char *out_path = (argc >= 2) ? argv[1] : NULL;

	if (argc > 2) {
		fprintf(stderr, "usage: %s [output-path]\n", argv[0]);
		return 2;
	}

	if (out_path == NULL || strcmp(out_path, "-") == 0) {
		m4_conf_write_reference(stdout);
		return 0;
	}

	{
		char tmp_path[4096];
		FILE *f;
		int n = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp-genconf", out_path);

		if (n < 0 || (size_t)n >= sizeof(tmp_path)) {
			fprintf(stderr, "%s: path too long\n", out_path);
			return 1;
		}
		f = fopen(tmp_path, "we");
		if (f == NULL) {
			fprintf(stderr, "%s: %s\n", tmp_path, strerror(errno));
			return 1;
		}
		m4_conf_write_reference(f);
		if (fflush(f) != 0 || ferror(f) != 0) {
			fprintf(stderr, "%s: write error: %s\n", tmp_path, strerror(errno));
			fclose(f);
			remove(tmp_path);
			return 1;
		}
		fclose(f);
		if (rename(tmp_path, out_path) != 0) {
			fprintf(stderr, "rename(%s, %s): %s\n", tmp_path, out_path, strerror(errno));
			remove(tmp_path);
			return 1;
		}
	}
	return 0;
}
