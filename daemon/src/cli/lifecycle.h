/* cli/lifecycle.h -- `momentum4 install|enable|disable|uninstall`
 * . See lifecycle.c's top comment for the
 * exact split of responsibilities between the four subcommands.
 */
#ifndef MOMENTUM4_CLI_LIFECYCLE_H
#define MOMENTUM4_CLI_LIFECYCLE_H

#ifdef __cplusplus
extern "C" {
#endif

int m4_cli_install_run(int argc, char **argv);
int m4_cli_enable_run(int argc, char **argv);
int m4_cli_disable_run(int argc, char **argv);
int m4_cli_uninstall_run(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif /* MOMENTUM4_CLI_LIFECYCLE_H */
