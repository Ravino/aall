#include "micdetect.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <spa/utils/dict.h>
#include <spa/utils/keys.h>
#include <spa/utils/string.h>

#include <pipewire/pipewire.h>

#include "loop_glue.h"
#include "../util/log.h"
#include "../../../common/m4-i18n.h"

/* ===================== pure decision logic ===================== */

static const char *const M4_MIC_EXCLUDE_BINARIES[] = {
	"pavucontrol",
	"pw-mon",
	"pw-cat",
};
#define M4_MIC_EXCLUDE_BINARIES_COUNT \
	(sizeof(M4_MIC_EXCLUDE_BINARIES) / sizeof(M4_MIC_EXCLUDE_BINARIES[0]))

/* `[automode] exclude-apps`/`holder-apps`
 * runtime override for the two compiled lists above/below -- see
 * micdetect.h's own comment on m4_mic_set_exclude_apps()/
 * m4_mic_set_holder_apps() for why this is a process-wide override rather
 * than a parameter threaded through m4_mic_binary_excluded()/
 * m4_mic_is_holder() (both stay pure, zero-argument-beyond-the-name
 * functions on purpose -- tests/test_micdetect.c calls them directly
 * against the compiled defaults and never touches this override). */
#define M4_MIC_OVERRIDE_MAX 16
#define M4_MIC_OVERRIDE_NAME_LEN 64

static char g_mic_exclude_override[M4_MIC_OVERRIDE_MAX][M4_MIC_OVERRIDE_NAME_LEN];
static size_t g_mic_exclude_override_count = 0;

static size_t mic_split_csv(const char *csv, char out[][M4_MIC_OVERRIDE_NAME_LEN], size_t max)
{
	const char *p = csv;
	size_t n = 0;

	while (p != NULL && *p != '\0' && n < max) {
		const char *comma = strchr(p, ',');
		size_t len = comma != NULL ? (size_t)(comma - p) : strlen(p);
		const char *start = p;

		while (len > 0 && (*start == ' ' || *start == '\t')) {
			start++;
			len--;
		}
		while (len > 0 && (start[len - 1] == ' ' || start[len - 1] == '\t'))
			len--;
		if (len > 0 && len < M4_MIC_OVERRIDE_NAME_LEN) {
			memcpy(out[n], start, len);
			out[n][len] = '\0';
			n++;
		}
		p = comma != NULL ? comma + 1 : NULL;
	}
	return n;
}

void m4_mic_set_exclude_apps(const char *comma_separated)
{
	if (comma_separated == NULL || comma_separated[0] == '\0') {
		g_mic_exclude_override_count = 0;
		return;
	}
	g_mic_exclude_override_count = mic_split_csv(comma_separated, g_mic_exclude_override, M4_MIC_OVERRIDE_MAX);
}

bool m4_mic_binary_excluded(const char *app_process_binary)
{
	size_t i;

	if (app_process_binary == NULL || app_process_binary[0] == '\0')
		return false;
	if (g_mic_exclude_override_count > 0) {
		for (i = 0; i < g_mic_exclude_override_count; i++) {
			if (strcmp(app_process_binary, g_mic_exclude_override[i]) == 0)
				return true;
		}
		return false;
	}
	for (i = 0; i < M4_MIC_EXCLUDE_BINARIES_COUNT; i++) {
		if (strcmp(app_process_binary, M4_MIC_EXCLUDE_BINARIES[i]) == 0)
			return true;
	}
	return false;
}

void m4_mic_placeholder_capture_name(const char *placeholder_name, char *out, size_t out_cap)
{
	if (out_cap == 0)
		return;
	if (placeholder_name == NULL || placeholder_name[0] == '\0') {
		out[0] = '\0';
		return;
	}
	snprintf(out, out_cap, "%s_capture", placeholder_name);
}

bool m4_mic_source_is_m4(const char *node_name, const char *mac_address,
			  const char *placeholder_name)
{
	static const char PREFIX[] = "bluez_input.";
	char prefix[64];
	size_t o;

	if (node_name == NULL)
		return false;

	if (placeholder_name != NULL && placeholder_name[0] != '\0' &&
	    strcmp(node_name, placeholder_name) == 0)
		return true;

	if (mac_address == NULL || mac_address[0] == '\0')
		return false;

	o = sizeof(PREFIX) - 1;
	if (o >= sizeof(prefix))
		return false;
	memcpy(prefix, PREFIX, o);
	for (const char *p = mac_address; *p != '\0' && o + 1 < sizeof(prefix); p++)
		prefix[o++] = (*p == ':') ? '_' : *p;
	if (o + 1 >= sizeof(prefix))
		return false;
	prefix[o++] = '.';
	prefix[o] = '\0';

	return strncmp(node_name, prefix, o) == 0;
}

const char *const M4_MIC_HOLDER_BINARIES[M4_MIC_HOLDER_BINARIES_COUNT] = {
	"signal-desktop",
};

static char g_mic_holder_override[M4_MIC_OVERRIDE_MAX][M4_MIC_OVERRIDE_NAME_LEN];
static size_t g_mic_holder_override_count = 0;

void m4_mic_set_holder_apps(const char *comma_separated)
{
	if (comma_separated == NULL || comma_separated[0] == '\0') {
		g_mic_holder_override_count = 0;
		return;
	}
	g_mic_holder_override_count = mic_split_csv(comma_separated, g_mic_holder_override, M4_MIC_OVERRIDE_MAX);
}

bool m4_mic_is_holder(const char *app_process_binary)
{
	size_t i;

	if (app_process_binary == NULL || app_process_binary[0] == '\0')
		return false;
	if (g_mic_holder_override_count > 0) {
		for (i = 0; i < g_mic_holder_override_count; i++) {
			if (strcmp(app_process_binary, g_mic_holder_override[i]) == 0)
				return true;
		}
		return false;
	}
	for (i = 0; i < M4_MIC_HOLDER_BINARIES_COUNT; i++) {
		if (strcmp(app_process_binary, M4_MIC_HOLDER_BINARIES[i]) == 0)
			return true;
	}
	return false;
}

bool m4_mic_decide_hold(const struct m4_mic_candidate *candidates, size_t n_candidates,
			 const char *placeholder_capture_name)
{
	struct m4_mic_candidate filtered[M4_MIC_MAX_CANDIDATES];
	size_t n_filtered = 0;
	size_t i;

	if (candidates == NULL)
		return false;

	for (i = 0; i < n_candidates && n_filtered < M4_MIC_MAX_CANDIDATES; i++) {
		if (m4_mic_is_holder(candidates[i].sink_app_binary))
			continue;
		filtered[n_filtered++] = candidates[i];
	}
	return m4_mic_decide(filtered, n_filtered, placeholder_capture_name);
}

bool m4_mic_decide(const struct m4_mic_candidate *candidates, size_t n_candidates,
		    const char *placeholder_capture_name)
{
	bool have_capture_name = placeholder_capture_name != NULL &&
				  placeholder_capture_name[0] != '\0';
	size_t i;

	if (candidates == NULL)
		return false;

	for (i = 0; i < n_candidates; i++) {
		const struct m4_mic_candidate *c = &candidates[i];

		if (!c->source_is_m4_mic)
			continue;
		if (strcmp(c->sink_media_class, "Stream/Input/Audio") != 0)
			continue;
		/* Order matches the prior implementation exactly:
		 * binary exclude set, then monitor, then
		 * passive, then our own placeholder capture by name. */
		if (m4_mic_binary_excluded(c->sink_app_binary))
			continue;
		if (c->sink_stream_monitor)
			continue;
		if (c->sink_passive)
			continue;
		if (have_capture_name && strcmp(c->sink_node_name, placeholder_capture_name) == 0)
			continue;
		return true;
	}
	return false;
}

/* ===================== live PipeWire glue ===================== */

/* Generous desktop-scale caps (graph_pw.h's own convention: silently drop
 * past the cap rather than fail). Only Stream/Input/Audio nodes and
 * candidate mic-source nodes are stored at all (see micdetect.h's top
 * comment) -- everything else (sinks, other devices, monitor ports, etc.)
 * is ignored at registry.global time and never occupies a slot. */
#define M4_MIC_MAX_NODES 256
#define M4_MIC_MAX_LINKS 256

struct m4_micdetect_ctx;

struct mic_node {
	bool used;   /* tombstoned (not compacted) on removal -- see remove_node():
		      * this struct's address is handed to pw_node_add_listener()
		      * as the listener's `data` and MUST stay stable for as long
		      * as `proxy`/`listener` are alive, so slots are never moved/
		      * swapped, only freed in place and reused for a LATER,
		      * unrelated node. */
	uint32_t id;
	char node_name[128];
	bool is_stream_input; /* media.class == "Stream/Input/Audio" */
	bool is_mic_source;   /* m4_mic_source_is_m4(node_name, ...) */

	struct m4_micdetect_ctx *ctx; /* back-pointer, for the node info callback
					* to reach recompute() -- valid only while
					* `used` (set together with it) */

	/* Only used/valid when is_stream_input: registry.global's props for
	 * a Node are a REDUCED set (micdetect.h's top comment, live-
	 * confirmed) -- node.passive/stream.monitor/
	 * application.process.binary are only available after binding and
	 * reading the bound object's `info` event. */
	struct pw_proxy *proxy;
	struct spa_hook listener;
	bool have_props;
	bool passive;
	bool stream_monitor;
	char app_binary[128];
};

struct mic_link {
	uint32_t id;
	uint32_t output_node_id;
	uint32_t input_node_id;
};

struct m4_micdetect_ctx {
	sd_event *event; /* caller's loop -- NOT owned */
	char mac_address[32];
	char placeholder_name[64];
	char placeholder_capture_name[80];
	m4_micdetect_cb cb;
	void *user_data;

	struct pw_loop *loop;
	struct pw_context *context;
	sd_event_source *pw_io_source;
	struct pw_core *core;
	struct pw_registry *registry;
	struct spa_hook core_listener;
	struct spa_hook registry_listener;

	struct mic_node nodes[M4_MIC_MAX_NODES];
	size_t n_node_slots; /* high-water mark: slots [0, n_node_slots) may be
			       * used or tombstoned; slots beyond it are untouched */

	/* Links have no embedded proxy/listener and nothing else ever keeps a
	 * pointer into this array between calls, so swap-remove (unlike nodes)
	 * is safe here. */
	struct mic_link links[M4_MIC_MAX_LINKS];
	size_t n_links;

	bool have_result;  /* at least one recompute() has run -- distinguishes
			     * "not settled yet" (mic_active reported as false by
			     * m4_micdetect_get_active(), but no callback fired
			     * yet either) from a genuine, settled false result */
	bool mic_active;
	bool hold_active;  /* Bug 1 -- see micdetect.h's "long-lived holders" section */
};

static struct mic_node *find_node(struct m4_micdetect_ctx *ctx, uint32_t id)
{
	for (size_t i = 0; i < ctx->n_node_slots; i++)
		if (ctx->nodes[i].used && ctx->nodes[i].id == id)
			return &ctx->nodes[i];
	return NULL;
}

static struct mic_node *alloc_node_slot(struct m4_micdetect_ctx *ctx)
{
	for (size_t i = 0; i < ctx->n_node_slots; i++)
		if (!ctx->nodes[i].used)
			return &ctx->nodes[i];
	if (ctx->n_node_slots < M4_MIC_MAX_NODES)
		return &ctx->nodes[ctx->n_node_slots++];
	return NULL;
}

static bool media_class_is_stream_input(const char *media_class)
{
	return media_class != NULL && strcmp(media_class, "Stream/Input/Audio") == 0;
}

static void recompute(struct m4_micdetect_ctx *ctx)
{
	struct m4_mic_candidate candidates[M4_MIC_MAX_CANDIDATES];
	size_t n_candidates = 0;
	bool new_active, new_hold_active;

	for (size_t i = 0; i < ctx->n_links && n_candidates < M4_MIC_MAX_CANDIDATES; i++) {
		struct mic_node *src, *dst;
		struct m4_mic_candidate *c;

		src = find_node(ctx, ctx->links[i].output_node_id);
		if (src == NULL || !src->is_mic_source)
			continue;
		dst = find_node(ctx, ctx->links[i].input_node_id);
		/* Only Stream/Input/Audio nodes are ever stored (see
		 * on_registry_global()), so a missing/non-stream-input `dst`
		 * here means the link's input is something else entirely --
		 * correctly excluded by just not building a candidate for it.
		 * A `dst` that IS stored but whose info hasn't arrived yet
		 * (have_props still false, right after it first appeared) is
		 * also skipped on purpose: better to transiently under-report
		 * "mic in use" for the brief window until its info event
		 * lands (recompute() runs again then, from on_node_info())
		 * than to over-report based on not-yet-known passive/monitor/
		 * binary values -- automode.c driving HFP entry is exactly
		 * the kind of action a spurious blip must never trigger. */
		if (dst == NULL || !dst->is_stream_input || !dst->have_props)
			continue;

		c = &candidates[n_candidates++];
		memset(c, 0, sizeof(*c));
		c->source_is_m4_mic = true;
		snprintf(c->sink_media_class, sizeof(c->sink_media_class), "%s", "Stream/Input/Audio");
		snprintf(c->sink_node_name, sizeof(c->sink_node_name), "%s", dst->node_name);
		c->sink_passive = dst->passive;
		c->sink_stream_monitor = dst->stream_monitor;
		snprintf(c->sink_app_binary, sizeof(c->sink_app_binary), "%s", dst->app_binary);
	}

	new_active = m4_mic_decide(candidates, n_candidates,
				    ctx->placeholder_capture_name[0] != '\0' ?
					    ctx->placeholder_capture_name : NULL);
	new_hold_active = m4_mic_decide_hold(candidates, n_candidates,
					      ctx->placeholder_capture_name[0] != '\0' ?
						      ctx->placeholder_capture_name : NULL);

	if (ctx->have_result && new_active == ctx->mic_active && new_hold_active == ctx->hold_active)
		return;

	ctx->have_result = true;
	ctx->mic_active = new_active;
	ctx->hold_active = new_hold_active;
	if (ctx->cb != NULL)
		ctx->cb(ctx->user_data, ctx->mic_active, ctx->hold_active);
}

static void on_node_info(void *data, const struct pw_node_info *info)
{
	struct mic_node *n = data;
	const char *v;

	if (info->props != NULL) {
		v = spa_dict_lookup(info->props, PW_KEY_NODE_PASSIVE);
		n->passive = v != NULL && spa_streq(v, "true");

		v = spa_dict_lookup(info->props, PW_KEY_STREAM_MONITOR);
		n->stream_monitor = v != NULL && spa_streq(v, "true");

		v = spa_dict_lookup(info->props, PW_KEY_APP_PROCESS_BINARY);
		if (v != NULL)
			snprintf(n->app_binary, sizeof(n->app_binary), "%s", v);
	}
	n->have_props = true;
	recompute(n->ctx);
}

static void bind_node(struct m4_micdetect_ctx *ctx, struct mic_node *n)
{
	static const struct pw_node_events node_events = {
		.version = PW_VERSION_NODE_EVENTS,
		.info = on_node_info,
	};

	n->proxy = pw_registry_bind(ctx->registry, n->id, PW_TYPE_INTERFACE_Node,
				     PW_VERSION_NODE, 0);
	if (n->proxy == NULL) {
		m4_log(M4_LOG_WARN, M4_T("PipeWire (micdetect): failed to bind to node id=%u",
"PipeWire (micdetect): не удалось привязаться к узлу id=%u"), n->id);
		return;
	}
	pw_node_add_listener((struct pw_node *)n->proxy, &n->listener, &node_events, n);
}

static void on_registry_global(void *data, uint32_t id, uint32_t permissions,
				const char *type, uint32_t version,
				const struct spa_dict *props)
{
	struct m4_micdetect_ctx *ctx = data;
	(void)permissions;
	(void)version;

	if (props == NULL)
		return;

	if (spa_streq(type, PW_TYPE_INTERFACE_Node)) {
		const char *name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
		const char *media_class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
		bool is_stream_input = media_class_is_stream_input(media_class);
		bool is_mic_source = m4_mic_source_is_m4(name, ctx->mac_address,
							  ctx->placeholder_name[0] != '\0' ?
								  ctx->placeholder_name : NULL);
		struct mic_node *n;

		if (!is_stream_input && !is_mic_source)
			return; /* not interesting -- never stored (see struct comment) */

		n = alloc_node_slot(ctx);
		if (n == NULL)
			return; /* cap reached; silently drop, same convention as graph_pw.h */

		memset(n, 0, sizeof(*n));
		n->used = true;
		n->id = id;
		n->ctx = ctx;
		if (name != NULL)
			snprintf(n->node_name, sizeof(n->node_name), "%s", name);
		n->is_stream_input = is_stream_input;
		n->is_mic_source = is_mic_source;

		if (is_stream_input)
			bind_node(ctx, n);

		recompute(ctx);
	} else if (spa_streq(type, PW_TYPE_INTERFACE_Link)) {
		const char *out_s = spa_dict_lookup(props, PW_KEY_LINK_OUTPUT_NODE);
		const char *in_s = spa_dict_lookup(props, PW_KEY_LINK_INPUT_NODE);
		struct mic_link *l;

		/* Link globals carry link.output.node/link.input.node directly
		 * in registry.global's props (live-confirmed, micdetect.h's
		 * top comment) -- no binding needed. */
		if (out_s == NULL || in_s == NULL)
			return;
		if (ctx->n_links >= M4_MIC_MAX_LINKS)
			return;

		l = &ctx->links[ctx->n_links++];
		l->id = id;
		l->output_node_id = (uint32_t)strtoul(out_s, NULL, 10);
		l->input_node_id = (uint32_t)strtoul(in_s, NULL, 10);

		recompute(ctx);
	}
}

static void on_registry_global_remove(void *data, uint32_t id)
{
	struct m4_micdetect_ctx *ctx = data;
	bool changed = false;

	for (size_t i = 0; i < ctx->n_node_slots; i++) {
		if (ctx->nodes[i].used && ctx->nodes[i].id == id) {
			if (ctx->nodes[i].proxy != NULL)
				pw_proxy_destroy(ctx->nodes[i].proxy);
			memset(&ctx->nodes[i], 0, sizeof(ctx->nodes[i]));
			ctx->nodes[i].used = false;
			changed = true;
			break;
		}
	}
	for (size_t i = 0; i < ctx->n_links; i++) {
		if (ctx->links[i].id == id) {
			ctx->links[i] = ctx->links[ctx->n_links - 1];
			ctx->n_links--;
			changed = true;
			break;
		}
	}
	if (changed)
		recompute(ctx);
}

static void on_core_error(void *data, uint32_t id, int seq, int res, const char *message)
{
	(void)data;
	(void)seq;
	m4_log(M4_LOG_WARN, M4_T("PipeWire (micdetect): error id=%u res=%d: %s",
"PipeWire (micdetect): ошибка id=%u res=%d: %s"), id, res,
	       message != NULL ? message : "");
}

void *m4_micdetect_open(sd_event *event, const struct m4_micdetect_config *cfg,
			 m4_micdetect_cb cb, void *user_data)
{
	static const struct pw_registry_events registry_events = {
		.version = PW_VERSION_REGISTRY_EVENTS,
		.global = on_registry_global,
		.global_remove = on_registry_global_remove,
	};
	static const struct pw_core_events core_events = {
		.version = PW_VERSION_CORE_EVENTS,
		.error = on_core_error,
	};
	struct m4_micdetect_ctx *ctx;

	if (event == NULL || cfg == NULL)
		return NULL;

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL)
		return NULL;

	ctx->event = event;
	ctx->cb = cb;
	ctx->user_data = user_data;
	if (cfg->mac_address != NULL)
		snprintf(ctx->mac_address, sizeof(ctx->mac_address), "%s", cfg->mac_address);
	if (cfg->placeholder_name != NULL)
		snprintf(ctx->placeholder_name, sizeof(ctx->placeholder_name), "%s",
			 cfg->placeholder_name);
	m4_mic_placeholder_capture_name(ctx->placeholder_name[0] != '\0' ? ctx->placeholder_name : NULL,
					 ctx->placeholder_capture_name,
					 sizeof(ctx->placeholder_capture_name));

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
		m4_log(M4_LOG_WARN, M4_T("PipeWire unavailable (micdetect): %s",
"PipeWire недоступен (micdetect): %s"), strerror(errno));
		goto fail;
	}
	ctx->registry = pw_core_get_registry(ctx->core, PW_VERSION_REGISTRY, 0);
	if (ctx->registry == NULL)
		goto fail;

	pw_core_add_listener(ctx->core, &ctx->core_listener, &core_events, ctx);
	pw_registry_add_listener(ctx->registry, &ctx->registry_listener, &registry_events, ctx);

	/* No initial pw_core_sync()/core.done step: unlike graph_ops (which
	 * needs a well-defined "the registry burst is over" point to decide
	 * whether the M4 Device exists at all), mic detection has no such
	 * single yes/no moment -- recompute() naturally converges to the
	 * right answer as Node/Link globals stream in and node info events
	 * land, firing the callback (once, per micdetect.h's contract) as
	 * soon as it first produces a result. */

	return ctx;

fail:
	m4_micdetect_close(ctx);
	return NULL;
}

void m4_micdetect_close(void *handle)
{
	struct m4_micdetect_ctx *ctx = handle;

	if (ctx == NULL)
		return;

	for (size_t i = 0; i < ctx->n_node_slots; i++) {
		if (ctx->nodes[i].used && ctx->nodes[i].proxy != NULL)
			pw_proxy_destroy(ctx->nodes[i].proxy);
	}
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

bool m4_micdetect_get_active(const void *handle)
{
	const struct m4_micdetect_ctx *ctx = handle;

	return ctx != NULL && ctx->mic_active;
}

bool m4_micdetect_get_hold_active(const void *handle)
{
	const struct m4_micdetect_ctx *ctx = handle;

	return ctx != NULL && ctx->hold_active;
}
