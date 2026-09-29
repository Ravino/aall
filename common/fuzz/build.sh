#!/usr/bin/env bash
# common/fuzz/build.sh -- builds the m4-conf.c fuzz target. Tries real
# libFuzzer via clang first (best bug-finding per
# CPU-second, coverage-guided); falls back to the dependency-free
# standalone_driver.c (plain gcc + ASan/UBSan, brute-force mutation, see
# that file's own header comment for why and its exact scope) if no clang
# is found (`which clang clang-14..19` empty).
#
# usage: common/fuzz/build.sh [output-dir, default: build next to this script]
#        common/fuzz/build.sh --run [SECONDS]   # build (if needed) + run
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="$HERE/build"
RUN=0
SECONDS_BUDGET=60

while [ $# -gt 0 ]; do
	case "$1" in
	--run)
		RUN=1
		shift
		if [ $# -gt 0 ] && [[ "$1" =~ ^[0-9]+$ ]]; then
			SECONDS_BUDGET="$1"
			shift
		fi
		;;
	*)
		OUT="$1"
		shift
		;;
	esac
done
mkdir -p "$OUT"

CLANG=""
for c in clang clang-19 clang-18 clang-17 clang-16 clang-15 clang-14; do
	if command -v "$c" >/dev/null 2>&1; then
		CLANG="$c"
		break
	fi
done

if [ -n "$CLANG" ]; then
	echo "build.sh: using $CLANG (real libFuzzer, -fsanitize=fuzzer,address,undefined)"
	"$CLANG" -std=c99 -g -O1 -fsanitize=fuzzer,address,undefined \
		-I"$HERE/.." -o "$OUT/fuzz_m4_conf" \
		"$HERE/fuzz_m4_conf.c" "$HERE/../m4-conf.c" "$HERE/../m4-i18n.c" -lm
	BIN="$OUT/fuzz_m4_conf"
	RUN_CMD=("$BIN" "-max_total_time=$SECONDS_BUDGET" "$HERE/corpus")
else
	echo "build.sh: no clang found (checked clang, clang-14..19) -- falling back to" \
		"standalone_driver.c (gcc + ASan/UBSan, no coverage guidance, see its own" \
		"header comment). Install clang for real libFuzzer coverage" \
		"(not done automatically by this script)."
	gcc -std=c99 -Wall -Wextra -D_POSIX_C_SOURCE=200809L -g -O1 -fsanitize=address,undefined \
		-I"$HERE/.." -o "$OUT/fuzz_m4_conf_standalone" \
		"$HERE/standalone_driver.c" "$HERE/fuzz_m4_conf.c" "$HERE/../m4-conf.c" "$HERE/../m4-i18n.c" -lm
	BIN="$OUT/fuzz_m4_conf_standalone"
	RUN_CMD=("$BIN" "--seconds" "$SECONDS_BUDGET" "$HERE/corpus")
fi

echo "build.sh: built $BIN"
if [ "$RUN" = "1" ]; then
	echo "build.sh: running for ${SECONDS_BUDGET}s: ${RUN_CMD[*]}"
	"${RUN_CMD[@]}"
fi
