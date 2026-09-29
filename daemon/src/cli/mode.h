/* cli/mode.h -- `momentum4 mode observe|manual|auto|get`. Same daemon-only
 * contract as cli/call.h -- no fallback if the daemon is unreachable. */
#ifndef MOMENTUM4_CLI_MODE_H
#define MOMENTUM4_CLI_MODE_H

#ifdef __cplusplus
extern "C" {
#endif

/* argv/argc AFTER the "mode" word. `momentum4 mode` with no argument (or
 * `momentum4 mode get`) prints the current mode instead of changing it.
 * Same exit code convention as cli/call.h. */
int m4_cli_mode_run(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif /* MOMENTUM4_CLI_MODE_H */
