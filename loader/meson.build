# loader/meson.build -- builds the Hexagon guest loader with a system
# clang (clang --target=hexagon): probes clang-14..19 for real Hexagon
# target support and fails with a clear error if none is found.
#
# This is a subdir() of the top-level project (../meson.build) -- no
# project() call here. Only the freestanding Hexagon guest binary
# (hx_adaptive_loader) is built by this file: the x86_64 host-side
# measurement/benchmark tools (hx_bench, hx_capture, pcmgen,
# gen_fc_signals, gen_quiet_sine_hq) are internal development tools, not
# part of the shipped product, and are built independently of this file.

hx_srcs = files(
  'hexagon/start.c',
  'hexagon/main.c',
  'hexagon/elfload.c',
  'hexagon/runtime.c',
  'hexagon/ota.c',
  'hexagon/hx_string.c',
  'hexagon/softfloat.c',
)

# --- 1. find a clang with Hexagon target support ---
#
# Debian/Ubuntu ship versioned clang binaries (clang-14 .. clang-19 at the
# time of writing) side by side with an unversioned "clang" alternative
# that may or may not point at one of them. We probe newest-first (a newer
# clang is still expected to support hexagon-unknown-linux-musl; LLVM has
# not dropped the Hexagon backend) and accept the first one found, unless
# -Dhexagon_cc= names one explicitly.
hexagon_cc_opt = get_option('hexagon_cc')

if hexagon_cc_opt != ''
  clang_candidates = [hexagon_cc_opt]
else
  clang_candidates = [
    'clang-19', 'clang-18', 'clang-17', 'clang-16', 'clang-15', 'clang-14', 'clang',
  ]
endif

clang_prog = disabler()
clang_name = ''
foreach cand : clang_candidates
  cand_prog = find_program(cand, required : false)
  if cand_prog.found() and not clang_prog.found()
    clang_prog = cand_prog
    clang_name = cand
  endif
endforeach

if not clang_prog.found()
  error(
    'loader (loader): no clang found. Tried: ' +
    ', '.join(clang_candidates) + '.\n' +
    'Install clang 14-19 (Debian/Ubuntu package "clang" or ' +
    '"clang-<version>") and retry, or point at one explicitly:\n' +
    '  meson configure <builddir> -Dhexagon_cc=/path/to/clang\n' +
    'aptX Adaptive will not build without this -- by itself that does NOT ' +
    'break audio: without the loader, the codec simply never registers, and ' +
    'the M4 stays on regular aptX HD (see README.md, "Requirements").'
  )
endif

# --- 2. verify THIS clang actually has the Hexagon backend built in ---
#
# Some distutions build clang with a reduced LLVM_TARGETS_TO_BUILD that
# excludes less common backends. --target=hexagon alone does not fail
# until you actually try to compile something for it, so probe with a
# trivial freestanding translation unit instead of trusting --version.
hexagon_probe = run_command(
  clang_prog, '--target=hexagon-unknown-linux-musl', '-mcpu=hexagonv66',
  '-c', meson.current_source_dir() / 'hexagon-probe.c', '-o', meson.current_build_dir() / 'hexagon-probe.o',
  check : false,
)
if hexagon_probe.returncode() != 0
  error(
    'loader (loader): ' + clang_name + ' was found (' +
    clang_prog.full_path() + '), but cannot target hexagon-unknown-linux-musl.\n' +
    'clang output:\n' + hexagon_probe.stderr().strip() + '\n' +
    'You need a clang build with the Hexagon backend enabled (this is the ' +
    'case in official Debian/Ubuntu packages from version 14 onward -- LLVM ' +
    'is built there with every target). Try a different installed version ' +
    'via -Dhexagon_cc=/path/to/clang-NN.'
  )
endif

# --- 3. verify lld is present (required by -fuse-ld=lld below) ---
lld_prog = find_program('ld.lld', required : false)
if not lld_prog.found()
  error(
    'loader (loader): lld (ld.lld) is required to link ' +
    'hx_adaptive_loader -- plain GNU ld does not understand clang\'s output ' +
    'for hexagon-unknown-linux-musl. Install the "lld" package (usually ' +
    'part of "llvm") and retry.'
  )
endif

hx_cflags = [
  '--target=hexagon-unknown-linux-musl', '-mcpu=hexagonv66',
  '-O2', '-g',
  '-ffreestanding', '-fno-builtin', '-nostdlib', '-static',
  '-fuse-ld=lld',
  '-Wall', '-Wextra', '-Wno-unused-parameter',
  '-fno-stack-protector',
]

hx_adaptive_loader = custom_target('hx_adaptive_loader',
  input : hx_srcs,
  output : 'hx_adaptive_loader',
  command : [clang_prog] + hx_cflags + ['@INPUT@', '-o', '@OUTPUT@'],
  build_by_default : true,
  install : true,
  # matches common/m4-conf-keys.def's compiled default for `[helper]
  # loader` (@libdir@/momentum4/hexagon/hx_adaptive_loader).
  install_dir : get_option('libdir') / 'momentum4' / 'hexagon',
)
