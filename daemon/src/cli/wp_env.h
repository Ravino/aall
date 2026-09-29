/* cli/wp_env.h -- `momentum4 wp-env`: writes the systemd `EnvironmentFile=` that decides
 * whether WirePlumber loads OUR bluez5 SPA modules (aptX Adaptive) or the
 * STOCK ones already shipped by the distro.
 *
 * Called as `ExecStartPre=-@bindir@/momentum4 wp-env` from the WirePlumber
 * overlay drop-in ( C6/C5 template -- this file only defines
 * the CONTRACT that template relies on, see below), immediately followed by
 * `EnvironmentFile=-%t/momentum4/wp.env`. Both lines carry a leading `-`:
 * systemd tolerates ExecStartPre failing and EnvironmentFile= not existing
 * or being empty -- WirePlumber ALWAYS starts, with or without our
 * override. This command itself additionally never exits non-zero for a
 * "just disable" outcome (see m4_cli_wp_env_run()) -- the leading `-` is a
 * second, redundant safety net: audio continuity always wins over enabling
 * our own modules.
 *
 * Decision (pure logic in m4_wp_env_decide(), see below; this file's I/O
 * wrapper is m4_cli_wp_env_run()): our modules are enabled ONLY if ALL of
 * -- BUILD-INFO exists and could be read (spa/plugins/bluez5/BUILD-INFO,
 *    next to our .so's, written by prepare-tree.sh);
 * -- its `pipewire_version` field matches (byte-for-byte) `pw_get_library_
 *    version()` of the libpipewire THIS binary is linked against (same
 *    library WirePlumber itself will load, both being the one system
 *    libpipewire-0.3-0.so -- an exact string match, not just major.minor,
 * so it matches the module's own BUILD-INFO);
 * -- `libspa-codec-bluez5-aptx.so` is present and readable next to it;
 * -- `[paths] spa-system-dir` resolves to a non-empty, existing directory
 *    (CRITICAL: SPA_PLUGIN_DIR REPLACES PipeWire's whole plugin search path
 *    when set, not just the bluez5 subtree -- an empty/wrong system dir
 *    here would silently break EVERY SPA plugin, not just ours, i.e. ALL
 *    audio, the one outcome this whole project exists to prevent; wp-env
 *    refuses to enable rather than risk that).
 * Any other outcome writes an EMPTY env file (0 bytes) -- WirePlumber then
 * starts with its own default plugin search path (`[paths] spa-system-dir`
 * from `spa-0.2`'s own pkg-config plugindir, i.e. exactly what it would use
 * with no override at all) and M4 falls back to whatever codec the distro's
 * stock bluez5 SPA module negotiates (aptX HD or better, never silence).
 */
#ifndef MOMENTUM4_CLI_WP_ENV_H
#define MOMENTUM4_CLI_WP_ENV_H

#include <stdbool.h>

#include "../util/build_info.h"

#ifdef __cplusplus
extern "C" {
#endif

enum m4_wp_env_decision {
	M4_WP_ENV_ENABLE = 0,
	M4_WP_ENV_DISABLE_NO_BUILD_INFO,
	M4_WP_ENV_DISABLE_VERSION_MISMATCH,
	M4_WP_ENV_DISABLE_SO_MISSING,
	M4_WP_ENV_DISABLE_NO_SYSTEM_DIR,
};

/* Human-readable (Russian, matches this whole project's log language)
 * one-line reason for a decision -- shared by `momentum4 wp-env`'s own log
 * line and `momentum4 doctor`'s check_pipewire_version(), so the two never
 * drift apart (both call this, not their own copy of the wording). */
const char *m4_wp_env_decision_str(enum m4_wp_env_decision d);

/* Pure decision function -- no file I/O, no PipeWire connection (this is
 * exactly why it is pulled out of m4_cli_wp_env_run(): ExecStartPre= runs
 * before WirePlumber/PipeWire itself is necessarily up, and must not depend
 * on either). `build_info_loaded` is m4_build_info_load()'s own return
 * value (false = BUILD-INFO missing/unreadable, distinct from "loaded but a
 * field was empty", which m4_build_info_load() already represents as ""
 * inside `bi` and which compares unequal to any real installed_pw_version,
 * so it naturally falls through to VERSION_MISMATCH -- no separate case
 * needed). `installed_pw_version` is normally `pw_get_library_version()`
 * (a NULL/empty value can never match a non-empty BUILD-INFO field, so it
 * naturally disables too). */
enum m4_wp_env_decision m4_wp_env_decide(const struct m4_build_info *bi, bool build_info_loaded,
					  const char *installed_pw_version, bool so_present,
					  bool spa_system_dir_present);

/* `momentum4 wp-env` itself. Always returns 0 (see this file's own top
 * comment: never blocks WirePlumber startup) UNLESS argv has stray
 * arguments (usage error, 1) -- a decision of "disable" is NOT a failure,
 * it successfully wrote an empty env file, which is exactly what was
 * asked. Writes to stdout/journal (via util/log.h) which outcome it picked
 * and why. */
int m4_cli_wp_env_run(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif /* MOMENTUM4_CLI_WP_ENV_H */
