/* common/m4-conf.h -- shared momentum4.conf parser + key registry
 * .
 *
 * ONE piece of code, linked into BOTH the daemon (via
 * meson) and the aptX Adaptive SPA plugin (module/, copied into
 * m4-src/ -- see the "common/ placement" note at
 * the bottom of this file). Because of the second consumer this file is
 * held to a stricter bar than most of this repo's C:
 *
 *   - NO malloc/free/realloc anywhere, ever. Every m4_conf_t is one fixed-
 *     size struct a caller owns (stack, static, or embedded in another
 *     struct) -- safe to construct on a SPA plugin's RT-adjacent config
 *     thread without worrying about allocator contention/failure.
 *   - NO dependency beyond the C99 standard library (<stdio.h> FILE* for
 *     reading files is the only libc surface beyond string/stdlib/ctype).
 *     Specifically: no <spa/...>, no <pipewire/...>, no libsystemd, no
 *     gesture.h/state.h/anything daemon-specific -- see settings.h in
 *     daemon for that (layered ON TOP of this file by the daemon,
 *     not replaced by it; settings.h's OLD flat momentum4.conf format is
 *     read by this file too, but only as "legacy", see m4_conf_load_legacy_flat()).
 *   - Never fatal. A malformed file, an unknown section, an unknown key, an
 *     out-of-range value -- all become one entry in `warnings` and the
 *     previous (lower-priority-layer, ultimately the compiled-in default)
 *     value is kept. This mirrors util/settings.c's existing contract
 *     ("a typo in the config must not keep the daemon from starting").
 *
 * File format :
 *   # comment (whole line only; no trailing "key = value # comment" --
 *   #  unlike the legacy flat format, deliberately simpler to parse and to
 *   #  round-trip losslessly out of gen-conf.c)
 *   ; comment (same)
 *   [section]
 *   key = value
 *   key=value
 *   key = "value with leading/trailing spaces preserved"
 * Blank lines ignored. Keys are case-insensitive, '_' and '-' interchange
 * (same convention as util/settings.c). A `key = value` line before any
 * `[section]` header applies to an implicit section named "" -- rejected as
 * "unknown key" by m4_conf_load_buffer() (every real key in the registry
 * lives in a named section), but this keeps the parser itself simple/total
 * (no special no-section-yet error path).
 *
 * Placeholder substitution, applied to STRING/ENUM values only, after
 * validation, once, at the time a value is stored :
 *   ${XDG_RUNTIME_DIR} ${XDG_CONFIG_HOME} ${XDG_DATA_HOME} ${XDG_STATE_HOME}
 *   ${HOME}             -- expanded from the process environment; a
 *                          variable that is unset or empty expands to "".
 *   %h                  -- systemd-specifier style alias for $HOME (used by
 *                          systemd unit ExecStart= lines already in this
 *                          repo, e.g. momentum4d.service's
 *                          "%h/.local/bin/momentum4" -- kept consistent
 *                          here so a value copy-pasted from/to a .service
 *                          file needs no translation).
 *   @prefix@ @libdir@ @libexecdir@ @datadir@ @spa_system_dir@
 *   @systemd_user_unit_dir@
 *                       -- build-time install paths. NOT environment
 *                          variables: resolved from the M4_CONF_LIBDIR/
 *                          M4_CONF_LIBEXECDIR/M4_CONF_DATADIR/M4_CONF_PREFIX/
 *                          M4_CONF_SPA_SYSTEM_DIR/
 *                          M4_CONF_SYSTEMD_USER_UNIT_DIR preprocessor
 *                          defines (meson configures these; m4-conf.c
 *                          supplies conservative /usr/local fallbacks --
 *                          "" for spa_system_dir, see its own #ifndef --
 *                          when a define is absent, so this file also
 *                          builds and runs standalone, e.g. under the
 *                          fuzzer/unit tests in common/tests, common/fuzz).
 *
 * Layering :
 *   1. compiled-in defaults (this file's registry, m4-conf-keys.def)
 *   2. @datadir@/momentum4/momentum4.conf   (package-provided reference copy)
 *   3. /etc/momentum4/momentum4.conf        (system administrator)
 *   4. $XDG_CONFIG_HOME/momentum4/momentum4.conf,
 *        else $HOME/.config/momentum4/momentum4.conf   (user)
 *   $MOMENTUM4_CONFIG, if set and non-empty, REPLACES step 4's path (does
 *   not add a 5th layer) -- same override contract util/config.c's
 *   m4_config_file_path() already documents for the daemon.
 *   $MOMENTUM4_DATADIR, if set and non-empty, replaces step 2's @datadir@
 *   prefix (mirrors util/wp_overlay.c's own test-only override of the same
 *   name); $MOMENTUM4_ETC_CONF, if set and non-empty, replaces step 3's
 *   whole path (/etc/momentum4/momentum4.conf is a plain literal, not
 *   @-substituted, so it needs its own override, unlike step 2). Both are
 * test-only (without them, `m4_conf_load_layers()`
 *   silently picks up whatever real @datadir@/etc/HOME copy happens to
 *   exist on the machine running the test suite, e.g. right after `make
 *   install` on the same box) -- never documented for real use, same as
 *   $MOMENTUM4_MAC/$MOMENTUM4_BT_ADAPTER in daemon/src/util/config.h.
 *   m4_conf_load_layers() performs exactly this; m4_conf_load_file() loads
 *   one arbitrary file on top of whatever `c` already holds, for callers
 *   that want to compose their own layer list (tests, `config migrate`).
 *
 * Legacy compatibility: the OLD flat (no-sections) momentum4.conf
 * (util/settings.h's grammar) and the OLD aptx-adaptive.conf (m4-adaptive-
 * config.h's grammar) are both "key = value" with no [section] -- this file
 * can read either via m4_conf_load_legacy_flat(), which needs an explicit
 * table telling it which section each bare key belongs to in the NEW
 * format (see daemon/src/util/legacy_conf.h, built on top of
 * this file, for the actual table -- kept out of common/ on purpose: the
 * legacy key sets are daemon/plugin history, not something the SPA plugin
 * build needs to know about. m4_conf_load_legacy_flat() itself is generic:
 * "flat key -> (section, key)" is just a caller-supplied lookup callback).
 *
 * common/ placement: this directory is consumed two different ways,
 * deliberately NOT identical, because the two consumers have unrelated
 * build systems:
 *   - daemon (plain meson project): daemon/meson.build references
 *     common/m4-conf.c and common/gen-conf.c by a relative source path
 *     ('../common/m4-conf.c') -- no subproject, no installed library.
 *     The top-level meson.build also pulls common/ in as a real
 *     subproject(), but only to build one canonical gen-conf binary for
 *     data/momentum4.conf; the daemon keeps its own relative-path build
 *     of the same sources so it still builds standalone.
 *   - SPA plugin (module, built INSIDE PipeWire's own meson project via
 *     prepare-tree.sh's patch/copy pipeline, since the plugin is pinned
 *     to a specific PipeWire release): prepare-tree.sh copies
 *     common/m4-conf.{c,h} and common/m4-conf-keys.def verbatim into
 *     m4-src/ before patch 0005 is applied, and the patch adds m4-conf.c
 *     to bluez5's meson.build sources list. A real meson subproject() was
 *     considered and rejected for the plugin specifically: PipeWire's own
 *     build does not go through an intermediate project this repo
 *     controls, so a subproject() dependency would need PipeWire's own
 *     meson.build patched to know about an external subproject directory
 *     outside its own source tree -- a bigger, riskier patch than one
 *     more prepare-tree.sh copy step for a single-TU dependency-free
 *     library.
 */
#ifndef MOMENTUM4_COMMON_M4_CONF_H
#define MOMENTUM4_COMMON_M4_CONF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h> /* FILE*, only for m4_conf_write_reference() below */

#ifdef __cplusplus
extern "C" {
#endif

#define M4_CONF_MAX_SECTION 24
#define M4_CONF_MAX_KEY 40
#define M4_CONF_MAX_VALUE 384
#define M4_CONF_MAX_SOURCE 384
#define M4_CONF_MAX_WARNINGS 64
#define M4_CONF_MAX_WARNING_MSG 200

typedef enum {
	M4_CONF_T_BOOL = 0,
	M4_CONF_T_INT,
	M4_CONF_T_DOUBLE,
	M4_CONF_T_STRING,
	M4_CONF_T_ENUM, /* stored/validated as string, against `enum_values` */
} m4_conf_type_t;

/* One row of the compiled-in registry (m4-conf-keys.def via M4_CONF_KEY()).
 * `min`/`max` only apply to M4_CONF_T_INT/M4_CONF_T_DOUBLE (ignored
 * otherwise). `enum_values` only applies to M4_CONF_T_ENUM: '|'-separated,
 * e.g. "ll0|ll1", NULL for every other type. `default_value` is always the
 * canonical text form (e.g. "on"/"off" for bool, "-15" for int) -- this is
 * also exactly what gen-conf.c prints as the shipped default.
 *
 * comment_en/comment_ru: two language variants of the same key
 * description (see common/m4-i18n.h) -- kept as two separate fields,
 * never merged through M4_T() here, because this whole array is a
 * file-scope (static) initializer and `m4_lang == ...` is not a constant
 * expression. Callers pick one with M4_T(def->comment_en, def->comment_ru)
 * at the point they print it (m4-conf.c's m4_conf_print_comment_block()
 * callers, doctor.c, config.c). comment_ru is the authoritative original
 * text; comment_en is what ships by default ([general] language = en). */
typedef struct {
	const char *section;
	const char *key;
	m4_conf_type_t type;
	const char *default_value;
	double min;
	double max;
	const char *enum_values;
	const char *comment_en; /* one or more lines, '\n'-separated; gen-conf.c
				  * prints each as its own "# " line above the key */
	const char *comment_ru; /* same, Russian */
} m4_conf_key_def_t;

extern const m4_conf_key_def_t m4_conf_registry[];
extern const size_t m4_conf_registry_count;

/* Index into m4_conf_t.entries[]/m4_conf_registry[] -- returned by
 * m4_conf_find() so hot callers (e.g. the SPA plugin's per-codec_init
 * re-read) can look a key up once and reuse the index instead of
 * re-scanning the registry by name on every access. -1 if not found. */
int m4_conf_find(const char *section, const char *key);

typedef struct {
	char value[M4_CONF_MAX_VALUE];   /* fully validated, substituted text */
	char source[M4_CONF_MAX_SOURCE]; /* "default", "env:VAR", or a file path */
	int source_line;                 /* 0 if not from a file line (default/env) */
} m4_conf_entry_t;

typedef struct {
	/* entries[i] corresponds 1:1 to m4_conf_registry[i] -- see
	 * m4_conf_find(). Sized to the registry at compile time, so a future
	 * registry grows the struct, never overflows it. */
	m4_conf_entry_t entries[/* m4_conf_registry_count, see m4-conf.c */ 256];

	int n_warnings;         /* total, saturates at M4_CONF_MAX_WARNINGS */
	int n_files_loaded;     /* how many of the 3 optional file layers existed */
	char warnings[M4_CONF_MAX_WARNINGS][M4_CONF_MAX_WARNING_MSG];
} m4_conf_t;

/* Resets `c` to the compiled-in defaults (m4_conf_registry). Always
 * succeeds; `c` is fully valid immediately afterwards (every accessor
 * below returns a sane value even if nothing is ever loaded on top). */
void m4_conf_init(m4_conf_t *c);

/* Parses `text` (NUL-terminated) as one INI layer and applies it ON TOP of
 * whatever `c` already holds -- call m4_conf_init() first. `source_label`
 * is stored verbatim in the matching entries' `.source` (e.g. a file path,
 * or "env:MOMENTUM4_MAC" for programmatic single-key overrides via
 * m4_conf_set()). Every problem (unknown section, unknown key, value
 * fails validation for its declared type/range/enum) appends one line to
 * `c->warnings` (capped at M4_CONF_MAX_WARNINGS; further ones are still
 * counted in n_warnings but not stored) and otherwise changes nothing for
 * that one key -- never fatal, matches util/settings.c's contract. */
void m4_conf_load_buffer(m4_conf_t *c, const char *text, const char *source_label);

/* m4_conf_load_buffer() on the contents of `path`. Returns 1 if the file
 * existed and was read (even if it produced warnings), 0 if it does not
 * exist (ENOENT -- normal, not a warning: most of the 3 optional layers
 * will not exist on a given machine), -1 on any other read error (logged
 * as one warning, e.g. "conf %s: unreadable (%s)"). */
int m4_conf_load_file(m4_conf_t *c, const char *path);

/* m4_conf_init() + the full 4-layer resolution documented at the top of
 * this file (defaults < @datadir@ < /etc < $XDG_CONFIG_HOME/$HOME, with
 * $MOMENTUM4_CONFIG replacing the last one if set). Returns the number of
 * the 3 optional file layers that actually existed on disk (0..3) --
 * never negative, this function does not fail: an absent/unreadable file
 * at any layer is just skipped (with a warning already appended for read
 * errors other than ENOENT, same as m4_conf_load_file()). */
int m4_conf_load_layers(m4_conf_t *c);

/* Sets ONE key's value programmatically (e.g. a test fixture, or a future
 * `-o section.key=value` CLI flag) -- goes through the exact same
 * validation as a file line would (type/range/enum), appends a warning
 * and leaves the previous value untouched on failure. `source_label`
 * defaults to "env:<KEY>" style callers may pass explicitly; NULL means
 * "override". Returns true on success. */
bool m4_conf_set(m4_conf_t *c, const char *section, const char *key, const char *value,
		  const char *source_label);

/* Typed accessors. Every one of them is total: an unknown (section,key)
 * pair (a programmer error -- every real key is in the compiled-in
 * registry) returns `fallback` and asserts nothing (fails soft, matching
 * this file's "never fatal" spirit even for our own bugs -- an SPA plugin
 * RT-adjacent thread must not be able to crash on a typo introduced by a
 * refactor). Values ALWAYS come from `c->entries` (i.e. already resolved
 * through the layering) -- there is no separate "read the registry
 * default" path once `c` has been through m4_conf_init(). */
bool m4_conf_get_bool(const m4_conf_t *c, const char *section, const char *key, bool fallback);
long m4_conf_get_int(const m4_conf_t *c, const char *section, const char *key, long fallback);
double m4_conf_get_double(const m4_conf_t *c, const char *section, const char *key, double fallback);
/* Returns c->entries[idx].value directly (no copy) -- valid as long as `c`
 * is. NULL on an unknown (section,key), never on a known one (a known key
 * with type STRING/ENUM always has *some* value, at minimum its default). */
const char *m4_conf_get_string(const m4_conf_t *c, const char *section, const char *key);

/* Same four, but by registry index (see m4_conf_find()) -- for hot call
 * sites that already resolved the index once. No bounds/type checking
 * beyond an assert-like silent fallback, same contract as the by-name
 * versions above. */
bool m4_conf_get_bool_i(const m4_conf_t *c, int idx, bool fallback);
long m4_conf_get_int_i(const m4_conf_t *c, int idx, long fallback);
double m4_conf_get_double_i(const m4_conf_t *c, int idx, double fallback);
const char *m4_conf_get_string_i(const m4_conf_t *c, int idx);

/* true for 1/yes/on/true, false for 0/no/off/false (case-insensitive);
 * *ok=false (if ok != NULL) if neither -- same rule set as
 * util/settings.c's m4_settings_parse_bool(), duplicated here rather than
 * shared because settings.c depends on this file's eventual daemon-side
 * caller, not the other way around (common/ has no daemon dependency). */
bool m4_conf_parse_bool(const char *v, bool *ok);

/* Expands ${VAR}/%h/@word@ placeholders from `in` into `out` (size
 * `out_cap`), per the rules documented at the top of this file. Exposed
 * separately from the loader (used by gen-conf.c to show BOTH the raw
 * default text with placeholders intact in comments, and the expanded
 * form) and by common/tests. Truncates (always NUL-terminated) rather
 * than failing if `out` is too small -- matches this file's fixed-buffer,
 * never-fatal style; callers needing to detect truncation can compare
 * strlen(out) against their own expectation. */
void m4_conf_expand(const char *in, char *out, size_t out_cap);

/* Prints a human-readable "effective config" listing (section, key, value,
 * source, source_line if any) to `out` -- backs `momentum4 config show`.
 * One line per registry key, in registry order (i.e. grouped by section,
 * same order gen-conf.c uses). Never fails (truncates like m4_conf_expand
 * if `out` is too small; check the return value, which is the number of
 * bytes that WOULD have been written, snprintf-style, to detect that). */
int m4_conf_format_show(const m4_conf_t *c, char *out, size_t out_cap);

/* Writes the fully-commented reference file (every key, its comment, type
 * hint and default value, all `# `-commented) to `out` -- the single
 * source of truth behind BOTH common/gen-conf.c (the build-time
 * @datadir@/momentum4/momentum4.conf generator) and `momentum4 config
 * defaults` (daemon/src/cli/config.c), so the two can never drift.
 * Pure stdio (fprintf to `out`) -- no path handling, no malloc; callers
 * that need atomic-write-to-a-path semantics (both of the above do) wrap
 * this themselves. */
void m4_conf_write_reference(FILE *out);

#ifdef __cplusplus
}
#endif

#endif /* MOMENTUM4_COMMON_M4_CONF_H */
