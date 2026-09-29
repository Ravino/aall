/* control/mpris_player.c -- see mpris_player.h. */
#include "mpris_player.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../util/log.h"
#include "../../../common/m4-i18n.h"

#define QUEUE_CAP 16
#define REGISTER_TIMEOUT_USEC (5ULL * 1000 * 1000)
#define RESYNC_DELAY_MS_DEFAULT 800
#define RESYNC_GAP_MS_DEFAULT 500

/* resync state (mpris_player.h m4_mpris_player_resync()) */
enum resync_state {
	RS_IDLE = 0,
	RS_WAIT,  /* trigger seen, waiting resync_delay_ms (debounce) */
	RS_BLIP,  /* "Stopped" announced, waiting resync_gap_ms */
};

struct m4_mpris_player {
	sd_event *event;
	sd_bus *bus;
	char service[64];
	char adapter_path[128];
	char object_path[128];
	char identity[64];
	int retry_ms;
	int reappear_delay_ms;
	bool accept_any_sender;
	m4_mpris_player_cmd_cb cb;
	void *user_data;

	m4_player_status_policy_t status;

	sd_bus_slot *slot_player_vtable;
	sd_bus_slot *slot_root_vtable;
	sd_bus_slot *slot_owner_match;
	sd_bus_slot *slot_ifadded_match;
	sd_bus_slot *slot_ifremoved_match;
	sd_bus_slot *slot_get_owner;
	sd_bus_slot *slot_register;
	sd_event_source *retry_timer;
	sd_event_source *defer;

	char bluez_owner[64]; /* "" = unknown / not on the bus */
	bool registered;
	int register_count;
	int rejected_count;
	int fail_streak;

	struct m4_player_event queue[QUEUE_CAP];
	int q_head, q_len;

	/* resync */
	char watch_ns[160];        /* path_namespace for device signals */
	bool auto_resync;
	int resync_delay_ms;
	int resync_gap_ms;
	enum resync_state rs_state;
	bool blip;                 /* PlaybackStatus currently reads "Stopped" */
	bool rs_again;             /* trigger arrived during RS_BLIP */
	char rs_reason[128];
	int resync_count;
	sd_event_source *resync_timer;
	sd_bus_slot *slot_dev_props_match;
};

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

static void do_register(struct m4_mpris_player *p);
static void resync_cancel(struct m4_mpris_player *p);
static void resync_request(struct m4_mpris_player *p, const char *reason);

/* ------------------------------------------------------------------ */
/* retry / delayed registration */

static int on_retry_timer(sd_event_source *s, uint64_t usec, void *userdata)
{
	struct m4_mpris_player *p = userdata;

	(void)s;
	(void)usec;
	if (!p->registered)
		do_register(p);
	return 0;
}

static void arm_timer(struct m4_mpris_player *p, int delay_ms)
{
	uint64_t usec = (uint64_t)(delay_ms > 0 ? delay_ms : 1) * 1000;

	if (p->retry_timer == NULL) {
		if (sd_event_add_time_relative(p->event, &p->retry_timer, CLOCK_MONOTONIC, usec, 10 * 1000,
						on_retry_timer, p) < 0) {
			m4_log(M4_LOG_WARN, M4_T("MPRIS: failed to arm the registration retry timer",
"MPRIS: не удалось завести таймер повтора регистрации"));
			p->retry_timer = NULL;
		}
		return;
	}
	sd_event_source_set_time_relative(p->retry_timer, usec);
	sd_event_source_set_enabled(p->retry_timer, SD_EVENT_ONESHOT);
}

static void mark_unregistered(struct m4_mpris_player *p, const char *why)
{
	if (p->registered)
		m4_log(M4_LOG_WARN, M4_T("MPRIS: player no longer registered with BlueZ (%s)",
"MPRIS: плеер больше не зарегистрирован в BlueZ (%s)"), why);
	p->registered = false;
	p->slot_register = sd_bus_slot_unref(p->slot_register);
	resync_cancel(p);
}

/* ------------------------------------------------------------------ */
/* RegisterPlayer */

static int on_register_reply(sd_bus_message *reply, void *userdata, sd_bus_error *ret_error)
{
	struct m4_mpris_player *p = userdata;
	const sd_bus_error *e;

	(void)ret_error;
	p->slot_register = sd_bus_slot_unref(p->slot_register);

	if (sd_bus_message_is_method_error(reply, NULL)) {
		e = sd_bus_message_get_error(reply);
		if (e != NULL && e->name != NULL && strstr(e->name, "AlreadyExists") != NULL) {
			/* our own earlier registration survived (e.g. a reply lost
			 * while BlueZ was busy) -- the desired state holds */
			p->registered = true;
			p->fail_streak = 0;
			m4_log(M4_LOG_INFO, M4_T("MPRIS: player already registered with BlueZ (%s)",
"MPRIS: плеер уже зарегистрирован в BlueZ (%s)"), e->name);
			if (p->auto_resync)
				resync_request(p, M4_T("BlueZ registration (AlreadyExists)",
"регистрация в BlueZ (AlreadyExists)"));
			return 0;
		}
		p->fail_streak++;
		m4_log(p->fail_streak == 1 ? M4_LOG_WARN : M4_LOG_DEBUG,
		       M4_T("MPRIS: RegisterPlayer(%s) on %s failed: %s: %s -- retrying in %d ms",
"MPRIS: RegisterPlayer(%s) на %s не удался: %s: %s -- повтор через %d мс"),
		       p->object_path, p->adapter_path, e != NULL && e->name ? e->name : "?",
		       e != NULL && e->message ? e->message : "", p->retry_ms);
		arm_timer(p, p->retry_ms);
		return 0;
	}

	p->registered = true;
	p->fail_streak = 0;
	p->register_count++;
	m4_log(M4_LOG_INFO, M4_T("MPRIS: player %s registered with BlueZ (%s, owner %s), status %s",
"MPRIS: плеер %s зарегистрирован в BlueZ (%s, владелец %s), статус %s"),
	       p->object_path, p->adapter_path, p->bluez_owner[0] ? p->bluez_owner : "?",
	       m4_player_status_mpris(p->status));
	/* A NEW player never sends AVRCP CHANGED to an already connected M4
	 * (avrcp_register_player() only attaches it to the session; the status
	 * M4 last saw came from the previous player / our previous process). */
	if (p->auto_resync)
		resync_request(p, M4_T("BlueZ registration",
"регистрация в BlueZ"));
	return 0;
}

static int append_bool_entry(sd_bus_message *m, const char *key, int value)
{
	int r = sd_bus_message_open_container(m, 'e', "sv");

	if (r >= 0)
		r = sd_bus_message_append(m, "s", key);
	if (r >= 0)
		r = sd_bus_message_append(m, "v", "b", value);
	if (r >= 0)
		r = sd_bus_message_close_container(m);
	return r;
}

static int append_str_entry(sd_bus_message *m, const char *key, const char *value)
{
	int r = sd_bus_message_open_container(m, 'e', "sv");

	if (r >= 0)
		r = sd_bus_message_append(m, "s", key);
	if (r >= 0)
		r = sd_bus_message_append(m, "v", "s", value);
	if (r >= 0)
		r = sd_bus_message_close_container(m);
	return r;
}

static void do_register(struct m4_mpris_player *p)
{
	sd_bus_message *m = NULL;
	int r;

	if (p->registered || p->slot_register != NULL)
		return;

	r = sd_bus_message_new_method_call(p->bus, &m, p->service, p->adapter_path, M4_BLUEZ_MEDIA_IFACE,
					   "RegisterPlayer");
	if (r >= 0)
		r = sd_bus_message_append(m, "o", p->object_path);
	if (r >= 0)
		r = sd_bus_message_open_container(m, 'a', "{sv}");
	if (r >= 0)
		r = append_str_entry(m, "PlaybackStatus", m4_player_status_mpris(p->status));
	if (r >= 0)
		r = append_bool_entry(m, "CanControl", 1);
	if (r >= 0)
		r = append_bool_entry(m, "CanPlay", 1);
	if (r >= 0)
		r = append_bool_entry(m, "CanPause", 1);
	if (r >= 0)
		r = append_bool_entry(m, "CanGoNext", 1);
	if (r >= 0)
		r = append_bool_entry(m, "CanGoPrevious", 1);
	if (r >= 0)
		r = append_str_entry(m, "Identity", p->identity);
	if (r >= 0)
		r = sd_bus_message_close_container(m);
	if (r >= 0)
		r = sd_bus_call_async(p->bus, &p->slot_register, m, on_register_reply, p,
				      REGISTER_TIMEOUT_USEC);
	sd_bus_message_unref(m);
	if (r < 0) {
		p->slot_register = NULL;
		m4_log(M4_LOG_WARN, M4_T("MPRIS: failed to send RegisterPlayer: %s -- retrying in %d ms",
"MPRIS: не удалось отправить RegisterPlayer: %s -- повтор через %d мс"),
		       strerror(-r), p->retry_ms);
		arm_timer(p, p->retry_ms);
	}
}

/* ------------------------------------------------------------------ */
/* org.bluez owner / adapter Media1 tracking */

static void owner_appeared(struct m4_mpris_player *p, const char *owner)
{
	snprintf(p->bluez_owner, sizeof(p->bluez_owner), "%s", owner);
	mark_unregistered(p, M4_T("new org.bluez owner",
"новый владелец org.bluez"));
	m4_log(M4_LOG_INFO, M4_T("MPRIS: %s on the bus (%s), registering in %d ms",
"MPRIS: %s на шине (%s), регистрация через %d мс"), p->service, owner,
	       p->reappear_delay_ms);
	arm_timer(p, p->reappear_delay_ms);
}

static int on_get_owner_reply(sd_bus_message *reply, void *userdata, sd_bus_error *ret_error)
{
	struct m4_mpris_player *p = userdata;
	const char *owner = NULL;

	(void)ret_error;
	p->slot_get_owner = sd_bus_slot_unref(p->slot_get_owner);
	if (sd_bus_message_is_method_error(reply, NULL)) {
		m4_log(M4_LOG_WARN, M4_T("MPRIS: %s is not on the bus right now -- waiting for it to appear",
"MPRIS: %s сейчас нет на шине -- жду его появления"), p->service);
		return 0;
	}
	if (sd_bus_message_read(reply, "s", &owner) < 0 || owner == NULL)
		return 0;
	snprintf(p->bluez_owner, sizeof(p->bluez_owner), "%s", owner);
	do_register(p);
	return 0;
}

static int on_owner_changed(sd_bus_message *m, void *userdata, sd_bus_error *ret_error)
{
	struct m4_mpris_player *p = userdata;
	const char *name = NULL, *old_owner = NULL, *new_owner = NULL;

	(void)ret_error;
	if (sd_bus_message_read(m, "sss", &name, &old_owner, &new_owner) < 0 || name == NULL)
		return 0;
	if (strcmp(name, p->service) != 0)
		return 0;
	if (new_owner == NULL || new_owner[0] == '\0') {
		p->bluez_owner[0] = '\0';
		mark_unregistered(p, M4_T("org.bluez left the bus",
"org.bluez ушёл с шины"));
		m4_log(M4_LOG_WARN, M4_T("MPRIS: %s left the bus (bluetoothd restart?)",
"MPRIS: %s ушёл с шины (перезапуск bluetoothd?)"), p->service);
		return 0;
	}
	owner_appeared(p, new_owner);
	return 0;
}

static bool path_is_adapter(struct m4_mpris_player *p, const char *path)
{
	return path != NULL && strcmp(path, p->adapter_path) == 0;
}

/* `path` == ns or below it ("/org/bluez/hci0/dev_X" covers ".../player0",
 * ".../sep1/fd3" but not "/org/bluez/hci0/dev_X2") */
static bool path_in_ns(const char *ns, const char *path)
{
	size_t n = strlen(ns);

	if (path == NULL || n == 0 || strncmp(path, ns, n) != 0)
		return false;
	return path[n] == '\0' || path[n] == '/';
}

static int on_interfaces_added(sd_bus_message *m, void *userdata, sd_bus_error *ret_error)
{
	struct m4_mpris_player *p = userdata;
	const char *path = NULL;
	bool media = false, remote_player = false;
	bool adapter, device;

	(void)ret_error;
	if (sd_bus_message_read(m, "o", &path) < 0)
		return 0;
	adapter = path_is_adapter(p, path);
	device = !adapter && path_in_ns(p->watch_ns, path);
	if (!adapter && !device)
		return 0;
	if (sd_bus_message_enter_container(m, 'a', "{sa{sv}}") < 0)
		return 0;
	while (sd_bus_message_enter_container(m, 'e', "sa{sv}") > 0) {
		const char *iface = NULL;

		if (sd_bus_message_read(m, "s", &iface) < 0)
			break;
		if (iface != NULL && adapter && strcmp(iface, M4_BLUEZ_MEDIA_IFACE) == 0)
			media = true;
		if (iface != NULL && device && strcmp(iface, "org.bluez.MediaPlayer1") == 0)
			remote_player = true;
		if (sd_bus_message_skip(m, "a{sv}") < 0 || sd_bus_message_exit_container(m) < 0)
			break;
	}
	if (remote_player && p->auto_resync) {
		char why[200];

		snprintf(why, sizeof(why), M4_T("new MediaPlayer1 %s",
"новый MediaPlayer1 %s"), path);
		resync_request(p, why);
	}
	if (media) {
		mark_unregistered(p, M4_T("adapter's Media1 recreated",
"Media1 адаптера пересоздан"));
		m4_log(M4_LOG_INFO, M4_T("MPRIS: %s provides Media1 again -- registering in %d ms",
"MPRIS: %s снова предоставляет Media1 -- регистрация через %d мс"),
		       p->adapter_path, p->reappear_delay_ms);
		arm_timer(p, p->reappear_delay_ms);
	}
	return 0;
}

static int on_interfaces_removed(sd_bus_message *m, void *userdata, sd_bus_error *ret_error)
{
	struct m4_mpris_player *p = userdata;
	const char *path = NULL;
	char **ifaces = NULL;
	bool media = false;

	(void)ret_error;
	if (sd_bus_message_read(m, "o", &path) < 0 || !path_is_adapter(p, path))
		return 0;
	if (sd_bus_message_read_strv(m, &ifaces) < 0)
		return 0;
	for (char **i = ifaces; i != NULL && *i != NULL; i++) {
		if (strcmp(*i, M4_BLUEZ_MEDIA_IFACE) == 0)
			media = true;
		free(*i);
	}
	free(ifaces);
	if (media)
		mark_unregistered(p, M4_T("adapter removed Media1",
"адаптер убрал Media1"));
	return 0;
}

/* PropertiesChanged under watch_ns: M4 (re)connected or A2DP started */
static int on_dev_props_changed(sd_bus_message *m, void *userdata, sd_bus_error *ret_error)
{
	struct m4_mpris_player *p = userdata;
	const char *iface = NULL;
	const char *path = sd_bus_message_get_path(m);
	char why[200];
	bool hit = false;
	enum { W_NONE, W_DEVICE, W_CONTROL, W_TRANSPORT } which;

	(void)ret_error;
	if (!p->auto_resync || !path_in_ns(p->watch_ns, path))
		return 0;
	if (sd_bus_message_read(m, "s", &iface) < 0 || iface == NULL)
		return 0;
	if (strcmp(iface, "org.bluez.Device1") == 0)
		which = W_DEVICE;
	else if (strcmp(iface, "org.bluez.MediaControl1") == 0)
		which = W_CONTROL;
	else if (strcmp(iface, "org.bluez.MediaTransport1") == 0)
		which = W_TRANSPORT;
	else
		return 0;
	if (sd_bus_message_enter_container(m, 'a', "{sv}") < 0)
		return 0;
	while (!hit && sd_bus_message_enter_container(m, 'e', "sv") > 0) {
		const char *key = NULL, *contents = NULL;
		char type = 0;

		if (sd_bus_message_read(m, "s", &key) < 0 || key == NULL)
			break;
		if (sd_bus_message_peek_type(m, &type, &contents) < 0 || contents == NULL)
			break;
		if (which != W_TRANSPORT && strcmp(key, "Connected") == 0 && strcmp(contents, "b") == 0) {
			int b = 0;

			if (sd_bus_message_read(m, "v", "b", &b) < 0)
				break;
			if (b) {
				snprintf(why, sizeof(why), "%s.Connected=true (%s)",
					 which == W_DEVICE ? "Device1" : "MediaControl1", path);
				hit = true;
			}
		} else if (which == W_TRANSPORT && strcmp(key, "State") == 0 && strcmp(contents, "s") == 0) {
			const char *v = NULL;

			if (sd_bus_message_read(m, "v", "s", &v) < 0)
				break;
			if (v != NULL && strcmp(v, "active") == 0) {
				snprintf(why, sizeof(why), "MediaTransport1.State=active (%s)", path);
				hit = true;
			}
		} else if (sd_bus_message_skip(m, "v") < 0) {
			break;
		}
		if (sd_bus_message_exit_container(m) < 0)
			break;
	}
	if (hit)
		resync_request(p, why);
	return 0;
}

/* ------------------------------------------------------------------ */
/* status resync: Stopped -> real value (mpris_player.h) */

static int on_resync_timer(sd_event_source *s, uint64_t usec, void *userdata);

static void resync_arm(struct m4_mpris_player *p, int delay_ms)
{
	uint64_t usec = (uint64_t)(delay_ms > 0 ? delay_ms : 1) * 1000;

	if (p->resync_timer == NULL) {
		if (sd_event_add_time_relative(p->event, &p->resync_timer, CLOCK_MONOTONIC, usec, 10 * 1000,
						on_resync_timer, p) < 0) {
			m4_log(M4_LOG_WARN, M4_T("MPRIS: failed to arm the status resend timer",
"MPRIS: не удалось завести таймер переотправки статуса"));
			p->resync_timer = NULL;
			p->rs_state = RS_IDLE;
			p->blip = false;
		}
		return;
	}
	sd_event_source_set_time_relative(p->resync_timer, usec);
	sd_event_source_set_enabled(p->resync_timer, SD_EVENT_ONESHOT);
}

static void emit_status(struct m4_mpris_player *p)
{
	sd_bus_emit_properties_changed(p->bus, p->object_path, M4_MPRIS_PLAYER_IFACE, "PlaybackStatus", NULL);
}

static void resync_cancel(struct m4_mpris_player *p)
{
	/* nothing to emit: with no registration BlueZ is not listening, and
	 * the next RegisterPlayer carries the real status anyway */
	p->blip = false;
	p->rs_again = false;
	p->rs_state = RS_IDLE;
	if (p->resync_timer != NULL)
		sd_event_source_set_enabled(p->resync_timer, SD_EVENT_OFF);
}

static void resync_request(struct m4_mpris_player *p, const char *reason)
{
	snprintf(p->rs_reason, sizeof(p->rs_reason), "%s", reason != NULL ? reason : "?");
	if (p->rs_state == RS_BLIP) {
		p->rs_again = true;
		m4_log(M4_LOG_DEBUG, M4_T("MPRIS: a status resend is already in progress -- one more after it (%s)",
"MPRIS: переотправка статуса уже идёт -- ещё одна после неё (%s)"),
		       p->rs_reason);
		return;
	}
	p->rs_state = RS_WAIT;
	m4_log(M4_LOG_DEBUG, M4_T("MPRIS: resending status for the M4 in %d ms (%s)",
"MPRIS: переотправка статуса для M4 через %d мс (%s)"), p->resync_delay_ms,
	       p->rs_reason);
	resync_arm(p, p->resync_delay_ms);
}

static int on_resync_timer(sd_event_source *s, uint64_t usec, void *userdata)
{
	struct m4_mpris_player *p = userdata;

	(void)s;
	(void)usec;
	switch (p->rs_state) {
	case RS_WAIT:
		if (!p->registered) {
			m4_log(M4_LOG_DEBUG, M4_T("MPRIS: status resend skipped -- player not registered (%s)",
"MPRIS: переотправка статуса пропущена -- плеер не зарегистрирован (%s)"),
			       p->rs_reason);
			p->rs_state = RS_IDLE;
			return 0;
		}
		if (p->status == M4_PLAYER_STATUS_STOPPED) {
			m4_log(M4_LOG_DEBUG, M4_T("MPRIS: status resend not needed -- status is Stopped (%s)",
"MPRIS: переотправка статуса не нужна -- статус Stopped (%s)"),
			       p->rs_reason);
			p->rs_state = RS_IDLE;
			return 0;
		}
		p->blip = true;
		p->rs_state = RS_BLIP;
		emit_status(p);
		resync_arm(p, p->resync_gap_ms);
		return 0;
	case RS_BLIP:
		p->blip = false;
		p->rs_state = RS_IDLE;
		emit_status(p);
		p->resync_count++;
		m4_log(M4_LOG_INFO, M4_T("MPRIS: status resent for the M4: Stopped -> %s (%s)",
"MPRIS: статус для M4 переотправлен: Stopped -> %s (%s)"),
		       m4_player_status_mpris(p->status), p->rs_reason);
		if (p->rs_again) {
			char again[sizeof(p->rs_reason)];

			memcpy(again, p->rs_reason, sizeof(again)); /* resync_request() rewrites rs_reason */
			p->rs_again = false;
			resync_request(p, again);
		}
		return 0;
	case RS_IDLE:
	default:
		return 0;
	}
}

/* ------------------------------------------------------------------ */
/* command queue -> cb (from a defer source, outside the D-Bus handler) */

static int on_defer(sd_event_source *s, void *userdata)
{
	struct m4_mpris_player *p = userdata;

	while (p->q_len > 0) {
		struct m4_player_event ev = p->queue[p->q_head];

		p->q_head = (p->q_head + 1) % QUEUE_CAP;
		p->q_len--;
		if (p->cb != NULL)
			p->cb(p->user_data, &ev);
	}
	sd_event_source_set_enabled(s, SD_EVENT_OFF);
	return 0;
}

static void enqueue(struct m4_mpris_player *p, m4_player_cmd_t cmd, const char *sender)
{
	struct m4_player_event *ev;

	if (p->q_len == QUEUE_CAP) {
		m4_log(M4_LOG_WARN, M4_T("MPRIS: command queue full -- %s dropped",
"MPRIS: очередь команд переполнена -- %s отброшена"),
		       m4_player_cmd_method(cmd));
		return;
	}
	ev = &p->queue[(p->q_head + p->q_len) % QUEUE_CAP];
	p->q_len++;
	memset(ev, 0, sizeof(*ev));
	ev->cmd = cmd;
	snprintf(ev->sender, sizeof(ev->sender), "%s", sender != NULL ? sender : "?");
	ev->realtime_usec = realtime_usec();
	ev->monotonic_s = monotonic_s();
	if (p->defer != NULL)
		sd_event_source_set_enabled(p->defer, SD_EVENT_ON);
}

/* ------------------------------------------------------------------ */
/* exported object */

static int method_cmd(sd_bus_message *m, void *userdata, sd_bus_error *ret_error)
{
	struct m4_mpris_player *p = userdata;
	const char *member = sd_bus_message_get_member(m);
	const char *sender = sd_bus_message_get_sender(m);
	m4_player_cmd_t cmd;

	if (m4_player_cmd_from_method(member, &cmd) < 0)
		return sd_bus_error_set(ret_error, SD_BUS_ERROR_UNKNOWN_METHOD, "unknown method");

	if (!p->accept_any_sender && p->bluez_owner[0] != '\0' &&
	    (sender == NULL || strcmp(sender, p->bluez_owner) != 0)) {
		p->rejected_count++;
		m4_log(M4_LOG_WARN, M4_T("MPRIS: %s from %s rejected -- not BlueZ (%s)",
"MPRIS: %s от %s отклонён -- не BlueZ (%s)"), member,
		       sender != NULL ? sender : "?", p->bluez_owner);
		return sd_bus_error_set(ret_error, SD_BUS_ERROR_ACCESS_DENIED,
					"momentum4 player accepts commands from BlueZ only");
	}

	enqueue(p, cmd, sender);
	return sd_bus_reply_method_return(m, NULL);
}

static int method_noop(sd_bus_message *m, void *userdata, sd_bus_error *ret_error)
{
	(void)userdata;
	(void)ret_error;
	return sd_bus_reply_method_return(m, NULL);
}

static int prop_status(sd_bus *bus, const char *path, const char *interface, const char *property,
		       sd_bus_message *reply, void *userdata, sd_bus_error *ret_error)
{
	struct m4_mpris_player *p = userdata;

	(void)bus;
	(void)path;
	(void)interface;
	(void)property;
	(void)ret_error;
	return sd_bus_message_append(reply, "s",
				     m4_player_status_mpris(p->blip ? M4_PLAYER_STATUS_STOPPED : p->status));
}

static int prop_true(sd_bus *bus, const char *path, const char *interface, const char *property,
		     sd_bus_message *reply, void *userdata, sd_bus_error *ret_error)
{
	(void)bus; (void)path; (void)interface; (void)property; (void)userdata; (void)ret_error;
	return sd_bus_message_append(reply, "b", 1);
}

static int prop_false(sd_bus *bus, const char *path, const char *interface, const char *property,
		      sd_bus_message *reply, void *userdata, sd_bus_error *ret_error)
{
	(void)bus; (void)path; (void)interface; (void)property; (void)userdata; (void)ret_error;
	return sd_bus_message_append(reply, "b", 0);
}

static int prop_one(sd_bus *bus, const char *path, const char *interface, const char *property,
		    sd_bus_message *reply, void *userdata, sd_bus_error *ret_error)
{
	(void)bus; (void)path; (void)interface; (void)property; (void)userdata; (void)ret_error;
	return sd_bus_message_append(reply, "d", 1.0);
}

static int prop_zero_pos(sd_bus *bus, const char *path, const char *interface, const char *property,
			 sd_bus_message *reply, void *userdata, sd_bus_error *ret_error)
{
	(void)bus; (void)path; (void)interface; (void)property; (void)userdata; (void)ret_error;
	return sd_bus_message_append(reply, "x", (int64_t)0);
}

static int prop_metadata(sd_bus *bus, const char *path, const char *interface, const char *property,
			 sd_bus_message *reply, void *userdata, sd_bus_error *ret_error)
{
	(void)bus; (void)path; (void)interface; (void)property; (void)userdata; (void)ret_error;
	return sd_bus_message_append(reply, "a{sv}", 1, "mpris:trackid", "o",
				     "/org/mpris/MediaPlayer2/TrackList/NoTrack");
}

static int prop_identity(sd_bus *bus, const char *path, const char *interface, const char *property,
			 sd_bus_message *reply, void *userdata, sd_bus_error *ret_error)
{
	struct m4_mpris_player *p = userdata;

	(void)bus; (void)path; (void)interface; (void)property; (void)ret_error;
	return sd_bus_message_append(reply, "s", p->identity);
}

static int prop_empty_as(sd_bus *bus, const char *path, const char *interface, const char *property,
			 sd_bus_message *reply, void *userdata, sd_bus_error *ret_error)
{
	(void)bus; (void)path; (void)interface; (void)property; (void)userdata; (void)ret_error;
	return sd_bus_message_append(reply, "as", 0);
}

#define U SD_BUS_VTABLE_UNPRIVILEGED
#define C SD_BUS_VTABLE_PROPERTY_CONST

static const sd_bus_vtable player_vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD("Next", "", "", method_cmd, U),
	SD_BUS_METHOD("Previous", "", "", method_cmd, U),
	SD_BUS_METHOD("Pause", "", "", method_cmd, U),
	SD_BUS_METHOD("PlayPause", "", "", method_cmd, U),
	SD_BUS_METHOD("Stop", "", "", method_cmd, U),
	SD_BUS_METHOD("Play", "", "", method_cmd, U),
	SD_BUS_METHOD("Seek", "x", "", method_noop, U),
	SD_BUS_METHOD("SetPosition", "ox", "", method_noop, U),
	SD_BUS_METHOD("OpenUri", "s", "", method_noop, U),
	SD_BUS_PROPERTY("PlaybackStatus", "s", prop_status, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
	SD_BUS_PROPERTY("Rate", "d", prop_one, 0, C),
	SD_BUS_PROPERTY("MinimumRate", "d", prop_one, 0, C),
	SD_BUS_PROPERTY("MaximumRate", "d", prop_one, 0, C),
	SD_BUS_PROPERTY("Metadata", "a{sv}", prop_metadata, 0, C),
	SD_BUS_PROPERTY("Position", "x", prop_zero_pos, 0, SD_BUS_VTABLE_PROPERTY_EMITS_INVALIDATION),
	SD_BUS_PROPERTY("CanGoNext", "b", prop_true, 0, C),
	SD_BUS_PROPERTY("CanGoPrevious", "b", prop_true, 0, C),
	SD_BUS_PROPERTY("CanPlay", "b", prop_true, 0, C),
	SD_BUS_PROPERTY("CanPause", "b", prop_true, 0, C),
	SD_BUS_PROPERTY("CanSeek", "b", prop_false, 0, C),
	SD_BUS_PROPERTY("CanControl", "b", prop_true, 0, C),
	SD_BUS_VTABLE_END,
};

static const sd_bus_vtable root_vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD("Raise", "", "", method_noop, U),
	SD_BUS_METHOD("Quit", "", "", method_noop, U),
	SD_BUS_PROPERTY("CanQuit", "b", prop_false, 0, C),
	SD_BUS_PROPERTY("CanRaise", "b", prop_false, 0, C),
	SD_BUS_PROPERTY("HasTrackList", "b", prop_false, 0, C),
	SD_BUS_PROPERTY("Identity", "s", prop_identity, 0, C),
	SD_BUS_PROPERTY("SupportedUriSchemes", "as", prop_empty_as, 0, C),
	SD_BUS_PROPERTY("SupportedMimeTypes", "as", prop_empty_as, 0, C),
	SD_BUS_VTABLE_END,
};

#undef U
#undef C

/* ------------------------------------------------------------------ */

struct m4_mpris_player *m4_mpris_player_new(sd_event *event, const struct m4_mpris_player_config *cfg)
{
	struct m4_mpris_player *p;
	char match[384];
	int r;

	if (event == NULL || cfg == NULL || cfg->bus == NULL)
		return NULL;
	p = calloc(1, sizeof(*p));
	if (p == NULL)
		return NULL;

	p->event = sd_event_ref(event);
	p->bus = sd_bus_ref(cfg->bus);
	snprintf(p->service, sizeof(p->service), "%s", cfg->bluez_service ? cfg->bluez_service : M4_BLUEZ_SERVICE);
	snprintf(p->adapter_path, sizeof(p->adapter_path), "%s",
		 cfg->adapter_path ? cfg->adapter_path : "/org/bluez/hci0");
	snprintf(p->object_path, sizeof(p->object_path), "%s",
		 cfg->object_path ? cfg->object_path : M4_MPRIS_PLAYER_PATH);
	snprintf(p->identity, sizeof(p->identity), "%s", cfg->identity ? cfg->identity : "momentum4");
	p->retry_ms = cfg->retry_ms > 0 ? cfg->retry_ms : 10000;
	p->reappear_delay_ms = cfg->reappear_delay_ms > 0 ? cfg->reappear_delay_ms : 1000;
	p->accept_any_sender = cfg->accept_any_sender;
	p->cb = cfg->cb;
	p->user_data = cfg->user_data;
	p->status = cfg->initial_status == M4_PLAYER_STATUS_FOLLOW ? M4_PLAYER_STATUS_PLAYING
								   : cfg->initial_status;
	snprintf(p->watch_ns, sizeof(p->watch_ns), "%s",
		 cfg->device_path != NULL && cfg->device_path[0] != '\0' ? cfg->device_path : p->adapter_path);
	p->auto_resync = cfg->auto_resync;
	p->resync_delay_ms = cfg->resync_delay_ms > 0 ? cfg->resync_delay_ms : RESYNC_DELAY_MS_DEFAULT;
	p->resync_gap_ms = cfg->resync_gap_ms > 0 ? cfg->resync_gap_ms : RESYNC_GAP_MS_DEFAULT;

	r = sd_bus_add_object_vtable(p->bus, &p->slot_player_vtable, p->object_path, M4_MPRIS_PLAYER_IFACE,
				     player_vtable, p);
	if (r >= 0)
		r = sd_bus_add_object_vtable(p->bus, &p->slot_root_vtable, p->object_path, M4_MPRIS_ROOT_IFACE,
					     root_vtable, p);
	if (r < 0) {
		m4_log(M4_LOG_WARN, M4_T("MPRIS: failed to export %s: %s",
"MPRIS: не удалось экспортировать %s: %s"), p->object_path, strerror(-r));
		m4_mpris_player_free(p);
		return NULL;
	}

	r = sd_event_add_defer(p->event, &p->defer, on_defer, p);
	if (r < 0) {
		m4_log(M4_LOG_WARN, "MPRIS: sd_event_add_defer: %s", strerror(-r));
		m4_mpris_player_free(p);
		return NULL;
	}
	sd_event_source_set_enabled(p->defer, SD_EVENT_OFF);

	snprintf(match, sizeof(match),
		 "type='signal',sender='org.freedesktop.DBus',path='/org/freedesktop/DBus',"
		 "interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='%s'",
		 p->service);
	r = sd_bus_add_match_async(p->bus, &p->slot_owner_match, match, on_owner_changed, NULL, p);
	if (r >= 0) {
		snprintf(match, sizeof(match),
			 "type='signal',sender='%s',interface='org.freedesktop.DBus.ObjectManager',"
			 "member='InterfacesAdded'",
			 p->service);
		r = sd_bus_add_match_async(p->bus, &p->slot_ifadded_match, match, on_interfaces_added, NULL, p);
	}
	if (r >= 0) {
		snprintf(match, sizeof(match),
			 "type='signal',sender='%s',interface='org.freedesktop.DBus.ObjectManager',"
			 "member='InterfacesRemoved'",
			 p->service);
		r = sd_bus_add_match_async(p->bus, &p->slot_ifremoved_match, match, on_interfaces_removed, NULL,
					   p);
	}
	if (r >= 0) {
		/* always subscribed (cheap); on_dev_props_changed() checks
		 * auto_resync itself, so set_auto_resync() needs no re-match */
		snprintf(match, sizeof(match),
			 "type='signal',sender='%s',interface='org.freedesktop.DBus.Properties',"
			 "member='PropertiesChanged',path_namespace='%s'",
			 p->service, p->watch_ns);
		r = sd_bus_add_match_async(p->bus, &p->slot_dev_props_match, match, on_dev_props_changed, NULL,
					   p);
	}
	if (r >= 0)
		r = sd_bus_call_method_async(p->bus, &p->slot_get_owner, "org.freedesktop.DBus",
					     "/org/freedesktop/DBus", "org.freedesktop.DBus", "GetNameOwner",
					     on_get_owner_reply, p, "s", p->service);
	if (r < 0) {
		m4_log(M4_LOG_WARN, M4_T("MPRIS: failed to subscribe to bus signals: %s",
"MPRIS: не удалось подписаться на сигналы шины: %s"), strerror(-r));
		m4_mpris_player_free(p);
		return NULL;
	}
	return p;
}

void m4_mpris_player_free(struct m4_mpris_player *p)
{
	if (p == NULL)
		return;

	if (p->registered && p->bus != NULL) {
		/* fire-and-forget: BlueZ drops the player on disconnect anyway */
		sd_bus_message *m = NULL;

		if (sd_bus_message_new_method_call(p->bus, &m, p->service, p->adapter_path,
						   M4_BLUEZ_MEDIA_IFACE, "UnregisterPlayer") >= 0 &&
		    sd_bus_message_append(m, "o", p->object_path) >= 0) {
			sd_bus_message_set_expect_reply(m, 0);
			sd_bus_send(p->bus, m, NULL);
			sd_bus_flush(p->bus);
		}
		sd_bus_message_unref(m);
	}

	sd_bus_slot_unref(p->slot_register);
	sd_bus_slot_unref(p->slot_get_owner);
	sd_bus_slot_unref(p->slot_dev_props_match);
	sd_event_source_unref(p->resync_timer);
	sd_bus_slot_unref(p->slot_ifremoved_match);
	sd_bus_slot_unref(p->slot_ifadded_match);
	sd_bus_slot_unref(p->slot_owner_match);
	sd_bus_slot_unref(p->slot_root_vtable);
	sd_bus_slot_unref(p->slot_player_vtable);
	sd_event_source_unref(p->retry_timer);
	sd_event_source_unref(p->defer);
	sd_bus_unref(p->bus);
	sd_event_unref(p->event);
	free(p);
}

void m4_mpris_player_set_status(struct m4_mpris_player *p, m4_player_status_policy_t concrete)
{
	if (p == NULL)
		return;
	if (concrete == M4_PLAYER_STATUS_FOLLOW)
		concrete = M4_PLAYER_STATUS_STOPPED;
	if (p->status == concrete)
		return;
	p->status = concrete;
	if (p->blip)
		return; /* the resync's second half announces the new value */
	emit_status(p);
}

void m4_mpris_player_resync(struct m4_mpris_player *p, const char *reason)
{
	if (p == NULL)
		return;
	resync_request(p, reason);
}

void m4_mpris_player_set_auto_resync(struct m4_mpris_player *p, bool on)
{
	if (p != NULL)
		p->auto_resync = on;
}

int m4_mpris_player_resync_count(const struct m4_mpris_player *p)
{
	return p != NULL ? p->resync_count : 0;
}

bool m4_mpris_player_is_registered(const struct m4_mpris_player *p)
{
	return p != NULL && p->registered;
}

int m4_mpris_player_register_count(const struct m4_mpris_player *p)
{
	return p != NULL ? p->register_count : 0;
}

int m4_mpris_player_rejected_count(const struct m4_mpris_player *p)
{
	return p != NULL ? p->rejected_count : 0;
}
