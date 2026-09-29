/* control/passthrough.c -- see passthrough.h. */
#include "passthrough.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../util/log.h"
#include "../../../common/m4-i18n.h"

#define CALL_TIMEOUT_USEC (300ULL * 1000)

int m4_passthrough_pick(const struct m4_mpris_candidate *c, size_t n)
{
	static const char *const pref[] = { "Playing", "Paused" };
	int best = -1;

	for (size_t k = 0; k < sizeof(pref) / sizeof(pref[0]); k++) {
		for (size_t i = 0; i < n; i++) {
			if (strcmp(c[i].status, pref[k]) != 0)
				continue;
			if (best < 0 || strcmp(c[i].name, c[best].name) < 0)
				best = (int)i;
		}
		if (best >= 0)
			return best;
	}
	for (size_t i = 0; i < n; i++) {
		if (best < 0 || strcmp(c[i].name, c[best].name) < 0)
			best = (int)i;
	}
	return best;
}

struct m4_passthrough_sdbus {
	sd_bus *bus;
	bool own_bus; /* opened by us (session bus) -> may be reopened */
};

static sd_bus *get_bus(struct m4_passthrough_sdbus *pt)
{
	if (pt->bus != NULL && sd_bus_is_open(pt->bus) > 0)
		return pt->bus;
	if (!pt->own_bus)
		return NULL;
	pt->bus = sd_bus_flush_close_unref(pt->bus);
	if (sd_bus_open_user(&pt->bus) < 0) {
		pt->bus = NULL;
		return NULL;
	}
	return pt->bus;
}

static int list_candidates(sd_bus *bus, struct m4_mpris_candidate *out, size_t cap, size_t *n_out)
{
	sd_bus_error err = SD_BUS_ERROR_NULL;
	sd_bus_message *reply = NULL;
	char **names = NULL;
	size_t n = 0;
	int r;

	r = sd_bus_call_method(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
			       "ListNames", &err, &reply, "");
	sd_bus_error_free(&err);
	if (r < 0)
		return r;
	r = sd_bus_message_read_strv(reply, &names);
	sd_bus_message_unref(reply);
	if (r < 0)
		return r;

	for (char **i = names; i != NULL && *i != NULL; i++) {
		if (n < cap && strncmp(*i, M4_MPRIS_NAME_PREFIX, strlen(M4_MPRIS_NAME_PREFIX)) == 0) {
			/* plain Properties.Get with our own short timeout (the
			 * sd_bus_get_property*() helpers use the bus default,
			 * 25 s) -- a hung player must not stall the daemon */
			sd_bus_error gerr = SD_BUS_ERROR_NULL;
			sd_bus_message *m = NULL, *rep = NULL;
			const char *s = NULL;

			memset(&out[n], 0, sizeof(out[n]));
			snprintf(out[n].name, sizeof(out[n].name), "%s", *i);
			if (sd_bus_message_new_method_call(bus, &m, *i, "/org/mpris/MediaPlayer2",
							   "org.freedesktop.DBus.Properties", "Get") >= 0 &&
			    sd_bus_message_append(m, "ss", "org.mpris.MediaPlayer2.Player", "PlaybackStatus") >= 0 &&
			    sd_bus_call(bus, m, CALL_TIMEOUT_USEC, &gerr, &rep) >= 0 &&
			    sd_bus_message_read(rep, "v", "s", &s) >= 0 && s != NULL)
				snprintf(out[n].status, sizeof(out[n].status), "%s", s);
			sd_bus_message_unref(rep);
			sd_bus_message_unref(m);
			sd_bus_error_free(&gerr);
			n++;
		}
		free(*i);
	}
	free(names);
	*n_out = n;
	return 0;
}

static int sdbus_forward(void *ctx, const char *method, char *target, size_t target_cap)
{
	struct m4_passthrough_sdbus *pt = ctx;
	struct m4_mpris_candidate c[M4_PASSTHROUGH_MAX_CANDIDATES];
	sd_bus_message *m = NULL;
	sd_bus *bus;
	size_t n = 0;
	int idx, r;

	if (target_cap > 0)
		target[0] = '\0';
	bus = get_bus(pt);
	if (bus == NULL) {
		snprintf(target, target_cap, M4_T("session D-Bus bus unavailable",
"сессионная шина D-Bus недоступна"));
		return -1;
	}
	r = list_candidates(bus, c, M4_PASSTHROUGH_MAX_CANDIDATES, &n);
	if (r < 0) {
		snprintf(target, target_cap, "ListNames: %s", strerror(-r));
		return -1;
	}
	idx = m4_passthrough_pick(c, n);
	if (idx < 0)
		return 1;

	snprintf(target, target_cap, "%s (%s)", c[idx].name, c[idx].status[0] ? c[idx].status : "?");
	r = sd_bus_message_new_method_call(bus, &m, c[idx].name, "/org/mpris/MediaPlayer2",
					   "org.mpris.MediaPlayer2.Player", method);
	if (r >= 0) {
		sd_bus_message_set_expect_reply(m, 0);
		r = sd_bus_send(bus, m, NULL);
	}
	sd_bus_message_unref(m);
	if (r >= 0)
		sd_bus_flush(bus);
	if (r < 0) {
		snprintf(target, target_cap, "%s: %s", c[idx].name, strerror(-r));
		return -1;
	}
	return 0;
}

struct m4_passthrough_sdbus *m4_passthrough_sdbus_new(sd_bus *bus)
{
	struct m4_passthrough_sdbus *pt = calloc(1, sizeof(*pt));

	if (pt == NULL)
		return NULL;
	if (bus != NULL) {
		pt->bus = sd_bus_ref(bus);
		pt->own_bus = false;
	} else {
		pt->own_bus = true;
	}
	return pt;
}

void m4_passthrough_sdbus_free(struct m4_passthrough_sdbus *pt)
{
	if (pt == NULL)
		return;
	if (pt->own_bus)
		sd_bus_flush_close_unref(pt->bus);
	else
		sd_bus_unref(pt->bus);
	free(pt);
}

struct m4_passthrough_ops m4_passthrough_sdbus_ops(struct m4_passthrough_sdbus *pt)
{
	struct m4_passthrough_ops ops = { .ctx = pt, .forward = sdbus_forward };
	return ops;
}
