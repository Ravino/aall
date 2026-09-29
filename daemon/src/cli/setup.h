/* cli/setup.h -- `momentum4 setup --capi <path> [--force]` (see
 * docs/user/obtaining-encoder.md's own step 4): validates a
 * user-supplied CAPI v2 aptX Adaptive encoder .so (Hexagon ELF, EM_HEXAGON)
 * against data/capi-sha256.txt's known hashes and copies it into
 * `${XDG_DATA_HOME}/momentum4/capi/` (helper.capi's own default location --
 * see common/m4-conf-keys.def -- so no momentum4.conf write is needed for
 * the common case).
 */
#ifndef MOMENTUM4_CLI_SETUP_H
#define MOMENTUM4_CLI_SETUP_H

#ifdef __cplusplus
extern "C" {
#endif

int m4_cli_setup_run(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif /* MOMENTUM4_CLI_SETUP_H */
