/* cli/doctor.h -- `momentum4 doctor`: read-only sanity checks, never
 * modifies anything. Checks: PipeWire version, CAPI sha256 against
 * data/capi-sha256.txt, qemu/loader present, units/overlay present, no
 * leftover dev-checkout path references in a live install's units/config
 * (dev builds only, see M4_DEV_CHECKOUT_ROOT). Deliberately does not
 * check speech-dispatcher health -- that is a screen-reader concern
 * outside this project's scope.
 */
#ifndef MOMENTUM4_CLI_DOCTOR_H
#define MOMENTUM4_CLI_DOCTOR_H

#ifdef __cplusplus
extern "C" {
#endif

/* Exit code: 0 -- every check passed. 1 -- at least one WARN (informational,
 * not necessarily broken: e.g. CAPI not configured yet, module not
 * installed). Never returns anything else -- this command cannot fail in a
 * way that should abort a script pipeline harder than "found problems". */
int m4_cli_doctor_run(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif /* MOMENTUM4_CLI_DOCTOR_H */
