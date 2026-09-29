/* cli/lifecycle.c -- see lifecycle.h.
 *
 * Split of responsibilities :
 *
 *   install    Prepares EVERYTHING except actually starting anything or
 *              touching WirePlumber: pins `[device] address`/`[helper]
 *              qemu,loader,capi` into the user's momentum4.conf (util/
 *              conf_upsert.c) to the values THIS build resolved (closes
 *              the gap where the SPA plugin, built against a possibly
 *              different PipeWire prefix, would otherwise fall back to
 *              its OWN different compiled defaults, see wp_overlay.h's
 *              header for the parallel BUILD-INFO/wp-env mechanism this
 *              is unrelated to); installs the WirePlumber overlay
 *              (util/wp_overlay.c) and, if `[graph] no-suspend`, the
 *              no-suspend drop-in; EnableUnitFiles() on every momentum4
 *              unit found under `[paths] systemd-user-unit-dir`
 *              (util/systemd_unit.c) -- which, given absolute paths, both
 *              links a unit living outside systemd's default search path
 *              AND processes its [Install] section in one call, matching
 *              `systemctl enable /abs/path/to/unit`'s own long-standing
 *              behaviour; daemon-reload. NEVER starts a unit, NEVER
 *              touches wireplumber.service -- zero audio impact.
 *   enable     The one operation that actually changes what M4 hears:
 *              starts momentum4d/momentum4-helper.socket/momentum4-graph,
 *              then restarts wireplumber.service. Always prints the
 *              "sound will cut out for a few seconds" warning first,
 *              whether or not --yes was given.
 *   disable    The user-facing rollback: stops our
 *              units, DisableUnitFiles() (so a future login does not
 *              silently reactivate momentum4-helper.socket's
 *              WantedBy=default.target), removes the WirePlumber overlay
 *              AND the no-suspend drop-in, daemon-reload, restarts
 *              wireplumber.service -- M4 returns to whatever codec
 *              WirePlumber's OWN stock bluez5 SPA plugin negotiates
 *              (ordinarily aptX HD). Does NOT touch momentum4.conf, does
 *              NOT remove the linked unit FILES (`momentum4 enable` after
 *              a `disable` works again without re-running `install`).
 *              Deliberately does not attempt to fix/restart speech-
 * dispatcher/Orca -- out of scope for this
 *              binary, see docs/user/README.ru.md's own "if the sound disappears"
 *              section (Russian heading); the English README.md covers the same command under "If the
 *              sound disappears entirely").
 *   uninstall  disable, then also removes the linked unit files
 *              themselves and, with --purge, momentum4.conf and our own
 *              copy of the CAPI file (only the well-known
 *              $XDG_DATA_HOME/momentum4/capi/ location this project
 *              itself copies files into via `momentum4 setup --capi` --
 *              never a user-pointed-elsewhere path).
 *
 * Every one of the four is dry-run by default (prints the exact plan,
 * touches NOTHING -- no sd-bus connection is even attempted in dry-run,
 * so the plan is provably side-effect-free by inspection, not just by a
 * runtime check) and requires --yes to actually do anything. `enable`/
 * `disable` additionally print the WirePlumber-restart warning even in
 * dry-run, so a user reading the plan already knows what to expect.
 */
#include "lifecycle.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../../common/m4-conf.h"
#include "../../../common/m4-i18n.h"
#include "../util/config.h"
#include "../util/conf_upsert.h"
#include "../util/systemd_unit.h"
#include "../util/wp_overlay.h"

/* Every unit this project ships -- present in this exact
 * order everywhere below (install enables them in this order; disable/
 * uninstall stop/disable them in REVERSE, daemon last to first stopped
 * gracefully before the socket/graph disappear from under it). */
static const char *const M4_UNIT_NAMES[] = {
	"momentum4.target",
	"momentum4d.service",
	"momentum4-helper.socket",
	"momentum4-graph.service",
};
#define M4_UNIT_COUNT (sizeof(M4_UNIT_NAMES) / sizeof(M4_UNIT_NAMES[0]))

static bool parse_yes_flag(int argc, char **argv, bool *purge_out)
{
	int i;
	bool yes = false;

	if (purge_out != NULL)
		*purge_out = false;
	for (i = 0; i < argc; i++) {
		if (strcmp(argv[i], "--yes") == 0)
			yes = true;
		else if (purge_out != NULL && strcmp(argv[i], "--purge") == 0)
			*purge_out = true;
	}
	return yes;
}

static void unit_path(const char *unit_dir, const char *name, char *out, size_t out_len)
{
	snprintf(out, out_len, "%s/%s", unit_dir, name);
}

/* ------------------------------------------------------------- install -- */

int m4_cli_install_run(int argc, char **argv)
{
	bool yes = parse_yes_flag(argc, argv, NULL);
	const m4_conf_t *cfg = m4_config();
	const char *mac = m4_config_mac_address();
	const char *unit_dir = m4_conf_get_string(cfg, "paths", "systemd-user-unit-dir");
	const char *helper_qemu = m4_conf_get_string(cfg, "helper", "qemu");
	const char *helper_loader = m4_conf_get_string(cfg, "helper", "loader");
	const char *helper_capi = m4_conf_get_string(cfg, "helper", "capi");
	bool want_no_suspend = m4_conf_get_bool(cfg, "graph", "no-suspend", true);
	char momentum4_conf_path[512];
	char present_paths[M4_UNIT_COUNT][768];
	const char *present_paths_p[M4_UNIT_COUNT];
	size_t n_present = 0;
	size_t i;

	if (argc > 1 || (argc == 1 && strcmp(argv[0], "--yes") != 0)) {
		fprintf(stderr, "usage: momentum4 install [--yes]\n");
		return 2;
	}

	printf("momentum4 install -- %s\n\n", yes ? M4_T("APPLYING",
"ПРИМЕНЯЮ") : M4_T("dry run (--yes to apply)",
"сухой прогон (--yes для применения)"));

	if (unit_dir == NULL || unit_dir[0] == '\0') {
		fprintf(stderr, M4_T("momentum4 install: [paths] systemd-user-unit-dir is empty -- cannot find "
				"the units. Rebuild (`make`) or set the value by hand in momentum4.conf.\n",
"momentum4 install: [paths] systemd-user-unit-dir пуст -- не могу найти "
				"юниты. Пересоберите (`make`) или задайте значение вручную в momentum4.conf.\n"));
		return 1;
	}
	if (!m4_systemd_unit_dir_in_search_path(unit_dir)) {
		fprintf(stderr,
			M4_T("ERROR: [paths] systemd-user-unit-dir (%s) is not in "
			"systemd --user's search path (check `systemctl --user show -p UnitPath`). The "
			"units will still be enabled via an absolute path (EnableUnitFiles will create a symlink "
			"in ~/.config/systemd/user), but NEVER remove %s by hand -- it is the only "
			"place that symlink points to. This usually means the build was configured with "
			"a non-standard --prefix -- rebuild with --prefix=/usr/local or "
			"--prefix=$HOME/.local to make this warning go away.\n\n",
"ОШИБКА: [paths] systemd-user-unit-dir (%s) не входит в путь поиска "
			"systemd --user (проверьте `systemctl --user show -p UnitPath`). Юниты всё "
			"равно будут включены через абсолютный путь (EnableUnitFiles создаст симлинк "
			"в ~/.config/systemd/user), но НИКОГДА не удаляйте %s руками -- это единственное "
			"место, куда указывает этот симлинк. Обычно это значит, что сборка настроена с "
			"нестандартным --prefix -- пересоберите с --prefix=/usr/local или "
			"--prefix=$HOME/.local, чтобы это предупреждение исчезло.\n\n"),
			unit_dir, unit_dir);
	}

	printf(M4_T("momentum4.conf will be updated (%s):\n",
"momentum4.conf будет обновлён (%s):\n"),
	       m4_config_file_path(momentum4_conf_path, sizeof(momentum4_conf_path)) != NULL
		       ? momentum4_conf_path
		       : M4_T("path not determined",
"путь не определён"));
	printf("  [device] address = %s\n", (mac != NULL && mac[0] != '\0') ? mac : M4_T("(not detected -- "
										     "no-suspend will be skipped)",
"(не обнаружен -- "
										     "no-suspend будет пропущен)"));
	printf("  [helper] qemu    = %s\n", helper_qemu);
	printf("  [helper] loader  = %s\n", helper_loader);
	if (helper_capi != NULL && helper_capi[0] != '\0' && access(helper_capi, F_OK) == 0)
		printf("  [helper] capi    = %s\n", helper_capi);
	else
		printf(M4_T("  [helper] capi    -- not found yet, skipped (see `momentum4 setup --capi`, "
		       "docs/user/obtaining-encoder.md)\n",
"  [helper] capi    -- пока не найден, пропущено (см. `momentum4 setup --capi`, "
		       "docs/user/obtaining-encoder.md)\n"));

	printf("\nWirePlumber:\n");
	printf("  overlay: ~/.config/systemd/user/wireplumber.service.d/99-m4-adaptive-overlay.conf\n");
	if (want_no_suspend && mac != NULL && mac[0] != '\0')
		printf("  no-suspend: ~/.config/wireplumber/wireplumber.conf.d/51-m4-no-suspend.conf (MAC %s)\n",
		       mac);
	else
		printf(M4_T("  no-suspend: skipped (%s)\n",
"  no-suspend: пропущено (%s)\n"),
		       want_no_suspend ? M4_T("M4 MAC not detected",
"MAC M4 не обнаружен") : "[graph] no-suspend = off");
	printf(M4_T("  WirePlumber is NOT restarted by this command (see `momentum4 enable`).\n",
"  WirePlumber НЕ перезапускается этой командой (см. `momentum4 enable`).\n"));

	printf(M4_T("\nUnits (%s):\n",
"\nЮниты (%s):\n"), unit_dir);
	for (i = 0; i < M4_UNIT_COUNT; i++) {
		char path[768];

		unit_path(unit_dir, M4_UNIT_NAMES[i], path, sizeof(path));
		if (access(path, F_OK) == 0) {
			printf(M4_T("  found:      %s\n",
"  найден:     %s\n"), path);
			snprintf(present_paths[n_present], sizeof(present_paths[0]), "%s", path);
			present_paths_p[n_present] = present_paths[n_present];
			n_present++;
		} else {
			printf(M4_T("  MISSING:    %s (make install?)\n",
"  ОТСУТСТВУЕТ: %s (make install?)\n"), path);
		}
	}
	printf(M4_T("  will be enabled (EnableUnitFiles) -- will NOT be started (see `momentum4 enable`).\n",
"  будут включены (EnableUnitFiles) -- запущены НЕ будут (см. `momentum4 enable`).\n"));
	printf("  daemon-reload.\n");

	if (!yes) {
		printf(M4_T("\nDRY RUN -- nothing changed. Re-run with --yes to apply.\n",
"\nСУХОЙ ПРОГОН -- ничего не изменено. Повторите с --yes, чтобы применить.\n"));
		return 0;
	}

	{
		struct m4_conf_upsert_kv kvs[4];
		size_t n_kvs = 0;
		char err[256];
		int rc;

		if (mac != NULL && mac[0] != '\0') {
			kvs[n_kvs].section = "device";
			kvs[n_kvs].key = "address";
			kvs[n_kvs].value = mac;
			n_kvs++;
		}
		kvs[n_kvs].section = "helper";
		kvs[n_kvs].key = "qemu";
		kvs[n_kvs].value = helper_qemu;
		n_kvs++;
		kvs[n_kvs].section = "helper";
		kvs[n_kvs].key = "loader";
		kvs[n_kvs].value = helper_loader;
		n_kvs++;
		if (helper_capi != NULL && helper_capi[0] != '\0' && access(helper_capi, F_OK) == 0) {
			kvs[n_kvs].section = "helper";
			kvs[n_kvs].key = "capi";
			kvs[n_kvs].value = helper_capi;
			n_kvs++;
		}
		rc = m4_conf_upsert(momentum4_conf_path, kvs, n_kvs, err, sizeof(err));
		if (rc < 0) {
			fprintf(stderr, "\nmomentum4 install: momentum4.conf: %s\n", err);
			return 1;
		}
		printf(M4_T("\nmomentum4.conf: %d key%s updated/added.\n",
"\nmomentum4.conf: %d ключ%s обновлено/добавлено.\n"), rc,
		       rc == 1 ? "" : (rc >= 2 && rc <= 4 ? M4_T("s",
"а") : M4_T("s",
"ей")));
	}

	{
		char err[256];

		if (m4_wp_overlay_install(err, sizeof(err)) != 0) {
			fprintf(stderr, "momentum4 install: overlay WirePlumber: %s\n", err);
			return 1;
		}
		printf(M4_T("WirePlumber overlay installed.\n",
"overlay WirePlumber установлен.\n"));

		if (want_no_suspend && mac != NULL && mac[0] != '\0') {
			if (m4_no_suspend_install(mac, err, sizeof(err)) != 0) {
				fprintf(stderr, M4_T("momentum4 install: no-suspend: %s (not critical -- continuing)\n",
"momentum4 install: no-suspend: %s (не критично -- продолжаю)\n"),
					err);
			} else {
				printf(M4_T("no-suspend installed.\n",
"no-suspend установлен.\n"));
			}
		}
	}

	{
		sd_bus *bus = NULL;
		char err[256];

		if (n_present == 0) {
			printf(M4_T("\nno units found under %s -- nothing to enable (run `make install` first).\n",
"\nни один юнит не найден под %s -- нечего включать (`make install` сначала).\n"),
			       unit_dir);
			return 0;
		}
		if (m4_systemd_user_connect(&bus, err, sizeof(err)) != 0) {
			fprintf(stderr, "\nmomentum4 install: %s\n", err);
			return 1;
		}
		if (m4_systemd_enable_unit_files(bus, present_paths_p, n_present, false, err, sizeof(err)) !=
		    0) {
			fprintf(stderr, "\nmomentum4 install: EnableUnitFiles: %s\n", err);
			sd_bus_unref(bus);
			return 1;
		}
		if (m4_systemd_daemon_reload(bus, err, sizeof(err)) != 0)
			fprintf(stderr, M4_T("momentum4 install: daemon-reload: %s (not critical)\n",
"momentum4 install: daemon-reload: %s (не критично)\n"), err);
		sd_bus_unref(bus);
	}

	printf(M4_T("\nDone. Units are enabled, but not started, and WirePlumber was not touched. Next: "
	       "`momentum4 enable --yes` (interrupts audio for a few seconds).\n",
"\nГотово. Юниты включены, но не запущены и WirePlumber не тронут. Дальше: "
	       "`momentum4 enable --yes` (прервёт звук на несколько секунд).\n"));
	return 0;
}

/* -------------------------------------------------------------- enable -- */

int m4_cli_enable_run(int argc, char **argv)
{
	bool yes = parse_yes_flag(argc, argv, NULL);
	sd_bus *bus = NULL;
	char err[256];
	size_t i;
	bool had_error = false;

	if (argc > 1 || (argc == 1 && strcmp(argv[0], "--yes") != 0)) {
		fprintf(stderr, "usage: momentum4 enable [--yes]\n");
		return 2;
	}

	printf("momentum4 enable -- %s\n\n", yes ? M4_T("APPLYING",
"ПРИМЕНЯЮ") : M4_T("dry run (--yes to apply)",
"сухой прогон (--yes для применения)"));
	printf(M4_T("WARNING: restarting wireplumber.service will interrupt audio (on all devices, "
	       "not just the M4) for a few seconds.\n\n",
"ПРЕДУПРЕЖДЕНИЕ: рестарт wireplumber.service прервёт звук (в т.ч. на всех устройствах, "
	       "не только M4) на несколько секунд.\n\n"));
	printf(M4_T("Plan:\n",
"План:\n"));
	for (i = 0; i < M4_UNIT_COUNT; i++)
		if (strcmp(M4_UNIT_NAMES[i], "momentum4.target") != 0)
			printf("  StartUnit(%s)\n", M4_UNIT_NAMES[i]);
	printf("  RestartUnit(wireplumber.service)\n");

	if (!yes) {
		printf(M4_T("\nDRY RUN -- nothing changed. Re-run with --yes to apply.\n",
"\nСУХОЙ ПРОГОН -- ничего не изменено. Повторите с --yes, чтобы применить.\n"));
		return 0;
	}

	if (m4_systemd_user_connect(&bus, err, sizeof(err)) != 0) {
		fprintf(stderr, "\nmomentum4 enable: %s\n", err);
		return 1;
	}

	printf("\n");
	for (i = 0; i < M4_UNIT_COUNT; i++) {
		if (strcmp(M4_UNIT_NAMES[i], "momentum4.target") == 0)
			continue;
		if (m4_systemd_start_unit(bus, M4_UNIT_NAMES[i], err, sizeof(err)) != 0) {
			fprintf(stderr, "StartUnit(%s): %s\n", M4_UNIT_NAMES[i], err);
			had_error = true;
		} else {
			printf("StartUnit(%s): ok\n", M4_UNIT_NAMES[i]);
		}
	}
	if (m4_systemd_restart_unit(bus, "wireplumber.service", err, sizeof(err)) != 0) {
		fprintf(stderr, "RestartUnit(wireplumber.service): %s\n", err);
		had_error = true;
	} else {
		printf("RestartUnit(wireplumber.service): ok\n");
	}

	sleep(1);
	printf(M4_T("\nReport:\n",
"\nОтчёт:\n"));
	for (i = 0; i < M4_UNIT_COUNT; i++) {
		char state[64];

		if (strcmp(M4_UNIT_NAMES[i], "momentum4.target") == 0)
			continue;
		m4_systemd_get_active_state(bus, M4_UNIT_NAMES[i], state, sizeof(state));
		printf("  %-28s %s\n", M4_UNIT_NAMES[i], state);
	}
	{
		char state[64];

		m4_systemd_get_active_state(bus, "wireplumber.service", state, sizeof(state));
		printf("  %-28s %s\n", "wireplumber.service", state);
	}
	printf(M4_T("\nCheck the audio (`spd-say test` or similar). If it is silent -- `momentum4 disable --yes` "
	       "rolls back our part; the M4 should return to plain aptX HD.\n",
"\nПроверьте звук (`spd-say test` или аналог). Если тихо -- `momentum4 disable --yes` "
	       "откатывает нашу часть; M4 должен вернуться на обычный aptX HD.\n"));

	sd_bus_unref(bus);
	return had_error ? 1 : 0;
}

/* ------------------------------------------------------------- disable -- */

static int disable_common(bool yes, bool report_next_steps)
{
	sd_bus *bus = NULL;
	char err[256];
	size_t i;
	bool had_error = false;
	const char *stop_order[] = {
		"momentum4-graph.service",
		"momentum4-helper.socket",
		"momentum4d.service",
	};
	const size_t n_stop = sizeof(stop_order) / sizeof(stop_order[0]);

	printf(M4_T("WARNING: restarting wireplumber.service will interrupt audio (on all devices) "
	       "for a few seconds.\n\n",
"ПРЕДУПРЕЖДЕНИЕ: рестарт wireplumber.service прервёт звук (в т.ч. на всех устройствах) "
	       "на несколько секунд.\n\n"));
	printf(M4_T("Plan:\n",
"План:\n"));
	for (i = 0; i < n_stop; i++)
		printf("  StopUnit(%s)\n", stop_order[i]);
	printf("  DisableUnitFiles(%s)\n", "momentum4d.service, momentum4-helper.socket, "
					     "momentum4-graph.service, momentum4.target");
	printf(M4_T("  removing the WirePlumber overlay and the no-suspend drop-in\n",
"  удаление overlay WirePlumber и no-suspend drop-in\n"));
	printf("  daemon-reload\n");
	printf("  RestartUnit(wireplumber.service)\n");

	if (!yes) {
		printf(M4_T("\nDRY RUN -- nothing changed. Re-run with --yes to apply.\n",
"\nСУХОЙ ПРОГОН -- ничего не изменено. Повторите с --yes, чтобы применить.\n"));
		return 0;
	}

	if (m4_systemd_user_connect(&bus, err, sizeof(err)) != 0) {
		fprintf(stderr, "\nmomentum4 disable: %s\n", err);
		return 1;
	}

	printf("\n");
	for (i = 0; i < n_stop; i++) {
		if (m4_systemd_stop_unit(bus, stop_order[i], err, sizeof(err)) != 0) {
			fprintf(stderr, M4_T("StopUnit(%s): %s (continuing)\n",
"StopUnit(%s): %s (продолжаю)\n"), stop_order[i], err);
			had_error = true;
		} else {
			printf("StopUnit(%s): ok\n", stop_order[i]);
		}
	}
	if (m4_systemd_disable_unit_files(bus, M4_UNIT_NAMES, M4_UNIT_COUNT, err, sizeof(err)) != 0) {
		fprintf(stderr, M4_T("DisableUnitFiles: %s (continuing)\n",
"DisableUnitFiles: %s (продолжаю)\n"), err);
		had_error = true;
	} else {
		printf("DisableUnitFiles: ok\n");
	}

	if (m4_wp_overlay_remove(err, sizeof(err)) != 0) {
		fprintf(stderr, M4_T("WirePlumber overlay: %s (continuing)\n",
"overlay WirePlumber: %s (продолжаю)\n"), err);
		had_error = true;
	} else {
		printf(M4_T("WirePlumber overlay removed.\n",
"overlay WirePlumber удалён.\n"));
	}
	if (m4_no_suspend_remove(err, sizeof(err)) != 0) {
		fprintf(stderr, M4_T("no-suspend: %s (continuing)\n",
"no-suspend: %s (продолжаю)\n"), err);
		had_error = true;
	} else {
		printf(M4_T("no-suspend removed.\n",
"no-suspend удалён.\n"));
	}

	if (m4_systemd_daemon_reload(bus, err, sizeof(err)) != 0)
		fprintf(stderr, M4_T("daemon-reload: %s (not critical)\n",
"daemon-reload: %s (не критично)\n"), err);

	if (m4_systemd_restart_unit(bus, "wireplumber.service", err, sizeof(err)) != 0) {
		fprintf(stderr, "RestartUnit(wireplumber.service): %s\n", err);
		had_error = true;
	} else {
		printf("RestartUnit(wireplumber.service): ok\n");
	}

	sd_bus_unref(bus);

	if (report_next_steps)
		printf(M4_T("\nDone. The M4 should be back on plain aptX HD. Check the audio. Speech "
		       "(speech-dispatcher/Orca) is not checked or fixed by this command -- see "
		       "README.md \"If the sound disappears entirely\" (Russian: README.ru.md, "
		       "\"Если что-то пошло не так и звук пропал\").\n",
"\nГотово. M4 должен вернуться на обычный aptX HD. Проверьте звук. Речь "
		       "(speech-dispatcher/Orca) этой командой не проверяется и не чинится -- см. "
		       "README.ru.md \"Если что-то пошло не так и звук пропал\" (English: README.md, "
		       "\"If the sound disappears entirely\").\n"));
	return had_error ? 1 : 0;
}

int m4_cli_disable_run(int argc, char **argv)
{
	bool yes = parse_yes_flag(argc, argv, NULL);

	if (argc > 1 || (argc == 1 && strcmp(argv[0], "--yes") != 0)) {
		fprintf(stderr, "usage: momentum4 disable [--yes]\n");
		return 2;
	}
	printf("momentum4 disable -- %s\n\n", yes ? M4_T("APPLYING",
"ПРИМЕНЯЮ") : M4_T("dry run (--yes to apply)",
"сухой прогон (--yes для применения)"));
	return disable_common(yes, true);
}

/* ------------------------------------------------------------ uninstall -- */

int m4_cli_uninstall_run(int argc, char **argv)
{
	bool purge = false;
	bool yes = parse_yes_flag(argc, argv, &purge);
	const char *unit_dir = m4_conf_get_string(m4_config(), "paths", "systemd-user-unit-dir");
	const char *home = getenv("HOME");
	int rc;
	int a;
	size_t i;

	for (a = 0; a < argc; a++) {
		if (strcmp(argv[a], "--yes") == 0 || strcmp(argv[a], "--purge") == 0)
			continue;
		fprintf(stderr, "usage: momentum4 uninstall [--purge] [--yes]\n");
		return 2;
	}

	printf("momentum4 uninstall -- %s%s\n\n", yes ? M4_T("APPLYING",
"ПРИМЕНЯЮ") : M4_T("dry run (--yes to apply)",
"сухой прогон (--yes для применения)"),
	       purge ? M4_T(" (--purge: also momentum4.conf and our CAPI copy)",
" (--purge: также momentum4.conf и наша копия CAPI)") : "");

	rc = disable_common(yes, false);

	if (!yes) {
		printf(M4_T("\nAfter disable, also:\n",
"\nПосле disable также:\n"));
		printf(M4_T("  removes the unit links under %s\n",
"  удаление линков юнитов под %s\n"), unit_dir);
		if (purge) {
			char conf_path[512];

			printf(M4_T("  --purge: removes %s\n",
"  --purge: удаление %s\n"),
			       m4_config_file_path(conf_path, sizeof(conf_path)) != NULL ? conf_path
											    : "momentum4.conf");
			printf(M4_T("  --purge: removes $XDG_DATA_HOME/momentum4/capi/ (our copy, "
			       "`momentum4 setup --capi`)\n",
"  --purge: удаление $XDG_DATA_HOME/momentum4/capi/ (наша копия, "
			       "`momentum4 setup --capi`)\n"));
		}
		printf(M4_T("\nDRY RUN -- nothing changed. Re-run with --yes to apply.\n",
"\nСУХОЙ ПРОГОН -- ничего не изменено. Повторите с --yes, чтобы применить.\n"));
		return 0;
	}

	if (rc != 0)
		fprintf(stderr, M4_T("\nmomentum4 uninstall: disable returned error(s) above -- continuing with cleanup.\n",
"\nmomentum4 uninstall: disable вернул ошибку(и) выше -- продолжаю очистку.\n"));

	for (i = 0; i < M4_UNIT_COUNT; i++) {
		char path[768];

		if (home == NULL || home[0] == '\0')
			break;
		snprintf(path, sizeof(path), "%s/.config/systemd/user/%s", home, M4_UNIT_NAMES[i]);
		if (unlink(path) == 0)
			printf(M4_T("removed unit link: %s\n",
"удалён линк юнита: %s\n"), path);
		/* ENOENT is the expected/common case (DisableUnitFiles already
		 * removed it, or install/enable never ran) -- not reported. */
	}

	if (purge) {
		char conf_path[512];
		const char *xdg_data = getenv("XDG_DATA_HOME");
		char capi_dir[600];

		if (m4_config_file_path(conf_path, sizeof(conf_path)) != NULL) {
			if (unlink(conf_path) == 0)
				printf(M4_T("removed: %s\n",
"удалён: %s\n"), conf_path);
		}
		if (xdg_data != NULL && xdg_data[0] != '\0')
			snprintf(capi_dir, sizeof(capi_dir), "%s/momentum4/capi", xdg_data);
		else if (home != NULL && home[0] != '\0')
			snprintf(capi_dir, sizeof(capi_dir), "%s/.local/share/momentum4/capi", home);
		else
			capi_dir[0] = '\0';
		if (capi_dir[0] != '\0') {
			char cmd_note[700];

			snprintf(cmd_note, sizeof(cmd_note), "%s", capi_dir);
			printf(M4_T("--purge: the CAPI directory %s was deliberately left untouched -- remove it by hand "
			       "if you no longer need it (rm -r '%s'); momentum4 never removes whole "
			       "directories itself, only the specific files it put there.\n",
"--purge: каталог CAPI %s оставлен нетронутым намеренно -- удалите вручную, "
			       "если он вам больше не нужен (rm -r '%s'); momentum4 сам не удаляет целые "
			       "каталоги, только конкретные файлы, которые сам туда положил.\n"),
			       cmd_note, cmd_note);
		}
	}

	printf(M4_T("\nDone.\n",
"\nГотово.\n"));
	return rc;
}
