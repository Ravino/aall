/* util/earcon.c -- see earcon.h. */
#include "earcon.h"

#include "../../../common/m4-i18n.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "log.h"

#ifndef M4_SOURCE_SOUNDS_DIR
#define M4_SOURCE_SOUNDS_DIR ""
#endif

#define M4_EARCON_TIMEOUT_S "6" /* pw-play watchdog: never let a stuck/hung earcon block anything */

static int try_dir(const char *dir, const char *kind, char *buf, size_t cap)
{
	int n;

	if (dir == NULL || dir[0] == '\0')
		return -1;
	n = snprintf(buf, cap, "%s/%s.wav", dir, kind);
	if (n < 0 || (size_t)n >= cap)
		return -1;
	return access(buf, R_OK) == 0 ? 0 : -1;
}

const char *m4_earcon_resolve(const char *dir_override, const char *kind, char *buf, size_t cap)
{
	char tmp[512];
	const char *v;

	if (try_dir(dir_override, kind, buf, cap) == 0)
		return buf;
	if (try_dir(getenv("MOMENTUM4_SOUNDS_DIR"), kind, buf, cap) == 0)
		return buf;
	if ((v = getenv("XDG_DATA_HOME")) != NULL && v[0] != '\0') {
		snprintf(tmp, sizeof(tmp), "%s/momentum4/sounds", v);
		if (try_dir(tmp, kind, buf, cap) == 0)
			return buf;
	} else if ((v = getenv("HOME")) != NULL && v[0] != '\0') {
		snprintf(tmp, sizeof(tmp), "%s/.local/share/momentum4/sounds", v);
		if (try_dir(tmp, kind, buf, cap) == 0)
			return buf;
	}
	if (try_dir("/usr/share/momentum4/sounds", kind, buf, cap) == 0)
		return buf;
	if (try_dir(M4_SOURCE_SOUNDS_DIR, kind, buf, cap) == 0)
		return buf;
	return NULL;
}

static int find_in_path(const char *name, char *buf, size_t cap)
{
	const char *path = getenv("PATH");
	const char *p;

	if (path == NULL || path[0] == '\0')
		path = "/usr/local/bin:/usr/bin:/bin";
	for (p = path; *p != '\0';) {
		const char *colon = strchr(p, ':');
		size_t len = colon != NULL ? (size_t)(colon - p) : strlen(p);
		int n = snprintf(buf, cap, "%.*s/%s", (int)len, p, name);

		if (n > 0 && (size_t)n < cap && len > 0 && access(buf, X_OK) == 0)
			return 0;
		if (colon == NULL)
			break;
		p = colon + 1;
	}
	return -1;
}

int m4_earcon_play(const char *dir_override, const char *kind)
{
	char file[600], timeout_bin[256], pwplay_bin[256];
	char *argv[5];
	pid_t pid;
	int status;

	if (m4_earcon_resolve(dir_override, kind, file, sizeof(file)) == NULL) {
		m4_log(M4_LOG_WARN, M4_T("earcon: %s.wav not found -- sound skipped",
"earcon: %s.wav не найден -- сигнал пропущен"), kind);
		return -1;
	}
	if (find_in_path("pw-play", pwplay_bin, sizeof(pwplay_bin)) < 0) {
		m4_log(M4_LOG_WARN, M4_T("earcon: pw-play not found -- sound skipped",
"earcon: pw-play не найден -- сигнал пропущен"));
		return -1;
	}
	if (find_in_path("timeout", timeout_bin, sizeof(timeout_bin)) == 0) {
		argv[0] = timeout_bin;
		argv[1] = (char *)M4_EARCON_TIMEOUT_S;
		argv[2] = pwplay_bin;
		argv[3] = file;
		argv[4] = NULL;
	} else {
		argv[0] = pwplay_bin;
		argv[1] = file;
		argv[2] = NULL;
	}

	pid = fork();
	if (pid < 0) {
		m4_log(M4_LOG_WARN, "earcon: fork: %s", strerror(errno));
		return -1;
	}
	if (pid == 0) {
		/* async-signal-safe only from here on */
		pid_t pid2;
		int fd;

		setsid();
		pid2 = fork();
		if (pid2 != 0)
			_exit(pid2 < 0 ? 1 : 0);
		fd = open("/dev/null", O_RDWR);
		if (fd >= 0) {
			dup2(fd, STDIN_FILENO);
			dup2(fd, STDOUT_FILENO);
			dup2(fd, STDERR_FILENO);
			if (fd > STDERR_FILENO)
				close(fd);
		}
		execv(argv[0], argv);
		_exit(127);
	}
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
		;
	m4_log(M4_LOG_INFO, "earcon: %s (%s)", kind, file);
	return 0;
}
