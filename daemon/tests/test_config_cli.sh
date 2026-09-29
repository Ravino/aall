#!/usr/bin/env bash
# tests/test_config_cli.sh -- end-to-end test of `momentum4 config
# show|validate|defaults|migrate` against the
# real compiled binary, in a fully isolated $HOME/$XDG_*, so it never reads
# or writes anything outside its own tmpdir. No python, no framework --
# same "plain script, exit non-zero on first failure" convention as the
# rest of this project's *.sh tests (see test_automode_diff.sh).
#
# usage: test_config_cli.sh <path-to-momentum4-binary>
set -uo pipefail

BIN="${1:?usage: test_config_cli.sh <path-to-momentum4-binary>}"
if [ ! -x "$BIN" ]; then
	echo "test_config_cli.sh: binary not found/executable: $BIN" >&2
	exit 2
fi

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
export HOME="$D/home"
export XDG_CONFIG_HOME="$D/home/.config"
export XDG_RUNTIME_DIR="$D/xdg"
mkdir -p "$HOME" "$XDG_CONFIG_HOME" "$XDG_RUNTIME_DIR"
unset MOMENTUM4_CONFIG M4_ADAPTIVE_CONFIG

# Isolate layers 2 (@datadir@/momentum4/momentum4.conf) and 3
# (/etc/momentum4/momentum4.conf) with the same test-only overrides
# common/m4-conf.c's own test_load_layers_isolated() uses,
# pointed at scratch paths that do not exist. Without
# this, "validate"'s "0 layers found" check below silently depends on
# whether a real `make install` already left /usr/local/share/
# momentum4/momentum4.conf on the machine running this test -- it does
# after the README's own module->install->check order, and the test
# used to fail right after a clean install for the same reason, but in
# this script, which the fix for that did not touch).
export MOMENTUM4_DATADIR="$D/no-such-datadir"
export MOMENTUM4_ETC_CONF="$D/no-such-etc-momentum4.conf"

echo "== config show (no user file at all -- pure compiled-in defaults) =="
out="$("$BIN" config show)"
rc=$?
check "show exits 0" '[ "$rc" -eq 0 ]'
check "show: has [device] section" 'grep -q "^\[device\]$" <<<"$out"'
check "show: mode default ll0" 'grep -qE "^mode +\= ll0 " <<<"$out"'
check "show: every line has a source (\"# \")" '! grep -vE "^\[|^$" <<<"$out" | grep -qv "#"'

echo "== config get/set =="
out="$("$BIN" config get general.language)"
rc=$?
check "get exits 0" '[ "$rc" -eq 0 ]'
check "get general.language default is en" '[ "$out" = "en" ]'
out="$("$BIN" config get bogus.key 2>&1)"
rc=$?
check "get on an unknown key exits 1" '[ "$rc" -eq 1 ]'
check "get on an unknown key says so" 'grep -qi "unknown key" <<<"$out"'

out="$("$BIN" config set general.language ru)"
rc=$?
check "set exits 0" '[ "$rc" -eq 0 ]'
check "set: confirms the new value" 'grep -q "general.language = ru" <<<"$out"'
check "set: wrote [general]/language = ru to the user file" \
	'grep -q "^language = ru$" "$XDG_CONFIG_HOME/momentum4/momentum4.conf"'
out="$("$BIN" config get general.language)"
check "get after set sees the new value" '[ "$out" = "ru" ]'
out="$("$BIN" config set bogus.key val 2>&1)"
rc=$?
check "set on an unknown key exits 1" '[ "$rc" -eq 1 ]'
check "set on an unknown key wrote nothing new" \
	'! grep -q "bogus" "$XDG_CONFIG_HOME/momentum4/momentum4.conf"'
# Every check below this point assumes layer 4 (the user config) is ABSENT
# ("0 layers found") -- remove the file `config set` just created rather
# than merely resetting its one key back to the default.
rm -f "$XDG_CONFIG_HOME/momentum4/momentum4.conf"
rmdir "$XDG_CONFIG_HOME/momentum4" 2>/dev/null || true

echo "== config validate (no files -- 0 layers, 0 warnings) =="
out="$("$BIN" config validate)"
rc=$?
check "validate exits 0 on a clean (absent) config" '[ "$rc" -eq 0 ]'
check "validate: reports 0 layers found" 'grep -q "layers found: 0" <<<"$out"'
check "validate: OK message" 'grep -q "^OK: " <<<"$out"'

echo "== config validate: a fake datadir layer (\$MOMENTUM4_DATADIR) IS picked up =="
# The "0 layers" check above passes trivially if the override is simply
# ignored and the real layer happens to be absent too -- prove the
# override actually feeds m4_conf_load_layers() by pointing it at a
# layer 2 file that DOES exist and checking it is counted.
FAKE_DATADIR="$D/fake-datadir"
mkdir -p "$FAKE_DATADIR/momentum4"
cat >"$FAKE_DATADIR/momentum4/momentum4.conf" <<'EOF'
[device]
address = AA:BB:CC:DD:EE:FF
EOF
out="$(MOMENTUM4_DATADIR="$FAKE_DATADIR" "$BIN" config validate)"
rc=$?
check "validate with fake datadir layer exits 0" '[ "$rc" -eq 0 ]'
check "validate: fake datadir layer is counted (1 layer found)" 'grep -q "layers found: 1" <<<"$out"'
rm -rf "$FAKE_DATADIR"

echo "== config validate: a fake /etc layer (\$MOMENTUM4_ETC_CONF) IS picked up =="
FAKE_ETC_CONF="$D/fake-etc-momentum4.conf"
cat >"$FAKE_ETC_CONF" <<'EOF'
[device]
address = 11:22:33:44:55:66
EOF
out="$(MOMENTUM4_ETC_CONF="$FAKE_ETC_CONF" "$BIN" config validate)"
rc=$?
check "validate with fake etc layer exits 0" '[ "$rc" -eq 0 ]'
check "validate: fake etc layer is counted (1 layer found)" 'grep -q "layers found: 1" <<<"$out"'
rm -f "$FAKE_ETC_CONF"

echo "== config validate PATH (explicit file, with an out-of-range value) =="
BAD="$D/bad.conf"
cat >"$BAD" <<'EOF'
[adaptive]
resync-period-min = 99999
EOF
out="$("$BIN" config validate "$BAD")"
rc=$?
check "validate PATH exits 1 (one warning)" '[ "$rc" -eq 1 ]'
check "validate PATH: mentions resync-period-min" 'grep -q "resync-period-min" <<<"$out"'

echo "== config validate PATH (nonexistent file) =="
out="$("$BIN" config validate "$D/does-not-exist.conf" 2>&1)"
rc=$?
check "validate on a missing explicit PATH exits 1" '[ "$rc" -eq 1 ]'
check "validate on a missing explicit PATH says so" 'grep -qi "not found" <<<"$out"'

echo "== config defaults (full commented reference) =="
out="$("$BIN" config defaults)"
rc=$?
check "defaults exits 0" '[ "$rc" -eq 0 ]'
check "defaults: has >500 lines (117 keys, multi-line comments)" '[ "$(wc -l <<<"$out")" -gt 500 ]'
check "defaults: every real key line is commented out (# key = ...)" \
	'! grep -E "^[a-z-]+ = " <<<"$out" | grep -qv "^#"'

echo "== config defaults --out PATH =="
OUTCONF="$D/sub/dir/momentum4.conf"
"$BIN" config defaults --out "$OUTCONF" >/dev/null
check "defaults --out creates parent dirs and the file" '[ -f "$OUTCONF" ]'
check "defaults --out content matches stdout form" \
	'[ "$(wc -l <"$OUTCONF")" = "$(wc -l <<<"$out")" ]'

echo "== config migrate: nothing to migrate (no legacy files) =="
out="$("$BIN" config migrate)"
rc=$?
check "migrate with nothing present exits 0" '[ "$rc" -eq 0 ]'
check "migrate: says nothing to migrate" 'grep -qi "nothing to migrate" <<<"$out"'

echo "== config migrate: dry-run leaves files untouched =="
mkdir -p "$XDG_CONFIG_HOME/momentum4"
LEGACY_M4="$XDG_CONFIG_HOME/momentum4/momentum4.conf"
LEGACY_APTX="$XDG_CONFIG_HOME/momentum4/aptx-adaptive.conf"
cat >"$LEGACY_M4" <<'EOF'
mode = auto
gesture-cooldown-ms = 250
EOF
cat >"$LEGACY_APTX" <<'EOF'
mode = ll1
rt-priority = 65
EOF
BEFORE_M4="$(cat "$LEGACY_M4")"
BEFORE_APTX="$(cat "$LEGACY_APTX")"
out="$("$BIN" config migrate)"
rc=$?
check "migrate dry-run exits 0" '[ "$rc" -eq 0 ]'
check "migrate dry-run: says DRY RUN" 'grep -q "DRY RUN" <<<"$out"'
check "migrate dry-run: momentum4.conf untouched" '[ "$(cat "$LEGACY_M4")" = "$BEFORE_M4" ]'
check "migrate dry-run: aptx-adaptive.conf untouched" '[ "$(cat "$LEGACY_APTX")" = "$BEFORE_APTX" ]'
check "migrate dry-run: no .v0.1 backup created" '[ ! -f "$LEGACY_M4.v0.1" ]'
check "migrate dry-run: reports 2 keys mapped from each file" \
	'[ "$(grep -c "keys migrated: 2" <<<"$out")" = 2 ]'

echo "== config migrate --apply =="
out="$("$BIN" config migrate --apply)"
rc=$?
check "migrate --apply exits 0" '[ "$rc" -eq 0 ]'
check "migrate --apply: backup momentum4.conf.v0.1 created" '[ -f "$LEGACY_M4.v0.1" ]'
check "migrate --apply: backup aptx-adaptive.conf.v0.1 created" '[ -f "$LEGACY_APTX.v0.1" ]'
check "migrate --apply: backup content == pre-migration original" \
	'[ "$(cat "$LEGACY_M4.v0.1")" = "$BEFORE_M4" ]'
check "migrate --apply: new momentum4.conf is sectioned" 'grep -q "^\[adaptive\]$" "$LEGACY_M4"'
check "migrate --apply: aptx-adaptive.conf itself left in place (still legacy-readable)" \
	'[ -f "$LEGACY_APTX" ] && grep -q "rt-priority" "$LEGACY_APTX"'

echo "== config show after migrate: effective values match the old ones, zero warnings =="
out="$("$BIN" config show 2>"$D/show.err")"
rc=$?
check "show after migrate exits 0" '[ "$rc" -eq 0 ]'
check "show after migrate: no warnings on stderr" '[ ! -s "$D/show.err" ]'
check "show after migrate: adaptive.mode == ll1 (from aptx-adaptive.conf)" \
	'grep -qE "^mode +\= ll1 " <<<"$out"'
check "show after migrate: adaptive.io-rt-priority == 65 (renamed from rt-priority)" \
	'grep -qE "^io-rt-priority +\= 65 " <<<"$out"'
check "show after migrate: daemon.mode == auto" \
	'grep -A3 "^\[daemon\]$" <<<"$out" | grep -qE "^mode +\= auto "'
check "show after migrate: control.gesture-cooldown-ms == 250" \
	'grep -qE "^gesture-cooldown-ms +\= 250 " <<<"$out"'

echo "== config migrate --apply again: idempotent, does not clobber the first backup =="
BACKUP_BEFORE="$(cat "$LEGACY_M4.v0.1")"
"$BIN" config migrate --apply >/dev/null
check "second migrate --apply: momentum4.conf.v0.1 unchanged (not re-created)" \
	'[ "$(cat "$LEGACY_M4.v0.1")" = "$BACKUP_BEFORE" ]'
check "second migrate --apply: momentum4.conf still validates clean" \
	'"$BIN" config validate "$LEGACY_M4" | grep -q "^OK: "'

echo "== [general] language = ru (common/m4-i18n.h M4_T()): config show/validate switch =="
# The ONE end-to-end exercise of the ru branch of M4_T() through the real
# compiled binary (item 4 of the language-support task) -- everywhere else
# in this script implicitly assumes the default (no [general] language key
# set -> "en").
RUCONF="$D/ru-lang-momentum4.conf"
cat >"$RUCONF" <<'EOF'
[general]
language = ru
EOF
out="$(MOMENTUM4_CONFIG="$RUCONF" "$BIN" config show)"
rc=$?
check "show (ru) exits 0" '[ "$rc" -eq 0 ]'
check "show (ru): section header stays [general] (not translated)" 'grep -q "^\[general\]$" <<<"$out"'
out="$(MOMENTUM4_CONFIG="$RUCONF" "$BIN" config validate "$D/does-not-exist.conf" 2>&1)"
rc=$?
check "validate (ru) on a missing explicit PATH exits 1" '[ "$rc" -eq 1 ]'
check "validate (ru) on a missing explicit PATH says so, in Russian" 'grep -qi "не найден" <<<"$out"'

echo
echo "$((CHECKS - 0)) checks run"
if [ "$FAIL" -eq 0 ]; then
	echo "OK: momentum4 config show/validate/defaults/migrate ($CHECKS checks)"
else
	echo "FAILED"
fi
exit "$FAIL"
