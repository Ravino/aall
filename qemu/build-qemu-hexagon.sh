#!/usr/bin/env bash
# qemu/build-qemu-hexagon.sh -- builds a linux-user-only qemu-hexagon from
# upstream source . No sudo, no
# `apt install`, no pip (see "Dependencies" below and the README.md next to
# this script for exactly why).
#
# Usage:
#   qemu/build-qemu-hexagon.sh [--version X.Y.Z] [--sha256 HEX] [--prefix DIR]
#                              [--jobs N] [--skip-verify]
#
#   --version X.Y.Z  which upstream QEMU release to build (default: 11.1.0)
#   --sha256 HEX      expected sha256 of qemu-X.Y.Z.tar.xz. Defaults to the
#                      hash below for 11.1.0 (independently re-downloaded
#                      and re-hashed twice on two different runs while
# writing this script
#                      qemu.org does not itself publish a separate
#                      checksum/signature file to cross-verify against, see
#                      the README for what that does and does not mean).
#                      For any OTHER --version, you must pass --sha256
#                      explicitly (or --skip-verify, at your own risk) --
#                      this script refuses to download an unpinned tarball
#                      silently.
#   --prefix DIR       where to install (default: qemu/build/install,
#                       relative to the repository root)
#   --jobs N            ninja -j (default: nproc)
#   --skip-verify       download/build even if the sha256 does not match
#                        (or is unknown for a non-default --version) --
#                        NOT recommended, only for developers deliberately
#                        testing a different upstream revision
#
# What this script does NOT do, on purpose:
#   - does not touch anything outside qemu/build/ (this script's own
#     working directory) and the --prefix directory
#   - does not run `apt install`/`apt-get install` (only checks that the
#     tools listed in "Dependencies" below are already on $PATH, and prints
#     the exact package names to install if not -- installing system
#     packages needs the user's explicit approval, never done silently)
#   - does not call `pip install` -- see "Dependencies" for why this is
#     possible at all (QEMU's own configure script sets up meson without
#     reaching the network, as of the 11.1.0 release this was written
#     against)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

VERSION="11.1.0"
# sha256 of https://download.qemu.org/qemu-11.1.0.tar.xz -- confirmed by
# downloading it twice independently (once into this repo's work/ cache,
# once freshly while writing this script) and
# comparing: both copies hash to this exact value.
KNOWN_SHA256_11_1_0="6ee1d1a61f68212476b27108c26da5f449dc09b626d42f8279ba0dc2e08fa858"
SHA256=""
PREFIX="$SCRIPT_DIR/build/install"
JOBS="$(nproc)"
SKIP_VERIFY=0

while [ $# -gt 0 ]; do
	case "$1" in
	--version)
		VERSION="$2"
		shift 2
		;;
	--sha256)
		SHA256="$2"
		shift 2
		;;
	--prefix)
		PREFIX="$2"
		shift 2
		;;
	--jobs)
		JOBS="$2"
		shift 2
		;;
	--skip-verify)
		SKIP_VERIFY=1
		shift
		;;
	-h | --help)
		sed -n '2,40p' "${BASH_SOURCE[0]}"
		exit 0
		;;
	*)
		echo "unknown argument: $1" >&2
		exit 2
		;;
	esac
done

if [ -z "$SHA256" ]; then
	if [ "$VERSION" = "11.1.0" ]; then
		SHA256="$KNOWN_SHA256_11_1_0"
	elif [ "$SKIP_VERIFY" != "1" ]; then
		echo "error: no known sha256 for qemu-$VERSION.tar.xz -- pass --sha256 HEX" >&2
		echo "  (find it yourself, e.g. by downloading once and running sha256sum," >&2
		echo "  or via a source you trust) or --skip-verify to proceed unverified." >&2
		exit 1
	fi
fi

# --- dependency check (no installs, just a clear error naming the package) ---
missing=()
command -v python3 >/dev/null 2>&1 || missing+=("python3")
python3 -c "import venv" >/dev/null 2>&1 || missing+=("python3-venv")
command -v ninja >/dev/null 2>&1 || missing+=("ninja-build")
pkg-config --exists glib-2.0 2>/dev/null || missing+=("libglib2.0-dev")
command -v flex >/dev/null 2>&1 || missing+=("flex")
command -v bison >/dev/null 2>&1 || missing+=("bison")
command -v curl >/dev/null 2>&1 || missing+=("curl")
if [ "${#missing[@]}" -gt 0 ]; then
	echo "error: missing dependencies: ${missing[*]}" >&2
	echo " install with (needs maintainer/user approval if you did not run this yourself):" >&2
	echo "    sudo apt install ${missing[*]}" >&2
	echo "  see qemu/README.md \"Dependencies\" for why each one is needed." >&2
	exit 1
fi

BUILD_DIR="$SCRIPT_DIR/build"
TARBALL="$BUILD_DIR/qemu-$VERSION.tar.xz"
SRC_DIR="$BUILD_DIR/qemu-$VERSION"
mkdir -p "$BUILD_DIR"

# --- download + verify ---
if [ ! -f "$TARBALL" ]; then
	echo "== downloading qemu-$VERSION.tar.xz (~140 MB) =="
	curl -fL -o "$TARBALL.partial" "https://download.qemu.org/qemu-$VERSION.tar.xz"
	mv "$TARBALL.partial" "$TARBALL"
fi

if [ -n "$SHA256" ]; then
	ACTUAL="$(sha256sum "$TARBALL" | cut -d' ' -f1)"
	if [ "$ACTUAL" != "$SHA256" ]; then
		echo "error: sha256 mismatch for $TARBALL" >&2
		echo "  expected: $SHA256" >&2
		echo "  actual:   $ACTUAL" >&2
		if [ "$SKIP_VERIFY" != "1" ]; then
			echo "  refusing to build from an unverified tarball (--skip-verify overrides this)." >&2
			rm -f "$TARBALL"
			exit 1
		fi
		echo "  --skip-verify given: proceeding anyway." >&2
	else
		echo "== sha256 verified: $ACTUAL =="
	fi
elif [ "$SKIP_VERIFY" != "1" ]; then
	# unreachable given the check above, kept as a defensive second gate
	echo "error: no sha256 to verify against and --skip-verify not given" >&2
	exit 1
fi

if [ ! -d "$SRC_DIR" ]; then
	echo "== extracting =="
	tar xJf "$TARBALL" -C "$BUILD_DIR"
fi

# --- configure + build ---
#
# meson: QEMU's own ./configure sets up a private python venv
# (build/pyvenv) and installs meson INTO it from wheels vendored in the
# QEMU source tree itself (python/wheels/) -- no PyPI/network access, no
# `pip install` run by THIS script. python3-venv (checked above) is what
# lets that mechanism work at all; if it is missing, QEMU's own configure
# will fail with its own (also actionable) error message about creating
# the venv, not this script's.
#
# target-list=hexagon-linux-user + disable-system: only the user-mode
# Hexagon emulator is built (no full-system qemu-system-hexagon, no other
# guest architectures) -- this is what `momentum4 helper-exec` /
# WirePlumber's helper unit actually runs: one Hexagon ELF process
# (hx_adaptive_loader + the CAPI .so) translated by QEMU's TCG JIT, NOT a
# virtual machine (see README.md's "How it works" for the
# user-visible explanation -- KVM/`/dev/kvm` play no role here at all,
# TCG cannot be hardware-accelerated for a foreign guest architecture like
# Hexagon on an x86_64 host).
echo "== configuring (target-list=hexagon-linux-user, linux-user only) =="
(
	cd "$SRC_DIR"
	rm -rf build
	./configure --prefix="$PREFIX" \
		--target-list=hexagon-linux-user \
		--disable-system --enable-linux-user \
		--disable-docs --disable-gtk --disable-sdl --disable-vnc \
		--disable-curses --disable-opengl --disable-virglrenderer \
		--disable-capstone --disable-slirp --disable-werror
	ninja -C build -j"$JOBS"
	ninja -C build install
)

echo "== done: $PREFIX/bin/qemu-hexagon =="
"$PREFIX/bin/qemu-hexagon" --version
echo
echo "Point [helper] qemu at this binary (momentum4.conf), or re-run"
echo "\`make install\` after this script -- it symlinks @libexecdir@/momentum4/qemu-hexagon"
echo "at this path automatically when qemu/build/install/bin/qemu-hexagon exists."
