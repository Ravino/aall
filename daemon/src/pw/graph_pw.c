#include "graph_pw.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include <spa/param/profile.h>
#include <spa/param/props.h>
#include <spa/param/route.h>
#include <spa/pod/builder.h>
#include <spa/pod/parser.h>
#include <spa/utils/dict.h>
#include <spa/utils/keys.h>
#include <spa/utils/result.h>
#include <spa/utils/string.h>

#include <pipewire/pipewire.h>

#include "loop_glue.h"
#include "../util/log.h"
#include "../../../common/m4-i18n.h"

/* Two-step sync (registry read, then bound
 * objects' params/info, nothing else):
 *  step 1 -- fired once the initial burst of `registry.global` events
 *             (which always precede a `core.done` for any sync queued after
 *             get_registry(), server processes requests in order) has been
 *             delivered. If we found the M4 Device global in that burst,
 *             bind it (and its bluez5 Sink Node, if any) and ask for their
 *             params/info; otherwise we're done (no M4).
 *  step 2 -- fired once the bound objects' replies (queued in the step-1
 *             handler) have all arrived. Always the last step.
 *
 * IMPORTANT (found by testing against the live system, not just reading the
 * header): the `seq` in a `core.done` event is NOT the literal int you pass
 * to pw_core_sync()'s third argument echoed back -- pw_core_sync() is one of
 * the "async" methods, whose actual pending id is only known from ITS OWN
 * RETURN VALUE (see PW_API_CORE_IMPL's spa_api_method_r() wrapping and
 * SPA_RESULT_IS_ASYNC()/SPA_RESULT_ASYNC_SEQ() in spa/utils/result.h). Every
 * real caller in this PipeWire tree (e.g.
 * module-protocol-pulse/manager.c:72, "m->sync_seq = pw_core_sync(...)")
 * captures the return value and compares THAT against the done event's
 * seq -- not the input. Tracked explicitly below instead of hardcoding
 * phase numbers. */

/* ALSO found by testing against the live system: `registry.global`'s own
 * `props` argument is a REDUCED set, not the object's full property set --
 * for a bluez5 card Device it only ever contained
 * object.serial/factory.id/client.id/device.api/device.description/
 * device.name/media.class (no api.bluez5.address/connection at all), and for
 * its child Node only object.serial/factory.id/client.id/device.id/
 * priority.session, priority.driver, node.description, node.name, media.class
 * (no api.bluez5.codec).
 * `pw-dump`/WirePlumber see the fuller set because they BIND every object
 * and read it from the bound object's own `info` event's `props`, which
 * *is* the complete, live-updated set -- that is what this file does too:
 * identify the M4 Device/Node by the always-present, structurally-derived
 * keys (`device.name` built from the MAC the same way bluez5-device.c does;
 * a child Node's `device.id`), then bind them to read api.bluez5.address/
 * connection/codec from their `info` events. */
#define M4_PW_MAX_SINK_CANDIDATES 32

struct m4_sink_candidate {
	uint32_t id;
	uint32_t device_id;
};

struct m4_pw_query_ctx {
	const char *mac_address;
	char device_name[80]; /* "bluez_card." + mac with ':' -> '_' */
	struct m4_pw_snapshot *out;

	sd_event *event;
	struct pw_core *core;
	struct pw_registry *registry;

	struct pw_proxy *device_proxy;
	struct spa_hook device_listener;
	struct pw_proxy *node_proxy;
	struct spa_hook node_listener;

	struct spa_hook core_listener;
	struct spa_hook registry_listener;

	uint32_t device_id;
	bool device_found;
	bool device_bound;

	struct m4_sink_candidate sink_candidates[M4_PW_MAX_SINK_CANDIDATES];
	unsigned n_sink_candidates;
	uint32_t node_id;
	bool node_found;
	bool node_bound;

	int pending_sync_seq;  /* what pw_core_sync() last returned */
	bool awaiting_bound;   /* false: pending_sync_seq is the phase-1 (globals)
				 * sync; true: it is the phase-2 (bound object) sync */
};

static void finish(struct m4_pw_query_ctx *ctx)
{
	sd_event_exit(ctx->event, 0);
}

static void bind_device_and_node(struct m4_pw_query_ctx *ctx);

static void on_core_done(void *data, uint32_t id, int seq)
{
	struct m4_pw_query_ctx *ctx = data;

	if (id != PW_ID_CORE || seq != ctx->pending_sync_seq)
		return;

	if (ctx->awaiting_bound) {
		finish(ctx);
		return;
	}

	if (ctx->device_found) {
		bind_device_and_node(ctx);
		if (ctx->device_bound || ctx->node_bound) {
			ctx->awaiting_bound = true;
			ctx->pending_sync_seq = pw_core_sync(ctx->core, PW_ID_CORE, ctx->pending_sync_seq);
			return;
		}
	}
	finish(ctx);
}

static void on_core_error(void *data, uint32_t id, int seq, int res, const char *message)
{
	struct m4_pw_query_ctx *ctx = data;
	(void)seq;

	/* id==PW_ID_CORE errors are fatal to this query; errors about some
	 * other, unrelated object (we only ever bind at most two: the M4
	 * device and its sink node) are logged but not fatal -- keep going,
	 * the sync we already queued will still fire. */
	m4_log(M4_LOG_WARN, M4_T("PipeWire: error id=%u res=%d: %s",
"PipeWire: ошибка id=%u res=%d: %s"), id, res,
	       message != NULL ? message : "");
	if (id == PW_ID_CORE)
		finish(ctx);
}

static void on_device_info(void *data, const struct pw_device_info *info)
{
	struct m4_pw_query_ctx *ctx = data;
	const char *conn;

	if (info->props == NULL)
		return;
	conn = spa_dict_lookup(info->props, SPA_KEY_API_BLUEZ5_CONNECTION);
	if (conn != NULL)
		ctx->out->m4_connected = spa_streq(conn, "connected");
}

static void on_device_param(void *data, int seq, uint32_t id, uint32_t index,
			     uint32_t next, const struct spa_pod *param)
{
	struct m4_pw_query_ctx *ctx = data;
	(void)seq;
	(void)index;
	(void)next;

	if (id == SPA_PARAM_EnumProfile) {
		ctx->out->enum_profile_count++;
	} else if (id == SPA_PARAM_Profile) {
		const char *name = NULL, *description = NULL;

		if (spa_pod_parse_object(param,
				SPA_TYPE_OBJECT_ParamProfile, NULL,
				SPA_PARAM_PROFILE_name, SPA_POD_String(&name),
				SPA_PARAM_PROFILE_description, SPA_POD_OPT_String(&description)) < 0)
			return;

		ctx->out->have_profile = true;
		if (name != NULL) {
			snprintf(ctx->out->profile_name, sizeof(ctx->out->profile_name), "%s", name);
			ctx->out->mode = m4_profile_mode(name);
		}
		if (description != NULL)
			snprintf(ctx->out->profile_description, sizeof(ctx->out->profile_description),
				 "%s", description);
	}
}

static void on_node_info(void *data, const struct pw_node_info *info)
{
	struct m4_pw_query_ctx *ctx = data;
	const char *codec;

	if (info->props == NULL)
		return;
	codec = spa_dict_lookup(info->props, SPA_KEY_API_BLUEZ5_CODEC);
	if (codec != NULL && codec[0] != '\0') {
		snprintf(ctx->out->codec, sizeof(ctx->out->codec), "%s", codec);
		ctx->out->have_codec = true;
	}
}

static void bind_device_and_node(struct m4_pw_query_ctx *ctx)
{
	static const struct pw_device_events device_events = {
		.version = PW_VERSION_DEVICE_EVENTS,
		.info = on_device_info,
		.param = on_device_param,
	};
	static const struct pw_node_events node_events = {
		.version = PW_VERSION_NODE_EVENTS,
		.info = on_node_info,
	};
	struct pw_device *device;

	ctx->device_proxy = pw_registry_bind(ctx->registry, ctx->device_id,
					      PW_TYPE_INTERFACE_Device, PW_VERSION_DEVICE, 0);
	if (ctx->device_proxy == NULL) {
		m4_log(M4_LOG_WARN, M4_T("PipeWire: failed to bind to the M4 device (id=%u)",
"PipeWire: не удалось привязаться к устройству M4 (id=%u)"),
		       ctx->device_id);
	} else {
		ctx->device_bound = true;
		device = (struct pw_device *)ctx->device_proxy;
		pw_device_add_listener(device, &ctx->device_listener, &device_events, ctx);
		pw_device_enum_params(device, 0, SPA_PARAM_EnumProfile, 0, UINT32_MAX, NULL);
		pw_device_enum_params(device, 0, SPA_PARAM_Profile, 0, UINT32_MAX, NULL);
	}

	/* Pick the first Audio/Sink child Node of our Device seen in the
	 * registry burst, if any (no M4 audio path active right now, e.g.
	 * profile "off" or HFP-only with no sink -- not an error, just leaves
	 * out->have_codec false). */
	for (unsigned i = 0; i < ctx->n_sink_candidates && !ctx->node_found; i++) {
		if (ctx->sink_candidates[i].device_id == ctx->device_id) {
			ctx->node_found = true;
			ctx->node_id = ctx->sink_candidates[i].id;
		}
	}
	if (!ctx->node_found)
		return;

	ctx->node_proxy = pw_registry_bind(ctx->registry, ctx->node_id,
					    PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0);
	if (ctx->node_proxy == NULL)
		return;
	ctx->node_bound = true;
	pw_node_add_listener((struct pw_node *)ctx->node_proxy, &ctx->node_listener,
			      &node_events, ctx);
}

static void on_registry_global(void *data, uint32_t id, uint32_t permissions,
				const char *type, uint32_t version,
				const struct spa_dict *props)
{
	struct m4_pw_query_ctx *ctx = data;
	(void)permissions;
	(void)version;

	if (props == NULL)
		return;

	if (!ctx->device_found && spa_streq(type, PW_TYPE_INTERFACE_Device)) {
		const char *name = spa_dict_lookup(props, PW_KEY_DEVICE_NAME);
		const char *desc;

		if (name == NULL || strcasecmp(name, ctx->device_name) != 0)
			return;

		ctx->device_found = true;
		ctx->device_id = id;
		ctx->out->m4_present = true;

		desc = spa_dict_lookup(props, PW_KEY_DEVICE_DESCRIPTION);
		if (desc != NULL)
			snprintf(ctx->out->device_description, sizeof(ctx->out->device_description),
				 "%s", desc);
	} else if (spa_streq(type, PW_TYPE_INTERFACE_Node)) {
		const char *device_id_str = spa_dict_lookup(props, PW_KEY_DEVICE_ID);
		const char *media_class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);

		if (device_id_str == NULL || !m4_pw_media_class_is_sink(media_class))
			return;
		if (ctx->n_sink_candidates >= M4_PW_MAX_SINK_CANDIDATES)
			return; /* generous cap for a real system; silently drop the rest */

		ctx->sink_candidates[ctx->n_sink_candidates].id = id;
		ctx->sink_candidates[ctx->n_sink_candidates].device_id =
			(uint32_t)strtoul(device_id_str, NULL, 10);
		ctx->n_sink_candidates++;
	}
}

static int on_timeout(sd_event_source *s, uint64_t usec, void *userdata)
{
	struct m4_pw_query_ctx *ctx = userdata;
	(void)s;
	(void)usec;

	ctx->out->timed_out = true;
	finish(ctx);
	return 0;
}

void m4_pw_build_device_name(const char *mac_address, char *out, size_t out_cap)
{
	size_t o;
	int n = snprintf(out, out_cap, "bluez_card.");
	if (n < 0 || (size_t)n >= out_cap)
		return;
	o = (size_t)n;
	for (const char *p = mac_address; *p != '\0' && o + 1 < out_cap; p++)
		out[o++] = (*p == ':') ? '_' : *p;
	out[o] = '\0';
}

bool m4_pw_media_class_is_sink(const char *media_class)
{
	return media_class != NULL && strstr(media_class, "Sink") != NULL;
}

/* ======================================================================
 * long-lived struct graph_ops implementation (m4_graph_ops_pw)
 *
 * Separate state machine from m4_pw_query_snapshot() above: instead of
 * driving a private sd_event to completion once, this attaches to the
 * CALLER's sd_event and keeps running, pushing
 * on_state_changed() whenever the M4 device's presence/connection/profile/
 * EnumProfile list changes -- for as long as the handle stays open.
 *
 * Refresh protocol for SPA_PARAM_EnumProfile/SPA_PARAM_Profile (chosen
 * after reading PipeWire's own pw-dump tool's device_event_info(), the
 * only "keeps a device's params live over time" reference implementation
 * in this PipeWire tree):
 * pw-dump tracks a per-param dirty bit (struct spa_param_info.user, set by
 * pw_device_info_merge() whenever a param's SPA_PARAM_INFO_SERIAL flips)
 * and re-enum_params() only the ids that actually changed. This file takes
 * a simpler, slightly less surgical approach that is easier to reason
 * about and just as correct for an object with 8 profiles polled rarely:
 * ANY `info` event with PW_DEVICE_CHANGE_MASK_PARAMS set (including the
 * very first one, right after bind -- so no separate "seed the initial
 * value" step is needed) clears the staging buffers and re-issues BOTH
 * enum_params(EnumProfile) and enum_params(Profile) unconditionally, then
 * pw_core_sync()s (same "real seq is the return value" technique as
 * m4_pw_query_snapshot() above) to know when every reply has arrived
 * before atomically committing staged -> live state and firing the
 * callback -- so get_state() never observes a half-rebuilt profiles[]
 * list. At most one refresh is ever in flight at a time; an `info` event
 * that arrives while one is already pending just marks `refresh_dirty`
 * so a fresh refresh starts as soon as the current one commits (rather
 * than being silently dropped or racing two overlapping enum_params
 * batches into the same staging buffer).
 * ====================================================================== */

struct m4_graph_pw_ctx {
	sd_event *event; /* caller's loop -- NOT owned, NOT destroyed by close() */
	char device_name[80];
	struct m4_graph_callbacks cb;
	void *user_data;

	struct pw_loop *loop;
	struct pw_context *context;
	sd_event_source *pw_io_source;
	struct pw_core *core;
	struct pw_registry *registry;
	struct spa_hook core_listener;
	struct spa_hook registry_listener;

	bool device_found;
	uint32_t device_id;
	struct pw_proxy *device_proxy;
	struct spa_hook device_listener;

	bool refresh_pending;   /* an enum_params(EnumProfile)+enum_params(Profile)
				  * pair is in flight, awaiting its matching core.done */
	bool refresh_dirty;     /* another refresh was requested while one was
				  * already pending -- start one more once this commits */
	int pending_sync_seq;

	struct m4_graph_state state; /* live, committed; what get_state() returns */

	/* staging: cleared right before each enum_params pair, committed into
	 * `state` only on the matching core.done (see block comment above) */
	size_t n_staged_profiles;
	struct m4_graph_profile staged_profiles[M4_GRAPH_MAX_PROFILES];
	bool staged_have_profile;
	char staged_profile_name[64];

	/* Bug 2: same staging discipline as the profile
	 * fields above, for SPA_PARAM_Route index=ROUTE_INPUT(0) -- see
	 * graph_ops.h's have_mic_route/mic_volume/mic_mute doc comment. */
	bool staged_have_mic_route;
	float staged_mic_volume;
	bool staged_mic_mute;
};

static void m4_graph_pw_close(void *handle);
static void graph_start_refresh(struct m4_graph_pw_ctx *ctx);

static void graph_notify(struct m4_graph_pw_ctx *ctx)
{
	if (ctx->cb.on_state_changed != NULL)
		ctx->cb.on_state_changed(ctx->user_data, &ctx->state);
}

static void graph_unbind_device(struct m4_graph_pw_ctx *ctx)
{
	if (ctx->device_proxy != NULL) {
		pw_proxy_destroy(ctx->device_proxy);
		ctx->device_proxy = NULL;
	}
	ctx->device_found = false;
	ctx->refresh_pending = false;
	ctx->refresh_dirty = false;
}

static void graph_on_device_info(void *data, const struct pw_device_info *info)
{
	struct m4_graph_pw_ctx *ctx = data;
	bool changed = false;

	if (info->props != NULL) {
		const char *conn = spa_dict_lookup(info->props, SPA_KEY_API_BLUEZ5_CONNECTION);

		if (conn != NULL) {
			bool now_connected = spa_streq(conn, "connected");

			if (now_connected != ctx->state.m4_connected) {
				ctx->state.m4_connected = now_connected;
				changed = true;
			}
		}
	}

	if (info->change_mask & PW_DEVICE_CHANGE_MASK_PARAMS)
		graph_start_refresh(ctx);

	if (changed)
		graph_notify(ctx);
}

static void graph_parse_profile_pod(struct m4_graph_pw_ctx *ctx, uint32_t id,
				     const struct spa_pod *param)
{
	const char *name = NULL, *description = NULL;
	int32_t index = -1;
	uint32_t available = SPA_PARAM_AVAILABILITY_unknown;

	if (spa_pod_parse_object(param,
			SPA_TYPE_OBJECT_ParamProfile, NULL,
			SPA_PARAM_PROFILE_index, SPA_POD_Int(&index),
			SPA_PARAM_PROFILE_name, SPA_POD_OPT_String(&name),
			SPA_PARAM_PROFILE_description, SPA_POD_OPT_String(&description),
			SPA_PARAM_PROFILE_available, SPA_POD_OPT_Id(&available)) < 0)
		return;

	if (id == SPA_PARAM_EnumProfile) {
		struct m4_graph_profile *p;

		if (ctx->n_staged_profiles >= M4_GRAPH_MAX_PROFILES || index < 0)
			return;
		p = &ctx->staged_profiles[ctx->n_staged_profiles++];
		memset(p, 0, sizeof(*p));
		p->index = (uint32_t)index;
		if (name != NULL)
			snprintf(p->name, sizeof(p->name), "%s", name);
		if (description != NULL)
			snprintf(p->description, sizeof(p->description), "%s", description);
		p->available = (available != SPA_PARAM_AVAILABILITY_no);
	} else if (id == SPA_PARAM_Profile) {
		ctx->staged_have_profile = true;
		ctx->staged_profile_name[0] = '\0';
		if (name != NULL)
			snprintf(ctx->staged_profile_name, sizeof(ctx->staged_profile_name), "%s", name);
	}
}

/* Bug 2: parses one SPA_PARAM_Route entry, keeping only
 * index==ROUTE_INPUT(0) (the M4 microphone -- bluez5-device.c's `enum {
 * ROUTE_INPUT = 0, ROUTE_OUTPUT, ... }`, spa/plugins/bluez5/bluez5-device.c:
 * 66-68, confirmed by reading it). `props` is only present at all when
 * build_route() was called with a real `profile` (impl_enum_params()'s
 * SPA_PARAM_Route case always passes `this->profile`, never
 * SPA_ID_INVALID -- unlike SPA_PARAM_EnumRoute) AND that profile's
 * direction mask includes SPA_DIRECTION_INPUT -- i.e. exactly "the M4 is
 * currently in a profile with a microphone" (HFP/HSP). Absence of `props`
 * here (not just absence of the whole Route entry) is therefore itself
 * meaningful and must NOT set staged_have_mic_route -- see the two `return`s
 * below, both intentional, matching AALL (a2dp-sink) correctly reporting
 * "no mic route" rather than stale leftover values from a previous HFP
 * session. */
static void graph_parse_route_pod(struct m4_graph_pw_ctx *ctx, const struct spa_pod *param)
{
	int32_t index = -1;
	struct spa_pod *props = NULL;

	if (spa_pod_parse_object(param,
			SPA_TYPE_OBJECT_ParamRoute, NULL,
			SPA_PARAM_ROUTE_index, SPA_POD_Int(&index),
			SPA_PARAM_ROUTE_props, SPA_POD_OPT_Pod(&props)) < 0)
		return;
	if (index != 0 /* ROUTE_INPUT, bluez5-device.c */ || props == NULL)
		return;
	if (!spa_pod_is_object_type(props, SPA_TYPE_OBJECT_Props))
		return;

	ctx->staged_have_mic_route = true;
	ctx->staged_mic_volume = 0.0f;
	ctx->staged_mic_mute = false;

	struct spa_pod_object *obj = (struct spa_pod_object *)props;
	struct spa_pod_prop *prop;
	SPA_POD_OBJECT_FOREACH(obj, prop) {
		if (prop->key == SPA_PROP_mute) {
			bool m;
			if (spa_pod_get_bool(&prop->value, &m) == 0)
				ctx->staged_mic_mute = m;
		} else if (prop->key == SPA_PROP_channelVolumes) {
			float vols[8];
			uint32_t n = spa_pod_copy_array(&prop->value, SPA_TYPE_Float, vols, 8);
			float mx = 0.0f;

			/* max(), not the first element -- mirrors bluez5-device.c's
			 * own node_get_hw_volume() (SPA_MAX over all channels),
			 * see graph_ops.h's mic_volume doc comment. */
			for (uint32_t i = 0; i < n; i++)
				if (vols[i] > mx)
					mx = vols[i];
			ctx->staged_mic_volume = mx;
		}
	}
}

static void graph_on_device_param(void *data, int seq, uint32_t id, uint32_t index,
				   uint32_t next, const struct spa_pod *param)
{
	struct m4_graph_pw_ctx *ctx = data;
	(void)seq;
	(void)index;
	(void)next;

	/* Only meaningful while a refresh we ourselves triggered is in
	 * flight -- see the block comment above for why per-call seq
	 * correlation is unnecessary (at most one refresh in flight ever). */
	if (!ctx->refresh_pending)
		return;
	if (param == NULL)
		return;
	if (id == SPA_PARAM_EnumProfile || id == SPA_PARAM_Profile)
		graph_parse_profile_pod(ctx, id, param);
	else if (id == SPA_PARAM_Route)
		graph_parse_route_pod(ctx, param);
}

static void graph_bind_device(struct m4_graph_pw_ctx *ctx)
{
	static const struct pw_device_events device_events = {
		.version = PW_VERSION_DEVICE_EVENTS,
		.info = graph_on_device_info,
		.param = graph_on_device_param,
	};

	ctx->device_proxy = pw_registry_bind(ctx->registry, ctx->device_id,
					      PW_TYPE_INTERFACE_Device, PW_VERSION_DEVICE, 0);
	if (ctx->device_proxy == NULL) {
		m4_log(M4_LOG_WARN, M4_T("PipeWire: failed to bind to the M4 device (id=%u)",
"PipeWire: не удалось привязаться к устройству M4 (id=%u)"),
		       ctx->device_id);
		ctx->device_found = false;
		return;
	}
	pw_device_add_listener((struct pw_device *)ctx->device_proxy, &ctx->device_listener,
				&device_events, ctx);
	/* Seed initial state right away -- do not wait for an `info` event,
	 * matching m4_pw_query_snapshot()'s bind_device_and_node() above
	 * (live-verified: the very first info event that follows a bind
	 * also carries PW_DEVICE_CHANGE_MASK_PARAMS, so this is technically
	 * redundant with graph_on_device_info()'s own trigger, but issuing
	 * it here too costs nothing and removes any dependency on that
	 * event's exact timing/ordering relative to bind). */
	graph_start_refresh(ctx);
}

static void graph_start_refresh(struct m4_graph_pw_ctx *ctx)
{
	if (ctx->device_proxy == NULL)
		return;
	if (ctx->refresh_pending) {
		ctx->refresh_dirty = true;
		return;
	}

	ctx->n_staged_profiles = 0;
	ctx->staged_have_profile = false;
	ctx->staged_profile_name[0] = '\0';
	/* Bug 2: cleared unconditionally on every refresh,
	 * same as the profile fields above -- if this round's SPA_PARAM_Route
	 * enum yields no ROUTE_INPUT props at all (AALL, no mic route right
	 * now), staged_have_mic_route correctly stays false rather than
	 * carrying over a stale value from a previous HFP session. */
	ctx->staged_have_mic_route = false;
	ctx->staged_mic_volume = 0.0f;
	ctx->staged_mic_mute = false;

	pw_device_enum_params((struct pw_device *)ctx->device_proxy, 0,
			       SPA_PARAM_EnumProfile, 0, UINT32_MAX, NULL);
	pw_device_enum_params((struct pw_device *)ctx->device_proxy, 0,
			       SPA_PARAM_Profile, 0, UINT32_MAX, NULL);
	/* Bug 2, CORRECTED ("M4 button = mute (VGM)" task): every
	 * refresh triggered by a genuine PARAMS-changed `info` event (profile
	 * switches, OUR OWN set_mic_volume() below) piggy-backs an up-to-date
	 * Route read for free -- unchanged from before. The PREVIOUS version
	 * of this comment additionally claimed an incoming AT+VGM from the M4
	 * itself does NOT trigger a fresh `info`/PARAMS event at all (making a
	 * mid-call, M4-initiated volume change invisible to this refresh
	 * mechanism until some UNRELATED event happened to trigger a refresh) --
	 * that claim was WRONG, from misreading bluez5-device.c's TWO different
	 * `emit_volume` names: the local per-write helper `emit_volume()`
	 * (node_set_volume()'s own POD-props-only SPA_DEVICE_EVENT_ObjectConfig,
	 * genuinely no PARAMS/serial bump on its own) is NOT the same function
	 * as the RX/TX transport's `.volume_changed` LISTENER callback (also
	 * named `volume_changed()`, confusingly) that fires on every incoming
	 * AT+VGM (spa_bt_transport_emit_volume_changed(), invoked from
	 * backend-native.c's rfcomm_emit_volume_changed()) -- THAT function
	 * calls emit_volume() too, but then ALSO does
	 * `impl->info.change_mask |= SPA_DEVICE_CHANGE_MASK_PARAMS;
	 * impl->params[IDX_Route].flags ^= SPA_PARAM_INFO_SERIAL; emit_info(impl,
	 * false);` -- confirmed by reading its body (installed pipewire-src's
	 * spa/plugins/bluez5/bluez5-device.c, the same version this whole tree
	 * is built against) -- which DOES reach graph_on_device_info() below via
	 * PW_DEVICE_CHANGE_MASK_PARAMS, the exact same path a profile switch
	 * takes. So a mid-call, M4-initiated AT+VGM (e.g. the physical mute
	 * button) DOES trigger a fresh refresh on its own, promptly (one BlueZ
	 * RFCOMM round trip plus one pw_core_sync(), no daemon-side polling of
	 * any kind) -- this is what daemon.c's real-time mic-route-mute
	 * observation (daemon_check_mic_route_transition()) relies on. Not
	 * independently re-verified against a LIVE M4 (code-reading only so
	 * far) -- if a live check ever contradicts this comment, trust the live
	 * behaviour, not this comment. */
	pw_device_enum_params((struct pw_device *)ctx->device_proxy, 0,
			       SPA_PARAM_Route, 0, UINT32_MAX, NULL);
	ctx->pending_sync_seq = pw_core_sync(ctx->core, PW_ID_CORE, ctx->pending_sync_seq);
	ctx->refresh_pending = true;
}

/* True if the EnumProfile-derived parts of two states are identical
 * (index/name/description/available, in the same order -- PipeWire always
 * enumerates a device's profiles in the same index order, so order is a
 * legitimate part of "did anything change" here, not just set membership).
 * Used by graph_on_core_done() below to avoid firing on_state_changed()
 * twice with byte-for-byte identical content -- which DOES happen live:
 * two PW_DEVICE_CHANGE_MASK_PARAMS `info` events
 * arriving close together each trigger their own refresh via the
 * refresh_dirty coalescing path, and both can legitimately converge to the
 * exact same result, e.g. only Profile's SERIAL flipped and EnumProfile's
 * content didn't actually change). */
static bool graph_profiles_equal(const struct m4_graph_state *a, const struct m4_graph_state *b)
{
	if (a->n_profiles != b->n_profiles)
		return false;
	for (size_t i = 0; i < a->n_profiles; i++) {
		const struct m4_graph_profile *pa = &a->profiles[i], *pb = &b->profiles[i];

		if (pa->index != pb->index || pa->available != pb->available)
			return false;
		if (strcmp(pa->name, pb->name) != 0 || strcmp(pa->description, pb->description) != 0)
			return false;
	}
	return true;
}

static void graph_on_core_done(void *data, uint32_t id, int seq)
{
	struct m4_graph_pw_ctx *ctx = data;
	struct m4_graph_state before;

	if (id != PW_ID_CORE || !ctx->refresh_pending || seq != ctx->pending_sync_seq)
		return;

	ctx->refresh_pending = false;
	before = ctx->state;

	ctx->state.n_profiles = ctx->n_staged_profiles;
	memcpy(ctx->state.profiles, ctx->staged_profiles,
	       ctx->n_staged_profiles * sizeof(ctx->staged_profiles[0]));
	if (ctx->staged_have_profile) {
		ctx->state.have_profile = true;
		snprintf(ctx->state.profile_name, sizeof(ctx->state.profile_name),
			 "%s", ctx->staged_profile_name);
	}
	/* Bug 2: committed unconditionally (not "only if staged_have_mic_route"
	 * -- see graph_start_refresh()'s comment, this round's absence is
	 * meaningful and must overwrite a stale true from before). */
	ctx->state.have_mic_route = ctx->staged_have_mic_route;
	ctx->state.mic_volume = ctx->staged_mic_volume;
	ctx->state.mic_mute = ctx->staged_mic_mute;

	if (before.have_profile != ctx->state.have_profile ||
	    strcmp(before.profile_name, ctx->state.profile_name) != 0 ||
	    !graph_profiles_equal(&before, &ctx->state) ||
	    before.have_mic_route != ctx->state.have_mic_route ||
	    before.mic_volume != ctx->state.mic_volume ||
	    before.mic_mute != ctx->state.mic_mute)
		graph_notify(ctx);

	if (ctx->refresh_dirty) {
		ctx->refresh_dirty = false;
		graph_start_refresh(ctx);
	}
}

bool m4_graph_error_is_benign_stale_destroy(int res, const char *message)
{
	/* Observed in production (noticed incidentally
	 * while investigating something else): the logged error (id=0 res=-2: unknown
	 * resource 3 op:2) -- confirmed by reading
	 * work/src/pipewire-1.4.2/src/modules/module-protocol-native/
	 * protocol-native.c's core_method_demarshal_destroy(): this EXACT
	 * message ("unknown resource %u op:%u") is generated SERVER-SIDE only
	 * when the CLIENT sends a `pw_core.destroy(id)` request for a
	 * resource the server can no longer find -- i.e. our own
	 * pw_proxy_destroy() (graph_unbind_device(), fired from
	 * graph_on_registry_global_remove() below) queued a destroy
	 * notification for the M4 device proxy a moment after the SERVER had
	 * already auto-destroyed that same resource itself, as part of
	 * processing the underlying global's removal (a well-known, benign
	 * PipeWire double-cleanup race -- confirmed by reading
	 * core_method_demarshal_destroy()'s only call site, msg->opcode for a
	 * destroy request). The server routes this error via the core (id=0,
	 * PW_ID_CORE) because the resource that WOULD have received it no
	 * longer exists locally either. res == -ENOENT always accompanies
	 * it (pw_resource_errorf(resource, -ENOENT, ...) at that call site).
	 *
	 * This is NOT a sign the core connection itself is broken -- treating
	 * it as one (this function's caller used to, unconditionally, for
	 * every id==PW_ID_CORE error) is actively dangerous specifically
	 * during a device remove+re-add (e.g. after `bluetoothctl
	 * disconnect;connect`, which fully recreates the M4's Device object,
	 * hence this predicate's own "resilient to re-creation" role): by the
	 * time this STALE reply for the OLD, already-unbound proxy arrives,
	 * graph_on_registry_global()/graph_bind_device() may already have
	 * bound a BRAND NEW device_proxy for the freshly-reappeared M4 --
	 * unconditionally tearing down `ctx->state`/calling
	 * graph_unbind_device() here would destroy that NEW proxy and falsely
	 * report the M4 as gone, seconds after it correctly reappeared.
	 *
	 * Exposed (not `static`) purely so tests/test_graph_props.c can
	 * exercise this predicate without a live PipeWire connection, exactly
	 * like m4_pw_media_class_is_sink() above -- pure string/int matching,
	 * no I/O. */
	if (res != -ENOENT || message == NULL)
		return false;
	return strstr(message, "unknown resource") != NULL;
}

static void graph_on_core_error(void *data, uint32_t id, int seq, int res, const char *message)
{
	struct m4_graph_pw_ctx *ctx = data;
	(void)seq;

	if (m4_graph_error_is_benign_stale_destroy(res, message)) {
		/* See m4_graph_error_is_benign_stale_destroy()'s own comment --
		 * intentionally NOT a WARN: this is an expected, harmless
		 * race, not something worth alarming a human reading the
		 * journal. registry.global_remove()/global() already are, and
		 * remain, the only source of truth for M4 presence -- nothing
		 * else to do here. */
		m4_log(M4_LOG_INFO,
		       M4_T("PipeWire (graph_ops): late destroy() reply for a resource already removed "
		       "server-side (id=%u res=%d: %s) -- ignoring, the connection is not broken",
"PipeWire (graph_ops): запоздалый ответ на destroy() уже удалённого на сервере "
		       "ресурса (id=%u res=%d: %s) -- игнорирую, соединение не разорвано"),
		       id, res, message != NULL ? message : "");
		return;
	}

	m4_log(M4_LOG_WARN, M4_T("PipeWire (graph_ops): error id=%u res=%d: %s",
"PipeWire (graph_ops): ошибка id=%u res=%d: %s"), id, res,
	       message != NULL ? message : "");
	if (id == PW_ID_CORE) {
		/* Core connection itself is in trouble. There is no
		 * reconnect-with-backoff here on purpose -- that
		 * responsibility is assigned to the daemon layer above
		 * graph_ops, not to this module. What we CAN do without
		 * making anything up is stop claiming stale state is fresh:
		 * tell the caller the M4 is not usable right now so it does
		 * not act on a state that may no longer be true. The caller
		 * (callfsm.c/automode.c) is expected to close() and later
		 * re-open() a fresh handle once it decides to retry -- this
		 * is a genuine open question, not fully resolved here. */
		ctx->state.m4_present = false;
		ctx->state.m4_connected = false;
		ctx->state.have_profile = false;
		ctx->state.n_profiles = 0;
		graph_unbind_device(ctx);
		graph_notify(ctx);
	}
}

static void graph_on_registry_global(void *data, uint32_t id, uint32_t permissions,
				      const char *type, uint32_t version,
				      const struct spa_dict *props)
{
	struct m4_graph_pw_ctx *ctx = data;
	const char *name;

	(void)permissions;
	(void)version;

	if (ctx->device_found || props == NULL || !spa_streq(type, PW_TYPE_INTERFACE_Device))
		return;

	name = spa_dict_lookup(props, PW_KEY_DEVICE_NAME);
	if (name == NULL || strcasecmp(name, ctx->device_name) != 0)
		return;

	ctx->device_found = true;
	ctx->device_id = id;
	ctx->state.m4_present = true;
	graph_notify(ctx);
	graph_bind_device(ctx);
}

static void graph_on_registry_global_remove(void *data, uint32_t id)
{
	struct m4_graph_pw_ctx *ctx = data;

	if (!ctx->device_found || id != ctx->device_id)
		return;

	graph_unbind_device(ctx);
	ctx->state.m4_present = false;
	ctx->state.m4_connected = false;
	ctx->state.have_profile = false;
	ctx->state.n_profiles = 0;
	ctx->state.profile_name[0] = '\0';
	graph_notify(ctx);
}

static void *m4_graph_pw_open(sd_event *event, const char *mac_address,
			       const struct m4_graph_callbacks *cb, void *user_data)
{
	static const struct pw_core_events core_events = {
		.version = PW_VERSION_CORE_EVENTS,
		.done = graph_on_core_done,
		.error = graph_on_core_error,
	};
	static const struct pw_registry_events registry_events = {
		.version = PW_VERSION_REGISTRY_EVENTS,
		.global = graph_on_registry_global,
		.global_remove = graph_on_registry_global_remove,
	};
	struct m4_graph_pw_ctx *ctx;

	if (event == NULL || mac_address == NULL)
		return NULL;

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL)
		return NULL;

	ctx->event = event;
	if (cb != NULL)
		ctx->cb = *cb;
	ctx->user_data = user_data;
	m4_pw_build_device_name(mac_address, ctx->device_name, sizeof(ctx->device_name));

	ctx->loop = pw_loop_new(NULL);
	if (ctx->loop == NULL)
		goto fail;
	if (m4_loop_glue_attach(ctx->event, ctx->loop, &ctx->pw_io_source) < 0)
		goto fail;
	ctx->context = pw_context_new(ctx->loop, NULL, 0);
	if (ctx->context == NULL)
		goto fail;
	ctx->core = pw_context_connect(ctx->context, NULL, 0);
	if (ctx->core == NULL) {
		m4_log(M4_LOG_WARN, M4_T("PipeWire unavailable (graph_ops): %s",
"PipeWire недоступен (graph_ops): %s"), strerror(errno));
		goto fail;
	}
	ctx->registry = pw_core_get_registry(ctx->core, PW_VERSION_REGISTRY, 0);
	if (ctx->registry == NULL)
		goto fail;

	pw_core_add_listener(ctx->core, &ctx->core_listener, &core_events, ctx);
	pw_registry_add_listener(ctx->registry, &ctx->registry_listener, &registry_events, ctx);

	return ctx;

fail:
	m4_graph_pw_close(ctx);
	return NULL;
}

static void m4_graph_pw_close(void *handle)
{
	struct m4_graph_pw_ctx *ctx = handle;

	if (ctx == NULL)
		return;

	if (ctx->device_proxy != NULL)
		pw_proxy_destroy(ctx->device_proxy);
	if (ctx->registry != NULL)
		pw_proxy_destroy((struct pw_proxy *)ctx->registry);
	if (ctx->core != NULL)
		pw_core_disconnect(ctx->core);
	if (ctx->context != NULL)
		pw_context_destroy(ctx->context);
	if (ctx->pw_io_source != NULL)
		sd_event_source_unref(ctx->pw_io_source);
	if (ctx->loop != NULL)
		pw_loop_destroy(ctx->loop);
	free(ctx);
}

static enum m4_graph_status m4_graph_pw_get_state(void *handle, struct m4_graph_state *out)
{
	struct m4_graph_pw_ctx *ctx = handle;

	if (ctx == NULL || out == NULL)
		return M4_GRAPH_ERR_INVALID;
	*out = ctx->state;
	return M4_GRAPH_OK;
}

bool m4_graph_find_profile_index(const struct m4_graph_state *state,
				  const char *profile_name, uint32_t *out_index)
{
	size_t i;

	if (state == NULL || profile_name == NULL || out_index == NULL)
		return false;
	for (i = 0; i < state->n_profiles; i++) {
		if (strcmp(state->profiles[i].name, profile_name) == 0) {
			*out_index = state->profiles[i].index;
			return true;
		}
	}
	return false;
}

static enum m4_graph_status m4_graph_pw_set_profile(void *handle, const char *profile_name,
						     bool save)
{
	struct m4_graph_pw_ctx *ctx = handle;
	uint32_t index;
	uint8_t buffer[512];
	struct spa_pod_builder b;
	struct spa_pod *pod;
	int r;

	if (ctx == NULL || profile_name == NULL || profile_name[0] == '\0')
		return M4_GRAPH_ERR_INVALID;
	if (ctx->device_proxy == NULL || !ctx->state.m4_present)
		return M4_GRAPH_ERR_NOT_FOUND;
	if (!m4_graph_find_profile_index(&ctx->state, profile_name, &index))
		return M4_GRAPH_ERR_NO_SUCH_PROFILE;

	/* Exact same construction as module-protocol-pulse/pulse-server.c's
	 * do_set_profile() (pactl's "set-card-profile" implementation
	 * -- confirmed by reading the function body, not just
	 * quoting it): pw_device_set_param(device, SPA_PARAM_Profile, 0,
	 * SPA_TYPE_OBJECT_ParamProfile{ index, save }). WirePlumber's own
	 * apply-profile.lua does the equivalent from the Lua side. */
	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	pod = spa_pod_builder_add_object(&b,
			SPA_TYPE_OBJECT_ParamProfile, SPA_PARAM_Profile,
			SPA_PARAM_PROFILE_index, SPA_POD_Int((int32_t)index),
			SPA_PARAM_PROFILE_save, SPA_POD_Bool(save));

	r = pw_device_set_param((struct pw_device *)ctx->device_proxy, SPA_PARAM_Profile, 0, pod);
	if (r < 0 && !SPA_RESULT_IS_ASYNC(r)) {
		m4_log(M4_LOG_WARN, M4_T("PipeWire: set_param(Profile) rejected (%d) for \"%s\"",
"PipeWire: set_param(Profile) отклонён (%d) для \"%s\""),
		       r, profile_name);
		/* Still M4_GRAPH_OK: the request WAS issued to the server (it
		 * rejected it, e.g. -EACCES without W|X permission) -- this
		 * is not a local usage error like the ones above, and the
		 * caller finds out the same way it finds out about any other
		 * outcome: watching on_state_changed(), per this header's
		 * documented set_profile() contract (fire-and-forget). */
	}
	return M4_GRAPH_OK;
}

/* Bug 2: exact same construction as
 * module-protocol-pulse/pulse-server.c's set_card_volume_mute_delay() (the
 * real implementation behind `pactl set-source-volume`/`set-source-mute`
 * for a card-backed source, confirmed by reading its body) -- but with
 * ROUTE_INPUT(0)/DEVICE_ID_SOURCE(0) hardcoded (both fixed enum constants in
 * bluez5-device.c, spa/plugins/bluez5/bluez5-device.c:43,67 -- unlike
 * set_profile()'s index, this needs no lookup in cached state at all, see
 * graph_ops.h's set_mic_volume doc comment for why a single-element
 * channelVolumes array is always sufficient). */
static enum m4_graph_status m4_graph_pw_set_mic_volume(void *handle, float volume, bool mute, bool save)
{
	struct m4_graph_pw_ctx *ctx = handle;
	uint8_t buffer[512];
	struct spa_pod_builder b;
	struct spa_pod_frame f[2];
	struct spa_pod *pod;
	int r;

	if (ctx == NULL)
		return M4_GRAPH_ERR_INVALID;
	if (ctx->device_proxy == NULL || !ctx->state.m4_present)
		return M4_GRAPH_ERR_NOT_FOUND;

	if (volume < 0.0f)
		volume = 0.0f;
	if (volume > 1.0f)
		volume = 1.0f;

	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	spa_pod_builder_push_object(&b, &f[0], SPA_TYPE_OBJECT_ParamRoute, SPA_PARAM_Route);
	spa_pod_builder_add(&b,
			SPA_PARAM_ROUTE_index, SPA_POD_Int(0),  /* ROUTE_INPUT */
			SPA_PARAM_ROUTE_device, SPA_POD_Int(0), /* DEVICE_ID_SOURCE */
			0);
	spa_pod_builder_prop(&b, SPA_PARAM_ROUTE_props, 0);
	spa_pod_builder_push_object(&b, &f[1], SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
	spa_pod_builder_add(&b,
			SPA_PROP_mute, SPA_POD_Bool(mute),
			SPA_PROP_channelVolumes, SPA_POD_Array(sizeof(float), SPA_TYPE_Float, 1, &volume),
			0);
	spa_pod_builder_pop(&b, &f[1]);
	spa_pod_builder_prop(&b, SPA_PARAM_ROUTE_save, 0);
	spa_pod_builder_bool(&b, save);
	pod = spa_pod_builder_pop(&b, &f[0]);

	r = pw_device_set_param((struct pw_device *)ctx->device_proxy, SPA_PARAM_Route, 0, pod);
	if (r < 0 && !SPA_RESULT_IS_ASYNC(r)) {
		m4_log(M4_LOG_WARN, M4_T("PipeWire: set_param(Route) rejected (%d) for the M4 microphone volume",
"PipeWire: set_param(Route) отклонён (%d) для громкости микрофона M4"), r);
		/* Still M4_GRAPH_OK -- same fire-and-forget contract as
		 * set_profile() above (see graph_ops.h): the request WAS
		 * issued, the caller finds out the outcome via get_state(). */
	}
	return M4_GRAPH_OK;
}

/* graph_ops.h's optional iterate(): pump this handle's own pw_loop
 * directly (same enter/iterate/leave triple pw/loop_glue.c's sd-event io
 * callback runs, just with a real timeout) -- lets a bounded confirm-poll
 * make progress while the caller is itself inside an sd-event dispatch,
 * where a nested sd_event_run() would return -EBUSY (pw/graph_wait.h). */
static int m4_graph_pw_iterate(void *handle, int timeout_ms)
{
	struct m4_graph_pw_ctx *ctx = handle;
	int r;

	if (ctx == NULL || ctx->loop == NULL)
		return -EINVAL;
	pw_loop_enter(ctx->loop);
	r = pw_loop_iterate(ctx->loop, timeout_ms);
	pw_loop_leave(ctx->loop);
	return r < 0 && r != -EINTR ? r : 0;
}

const struct graph_ops m4_graph_ops_pw = {
	.open = m4_graph_pw_open,
	.close = m4_graph_pw_close,
	.get_state = m4_graph_pw_get_state,
	.set_profile = m4_graph_pw_set_profile,
	.set_mic_volume = m4_graph_pw_set_mic_volume,
	.iterate = m4_graph_pw_iterate,
};

enum m4_pw_query_status m4_pw_query_snapshot(const char *mac_address, int timeout_ms,
					      struct m4_pw_snapshot *out)
{
	static const struct pw_core_events core_events = {
		.version = PW_VERSION_CORE_EVENTS,
		.done = on_core_done,
		.error = on_core_error,
	};
	static const struct pw_registry_events registry_events = {
		.version = PW_VERSION_REGISTRY_EVENTS,
		.global = on_registry_global,
	};
	struct m4_pw_query_ctx ctx;
	struct pw_loop *loop = NULL;
	struct pw_context *context = NULL;
	sd_event_source *pw_io_source = NULL;
	sd_event_source *timeout_source = NULL;
	enum m4_pw_query_status status = M4_PW_ERR_CONNECT;

	memset(out, 0, sizeof(*out));
	memset(&ctx, 0, sizeof(ctx));
	ctx.mac_address = mac_address;
	ctx.out = out;
	m4_pw_build_device_name(mac_address, ctx.device_name, sizeof(ctx.device_name));

	if (sd_event_new(&ctx.event) < 0)
		goto out;

	loop = pw_loop_new(NULL);
	if (loop == NULL)
		goto out;

	if (m4_loop_glue_attach(ctx.event, loop, &pw_io_source) < 0)
		goto out;

	context = pw_context_new(loop, NULL, 0);
	if (context == NULL)
		goto out;

	ctx.core = pw_context_connect(context, NULL, 0);
	if (ctx.core == NULL) {
		m4_log(M4_LOG_WARN, M4_T("PipeWire unavailable: %s",
"PipeWire недоступен: %s"), strerror(errno));
		goto out;
	}

	ctx.registry = pw_core_get_registry(ctx.core, PW_VERSION_REGISTRY, 0);
	if (ctx.registry == NULL)
		goto out;

	pw_core_add_listener(ctx.core, &ctx.core_listener, &core_events, &ctx);
	pw_registry_add_listener(ctx.registry, &ctx.registry_listener, &registry_events, &ctx);

	if (sd_event_add_time_relative(ctx.event, &timeout_source, CLOCK_MONOTONIC,
					(uint64_t)(timeout_ms > 0 ? timeout_ms : 1) * 1000, 1000,
					on_timeout, &ctx) < 0)
		goto out;

	ctx.pending_sync_seq = pw_core_sync(ctx.core, PW_ID_CORE, 0);

	/* Runs until finish()/on_timeout() calls sd_event_exit(). We reached
	 * this point with a live core connection, so from here on the query
	 * "ran" (M4_PW_OK) even if it timed out -- a timeout is reported via
	 * out->timed_out, not as a connection failure. */
	sd_event_loop(ctx.event);
	status = M4_PW_OK;

out:
	if (timeout_source != NULL)
		sd_event_source_unref(timeout_source);
	/* Every pw_registry_bind()/pw_core_get_registry() call allocates a
	 * struct pw_proxy (pw_proxy_new()) that is NOT freed by
	 * pw_core_disconnect()/pw_context_destroy() -- confirmed the hard way,
	 * via AddressSanitizer LeakSanitizer flagging exactly these three
	 * allocations (registry + device + node) on a live run before this
	 * cleanup was added. Must destroy them explicitly, in this order,
	 * before disconnecting the core (examples/gmain.c:92 does the same
	 * cast for the registry: it, too, is "just" a pw_proxy). */
	if (ctx.node_proxy != NULL)
		pw_proxy_destroy(ctx.node_proxy);
	if (ctx.device_proxy != NULL)
		pw_proxy_destroy(ctx.device_proxy);
	if (ctx.registry != NULL)
		pw_proxy_destroy((struct pw_proxy *)ctx.registry);
	if (ctx.core != NULL)
		pw_core_disconnect(ctx.core);
	if (context != NULL)
		pw_context_destroy(context);
	if (pw_io_source != NULL)
		sd_event_source_unref(pw_io_source);
	if (loop != NULL)
		pw_loop_destroy(loop);
	if (ctx.event != NULL)
		sd_event_unref(ctx.event);

	if (status != M4_PW_OK)
		memset(out, 0, sizeof(*out));
	return status;
}
