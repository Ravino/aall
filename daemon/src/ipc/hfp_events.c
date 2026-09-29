/* ipc/hfp_events.c -- see hfp_events.h. */
#define _GNU_SOURCE /* struct ucred / SCM_CREDENTIALS -- glibc guards them on this, same as ipc/ctl.c */
#include "hfp_events.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "../util/log.h"
#include "../../../common/m4-i18n.h"

/* patch 0006's lines are "<VERB> <bdaddr> <ts_ms>\0" -- well under 64 bytes
 * even for the longest verb/bdaddr combination; sized generously for
 * forward compatibility without inviting a giant stack buffer. */
#define M4_HFP_EVENTS_MAX_DGRAM 200
/* Bounded drain per io callback dispatch (hfp_events.h's doc comment):
 * every iteration is itself non-blocking (MSG_DONTWAIT), so this bound only
 * protects the rest of the sd-event loop from one flooding/misbehaving
 * local peer monopolising this callback; 32 datagrams is far more than one
 * HFP gesture burst (patch 0006's SPA side sends at most one per AT
 * command, and M4 sends at most a handful per user gesture
 * up to 7 CHUP in ~20s in the one live capture seen so far). */
#define M4_HFP_EVENTS_MAX_DRAIN 32

struct m4_hfp_events {
	sd_event *event;
	int fd;
	sd_event_source *io_source;
	char socket_path[560];
	m4_hfp_event_cb cb;
	void *user_data;
};

const char *m4_hfp_verb_name(m4_hfp_verb_t v)
{
	switch (v) {
	case M4_HFP_VERB_CHUP: return "CHUP";
	case M4_HFP_VERB_ATA:  return "ATA";
	case M4_HFP_VERB_BLDN: return "BLDN";
	case M4_HFP_VERB_BVRA: return "BVRA";
	case M4_HFP_VERB_RAW:  return "RAW";
	case M4_HFP_VERB_SCODROP: return "SCODROP";
	case M4_HFP_VERB_SCOUP: return "SCOUP";
	case M4_HFP_VERB_SCOFAIL: return "SCOFAIL";
	case M4_HFP_VERB_COUNT:
	default: return "?";
	}
}

int m4_hfp_verb_from_name(const char *name, m4_hfp_verb_t *out)
{
	if (name == NULL || out == NULL)
		return -1;
	for (int v = 0; v < (int)M4_HFP_VERB_COUNT; v++) {
		if (strcmp(name, m4_hfp_verb_name((m4_hfp_verb_t)v)) == 0) {
			*out = (m4_hfp_verb_t)v;
			return 0;
		}
	}
	return -1;
}

int m4_hfp_event_parse(const char *line, struct m4_hfp_event *out)
{
	char verb[16];
	char bdaddr[32];
	long long ts_ms;

	if (line == NULL || out == NULL)
		return -1;
	memset(out, 0, sizeof(*out));

	/* patch 0007 ("tap = mic" task): "RAW <token>
	 * <bdaddr> <ts_ms>" -- 4 fields, distinct shape from the named-verb
	 * lines below, so it is parsed first, on its own. The peeked-at first
	 * word is intentionally re-parsed below (cheap, and keeps this
	 * function's error handling in one place per shape rather than
	 * threading a "which shape" flag through). */
	{
		char first[16];

		if (sscanf(line, "%15s", first) == 1 && strcmp(first, "RAW") == 0) {
			char token[64];
			char extra[4]; /* present -> more than 4 fields -> malformed */
			int n = sscanf(line, "%15s %63s %31s %lld %3s", verb, token, bdaddr, &ts_ms, extra);

			if (n != 4 || ts_ms < 0) {
				memset(out, 0, sizeof(*out));
				return -1;
			}
			out->verb = M4_HFP_VERB_RAW;
			snprintf(out->raw_text, sizeof(out->raw_text), "%s", token);
			snprintf(out->bdaddr, sizeof(out->bdaddr), "%s", bdaddr);
			out->wire_realtime_usec = (uint64_t)ts_ms * 1000ULL;
			return 0;
		}
	}

	{
		char extra[4]; /* present -> more than 3 fields -> malformed, see below */
		int n = sscanf(line, "%15s %31s %lld %3s", verb, bdaddr, &ts_ms, extra);

		if (n != 3 || ts_ms < 0)
			return -1; /* n==4 means a stray 4th field -- reject rather than ignore it */
	}

	if (strcmp(verb, "CHUP") == 0)
		out->verb = M4_HFP_VERB_CHUP;
	else if (strcmp(verb, "ATA") == 0)
		out->verb = M4_HFP_VERB_ATA;
	else if (strcmp(verb, "BLDN") == 0)
		out->verb = M4_HFP_VERB_BLDN;
	else if (strcmp(verb, "BVRA") == 0)
		out->verb = M4_HFP_VERB_BVRA;
	else if (strcmp(verb, "SCODROP") == 0) /* patch 0008 */
		out->verb = M4_HFP_VERB_SCODROP;
	else if (strcmp(verb, "SCOUP") == 0)
		out->verb = M4_HFP_VERB_SCOUP;
	else if (strcmp(verb, "SCOFAIL") == 0)
		out->verb = M4_HFP_VERB_SCOFAIL;
	else {
		memset(out, 0, sizeof(*out));
		return -1;
	}

	snprintf(out->bdaddr, sizeof(out->bdaddr), "%s", bdaddr);
	out->wire_realtime_usec = (uint64_t)ts_ms * 1000ULL;
	return 0;
}

static uint64_t realtime_usec(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_REALTIME, &ts);
	return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

static double monotonic_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* One dispatch: drains up to M4_HFP_EVENTS_MAX_DRAIN pending datagrams,
 * each via a single non-blocking recvmsg() (MSG_DONTWAIT -- redundant with
 * the socket's own SOCK_NONBLOCK, kept for the same "belt and suspenders"
 * reason m4-hfp-events.c's sendto() uses it). Stops early at the first
 * EAGAIN/EWOULDBLOCK (nothing more pending right now) -- this function
 * never blocks under any condition. */
static int on_readable(sd_event_source *s, int fd, uint32_t revents, void *userdata)
{
	struct m4_hfp_events *h = userdata;
	int drained;

	(void)s;
	(void)revents;

	for (drained = 0; drained < M4_HFP_EVENTS_MAX_DRAIN; drained++) {
		char buf[M4_HFP_EVENTS_MAX_DGRAM + 1];
		char cmsgbuf[CMSG_SPACE(sizeof(struct ucred))];
		struct iovec iov = { .iov_base = buf, .iov_len = sizeof(buf) - 1 };
		struct msghdr msg;
		struct cmsghdr *cmsg;
		struct ucred *cred = NULL;
		struct m4_hfp_event ev;
		ssize_t r;

		memset(&msg, 0, sizeof(msg));
		msg.msg_iov = &iov;
		msg.msg_iovlen = 1;
		msg.msg_control = cmsgbuf;
		msg.msg_controllen = sizeof(cmsgbuf);

		r = recvmsg(fd, &msg, MSG_DONTWAIT);
		if (r < 0) {
			if (errno != EAGAIN && errno != EWOULDBLOCK)
				m4_log(M4_LOG_DEBUG, "hfp-events.sock: recvmsg: %s", strerror(errno));
			break;
		}
		if (r == 0)
			continue; /* empty datagram -- nothing to parse, keep draining */
		if (msg.msg_flags & MSG_TRUNC) {
			m4_log(M4_LOG_DEBUG, M4_T("hfp-events.sock: datagram longer than %d bytes, dropped",
"hfp-events.sock: датаграмма длиннее %d байт, отброшена"),
			       M4_HFP_EVENTS_MAX_DGRAM);
			continue;
		}
		buf[r] = '\0';

		for (cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
			if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_CREDENTIALS) {
				cred = (struct ucred *)CMSG_DATA(cmsg);
				break;
			}
		}
		/* No SCM_CREDENTIALS at all (SO_PASSCRED not set / failed, or a
		 * peer that predates it) or a different uid -- this socket has
		 * no accept()/SO_PEERCRED step to hang a check off of (it is
		 * SOCK_DGRAM), so this is the only authentication it gets.
		 * Fails closed: no credentials -> dropped, not delivered. */
		if (cred == NULL || cred->uid != getuid()) {
			m4_log(M4_LOG_DEBUG,
			       M4_T("hfp-events.sock: datagram without valid credentials, dropped",
"hfp-events.sock: датаграмма без корректных credentials, отброшена"));
			continue;
		}

		if (m4_hfp_event_parse(buf, &ev) < 0) {
			m4_log(M4_LOG_DEBUG, M4_T("hfp-events.sock: unrecognized datagram: \"%s\"",
"hfp-events.sock: нераспознанная датаграмма: \"%s\""), buf);
			continue;
		}

		ev.realtime_usec = realtime_usec();
		ev.monotonic_s = monotonic_s();

		if (h->cb != NULL)
			h->cb(h->user_data, &ev);
	}
	return 0;
}

struct m4_hfp_events *m4_hfp_events_open(sd_event *event, const char *runtime_dir,
					   m4_hfp_event_cb cb, void *user_data)
{
	struct m4_hfp_events *h;
	struct sockaddr_un addr;
	int one = 1;

	if (event == NULL || runtime_dir == NULL || runtime_dir[0] == '\0')
		return NULL;

	h = calloc(1, sizeof(*h));
	if (h == NULL)
		return NULL;
	h->fd = -1;
	h->event = event;
	h->cb = cb;
	h->user_data = user_data;

	if (snprintf(h->socket_path, sizeof(h->socket_path), "%s/hfp-events.sock", runtime_dir) >=
	    (int)sizeof(h->socket_path)) {
		m4_log(M4_LOG_WARN, M4_T("hfp-events.sock: runtime_dir path too long",
"hfp-events.sock: путь runtime_dir слишком длинный"));
		free(h);
		return NULL;
	}

	if (mkdir(runtime_dir, 0700) < 0 && errno != EEXIST) {
		m4_log(M4_LOG_WARN, M4_T("hfp-events.sock: failed to create %s: %s",
"hfp-events.sock: не удалось создать %s: %s"), runtime_dir,
		       strerror(errno));
		goto fail;
	}

	if (strlen(h->socket_path) >= sizeof(addr.sun_path)) {
		m4_log(M4_LOG_WARN, M4_T("hfp-events.sock: path %s too long for AF_UNIX",
"hfp-events.sock: путь %s слишком длинный для AF_UNIX"),
		       h->socket_path);
		goto fail;
	}

	h->fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (h->fd < 0) {
		m4_log(M4_LOG_WARN, M4_T("hfp-events.sock: socket() failed: %s",
"hfp-events.sock: socket() не удался: %s"), strerror(errno));
		goto fail;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	memcpy(addr.sun_path, h->socket_path, strlen(h->socket_path));

	/* Stale socket file from a crashed previous instance -- daemon.lock's
	 * flock already guarantees only one momentum4d instance ever reaches
	 * this call (same reasoning as ipc/ctl.c's own unlink() before bind()). */
	unlink(h->socket_path);

	if (bind(h->fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		m4_log(M4_LOG_WARN, M4_T("hfp-events.sock: bind(%s) failed: %s",
"hfp-events.sock: bind(%s) не удался: %s"), h->socket_path,
		       strerror(errno));
		goto fail;
	}

	if (setsockopt(h->fd, SOL_SOCKET, SO_PASSCRED, &one, sizeof(one)) < 0)
		m4_log(M4_LOG_WARN,
		       M4_T("hfp-events.sock: SO_PASSCRED failed (%s) -- events will be dropped "
		       "(no credentials to check the uid)",
"hfp-events.sock: SO_PASSCRED не удался (%s) -- события будут отбрасываться "
		       "(нет credentials для проверки uid)"),
		       strerror(errno));

	if (sd_event_add_io(event, &h->io_source, h->fd, EPOLLIN, on_readable, h) < 0) {
		m4_log(M4_LOG_WARN, M4_T("hfp-events.sock: sd_event_add_io() failed",
"hfp-events.sock: sd_event_add_io() не удался"));
		goto fail;
	}

	m4_log(M4_LOG_INFO, M4_T("hfp-events.sock: listening on %s",
"hfp-events.sock: слушаю %s"), h->socket_path);
	return h;

fail:
	m4_hfp_events_close(h);
	return NULL;
}

void m4_hfp_events_close(struct m4_hfp_events *h)
{
	if (h == NULL)
		return;
	if (h->io_source != NULL)
		sd_event_source_unref(h->io_source);
	if (h->fd >= 0)
		close(h->fd);
	if (h->socket_path[0] != '\0')
		unlink(h->socket_path);
	free(h);
}
