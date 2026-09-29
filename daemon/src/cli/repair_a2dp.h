/* cli/repair_a2dp.h -- `momentum4 repair-a2dp`: an explicit command to
 * repair the A2DP profile. Unlike cli/call.c (which reports an error with a
 * hint when the daemon is unreachable), this command DOES have a
 * direct-graph fallback -- it must work with no daemon running at all,
 * because an emergency-rollback caller may stop momentum4d.service as one
 * of its first steps, before ever reaching the profile-repair step that
 * calls this -- mirrors cli/status.c's daemon-first-then-direct
 * pattern, not cli/call.c's daemon-only one.
 */
#ifndef MOMENTUM4_CLI_REPAIR_A2DP_H
#define MOMENTUM4_CLI_REPAIR_A2DP_H

#ifdef __cplusplus
extern "C" {
#endif

/* `momentum4 repair-a2dp` takes no arguments. Returns a process exit code:
 * 0 success, 2 usage error, 4 repair failed (daemon-reported ERR or direct
 * run's own status != OK). Never exits 3 the way cli/call.c does for
 * "daemon unreachable" -- that case falls back to the direct path instead
 * of failing. */
int m4_cli_repair_a2dp_run(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif /* MOMENTUM4_CLI_REPAIR_A2DP_H */
