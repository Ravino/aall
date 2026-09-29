/* util/conf_upsert.h -- minimal, surgical "set these few keys, touch
 * nothing else" editor for momentum4.conf:
 * `momentum4 install` needs to pin `[device] address` and `[helper]
 * qemu/loader/capi` to the values THIS build actually resolved, so the
 * SPA plugin -- which may be compiled against a different PipeWire
 * install prefix and therefore have different compiled-in defaults for
 * those same keys.1 vs. this build's own
 * M4_CONF_{PREFIX,LIBDIR,...} -- reads the SAME explicit values, not its
 * own possibly-mismatched fallback).
 *
 * Deliberately NOT part of common/m4-conf.c (that parser is read-only by
 * design, shared with the RT-adjacent SPA plugin, and must stay
 * malloc-free/side-effect-free, per m4-conf.h's own contract) and
 * deliberately NOT cli/config.c's `migrate` machinery (that one always
 * writes a FRESH file from an in-memory m4_conf_t -- fine when there is
 * no existing sectioned file yet, wrong here: `install` may run against
 * an already-customized momentum4.conf and must never discard unrelated
 * keys, comments, or ordering).
 *
 * Grammar handled is exactly common/m4-conf.h's own INI subset
 * (`[section]`, `key = value`, `#`/`;` full-line comments) -- nothing
 * fancier (no multi-line values, matching the parser this pairs with).
 */
#ifndef MOMENTUM4_UTIL_CONF_UPSERT_H
#define MOMENTUM4_UTIL_CONF_UPSERT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct m4_conf_upsert_kv {
	const char *section;
	const char *key;
	const char *value; /* NULL or "" -- skipped (nothing to pin) */
};

/* Reads `path` if it exists (missing file treated as empty), applies each
 * kv in `kvs` (in order: first matching "key = " line under the right
 * [section] is replaced in place; if the section exists but the key does
 * not, the key is appended right after the section header; if the
 * section does not exist at all, a new "\n[section]\nkey = value\n" block
 * is appended at end of file), and writes the result back atomically
 * (temp file in the same directory + rename, same pattern
 * common/gen-conf.c already uses). Creates parent directories as needed.
 *
 * Returns the number of keys actually changed (0 if every kv already had
 * that exact value -- the file is NOT rewritten in that case, so mtime-based
 * caches, e.g. the SPA plugin's own `[hfp]` mtime cache, see nothing), or
 * -1 on error (message in err, if non-NULL). */
int m4_conf_upsert(const char *path, const struct m4_conf_upsert_kv *kvs, size_t n_kvs,
		    char *err, size_t err_len);

#ifdef __cplusplus
}
#endif

#endif /* MOMENTUM4_UTIL_CONF_UPSERT_H */
