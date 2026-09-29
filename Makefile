#!/usr/bin/make -f
# Makefile -- thin orchestration wrapper .
#
#   make                 build common + daemon + loader (via ./meson.build)
#                        -- the default goal; `make help` prints this list
#                        without building anything
#   make daemon          build only the daemon/CLI (daemon, its own
#                        independent meson project)
#   make module          prepare-tree.sh + nested meson build of the
#                        WirePlumber/PipeWire bluez5 aptX Adaptive plugin
#                        (module) -- needs network the first
#                        time (PipeWire 1.4.2 tarball) unless work/src/
#                        already has a cached copy
#   make qemu            build a fresh qemu-hexagon (qemu/build-qemu-hexagon.sh)
#                        -- optional, a system qemu-hexagon-static also
#                        works, just slower (see README.md "Requirements")
#   make all             build + daemon + module (NOT qemu -- opt-in, see above)
#   make check           run every test suite that does not need a live
#                        Bluetooth device (common, daemon; module's own
#                        qemu-hexagon-backed tests are run separately, see
#                        module/test/*.c's own header comments for the
#                        exact build/run commands -- they are not yet
#                        wired into this target)
#   make install PREFIX=/usr/local     (default; also try ~/.local, no root)
#   make uninstall PREFIX=...          removes exactly what `install` placed
#   make clean            removes build-top/, daemon/build-release/
#                          (NOT module/pipewire-src or
#                          .../build -- those are meson.build's own concern,
#                          `make module-clean`, see below -- kept separate
#                          because re-downloading/re-patching the PipeWire
#                          tree is the expensive part, not worth losing on
#                          every `make clean`)
#   make module-clean     removes module/{pipewire-src,build}
#   make distclean        clean + module-clean + qemu/build
#
# None of this touches the live installation (~/.config, ~/.local) --
# only `momentum4 install`/`enable`/`disable`/`uninstall` (the CLI itself)
# ever writes there, always dry-run unless given --yes.

PREFIX ?= /usr/local
DESTDIR ?=
JOBS ?= $(shell nproc 2>/dev/null || echo 4)
MESON ?= meson
NINJA ?= ninja
PYTHON3 ?= python3

BUILD_TOP := build-top
BUILD_DAEMON := daemon/build-release
MODULE_DIR := module
MODULE_BUILD := $(MODULE_DIR)/build
# BUILD-INFO is written by prepare-tree.sh into
# the *source* tree it reconstructs, not into the (out-of-tree) meson build
# directory above -- see prepare-tree.sh's own "write BUILD-INFO" step.
MODULE_SRC := $(MODULE_DIR)/pipewire-src

MODULE_TARGETS := spa/plugins/bluez5/libspa-bluez5.so \
	spa/plugins/bluez5/libspa-codec-bluez5-aptx.so \
	spa/plugins/bluez5/libspa-codec-bluez5-sbc.so \
	spa/plugins/bluez5/libspa-codec-bluez5-faststream.so \
	spa/plugins/bluez5/libspa-codec-bluez5-ldac.so \
	spa/plugins/bluez5/libspa-codec-bluez5-lc3.so \
	spa/plugins/bluez5/libspa-codec-bluez5-opus.so \
	spa/plugins/bluez5/libspa-codec-bluez5-opus-g.so \
	spa/plugins/bluez5/libspa-codec-bluez5-g722.so

.PHONY: default all build daemon module qemu install uninstall check \
	clean module-clean distclean help

# Bare `make` builds (README.md "Build": "common + daemon/CLI + loader"),
# matching the comment at the top of this file -- it must NOT run `help`
# (a plain `git clone && make` should build the project, not just print
# help text) and must NOT touch the network
# by default (module needs the PipeWire tarball the first time -- that
# stays an explicit `make module` step, see README.md).
.DEFAULT_GOAL := default
default: build daemon

help:
	@sed -n '2,32p' $(firstword $(MAKEFILE_LIST))

all: build daemon module

# ---------------------------------------------------------- build (top) --
build: $(BUILD_TOP)/build.ninja
	$(MESON) compile -C $(BUILD_TOP)

$(BUILD_TOP)/build.ninja:
	$(MESON) setup $(BUILD_TOP) --prefix=$(PREFIX) $(MESON_ARGS)

# --------------------------------------------------------------- daemon --
daemon: $(BUILD_DAEMON)/build.ninja
	$(MESON) compile -C $(BUILD_DAEMON)

$(BUILD_DAEMON)/build.ninja:
	$(MESON) setup $(BUILD_DAEMON) daemon --prefix=$(PREFIX) \
		-Ddev_build=false $(MESON_ARGS)

# --------------------------------------------------------------- module --
#
# auto_features=disabled + the explicit -Dbluez5-codec-*/libusb/opus list:
# matches module/prepare-tree.sh's own documented build
# command exactly (see that script's header comment) -- pinning
# auto_features=disabled is what avoids PipeWire's build silently picking
# up (or silently NOT picking up) unrelated codecs/LC3/libusb depending on
# whatever -dev packages happen to be installed system-wide on the build
# machine .
module: $(MODULE_BUILD)/build.ninja
	$(NINJA) -C $(MODULE_BUILD) $(MODULE_TARGETS)

$(MODULE_BUILD)/build.ninja: $(MODULE_DIR)/prepare-tree.sh
	$(MODULE_DIR)/prepare-tree.sh --force
	$(MESON) setup $(MODULE_BUILD) $(MODULE_DIR)/pipewire-src \
		-Dauto_features=disabled -Dbluez5=enabled \
		-Dbluez5-codec-aptx=enabled -Dbluez5-codec-ldac=enabled \
		-Dbluez5-codec-opus=enabled -Dbluez5-codec-lc3=enabled \
		-Dbluez5-codec-g722=enabled -Dlibusb=enabled -Dopus=enabled

module-clean:
	rm -rf $(MODULE_DIR)/pipewire-src $(MODULE_BUILD)

# ----------------------------------------------------------------- qemu --
qemu:
	qemu/build-qemu-hexagon.sh $(QEMU_ARGS)

# -------------------------------------------------------------- install --
#
# Three independent `meson install` calls (top/daemon each own their own
# meson project, see ./meson.build's header comment for why) plus a plain
# file copy for module/'s .so files (module is not a meson subproject of
# this build either, same reasoning) plus the qemu-hexagon
# symlink-or-warn step described in README.md "Requirements".
install: build daemon
	$(MESON) install -C $(BUILD_TOP) --destdir='$(abspath $(DESTDIR))' --no-rebuild
	$(MESON) install -C $(BUILD_DAEMON) --destdir='$(abspath $(DESTDIR))' --no-rebuild
	@libdir="$$($(PYTHON3) tools/build/meson-option.py $(BUILD_TOP) libdir)"; \
	libexecdir="$$($(PYTHON3) tools/build/meson-option.py $(BUILD_TOP) libexecdir)"; \
	plugindir="$(DESTDIR)$(PREFIX)/$$libdir/momentum4/spa-plugins/bluez5"; \
	qemudir="$(DESTDIR)$(PREFIX)/$$libexecdir/momentum4"; \
	if [ -f "$(MODULE_BUILD)/build.ninja" ]; then \
		echo "installing module .so files to $$plugindir"; \
		mkdir -p "$$plugindir"; \
		for t in $(MODULE_TARGETS); do \
			cp -f "$(MODULE_BUILD)/$$t" "$$plugindir/"; \
		done; \
		if [ -f "$(MODULE_SRC)/spa/plugins/bluez5/BUILD-INFO" ]; then \
			cp -f "$(MODULE_SRC)/spa/plugins/bluez5/BUILD-INFO" "$$plugindir/"; \
		elif [ -f "$(MODULE_BUILD)/spa/plugins/bluez5/BUILD-INFO" ]; then \
			cp -f "$(MODULE_BUILD)/spa/plugins/bluez5/BUILD-INFO" "$$plugindir/"; \
		else \
			echo "WARNING: BUILD-INFO not found under $(MODULE_SRC)/spa/plugins/bluez5/"; \
			echo " -- the"; \
			echo " wp-env version guard will never"; \
			echo "  enable SPA_PLUGIN_DIR without it. Re-run \`make module\`."; \
		fi; \
	else \
		echo "NOTE: module/ not built (run \`make module\` first) -- aptX Adaptive"; \
		echo "  will not be available; M4/ADK-stereo devices still work on aptX HD."; \
	fi; \
	mkdir -p "$$qemudir"; \
	if [ -x "qemu/build/install/bin/qemu-hexagon" ]; then \
		echo "installing freshly built qemu-hexagon to $$qemudir/qemu-hexagon"; \
		cp -f "qemu/build/install/bin/qemu-hexagon" "$$qemudir/qemu-hexagon"; \
	elif [ -x /usr/bin/qemu-hexagon ]; then \
		echo "linking system /usr/bin/qemu-hexagon into $$qemudir/qemu-hexagon"; \
		ln -sf /usr/bin/qemu-hexagon "$$qemudir/qemu-hexagon"; \
	else \
		echo "NOTE: no qemu-hexagon found (run \`make qemu\` or install"; \
		echo "  qemu-user-static system-wide) -- aptX Adaptive will not be"; \
		echo "  available until one exists; M4/ADK-stereo devices still work"; \
		echo "  on aptX HD."; \
	fi

uninstall:
	$(MESON) --internal uninstall -C $(BUILD_TOP) 2>/dev/null || true
	$(MESON) --internal uninstall -C $(BUILD_DAEMON) 2>/dev/null || true
	@echo "NOTE: module .so files and qemu-hexagon copied/symlinked by"
	@echo "  \`make install\` are not tracked by meson's own install-log and"
	@echo "  are left in place -- remove $(PREFIX)/lib/*/momentum4/ by hand"
	@echo "  if you are fully removing a system-wide install."

# ----------------------------------------------------------------- test --
check: build daemon
	$(MESON) test -C $(BUILD_TOP)
	$(MESON) test -C $(BUILD_DAEMON)
	@if [ -f "$(MODULE_BUILD)/build.ninja" ]; then \
		echo "NOTE: module/ tests (test/*.c) are not yet wired into"; \
		echo "  \`make check\` -- they need a real qemu-hexagon and are run"; \
		echo "  separately today, see each test/*.c's own header comment"; \
		echo "  for the exact build/run command."; \
	fi

# ---------------------------------------------------------------- clean --
clean:
	rm -rf $(BUILD_TOP) $(BUILD_DAEMON)

distclean: clean module-clean
	rm -rf qemu/build
