/* Unit tests for ipc/ctl.c: the $runtime_dir/ctl.sock
 * transport, exercised as a real AF_UNIX socket in a throwaway mkdtemp()
 * directory. m4_ctl_client_request() is a single blocking call (by design,
 * see ctl.h), so it cannot be used against a server whose sd_event loop is
 * only pumped by the SAME thread that would be blocked in it -- these
 * tests instead drive the client side by hand (connect/send/shutdown,
 * then pump the server's sd_event a few iterations, then recv the
 * already-buffered response) to stay single-threaded, and test
 * m4_ctl_client_request() itself only against a "nobody is listening"
 * socket path (its one deterministic, non-blocking-server-required case).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <systemd/sd-event.h>

#include "ipc/ctl.h"

static int failures = 0;

#define CHECK(cond, msg) \
	do { \
		if (!(cond)) { \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, (msg)); \
			failures++; \
		} \
	} while (0)

static void make_tmp_socket_path(char *out, size_t out_len)
{
	char dir[128];

	snprintf(dir, sizeof(dir), "/tmp/m4d-test-ctl.XXXXXX");
	CHECK(mkdtemp(dir) != NULL, "mkdtemp");
	snprintf(out, out_len, "%s/ctl.sock", dir);
}

static int g_cb_calls;
static char g_last_request[M4_CTL_MAX_LINE];

static void test_cb(void *user_data, const char *request, char *response, size_t response_cap)
{
	(void)user_data;
	g_cb_calls++;
	snprintf(g_last_request, sizeof(g_last_request), "%s", request);

	if (strcmp(request, "PING") == 0)
		snprintf(response, response_cap, "OK\nfield=1\n");
	else
		snprintf(response, response_cap, "ERR unknown: %s\n", request);
}

/* Sends `request_line` (already including any trailing content the caller
 * wants -- this helper appends exactly one '\n') over a fresh connection to
 * `sock_path`, pumps `event` so the server can process it, then reads
 * whatever the server wrote back. */
static void manual_roundtrip(const char *sock_path, sd_event *event, const char *request_line,
			      char *response, size_t response_cap)
{
	int fd;
	struct sockaddr_un addr = {0};
	char buf[M4_CTL_MAX_LINE + 8];
	int n;
	size_t off = 0;

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	CHECK(fd >= 0, "client socket()");
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock_path);
	CHECK(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0, "client connect()");

	n = snprintf(buf, sizeof(buf), "%s\n", request_line);
	CHECK(send(fd, buf, (size_t)n, 0) == n, "client send() full request");
	shutdown(fd, SHUT_WR);

	for (int i = 0; i < 20; i++)
		sd_event_run(event, 20 * 1000);

	response[0] = '\0';
	for (;;) {
		ssize_t r;

		if (off + 1 >= response_cap)
			break;
		r = recv(fd, response + off, response_cap - 1 - off, 0);
		if (r <= 0)
			break;
		off += (size_t)r;
	}
	response[off] = '\0';
	close(fd);
}

static void test_request_response_roundtrip(void)
{
	char sock_path[192];
	sd_event *event;
	struct m4_ctl_server *srv;
	char response[M4_CTL_MAX_RESPONSE];
	char field[64];

	make_tmp_socket_path(sock_path, sizeof(sock_path));
	CHECK(sd_event_new(&event) >= 0, "sd_event_new");
	g_cb_calls = 0;

	srv = m4_ctl_server_open(event, sock_path, test_cb, NULL);
	CHECK(srv != NULL, "server open");

	manual_roundtrip(sock_path, event, "PING", response, sizeof(response));
	CHECK(g_cb_calls == 1, "cb invoked once");
	CHECK(strcmp(g_last_request, "PING") == 0, "cb saw trimmed request (no trailing \\n)");
	CHECK(strncmp(response, "OK", 2) == 0, "response starts with OK");
	CHECK(m4_ctl_response_field(response, "field", field, sizeof(field)), "field= present");
	CHECK(strcmp(field, "1") == 0, "field=1");

	manual_roundtrip(sock_path, event, "BOGUS", response, sizeof(response));
	CHECK(strncmp(response, "ERR unknown: BOGUS", 18) == 0, "unknown command -> ERR");

	m4_ctl_server_close(srv);
	sd_event_unref(event);
}

static void test_empty_request_no_cb(void)
{
	char sock_path[192];
	sd_event *event;
	struct m4_ctl_server *srv;
	char response[M4_CTL_MAX_RESPONSE];

	make_tmp_socket_path(sock_path, sizeof(sock_path));
	CHECK(sd_event_new(&event) >= 0, "sd_event_new");
	g_cb_calls = 0;

	srv = m4_ctl_server_open(event, sock_path, test_cb, NULL);
	CHECK(srv != NULL, "server open");

	/* Send a bare newline -- an empty request. */
	manual_roundtrip(sock_path, event, "", response, sizeof(response));
	CHECK(g_cb_calls == 0, "cb not invoked for an empty request");
	CHECK(strncmp(response, "ERR", 3) == 0, "server itself rejects it");

	m4_ctl_server_close(srv);
	sd_event_unref(event);
}

static void test_client_request_no_listener(void)
{
	char response[64];
	int rc;

	/* A path under a directory that certainly has no listener bound. */
	rc = m4_ctl_client_request("/tmp/m4d-test-ctl-nobody-here.sock", "PING", response,
				    sizeof(response), 200);
	CHECK(rc < 0, "no listener -> client_request fails");
}

static void test_response_field_parsing(void)
{
	char out[64];

	CHECK(m4_ctl_response_field("OK\na=1\nb=hello\n", "a", out, sizeof(out)) && strcmp(out, "1") == 0,
	      "first key");
	CHECK(m4_ctl_response_field("OK\na=1\nb=hello\n", "b", out, sizeof(out)) &&
		      strcmp(out, "hello") == 0,
	      "second key");
	CHECK(!m4_ctl_response_field("OK\na=1\n", "missing", out, sizeof(out)), "missing key -> false");
	CHECK(m4_ctl_response_field("OK\nc=\n", "c", out, sizeof(out)) && out[0] == '\0',
	      "empty value parses as empty string");
	/* No trailing newline on the last line. */
	CHECK(m4_ctl_response_field("OK\nd=last", "d", out, sizeof(out)) && strcmp(out, "last") == 0,
	      "last line without trailing newline");
}

int main(void)
{
	test_request_response_roundtrip();
	test_empty_request_no_cb();
	test_client_request_no_listener();
	test_response_field_parsing();

	if (failures > 0) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("ctl: all checks passed\n");
	return 0;
}
