#include "log.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <systemd/sd-journal.h>

static int level_to_priority(enum m4_log_level level)
{
	switch (level) {
	case M4_LOG_DEBUG: return LOG_DEBUG;
	case M4_LOG_INFO: return LOG_INFO;
	case M4_LOG_WARN: return LOG_WARNING;
	case M4_LOG_ERR: default: return LOG_ERR;
	}
}

static const char *level_prefix(enum m4_log_level level)
{
	switch (level) {
	case M4_LOG_DEBUG: return "debug";
	case M4_LOG_INFO: return "info";
	case M4_LOG_WARN: return "warn";
	case M4_LOG_ERR: default: return "error";
	}
}

void m4_logv(enum m4_log_level level, const char *fmt, va_list ap)
{
	char buf[1024];
	va_list ap_copy;
	bool to_stderr;

	va_copy(ap_copy, ap);
	vsnprintf(buf, sizeof(buf), fmt, ap_copy);
	va_end(ap_copy);

	/* One %s, not a re-parsed format string -- `buf` may legitimately
	 * contain '%' (e.g. a profile description) after vsnprintf. */
	sd_journal_print(level_to_priority(level), "%s", buf);

	to_stderr = (level == M4_LOG_WARN || level == M4_LOG_ERR) ||
		(level == M4_LOG_INFO && isatty(STDERR_FILENO)) ||
		(level == M4_LOG_DEBUG && getenv("MOMENTUM4_DEBUG") != NULL);
	if (to_stderr)
		fprintf(stderr, "momentum4: %s: %s\n", level_prefix(level), buf);
}

void m4_log(enum m4_log_level level, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	m4_logv(level, fmt, ap);
	va_end(ap);
}
