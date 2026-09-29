/* cli/wp_env.c -- see wp_env.h. */
#include "wp_env.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <pipewire/pipewire.h>

#include "../../../common/m4-conf.h"
#include "../../../common/m4-i18n.h"
#include "../util/config.h"
#include "../util/log.h"

const char *m4_wp_env_decision_str(enum m4_wp_env_decision d)
{
	switch (d) {
	case M4_WP_ENV_ENABLE:
		return M4_T("PipeWire version matches the module's BUILD-INFO, the .so is in place -- enabling our SPA_PLUGIN_DIR",
"версия PipeWire совпадает с BUILD-INFO модуля, .so на месте -- включаю наш SPA_PLUGIN_DIR");
	case M4_WP_ENV_DISABLE_NO_BUILD_INFO:
		return M4_T("BUILD-INFO not found/unreadable -- WirePlumber starts with the stock plugins (aptX HD)",
"BUILD-INFO не найден/не читается -- WirePlumber стартует со штатными плагинами (aptX HD)");
	case M4_WP_ENV_DISABLE_VERSION_MISMATCH:
		return M4_T("installed PipeWire version does not match the module's BUILD-INFO -- a rebuild is needed "
		       "(make && make install); WirePlumber starts with the stock plugins for now",
"версия установленного PipeWire не совпадает с BUILD-INFO модуля -- нужна пересборка "
		       "(make && make install), пока WirePlumber стартует со штатными плагинами");
	case M4_WP_ENV_DISABLE_SO_MISSING:
		return M4_T("libspa-codec-bluez5-aptx.so not found next to BUILD-INFO -- WirePlumber starts with "
		       "the stock plugins",
"libspa-codec-bluez5-aptx.so не найден рядом с BUILD-INFO -- WirePlumber стартует со "
		       "штатными плагинами");
	case M4_WP_ENV_DISABLE_NO_SYSTEM_DIR:
		return M4_T("[paths] spa-system-dir not found -- refusing to touch SPA_PLUGIN_DIR (that would break "
		       "ALL SPA plugins, not just bluez5); WirePlumber starts with the stock plugins",
"[paths] spa-system-dir не найден -- отказываюсь трогать SPA_PLUGIN_DIR (это сломало бы "
		       "ВСЕ SPA-плагины, не только bluez5), WirePlumber стартует со штатными плагинами");
	}
	return M4_T("unknown decision",
"неизвестное решение");
}

enum m4_wp_env_decision m4_wp_env_decide(const struct m4_build_info *bi, bool build_info_loaded,
					  const char *installed_pw_version, bool so_present,
					  bool spa_system_dir_present)
{
	if (!build_info_loaded || bi == NULL)
		return M4_WP_ENV_DISABLE_NO_BUILD_INFO;
	if (installed_pw_version == NULL || installed_pw_version[0] == '\0' ||
	    bi->pipewire_version[0] == '\0' || strcmp(bi->pipewire_version, installed_pw_version) != 0)
		return M4_WP_ENV_DISABLE_VERSION_MISMATCH;
	if (!so_present)
		return M4_WP_ENV_DISABLE_SO_MISSING;
	if (!spa_system_dir_present)
		return M4_WP_ENV_DISABLE_NO_SYSTEM_DIR;
	return M4_WP_ENV_ENABLE;
}

static bool dir_exists(const char *path)
{
	struct stat st;

	return path != NULL && path[0] != '\0' && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool file_readable(const char *path)
{
	return path != NULL && path[0] != '\0' && access(path, R_OK) == 0;
}

/* Writes `content` (may be "", meaning "empty file, 0 bytes") to `path`,
 * best-effort: creates the parent directory (mkdir -p style, single level
 * -- `[paths] runtime-dir` is always exactly one component below
 * $XDG_RUNTIME_DIR) if missing, then an atomic rename-into-place (write to
 * "<path>.tmp", fsync, rename) so a concurrent WirePlumber start never sees
 * a half-written file. Never treated as fatal by the caller -- if this
 * fails outright, m4_cli_wp_env_run() still returns 0 (this file's own top
 * comment: never a reason to fail startup) and just logs a warning; systemd's
 * `EnvironmentFile=-...` already tolerates the file not existing at all. */
static bool write_env_file(const char *path, const char *content)
{
	char tmp_path[600];
	FILE *f;

	if (path == NULL || path[0] == '\0')
		return false;
	if (snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path) >= (int)sizeof(tmp_path))
		return false;

	f = fopen(tmp_path, "we");
	if (f == NULL)
		return false;
	if (content != NULL && content[0] != '\0')
		fputs(content, f);
	if (fflush(f) != 0 || fsync(fileno(f)) != 0) {
		fclose(f);
		unlink(tmp_path);
		return false;
	}
	fclose(f);
	if (rename(tmp_path, path) != 0) {
		unlink(tmp_path);
		return false;
	}
	return true;
}

int m4_cli_wp_env_run(int argc, char **argv)
{
	char spa_plugin_dir[512];
	char spa_system_dir[512];
	char build_info_path[768];
	char so_path[768];
	char env_path[600];
	char runtime_dir[512];
	struct m4_build_info bi;
	bool bi_loaded;
	const char *installed_version;
	bool so_present, sysdir_present;
	enum m4_wp_env_decision decision;

	(void)argv;
	if (argc != 0) {
		fprintf(stderr, "usage: momentum4 wp-env\n");
		return 1;
	}

	snprintf(spa_plugin_dir, sizeof(spa_plugin_dir), "%s",
		 m4_conf_get_string(m4_config(), "paths", "spa-plugin-dir"));
	snprintf(spa_system_dir, sizeof(spa_system_dir), "%s",
		 m4_conf_get_string(m4_config(), "paths", "spa-system-dir"));

	snprintf(build_info_path, sizeof(build_info_path), "%s/bluez5/BUILD-INFO", spa_plugin_dir);
	snprintf(so_path, sizeof(so_path), "%s/bluez5/libspa-codec-bluez5-aptx.so", spa_plugin_dir);

	bi_loaded = m4_build_info_load(build_info_path, &bi);
	installed_version = pw_get_library_version();
	so_present = file_readable(so_path);
	sysdir_present = dir_exists(spa_system_dir);

	decision = m4_wp_env_decide(&bi, bi_loaded, installed_version, so_present, sysdir_present);

	m4_log(decision == M4_WP_ENV_ENABLE ? M4_LOG_INFO : M4_LOG_WARN,
	       M4_T("wp-env: %s (BUILD-INFO=\"%s\" installed=\"%s\")",
"wp-env: %s (BUILD-INFO=\"%s\" установлен=\"%s\")"),
	       m4_wp_env_decision_str(decision), bi_loaded ? bi.pipewire_version : M4_T("(none)",
"(нет)"),
	       installed_version != NULL ? installed_version : "?");

	if (m4_config_runtime_dir(runtime_dir, sizeof(runtime_dir)) == NULL) {
		m4_log(M4_LOG_WARN, M4_T("wp-env: failed to determine [paths] runtime-dir "
				     "($XDG_RUNTIME_DIR not set?) -- env file not written",
"wp-env: не удалось определить [paths] runtime-dir "
				     "($XDG_RUNTIME_DIR не задан?) -- env-файл не записан"));
		return 0;
	}
	mkdir(runtime_dir, 0700); /* best-effort; write_env_file()'s own fopen()
				    * failing is the real (still non-fatal) signal */
	if (snprintf(env_path, sizeof(env_path), "%s/wp.env", runtime_dir) >= (int)sizeof(env_path)) {
		m4_log(M4_LOG_WARN, M4_T("wp-env: wp.env path too long -- not written",
"wp-env: путь к wp.env слишком длинный -- не записан"));
		return 0;
	}

	if (decision == M4_WP_ENV_ENABLE) {
		char content[1200];

		snprintf(content, sizeof(content), "SPA_PLUGIN_DIR=%s:%s\n", spa_plugin_dir, spa_system_dir);
		if (!write_env_file(env_path, content))
			m4_log(M4_LOG_WARN, M4_T("wp-env: failed to write %s (%m) -- WirePlumber "
					     "starts with the stock plugins (EnvironmentFile=- tolerates "
					     "a missing file)",
"wp-env: не удалось записать %s (%m) -- WirePlumber "
					     "стартует со штатными плагинами (EnvironmentFile=- допускает "
					     "отсутствие файла)"),
			       env_path);
	} else {
		if (!write_env_file(env_path, ""))
			m4_log(M4_LOG_WARN, M4_T("wp-env: failed to write an empty %s (%m) -- not critical, "
					     "EnvironmentFile=- tolerates a missing file",
"wp-env: не удалось записать пустой %s (%m) -- не критично, "
					     "EnvironmentFile=- допускает отсутствие файла"),
			       env_path);
	}

	return 0;
}
