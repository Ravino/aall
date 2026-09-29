project('momentum4', 'c',
  version : '1.0.0',
  license : 'MIT',
  meson_version : '>= 0.61.1',
  default_options : [
    'c_std=gnu11',
    'warning_level=2',
    'buildtype=debugoptimized',
  ],
)

cc = meson.get_compiler('c')

add_project_arguments(
  cc.get_supported_arguments(['-Wall', '-Wextra']),
  language : 'c',
)

# `momentum4 version` prints meson.project_version() -- this is the ONE
# place the version string is defined; main.c has no separate literal.
add_project_arguments(
  '-DMOMENTUM4_VERSION="' + meson.project_version() + '"',
  language : 'c',
)

# common/m4-conf.c's compiled-in fallback defaults for `@prefix@`/`@libdir@`/
# `@libexecdir@`/`@datadir@` substitutions :
# without these, m4-conf.c falls back to its own hardcoded "/usr/local"
# family of constants (see that file's M4_CONF_PREFIX #ifndef guard),
# which is WRONG whenever this binary is actually installed with a
# different -Dprefix= or (as on this machine, and any other Debian/Ubuntu
# multiarch system) a --libdir that is NOT plain "PREFIX/lib" -- e.g.
# get_option('libdir') here is "lib/x86_64-linux-gnu", not "lib", so
# `[helper] loader`'s compiled default must say
# "PREFIX/lib/x86_64-linux-gnu/momentum4/hexagon/..." to match where
# ../meson.build's `meson install` (or a distro package) actually put
# loader's hx_adaptive_loader -- verified by installing both
# builds into the same DESTDIR and comparing paths byte for byte. The SPA
# plugin side of this exact same problem was already solved the same way
# in patches/0011 (M4_CONF_{PREFIX,LIBDIR..}
# computed from PipeWire's own already-multiarch-aware
# prefix/pipewire_libdir/pipewire_libexecdir/pipewire_datadir) -- this is
# the daemon-side equivalent of that same fix.
add_project_arguments(
  '-DM4_CONF_PREFIX="' + get_option('prefix') + '"',
  '-DM4_CONF_LIBDIR="' + get_option('prefix') / get_option('libdir') + '"',
  '-DM4_CONF_LIBEXECDIR="' + get_option('prefix') / get_option('libexecdir') + '"',
  '-DM4_CONF_DATADIR="' + get_option('prefix') / get_option('datadir') + '"',
  language : 'c',
)

# [paths] systemd-user-unit-dir's build-time default: mirrors the SAME
# prefix-dependent logic as ../meson.build's own systemd_user_unit_dir
# (duplicated, not shared, for the same cross-subproject sandboxing reason
# documented at the top of this file for M4_CONF_PREFIX/LIBDIR/...) --
# systemd --user's real UnitPath (confirmed with `systemctl --user show -p
# UnitPath`) only has a "lib/systemd/user" entry under /usr and
# /usr/local; any other --prefix (a non-root $HOME/.local install, in
# particular) is searched under "PREFIX/share/systemd/user" instead
# (= $XDG_DATA_HOME/systemd/user for the conventional $HOME/.local
# prefix), never "PREFIX/lib/systemd/user". Without this, `momentum4
# install` would link/enable units at a path systemd --user never scans.
systemd_pc = dependency('systemd', required : false)
if get_option('prefix') == '/usr'
  systemd_user_unit_dir = systemd_pc.found() \
    ? systemd_pc.get_variable(pkgconfig : 'systemduserunitdir', default_value : '/usr/lib/systemd/user') \
    : '/usr/lib/systemd/user'
elif get_option('prefix') == '/usr/local'
  systemd_user_unit_dir = '/usr/local/lib/systemd/user'
else
  systemd_user_unit_dir = get_option('prefix') / 'share' / 'systemd' / 'user'
endif
add_project_arguments(
  '-DM4_CONF_SYSTEMD_USER_UNIT_DIR="' + systemd_user_unit_dir + '"',
  language : 'c',
)

# Pure-logic sources (automode.c/callfsm.c/profile_rules.c, plus the
# test-only callfsm_fake.c) live in their own meson.build, separate from
# pw/* below, so the two source lists can be edited independently -- see
# src/core/meson.build.
subdir('src/core')

# libpipewire-0.3 pulls in libspa-0.2 (Requires: in its .pc). Both resolve
# via the hand-written overlay in work/tools/root/pkgconfig-overlay when
# PKG_CONFIG_PATH points there (see env.sh.example in this directory) --
# neither -dev package is installed system-wide on the build machine, only
# the runtime .so (same "apt-get download + dpkg -x into work/, nothing
# installed into the system" pattern as module).
pipewire_dep = dependency('libpipewire-0.3', version : '>=1.4.2')
systemd_dep = dependency('libsystemd')
# spa/utils/json.h is header-only (static inline) -- used directly by
# metrics/stats.c and by tests/test_stats.c, without needing to link
# libpipewire at all for the latter.
spa_dep = dependency('libspa-0.2')

# [paths] spa-system-dir's build-time default : the
# key's comment in common/m4-conf-keys.def has always documented this as
# "determined at build time from `pkg-config --variable=plugindir
# libspa-0.2`" but nothing actually did that -- the key's default_value was
# a plain "" literal, so `momentum4 wp-env` (cli/wp_env.c) never had a
# system plugin directory to fall back on and silently never enabled
# SPA_PLUGIN_DIR on any clean install (verified independently).
# spa_dep (above) is a required dependency already, so this
# pkg-config variable lookup only fails to produce a value if this
# particular libspa-0.2.pc does not export "plugindir" at all (some
# hand-rolled .pc overlays might not) -- warn, do not hard error, and let
# M4_CONF_SPA_SYSTEM_DIR fall back to m4-conf.c's own "" default (which
# cli/wp_env.c already treats as a safe, do-nothing disable, not a guess).
spa_plugindir = spa_dep.get_variable(pkgconfig : 'plugindir', default_value : '')
if spa_plugindir == ''
  warning(
    'libspa-0.2.pc does not export a "plugindir" pkg-config variable -- ' +
    '[paths] spa-system-dir will default to empty, and `momentum4 wp-env` ' +
    'will refuse to enable SPA_PLUGIN_DIR (safe, but aptX Adaptive will ' +
    'not load into WirePlumber) until spa-system-dir is set by hand in ' +
    'momentum4.conf -- see docs/user/TROUBLESHOOTING.md.'
  )
endif
add_project_arguments(
  '-DM4_CONF_SPA_SYSTEM_DIR="' + spa_plugindir + '"',
  language : 'c',
)

# pw_sources (graph_pw.c/loop_glue.c/micdetect.c/callfsm_graph_adapter.c)
# lives in its own meson.build for the same reason src/core does (see
# src/core/meson.build's comment): disjoint file lists that can be edited
# independently without touching this file's momentum4_sources.
subdir('src/pw')

# ipc/ctl.c ($runtime_dir/ctl.sock) and daemon/daemon.c (the daemon core)
# -- own meson.build files for the same reason as src/core and src/pw
# above.
subdir('src/ipc')
subdir('src/daemon')

# src/bt/bt_ops_sdbus.c -- the one BlueZ D-Bus call this project makes
# directly (org.bluez.Device1.ConnectProfile, "repair-a2dp" command).
# libsystemd (already a dependency for sd-event) also provides sd-bus, so
# no new dependency is needed.
subdir('src/bt')

# src/control/ -- M4 gestures: our MPRIS player in BlueZ (system bus),
# passthrough to the session's MPRIS player, gesture->action map
# . sd-bus only, no new dependency.
subdir('src/control')

# util/earcon.c falls back to the repo's own sound files when nothing is
# installed -- ONLY in a dev build (M4_SOURCE_SOUNDS_DIR is set only for a
# dev build). A release build
# (-Ddev_build=false, the eventual packaging default) never bakes
# this checkout's own absolute path into the installed binary at all --
# earcon.c's search list already ends with `${XDG_DATA_HOME}/momentum4/sounds`
# / `/usr/share/momentum4/sounds` / `[earcon] dir` from momentum4.conf, any
# of which is sufficient once `data/sounds` is actually installed;
# this repo-relative fallback exists purely so `ninja test`/running
# momentum4d straight out of this checkout (dev_build's default, matches
# every other build option's current default in this tree) has earcons
# without an install step. Points at data/sounds/ (see meson.build's own
# comment on the `data/sounds/*.wav` install_data() calls).
if get_option('dev_build')
  add_project_arguments(
    '-DM4_SOURCE_SOUNDS_DIR="' + meson.project_source_root() / '..' / 'data' / 'sounds' + '"',
    language : 'c',
  )
  # Same reasoning, for `momentum4 setup --capi`'s data/capi-sha256.txt
  # lookup (cli/setup.c) -- a release build relies on @datadir@/momentum4/
  # capi-sha256.txt (installed alongside the rest of `data/`) instead.
  add_project_arguments(
    '-DM4_SOURCE_DATA_DIR="' + meson.project_source_root() / '..' / 'data' + '"',
    language : 'c',
  )
  # `momentum4 doctor`'s "checkout path leaked into an installed unit/
  # config" check (cli/doctor.c) needs to know what that checkout path
  # actually is; only meaningful in a dev build, where units/configs are
  # sometimes hand-written to point straight at this source tree instead
  # of an install prefix. A release build never bakes this in, so the
  # check is simply skipped there (nothing installed by a release build
  # can ever reference a source checkout in the first place).
  add_project_arguments(
    '-DM4_DEV_CHECKOUT_ROOT="' + meson.project_source_root() / '..' + '"',
    language : 'c',
  )
endif

# common/ : m4-conf.c is referenced by a
# relative source path, NOT a subdir()/subproject() -- see m4-conf.h's own
# "common/ placement" header comment for why (momentum4d has no top-level
# meson.build of its own to subdir() common/ from yet).
# gen-conf.c is its own standalone executable() below (not part of
# momentum4_sources -- it is a build-time tool, has its own main()).
common_dir = meson.project_source_root() / '..' / 'common'
common_inc = include_directories('../common')

momentum4_sources = [
  'src/main.c',
  'src/metrics/stats.c',
  'src/util/config.c',
  'src/util/log.c',
  'src/cli/status.c',
  'src/cli/call.c',
  'src/cli/mic.c',
  'src/cli/mode.c',
  'src/cli/resume.c',
  'src/cli/daemon_cmd.c',
  'src/cli/repair_a2dp.c',
  'src/cli/player.c',
  'src/cli/config.c',
  'src/cli/stats.c',
  'src/cli/doctor.c',
  'src/cli/log.c',
  'src/cli/setup.c',
  'src/cli/helper_exec.c',
  'src/cli/graph_host.c',
  'src/cli/wp_env.c',
  'src/graph/graph_host_build.c',
  'src/util/build_info.c',
  'src/util/settings.c',
  'src/util/earcon.c',
  'src/util/legacy_conf.c',
  'src/util/sha256.c',
  'src/util/capi_check.c',
  'src/util/rt_status.c',
  'src/util/systemd_unit.c',
  'src/util/wp_overlay.c',
  'src/util/conf_upsert.c',
  'src/cli/lifecycle.c',
  common_dir / 'm4-conf.c',
  common_dir / 'm4-i18n.c',
] + core_sources + pw_sources + ipc_sources + daemon_sources + bt_sources + control_sources

# NOTE (found while wiring up this build, not its own change in
# intent -- flagged, not redesigned): core/callfsm.c/automode.c #include
# "core/callfsm.h" etc. (absolute-from-src style, unlike pw/*.c's own
# "../core/..." relative style) but nothing set include_directories('src')
# for THIS executable() target yet (tests/meson.build's targets already do,
# per-target -- that's why `meson test` worked before this was added but a
# full `ninja` of the momentum4 binary itself did not). One line, added
# here so the whole tree builds; not a design decision.
momentum4_inc = include_directories('src')

# install: true (top-level meson.build/Makefile) -- when this
# meson.build is entered as a subproject() of the top-level build (see
# ../meson.build), `meson install` must actually place the binary at
# @bindir@/momentum4; standalone dev builds (`meson setup build .` right
# here) install it too now, which is harmless (nobody runs `meson install`
# in this directory by habit -- every documented workflow in this repo so
# far used `ninja`/`meson test` in build/, never `meson install`) and keeps
# this the single source of truth for the target instead of a second
# `executable()` duplicate in the top-level file.
momentum4_exe = executable('momentum4',
  momentum4_sources,
  include_directories : [momentum4_inc, common_inc],
  dependencies : [pipewire_dep, systemd_dep],
  install : true,
)

# common/gen-conf.c: build-time generator for the fully-commented
# reference momentum4.conf (data/momentum4.conf in the eventual packaging
# layout) -- not installed by momentum4d itself (that belongs to whatever
# packages `data/`), just built here so `ninja gen-conf` and
# `common/tests` both have a known-good binary during this phase.
executable('gen-conf',
  [common_dir / 'gen-conf.c', common_dir / 'm4-conf.c', common_dir / 'm4-i18n.c'],
  include_directories : common_inc,
  install : false,
)

if get_option('tests')
  subdir('tests')
endif
