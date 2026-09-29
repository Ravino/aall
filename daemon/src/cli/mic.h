/* cli/mic.h -- `momentum4 mic on|off|toggle|status` ("tap = mic" task).
 * Same shape and same daemon-only
 * contract as cli/call.h (talks to ctl.sock ONLY, no direct-graph-write
 * fallback -- mode gating (observe/manual/auto) and safe-latch must apply
 * exactly as they do for `call on/off`, and only the daemon has the cached
 * mic Route state (core/graph_ops.h's have_mic_route/mic_mute/mic_volume)
 * to act on in the first place).
 */
#ifndef MOMENTUM4_CLI_MIC_H
#define MOMENTUM4_CLI_MIC_H

#ifdef __cplusplus
extern "C" {
#endif

/* argv/argc AFTER the "mic" word itself, e.g. for `momentum4 mic on`,
 * argc==1, argv[0]=="on". Returns a process exit code: 0 success, 2 usage
 * error, 3 daemon unreachable, 4 daemon returned ERR (no mic route right
 * now -- not in a call -- lands here too, same as any other ERR). */
int m4_cli_mic_run(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif /* MOMENTUM4_CLI_MIC_H */
