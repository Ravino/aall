/* cli/helper_exec.h -- `momentum4 helper-exec`: the ExecStart= target
 * for the helper's socket-activated
 * user service. Reads `[helper]` from momentum4.conf, validates
 * qemu-hexagon/hx_adaptive_loader/CAPI are in place, applies RT priority/
 * nice/CPU affinity and the APTX_AD_* env overrides, then execve()s
 * qemu-hexagon with fd 0/1 untouched (the one AF_UNIX connection systemd
 * already accepted for this instance) -- see helper_exec.c's own top
 * comment for the exact 1:1 mapping from the old wrapper's 3 numbered
 * steps.
 */
#ifndef MOMENTUM4_CLI_HELPER_EXEC_H
#define MOMENTUM4_CLI_HELPER_EXEC_H

#ifdef __cplusplus
extern "C" {
#endif

/* Never returns on success (execv() replaces this process image, matching
 * the old wrapper's `exec`). Returns a process exit code only on failure
 * (127, same as the old wrapper, for "a configured path is missing/not
 * usable" or "execv() itself failed") -- main.c's caller just returns
 * whatever this gives back, unreached in the success path. */
int m4_cli_helper_exec_run(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif /* MOMENTUM4_CLI_HELPER_EXEC_H */
