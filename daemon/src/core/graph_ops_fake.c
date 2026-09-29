/* core/graph_ops_fake.c -- see graph_ops_fake.h.
 *
 * `struct graph_ops` (unlike m4_callfsm_ops_t) has no per-instance `ctx`
 * field in its function pointers -- m4_graph_ops_pw itself is a single
 * process-wide `extern const struct graph_ops`, not something constructed
 * per-caller. To hand out an equally shaped fake, m4_graph_ops_fake(fake)
 * stashes `fake` in a single static "which fake is about to be open()ed"
 * slot and returns a static vtable whose open() reads that slot and copies
 * the pointer into the handle it allocates -- after open() returns, the
 * handle is fully independent of the static slot (only open() itself reads
 * it), so this is safe for SEQUENTIAL single-threaded tests (open handle A
 * against fake A, use it, close it, THEN call m4_graph_ops_fake(&fakeB) and
 * open handle B) but not for concurrently open()ing two different fakes at
 * once, or from more than one thread -- fine for this project's tests
 * (meson test runs each test binary single-threaded, and no test in this
 * tree opens two independently-configured fakes concurrently).
 */
#include "graph_ops_fake.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static m4_graph_ops_fake_t *g_next_fake;

struct m4_graph_ops_fake_handle {
	m4_graph_ops_fake_t *fake;
};

void m4_graph_ops_fake_init(m4_graph_ops_fake_t *fake)
{
	memset(fake, 0, sizeof(*fake));
	fake->m4_present = true;
	fake->m4_connected = true;

	fake->n_profiles = 3;
	fake->profiles[0] = (struct m4_graph_profile){ .index = 0, .available = true };
	snprintf(fake->profiles[0].name, sizeof(fake->profiles[0].name), "a2dp-sink");
	snprintf(fake->profiles[0].description, sizeof(fake->profiles[0].description), "A2DP");
	fake->profiles[1] = (struct m4_graph_profile){ .index = 1, .available = true };
	snprintf(fake->profiles[1].name, sizeof(fake->profiles[1].name), "headset-head-unit");
	snprintf(fake->profiles[1].description, sizeof(fake->profiles[1].description), "HFP mSBC");
	fake->profiles[2] = (struct m4_graph_profile){ .index = 2, .available = true };
	snprintf(fake->profiles[2].name, sizeof(fake->profiles[2].name), "headset-head-unit-cvsd");
	snprintf(fake->profiles[2].description, sizeof(fake->profiles[2].description), "HFP CVSD");

	fake->have_profile = true;
	snprintf(fake->profile_name, sizeof(fake->profile_name), "a2dp-sink");
}

static void notify(m4_graph_ops_fake_t *fake)
{
	struct m4_graph_state st = {0};

	if (fake->cb.on_state_changed == NULL)
		return;

	st.m4_present = fake->m4_present;
	st.m4_connected = fake->m4_connected;
	st.have_profile = fake->have_profile;
	snprintf(st.profile_name, sizeof(st.profile_name), "%s", fake->profile_name);
	st.n_profiles = fake->n_profiles;
	memcpy(st.profiles, fake->profiles, fake->n_profiles * sizeof(fake->profiles[0]));

	st.have_mic_route = fake->mic_have_route;
	st.mic_volume = fake->mic_volume;
	st.mic_mute = fake->mic_mute;

	fake->cb.on_state_changed(fake->user_data, &st);
}

void m4_graph_ops_fake_set_mic_route(m4_graph_ops_fake_t *fake, bool have_route, float volume, bool mute)
{
	fake->mic_have_route = have_route;
	if (have_route) {
		fake->mic_volume = volume;
		fake->mic_mute = mute;
	} else {
		fake->mic_volume = 0.0f;
		fake->mic_mute = false;
	}
	notify(fake);
}

void m4_graph_ops_fake_set_profile_external(m4_graph_ops_fake_t *fake, const char *profile_name)
{
	fake->have_profile = true;
	snprintf(fake->profile_name, sizeof(fake->profile_name), "%s",
		 profile_name != NULL ? profile_name : "");
	notify(fake);
}

void m4_graph_ops_fake_set_present(m4_graph_ops_fake_t *fake, bool present, bool connected)
{
	fake->m4_present = present;
	fake->m4_connected = present && connected;
	if (!present) {
		fake->have_profile = false;
		fake->profile_name[0] = '\0';
	}
	notify(fake);
}

static void *fake_open(sd_event *event, const char *mac_address,
			const struct m4_graph_callbacks *cb, void *user_data)
{
	struct m4_graph_ops_fake_handle *h;

	(void)event;
	(void)mac_address;
	if (g_next_fake == NULL)
		return NULL;

	h = calloc(1, sizeof(*h));
	if (h == NULL)
		return NULL;
	h->fake = g_next_fake;
	if (cb != NULL)
		h->fake->cb = *cb;
	h->fake->user_data = user_data;
	h->fake->open_calls++;

	/* Deliver the initial state right away, synchronously -- mirrors
	 * m4_graph_ops_pw's contract ("the first on_state_changed() call once
	 * the registry burst has been processed"), just without an actual
	 * burst to wait for. */
	notify(h->fake);
	return h;
}

static void fake_close(void *handle)
{
	struct m4_graph_ops_fake_handle *h = handle;

	if (h == NULL)
		return;
	h->fake->close_calls++;
	h->fake->cb.on_state_changed = NULL;
	free(h);
}

static enum m4_graph_status fake_get_state(void *handle, struct m4_graph_state *out)
{
	struct m4_graph_ops_fake_handle *h = handle;

	if (h == NULL || out == NULL)
		return M4_GRAPH_ERR_INVALID;

	memset(out, 0, sizeof(*out));
	out->m4_present = h->fake->m4_present;
	out->m4_connected = h->fake->m4_connected;
	out->have_profile = h->fake->have_profile;
	snprintf(out->profile_name, sizeof(out->profile_name), "%s", h->fake->profile_name);
	out->n_profiles = h->fake->n_profiles;
	memcpy(out->profiles, h->fake->profiles, h->fake->n_profiles * sizeof(h->fake->profiles[0]));
	out->have_mic_route = h->fake->mic_have_route;
	out->mic_volume = h->fake->mic_volume;
	out->mic_mute = h->fake->mic_mute;
	return M4_GRAPH_OK;
}

static enum m4_graph_status fake_set_profile(void *handle, const char *profile_name, bool save)
{
	struct m4_graph_ops_fake_handle *h = handle;

	if (h == NULL || profile_name == NULL || profile_name[0] == '\0')
		return M4_GRAPH_ERR_INVALID;
	if (!h->fake->m4_present)
		return M4_GRAPH_ERR_NOT_FOUND;

	h->fake->set_profile_calls++;
	snprintf(h->fake->last_set_profile_name, sizeof(h->fake->last_set_profile_name), "%s",
		 profile_name);
	h->fake->last_set_profile_save = save;

	if (h->fake->fail_next_set_profile) {
		h->fake->fail_next_set_profile = false;
		return M4_GRAPH_OK; /* "issued", like the real thing -- just never confirms */
	}

	if (h->fake->async_set_profile) {
		h->fake->pending_set_profile = true;
		snprintf(h->fake->pending_profile_name, sizeof(h->fake->pending_profile_name), "%s",
			 profile_name);
		return M4_GRAPH_OK;
	}

	/* Linear-scan the fake's own EnumProfile list (deliberately not using
	 * m4_graph_find_profile_index() -- that helper takes a `const struct
	 * m4_graph_state *`, and building one here just to throw it away
	 * would need a second copy of profiles[]; both look up the same
	 * name/index pair). */
	for (size_t i = 0; i < h->fake->n_profiles; i++) {
		if (strcmp(h->fake->profiles[i].name, profile_name) == 0) {
			h->fake->have_profile = true;
			snprintf(h->fake->profile_name, sizeof(h->fake->profile_name), "%s",
				 profile_name);
			notify(h->fake);
			return M4_GRAPH_OK;
		}
	}
	return M4_GRAPH_ERR_NO_SUCH_PROFILE;
}

static enum m4_graph_status fake_set_mic_volume(void *handle, float volume, bool mute, bool save)
{
	struct m4_graph_ops_fake_handle *h = handle;

	if (h == NULL)
		return M4_GRAPH_ERR_INVALID;
	if (!h->fake->m4_present)
		return M4_GRAPH_ERR_NOT_FOUND;

	if (volume < 0.0f)
		volume = 0.0f;
	if (volume > 1.0f)
		volume = 1.0f;

	h->fake->set_mic_volume_calls++;
	h->fake->last_set_mic_volume = volume;
	h->fake->last_set_mic_mute = mute;
	h->fake->last_set_mic_save = save;

	/* Unlike the real bluez5-device.c (apply_device_props()'s route
	 * lookup fails with -EINVAL if the device has no input direction in
	 * the active profile), the fake applies unconditionally, matching its
	 * own documented "deliberately simpler than the real ABI" level (see
	 * graph_ops_fake.h) -- have_mic_route becomes true here too, so a
	 * caller that writes before ever observing a route still sees a
	 * consistent get_state() afterwards. */
	h->fake->mic_have_route = true;
	h->fake->mic_volume = volume;
	h->fake->mic_mute = mute;
	notify(h->fake);
	return M4_GRAPH_OK;
}

static int fake_iterate(void *handle, int timeout_ms)
{
	struct m4_graph_ops_fake_handle *h = handle;

	(void)timeout_ms;
	if (h == NULL)
		return -EINVAL;
	if (!h->fake->iterate_enabled)
		return -ENOTSUP;
	h->fake->iterate_calls++;
	if (h->fake->pending_set_profile) {
		h->fake->pending_set_profile = false;
		for (size_t i = 0; i < h->fake->n_profiles; i++) {
			if (strcmp(h->fake->profiles[i].name, h->fake->pending_profile_name) == 0) {
				h->fake->have_profile = true;
				snprintf(h->fake->profile_name, sizeof(h->fake->profile_name), "%s",
					 h->fake->pending_profile_name);
				notify(h->fake);
				break;
			}
		}
	}
	return 0;
}

static const struct graph_ops M4_GRAPH_OPS_FAKE_VTABLE = {
	.open = fake_open,
	.close = fake_close,
	.get_state = fake_get_state,
	.set_profile = fake_set_profile,
	.set_mic_volume = fake_set_mic_volume,
	.iterate = fake_iterate,
};

const struct graph_ops *m4_graph_ops_fake(m4_graph_ops_fake_t *fake)
{
	g_next_fake = fake;
	return &M4_GRAPH_OPS_FAKE_VTABLE;
}
