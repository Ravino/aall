/* main.c -- dispatcher for the single `momentum4` binary.
 * `momentum4 <subcommand> [args..]`. Implements
 * `status` (read-only), `version`, `daemon`
 * (also reachable as argv[0]=="momentum4d", the systemd unit ExecStart
 * symlink target), the ctl.sock-backed `call`/`mode`, and the
 * local-only `resume`. Every remaining subcommand (auto, stats, log,
 * doctor, safe, config, control) is still an explicit stub -- "do not
 * touch the live system" applies here too, and a silent no-op stub
 * would be indistinguishable from a bug.
 */
#include <libgen.h>
#include <stdio.h>
#include <string.h>

#include <pipewire/pipewire.h>

#include "cli/call.h"
#include "cli/config.h"
#include "cli/daemon_cmd.h"
#include "cli/doctor.h"
#include "cli/graph_host.h"
#include "cli/helper_exec.h"
#include "cli/lifecycle.h"
#include "cli/log.h"
#include "cli/mic.h"
#include "cli/mode.h"
#include "cli/player.h"
#include "cli/repair_a2dp.h"
#include "cli/resume.h"
#include "cli/setup.h"
#include "cli/stats.h"
#include "cli/status.h"
#include "cli/wp_env.h"
#include "util/config.h"

#include "../../common/m4-i18n.h"

/* MOMENTUM4_VERSION is defined by the build (meson.build's
 * project_version()) -- this fallback only fires for a stray
 * out-of-meson compile (e.g. a lone `gcc main.c` while poking around). */
#ifndef MOMENTUM4_VERSION
#define MOMENTUM4_VERSION "0.0.0-unknown-build"
#endif

static int cmd_version(void)
{
	printf("momentum4 %s\n", MOMENTUM4_VERSION);
	return 0;
}

static int cmd_not_implemented(const char *name)
{
	fprintf(stderr,
		M4_T("momentum4 %s: not implemented in this release.\n"
		"The live system is not changed by this binary.\n",
"momentum4 %s: ещё не реализовано в этом релизе.\n"
		"Живая система этим бинарником не меняется.\n"),
		name);
	return 1;
}

static void print_usage(FILE *out)
{
	fprintf(out,
		M4_T("usage: momentum4 <subcommand> [args...]\n"
		"  status [--json]     short summary (no daemon: direct read;\n"
		"                      with daemon: via ctl.sock)\n"
		"  daemon              run the momentum4 core (momentum4d.service)\n"
		"  call on|off         via the daemon (ctl.sock); an error without the daemon\n"
		"  mic on|off|toggle|status  M4 microphone during a call (via the daemon)\n"
		"  mode [observe|manual|auto|get]  daemon mode via ctl.sock\n"
		"  repair-a2dp         restore a2dp-sink (ConnectProfile); via the daemon if alive,\n"
		"                      otherwise a direct call (works without the daemon)\n"
		"  resume              release the safe-latch (works without the daemon)\n"
		"  player [status stopped|playing|paused|follow | resync]\n"
		"                      the daemon's MPRIS player in BlueZ (M4 gestures): state /\n"
		"                      status the headset sees until the daemon restarts\n"
		"  reload              re-read momentum4.conf (gestures, player-status, earcon;\n"
		"                      mode and player on/off -- only on restart)\n"
		"  config show|get|set|validate|migrate|defaults\n"
		"                      momentum4.conf: effective values / validation / migration\n"
		"                      of the old format / full reference (common/m4-conf)\n"
		"  stats [--json] [--history [N]] [--path FILE]\n"
		"                      aptX Adaptive bridge metrics (adaptive-stats.json)\n"
		"  doctor              read-only: CAPI/qemu/loader/units/overlay, no repairs\n"
		"  log [-n N] [--follow]  latest momentum4 entries in journald\n"
		"  setup --capi PATH [--force]  check and wire up the CAPI encoder (see\n"
		"                      docs/user/obtaining-encoder.md)\n"
		"  helper-exec         ExecStart= of the helper unit (qemu-hexagon), not for manual use\n"
		"  graph-host          [graph]: combine/mic-placeholder/force-quantum, ExecStart=\n"
		"                      of momentum4-graph.service, runs until SIGTERM\n"
		"  wp-env              ExecStartPre= of the WirePlumber overlay -- writes SPA_PLUGIN_DIR,\n"
		"                      only if the PipeWire version matches the module's BUILD-INFO\n"
		"  install [--yes]     pin down paths in momentum4.conf, WirePlumber overlay/no-suspend,\n"
		"                      enable units (no start, no WirePlumber restart)\n"
		"  enable [--yes]      start the units and restart WirePlumber (interrupts audio)\n"
		"  disable [--yes]     roll back our part: stop units, remove overlay/no-suspend,\n"
		"                      restart WirePlumber -- M4 back on plain aptX HD\n"
		"  uninstall [--purge] [--yes]  disable + remove unit links; --purge also removes\n"
		"                      momentum4.conf and our CAPI copy\n"
		"  version             build version\n"
		"  auto, safe, control reserved, not implemented in this release\n",
"usage: momentum4 <subcommand> [args...]\n"
		"  status [--json]     краткая сводка (без демона: прямое чтение;\n"
		"                      с демоном: через ctl.sock)\n"
		"  daemon              запустить демон (momentum4d.service)\n"
		"  call on|off         через демон (ctl.sock); без демона -- ошибка\n"
		"  mic on|off|toggle|status  микрофон M4 во время звонка (через демон)\n"
		"  mode [observe|manual|auto|get]  режим демона через ctl.sock\n"
		"  repair-a2dp         восстановить a2dp-sink (ConnectProfile); демон -- если жив,\n"
		"                      иначе прямой вызов (работает без демона)\n"
		"  resume              снять защёлку safe-latch (работает без демона)\n"
		"  player [status stopped|playing|paused|follow | resync]\n"
		"                      MPRIS-плеер демона в BlueZ (жесты M4): состояние /\n"
		"                      статус для наушников до перезапуска демона\n"
		"  reload              перечитать momentum4.conf (жесты, player-status, earcon;\n"
		"                      режим и player on/off -- только при перезапуске)\n"
		"  config show|get|set|validate|migrate|defaults\n"
		"                      momentum4.conf: эффективные значения / проверка / перенос\n"
		"                      старого формата / полный справочник (common/m4-conf)\n"
		"  stats [--json] [--history [N]] [--path FILE]\n"
		"                      метрики моста aptX Adaptive (adaptive-stats.json)\n"
		"  doctor              только чтение: CAPI/qemu/loader/юниты/overlay, без починки\n"
		"  log [-n N] [--follow]  последние записи momentum4 в journald\n"
		"  setup --capi PATH [--force]  проверить и подключить CAPI-энкодер (см.\n"
		"                      docs/user/obtaining-encoder.md)\n"
		"  helper-exec         ExecStart= helper-юнита (qemu-hexagon), не для ручного запуска\n"
		"  graph-host          [graph]: combine/mic-placeholder/force-quantum, ExecStart=\n"
		"                      momentum4-graph.service, работает до SIGTERM\n"
		"  wp-env              ExecStartPre= overlay'я WirePlumber -- пишет SPA_PLUGIN_DIR,\n"
		"                      только если версия PipeWire совпадает с BUILD-INFO модуля\n"
		"  install [--yes]     закрепить пути в momentum4.conf, overlay/no-suspend WirePlumber,\n"
		"                      включить юниты (без запуска, без рестарта WirePlumber)\n"
		"  enable [--yes]      запустить юниты и перезапустить WirePlumber (прервёт звук)\n"
		"  disable [--yes]     откатить нашу часть: остановить юниты, убрать overlay/no-suspend,\n"
		"                      перезапустить WirePlumber -- M4 на обычном aptX HD\n"
		"  uninstall [--purge] [--yes]  disable + удалить линки юнитов; --purge -- ещё\n"
		"                      momentum4.conf и нашу копию CAPI\n"
		"  version             версия сборки\n"
		"  auto, safe, control зарезервировано, не реализовано в этой фазе\n"));
}

int main(int argc, char **argv)
{
	const char *prog_name;
	const char *subcommand;
	int rc;

	/* [general] language (common/m4-i18n.h): decided ONCE, before looking
	 * at argv[1] at all -- so `momentum4 --help`/an unrecognized
	 * subcommand/the momentum4d re-exec branch right below all respect it
	 * too, not just the subcommands that happen to load util/config.h's
	 * m4_config() themselves for other keys. */
	m4_i18n_init_from_conf(m4_config());

	/* basename() may modify its argument on some implementations; argv[0]
	 * is not read again after this, so that is fine. */
	prog_name = basename(argv[0]);
	if (strcmp(prog_name, "momentum4d") == 0) {
		pw_init(&argc, &argv);
		rc = m4_cli_daemon_run(0, NULL);
		pw_deinit();
		return rc;
	}

	if (argc < 2) {
		print_usage(stderr);
		return 2;
	}
	subcommand = argv[1];

	if (strcmp(subcommand, "-h") == 0 || strcmp(subcommand, "--help") == 0) {
		print_usage(stdout);
		return 0;
	}

	/* pw_init()/pw_deinit(): once per process, regardless of which
	 * subcommand runs (graph_pw.h: "caller must have already called
	 * pw_init()"). Subcommands that never touch PipeWire (version, the
	 * not-implemented stubs) pay a negligible, argument-parsing-only cost
	 * for this (pw_init() does not connect to anything by itself). */
	pw_init(&argc, &argv);

	if (strcmp(subcommand, "status") == 0) {
		rc = m4_cli_status_run(argc - 2, argv + 2);
	} else if (strcmp(subcommand, "version") == 0) {
		rc = cmd_version();
	} else if (strcmp(subcommand, "daemon") == 0) {
		rc = m4_cli_daemon_run(argc - 2, argv + 2);
	} else if (strcmp(subcommand, "call") == 0) {
		rc = m4_cli_call_run(argc - 2, argv + 2);
	} else if (strcmp(subcommand, "mic") == 0) {
		rc = m4_cli_mic_run(argc - 2, argv + 2);
	} else if (strcmp(subcommand, "mode") == 0) {
		rc = m4_cli_mode_run(argc - 2, argv + 2);
	} else if (strcmp(subcommand, "repair-a2dp") == 0) {
		rc = m4_cli_repair_a2dp_run(argc - 2, argv + 2);
	} else if (strcmp(subcommand, "player") == 0) {
		rc = m4_cli_player_run(argc - 2, argv + 2);
	} else if (strcmp(subcommand, "reload") == 0) {
		rc = m4_cli_reload_run(argc - 2, argv + 2);
	} else if (strcmp(subcommand, "resume") == 0) {
		rc = m4_cli_resume_run(argc - 2, argv + 2);
	} else if (strcmp(subcommand, "config") == 0) {
		rc = m4_cli_config_run(argc - 2, argv + 2);
	} else if (strcmp(subcommand, "stats") == 0) {
		rc = m4_cli_stats_run(argc - 2, argv + 2);
	} else if (strcmp(subcommand, "doctor") == 0) {
		rc = m4_cli_doctor_run(argc - 2, argv + 2);
	} else if (strcmp(subcommand, "log") == 0) {
		rc = m4_cli_log_run(argc - 2, argv + 2);
	} else if (strcmp(subcommand, "setup") == 0) {
		rc = m4_cli_setup_run(argc - 2, argv + 2);
	} else if (strcmp(subcommand, "helper-exec") == 0) {
		rc = m4_cli_helper_exec_run(argc - 2, argv + 2);
	} else if (strcmp(subcommand, "graph-host") == 0) {
		rc = m4_cli_graph_host_run(argc - 2, argv + 2);
	} else if (strcmp(subcommand, "wp-env") == 0) {
		rc = m4_cli_wp_env_run(argc - 2, argv + 2);
	} else if (strcmp(subcommand, "install") == 0) {
		rc = m4_cli_install_run(argc - 2, argv + 2);
	} else if (strcmp(subcommand, "enable") == 0) {
		rc = m4_cli_enable_run(argc - 2, argv + 2);
	} else if (strcmp(subcommand, "disable") == 0) {
		rc = m4_cli_disable_run(argc - 2, argv + 2);
	} else if (strcmp(subcommand, "uninstall") == 0) {
		rc = m4_cli_uninstall_run(argc - 2, argv + 2);
	} else if (strcmp(subcommand, "auto") == 0 || strcmp(subcommand, "safe") == 0 ||
		   strcmp(subcommand, "control") == 0) {
		rc = cmd_not_implemented(subcommand);
	} else {
		fprintf(stderr, M4_T("momentum4: unknown subcommand: %s\n",
"momentum4: неизвестная подкоманда: %s\n"), subcommand);
		print_usage(stderr);
		rc = 2;
	}

	pw_deinit();
	return rc;
}
