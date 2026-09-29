/* tests/test_player_resync.c -- PlaybackStatus re-send ("resync",
 * player status after reconnect): control/
 * mpris_player.c's Stopped -> real-status round trip, its automatic
 * triggers (registration, M4 Device1/MediaControl1 Connected, new
 * MediaPlayer1, MediaTransport1 State=active) and the daemon's wiring
 * (after Play/Pause/Stop from M4 with a fixed status, ctl PLAYER RESYNC /
 * PLAYER STATUS, RELOAD of `player-resync`).
 *
 * Everything runs against a PRIVATE dbus-daemon spawned here (fake BlueZ
 * in-process on its own connection) -- never the system or the user's
 * session bus; DBUS_*_BUS_ADDRESS are unset before any test runs. The
 * daemon part uses the fake graph (no PipeWire). Exit 77 (meson: skipped)
 * if dbus-daemon is not installed.
 *
 * What the fake records is exactly what bluetoothd's media.c reacts to:
 * PropertiesChanged(org.mpris.MediaPlayer2.Player, PlaybackStatus) from the
 * registered sender/path -> set_status() -> AVRCP CHANGED to M4 when the
 * value differs from the previous one. */
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
#include "core/graph_ops_fake.h"
#include "daemon/daemon.h"
#include "ipc/ctl.h"
#include "util/settings.h"

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

#define DEV_PATH "/org/bluez/hci0/dev_00_1B_66_00_00_01"
#define OTHER_DEV_PATH "/org/bluez/hci0/dev_11_22_33_44_55_66"
#define DELAY_MS 60
#define GAP_MS 40

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

		if (r <= 0 || dbus_addr[n] == '\n')
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

static double now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

typedef bool (*pred_fn)(void *ctx);

static bool pump_until(sd_event *event, pred_fn pred, void *ctx, int timeout_ms)
{
	double t0 = now_ms();

	for (;;) {
		if (pred != NULL && pred(ctx))
			return true;
		if (now_ms() - t0 > timeout_ms)
			return pred != NULL ? pred(ctx) : true;
		sd_event_run(event, 5 * 1000);
	}
}

static void pump_for(sd_event *event, int ms)
{
	pump_until(event, NULL, NULL, ms);
}

/* --- fake BlueZ ------------------------------------------------------------ */

struct fake_bluez {
	sd_bus *bus;
	sd_bus_slot *vtable_slot;
	sd_bus_slot *pc_match;
	int registers;
	char player_sender[64];
	char player_path[128];
	char reg_status[16];   /* PlaybackStatus inside RegisterPlayer */
	char history[512];     /* every PropertiesChanged PlaybackStatus, "A,B," */
	int n_changes;
	double t_change[64];   /* now_ms() of each change */
};

static int fb_register(sd_bus_message *m, void *userdata, sd_bus_error *err)
{
	struct fake_bluez *fb = userdata;
	const char *path = NULL;
	int r;

	(void)err;
	r = sd_bus_message_read(m, "o", &path);
	if (r < 0)
		return r;
	snprintf(fb->player_path, sizeof(fb->player_path), "%s", path);
	snprintf(fb->player_sender, sizeof(fb->player_sender), "%s", sd_bus_message_get_sender(m));
	r = sd_bus_message_enter_container(m, 'a', "{sv}");
	if (r < 0)
		return r;
	while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
		const char *key = NULL, *contents = NULL, *v = NULL;
		char type;

		sd_bus_message_read(m, "s", &key);
		sd_bus_message_peek_type(m, &type, &contents);
		if (contents != NULL && strcmp(contents, "s") == 0 && strcmp(key, "PlaybackStatus") == 0) {
			sd_bus_message_read(m, "v", "s", &v);
			snprintf(fb->reg_status, sizeof(fb->reg_status), "%s", v);
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
	(void)userdata;
	(void)err;
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
		if (strcmp(key, "PlaybackStatus") == 0 && sd_bus_message_read(m, "v", "s", &v) >= 0) {
			size_t n = strlen(fb->history);

			snprintf(fb->history + n, sizeof(fb->history) - n, "%s,", v);
			if (fb->n_changes < 64)
				fb->t_change[fb->n_changes] = now_ms();
			fb->n_changes++;
		} else {
			sd_bus_message_skip(m, "v");
		}
		sd_bus_message_exit_container(m);
	}
	return 0;
}

static void fb_clear(struct fake_bluez *fb)
{
	fb->history[0] = '\0';
	fb->n_changes = 0;
}

/* bluetoothd's media_player_send() */
static void fb_send(struct fake_bluez *fb, const char *method)
{
	int r = sd_bus_call_method_async(fb->bus, NULL, fb->player_sender, fb->player_path, M4_MPRIS_PLAYER_IFACE,
					 method, NULL, NULL, "");
	CHECK(r >= 0, "async method call sent");
}

/* org.freedesktop.DBus.Properties.PropertiesChanged(iface, {key: value}, []) */
static void fb_emit_prop(struct fake_bluez *fb, const char *path, const char *iface, const char *key, char type,
			 const void *value)
{
	sd_bus_message *m = NULL;
	int r;

	r = sd_bus_message_new_signal(fb->bus, &m, path, "org.freedesktop.DBus.Properties", "PropertiesChanged");
	if (r >= 0)
		r = sd_bus_message_append(m, "s", iface);
	if (r >= 0 && type == 'b')
		r = sd_bus_message_append(m, "a{sv}", 1, key, "b", *(const int *)value);
	else if (r >= 0)
		r = sd_bus_message_append(m, "a{sv}", 1, key, "s", (const char *)value);
	if (r >= 0)
		r = sd_bus_message_append(m, "as", 0);
	if (r >= 0)
		r = sd_bus_send(fb->bus, m, NULL);
	CHECK(r >= 0, "emit PropertiesChanged");
	sd_bus_message_unref(m);
}

static void fb_emit_bool(struct fake_bluez *fb, const char *path, const char *iface, const char *key, int v)
{
	fb_emit_prop(fb, path, iface, key, 'b', &v);
}

static void fb_emit_added(struct fake_bluez *fb, const char *path, const char *iface)
{
	sd_bus_message *m = NULL;
	int r;

	r = sd_bus_message_new_signal(fb->bus, &m, "/", "org.freedesktop.DBus.ObjectManager", "InterfacesAdded");
	if (r >= 0)
		r = sd_bus_message_append(m, "o", path);
	if (r >= 0)
		r = sd_bus_message_append(m, "a{sa{sv}}", 1, iface, 0);
	if (r >= 0)
		r = sd_bus_send(fb->bus, m, NULL);
	CHECK(r >= 0, "emit InterfacesAdded");
	sd_bus_message_unref(m);
}

static bool fb_setup(struct fake_bluez *fb, sd_event *event)
{
	memset(fb, 0, sizeof(*fb));
	fb->bus = open_bus(event);
	if (fb->bus == NULL)
		return false;
	if (sd_bus_add_object_vtable(fb->bus, &fb->vtable_slot, "/org/bluez/hci0", M4_BLUEZ_MEDIA_IFACE, fb_vtable,
				     fb) < 0)
		return false;
	if (sd_bus_add_match(fb->bus, &fb->pc_match,
			     "type='signal',interface='org.freedesktop.DBus.Properties',member='PropertiesChanged',"
			     "path='" M4_MPRIS_PLAYER_PATH "'",
			     fb_on_props_changed, fb) < 0)
		return false;
	return sd_bus_request_name(fb->bus, "org.bluez", 0) >= 0;
}

static void fb_teardown(struct fake_bluez *fb)
{
	sd_bus_slot_unref(fb->pc_match);
	sd_bus_slot_unref(fb->vtable_slot);
	sd_bus_flush_close_unref(fb->bus);
}

/* --- predicates ------------------------------------------------------------ */

static bool pred_registered(void *ctx)
{
	return m4_mpris_player_is_registered(ctx);
}

struct wait_count {
	struct m4_mpris_player *p;
	int want;
};
static bool pred_resyncs(void *ctx)
{
	struct wait_count *w = ctx;
	return m4_mpris_player_resync_count(w->p) >= w->want;
}

struct wait_hist {
	struct fake_bluez *fb;
	const char *want;
};
static bool pred_hist(void *ctx)
{
	struct wait_hist *w = ctx;
	return strcmp(w->fb->history, w->want) == 0;
}

static bool wait_history(sd_event *e, struct fake_bluez *fb, const char *want, int timeout_ms)
{
	struct wait_hist w = { fb, want };
	bool ok = pump_until(e, pred_hist, &w, timeout_ms);

	if (!ok)
		fprintf(stderr, "  history: got \"%s\", want \"%s\"\n", fb->history, want);
	return ok;
}

/* --- player-level tests ----------------------------------------------------- */

struct env {
	sd_event *event;
	struct fake_bluez fb;
	sd_bus *player_bus;
	struct m4_mpris_player *p;
};

static void noop_cb(void *user_data, const struct m4_player_event *ev)
{
	(void)user_data;
	(void)ev;
}

static bool env_setup(struct env *v, m4_player_status_policy_t initial, bool auto_resync, const char *device_path,
		      m4_mpris_player_cmd_cb cb, void *cb_data)
{
	struct m4_mpris_player_config cfg = {0};

	memset(v, 0, sizeof(*v));
	CHECK(sd_event_new(&v->event) >= 0, "sd_event_new");
	CHECK(fb_setup(&v->fb, v->event), "fake BlueZ up");
	v->player_bus = open_bus(v->event);
	CHECK(v->player_bus != NULL, "player bus");
	if (v->player_bus == NULL)
		return false;
	cfg.bus = v->player_bus;
	cfg.adapter_path = "/org/bluez/hci0";
	cfg.initial_status = initial;
	cfg.retry_ms = 100;
	cfg.reappear_delay_ms = 30;
	cfg.device_path = device_path;
	cfg.auto_resync = auto_resync;
	cfg.resync_delay_ms = DELAY_MS;
	cfg.resync_gap_ms = GAP_MS;
	cfg.cb = cb != NULL ? cb : noop_cb;
	cfg.user_data = cb_data;
	v->p = m4_mpris_player_new(v->event, &cfg);
	CHECK(v->p != NULL, "player created");
	if (v->p == NULL)
		return false;
	CHECK(pump_until(v->event, pred_registered, v->p, 3000), "registered");
	return true;
}

static void env_teardown(struct env *v)
{
	m4_mpris_player_free(v->p);
	pump_for(v->event, 20);
	sd_bus_flush_close_unref(v->player_bus);
	fb_teardown(&v->fb);
	sd_event_unref(v->event);
}

/* 1. registration itself -> Stopped, Playing (the daemon-restart case: a
 *    NEW player never sends AVRCP CHANGED to an existing session) */
static void test_resync_after_registration(void)
{
	struct env v;
	struct wait_count wc;

	if (!env_setup(&v, M4_PLAYER_STATUS_PLAYING, true, DEV_PATH, NULL, NULL))
		return;
	CHECK(strcmp(v.fb.reg_status, "Playing") == 0, "RegisterPlayer carries the REAL status");
	CHECK(wait_history(v.event, &v.fb, "Stopped,Playing,", 2000), "registration -> Stopped, Playing");
	wc.p = v.p;
	wc.want = 1;
	CHECK(pump_until(v.event, pred_resyncs, &wc, 1000) && m4_mpris_player_resync_count(v.p) == 1,
	      "1 resync counted");
	CHECK(v.fb.n_changes == 2 && v.fb.t_change[1] - v.fb.t_change[0] >= GAP_MS - 10,
	      "the real status follows Stopped after the gap, not in the same breath");
	pump_for(v.event, 200);
	CHECK(strcmp(v.fb.history, "Stopped,Playing,") == 0, "nothing else afterwards");
	env_teardown(&v);
}

/* 2. the M4 (re)connect / A2DP signals, debounced into ONE round trip */
static void test_resync_device_signals(void)
{
	struct env v;
	const char *active = "active", *idle = "idle";

	if (!env_setup(&v, M4_PLAYER_STATUS_PLAYING, true, DEV_PATH, NULL, NULL))
		return;
	CHECK(wait_history(v.event, &v.fb, "Stopped,Playing,", 2000), "initial registration resync");
	fb_clear(&v.fb);

	/* each trigger alone */
	fb_emit_bool(&v.fb, DEV_PATH, "org.bluez.Device1", "Connected", 1);
	CHECK(wait_history(v.event, &v.fb, "Stopped,Playing,", 2000), "Device1.Connected=true -> resync");
	fb_clear(&v.fb);
	fb_emit_bool(&v.fb, DEV_PATH, "org.bluez.MediaControl1", "Connected", 1);
	CHECK(wait_history(v.event, &v.fb, "Stopped,Playing,", 2000), "MediaControl1.Connected=true -> resync");
	fb_clear(&v.fb);
	fb_emit_added(&v.fb, DEV_PATH "/player0", "org.bluez.MediaPlayer1");
	CHECK(wait_history(v.event, &v.fb, "Stopped,Playing,", 2000), "new MediaPlayer1 under M4 -> resync");
	fb_clear(&v.fb);
	fb_emit_prop(&v.fb, DEV_PATH "/sep1/fd0", "org.bluez.MediaTransport1", "State", 's', active);
	CHECK(wait_history(v.event, &v.fb, "Stopped,Playing,", 2000), "MediaTransport1.State=active -> resync");
	CHECK(m4_mpris_player_resync_count(v.p) == 5, "5 round trips so far");
	fb_clear(&v.fb);

	/* a real reconnect fires all of them within ms -> ONE round trip */
	fb_emit_bool(&v.fb, DEV_PATH, "org.bluez.Device1", "Connected", 1);
	fb_emit_bool(&v.fb, DEV_PATH, "org.bluez.MediaControl1", "Connected", 1);
	fb_emit_added(&v.fb, DEV_PATH "/player0", "org.bluez.MediaPlayer1");
	fb_emit_prop(&v.fb, DEV_PATH "/sep1/fd0", "org.bluez.MediaTransport1", "State", 's', active);
	CHECK(wait_history(v.event, &v.fb, "Stopped,Playing,", 2000), "burst -> Stopped, Playing");
	pump_for(v.event, 250);
	CHECK(strcmp(v.fb.history, "Stopped,Playing,") == 0 && m4_mpris_player_resync_count(v.p) == 6,
	      "burst debounced into exactly one round trip");
	fb_clear(&v.fb);

	/* not triggers: disconnect, other device, other states, prefix look-alike */
	fb_emit_bool(&v.fb, DEV_PATH, "org.bluez.Device1", "Connected", 0);
	fb_emit_bool(&v.fb, DEV_PATH, "org.bluez.MediaControl1", "Connected", 0);
	fb_emit_bool(&v.fb, OTHER_DEV_PATH, "org.bluez.Device1", "Connected", 1);
	fb_emit_bool(&v.fb, DEV_PATH "2", "org.bluez.Device1", "Connected", 1);
	fb_emit_prop(&v.fb, DEV_PATH "/sep1/fd0", "org.bluez.MediaTransport1", "State", 's', idle);
	fb_emit_bool(&v.fb, DEV_PATH, "org.bluez.Device1", "ServicesResolved", 1);
	fb_emit_added(&v.fb, OTHER_DEV_PATH "/player0", "org.bluez.MediaPlayer1");
	pump_for(v.event, DELAY_MS + GAP_MS + 200);
	CHECK(v.fb.history[0] == '\0' && m4_mpris_player_resync_count(v.p) == 6,
	      "disconnect / other device / idle / other property -> no resync");

	/* auto off: signals ignored, explicit resync still works */
	m4_mpris_player_set_auto_resync(v.p, false);
	fb_emit_bool(&v.fb, DEV_PATH, "org.bluez.Device1", "Connected", 1);
	pump_for(v.event, DELAY_MS + GAP_MS + 150);
	CHECK(v.fb.history[0] == '\0', "auto_resync off -> Connected ignored");
	m4_mpris_player_resync(v.p, "test explicit");
	CHECK(wait_history(v.event, &v.fb, "Stopped,Playing,", 2000), "explicit resync works with auto off");
	env_teardown(&v);
}

/* 3. no device path known -> any device on the adapter counts */
static void test_resync_any_device(void)
{
	struct env v;

	if (!env_setup(&v, M4_PLAYER_STATUS_PLAYING, true, "", NULL, NULL))
		return;
	CHECK(wait_history(v.event, &v.fb, "Stopped,Playing,", 2000), "registration resync");
	fb_clear(&v.fb);
	fb_emit_bool(&v.fb, OTHER_DEV_PATH, "org.bluez.MediaControl1", "Connected", 1);
	CHECK(wait_history(v.event, &v.fb, "Stopped,Playing,", 2000), "device_path \"\" -> any device triggers");
	fb_clear(&v.fb);
	fb_emit_bool(&v.fb, "/org/bluez/hci1/dev_00_1B_66_00_00_01", "org.bluez.Device1", "Connected", 1);
	pump_for(v.event, DELAY_MS + GAP_MS + 150);
	CHECK(v.fb.history[0] == '\0', "another adapter does not");
	env_teardown(&v);
}

/* 4. edge cases of the round trip itself */
static void test_resync_edges(void)
{
	struct env v;
	struct wait_hist wh;

	if (!env_setup(&v, M4_PLAYER_STATUS_STOPPED, false, DEV_PATH, NULL, NULL))
		return;
	/* Stopped: nothing to re-send */
	m4_mpris_player_resync(v.p, "stopped");
	pump_for(v.event, DELAY_MS + GAP_MS + 150);
	CHECK(v.fb.history[0] == '\0' && m4_mpris_player_resync_count(v.p) == 0, "status Stopped -> no round trip");

	m4_mpris_player_set_status(v.p, M4_PLAYER_STATUS_PLAYING);
	CHECK(wait_history(v.event, &v.fb, "Playing,", 1000), "plain status change still immediate");
	fb_clear(&v.fb);

	/* trigger during the Stopped phase -> exactly one more round trip after it */
	m4_mpris_player_resync(v.p, "first");
	wh.fb = &v.fb;
	wh.want = "Stopped,";
	CHECK(pump_until(v.event, pred_hist, &wh, 2000), "in the Stopped phase");
	m4_mpris_player_resync(v.p, "during blip");
	m4_mpris_player_resync(v.p, "during blip 2");
	CHECK(wait_history(v.event, &v.fb, "Stopped,Playing,Stopped,Playing,", 3000),
	      "trigger(s) during the blip -> one more round trip");
	pump_for(v.event, DELAY_MS + GAP_MS + 150);
	CHECK(m4_mpris_player_resync_count(v.p) == 2, "2 round trips");
	fb_clear(&v.fb);

	/* status changed during the Stopped phase -> the second half announces the NEW value */
	m4_mpris_player_resync(v.p, "status change");
	CHECK(pump_until(v.event, pred_hist, &wh, 2000), "in the Stopped phase");
	m4_mpris_player_set_status(v.p, M4_PLAYER_STATUS_PAUSED);
	CHECK(strcmp(v.fb.history, "Stopped,") == 0, "no immediate emission during the blip");
	CHECK(wait_history(v.event, &v.fb, "Stopped,Paused,", 2000), "blip ends with the new status");
	fb_clear(&v.fb);

	/* bluetoothd restart in the middle of a blip: cancelled, re-registered
	 * with the REAL status (never "Stopped"), then a fresh round trip */
	m4_mpris_player_set_auto_resync(v.p, true);
	m4_mpris_player_resync(v.p, "restart");
	CHECK(pump_until(v.event, pred_hist, &wh, 2000), "in the Stopped phase");
	CHECK(sd_bus_release_name(v.fb.bus, "org.bluez") >= 0, "BlueZ leaves");
	pump_for(v.event, 50);
	CHECK(!m4_mpris_player_is_registered(v.p), "unregistered");
	CHECK(sd_bus_request_name(v.fb.bus, "org.bluez", 0) >= 0, "BlueZ back");
	CHECK(pump_until(v.event, pred_registered, v.p, 3000), "re-registered");
	CHECK(strcmp(v.fb.reg_status, "Paused") == 0, "re-registration carries Paused, not the blip's Stopped");
	CHECK(wait_history(v.event, &v.fb, "Stopped,Stopped,Paused,", 3000),
	      "after re-registration: one fresh round trip (the cancelled blip's Stopped stays in history)");
	env_teardown(&v);
}

/* --- daemon wiring ------------------------------------------------------------ */

struct dfx {
	struct env v;
	m4_graph_ops_fake_t fake;
	char runtime_dir[128];
	char config_path[192];
	struct m4_settings settings;
	struct m4_daemon *d;
	int got;
};

static void dfx_cb(void *user_data, const struct m4_player_event *ev)
{
	struct dfx *f = user_data;

	f->got++;
	m4_daemon_player_event(f->d, ev);
}

static bool dfx_setup(struct dfx *f, const char *settings_text)
{
	struct m4_daemon_config cfg = {0};

	memset(f, 0, sizeof(*f));
	m4_settings_defaults(&f->settings);
	m4_settings_parse(&f->settings, settings_text, "fixture");
	snprintf(f->runtime_dir, sizeof(f->runtime_dir), "/tmp/m4d-test-resync.XXXXXX");
	CHECK(mkdtemp(f->runtime_dir) != NULL, "mkdtemp");
	snprintf(f->config_path, sizeof(f->config_path), "%s/momentum4.conf", f->runtime_dir);
	if (!env_setup(&f->v, m4_player_status_initial(f->settings.player_status),
		       f->settings.player_resync != M4_PLAYER_RESYNC_OFF, DEV_PATH, dfx_cb, f))
		return false;
	m4_graph_ops_fake_init(&f->fake);
	cfg.graph_ops = m4_graph_ops_fake(&f->fake);
	cfg.mac_address = "00:1B:66:00:00:01";
	cfg.runtime_dir = f->runtime_dir;
	cfg.stats_path = "";
	cfg.confirm_timeout_ms = 300;
	cfg.has_initial_mode = true;
	cfg.initial_mode = M4_DAEMON_MODE_MANUAL;
	cfg.settings = &f->settings;
	cfg.config_path = f->config_path;
	f->d = m4_daemon_new(f->v.event, &cfg);
	CHECK(f->d != NULL, "daemon_new");
	if (f->d == NULL)
		return false;
	m4_daemon_attach_player(f->d, f->v.p);
	return true;
}

static void dfx_teardown(struct dfx *f)
{
	char cmd[256];

	m4_daemon_free(f->d);
	env_teardown(&f->v);
	snprintf(cmd, sizeof(cmd), "rm -rf -- '%s'", f->runtime_dir);
	if (system(cmd) != 0)
		fprintf(stderr, "warn: cleanup of %s failed\n", f->runtime_dir);
}

struct wait_got {
	struct dfx *f;
	int want;
};
static bool pred_got(void *ctx)
{
	struct wait_got *w = ctx;
	return w->f->got >= w->want;
}

static void m4_sends(struct dfx *f, const char *method)
{
	struct wait_got w = { f, f->got + 1 };

	fb_send(&f->v.fb, method);
	CHECK(pump_until(f->v.event, pred_got, &w, 2000), "command reached the daemon");
}

/* fixed Playing, M4 taps (Pause) -> we must flip M4 back */
static void test_daemon_resync_after_tap(void)
{
	struct dfx f;
	char response[M4_CTL_MAX_RESPONSE];

	if (!dfx_setup(&f, "player-status = playing\ngesture-playpause = none\ngesture-next = none\n"
			   "gesture-previous = none\n"))
		return;
	CHECK(wait_history(f.v.event, &f.v.fb, "Stopped,Playing,", 2000), "start: registration resync");
	fb_clear(&f.v.fb);

	m4_sends(&f, "Pause");
	CHECK(wait_history(f.v.event, &f.v.fb, "Stopped,Playing,", 2000), "Pause from M4 -> Stopped, Playing");
	fb_clear(&f.v.fb);
	m4_sends(&f, "Play");
	CHECK(wait_history(f.v.event, &f.v.fb, "Stopped,Playing,", 2000), "Play from M4 -> Stopped, Playing");
	fb_clear(&f.v.fb);
	m4_sends(&f, "Stop");
	CHECK(wait_history(f.v.event, &f.v.fb, "Stopped,Playing,", 2000), "Stop from M4 -> Stopped, Playing");
	fb_clear(&f.v.fb);

	m4_sends(&f, "Next");
	m4_sends(&f, "Previous");
	pump_for(f.v.event, DELAY_MS + GAP_MS + 150);
	CHECK(f.v.fb.history[0] == '\0', "Next/Previous do not flip M4's belief -> no resync");

	m4_daemon_handle_request(f.d, "PLAYER", response, sizeof(response));
	CHECK(strstr(response, "player_resync=on\n") != NULL, "PLAYER shows player_resync=on");
	CHECK(strstr(response, "player_resync_count=4\n") != NULL, "PLAYER shows 4 round trips");

	/* ctl: PLAYER STATUS playing (same value) -> round trip (what the user did by hand live) */
	m4_daemon_handle_request(f.d, "PLAYER STATUS playing", response, sizeof(response));
	CHECK(strncmp(response, "OK", 2) == 0, "PLAYER STATUS ok");
	CHECK(wait_history(f.v.event, &f.v.fb, "Stopped,Playing,", 2000), "PLAYER STATUS playing -> round trip");
	fb_clear(&f.v.fb);
	m4_daemon_handle_request(f.d, "PLAYER RESYNC", response, sizeof(response));
	CHECK(strncmp(response, "OK", 2) == 0, "PLAYER RESYNC ok");
	CHECK(wait_history(f.v.event, &f.v.fb, "Stopped,Playing,", 2000), "PLAYER RESYNC -> round trip");
	fb_clear(&f.v.fb);

	/* RELOAD with player-resync = off: auto triggers off, tap no longer resyncs */
	{
		FILE *cf = fopen(f.config_path, "w");

		CHECK(cf != NULL, "write config");
		if (cf != NULL) {
			fputs("player-status = playing\nplayer-resync = off\ngesture-playpause = none\n", cf);
			fclose(cf);
		}
	}
	m4_daemon_handle_request(f.d, "RELOAD", response, sizeof(response));
	CHECK(strncmp(response, "OK", 2) == 0, "RELOAD ok");
	fb_emit_bool(&f.v.fb, DEV_PATH, "org.bluez.Device1", "Connected", 1);
	m4_sends(&f, "Pause");
	pump_for(f.v.event, DELAY_MS + GAP_MS + 150);
	CHECK(f.v.fb.history[0] == '\0', "player-resync = off -> neither Connected nor Pause resync");
	m4_daemon_handle_request(f.d, "PLAYER RESYNC", response, sizeof(response));
	CHECK(wait_history(f.v.event, &f.v.fb, "Stopped,Playing,", 2000), "explicit PLAYER RESYNC still works");
	dfx_teardown(&f);
}

static void test_daemon_resync_modes(void)
{
	struct dfx f;

	/* connect: taps do not resync, connections do */
	if (!dfx_setup(&f, "player-status = playing\nplayer-resync = connect\ngesture-playpause = none\n"))
		return;
	CHECK(wait_history(f.v.event, &f.v.fb, "Stopped,Playing,", 2000), "connect mode: registration resync");
	fb_clear(&f.v.fb);
	m4_sends(&f, "Pause");
	pump_for(f.v.event, DELAY_MS + GAP_MS + 150);
	CHECK(f.v.fb.history[0] == '\0', "connect mode: Pause -> no resync");
	fb_emit_bool(&f.v.fb, DEV_PATH, "org.bluez.MediaControl1", "Connected", 1);
	CHECK(wait_history(f.v.event, &f.v.fb, "Stopped,Playing,", 2000), "connect mode: MediaControl1 -> resync");
	dfx_teardown(&f);

	/* follow: status follows the taps -> real changes, no extra round trip */
	if (!dfx_setup(&f, "player-status = follow\ngesture-playpause = none\n"))
		return;
	CHECK(wait_history(f.v.event, &f.v.fb, "Stopped,Playing,", 2000), "follow: registration resync (starts Playing)");
	fb_clear(&f.v.fb);
	m4_sends(&f, "Pause");
	CHECK(wait_history(f.v.event, &f.v.fb, "Paused,", 2000), "follow: Pause -> Paused");
	pump_for(f.v.event, DELAY_MS + GAP_MS + 150);
	CHECK(strcmp(f.v.fb.history, "Paused,") == 0, "follow: no extra round trip");
	dfx_teardown(&f);

	/* stopped (config default): nothing ever re-sent */
	if (!dfx_setup(&f, "gesture-playpause = none\n"))
		return;
	m4_sends(&f, "Play");
	fb_emit_bool(&f.v.fb, DEV_PATH, "org.bluez.Device1", "Connected", 1);
	pump_for(f.v.event, DELAY_MS + GAP_MS + 200);
	CHECK(f.v.fb.history[0] == '\0', "player-status = stopped -> no round trips at all");
	dfx_teardown(&f);
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

	test_resync_after_registration();
	test_resync_device_signals();
	test_resync_any_device();
	test_resync_edges();
	test_daemon_resync_after_tap();
	test_daemon_resync_modes();

	stop_dbus_daemon();
	printf("test_player_resync: %d/%d checks passed\n", checks - failures, checks);
	return failures == 0 ? 0 : 1;
}
