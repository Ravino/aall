/* core/state.c -- see state.h. */
#include "state.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define M4_STATE_PATH_MAX 768

static int build_path(const char *runtime_dir, const char *leaf, char *out, size_t out_cap)
{
	int n;

	if (runtime_dir == NULL || runtime_dir[0] == '\0')
		return -1;
	n = snprintf(out, out_cap, "%s/%s", runtime_dir, leaf);
	if (n < 0 || (size_t)n >= out_cap)
		return -1;
	return 0;
}

static int ensure_dir(const char *runtime_dir)
{
	if (mkdir(runtime_dir, 0700) == 0 || errno == EEXIST)
		return 0;
	return -1;
}

/* Atomic write: tmp file in the same directory (so rename() is on the same
 * filesystem), then rename() over
 * the target. `content` is written verbatim (caller supplies trailing
 * newlines per line, same as the KEY=value writers below). */
static int write_file_atomic(const char *path, const char *content)
{
	char tmp[M4_STATE_PATH_MAX];
	FILE *f;
	int n;

	n = snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", path, (long)getpid());
	if (n < 0 || (size_t)n >= sizeof(tmp))
		return -1;

	f = fopen(tmp, "w");
	if (f == NULL)
		return -1;
	if (fputs(content, f) == EOF) {
		fclose(f);
		unlink(tmp);
		return -1;
	}
	if (fclose(f) != 0) {
		unlink(tmp);
		return -1;
	}
	if (rename(tmp, path) != 0) {
		unlink(tmp);
		return -1;
	}
	return 0;
}

/* Reads `path` into a small stack buffer (state files are a handful of short
 * lines -- generous cap, same spirit as metrics/stats.c's bounded read) and
 * finds the first "KEY=" line, copying whatever follows up to '\n' or EOF
 * into `out` (mirrors `grep -m1 -E "^${key}=" | cut -d= -f2-`). Returns 0 on
 * a match, -1 if the file could not be read at all or the key was not
 * found. */
#define M4_STATE_FILE_MAX 4096

static int read_key(const char *path, const char *key, char *out, size_t out_len)
{
	char buf[M4_STATE_FILE_MAX];
	FILE *f;
	size_t n;
	size_t key_len = strlen(key);

	if (out_len > 0)
		out[0] = '\0';

	f = fopen(path, "r");
	if (f == NULL)
		return -1;
	n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = '\0';

	for (const char *line = buf; line < buf + n;) {
		const char *nl = strchr(line, '\n');
		size_t line_len = (nl != NULL) ? (size_t)(nl - line) : strlen(line);

		if (line_len > key_len && strncmp(line, key, key_len) == 0 && line[key_len] == '=') {
			size_t val_len = line_len - key_len - 1;
			if (val_len >= out_len)
				val_len = out_len - 1;
			memcpy(out, line + key_len + 1, val_len);
			out[val_len] = '\0';
			return 0;
		}
		if (nl == NULL)
			break;
		line = nl + 1;
	}
	return -1;
}

int m4_state_write(const char *runtime_dir, const char *profile, const char *reason)
{
	char path[M4_STATE_PATH_MAX];
	char content[256];
	int n;

	if (build_path(runtime_dir, "call-state", path, sizeof(path)) < 0)
		return -1;
	if (ensure_dir(runtime_dir) < 0)
		return -1;

	n = snprintf(content, sizeof(content), "PROFILE=%s\nSAVED_AT=%ld\nREASON=%s\n",
		     profile != NULL ? profile : "", (long)time(NULL),
		     (reason != NULL && reason[0] != '\0') ? reason : "manual");
	if (n < 0 || (size_t)n >= sizeof(content))
		return -1;

	return write_file_atomic(path, content);
}

bool m4_state_present(const char *runtime_dir)
{
	char path[M4_STATE_PATH_MAX];
	struct stat st;

	if (build_path(runtime_dir, "call-state", path, sizeof(path)) < 0)
		return false;
	return stat(path, &st) == 0;
}

int m4_state_read(const char *runtime_dir, char *out_profile, size_t out_profile_len,
		   char *out_reason, size_t out_reason_len)
{
	char path[M4_STATE_PATH_MAX];

	if (out_reason_len > 0)
		out_reason[0] = '\0';
	if (build_path(runtime_dir, "call-state", path, sizeof(path)) < 0)
		return -1;
	if (read_key(path, "PROFILE", out_profile, out_profile_len) < 0)
		return -1;
	/* REASON is optional (legacy files) -- absence is not a failure here,
	 * a missing key is treated as
	 * "manual" at the *caller* level, not as an error from
	 * this read itself. */
	(void)read_key(path, "REASON", out_reason, out_reason_len);
	return 0;
}

void m4_state_remove(const char *runtime_dir)
{
	char path[M4_STATE_PATH_MAX];

	if (build_path(runtime_dir, "call-state", path, sizeof(path)) < 0)
		return;
	unlink(path);
}

bool m4_state_safe_latched(const char *runtime_dir)
{
	char path[M4_STATE_PATH_MAX];
	struct stat st;

	if (build_path(runtime_dir, "safe-latch", path, sizeof(path)) < 0)
		return false;
	return stat(path, &st) == 0;
}

const char *m4_daemon_mode_name(m4_daemon_mode_t mode)
{
	switch (mode) {
	case M4_DAEMON_MODE_MANUAL: return "manual";
	case M4_DAEMON_MODE_AUTO: return "auto";
	case M4_DAEMON_MODE_OBSERVE:
	default:
		return "observe";
	}
}

int m4_daemon_mode_parse(const char *name, m4_daemon_mode_t *out)
{
	if (name == NULL || out == NULL)
		return -1;
	if (strcasecmp(name, "observe") == 0) {
		*out = M4_DAEMON_MODE_OBSERVE;
		return 0;
	}
	if (strcasecmp(name, "manual") == 0 || strcasecmp(name, "off") == 0) {
		*out = M4_DAEMON_MODE_MANUAL;
		return 0;
	}
	if (strcasecmp(name, "auto") == 0 || strcasecmp(name, "on") == 0) {
		*out = M4_DAEMON_MODE_AUTO;
		return 0;
	}
	return -1;
}

int m4_state_mode_write(const char *runtime_dir, m4_daemon_mode_t mode)
{
	char path[M4_STATE_PATH_MAX];
	char content[32];

	if (build_path(runtime_dir, "mode", path, sizeof(path)) < 0)
		return -1;
	if (ensure_dir(runtime_dir) < 0)
		return -1;
	snprintf(content, sizeof(content), "%s\n", m4_daemon_mode_name(mode));
	return write_file_atomic(path, content);
}

m4_daemon_mode_t m4_state_mode_read(const char *runtime_dir)
{
	char path[M4_STATE_PATH_MAX];
	char buf[M4_STATE_FILE_MAX];
	FILE *f;
	size_t n;
	m4_daemon_mode_t mode;

	if (build_path(runtime_dir, "mode", path, sizeof(path)) < 0)
		return M4_DAEMON_MODE_OBSERVE;

	f = fopen(path, "r");
	if (f == NULL)
		return M4_DAEMON_MODE_OBSERVE;
	n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = '\0';

	/* Strip a single trailing newline, if any -- write_file_atomic() above
	 * always adds one. */
	if (n > 0 && buf[n - 1] == '\n')
		buf[n - 1] = '\0';

	if (m4_daemon_mode_parse(buf, &mode) < 0)
		return M4_DAEMON_MODE_OBSERVE;
	return mode;
}

/* ------------------------------------------------------------------ */
/* mic-on-enter: m4_mic_on_enter_t + mic-last-state persistence.     */
/* ------------------------------------------------------------------ */

const char *m4_mic_on_enter_name(m4_mic_on_enter_t v)
{
	switch (v) {
	case M4_MIC_ON_ENTER_OFF: return "off";
	case M4_MIC_ON_ENTER_REMEMBER: return "remember";
	case M4_MIC_ON_ENTER_ON:
	default:
		return "on";
	}
}

int m4_mic_on_enter_parse(const char *name, m4_mic_on_enter_t *out)
{
	if (name == NULL || out == NULL)
		return -1;
	if (strcasecmp(name, "on") == 0) {
		*out = M4_MIC_ON_ENTER_ON;
		return 0;
	}
	if (strcasecmp(name, "off") == 0) {
		*out = M4_MIC_ON_ENTER_OFF;
		return 0;
	}
	if (strcasecmp(name, "remember") == 0) {
		*out = M4_MIC_ON_ENTER_REMEMBER;
		return 0;
	}
	return -1;
}

int m4_mic_state_write(const char *runtime_dir, const char *state_dir, bool muted)
{
	char content[32];
	int rc_runtime = -1, rc_state = -1;

	snprintf(content, sizeof(content), "MUTED=%d\n", muted ? 1 : 0);

	if (runtime_dir != NULL && runtime_dir[0] != '\0') {
		char path[M4_STATE_PATH_MAX];

		if (build_path(runtime_dir, "mic-last-state", path, sizeof(path)) == 0 &&
		    ensure_dir(runtime_dir) == 0)
			rc_runtime = write_file_atomic(path, content);
	}
	if (state_dir != NULL && state_dir[0] != '\0') {
		char path[M4_STATE_PATH_MAX];

		if (build_path(state_dir, "mic-last-state", path, sizeof(path)) == 0 &&
		    ensure_dir(state_dir) == 0)
			rc_state = write_file_atomic(path, content);
	}
	return (rc_runtime == 0 || rc_state == 0) ? 0 : -1;
}

int m4_mic_state_read(const char *runtime_dir, const char *state_dir, bool *muted_out)
{
	char path[M4_STATE_PATH_MAX];
	char val[8];

	if (muted_out == NULL)
		return -1;

	if (runtime_dir != NULL && runtime_dir[0] != '\0' &&
	    build_path(runtime_dir, "mic-last-state", path, sizeof(path)) == 0 &&
	    read_key(path, "MUTED", val, sizeof(val)) == 0) {
		*muted_out = (val[0] == '1');
		return 0;
	}
	if (state_dir != NULL && state_dir[0] != '\0' &&
	    build_path(state_dir, "mic-last-state", path, sizeof(path)) == 0 &&
	    read_key(path, "MUTED", val, sizeof(val)) == 0) {
		*muted_out = (val[0] == '1');
		return 0;
	}
	return -1;
}
