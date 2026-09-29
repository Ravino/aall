#!/usr/bin/env bash
# tests/test_lifecycle_cli.sh -- end-to-end test of `momentum4
# install|enable|disable|uninstall` against
# the real compiled binary, in a fully isolated $HOME/$XDG_*/momentum4.conf.
#
# The dry-run path (no --yes) is exercised WITHOUT any D-Bus at all
# (env -i, no $DBUS_SESSION_BUS_ADDRESS) -- provably side-effect-free.
#
# The --yes path is exercised against tests/systemd_stub (a minimal fake of
# org.freedesktop.systemd1.Manager), run ONLY under `dbus-run-session` for
# a PRIVATE bus -- see util/systemd_unit.h's own header comment for exactly
# why $HOME/$XDG_RUNTIME_DIR overrides alone do NOT isolate sd-bus (this
# project found out live, the hard way, while developing this feature: an
# early manual test without dbus-run-session connected to the real session
# bus of the developer running it and created two real unit symlinks in
# their actual ~/.config/systemd/user/, since sd_bus_default_user() prefers
# $DBUS_SESSION_BUS_ADDRESS over $XDG_RUNTIME_DIR and that variable was
# still inherited from the outer shell). If dbus-run-session is not
# installed, the --yes checks are skipped (exit 77, meson's "skipped"
# convention) -- the dry-run checks still run unconditionally.
#
# usage: test_lifecycle_cli.sh <momentum4-binary> <systemd_stub-binary>
set -uo pipefail

BIN="${1:?usage: test_lifecycle_cli.sh <momentum4-binary> <systemd_stub-binary>}"
STUB="${2:?usage: test_lifecycle_cli.sh <momentum4-binary> <systemd_stub-binary>}"
[ -x "$BIN" ] || {
	echo "test_lifecycle_cli.sh: not executable: $BIN" >&2
	exit 2
}
[ -x "$STUB" ] || {
	echo "test_lifecycle_cli.sh: not executable: $STUB" >&2
	exit 2
}

FAIL=0
CHECKS=0
check() {
	CHECKS=$((CHECKS + 1))
	if ! eval "$2"; then
		echo "FAIL: $1"
		FAIL=1
	fi
}

D="$(mktemp -d)"
trap 'rm -rf "$D"' EXIT
HOME_DIR="$D/home"
UNITS_DIR="$D/units"
DATADIR="$D/datadir"
mkdir -p "$HOME_DIR/.config" "$UNITS_DIR" "$DATADIR/momentum4/wireplumber"

# One dummy file per unit momentum4 install/disable/uninstall expects to
# find -- content is irrelevant (the stub never reads it), only
# existence/readability matters (see cli/lifecycle.c's access(path, F_OK)
# presence check before it includes a unit in the EnableUnitFiles list).
for u in momentum4.target momentum4d.service momentum4-helper.socket momentum4-graph.service; do
	echo "# dummy $u for test_lifecycle_cli.sh" >"$UNITS_DIR/$u"
done

# util/wp_overlay.c's own datadir_template_path()'s test-only
# $MOMENTUM4_DATADIR override -- see that function's header comment for why
# @datadir@ (a compile-time constant) cannot be pointed at this tmpdir via
# $MOMENTUM4_CONFIG the way everything else in this test is. Synthetic
# content (not the real .in templates from ../../wireplumber/) is enough:
# this test exercises the CLI's copy/substitute/D-Bus call sequence, not
# the exact production template text (that is covered by manually
# installing via ../../meson.build + Makefile).
echo "[Service]
ExecStartPre=-/bin/rm -f %t/momentum4/wp.env" >"$DATADIR/momentum4/wireplumber/99-m4-adaptive-overlay.conf"
echo 'monitor.bluez.rules = [ { matches = [ { node.name = "~bluez_output\\.@M4_ADDRESS_UNDERSCORE@\\..*" } ] } ]' \
	>"$DATADIR/momentum4/wireplumber/51-m4-no-suspend.conf.in"

# `[device] address = auto` (the compiled-in
# default, never set explicitly in $CONF below) would otherwise make
# m4_config_mac_address() run BlueZ auto-discovery over the REAL system
# D-Bus (sd_bus_open_system(), src/bt/bt_discover.c) on every invocation
# below -- on a machine with a live, paired M4 this test happens to pass
# "by accident" using the developer's own real device, not an isolated
# fixture, and on any machine without bluetoothd/org.bluez it fails outright
# . $MOMENTUM4_MAC
# (util/config.c's m4_config_mac_address(), same override MOMENTUM4_CONFIG/
# MOMENTUM4_DATADIR already use for test isolation) short-circuits that
# entirely -- no D-Bus call of any kind is made to resolve the MAC used
# below to test the no-suspend substitution logic.
TEST_MAC="AA:BB:CC:DD:EE:FF"

CONF="$D/momentum4-test.conf"
cat >"$CONF" <<EOF
[paths]
systemd-user-unit-dir = $UNITS_DIR
[helper]
qemu = $UNITS_DIR/fake-qemu
loader = $UNITS_DIR/fake-loader
EOF

echo "== dry-run (env -i, NO D-Bus at all -- must still work and touch nothing) =="
out="$(env -i HOME="$HOME_DIR" XDG_CONFIG_HOME="$HOME_DIR/.config" MOMENTUM4_CONFIG="$CONF" MOMENTUM4_DATADIR="$DATADIR" MOMENTUM4_MAC="$TEST_MAC" \
	PATH="/usr/bin:/bin" "$BIN" install 2>&1)"
rc=$?
check "install dry-run exits 0" '[ "$rc" -eq 0 ]'
check "install dry-run says DRY RUN" 'echo "$out" | grep -q "DRY RUN"'
check "install dry-run wrote nothing under \$HOME" '[ -z "$(find "$HOME_DIR" -type f)" ]'

out="$(env -i HOME="$HOME_DIR" XDG_CONFIG_HOME="$HOME_DIR/.config" MOMENTUM4_CONFIG="$CONF" MOMENTUM4_DATADIR="$DATADIR" MOMENTUM4_MAC="$TEST_MAC" \
	PATH="/usr/bin:/bin" "$BIN" enable 2>&1)"
rc=$?
check "enable dry-run exits 0" '[ "$rc" -eq 0 ]'
check "enable dry-run warns about WirePlumber restart" 'echo "$out" | grep -q "WARNING"'
check "enable dry-run wrote nothing under \$HOME" '[ -z "$(find "$HOME_DIR" -type f)" ]'

out="$(env -i HOME="$HOME_DIR" XDG_CONFIG_HOME="$HOME_DIR/.config" MOMENTUM4_CONFIG="$CONF" MOMENTUM4_DATADIR="$DATADIR" MOMENTUM4_MAC="$TEST_MAC" \
	PATH="/usr/bin:/bin" "$BIN" disable 2>&1)"
rc=$?
check "disable dry-run exits 0" '[ "$rc" -eq 0 ]'

out="$(env -i HOME="$HOME_DIR" XDG_CONFIG_HOME="$HOME_DIR/.config" MOMENTUM4_CONFIG="$CONF" MOMENTUM4_DATADIR="$DATADIR" MOMENTUM4_MAC="$TEST_MAC" \
	PATH="/usr/bin:/bin" "$BIN" uninstall --purge 2>&1)"
rc=$?
check "uninstall --purge dry-run exits 0" '[ "$rc" -eq 0 ]'
check "uninstall dry-run mentions --purge plan" 'echo "$out" | grep -q "purge"'

if ! command -v dbus-run-session >/dev/null 2>&1; then
	echo "dbus-run-session not found -- skipping --yes checks (dry-run checks above still ran)"
	if [ "$FAIL" -ne 0 ]; then
		echo "checks: $CHECKS, FAILED"
		exit 1
	fi
	echo "checks: $CHECKS, all OK (dry-run only)"
	exit 77
fi

# --- --yes path, against the private stub bus ---
run_yes() {
	# $1 = subcommand args (e.g. "install --yes"), writes stub+cli output
	# to $2 (a file), returns the CLI's exit code via $RUN_YES_RC.
	local args="$1" outfile="$2"
	dbus-run-session -- bash -c '
		export HOME="'"$HOME_DIR"'"
		export MOMENTUM4_MAC="'"$TEST_MAC"'"
		export XDG_CONFIG_HOME="'"$HOME_DIR"'/.config"
		export MOMENTUM4_CONFIG="'"$CONF"'"
		export MOMENTUM4_DATADIR="'"$DATADIR"'"
		"'"$STUB"'" >"'"$D"'/stub.log" 2>&1 &
		STUB_PID=$!
		for i in $(seq 1 50); do
			grep -q "STUB: ready" "'"$D"'/stub.log" 2>/dev/null && break
			sleep 0.05
		done
		"'"$BIN"'" '"$args"'
		rc=$?
		kill "$STUB_PID" 2>/dev/null
		wait "$STUB_PID" 2>/dev/null
		exit "$rc"
	' >"$outfile" 2>&1
	RUN_YES_RC=$?
}

echo "== install --yes (stub bus) =="
run_yes "install --yes" "$D/install.out"
check "install --yes exits 0" '[ "$RUN_YES_RC" -eq 0 ]'
check "install --yes wrote momentum4.conf (MOMENTUM4_CONFIG target)" '[ -f "$CONF" ]'
check "install --yes wrote overlay" \
	'[ -f "$HOME_DIR/.config/systemd/user/wireplumber.service.d/99-m4-adaptive-overlay.conf" ]'
check "install --yes wrote no-suspend" \
	'[ -f "$HOME_DIR/.config/wireplumber/wireplumber.conf.d/51-m4-no-suspend.conf" ]'
check "install --yes enabled all 4 units" \
	'[ "$(grep -c "STUB: EnableUnitFiles" "'"$D"'/stub.log")" -eq 4 ]'
check "no-suspend has a real MAC substituted, not the placeholder" \
	'! grep -q "@M4_ADDRESS_UNDERSCORE@" "'"$HOME_DIR"'/.config/wireplumber/wireplumber.conf.d/51-m4-no-suspend.conf"'

echo "== install --yes again (idempotent -- 0 conf keys changed) =="
run_yes "install --yes" "$D/install2.out"
check "second install --yes exits 0" '[ "$RUN_YES_RC" -eq 0 ]'
check "second install --yes changed 0 keys" 'grep -q "momentum4.conf: 0 key" "$D/install2.out"'

echo "== enable --yes (stub bus) =="
run_yes "enable --yes" "$D/enable.out"
check "enable --yes exits 0" '[ "$RUN_YES_RC" -eq 0 ]'
check "enable --yes started momentum4d.service" \
	'grep -q "STUB: StartUnit(momentum4d.service" "'"$D"'/stub.log"'
check "enable --yes restarted wireplumber" \
	'grep -q "STUB: RestartUnit(wireplumber.service" "'"$D"'/stub.log"'
check "enable --yes report shows active units" 'grep -q "momentum4d.service.*active" "$D/enable.out"'

echo "== disable --yes (stub bus) =="
run_yes "disable --yes" "$D/disable.out"
check "disable --yes exits 0" '[ "$RUN_YES_RC" -eq 0 ]'
check "disable --yes stopped momentum4d.service" \
	'grep -q "STUB: StopUnit(momentum4d.service" "'"$D"'/stub.log"'
check "disable --yes removed overlay" \
	'[ ! -f "$HOME_DIR/.config/systemd/user/wireplumber.service.d/99-m4-adaptive-overlay.conf" ]'
check "disable --yes removed no-suspend" \
	'[ ! -f "$HOME_DIR/.config/wireplumber/wireplumber.conf.d/51-m4-no-suspend.conf" ]'
check "disable --yes left momentum4.conf alone" '[ -f "$CONF" ]'

echo "== uninstall --purge --yes (stub bus) =="
run_yes "uninstall --purge --yes" "$D/uninstall.out"
check "uninstall --purge --yes exits 0" '[ "$RUN_YES_RC" -eq 0 ]'
check "uninstall --purge removed momentum4.conf (MOMENTUM4_CONFIG target)" '[ ! -f "$CONF" ]'

echo "== [general] language = ru (common/m4-i18n.h M4_T()): install dry-run switches =="
# The ONE end-to-end exercise of the ru branch of M4_T() through
# `momentum4 install` (item 4 of the language-support task) -- every dry-run
# check above implicitly assumes the default (no [general] language key set
# -> "en").
RUCONF="$D/momentum4-test-ru.conf"
cat >"$RUCONF" <<EOF
[general]
language = ru
[paths]
systemd-user-unit-dir = $UNITS_DIR
[helper]
qemu = $UNITS_DIR/fake-qemu
loader = $UNITS_DIR/fake-loader
EOF
out="$(env -i HOME="$HOME_DIR" XDG_CONFIG_HOME="$HOME_DIR/.config" MOMENTUM4_CONFIG="$RUCONF" MOMENTUM4_DATADIR="$DATADIR" MOMENTUM4_MAC="$TEST_MAC" \
	PATH="/usr/bin:/bin" "$BIN" install 2>&1)"
rc=$?
check "install dry-run (ru) exits 0" '[ "$rc" -eq 0 ]'
check "install dry-run (ru) says СУХОЙ ПРОГОН" 'echo "$out" | grep -q "СУХОЙ ПРОГОН"'
check "install dry-run (ru) wrote nothing under \$HOME" '[ -z "$(find "$HOME_DIR" -type f)" ]'

if [ "$FAIL" -ne 0 ]; then
	echo "checks: $CHECKS, FAILED"
	exit 1
fi
echo "checks: $CHECKS, all OK"
