/* common/fuzz/fuzz_m4_conf.c -- libFuzzer entry point for common/m4-conf.c:
 * a clang-based, offline, time-bounded fuzz harness for the config parser.
 *
 * Exercises the ONE function in common/ that ever sees untrusted bytes:
 * m4_conf_load_buffer() (m4_conf_load_file() is the same parser, just fed
 * from a FILE* one line at a time -- see m4-conf.c's own comment on why
 * they share a line handler). Fuzzer input is NOT guaranteed NUL-terminated
 * or size-bounded, so this harness copies it into a fixed, NUL-terminated
 * local buffer first (matching how every real caller already has a bounded,
 * NUL-terminated buffer -- fopen()+fgets() or an INI file read whole) --
 * this deliberately does NOT fuzz "what if the input is 4 GiB and not
 * NUL-terminated", which is not m4_conf_load_buffer()'s contract (see
 * m4-conf.h: "Parses `text` (NUL-terminated)").
 *
 * Build (needs clang, for -fsanitize=fuzzer -- see build.sh for what this
 * repo actually has installed and the ASan+manual-driver fallback):
 *   clang -std=c99 -g -O1 -fsanitize=fuzzer,address,undefined \
 *     -I.. fuzz_m4_conf.c ../m4-conf.c -lm -o fuzz_m4_conf
 *   ./fuzz_m4_conf -max_total_time=60 corpus/
 *
 * Every crash libFuzzer finds is a real bug in m4-conf.c (this file has no
 * additional state/logic of its own beyond the copy-to-fixed-buffer step,
 * so a crash cannot be an artifact of the harness itself) -- see m4-conf.h's
 * "never fatal" contract: an assert()/abort() reachable from arbitrary file
 * content would itself be exactly the kind of bug this exists to catch
 * (m4_conf_init()'s registry-size abort() is intentionally NOT reachable
 * from any fuzzed input -- it only fires on a compile-time registry/struct
 * mismatch, never on parsed content).
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../m4-conf.h"

/* Generous but bounded -- matches this project's "no malloc" fixed-buffer
 * style; anything libFuzzer generates beyond this is simply truncated
 * before parsing (still deterministic, still a valid NUL-terminated C
 * string, so m4_conf_load_buffer()'s own contract is honoured either way). */
#define FUZZ_MAX_INPUT 16384

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	static char text[FUZZ_MAX_INPUT];
	m4_conf_t c;
	size_t n = size < sizeof(text) - 1 ? size : sizeof(text) - 1;

	memcpy(text, data, n);
	text[n] = '\0';

	m4_conf_init(&c);
	m4_conf_load_buffer(&c, text, "fuzz");

	/* Exercise every accessor for every registry key (not just whatever
	 * happened to parse) -- catches a corrupt/garbage `value`/`source`
	 * that m4_conf_load_buffer() itself did not reject but that a later
	 * accessor mishandles (e.g. strtol/strtod on a string that is not
	 * actually NUL-terminated within M4_CONF_MAX_VALUE -- ASan's
	 * redzones around `text` and `c` would catch that either way, but
	 * doing it explicitly makes the fuzz target's intent self-evident). */
	{
		size_t i;

		for (i = 0; i < m4_conf_registry_count; i++) {
			(void)m4_conf_get_bool_i(&c, (int)i, false);
			(void)m4_conf_get_int_i(&c, (int)i, 0);
			(void)m4_conf_get_double_i(&c, (int)i, 0.0);
			(void)m4_conf_get_string_i(&c, (int)i);
		}
	}

	{
		char show[8192];

		(void)m4_conf_format_show(&c, show, sizeof(show));
	}

	/* Also fuzz the placeholder expander directly with the same bytes --
	 * it is reachable from file content (STRING/ENUM values) but has its
	 * own bracket/pointer-arithmetic logic worth exercising on its own. */
	{
		char expanded[1024];

		m4_conf_expand(text, expanded, sizeof(expanded));
	}

	return 0;
}
