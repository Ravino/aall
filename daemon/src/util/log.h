/* util/log.h -- journal logging (via sd_journal_print).
 * Every momentum4 process logs to the systemd journal under the identifier
 * "momentum4" (set via getenv/prctl by the caller's unit file in later
 * phases; here we just tag each message so `journalctl -t momentum4` finds
 * it even when run interactively, unit-less, from a terminal). Warnings and
 * errors are always mirrored to stderr too, so a plain interactive
 * `momentum4 status` run shows problems immediately without needing
 * `journalctl` (status works without the daemon). */
#ifndef MOMENTUM4_UTIL_LOG_H
#define MOMENTUM4_UTIL_LOG_H

#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

enum m4_log_level {
	M4_LOG_DEBUG,
	M4_LOG_INFO,
	M4_LOG_WARN,
	M4_LOG_ERR,
};

__attribute__((format(printf, 2, 3)))
void m4_log(enum m4_log_level level, const char *fmt, ...);

void m4_logv(enum m4_log_level level, const char *fmt, va_list ap);

#ifdef __cplusplus
}
#endif

#endif /* MOMENTUM4_UTIL_LOG_H */
