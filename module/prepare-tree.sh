#!/usr/bin/env bash
# prepare-tree.sh -- reconstructs a build-ready, patched PipeWire 1.4.2 source
# tree for the aptX Adaptive (M4) SPA codec, without keeping a full copy of
# upstream PipeWire in git (hundreds of files, unrelated to this project).
#
# The output tree is named "pipewire-src" (default: a sibling directory of
# this script, i.e. module/pipewire-src) to match the layout
# every other script/test/doc in this directory already assumes (meson build
# directories "build"/"build-asan" configured directly against it, e.g.
# `meson setup build pipewire-src ...`; the top-level Makefile's `make
# module`/`make install` expect compiled plugins under
# build/spa/plugins/bluez5/*.so; test/*.c use
# `-Ipipewire-src/spa/include`). It is NOT named "build" itself (that name is
# reserved for the meson build directories) -- it is still git-ignored, via
# an explicit module/pipewire-src/ rule in the top-level
# .gitignore (its name doesn't match the generic "build*" pattern).
#
# What is actually ours and lives in git:
#   patches/*.patch   -- one patch per logical change against upstream
#                         PipeWire 1.4.2 (audio.h enum, type-info.h,
#                         codec-loader.c priority, meson.build, and the
#                         aptx_adaptive codec itself in a2dp-codec-aptx.c).
#   m4-src/*.{c,h}    -- our own new files (qemu-hexagon helper bridge,
#                         heartbeat/config modules, wire protocol header),
#                         not a modification of any upstream file.
#
# This script:
#   1. gets a pristine PipeWire 1.4.2 source tree (from work/src/ if present,
#      else downloads the official release tarball);
#   2. extracts it into a build directory OUTSIDE git ($OUT_DIR, default
#      "build/pipewire-src" under this directory -- matches the repo-wide
#      ".gitignore: build*" rule);
#   3. applies patches/*.patch on top, in order;
#   4. copies m4-src/*.{c,h} into spa/plugins/bluez5/ of that tree.
#
# The result is meant to be functionally identical to the tree that was
# actually used to build/test/install the aptx_adaptive codec (validated
# by a byte-for-byte comparison of this script's output against the
# previous, git-unfriendly copy).
#
# Usage:
#   module/prepare-tree.sh [--out DIR] [--tarball FILE] [--force]
#                                         [--skip-version-check] [--no-build-info]
#
#   --out DIR       output tree directory (default: <this dir>/pipewire-src)
#   --tarball FILE  use this exact pipewire-1.4.2 tarball instead of the
#                   work/src/ copy or a download
#   --force         remove OUT_DIR first if it already exists (default: refuse
#                   to overwrite a non-empty directory)
#   --skip-version-check
#                   build anyway even if the PipeWire actually installed on
#                   this machine is not in SUPPORTED (see "0. supported-
#                   version check" below) -- for developers deliberately
#                   testing against a newer/older PipeWire, NOT a normal
#                   flag to reach for. The resulting .so is still only
#                   PW_VERSION-ABI-compatible; loading it into a differently-
#                   versioned running WirePlumber is unsupported regardless
#                   of this flag (that risk is what the overlay's own
#                   `momentum4 wp-env` ExecStartPre version guard is for --
#                   this flag only skips prepare-tree.sh's own early
#                   warning).
#   --no-build-info generate the tree without spa/plugins/bluez5/BUILD-INFO
#                   (normally written so `momentum4 doctor`/a human can
#                   later confirm what this .so was actually built from --
#                   see "4. write BUILD-INFO" below). Only useful for a byte-
#                   for-byte reproducibility check against a tree that
#                   predates this file existing.
#
# 0. Supported-version check: compares the PipeWire
#    actually installed on THIS machine (best-effort detection: `pipewire
#    --version`'s "Compiled with libpipewire X.Y.Z" line, else `pkg-config
#    --modversion libpipewire-0.3`, else `dpkg-query -W -f='${Version}'
#    pipewire` with the Debian revision stripped) against the SUPPORTED file
#    (one version per line, currently just 1.4.2 -- the only version these
#    patches have ever been built/tested against). A clean, actionable
#    error and non-zero exit if it does not match (or PipeWire could not be
#    detected at all) -- unless --skip-version-check. This is a BUILD-TIME
#    sanity check only, not the runtime safety net (that is `momentum4
#    wp-env`'s own ExecStartPre guard in the WirePlumber overlay, which
#    only takes effect once the module is actually installed).
#
# After running this script, build as usual, e.g.:
#   cd module
#   cp env.sh.example env.sh   # first time only
#   source env.sh
#   meson setup build pipewire-src \
#     -Dauto_features=disabled -Dbluez5=enabled \
#     -Dbluez5-codec-aptx=enabled -Dbluez5-codec-ldac=enabled \
#     -Dbluez5-codec-opus=enabled -Dbluez5-codec-lc3=enabled \
#     -Dbluez5-codec-g722=enabled -Dlibusb=enabled -Dopus=enabled
#   ninja -C build spa/plugins/bluez5/{libspa-bluez5.so,libspa-codec-bluez5-aptx.so,libspa-codec-bluez5-sbc.so,libspa-codec-bluez5-faststream.so,libspa-codec-bluez5-ldac.so,libspa-codec-bluez5-lc3.so,libspa-codec-bluez5-opus.so,libspa-codec-bluez5-opus-g.so,libspa-codec-bluez5-g722.so}
#
# (`make install` expects the plugins at exactly
# build/spa/plugins/bluez5/*.so -- keep the build directory named "build",
# configured directly against pipewire-src, as above.)
#
# (The exact meson option set above matches what the installed Debian
# pipewire package resolves to on this machine. Adjust
# if your distribution's bluez5 codec set differs.)

set -euo pipefail

PW_VERSION="1.4.2"
PW_TARBALL_NAME="pipewire-${PW_VERSION}.tar.bz2"
PW_TARBALL_SHA256="aa9098001e1bc2f7742140727e19bcd57812b686840923afda2be6c227b88bf0"
PW_OFFICIAL_URL="https://gitlab.freedesktop.org/pipewire/pipewire/-/archive/${PW_VERSION}/pipewire-${PW_VERSION}.tar.bz2"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

SUPPORTED_FILE="$SCRIPT_DIR/SUPPORTED"

OUT_DIR="$SCRIPT_DIR/pipewire-src"
TARBALL_OVERRIDE=""
FORCE=0
SKIP_VERSION_CHECK=0
NO_BUILD_INFO=0

while [ $# -gt 0 ]; do
	case "$1" in
	--out)
		OUT_DIR="$2"
		shift 2
		;;
	--tarball)
		TARBALL_OVERRIDE="$2"
		shift 2
		;;
	--force)
		FORCE=1
		shift
		;;
	--skip-version-check)
		SKIP_VERSION_CHECK=1
		shift
		;;
	--no-build-info)
		NO_BUILD_INFO=1
		shift
		;;
	-h | --help)
		sed -n '2,98p' "$0"
		exit 0
		;;
	*)
		echo "prepare-tree.sh: unknown argument: $1" >&2
		exit 1
		;;
	esac
done

log() { printf '[prepare-tree] %s\n' "$1"; }

# --- 0. supported-version check --------------------------------------------
#
# Best-effort detection of the PipeWire actually installed on THIS machine,
# tried in order (first one that succeeds wins):
#   1. `pipewire --version`'s "Compiled with libpipewire X.Y.Z" line --
#      confirmed to work with no -dev package installed (just the pipewire
#      binary itself), which is the common case for anyone who has not set
#      up a full build environment yet -- exactly who this check is for.
#   2. `pkg-config --modversion libpipewire-0.3` -- works if the -dev
#      package (or an equivalent .pc file) happens to be installed; kept as
#      a fallback since it is the more "standard" mechanism and some
#      environments only have it.
#   3. `dpkg-query -W -f='${Version}' pipewire` on Debian/Ubuntu, with the
#      Debian package revision (the "-1", "-1~bpo12+1", etc. suffix after
#      the upstream version) stripped -- last resort for a minimal chroot/
#      container that has neither the pipewire binary on PATH nor
#      pkg-config data for it.
detect_installed_pipewire_version() {
	local v
	if command -v pipewire >/dev/null 2>&1; then
		v="$(pipewire --version 2>/dev/null | sed -n 's/^Compiled with libpipewire \([0-9][0-9.]*\).*/\1/p' | head -1)"
		if [ -n "$v" ]; then
			printf '%s' "$v"
			return 0
		fi
	fi
	if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists libpipewire-0.3 2>/dev/null; then
		v="$(pkg-config --modversion libpipewire-0.3 2>/dev/null)"
		if [ -n "$v" ]; then
			printf '%s' "$v"
			return 0
		fi
	fi
	if command -v dpkg-query >/dev/null 2>&1; then
		v="$(dpkg-query -W -f='${Version}' pipewire 2>/dev/null | sed -e 's/^[0-9]*://' -e 's/-[^-]*$//')"
		if [ -n "$v" ]; then
			printf '%s' "$v"
			return 0
		fi
	fi
	return 1
}

is_version_supported() {
	local v="$1" line
	[ -f "$SUPPORTED_FILE" ] || return 1
	while IFS= read -r line; do
		line="${line%%#*}"
		line="$(printf '%s' "$line" | tr -d '[:space:]')"
		[ -n "$line" ] || continue
		[ "$line" = "$v" ] && return 0
	done <"$SUPPORTED_FILE"
	return 1
}

if [ "$SKIP_VERSION_CHECK" = "1" ]; then
	log "--skip-version-check: not checking the installed PipeWire version"
else
	INSTALLED_PW_VERSION="$(detect_installed_pipewire_version || true)"
	if [ -z "$INSTALLED_PW_VERSION" ]; then
		log "WARNING: could not detect an installed PipeWire version on this machine"
		log "  (tried: \`pipewire --version\`, \`pkg-config --modversion libpipewire-0.3\`, \`dpkg-query ... pipewire\`)"
		log "  continuing anyway -- this tree will be built against pipewire-${PW_VERSION}"
		log "  regardless, but there is nothing installed here to compare it against yet."
	elif is_version_supported "$INSTALLED_PW_VERSION"; then
		log "installed PipeWire ${INSTALLED_PW_VERSION} is in SUPPORTED -- proceeding"
	else
		{
			echo "prepare-tree.sh: installed PipeWire is ${INSTALLED_PW_VERSION}, which is NOT in"
			echo "  $SUPPORTED_FILE (currently: $(tr '\n' ' ' <"$SUPPORTED_FILE" | sed -E 's/#[^ ]*//g; s/ +/ /g'))."
			echo ""
			echo "  This patch set has only ever been built and tested against PipeWire"
			echo "  ${PW_VERSION}. Building it anyway and loading the resulting .so into a"
			echo "  differently-versioned WirePlumber is UNSUPPORTED and may crash WirePlumber"
			echo "  or silently produce no Bluetooth audio at all -- do not skip this"
			echo "  check on a live installation without also re-testing end to end."
			echo ""
			echo "  What to do:"
			echo "   - if you just want to build+test against pipewire-${PW_VERSION} regardless"
			echo "     of what is installed (e.g. in a clean container, before installing"
			echo "     PipeWire at all), re-run with --skip-version-check;"
			echo "   - if PipeWire ${INSTALLED_PW_VERSION} support is what you actually need, that"
			echo "     requires re-checking/porting patches/*.patch against that version first"
			echo " and adding a new line to"
			echo "     $SUPPORTED_FILE only once that has actually been done."
		} >&2
		exit 3
	fi
fi

# --- 1. locate or fetch the pristine tarball ------------------------------

find_tarball() {
	if [ -n "$TARBALL_OVERRIDE" ]; then
		printf '%s' "$TARBALL_OVERRIDE"
		return 0
	fi
	local candidate="$REPO_ROOT/work/src/pipewire_${PW_VERSION}.orig.tar.bz2"
	if [ -f "$candidate" ]; then
		printf '%s' "$candidate"
		return 0
	fi
	printf ''
}

verify_sha256() {
	local file="$1"
	local got
	got="$(sha256sum "$file" | awk '{print $1}')"
	[ "$got" = "$PW_TARBALL_SHA256" ]
}

TARBALL="$(find_tarball)"
if [ -z "$TARBALL" ]; then
	mkdir -p "$REPO_ROOT/work/src"
	TARBALL="$REPO_ROOT/work/src/$PW_TARBALL_NAME"
	log "no local tarball found, downloading $PW_OFFICIAL_URL"
	if command -v curl >/dev/null 2>&1; then
		curl -fL --retry 3 -o "$TARBALL" "$PW_OFFICIAL_URL"
	elif command -v wget >/dev/null 2>&1; then
		wget -O "$TARBALL" "$PW_OFFICIAL_URL"
	else
		echo "prepare-tree.sh: neither curl nor wget available, and no local tarball in work/src/" >&2
		exit 1
	fi
fi

log "using tarball: $TARBALL"
if command -v sha256sum >/dev/null 2>&1; then
	if verify_sha256 "$TARBALL"; then
		log "sha256 OK ($PW_TARBALL_SHA256)"
	else
		log "WARNING: sha256 of $TARBALL does not match the known-good value for pipewire-${PW_VERSION}"
		log "  expected: $PW_TARBALL_SHA256"
		log "  got:      $(sha256sum "$TARBALL" | awk '{print $1}')"
		log "  continuing anyway (you may have a different but valid upstream tarball, e.g. re-packaged); patches may fail to apply if the content differs."
	fi
fi

# --- 2. extract into OUT_DIR ------------------------------------------------

if [ -e "$OUT_DIR" ]; then
	if [ "$FORCE" = "1" ]; then
		log "removing existing $OUT_DIR (--force)"
		rm -rf -- "$OUT_DIR"
	else
		echo "prepare-tree.sh: $OUT_DIR already exists, use --force to overwrite" >&2
		exit 1
	fi
fi

WORK_EXTRACT="$(mktemp -d)"
trap 'rm -rf -- "$WORK_EXTRACT"' EXIT

log "extracting $TARBALL"
tar xf "$TARBALL" -C "$WORK_EXTRACT"

EXTRACTED_DIR="$(find "$WORK_EXTRACT" -mindepth 1 -maxdepth 1 -type d | head -1)"
if [ -z "$EXTRACTED_DIR" ]; then
	echo "prepare-tree.sh: could not find extracted top-level directory in $TARBALL" >&2
	exit 1
fi

mkdir -p "$(dirname "$OUT_DIR")"
mv "$EXTRACTED_DIR" "$OUT_DIR"
log "extracted to $OUT_DIR"

# --- 3. apply patches -------------------------------------------------------

shopt -s nullglob
PATCHES=("$SCRIPT_DIR"/patches/*.patch)
shopt -u nullglob

if [ "${#PATCHES[@]}" -eq 0 ]; then
	log "WARNING: no patches found in $SCRIPT_DIR/patches -- tree is plain upstream"
fi

for p in "${PATCHES[@]}"; do
	log "applying $(basename "$p")"
	patch --strip=1 -d "$OUT_DIR" --forward < "$p"
done

# --- 4. copy our new files --------------------------------------------------

M4_SRC_DIR="$SCRIPT_DIR/m4-src"
if [ -d "$M4_SRC_DIR" ]; then
	DEST="$OUT_DIR/spa/plugins/bluez5"
	log "copying our new files from m4-src/ into spa/plugins/bluez5/"
	for f in "$M4_SRC_DIR"/*.c "$M4_SRC_DIR"/*.h; do
		[ -e "$f" ] || continue
		cp -v "$f" "$DEST/"
	done
else
	log "WARNING: $M4_SRC_DIR not found -- aptx_adaptive codec sources will be missing"
fi

# --- 4b. copy the shared config parser (common/m4-conf.c) ------------------
#
# common/m4-conf.{c,h}/m4-conf-keys.def are consumed by BOTH the daemon
# (daemon, via a plain relative meson source path) and this SPA
# plugin -- see common/m4-conf.h's own "common/ placement" header comment for
# why this is a copy step here (same mechanism as step 4 above for m4-src/)
# rather than a meson subproject(): PipeWire's own build does not go through
# an intermediate project this repo controls. Copied verbatim, byte for
# byte, from the ONE source of truth in common/ -- never duplicated/hand-
# edited in this tree. patches/0011 (meson.build) adds m4-conf.c to both the
# bluez_codec_aptx and bluez5lib (libspa-bluez5.so) source lists.
# m4-i18n.{c,h} (common/m4-i18n.h's M4_T()/m4_lang) is copied alongside it
# because m4-conf.c now links against it (the registry's comment_en/
# comment_ru pair is picked with M4_T()) -- the plugin itself never prints
# user-facing text, but it still has to link.
COMMON_DIR="$REPO_ROOT/common"
if [ -d "$COMMON_DIR" ]; then
	DEST="$OUT_DIR/spa/plugins/bluez5"
	log "copying common/m4-conf.{c,h}/m4-conf-keys.def/m4-i18n.{c,h} into spa/plugins/bluez5/"
	cp -v "$COMMON_DIR/m4-conf.c" "$COMMON_DIR/m4-conf.h" "$COMMON_DIR/m4-conf-keys.def" \
		"$COMMON_DIR/m4-i18n.c" "$COMMON_DIR/m4-i18n.h" "$DEST/"
else
	log "WARNING: $COMMON_DIR not found -- the plugin will fail to build (m4-conf.c missing)"
fi

# --- 5. write BUILD-INFO ----------------------------------------------------
#
# BUILD-INFO is written next to the .so and read by `momentum4 doctor`.
# Plain `key = value` lines (same style as the legacy aptx-adaptive.conf, easy
# for both a human and a future small C parser to read) written into
# spa/plugins/bluez5/BUILD-INFO -- spa/plugins/bluez5/meson.build's own
# aptx_dep.found() block (patches/0010) install_data()s this file into the
# SAME directory as libspa-codec-bluez5-aptx.so (spa_plugindir / 'bluez5'),
# so it always sits right next to the .so it describes, wherever that ends
# up installed.
#
# patches_sha256/m4_src_sha256 are "hash of the sorted list of per-file
# sha256sums" (sha256sum run twice), NOT a hash of the concatenated file
# bytes directly -- this is deliberately insensitive to how the files get
# concatenated (line-ending handling, tar member order, ...) and lets
# anyone reproduce the SAME digest from the exact same `sha256sum
# patches/*.patch m4-src/*.c m4-src/*.h | sha256sum` shape without needing
# this exact script.
if [ "$NO_BUILD_INFO" = "1" ]; then
	log "--no-build-info: not writing BUILD-INFO"
else
	BUILD_INFO_DEST="$OUT_DIR/spa/plugins/bluez5/BUILD-INFO"
	log "writing $BUILD_INFO_DEST"

	PATCHES_SHA256="none"
	if [ "${#PATCHES[@]}" -gt 0 ]; then
		PATCHES_SHA256="$(sha256sum "${PATCHES[@]}" | LC_ALL=C sort | sha256sum | awk '{print $1}')"
	fi

	M4_SRC_FILES=()
	if [ -d "$M4_SRC_DIR" ]; then
		shopt -s nullglob
		M4_SRC_FILES=("$M4_SRC_DIR"/*.c "$M4_SRC_DIR"/*.h)
		shopt -u nullglob
	fi
	# common/m4-conf.{c,h}/m4-conf-keys.def (step 4b above) are part of
	# what actually ends up compiled into the .so this BUILD-INFO describes
	# just as much as m4-src/ is -- included in the same hash so a change to
	# either shows up here.
	if [ -f "$COMMON_DIR/m4-conf.c" ]; then
		M4_SRC_FILES+=("$COMMON_DIR/m4-conf.c" "$COMMON_DIR/m4-conf.h" "$COMMON_DIR/m4-conf-keys.def" \
			"$COMMON_DIR/m4-i18n.c" "$COMMON_DIR/m4-i18n.h")
	fi
	M4_SRC_SHA256="none"
	if [ "${#M4_SRC_FILES[@]}" -gt 0 ]; then
		M4_SRC_SHA256="$(sha256sum "${M4_SRC_FILES[@]}" | LC_ALL=C sort | sha256sum | awk '{print $1}')"
	fi

	{
		printf 'pipewire_version=%s\n' "$PW_VERSION"
		printf 'pipewire_tarball_sha256=%s\n' "$PW_TARBALL_SHA256"
		printf 'patches_count=%s\n' "${#PATCHES[@]}"
		printf 'patches_sha256=%s\n' "$PATCHES_SHA256"
		printf 'm4_src_files_count=%s\n' "${#M4_SRC_FILES[@]}"
		printf 'm4_src_sha256=%s\n' "$M4_SRC_SHA256"
		printf 'prepared_at_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
		printf 'prepared_host_pipewire_version=%s\n' "${INSTALLED_PW_VERSION:-unknown}"
	} >"$BUILD_INFO_DEST"
fi

log "done. Tree ready at: $OUT_DIR"
log "next: meson setup a build directory against it and build the bluez5 plugins (see header of this script for the exact command)."
