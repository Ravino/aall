/* cli/status.h -- `momentum4 status` / `momentum4 status --json`
 * (strictly read-only). */
#ifndef MOMENTUM4_CLI_STATUS_H
#define MOMENTUM4_CLI_STATUS_H

#ifdef __cplusplus
extern "C" {
#endif

/* argv/argc of the arguments AFTER the "status" subcommand word itself
 * (i.e. for `momentum4 status --json`, argc==1 and argv[0]=="--json"; for
 * plain `momentum4 status`, argc==0).
 * Returns a process exit code (0 = ok, non-zero = something to report, but
 * `status` itself never fails loudly -- a PipeWire/stats read failure is
 * shown as part of the summary, not as a CLI error, so scripts checking the
 * exit code only need to distinguish "usage error" (2) from "ran" (0/1)). */
int m4_cli_status_run(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif /* MOMENTUM4_CLI_STATUS_H */
