/* cli/call.h -- `momentum4 call on|off|status`.
 * Talks to the daemon over ctl.sock ONLY -- unlike `momentum4 status`
 * (cli/status.c), there is no direct-graph-write fallback here: if the
 * daemon is not reachable, this fails with an error and a hint to start
 * it. This is deliberate, not a missing feature -- the daemon is
 * phased in gradually (observe -> manual -> auto), and letting the CLI
 * silently bypass the daemon's mode gating by writing to the graph
 * itself would defeat that. */
#ifndef MOMENTUM4_CLI_CALL_H
#define MOMENTUM4_CLI_CALL_H

#ifdef __cplusplus
extern "C" {
#endif

/* argv/argc AFTER the "call" word itself, e.g. for `momentum4 call on`,
 * argc==1, argv[0]=="on". Returns a process exit code: 0 success, 2 usage
 * error, 3 daemon unreachable, 4 daemon returned ERR. */
int m4_cli_call_run(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif /* MOMENTUM4_CLI_CALL_H */
