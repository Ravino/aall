/* tests/test_mpris_player.c -- control/mpris_player.c and
 * control/passthrough.c against a PRIVATE dbus-daemon (spawned here, killed
 * at the end) -- never the system or the user's session bus (live
 * registration is NOT done here).
 *
 * In this process: a fake BlueZ (owns "org.bluez", exports
 * /org/bluez/hci0 org.bluez.Media1 RegisterPlayer/UnregisterPlayer, then
 * calls our player's methods the way bluetoothd's media_player_send()
 * does), our player on its own connection, and a third "stranger"
 * connection. In a forked child: two fake session MPRIS players for the
 * passthrough test (passthrough does synchronous D-Bus calls, so the
 * peers must live in another process).
 *
 * Exit 77 (meson: skipped) if dbus-daemon is not installed.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <systemd/sd-bus.h>
#include <systemd/sd-event.h>

#include "control/gesture.h"
#include "control/mpris_player.h"
#include "control/passthrough.h"

static int failures = 0;
static int checks = 0;

#define CHECK(cond, msg)                                                          \
	do {                                                                      \
		checks++;                                                         \
		if (!(cond)) {                                                    \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, (msg)); \
			failures++;                                               \
		}                                                                 \
	} while (0)

/* --- private dbus-daemon ------------------------------------------------ */

static pid_t dbus_pid = -1;
static char dbus_addr[512];

static int start_dbus_daemon(void)
{
	int pfd[2];
	size_t n = 0;

	if (pipe(pfd) < 0)
		return -1;
	dbus_pid = fork();
	if (dbus_pid < 0)
		return -1;
	if (dbus_pid == 0) {
		char arg[64];

		close(pfd[0]);
		snprintf(arg, sizeof(arg), "--print-address=%d", pfd[1]);
		execlp("dbus-daemon", "dbus-daemon", "--session", "--nofork", "--nopidfile", arg, (char *)NULL);
		_exit(77);
	}
	close(pfd[1]);
	while (n + 1 < sizeof(dbus_addr)) {
		ssize_t r = read(pfd[0], dbus_addr + n, 1);

		if (r <= 0)
			break;
		if (dbus_addr[n] == '\n')
			break;
		n++;
	}
	dbus_addr[n] = '\0';
	close(pfd[0]);
	return n > 0 ? 0 : -1;
}

static void stop_dbus_daemon(void)
{
	if (dbus_pid > 0) {
		kill(dbus_pid, SIGTERM);
		waitpid(dbus_pid, NULL, 0);
	}
}

static sd_bus *open_bus(sd_event *event)
{
	sd_bus *bus = NULL;

	if (sd_bus_new(&bus) < 0)
		return NULL;
	if (sd_bus_set_address(bus, dbus_addr) < 0 || sd_bus_set_bus_client(bus, 1) < 0 ||
	    sd_bus_start(bus) < 0) {
		sd_bus_unref(bus);
		return NULL;
	}
	if (event != NULL && sd_bus_attach_event(bus, event, 0) < 0) {
		sd_bus_unref(bus);
		return NULL;
	}
	return bus;
}

typedef bool (*pred_fn)(void *ctx);

static bool pump_until(sd_event *event, pred_fn pred, void *ctx, int timeout_ms)
{
	struct timespec t0, t;

	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (;;) {
		if (pred(ctx))
			return true;
		clock_gettime(CLOCK_MONOTONIC, &t);
		if ((t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000 > timeout_ms)
			return pred(ctx);
		sd_event_run(event, 20 * 1000);
	}
}

/* --- fake BlueZ ------------------------------------------------------------ */

struct fake_bluez {
	sd_bus *bus;
	sd_bus_slot *vtable_slot;
	sd_bus_slot *pc_match;
	int register_attempts;
	int registers;
	int unregisters;
	int fail_next;
	char player_sender[64];
	char player_path[128];
	char status[16];
	char last_changed_status[16];
	int can_control, can_play, can_pause, can_next, can_prev;
	char identity[32];

	/* async call results */
	int replies_ok;
	int replies_err;
	char last_err[128];
};

static int fb_register(sd_bus_message *m, void *userdata, sd_bus_error *err)
{
	struct fake_bluez *fb = userdata;
	const char *path = NULL;
	int r;

	fb->register_attempts++;
	if (fb->fail_next > 0) {
		fb->fail_next--;
		return sd_bus_error_set(err, "org.bluez.Error.NotSupported", "fake failure");
	}
	r = sd_bus_message_read(m, "o", &path);
	if (r < 0)
		return r;
	snprintf(fb->player_path, sizeof(fb->player_path), "%s", path);
	snprintf(fb->player_sender, sizeof(fb->player_sender), "%s", sd_bus_message_get_sender(m));

	r = sd_bus_message_enter_container(m, 'a', "{sv}");
	if (r < 0)
		return r;
	while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
		const char *key = NULL;
		char type;
		const char *contents = NULL;

		sd_bus_message_read(m, "s", &key);
		sd_bus_message_peek_type(m, &type, &contents);
		if (contents != NULL && strcmp(contents, "b") == 0) {
			int b = 0;

			sd_bus_message_read(m, "v", "b", &b);
			if (strcmp(key, "CanControl") == 0)
				fb->can_control = b;
			else if (strcmp(key, "CanPlay") == 0)
				fb->can_play = b;
			else if (strcmp(key, "CanPause") == 0)
				fb->can_pause = b;
			else if (strcmp(key, "CanGoNext") == 0)
				fb->can_next = b;
			else if (strcmp(key, "CanGoPrevious") == 0)
				fb->can_prev = b;
		} else if (contents != NULL && strcmp(contents, "s") == 0) {
			const char *v = NULL;

			sd_bus_message_read(m, "v", "s", &v);
			if (strcmp(key, "PlaybackStatus") == 0)
				snprintf(fb->status, sizeof(fb->status), "%s", v);
			else if (strcmp(key, "Identity") == 0)
				snprintf(fb->identity, sizeof(fb->identity), "%s", v);
		} else {
			sd_bus_message_skip(m, "v");
		}
		sd_bus_message_exit_container(m);
	}
	sd_bus_message_exit_container(m);
	fb->registers++;
	return sd_bus_reply_method_return(m, NULL);
}

static int fb_unregister(sd_bus_message *m, void *userdata, sd_bus_error *err)
{
	struct fake_bluez *fb = userdata;

	(void)err;
	fb->unregisters++;
	return sd_bus_reply_method_return(m, NULL);
}

static const sd_bus_vtable fb_vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD("RegisterPlayer", "oa{sv}", "", fb_register, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("UnregisterPlayer", "o", "", fb_unregister, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_VTABLE_END,
};

static int fb_on_props_changed(sd_bus_message *m, void *userdata, sd_bus_error *err)
{
	struct fake_bluez *fb = userdata;
	const char *iface = NULL;

	(void)err;
	if (sd_bus_message_read(m, "s", &iface) < 0 || strcmp(iface, M4_MPRIS_PLAYER_IFACE) != 0)
		return 0;
	if (sd_bus_message_enter_container(m, 'a', "{sv}") < 0)
		return 0;
	while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
		const char *key = NULL, *v = NULL;

		sd_bus_message_read(m, "s", &key);
		if (strcmp(key, "PlaybackStatus") == 0 && sd_bus_message_read(m, "v", "s", &v) >= 0)
			snprintf(fb->last_changed_status, sizeof(fb->last_changed_status), "%s", v);
		else
			sd_bus_message_skip(m, "v");
		sd_bus_message_exit_container(m);
	}
	return 0;
}

static int fb_call_reply(sd_bus_message *reply, void *userdata, sd_bus_error *err)
{
	struct fake_bluez *fb = userdata;

	(void)err;
	if (sd_bus_message_is_method_error(reply, NULL)) {
		fb->replies_err++;
		snprintf(fb->last_err, sizeof(fb->last_err), "%s", sd_bus_message_get_error(reply)->name);
	} else {
		fb->replies_ok++;
	}
	return 0;
}

/* bluetoothd's media_player_send(): method call on (sender, path) */
static void fb_send(struct fake_bluez *fb, sd_bus *from, const char *method)
{
	int r = sd_bus_call_method_async(from, NULL, fb->player_sender, fb->player_path, M4_MPRIS_PLAYER_IFACE,
					 method, fb_call_reply, fb, "");
	CHECK(r >= 0, "async method call sent");
}

static void fb_emit_ifaces(struct fake_bluez *fb, bool added)
{
	sd_bus_message *m = NULL;
	int r;

	r = sd_bus_message_new_signal(fb->bus, &m, "/", "org.freedesktop.DBus.ObjectManager",
				      added ? "InterfacesAdded" : "InterfacesRemoved");
	if (r >= 0)
		r = sd_bus_message_append(m, "o", "/org/bluez/hci0");
	if (r >= 0) {
		if (added)
			r = sd_bus_message_append(m, "a{sa{sv}}", 1, M4_BLUEZ_MEDIA_IFACE, 0);
		else
			r = sd_bus_message_append(m, "as", 1, M4_BLUEZ_MEDIA_IFACE);
	}
	if (r >= 0)
		r = sd_bus_send(fb->bus, m, NULL);
	CHECK(r >= 0, "emit ObjectManager signal");
	sd_bus_message_unref(m);
}

/* --- our player's callback ------------------------------------------------- */

struct received {
	int n;
	m4_player_cmd_t cmds[16];
	char senders[16][64];
};

static void on_cmd(void *user_data, const struct m4_player_event *ev)
{
	struct received *rc = user_data;

	if (rc->n < 16) {
		rc->cmds[rc->n] = ev->cmd;
		snprintf(rc->senders[rc->n], sizeof(rc->senders[0]), "%s", ev->sender);
	}
	rc->n++;
	CHECK(ev->realtime_usec > 0 && ev->monotonic_s > 0, "event carries timestamps");
}

/* --- predicates ------------------------------------------------------------ */

struct wait_int {
	int *p;
	int want;
};
static bool pred_int_ge(void *ctx)
{
	struct wait_int *w = ctx;
	return *w->p >= w->want;
}
static bool pred_registered(void *ctx)
{
	return m4_mpris_player_is_registered(ctx);
}
static bool pred_unregistered(void *ctx)
{
	return !m4_mpris_player_is_registered(ctx);
}
struct wait_str {
	const char *s;
	const char *want;
};
static bool pred_str(void *ctx)
{
	struct wait_str *w = ctx;
	return strcmp(w->s, w->want) == 0;
}

/* --- fake session MPRIS players (child process) --------------------------- */

struct child_player {
	const char *short_name;
	const char *status;
	int wfd;
};

static int child_method(sd_bus_message *m, void *userdata, sd_bus_error *err)
{
	struct child_player *cp = userdata;
	char line[96];
	int n;

	(void)err;
	n = snprintf(line, sizeof(line), "%s %s\n", cp->short_name, sd_bus_message_get_member(m));
	if (write(cp->wfd, line, (size_t)n) < 0)
		_exit(3);
	return sd_bus_reply_method_return(m, NULL);
}

static int child_status(sd_bus *bus, const char *path, const char *interface, const char *property,
			sd_bus_message *reply, void *userdata, sd_bus_error *err)
{
	struct child_player *cp = userdata;

	(void)bus; (void)path; (void)interface; (void)property; (void)err;
	return sd_bus_message_append(reply, "s", cp->status);
}

static const sd_bus_vtable child_vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD("Play", "", "", child_method, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("Pause", "", "", child_method, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("PlayPause", "", "", child_method, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("Next", "", "", child_method, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("Previous", "", "", child_method, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("Stop", "", "", child_method, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_PROPERTY("PlaybackStatus", "s", child_status, 0, 0),
	SD_BUS_VTABLE_END,
};

static void run_child_players(int wfd, int readyfd)
{
	struct child_player cps[2] = {
		{ "aaa", "Paused", wfd },
		{ "bbb", "Playing", wfd },
	};
	sd_event *e = NULL;
	sd_bus *b[2];
	char name[96];

	if (sd_event_new(&e) < 0)
		_exit(2);
	for (int i = 0; i < 2; i++) {
		b[i] = open_bus(e);
		if (b[i] == NULL)
			_exit(2);
		if (sd_bus_add_object_vtable(b[i], NULL, "/org/mpris/MediaPlayer2", "org.mpris.MediaPlayer2.Player",
					     child_vtable, &cps[i]) < 0)
			_exit(2);
		snprintf(name, sizeof(name), "org.mpris.MediaPlayer2.%s", cps[i].short_name);
		if (sd_bus_request_name(b[i], name, 0) < 0)
			_exit(2);
	}
	if (write(readyfd, "R", 1) != 1)
		_exit(2);
	sd_event_loop(e);
	_exit(0);
}

static bool read_line_timeout(int fd, char *buf, size_t cap, int timeout_ms)
{
	size_t n = 0;

	while (n + 1 < cap) {
		struct pollfd p = { .fd = fd, .events = POLLIN };
		char c;

		if (poll(&p, 1, timeout_ms) <= 0)
			break;
		if (read(fd, &c, 1) != 1)
			break;
		if (c == '\n') {
			buf[n] = '\0';
			return true;
		}
		buf[n++] = c;
	}
	buf[n] = '\0';
	return false;
}

/* --- tests ---------------------------------------------------------------- */

static void test_player(void)
{
	sd_event *event = NULL;
	struct fake_bluez fb;
	sd_bus *player_bus, *stranger;
	struct m4_mpris_player_config cfg = {0};
	struct m4_mpris_player *p;
	struct received rc;
	const char *player_unique = NULL, *bluez_unique = NULL;
	static const char *const methods[] = { "Play", "Pause", "PlayPause", "Next", "Previous", "Stop" };
	static const m4_player_cmd_t expect[] = {
		M4_PLAYER_CMD_PLAY, M4_PLAYER_CMD_PAUSE, M4_PLAYER_CMD_PLAYPAUSE,
		M4_PLAYER_CMD_NEXT, M4_PLAYER_CMD_PREVIOUS, M4_PLAYER_CMD_STOP,
	};
	struct wait_int w;
	struct wait_str ws;
	char match[256];

	memset(&fb, 0, sizeof(fb));
	memset(&rc, 0, sizeof(rc));
	CHECK(sd_event_new(&event) >= 0, "sd_event_new");

	fb.bus = open_bus(event);
	player_bus = open_bus(event);
	stranger = open_bus(event);
	CHECK(fb.bus && player_bus && stranger, "3 connections to the private bus");
	if (!(fb.bus && player_bus && stranger))
		return;
	CHECK(sd_bus_add_object_vtable(fb.bus, &fb.vtable_slot, "/org/bluez/hci0", M4_BLUEZ_MEDIA_IFACE, fb_vtable,
				       &fb) >= 0,
	      "fake Media1 exported");
	fb.fail_next = 1; /* first RegisterPlayer fails -> retry path */
	CHECK(sd_bus_request_name(fb.bus, "org.bluez", 0) >= 0, "fake owns org.bluez");
	sd_bus_get_unique_name(player_bus, &player_unique);
	sd_bus_get_unique_name(fb.bus, &bluez_unique);

	cfg.bus = player_bus;
	cfg.adapter_path = "/org/bluez/hci0";
	cfg.initial_status = M4_PLAYER_STATUS_STOPPED;
	cfg.retry_ms = 100;
	cfg.reappear_delay_ms = 50;
	cfg.cb = on_cmd;
	cfg.user_data = &rc;
	p = m4_mpris_player_new(event, &cfg);
	CHECK(p != NULL, "player created");
	if (p == NULL)
		return;

	/* 1. registration (after one failure + retry) */
	CHECK(pump_until(event, pred_registered, p, 3000), "player registered");
	CHECK(fb.register_attempts == 2 && fb.registers == 1, "1 failure, then 1 successful registration");
	CHECK(strcmp(fb.player_sender, player_unique) == 0, "registered from our connection");
	CHECK(strcmp(fb.player_path, M4_MPRIS_PLAYER_PATH) == 0, "object path");
	CHECK(strcmp(fb.status, "Stopped") == 0, "PlaybackStatus Stopped");
	CHECK(fb.can_control && fb.can_play && fb.can_pause && fb.can_next && fb.can_prev,
	      "CanControl/CanPlay/CanPause/CanGoNext/CanGoPrevious (BlueZ checks them)");
	CHECK(strcmp(fb.identity, "momentum4") == 0, "Identity");

	/* 2. every command BlueZ can send arrives, in order, with sender */
	for (int i = 0; i < 6; i++) {
		fb_send(&fb, fb.bus, methods[i]);
		w.p = &rc.n;
		w.want = i + 1;
		CHECK(pump_until(event, pred_int_ge, &w, 2000), "command delivered");
		CHECK(rc.cmds[i] == expect[i], methods[i]);
		CHECK(strcmp(rc.senders[i], bluez_unique) == 0, "sender = BlueZ");
	}
	w.p = &fb.replies_ok;
	w.want = 6;
	CHECK(pump_until(event, pred_int_ge, &w, 2000), "all 6 got an (empty) method return");

	/* 3. a stranger cannot drive our player */
	fb_send(&fb, stranger, "Next");
	w.p = &fb.replies_err;
	w.want = 1;
	CHECK(pump_until(event, pred_int_ge, &w, 2000), "stranger got an error");
	CHECK(strstr(fb.last_err, "AccessDenied") != NULL, "AccessDenied");
	CHECK(rc.n == 6 && m4_mpris_player_rejected_count(p) == 1, "stranger's call not delivered");

	/* 4. status change -> PropertiesChanged (what BlueZ watches) */
	snprintf(match, sizeof(match),
		 "type='signal',sender='%s',path='%s',interface='org.freedesktop.DBus.Properties',"
		 "member='PropertiesChanged'",
		 player_unique, M4_MPRIS_PLAYER_PATH);
	CHECK(sd_bus_add_match(fb.bus, &fb.pc_match, match, fb_on_props_changed, &fb) >= 0, "match");
	m4_mpris_player_set_status(p, M4_PLAYER_STATUS_PLAYING);
	ws.s = fb.last_changed_status;
	ws.want = "Playing";
	CHECK(pump_until(event, pred_str, &ws, 2000), "PropertiesChanged PlaybackStatus=Playing");

	/* 5. bluetoothd restart: name lost -> unregistered; back -> re-registered */
	CHECK(sd_bus_release_name(fb.bus, "org.bluez") >= 0, "release org.bluez");
	CHECK(pump_until(event, pred_unregistered, p, 2000), "unregistered after BlueZ left");
	CHECK(sd_bus_request_name(fb.bus, "org.bluez", 0) >= 0, "re-acquire org.bluez");
	w.p = &fb.registers;
	w.want = 2;
	CHECK(pump_until(event, pred_int_ge, &w, 3000), "re-registered after BlueZ came back");
	CHECK(pump_until(event, pred_registered, p, 2000), "player saw the reply");
	CHECK(strcmp(fb.status, "Playing") == 0, "re-registration carries the current status");

	/* 6. adapter Media1 removed/added (e.g. adapter reset) */
	fb_emit_ifaces(&fb, false);
	CHECK(pump_until(event, pred_unregistered, p, 2000), "unregistered after InterfacesRemoved(Media1)");
	fb_emit_ifaces(&fb, true);
	w.want = 3;
	CHECK(pump_until(event, pred_int_ge, &w, 3000), "re-registered after InterfacesAdded(Media1)");
	CHECK(pump_until(event, pred_registered, p, 2000), "player saw the reply");
	CHECK(m4_mpris_player_register_count(p) == 3, "3 successful registrations");

	/* 7. free -> UnregisterPlayer */
	m4_mpris_player_free(p);
	w.p = &fb.unregisters;
	w.want = 1;
	CHECK(pump_until(event, pred_int_ge, &w, 2000), "UnregisterPlayer on free");

	sd_bus_slot_unref(fb.pc_match);
	sd_bus_slot_unref(fb.vtable_slot);
	sd_bus_flush_close_unref(stranger);
	sd_bus_flush_close_unref(player_bus);
	sd_bus_flush_close_unref(fb.bus);
	sd_event_unref(event);
}

static void test_passthrough(void)
{
	sd_bus *bus = open_bus(NULL);
	struct m4_passthrough_sdbus *pt;
	struct m4_passthrough_ops ops;
	char target[200], line[96];
	int calls[2], ready[2];
	pid_t child;
	char c;

	CHECK(bus != NULL, "bus for passthrough");
	if (bus == NULL)
		return;
	pt = m4_passthrough_sdbus_new(bus);
	ops = m4_passthrough_sdbus_ops(pt);

	CHECK(ops.forward(ops.ctx, "PlayPause", target, sizeof(target)) == 1, "no MPRIS player -> 1");

	CHECK(pipe(calls) == 0 && pipe(ready) == 0, "pipes");
	child = fork();
	if (child == 0) {
		close(calls[0]);
		close(ready[0]);
		run_child_players(calls[1], ready[1]);
	}
	close(calls[1]);
	close(ready[1]);
	CHECK(read(ready[0], &c, 1) == 1 && c == 'R', "fake session players ready");

	CHECK(ops.forward(ops.ctx, "PlayPause", target, sizeof(target)) == 0, "forward ok");
	CHECK(strstr(target, "org.mpris.MediaPlayer2.bbb") != NULL && strstr(target, "Playing") != NULL,
	      "the Playing player is chosen");
	CHECK(read_line_timeout(calls[0], line, sizeof(line), 2000) && strcmp(line, "bbb PlayPause") == 0,
	      "bbb received PlayPause");

	CHECK(ops.forward(ops.ctx, "Next", target, sizeof(target)) == 0, "forward Next");
	CHECK(read_line_timeout(calls[0], line, sizeof(line), 2000) && strcmp(line, "bbb Next") == 0,
	      "bbb received Next");

	kill(child, SIGKILL);
	waitpid(child, NULL, 0);
	close(calls[0]);
	close(ready[0]);
	m4_passthrough_sdbus_free(pt);
	sd_bus_flush_close_unref(bus);
}

int main(void)
{
	if (start_dbus_daemon() < 0) {
		fprintf(stderr, "dbus-daemon unavailable -- skipping\n");
		stop_dbus_daemon();
		return 77;
	}
	/* never let anything here touch the real buses */
	unsetenv("DBUS_SESSION_BUS_ADDRESS");
	unsetenv("DBUS_SYSTEM_BUS_ADDRESS");

	test_player();
	test_passthrough();

	stop_dbus_daemon();
	printf("test_mpris_player: %d/%d checks passed\n", checks - failures, checks);
	return failures == 0 ? 0 : 1;
}
