/* cli/resume.h -- `momentum4 resume`: removes the $runtime_dir/safe-latch
 * flag any local tool can set (see core/state.h). Deliberately a plain local
 * unlink(), NOT a ctl.sock command -- works even if momentum4d is dead,
 * matching every other rollback-adjacent command in this project
 * (`momentum4 status`'s direct-read fallback among them). The daemon
 * re-checks $runtime_dir/safe-latch's existence
 * fresh (stat(), never cached) before every single graph write, so it
 * notices the latch is gone on its very next decision -- no IPC round trip
 * needed for this to take effect. */
#ifndef MOMENTUM4_CLI_RESUME_H
#define MOMENTUM4_CLI_RESUME_H

#ifdef __cplusplus
extern "C" {
#endif

int m4_cli_resume_run(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif /* MOMENTUM4_CLI_RESUME_H */
