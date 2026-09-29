/* cli/config.h -- `momentum4 config show|validate|migrate|defaults`
 * . See config.c for each subcommand.
 */
#ifndef MOMENTUM4_CLI_CONFIG_H
#define MOMENTUM4_CLI_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

int m4_cli_config_run(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif /* MOMENTUM4_CLI_CONFIG_H */
