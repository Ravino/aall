/* util/systemd_unit.h -- thin sd-bus wrapper around
 * org.freedesktop.systemd1.Manager (the systemd --user session manager),
 * used ONLY by `momentum4 install|enable|disable|uninstall`: copies or
 * links unit files and enables them entirely over sd-bus, without ever
 * exec'ing systemctl(1).
 *
 * Every call here is a single, synchronous D-Bus method call on the
 * session bus (sd_bus_default_user()) -- no systemctl(1) subprocess is
 * ever spawned. `EnableUnitFiles([absolute paths], ...)` is what actually
 * both LINKS a unit living outside systemd's default search path (e.g.
 * @prefix@/lib/systemd/user when @prefix@ is not /usr) into
 * ~/.config/systemd/user/ AND processes its [Install] section -- this is
 * the same single-call behaviour `systemctl enable /absolute/path/unit`
 * has always had (systemd.unit(5) "If the unit files are located outside
 * of the usual unit search paths, a symlink ... will be created"). No
 * separate LinkUnitFiles() call is therefore needed for units that DO
 * carry an [Install] section (all of momentum4's own units do).
 *
 * All functions here are testable against ANY implementation of
 * org.freedesktop.systemd1.Manager on the bus they connect to -- see
 * tests/systemd_stub.c, a minimal in-process stub server used by
 * tests/test_lifecycle_cli.sh on a private `dbus-run-session` bus, per
 * used for dry-run install tests in an isolated
 * $HOME with a stub user-manager.
 */
#ifndef MOMENTUM4_UTIL_SYSTEMD_UNIT_H
#define MOMENTUM4_UTIL_SYSTEMD_UNIT_H

#include <stdbool.h>
#include <stddef.h>

#include <systemd/sd-bus.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Connects to the user (session) bus. Returns 0 and sets *out on success;
 * on failure returns -1 and leaves a human-readable message in err (if
 * non-NULL) -- e.g. "no session bus" when run outside any systemd --user
 * session (SSH without a lingering session, a container without dbus,
 * ...), which every caller in cli/lifecycle.c treats as a normal,
 *
 * TESTING HAZARD (confirmed the hard way while developing this file):
 * sd_bus_default_user() resolves the bus via
 * $DBUS_SESSION_BUS_ADDRESS FIRST, falling back to $XDG_RUNTIME_DIR/bus
 * only if that is unset. Overriding $HOME/$XDG_RUNTIME_DIR alone (e.g. for
 * an "isolated test $HOME") does NOT isolate this connection if the
 * surrounding shell already has a real $DBUS_SESSION_BUS_ADDRESS in its
 * environment -- it will silently connect to the REAL, live session bus
 * and issue REAL EnableUnitFiles/StartUnit/etc. calls there. Any test of
 * the --yes code path MUST run under `dbus-run-session -- <cmd>` (which
 * sets $DBUS_SESSION_BUS_ADDRESS to a fresh private bus for its child) --
 * see tests/test_lifecycle_cli.sh, and do NOT trust
 * $HOME/$XDG_RUNTIME_DIR overrides alone to make this safe.
 * expected failure mode (print it, exit non-zero), not a crash. */
int m4_systemd_user_connect(sd_bus **out, char *err, size_t err_len);

/* EnableUnitFiles(files, /o runtime=false/, force) -- files are absolute
 * paths (see header comment above for why this both links and enables in
 * one call for units with an [Install] section). Returns 0 on success,
 * -1 with `err` filled in on any D-Bus error. */
int m4_systemd_enable_unit_files(sd_bus *bus, const char *const *paths, size_t n_paths,
				  bool force, char *err, size_t err_len);

/* DisableUnitFiles(names, runtime=false) -- names are unit NAMES (e.g.
 * "momentum4d.service"), not paths: this removes every symlink under
 * ~/.config/systemd/user (both the [Install] WantedBy= links and, per
 * systemd's own unit_file_disable(), the absolute-path alias link
 * EnableUnitFiles created) that resolves to that unit name. */
int m4_systemd_disable_unit_files(sd_bus *bus, const char *const *names, size_t n_names,
				   char *err, size_t err_len);

/* Reload() -- re-reads all unit files (systemctl --user daemon-reload
 * equivalent). Synchronous: does not return until the reload has
 * completed. */
int m4_systemd_daemon_reload(sd_bus *bus, char *err, size_t err_len);

/* StartUnit/StopUnit/RestartUnit(name, mode="replace") -> job object path
 * (discarded -- every caller here treats "the method call returned
 * without a D-Bus error" as success, the same way `systemctl`'s own exit
 * code is normally treated; none of these
 * units are slow/conditional enough to need to wait for job completion). */
int m4_systemd_start_unit(sd_bus *bus, const char *name, char *err, size_t err_len);
int m4_systemd_stop_unit(sd_bus *bus, const char *name, char *err, size_t err_len);
int m4_systemd_restart_unit(sd_bus *bus, const char *name, char *err, size_t err_len);

/* GetUnitFileState(name) -> "enabled"/"disabled"/"static"/"not-found"/...
 * Copies at most out_len-1 bytes into `out` (always NUL-terminated).
 * Returns 0 on success (even if the state is "not-found" -- that is a
 * valid answer, not an error), -1 on a real D-Bus/connection failure. */
int m4_systemd_get_unit_file_state(sd_bus *bus, const char *name, char *out, size_t out_len);

/* ActiveState property (via org.freedesktop.DBus.Properties.Get on the
 * unit object returned by GetUnit) -- "active"/"inactive"/"failed"/... or
 * "unknown" if the unit object does not currently exist (never loaded, or
 * already garbage-collected). Used only for the post-action report in
 * `momentum4 enable`/`disable` -- never gates any decision. */
int m4_systemd_get_active_state(sd_bus *bus, const char *name, char *out, size_t out_len);

/* Pure, read-only (getenv(3) only -- no D-Bus, no filesystem access):
 * true if `dir` (an absolute path, no trailing slash) is one of the
 * well-known STATIC entries of systemd --user's own unit search path
 * (systemd.unit(5) "USER UNIT SEARCH PATH" -- $XDG_CONFIG_HOME/systemd/user,
 * $XDG_CONFIG_DIRS/.../systemd/user, /etc/systemd/user,
 * $XDG_DATA_HOME/systemd/user, $XDG_DATA_DIRS/.../systemd/user,
 * /usr/local/lib/systemd/user, /usr/lib/systemd/user), matched with the
 * same defaulting rules systemd itself uses when the XDG_* variable is
 * unset/empty. Deliberately does NOT include the handful of entries that
 * only exist transiently while a *particular* session is already running
 * ($XDG_RUNTIME_DIR/systemd/{user.control,transient,generator*}) -- those
 * are created/removed by systemd itself, never an installer's target
 * directory, and would make this check depend on session state instead of
 * on the on-disk layout `momentum4 install`/`doctor` actually control.
 * Used by `momentum4 doctor`/`install` to catch exactly the class of bug
 * where units get installed to a syntactically fine but never-searched
 * directory (e.g. PREFIX=~/.local's "lib/systemd/user" -- systemd --user
 * only searches "share/systemd/user" for that prefix, never "lib/..."). */
bool m4_systemd_unit_dir_in_search_path(const char *dir);

#ifdef __cplusplus
}
#endif

#endif /* MOMENTUM4_UTIL_SYSTEMD_UNIT_H */
