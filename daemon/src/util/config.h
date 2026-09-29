/* util/config.h -- momentum4d's one entry point into common/m4-conf.{c,h}:
 * the daemon is fully rebased on common/m4-conf, and
 * address=auto/adapter=auto find M4 via BlueZ, read-only, over sd-bus.
 * Originally this file only knew the M4 MAC/hci0
 * adapter as compile-time literals with a test-only env override; both are
 * now [device] keys in the shared registry (common/m4-conf-keys.def),
 * defaulting to "auto" -- see m4_config_mac_address()/
 * m4_config_bt_adapter() below for exactly how "auto" is resolved (bt/
 * bt_discover.h, read-only BlueZ D-Bus).
 *
 * m4_config() is the ONE m4_conf_t instance this whole binary (daemon AND
 * every CLI subcommand) reads from -- loaded lazily on first use (m4_conf_
 * load_layers(), defaults < @datadir@ < /etc < $XDG_CONFIG_HOME/$HOME) and
 * cached for the rest of the process; m4_config_reload() (`momentum4
 * reload`) forces the next m4_config() call to load again. Backward
 * compatibility (the old flat
 * momentum4.conf is still read, with a warning): if the user's momentum4.conf
 * has no `[section]` header at all (the old pre-[section] flat format), m4_config()
 * applies it through cli/config.c's legacy_flat_load() (declared here as
 * m4_config_apply_legacy_flat_momentum4_conf() so this file does not need
 * to depend on cli/ otherwise) instead of feeding it to m4_conf_load_file()
 * (which would reject every one of its lines as "key before any [section]").
 */
#ifndef MOMENTUM4_UTIL_CONFIG_H
#define MOMENTUM4_UTIL_CONFIG_H

#include <stddef.h>

#include "../../../common/m4-conf.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The effective, process-wide config (loaded on first call, cached after
 * that -- see m4_config_reload() to force a fresh load). Never NULL, never
 * fails: an unreadable/malformed file layer just appends warnings (m4_conf_
 * t's own contract) and this function still returns a fully valid,
 * defaults-backed m4_conf_t*. */
const m4_conf_t *m4_config(void);

/* Forces the NEXT m4_config() call to reload every layer from scratch
 * . Does not touch the m4_conf_t
 * returned by any EARLIER m4_config() call (callers that keep a config
 * struct across a reload, e.g. the running daemon, must re-fetch specific
 * values they care about themselves -- same "reload only takes effect for
 * what re-reads it" contract common/m4-conf.h documents for every section). */
void m4_config_reload(void);

/* Returns the MAC address to treat as "the M4": $MOMENTUM4_MAC if set and
 * non-empty (a test-only override, not documented for real use), else the
 * effective `[device] address` config value if it is
 * not the literal string "auto", else the result of one BlueZ auto-
 * discovery (bt/bt_discover.h, cached for the rest of the process -- BlueZ
 * is not re-queried on every call). If discovery also fails (BlueZ
 * unreachable, or no paired device matches `[device] name-match`), returns
 * "" (never NULL) and the failure is logged once (WARN) -- callers that
 * need an M4 MAC to do anything useful (daemon startup, `momentum4
 * repair-a2dp`) already handle an empty/absent M4 gracefully (m4_present
 * simply stays false), so this deliberately does not fall back to any
 * hardcoded address -- this function exists specifically so no device
 * MAC is ever compiled in. */
const char *m4_config_mac_address(void);

/* Returns the BlueZ adapter name (for org.bluez.Device1 object paths,
 * "/org/bluez/<adapter>/dev_..."): $MOMENTUM4_BT_ADAPTER if set and
 * non-empty (test-only override), else the effective `[device] adapter`
 * config value if not "auto", else the adapter of the SAME auto-discovered
 * device m4_config_mac_address() found (one shared discovery result, not a
 * second independent query) -- else "hci0" (the one placeholder default
 * this file keeps, since an adapter name is meaningless empty and every
 * existing caller already treats "hci0" as its own local fallback too). Never
 * returns NULL. */
const char *m4_config_bt_adapter(void);

/* Returns the momentum4 runtime directory: $MOMENTUM4_RUNTIME_DIR if set and
 * non-empty (a test-only override), else the effective `[paths]
 * runtime-dir` config value (already ${XDG_RUNTIME_DIR}-expanded by
 * common/m4-conf.c at load time). NULL if that expands to "" (no
 * $XDG_RUNTIME_DIR and no override -- same failure shape as
 * m4_config_stats_path()). Writes into `buf` (size `buf_cap`), returns
 * `buf` on success or NULL on failure. Does NOT create the directory --
 * callers that write files under it (core/state.c, ipc/ctl.c) are
 * responsible for mkdir(..., 0700). */
const char *m4_config_runtime_dir(char *buf, size_t buf_cap);

/* "$(m4_config_runtime_dir())/ctl.sock". NULL under the same conditions as
 * m4_config_runtime_dir(). */
const char *m4_config_ctl_socket_path(char *buf, size_t buf_cap);

/* Returns the momentum4 STATE directory (survives a reboot, unlike
 * m4_config_runtime_dir() -- `[call] mic-on-enter = remember`'s own second
 * persistence location.9): the effective `[paths]
 * state-dir` config value (already ${XDG_STATE_HOME}-expanded by
 * common/m4-conf.c at load time). NULL if that expands to "" (no
 * $XDG_STATE_HOME/$HOME). Writes into `buf` (size `buf_cap`), returns `buf`
 * on success or NULL on failure. Does NOT create the directory -- same
 * contract as m4_config_runtime_dir(). */
const char *m4_config_state_dir(char *buf, size_t buf_cap);

/* Path to the helper's stats.json: the effective `[stats] path` config
 * value ($M4_ADAPTIVE_STATS_PATH still honoured -- same override the
 * plugin's own writer reads, kept so a non-default path set for one side is
 * automatically also where `momentum4 status`/`stats` look), or NULL if
 * that expands to "" (no $XDG_RUNTIME_DIR and no override). Writes into
 * `buf` (size `buf_cap`), returns `buf` on success or NULL on failure. */
const char *m4_config_stats_path(char *buf, size_t buf_cap);

/* Path of the user config file momentum4.conf: $MOMENTUM4_CONFIG if set and
 * non-empty, else "$XDG_CONFIG_HOME/momentum4/momentum4.conf", else
 * "$HOME/.config/momentum4/momentum4.conf". NULL if none of those variables
 * is set or `buf` is too small. The file itself is optional -- this is the
 * SAME precedence common/m4-conf.c's own layer 4 uses internally; exposed
 * here too because `momentum4 config migrate`/`reload` need the exact path,
 * not just its resolved contents. */
const char *m4_config_file_path(char *buf, size_t buf_cap);

#ifdef __cplusplus
}
#endif

#endif /* MOMENTUM4_UTIL_CONFIG_H */
