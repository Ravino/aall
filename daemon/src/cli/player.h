/* cli/player.h -- `momentum4 player [status stopped|playing|paused|follow]`
 * and `momentum4 reload`, both via ctl.sock. */
#ifndef MOMENTUM4_CLI_PLAYER_H
#define MOMENTUM4_CLI_PLAYER_H

#ifdef __cplusplus
extern "C" {
#endif

int m4_cli_player_run(int argc, char **argv);
int m4_cli_reload_run(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif /* MOMENTUM4_CLI_PLAYER_H */
