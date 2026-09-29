/* cli/setup.c -- see setup.h. */
#include "setup.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../util/capi_check.h"
#include "../util/sha256.h"
#include "../../../common/m4-i18n.h"

#define M4_HEXAGON_EM 164 /* EM_HEXAGON, the ELF header's e_machine field
			    * at offset 18, 2 bytes LE -- confirmed against a
			    * real capi_v2_aptX_CLHDADV_Encoder.so. */

static void print_usage(FILE *out)
{
	fprintf(out,
		M4_T("usage: momentum4 setup --capi PATH [--force]\n"
		"  Checks that PATH is a Hexagon ELF (EM_HEXAGON), verifies its sha256 against\n"
		"  data/capi-sha256.txt and copies the file into $XDG_DATA_HOME/momentum4/capi/\n"
		"  (see docs/user/obtaining-encoder.md). An unknown hash is refused without\n"
		"  --force.\n",
"usage: momentum4 setup --capi PATH [--force]\n"
		"  Проверяет, что PATH -- Hexagon-ELF (EM_HEXAGON), сверяет sha256 с\n"
		"  data/capi-sha256.txt и копирует файл в $XDG_DATA_HOME/momentum4/capi/\n"
		"  (см. docs/user/obtaining-encoder.md). Неизвестный хэш -- отказ без\n"
		"  --force.\n"));
}

/* Reads the first 20 bytes of `path` and checks the ELF magic + e_machine
 * (offset 18, u16 LE) == EM_HEXAGON. Returns 1 if it looks like a Hexagon
 * ELF, 0 if it is readable but is not, -1 on a read error (errno set). */
static int looks_like_hexagon_elf(const char *path)
{
	unsigned char hdr[20];
	FILE *f = fopen(path, "rbe");
	size_t n;
	uint16_t e_machine;

	if (f == NULL)
		return -1;
	n = fread(hdr, 1, sizeof(hdr), f);
	fclose(f);
	if (n < sizeof(hdr))
		return 0;
	if (hdr[0] != 0x7f || hdr[1] != 'E' || hdr[2] != 'L' || hdr[3] != 'F')
		return 0;
	e_machine = (uint16_t)(hdr[18] | (hdr[19] << 8));
	return e_machine == M4_HEXAGON_EM ? 1 : 0;
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
	return mkdir(tmp, 0755) == 0 || errno == EEXIST ? 0 : -1;
}

static const char *basename_of(const char *path)
{
	const char *slash = strrchr(path, '/');

	return slash != NULL ? slash + 1 : path;
}

static int copy_file(const char *src, const char *dst)
{
	FILE *in, *out;
	char buf[65536];
	size_t n;
	int rc = 0;

	in = fopen(src, "rbe");
	if (in == NULL)
		return -1;
	out = fopen(dst, "wbe");
	if (out == NULL) {
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
	if (rc == 0)
		chmod(dst, 0644);
	else
		remove(dst);
	return rc;
}

static int cmd_capi(const char *src_path, bool force)
{
	int elf_rc;
	char hex[M4_SHA256_HEX_LEN];
	char list_path[512];
	const char *list = m4_capi_sha256_list_path(list_path, sizeof(list_path));
	char desc[256] = "";
	int known;
	const char *xdg_data;
	char dest_dir[512];
	char dest_path[600];

	elf_rc = looks_like_hexagon_elf(src_path);
	if (elf_rc < 0) {
		fprintf(stderr, "momentum4 setup --capi: %s: %s\n", src_path, strerror(errno));
		return 1;
	}
	if (elf_rc == 0) {
		fprintf(stderr,
			M4_T("momentum4 setup --capi: %s: does not look like a Hexagon ELF (no ELF magic, or "
			"e_machine != EM_HEXAGON) -- check that this is really the ADSP module and not a plain "
			"ARM library\n",
"momentum4 setup --capi: %s: не похоже на Hexagon ELF (нет магии ELF или "
			"e_machine != EM_HEXAGON) -- проверьте, что это именно ADSP-модуль, а не обычная "
			"ARM-библиотека\n"),
			src_path);
		return 1;
	}

	if (m4_sha256_file_hex(src_path, hex) != 0) {
		fprintf(stderr, M4_T("momentum4 setup --capi: %s: failed to compute the sha256 (%s)\n",
"momentum4 setup --capi: %s: не удалось посчитать sha256 (%s)\n"), src_path,
			strerror(errno));
		return 1;
	}
	printf("sha256: %s\n", hex);

	known = list != NULL ? m4_capi_sha256_lookup(list, hex, desc, sizeof(desc)) : -1;
	if (known == 1) {
		printf(M4_T("Hash known%s%s\n",
"Хэш известен%s%s\n"), desc[0] != '\0' ? ": " : "", desc);
	} else if (known == 0) {
		printf(M4_T("Hash NOT found in the known list (%s).\n",
"Хэш НЕ найден в списке известных (%s).\n"), list);
		if (!force) {
			fprintf(stderr,
				M4_T("momentum4 setup --capi: unknown hash -- refusing (an incompatible ABI can "
				"crash the helper process; this does not affect audio, it just stays on aptX HD).\n"
				"To use the file anyway at your own risk: momentum4 setup --capi %s "
				"--force\n",
"momentum4 setup --capi: неизвестный хэш -- отказ (несовместимый ABI может "
				"уронить helper-процесс; на звук это не влияет, просто останется aptX HD).\n"
				"Чтобы всё равно использовать файл на свой риск: momentum4 setup --capi %s "
				"--force\n"),
				src_path);
			return 1;
		}
		printf(M4_T("--force given -- continuing at your own risk.\n",
"--force указан -- продолжаю на свой риск.\n"));
	} else {
		printf(M4_T("The known-hash list was not found -- treating the hash as unknown.\n",
"Список известных хэшей не найден -- считаю хэш неизвестным.\n"));
		if (!force) {
			fprintf(stderr, M4_T("momentum4 setup --capi: no data/capi-sha256.txt and no "
					 "--force given -- refusing.\n",
"momentum4 setup --capi: нет data/capi-sha256.txt и не указан "
					 "--force -- отказ.\n"));
			return 1;
		}
		printf(M4_T("--force given -- continuing at your own risk.\n",
"--force указан -- продолжаю на свой риск.\n"));
	}

	xdg_data = getenv("XDG_DATA_HOME");
	if (xdg_data != NULL && xdg_data[0] != '\0') {
		snprintf(dest_dir, sizeof(dest_dir), "%s/momentum4/capi", xdg_data);
	} else {
		const char *home = getenv("HOME");

		if (home == NULL || home[0] == '\0') {
			fprintf(stderr, M4_T("momentum4 setup --capi: no $XDG_DATA_HOME and $HOME -- nowhere "
					 "to copy to\n",
"momentum4 setup --capi: нет $XDG_DATA_HOME и $HOME -- некуда "
					 "копировать\n"));
			return 1;
		}
		snprintf(dest_dir, sizeof(dest_dir), "%s/.local/share/momentum4/capi", home);
	}
	if (mkdir_parents(dest_dir) != 0) {
		fprintf(stderr, "momentum4 setup --capi: mkdir %s: %s\n", dest_dir, strerror(errno));
		return 1;
	}
	snprintf(dest_path, sizeof(dest_path), "%s/%s", dest_dir, basename_of(src_path));
	if (copy_file(src_path, dest_path) != 0) {
		fprintf(stderr, M4_T("momentum4 setup --capi: copy to %s: %s\n",
"momentum4 setup --capi: копирование в %s: %s\n"), dest_path, strerror(errno));
		return 1;
	}
	printf(M4_T("Copied: %s\n",
"Скопировано: %s\n"), dest_path);
	if (strcmp(basename_of(src_path), "capi_v2_aptX_CLHDADV_Encoder.so") == 0)
		printf(M4_T("Name matches the default helper.capi -- no need to change momentum4.conf.\n",
"Имя совпадает с дефолтным helper.capi -- momentum4.conf менять не нужно.\n"));
	else
		printf(M4_T("File name differs from the default helper.capi -- set the path by hand:\n"
		       "  [helper]\n  capi = %s\n",
"Имя файла отличается от дефолтного helper.capi -- пропишите путь вручную:\n"
		       "  [helper]\n  capi = %s\n"),
		       dest_path);
	return 0;
}

int m4_cli_setup_run(int argc, char **argv)
{
	const char *capi_path = NULL;
	bool force = false;
	int i;

	for (i = 0; i < argc; i++) {
		if (strcmp(argv[i], "--capi") == 0 && i + 1 < argc) {
			capi_path = argv[++i];
		} else if (strcmp(argv[i], "--force") == 0) {
			force = true;
		} else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
			print_usage(stdout);
			return 0;
		} else {
			fprintf(stderr, M4_T("momentum4 setup: unknown argument: %s\n",
"momentum4 setup: неизвестный аргумент: %s\n"), argv[i]);
			print_usage(stderr);
			return 2;
		}
	}
	if (capi_path == NULL) {
		print_usage(stderr);
		return 2;
	}
	return cmd_capi(capi_path, force);
}
