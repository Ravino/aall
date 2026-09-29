/* cli/log.h -- `momentum4 log [-n N] [--follow]`: last N sd-journal
 * entries for this project's own processes (SYSLOG_IDENTIFIER/_COMM
 * momentum4/momentum4d -- util/log.c's own m4_log()). Read-only.
 */
#ifndef MOMENTUM4_CLI_LOG_H
#define MOMENTUM4_CLI_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

int m4_cli_log_run(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif /* MOMENTUM4_CLI_LOG_H */
