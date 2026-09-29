/* cli/daemon_cmd.h -- `momentum4 daemon` / argv[0]==momentum4d entrypoint:
 * wires daemon/daemon.c against the REAL PipeWire
 * (m4_graph_ops_pw, live micdetect), a real ctl.sock, and systemd
 * sd_notify/watchdog. This is the ONLY place in the whole `momentum4`
 * binary that constructs a daemon with cfg->enable_ctl/enable_micdetect/
 * enable_tick_timer all true and cfg->graph_ops == &m4_graph_ops_pw --
 * every test in tests/ constructs its own struct m4_daemon_config with a
 * fake graph_ops and those three flags false instead (daemon/daemon.h's
 * top comment). */
#ifndef MOMENTUM4_CLI_DAEMON_CMD_H
#define MOMENTUM4_CLI_DAEMON_CMD_H

#ifdef __cplusplus
extern "C" {
#endif

/* argv/argc AFTER the "daemon" word (currently unused -- no daemon
 * subcommand-of-a-subcommand exists yet, e.g. no `momentum4 daemon
 * --foreground` flag to parse; kept for signature symmetry with the rest
 * of the other cli/ subcommand entry points, so adding one later does not change this function's
 * shape). Never returns under normal operation (m4_daemon_run() only
 * returns on a genuine sd-event failure or the process being killed, which
 * naturally ends the process instead of returning here). */
int m4_cli_daemon_run(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif /* MOMENTUM4_CLI_DAEMON_CMD_H */
