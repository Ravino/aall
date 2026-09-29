/* util/build_info.h -- reads spa/plugins/bluez5/BUILD-INFO (written by
 * module/prepare-tree.sh,
 * for the "version guard"). Plain `key = value` lines, same trivial grammar
 * prepare-tree.sh itself documents ("easy for both a human and a future
 * small C parser to read") -- this IS that future parser. No malloc, no
 * PipeWire dependency: usable from both `momentum4 doctor` and `momentum4
 * wp-env` (the latter runs as a systemd ExecStartPre, before/without a
 * PipeWire connection -- see cli/wp_env.h's own top comment for why that
 * matters).
 */
#ifndef MOMENTUM4_UTIL_BUILD_INFO_H
#define MOMENTUM4_UTIL_BUILD_INFO_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define M4_BUILD_INFO_STR_LEN 96

struct m4_build_info {
	char pipewire_version[M4_BUILD_INFO_STR_LEN];		/* the PipeWire this module was built against */
	char pipewire_tarball_sha256[M4_BUILD_INFO_STR_LEN];
	char patches_sha256[M4_BUILD_INFO_STR_LEN];
	char m4_src_sha256[M4_BUILD_INFO_STR_LEN];
	char prepared_at_utc[M4_BUILD_INFO_STR_LEN];
	long patches_count;		/* -1 if absent/unparsable */
	long m4_src_files_count;	/* -1 if absent/unparsable */
};

/* Reads `path` (typically "<spa-plugin-dir>/bluez5/BUILD-INFO") into `out`.
 * Unknown keys are ignored (forward-compatible with a future prepare-tree.sh
 * adding fields); a missing key leaves the corresponding field as an empty
 * string / -1. Returns true if the file could be opened and read at all
 * (even if some/all keys were missing -- callers decide what "incomplete"
 * means for their purpose), false if the file does not exist or cannot be
 * opened/read (`out` is still fully zeroed in that case, never left
 * partially initialized). */
bool m4_build_info_load(const char *path, struct m4_build_info *out);

#ifdef __cplusplus
}
#endif

#endif /* MOMENTUM4_UTIL_BUILD_INFO_H */
