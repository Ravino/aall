/* util/capi_check.h -- shared "find data/capi-sha256.txt, look a hash up in
 * it" logic, used by BOTH `momentum4 setup --capi` (cli/setup.c) and
 * `momentum4 doctor` (cli/doctor.c).
 */
#ifndef MOMENTUM4_UTIL_CAPI_CHECK_H
#define MOMENTUM4_UTIL_CAPI_CHECK_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Tries a short, fixed list of locations for data/capi-sha256.txt (no
 * installed-data-file story exists yet): $XDG_DATA_HOME (or
 * $HOME/.local/share)/momentum4/capi-sha256.txt, /usr/share/momentum4/
 * capi-sha256.txt, and (dev builds only) this checkout's own data/
 * directory. Returns `buf` (the first one that exists as a regular file),
 * NULL if none do. */
const char *m4_capi_sha256_list_path(char *buf, size_t buf_cap);

/* Looks `hex_hash` (case-insensitive) up in `list_path` (data/capi-
 * sha256.txt's own grammar: '#' comments, blank lines, "sha256<ws>
 * rest..." data lines). On a match, `out_desc` (capacity `out_desc_cap`,
 * may be NULL/0) is filled with the rest of that line (trimmed,
 * best-effort, may be truncated). Returns 1 on a match, 0 if not found, -1
 * if `list_path` could not be read at all. */
int m4_capi_sha256_lookup(const char *list_path, const char *hex_hash, char *out_desc, size_t out_desc_cap);

#ifdef __cplusplus
}
#endif

#endif /* MOMENTUM4_UTIL_CAPI_CHECK_H */
