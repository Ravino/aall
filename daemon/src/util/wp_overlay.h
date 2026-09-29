/* util/wp_overlay.h -- installs/removes the two WirePlumber drop-ins this
 * project ships :
 *
 *   ~/.config/systemd/user/wireplumber.service.d/99-m4-adaptive-overlay.conf
 *     (fixed name, version-guarded ExecStartPre=
 *     `momentum4 wp-env`, see wireplumber/99-m4-adaptive-overlay.conf.in)
 *   ~/.config/wireplumber/wireplumber.conf.d/51-m4-no-suspend.conf
 *     (session.suspend-timeout-seconds=0 for exactly
 *     one device's sink node, MAC substituted from `[device] address`)
 *
 * Both are byte-for-byte copies of files installed under
 * `[paths]`-derived @datadir@/momentum4/wireplumber/ by ../../meson.build
 * (see that directory's own .in templates for exactly what is/is not
 * substituted at build time vs. here). Every function is idempotent
 * (install over an existing identical file is a silent no-op; remove of an
 * already-absent file is not an error) and NEVER touches anything other
 * than these two exact paths -- no directory is ever recursively removed.
 */
#ifndef MOMENTUM4_UTIL_WP_OVERLAY_H
#define MOMENTUM4_UTIL_WP_OVERLAY_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Fills `out` with the fixed path this project pins
 * ("$HOME/.config/systemd/user/wireplumber.service.d/99-m4-adaptive-overlay.conf"),
 * NULL on missing $HOME. */
const char *m4_wp_overlay_path(char *out, size_t out_len);

/* Fills `out` with "$HOME/.config/wireplumber/wireplumber.conf.d/51-m4-no-suspend.conf". */
const char *m4_no_suspend_path(char *out, size_t out_len);

/* Copies @datadir@/momentum4/wireplumber/99-m4-adaptive-overlay.conf to
 * m4_wp_overlay_path(), creating parent directories as needed (0755).
 * Returns 0 on success, -1 with a message in err (if non-NULL). */
int m4_wp_overlay_install(char *err, size_t err_len);

/* unlink()s m4_wp_overlay_path() if present. Returns 0 whether or not the
 * file existed; -1 only on a real removal error (permissions, ...). */
int m4_wp_overlay_remove(char *err, size_t err_len);

/* Copies @datadir@/momentum4/wireplumber/51-m4-no-suspend.conf.in to
 * m4_no_suspend_path(), substituting every "@M4_ADDRESS_UNDERSCORE@" with
 * `mac_address` (colons replaced with underscores -- see that template's
 * own header comment for the exact convention). */
int m4_no_suspend_install(const char *mac_address, char *err, size_t err_len);

int m4_no_suspend_remove(char *err, size_t err_len);

#ifdef __cplusplus
}
#endif

#endif /* MOMENTUM4_UTIL_WP_OVERLAY_H */
