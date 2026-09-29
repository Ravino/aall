/* util/legacy_conf.h -- shared "old flat momentum4.conf/aptx-adaptive.conf
 * -> new [section] m4_conf_t" mapping, factored out of cli/config.c
 * so BOTH `momentum4 config migrate` (cli/config.c, an explicit,
 * one-shot admin command) AND the daemon's own normal startup config load
 * (util/config.c's m4_config(): the old flat momentum4.conf
 * is still read, with a warning) share exactly one legacy-key table and one
 * parser -- a value that migrate maps one way must be the SAME value the
 * daemon itself would have used before migrate ever ran.
 *
 * Kept out of common/ on purpose, same reasoning m4-conf.h's own "common/
 * placement" note already gives for cli/config.c's legacy handling: the old
 * flat grammar and its key set are daemon/plugin history, not something the
 * SPA plugin's own build needs to know about.
 */
#ifndef MOMENTUM4_UTIL_LEGACY_CONF_H
#define MOMENTUM4_UTIL_LEGACY_CONF_H

#include <stddef.h>

#include "../../../common/m4-conf.h"

#ifdef __cplusplus
extern "C" {
#endif

struct m4_legacy_map_entry {
	const char *legacy_key;
	const char *new_section;
	const char *new_key;
};

/* util/settings.c's OLD flat momentum4.conf grammar -> new [section] key
 * . */
extern const struct m4_legacy_map_entry m4_legacy_momentum4_conf_map[];
extern const size_t m4_legacy_momentum4_conf_map_count;

/* module's OLD flat aptx-adaptive.conf grammar (m4-adaptive-
 * config.h) -> new [section] key. "rt-priority" -> "io-rt-priority": the
 * one deliberate rename m4-conf-keys.def's own header comment documents. */
extern const struct m4_legacy_map_entry m4_legacy_aptx_adaptive_conf_map[];
extern const size_t m4_legacy_aptx_adaptive_conf_map_count;

/* Reads `path` (old flat "key = value" format: '#'/';' comments,
 * case-insensitive keys, '_'==' -', optional quotes) and, for every line
 * whose key is found in `map` (n_map entries), applies it to `out` via
 * m4_conf_set() under the mapped (section,key), with `source_label`
 * "<source_prefix>:<path>:<line>" (`source_prefix` lets callers tell apart,
 * in m4_conf_t's own .source field, an explicit `config migrate` run --
 * cli/config.c passes "migrate" -- from a value applied automatically on
 * every startup from a not-yet-migrated file -- util/config.c passes
 * "legacy"). Unmapped keys are reported via `on_unmapped(key, value,
 * userdata)` (may be NULL). Returns the number of lines successfully
 * mapped (>=0), or -1 if the file does not exist (ENOENT -- not an error)
 * or -2 on any other read error. Detects "this file already has [section]
 * headers" (i.e. is NOT actually the old flat format -- already migrated,
 * or hand-edited) and returns -3 without applying anything in that case. */
int m4_legacy_conf_load(const char *path, const struct m4_legacy_map_entry *map, size_t n_map, m4_conf_t *out,
			 const char *source_prefix,
			 void (*on_unmapped)(const char *key, const char *value, void *ud), void *ud);

#ifdef __cplusplus
}
#endif

#endif /* MOMENTUM4_UTIL_LEGACY_CONF_H */
