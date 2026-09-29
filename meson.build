# meson.build -- top-level build for momentum4: a single top-level project
# with a thin Makefile wrapper, building common, the daemon, the loader and
# the plugin.
#
# What this file builds directly:
#   - common/        (subprojects/common symlink -> ../common)  m4-conf,
#                    used here only to build ONE gen-conf binary (below)
#   - loader/        loader/meson.build (system clang)
#   - systemd/*.in, wireplumber/*.in  configured + installed
#   - data/          generated momentum4.conf, capi-sha256.txt, sounds
#
# What it deliberately does NOT build (see ../Makefile instead):
#   - the daemon/CLI binary (daemon): meson's subproject()
#     sandboxing (confirmed while building this file, meson 1.12.1)
#     refuses cross-subproject relative-path source grabs -- and
#     daemon/meson.build reaches into ../../common/m4-conf.c by a
#     relative path ON PURPOSE (see that file's own comment), because it
#     must ALSO keep building standalone (`meson setup build .` right in
# daemon/, same as every other workflow here).
#     Turning that into a dependency-injected `m4conf_dep` would work, but
#     it means every standalone build of daemon/ would ALSO need
#     its own subprojects/common symlink and subproject() call, doubling
#     the moving parts for a component that already builds and tests fine
#     on its own. ../Makefile instead runs `meson setup`/`meson
#     install` in daemon/ as an independent, already-working
#     nested build (same pattern it already uses for module/, below) --
#     one thin orchestration layer instead of forcing two different build
#     systems' sandboxing rules to agree.
#   - module/ (the WirePlumber/PipeWire SPA plugin): it lives in its own
#     copy of PipeWire's source tree, produced by
#     module/prepare-tree.sh, and built by PipeWire's OWN
#     meson.build, not this one -- nesting an entire second copy of
#     upstream PipeWire's build as a meson subproject() of THIS project
#     would mean re-solving PipeWire's own dependencies (alsa, v4l2,
#     bluez5, ...) through our option set for no benefit, since we only
#     ever want 9 specific bluez5 targets out of it. `make module` in
#     ../Makefile runs prepare-tree.sh then a nested, independent
#     `meson setup`/`ninja` invocation against that tree, exactly as every
#     test/build script in module/ already does today.
#   - qemu-hexagon itself: `make qemu` / qemu/build-qemu-hexagon.sh is a
#     multi-hundred-MB, GPLv2 upstream source build with its own configure
#     script; wiring it through meson would gain nothing over a shell
#     script and would make `meson setup` slow/networked by default for
#     everyone who does not need a freshly-built qemu (a system
#     qemu-hexagon, or a previous `make qemu` run, both already work).
#

project('momentum4-suite', 'c',
  version : '1.0.0',
  license : 'MIT',
  meson_version : '>= 0.61.1',
  default_options : [
    'c_std=gnu11',
    'warning_level=2',
    'buildtype=debugoptimized',
  ],
)

fs = import('fs')

if get_option('dev_build')
  warning(
    'dev_build=true: the installed momentum4 binary will embed this ' +
    'checkout\'s own absolute paths (' + meson.project_source_root() + ') ' +
    'as a last-resort fallback. Do not use this for a real install -- ' +
    'see the root README.md\'s "Build" section.'
  )
endif

# ---------------------------------------------------------------- common --
#
# subprojects/common is a symlink to ../common (see that directory's own
# meson.build header comment for why it is a separate, independently
# buildable meson project()). A symlink (rather than moving common/ under
# subprojects/, which meson's own convention normally expects) keeps
# `meson setup build-common common && meson test -C build-common`
# working unmodified
# for anyone who only wants to iterate on the config parser.
common_opts = get_option('tests') ? ['tests=true'] : ['tests=false']
common_sub = subproject('common', default_options : common_opts)
m4conf_dep = common_sub.get_variable('m4conf_dep')

# gen-conf: build-time generator for the fully-commented reference
# momentum4.conf (data/momentum4.conf below) and for `momentum4 config
# defaults`'s own in-process copy of the same function
# (m4_conf_write_reference(), common/m4-conf.c) -- built once here from
# common/'s own sources so this top-level file has one canonical binary to
# run as a custom_target(), instead of relying on either subproject's
# private (install:false) copy.
gen_conf_exe = executable('gen-conf',
  [meson.project_source_root() / 'common' / 'gen-conf.c'],
  dependencies : m4conf_dep,
  install : false,
)

# daemon (daemon): NOT built by this file, see the header comment
# above -- ../Makefile's `make daemon`/`make install` drive a nested
# `meson setup`/`meson install` there directly, with the same
# -Dtests=/-Ddev_build= this file's own options ask for.

pw_probe = dependency('libpipewire-0.3', required : false)
pipewire_min = get_option('pipewire_min_version')
if pw_probe.found() and pw_probe.version().version_compare('<' + pipewire_min)
  warning(
    'libpipewire-0.3 on this machine is @0@, older than -Dpipewire_min_version=@1@ -- ' \
      .format(pw_probe.version(), pipewire_min) +
    'the momentum4 binary itself will still build and run (it only uses stable, ' +
    'long-standing libpipewire API), but the aptX Adaptive SPA plugin (`make module`) is ' +
    'pinned to a specific PipeWire release by module/SUPPORTED and may refuse ' +
    'to build or (if forced) may not load correctly -- see README.md "Requirements".'
  )
elif not pw_probe.found()
  warning(
    'libpipewire-0.3 not found via pkg-config in this configuration ' +
    '(only affects this summary/warning -- the daemon and module builds ' +
    'each resolve it independently, see README.md "Build" for the ' +
    'PKG_CONFIG_PATH override some distros need).'
  )
endif

# --------------------------------------------------------------- loader --
subdir('loader')

# ------------------------------------------------------------- systemd --
#
# @bindir@ (absolute path) is the only substitution any of these need --
# wireplumber/99-m4-adaptive-overlay.conf.in also uses it (see below).
subst = configuration_data()
subst.set('bindir', get_option('prefix') / get_option('bindir'))

# Where systemd --user actually looks for *user* unit files (confirmed
# with `systemctl --user show -p UnitPath` on a real session): the search
# path has "lib/systemd/user" entries ONLY under /usr and /usr/local
# (/usr/lib/systemd/user, /usr/local/lib/systemd/user); for any other
# --prefix (in practice: a non-root install such as $HOME/.local) there is
# NO "PREFIX/lib/systemd/user" entry at all -- only
# "PREFIX/share/systemd/user", which is exactly $XDG_DATA_HOME/systemd/user
# for the conventional PREFIX=$HOME/.local (systemd.unit(5)'s own
# documented user-unit install location). Getting this wrong means a
# non-root `meson install`/`make install` places units where systemd
# --user will never look, even though every file/symlink step reports
# success (`momentum4 install`/`doctor` would then need to compensate with
# manual symlinks -- exactly the trap this logic avoids).
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
momentum4_datadir = get_option('prefix') / get_option('datadir') / 'momentum4'

systemd_unit_templates = [
  'momentum4d.service.in',
  'momentum4-helper.socket.in',
  'momentum4-helper@.service.in',
  'momentum4-graph.service.in',
]
foreach tmpl : systemd_unit_templates
  configure_file(
    input : 'systemd' / tmpl,
    output : fs.stem(tmpl),
    configuration : subst,
    install : true,
    install_dir : systemd_user_unit_dir,
  )
endforeach

# momentum4.target has no @-substitutions -- installed byte for byte.
install_data('systemd/momentum4.target', install_dir : systemd_user_unit_dir)

# --------------------------------------------------------- wireplumber --
#
# 99-...: has @bindir@, resolved at build time like the systemd units
# above (see that file's own header for why this one CAN be resolved at
# build time -- it has no per-install parameters).
configure_file(
  input : 'wireplumber/99-m4-adaptive-overlay.conf.in',
  output : '99-m4-adaptive-overlay.conf',
  configuration : subst,
  install : true,
  install_dir : momentum4_datadir / 'wireplumber',
)

# 51-...: installed RAW (still has @M4_ADDRESS_UNDERSCORE@ inside) --
# `momentum4 install` substitutes it at runtime, see that file's own
# header comment for why.
install_data('wireplumber/51-m4-no-suspend.conf.in',
  install_dir : momentum4_datadir / 'wireplumber')

# ---------------------------------------------------------------- data --
data_conf = custom_target('data-momentum4.conf',
  output : 'momentum4.conf',
  command : [gen_conf_exe, '@OUTPUT@'],
  build_by_default : true,
  install : true,
  install_dir : momentum4_datadir,
)

install_data('data/capi-sha256.txt', install_dir : momentum4_datadir)

# data/sounds/*.wav: the earcon set, moved here with `git mv` once the
# live installation's `[earcon] dir` no longer pointed into this checkout
# at all. data/ is now the one real source of truth for the shipped earcon
# set.
foreach wav : ['call-error.wav', 'call-off.wav', 'call-on.wav', 'mic-off.wav', 'mic-on.wav']
  install_data('data' / 'sounds' / wav, install_dir : momentum4_datadir / 'sounds')
endforeach

summary({
  'prefix' : get_option('prefix'),
  'bindir' : get_option('prefix') / get_option('bindir'),
  'datadir (momentum4)' : momentum4_datadir,
  'systemd user units' : systemd_user_unit_dir,
  'dev_build' : get_option('dev_build'),
  'tests' : get_option('tests'),
  'libpipewire found (pkg-config probe)' : pw_probe.found() ? pw_probe.version() : 'no',
}, section : 'momentum4')
