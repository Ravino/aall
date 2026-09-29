# common/meson.build -- standalone build for m4-conf. Buildable on its own:
#   meson setup build-common common && meson test -C build-common
#
# This is a SEPARATE meson project() on purpose, not a subdir() of
# daemon's -- see m4-conf.h's own "common/ placement" header comment for
# the two different ways this directory is actually consumed (daemon/
# meson.build references m4-conf.c/gen-conf.c by a relative source path;
# the SPA plugin gets a verbatim copy via prepare-tree.sh). The top-level
# meson.build also pulls this in as a subproject(), but only to build one
# canonical gen-conf binary for data/momentum4.conf -- this file exists so
# common/ has its OWN buildable/testable identity (`meson test` here alone
# exercises the whole parser/registry) independent of daemon ever building
# at all.
project('m4conf', 'c',
  version : '0.1',
  license : 'MIT',
  meson_version : '>= 0.61.1',
  default_options : [
    'c_std=c99',
    'warning_level=2',
    'buildtype=debugoptimized',
  ],
)

cc = meson.get_compiler('c')
add_project_arguments(
  cc.get_supported_arguments(['-Wall', '-Wextra']),
  language : 'c',
)

m4conf_inc = include_directories('.')

m4conf_lib = static_library('m4conf', 'm4-conf.c', 'm4-i18n.c',
  include_directories : m4conf_inc,
)

# Declared for a future top-level meson.build (C5) to consume via
# subdir('common') + this dependency object -- unused by momentum4d today
# (see the file header comment above), kept here so that wiring is a
# one-line change later rather than a new file.
m4conf_dep = declare_dependency(
  link_with : m4conf_lib,
  include_directories : m4conf_inc,
)

executable('gen-conf', 'gen-conf.c',
  link_with : m4conf_lib,
  include_directories : m4conf_inc,
  install : false,
)

if get_option('tests')
  test('m4_conf',
    executable('test_m4_conf', 'tests/test_m4_conf.c', 'm4-conf.c', 'm4-i18n.c',
      include_directories : m4conf_inc,
    ),
  )
endif
