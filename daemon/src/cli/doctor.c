/* cli/doctor.c -- see doctor.h. */
#include "doctor.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <pipewire/pipewire.h>

#include "../../../common/m4-conf.h"
#include "../../../common/m4-i18n.h"
#include "../metrics/stats.h"
#include "../util/build_info.h"
#include "../util/capi_check.h"
#include "../util/config.h"
#include "../util/rt_status.h"
#include "../util/sha256.h"
#include "../util/systemd_unit.h"
#include "wp_env.h"

/* Set by meson only in a dev build (see ../../meson.build); empty in a
 * release build, which disables the checkout-path leak checks entirely --
 * a release build's units/config can never reference a source checkout. */
#ifndef M4_DEV_CHECKOUT_ROOT
#define M4_DEV_CHECKOUT_ROOT ""
#endif

static int g_warnings = 0;

__attribute__((format(printf, 1, 2))) static void ok(const char *fmt, ...)
{
	va_list ap;

	printf("[OK]   ");
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
}

__attribute__((format(printf, 1, 2))) static void warn(const char *fmt, ...)
{
	va_list ap;

	printf("[WARN] ");
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
	g_warnings++;
}

__attribute__((format(printf, 1, 2))) static void info(const char *fmt, ...)
{
	va_list ap;

	printf("[--]   ");
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
}

static bool file_exists(const char *path)
{
	struct stat st;

	return path != NULL && path[0] != '\0' && stat(path, &st) == 0;
}

/* Used only for the dev-checkout-path leak checks below (M4_DEV_CHECKOUT_ROOT)
 * -- skips comment lines (systemd unit/drop-in and momentum4.conf both use
 * '#'/';' as the comment prefix) so a file that merely EXPLAINS, in a
 * comment, why this project never hardcodes the checkout path (or one that
 * still carries an old dev-only comment) does not trip a false warning;
 * only a real, active reference (ExecStart=, Documentation=, a config
 * value, ...) counts. */
static bool file_contains(const char *path, const char *needle)
{
	FILE *f = fopen(path, "re");
	char line[1024];
	bool found = false;

	if (f == NULL)
		return false;
	while (fgets(line, sizeof(line), f) != NULL) {
		const char *p = line;

		while (*p == ' ' || *p == '\t')
			p++;
		if (*p == '#' || *p == ';')
			continue;
		if (strstr(line, needle) != NULL) {
			found = true;
			break;
		}
	}
	fclose(f);
	return found;
}

/* Mirrors cli/wp_env.c's own m4_cli_wp_env_run() resolution exactly (same
 * paths, same m4_wp_env_decide() call) so `momentum4 doctor` reports the
 * SAME verdict `momentum4 wp-env` would act on, never a second, possibly
 * diverging, opinion -- `momentum4 doctor` only logs and reports it.
 * Read-only: never writes wp.env itself. */
static void check_pipewire_version(void)
{
	char spa_plugin_dir[512];
	char spa_system_dir[512];
	char build_info_path[768];
	char so_path[768];
	struct m4_build_info bi;
	bool bi_loaded;
	const char *installed_version = pw_get_library_version();
	bool so_present, sysdir_present;
	enum m4_wp_env_decision decision;

	info(M4_T("PipeWire (libpipewire, at runtime): %s",
"PipeWire (libpipewire, во время выполнения): %s"), installed_version != NULL ? installed_version : "?");

	snprintf(spa_plugin_dir, sizeof(spa_plugin_dir), "%s",
		 m4_conf_get_string(m4_config(), "paths", "spa-plugin-dir"));
	snprintf(spa_system_dir, sizeof(spa_system_dir), "%s",
		 m4_conf_get_string(m4_config(), "paths", "spa-system-dir"));
	snprintf(build_info_path, sizeof(build_info_path), "%s/bluez5/BUILD-INFO", spa_plugin_dir);
	snprintf(so_path, sizeof(so_path), "%s/bluez5/libspa-codec-bluez5-aptx.so", spa_plugin_dir);

	bi_loaded = m4_build_info_load(build_info_path, &bi);
	so_present = file_exists(so_path) && access(so_path, R_OK) == 0;
	sysdir_present = file_exists(spa_system_dir);

	decision = m4_wp_env_decide(&bi, bi_loaded, installed_version, so_present, sysdir_present);
	if (decision == M4_WP_ENV_ENABLE) {
		ok("wp-env: %s (BUILD-INFO=%s)", m4_wp_env_decision_str(decision), bi.pipewire_version);
	} else if (decision == M4_WP_ENV_DISABLE_NO_BUILD_INFO && !so_present) {
		/* Module not built/installed at all yet -- routine during
		 * early setup, not a warning-worthy state. */
		info("wp-env: %s", m4_wp_env_decision_str(decision));
	} else {
		warn("wp-env: %s", m4_wp_env_decision_str(decision));
	}
}

static void check_capi(void)
{
	const char *capi_path = m4_conf_get_string(m4_config(), "helper", "capi");
	char hex[M4_SHA256_HEX_LEN];
	char list_path[512];
	const char *list;
	char desc[256] = "";
	int known;

	if (capi_path == NULL || capi_path[0] == '\0' || !file_exists(capi_path)) {
		warn(M4_T("CAPI (%s) missing -- aptX Adaptive will not turn on, plain aptX HD "
		     "will be used instead (see docs/user/obtaining-encoder.md, `momentum4 setup --capi`).",
"CAPI (%s) отсутствует -- aptX Adaptive не включится, будет обычный aptX HD "
		     "(см. docs/user/obtaining-encoder.md, `momentum4 setup --capi`)."),
		     capi_path != NULL ? capi_path : M4_T("(path unknown)",
"(путь неизвестен)"));
		return;
	}
	if (m4_sha256_file_hex(capi_path, hex) != 0) {
		warn(M4_T("CAPI (%s): failed to compute the sha256.",
"CAPI (%s): не удалось посчитать sha256."), capi_path);
		return;
	}
	list = m4_capi_sha256_list_path(list_path, sizeof(list_path));
	known = list != NULL ? m4_capi_sha256_lookup(list, hex, desc, sizeof(desc)) : -1;
	if (known == 1)
		ok(M4_T("CAPI (%s): sha256 known%s%s.",
"CAPI (%s): sha256 известен%s%s."), capi_path, desc[0] != '\0' ? ": " : "", desc);
	else if (known == 0)
		warn(M4_T("CAPI (%s): sha256 %s NOT found in the known list (%s) -- possibly an incompatible ABI.",
"CAPI (%s): sha256 %s НЕ найден в списке известных (%s) -- возможен несовместимый ABI."),
		     capi_path, hex, list);
	else
		warn(M4_T("CAPI (%s): sha256 %s -- the known-hash list was not found, not checked.",
"CAPI (%s): sha256 %s -- список известных хэшей не найден, не проверено."), capi_path, hex);
}

static void check_helper_binaries(void)
{
	const char *qemu = m4_conf_get_string(m4_config(), "helper", "qemu");
	const char *loader = m4_conf_get_string(m4_config(), "helper", "loader");

	if (qemu != NULL && access(qemu, X_OK) == 0)
		ok(M4_T("qemu-hexagon: %s (executable)",
"qemu-hexagon: %s (исполняемый)"), qemu);
	else
		warn(M4_T("qemu-hexagon: %s -- not found or not executable (`make qemu`/-Dqemu=system).",
"qemu-hexagon: %s -- не найден или не исполняемый (`make qemu`/-Dqemu=system)."),
		     qemu != NULL ? qemu : M4_T("(path unknown)",
"(путь неизвестен)"));

	if (loader != NULL && access(loader, R_OK) == 0)
		ok(M4_T("hx_adaptive_loader: %s (readable)",
"hx_adaptive_loader: %s (читается)"), loader);
	else
		warn(M4_T("hx_adaptive_loader: %s -- not found or not readable.",
"hx_adaptive_loader: %s -- не найден или не читается."), loader != NULL ? loader : M4_T("(path unknown)",
"(путь неизвестен)"));
}

/* Read-only presence checks for the systemd --user units/overlay this
 * project installs -- NEVER calls systemctl/sd-bus here: doctor only
 * reads/observes; `momentum4 status` is where live state is
 * queried. Both the old names (m4-adaptive-helper.*) and the current
 * renamed ones (momentum4-helper.*) are accepted, since a machine
 * upgraded from an earlier release may still have the old names
 * installed. */
static void check_units_and_overlay(char user_unit_dirs[][512], size_t n_dirs)
{
	static const char *const unit_names[] = {
		"momentum4d.service",
		"momentum4-helper.socket",
		"m4-adaptive-helper.socket",
	};
	size_t i, j;
	bool any_daemon_unit = false, any_helper_unit = false;

	for (i = 0; i < n_dirs; i++) {
		for (j = 0; j < sizeof(unit_names) / sizeof(unit_names[0]); j++) {
			char path[1100];

			snprintf(path, sizeof(path), "%.512s/%s", user_unit_dirs[i], unit_names[j]);
			if (!file_exists(path))
				continue;
			if (strcmp(unit_names[j], "momentum4d.service") == 0)
				any_daemon_unit = true;
			else
				any_helper_unit = true;
			if (M4_DEV_CHECKOUT_ROOT[0] != '\0' && file_contains(path, M4_DEV_CHECKOUT_ROOT))
				warn(M4_T("unit %s references the checkout path (%s), not the release path.",
"юнит %s ссылается на путь чекаута (%s), не путь релиза."), path,
				     M4_DEV_CHECKOUT_ROOT);
		}
	}
	if (any_daemon_unit)
		ok(M4_T("momentum4d.service found (unit file).",
"momentum4d.service найден (юнит-файл)."));
	else
		info(M4_T("momentum4d.service not found -- the daemon is not yet installed as a unit.",
"momentum4d.service не найден -- демон ещё не установлен как юнит."));
	if (any_helper_unit)
		ok(M4_T("helper socket unit found.",
"сокет-юнит helper'а найден."));
	else
		info(M4_T("helper socket unit not found -- not yet installed.",
"сокет-юнит helper'а не найден -- ещё не установлен."));
}

/* RT status: compares the ACTUALLY OBTAINED SCHED_*
 * priority of the bridge's IO thread (stats.json, written by the bridge
 * itself -- see util/rt_status.h's own top comment for why this file
 * cannot read that any other way) and the qemu-hexagon HELPER PROCESS
 * (util/rt_status.h's /proc-only, read-only PID discovery) against
 * `[adaptive] io-rt-priority`/`[helper] rt-priority`. Purely read-only:
 * one optional stats.json read (same file/path resolution `momentum4
 * status` already uses) and a handful of /proc reads -- no systemctl/
 * sd-bus, consistent with this file's own top-of-function policy. */
static void check_rt_status(void)
{
	const m4_conf_t *conf = m4_config();
	int configured_io = (int)m4_conf_get_int(conf, "adaptive", "io-rt-priority", 70);
	int configured_helper_prio = (int)m4_conf_get_int(conf, "helper", "rt-priority", 10);
	int configured_helper_nice = (int)m4_conf_get_int(conf, "helper", "nice", -5);
	const char *qemu_path = m4_conf_get_string(conf, "helper", "qemu");
	char stats_path[512];
	struct m4_stats st;
	bool stats_ok = false;
	pid_t helper_pid;

	if (m4_config_stats_path(stats_path, sizeof(stats_path)) != NULL)
		stats_ok = (m4_stats_load(stats_path, &st) == 0);

	if (!stats_ok) {
		info(M4_T("bridge IO thread RT priority: no data (stats.json unavailable -- the bridge "
		     "does not look like it is running right now).",
"RT-приоритет IO-нити моста: нет данных (stats.json недоступен -- мост сейчас, "
		     "похоже, не запущен)."));
	} else if (!st.alive) {
		info(M4_T("bridge IO thread RT priority: the helper is not responding (stats.json is stale) -- not checked.",
"RT-приоритет IO-нити моста: helper не отвечает (stats.json устарел) -- не проверено."));
	} else if (m4_rt_status_matches(configured_io, st.bridge_rt_policy, st.bridge_rt_prio)) {
		ok(M4_T("bridge IO thread RT priority: %s/%d -- matches [adaptive] io-rt-priority=%d.",
"RT-приоритет IO-нити моста: %s/%d -- совпадает с [adaptive] io-rt-priority=%d."),
		   st.bridge_rt_policy, st.bridge_rt_prio, configured_io);
	} else {
		warn(M4_T("bridge IO thread RT priority: actually %s/%d, but [adaptive] io-rt-priority=%d expects %s "
		     "-- EPERM (wireplumber.service lacks CAP_SYS_NICE/LimitRTPRIO) is silently ignored by the code; "
		     "check LimitRTPRIO/rtkit.",
"RT-приоритет IO-нити моста: фактически %s/%d, а [adaptive] io-rt-priority=%d ожидает %s "
		     "-- EPERM (нет CAP_SYS_NICE/LimitRTPRIO у wireplumber.service) тихо игнорируется кодом; "
		     "проверьте LimitRTPRIO/rtkit."),
		     st.bridge_rt_policy, st.bridge_rt_prio, configured_io,
		     configured_io > 0 ? M4_T("SCHED_FIFO with the same number",
"SCHED_FIFO с тем же числом") : "SCHED_OTHER");
	}

	helper_pid = m4_rt_status_find_helper_pid(qemu_path);
	if (helper_pid <= 0) {
		info(M4_T("helper (qemu-hexagon) RT priority: process not currently found (not running/not connected) "
		     "-- not checked.",
"RT-приоритет helper'а (qemu-hexagon): процесс сейчас не найден (не запущен/не подключён) "
		     "-- не проверено."));
		return;
	}

	{
		struct m4_rt_proc_status ps;
		const char *policy_name;

		m4_rt_status_query_pid(helper_pid, &ps);
		policy_name = m4_rt_status_policy_name(ps.sched_policy);

		if (m4_rt_status_matches(configured_helper_prio, policy_name, ps.rt_priority)) {
			ok(M4_T("helper (qemu-hexagon, pid %d) RT priority: %s/%d -- matches [helper] "
			   "rt-priority=%d.",
"RT-приоритет helper'а (qemu-hexagon, pid %d): %s/%d -- совпадает с [helper] "
			   "rt-priority=%d."),
			   (int)helper_pid, policy_name, ps.rt_priority, configured_helper_prio);
		} else {
			warn(M4_T("helper (qemu-hexagon, pid %d) RT priority: actually %s/%d, but [helper] "
			     "rt-priority=%d expects %s -- the momentum4-helper@.service unit's LimitRTPRIO "
			     "(currently 50) or EPERM without rtkit may have clamped the value.",
"RT-приоритет helper'а (qemu-hexagon, pid %d): фактически %s/%d, а [helper] "
			     "rt-priority=%d ожидает %s -- LimitRTPRIO юнита momentum4-helper@.service "
			     "(сейчас 50) или EPERM без rtkit могли обрезать значение."),
			     (int)helper_pid, policy_name, ps.rt_priority, configured_helper_prio,
			     configured_helper_prio > 0 ? M4_T("SCHED_FIFO with the same number",
"SCHED_FIFO с тем же числом") : "SCHED_OTHER");
		}

		if (ps.have_nice) {
			if (ps.nice == configured_helper_nice)
				ok(M4_T("helper nice (pid %d): %d -- matches [helper] nice.",
"nice helper'а (pid %d): %d -- совпадает с [helper] nice."), (int)helper_pid,
				   ps.nice);
			else
				warn(M4_T("helper nice (pid %d): actually %d, but [helper] nice=%d -- the unit's LimitNICE "
				     "or EPERM may have prevented setpriority().",
"nice helper'а (pid %d): фактически %d, а [helper] nice=%d -- LimitNICE "
				     "юнита или EPERM могли помешать setpriority()."),
				     (int)helper_pid, ps.nice, configured_helper_nice);
		}

		{
			const char *cpu_list = m4_conf_get_string(conf, "helper", "cpu-list");

			if (cpu_list != NULL && cpu_list[0] != '\0' && ps.have_affinity)
				info(M4_T("helper CPU affinity (pid %d): %d core(s) allowed (set via [helper] "
				     "cpu-list=%s).",
"CPU affinity helper'а (pid %d): разрешено ядер %d (задано [helper] "
				     "cpu-list=%s)."),
				     (int)helper_pid, ps.affinity_count, cpu_list);
		}
	}
}

/* `[paths] systemd-user-unit-dir` (where `make install`/`momentum4 install`
 * place/look for our units) must itself be a directory systemd --user
 * actually scans -- otherwise units silently stop being found the moment
 * anyone (a cleanup script, a human) removes what looks like a leftover
 * build directory, even though `install` reported success (see
 * util/systemd_unit.h's m4_systemd_unit_dir_in_search_path() for exactly
 * which directories count and why). Purely read-only: no filesystem
 * access beyond what the caller already resolved from momentum4.conf. */
static void check_systemd_unit_dir(void)
{
	const char *unit_dir = m4_conf_get_string(m4_config(), "paths", "systemd-user-unit-dir");

	if (unit_dir == NULL || unit_dir[0] == '\0') {
		warn(M4_T("[paths] systemd-user-unit-dir is empty -- could not determine where "
		     "`make install`/`momentum4 install` put the units.",
"[paths] systemd-user-unit-dir пуст -- не удалось определить, куда "
		     "`make install`/`momentum4 install` кладут юниты."));
		return;
	}
	if (m4_systemd_unit_dir_in_search_path(unit_dir))
		ok(M4_T("[paths] systemd-user-unit-dir (%s) -- in systemd --user's search path.",
"[paths] systemd-user-unit-dir (%s) -- в пути поиска systemd --user."), unit_dir);
	else
		warn(M4_T("[paths] systemd-user-unit-dir (%s) is NOT in systemd --user's search path "
		     "(check `systemctl --user show -p UnitPath`) -- units installed there "
		     "will only be found as long as the explicit symlinks in ~/.config/systemd/user "
		     "(created by `momentum4 install`) survive; do not remove that directory directly. "
		     "This usually means the build was configured with a non-standard --prefix -- "
		     "rebuild with `--prefix=/usr/local` or `--prefix=$HOME/.local`.",
"[paths] systemd-user-unit-dir (%s) НЕ входит в путь поиска systemd --user "
		     "(проверьте `systemctl --user show -p UnitPath`) -- юниты, установленные туда, "
		     "останутся найдены только пока живы явные симлинки в ~/.config/systemd/user "
		     "(их создаёт `momentum4 install`); удалять этот каталог напрямую нельзя. "
		     "Обычно значит, что сборка была настроена с нестандартным --prefix -- "
		     "пересоберите с `--prefix=/usr/local` или `--prefix=$HOME/.local`."),
		     unit_dir);
}

static void check_overlay(const char *home)
{
	char path[768];

	if (home == NULL || home[0] == '\0')
		return;
	snprintf(path, sizeof(path), "%s/.config/systemd/user/wireplumber.service.d/99-m4-adaptive-overlay.conf",
		 home);
	if (!file_exists(path)) {
		info(M4_T("WirePlumber overlay (99-m4-adaptive-overlay.conf) not found -- the module is not installed yet.",
"overlay WirePlumber (99-m4-adaptive-overlay.conf) не найден -- модуль ещё не установлен."));
		return;
	}
	if (M4_DEV_CHECKOUT_ROOT[0] != '\0' && file_contains(path, M4_DEV_CHECKOUT_ROOT))
		warn(M4_T("overlay %s references the checkout path (%s), not the release path.",
"overlay %s ссылается на путь чекаута (%s), не путь релиза."), path,
		     M4_DEV_CHECKOUT_ROOT);
	else if (M4_DEV_CHECKOUT_ROOT[0] != '\0')
		ok(M4_T("WirePlumber overlay found, no mention of the checkout path.",
"overlay WirePlumber найден, путь чекаута не упоминается."));
}

int m4_cli_doctor_run(int argc, char **argv)
{
	const char *home = getenv("HOME");
	const char *xdg_config = getenv("XDG_CONFIG_HOME");
	const char *configured_unit_dir = m4_conf_get_string(m4_config(), "paths", "systemd-user-unit-dir");
	char user_unit_dirs[5][512];
	size_t n_dirs = 0;

	(void)argv;
	if (argc != 0) {
		fprintf(stderr, "usage: momentum4 doctor\n");
		return 2;
	}

	printf(M4_T("momentum4 doctor -- read-only, changes and restarts nothing.\n\n",
"momentum4 doctor -- только чтение, ничего не меняет и не перезапускает.\n\n"));

	check_pipewire_version();
	check_capi();
	check_helper_binaries();
	check_rt_status();

	if (xdg_config != NULL && xdg_config[0] != '\0')
		snprintf(user_unit_dirs[n_dirs++], sizeof(user_unit_dirs[0]), "%s/systemd/user", xdg_config);
	else if (home != NULL && home[0] != '\0')
		snprintf(user_unit_dirs[n_dirs++], sizeof(user_unit_dirs[0]), "%s/.config/systemd/user", home);
	if (home != NULL && home[0] != '\0')
		snprintf(user_unit_dirs[n_dirs++], sizeof(user_unit_dirs[0]), "%s/.local/share/systemd/user", home);
	/* Also scan the directory `make install`/`momentum4 install` actually
	 * use -- for a system-wide (/usr/local, /usr) install this is a
	 * different directory from the two ~/.config|.local/share ones above
	 * (which only ever hold the EnableUnitFiles-created symlinks/user
	 * overrides, never a system-wide package's own files). */
	if (configured_unit_dir != NULL && configured_unit_dir[0] != '\0')
		snprintf(user_unit_dirs[n_dirs++], sizeof(user_unit_dirs[0]), "%s", configured_unit_dir);
	check_units_and_overlay(user_unit_dirs, n_dirs);
	check_systemd_unit_dir();
	check_overlay(home);

	{
		char momentum4_conf[512];

		if (M4_DEV_CHECKOUT_ROOT[0] != '\0' && m4_config_file_path(momentum4_conf, sizeof(momentum4_conf)) != NULL &&
		    file_exists(momentum4_conf)) {
			if (file_contains(momentum4_conf, M4_DEV_CHECKOUT_ROOT))
				warn(M4_T("%s references the checkout path (%s) -- fine for "
				     "development only, not for a release.",
"%s ссылается на путь чекаута (%s) -- нормально только для "
				     "разработки, не для релиза."),
				     momentum4_conf, M4_DEV_CHECKOUT_ROOT);
			else
				ok(M4_T("%s: no mention of the checkout path.",
"%s: путь чекаута не упоминается."), momentum4_conf);
		}
	}

	printf(M4_T("\nspeech (speech-dispatcher/Orca) is deliberately NOT checked by this command -- "
	       "see README.md \"If the sound disappears entirely\" if you suspect a hang.\n",
"\nречь (speech-dispatcher/Orca) намеренно не проверяется этой командой -- "
	       "см. README.ru.md \"Если что-то пошло не так и звук пропал\" при подозрении на зависание.\n"));

	printf(M4_T("\ntotal warnings: %d\n",
"\nитого предупреждений: %d\n"), g_warnings);
	return g_warnings > 0 ? 1 : 0;
}
