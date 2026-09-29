/* ipc/ctl.c -- see ctl.h. */
#define _GNU_SOURCE /* struct ucred (SO_PEERCRED) -- glibc guards it on this */
#include "ctl.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "../util/log.h"
#include "../../../common/m4-i18n.h"

struct m4_ctl_server {
	sd_event *event;
	int listen_fd;
	sd_event_source *io_source;
	char socket_path[256];
	m4_ctl_request_cb cb;
	void *user_data;
};

static int on_listen_readable(sd_event_source *s, int fd, uint32_t revents, void *userdata)
{
	struct m4_ctl_server *srv = userdata;
	struct ucred cred;
	socklen_t cred_len = sizeof(cred);
	int cfd;
	char line[M4_CTL_MAX_LINE];
	size_t n = 0;
	char response[M4_CTL_MAX_RESPONSE];
	size_t rlen, off;
	struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };

	(void)s;
	(void)revents;

	cfd = accept4(fd, NULL, NULL, SOCK_CLOEXEC); /* earcon children must not inherit it */
	if (cfd < 0)
		return 0;

	/* SO_PEERCRED uid check -- this control socket only ever
	 * accepts requests from processes running as the same user (the
	 * daemon and the CLI both run in the same user session; there is no
	 * legitimate cross-user client). Silently close, do not even send an
	 * ERR -- an unauthorized peer does not need a reason. */
	if (getsockopt(cfd, SOL_SOCKET, SO_PEERCRED, &cred, &cred_len) < 0 ||
	    cred.uid != getuid()) {
		close(cfd);
		return 0;
	}

	setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

	for (;;) {
		char c;
		ssize_t r = recv(cfd, &c, 1, 0);

		if (r <= 0)
			break;
		if (c == '\n')
			break;
		if (n + 1 < sizeof(line))
			line[n++] = c;
	}
	line[n] = '\0';
	if (n > 0 && line[n - 1] == '\r')
		line[--n] = '\0';

	response[0] = '\0';
	if (n == 0) {
		snprintf(response, sizeof(response), "ERR empty request\n");
	} else if (srv->cb != NULL) {
		srv->cb(srv->user_data, line, response, sizeof(response));
		if (response[0] == '\0')
			snprintf(response, sizeof(response), "ERR empty response\n");
	} else {
		snprintf(response, sizeof(response), "ERR no handler\n");
	}

	rlen = strlen(response);
	off = 0;
	while (off < rlen) {
		ssize_t w = send(cfd, response + off, rlen - off, MSG_NOSIGNAL);

		if (w <= 0)
			break;
		off += (size_t)w;
	}
	close(cfd);
	return 0;
}

struct m4_ctl_server *m4_ctl_server_open(sd_event *event, const char *socket_path,
					   m4_ctl_request_cb cb, void *user_data)
{
	struct m4_ctl_server *srv;
	struct sockaddr_un addr;

	if (event == NULL || socket_path == NULL || strlen(socket_path) >= sizeof(addr.sun_path))
		return NULL;

	srv = calloc(1, sizeof(*srv));
	if (srv == NULL)
		return NULL;
	srv->listen_fd = -1;
	srv->event = event;
	srv->cb = cb;
	srv->user_data = user_data;
	snprintf(srv->socket_path, sizeof(srv->socket_path), "%s", socket_path);

	srv->listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (srv->listen_fd < 0) {
		m4_log(M4_LOG_WARN, M4_T("ctl.sock: socket() failed: %s",
"ctl.sock: socket() не удался: %s"), strerror(errno));
		goto fail;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", socket_path);

	/* Stale socket file from a crashed previous instance -- the flock in
	 * daemon/daemon.c already guarantees only one instance ever reaches
	 * this call, so an existing file here is leftover, not a live
	 * competitor. */
	unlink(socket_path);

	if (bind(srv->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		m4_log(M4_LOG_WARN, M4_T("ctl.sock: bind(%s) failed: %s",
"ctl.sock: bind(%s) не удался: %s"), socket_path, strerror(errno));
		goto fail;
	}
	if (listen(srv->listen_fd, 8) < 0) {
		m4_log(M4_LOG_WARN, M4_T("ctl.sock: listen() failed: %s",
"ctl.sock: listen() не удался: %s"), strerror(errno));
		goto fail;
	}

	if (sd_event_add_io(event, &srv->io_source, srv->listen_fd, EPOLLIN,
			     on_listen_readable, srv) < 0) {
		m4_log(M4_LOG_WARN, M4_T("ctl.sock: sd_event_add_io() failed",
"ctl.sock: sd_event_add_io() не удался"));
		goto fail;
	}

	return srv;

fail:
	m4_ctl_server_close(srv);
	return NULL;
}

void m4_ctl_server_close(struct m4_ctl_server *srv)
{
	if (srv == NULL)
		return;
	if (srv->io_source != NULL)
		sd_event_source_unref(srv->io_source);
	if (srv->listen_fd >= 0)
		close(srv->listen_fd);
	if (srv->socket_path[0] != '\0')
		unlink(srv->socket_path);
	free(srv);
}

int m4_ctl_client_request(const char *socket_path, const char *request_line,
			   char *response, size_t response_cap, int timeout_ms)
{
	int fd;
	struct sockaddr_un addr;
	struct timeval tv;
	char buf[M4_CTL_MAX_LINE];
	int n;
	size_t total, off, roff;

	if (socket_path == NULL || request_line == NULL || response == NULL || response_cap == 0)
		return -1;
	if (strlen(socket_path) >= sizeof(addr.sun_path))
		return -1;

	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;

	tv.tv_sec = timeout_ms > 0 ? timeout_ms / 1000 : 0;
	tv.tv_usec = timeout_ms > 0 ? (timeout_ms % 1000) * 1000 : 200000;
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", socket_path);

	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		return -1;
	}

	n = snprintf(buf, sizeof(buf), "%s\n", request_line);
	if (n < 0 || (size_t)n >= sizeof(buf)) {
		close(fd);
		return -1;
	}
	total = (size_t)n;
	off = 0;
	while (off < total) {
		ssize_t w = send(fd, buf + off, total - off, MSG_NOSIGNAL);

		if (w <= 0) {
			close(fd);
			return -1;
		}
		off += (size_t)w;
	}
	shutdown(fd, SHUT_WR);

	roff = 0;
	for (;;) {
		ssize_t r;

		if (roff + 1 >= response_cap)
			break;
		r = recv(fd, response + roff, response_cap - 1 - roff, 0);
		if (r < 0) {
			close(fd);
			return -1;
		}
		if (r == 0)
			break;
		roff += (size_t)r;
	}
	response[roff] = '\0';
	close(fd);
	return 0;
}

bool m4_ctl_response_field(const char *response, const char *key, char *out, size_t out_cap)
{
	size_t key_len;

	if (response == NULL || key == NULL || out == NULL || out_cap == 0)
		return false;
	key_len = strlen(key);

	for (const char *line = response; *line != '\0';) {
		const char *nl = strchr(line, '\n');
		size_t line_len = nl != NULL ? (size_t)(nl - line) : strlen(line);

		if (line_len > key_len && strncmp(line, key, key_len) == 0 && line[key_len] == '=') {
			size_t val_len = line_len - key_len - 1;

			if (val_len + 1 <= out_cap) {
				memcpy(out, line + key_len + 1, val_len);
				out[val_len] = '\0';
				return true;
			}
			return false;
		}
		if (nl == NULL)
			break;
		line = nl + 1;
	}
	return false;
}
