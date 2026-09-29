/* cli/graph_host.c -- see graph_host.h.
 *
 * Shutdown handshake: SIGINT/SIGTERM -> do_quit(). If force-quantum was
 * actually applied (fq_applied), do_quit() asks the metadata object to set
 * it back to 0 and waits (bounded by a 2s safety timer) for that write to
 * round-trip (pw_core_sync()/on_core_done(), the same pattern PipeWire's
 * own pw-metadata tool uses) before quitting the main loop -- this is the
 * in-process equivalent of
 * `pw-metadata -n settings 0 clock.force-quantum 0`. If
 * force-quantum was never applied (force_quantum<=0, the community
 * default), do_quit() quits the loop immediately -- there is nothing to
 * restore. Either way, `pw_context_destroy()` on the way out tears down
 * every stream the two loaded modules created; the real pipewire server
 * removes graph-host's client + its nodes the moment the connection closes
 * (true for a clean exit AND for `kill -9`, same property already relied
 * on for the qemu-hexagon helper).
 */
#include "graph_host.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <spa/utils/dict.h>
#include <spa/utils/hook.h>
#include <spa/utils/json.h>
#include <spa/utils/keys.h>
#include <spa/utils/result.h>
#include <spa/utils/string.h>

#include <pipewire/extensions/metadata.h>
#include <pipewire/impl.h>
#include <pipewire/pipewire.h>
#include <systemd/sd-daemon.h>

#include "../../../common/m4-conf.h"
#include "../../../common/m4-i18n.h"
#include "../graph/graph_host_build.h"
#include "../util/config.h"
#include "../util/log.h"

#define SHUTDOWN_TIMEOUT_SEC 2

struct graph_host {
	struct pw_main_loop *loop;
	struct pw_context *context;

	struct pw_impl_module *combine_module;
	struct spa_hook combine_module_listener;
	struct pw_impl_module *mic_module;
	struct spa_hook mic_module_listener;

	/* second, independent connection used ONLY for clock.force-quantum
	 * (see this file's own top comment) -- NULL entirely when
	 * force_quantum<=0, so the common case (default 0) never opens
	 * a second connection at all. */
	long force_quantum;
	struct pw_core *core;
	struct spa_hook core_listener;
	struct pw_registry *registry;
	struct spa_hook registry_listener;
	struct pw_metadata *settings_metadata;
	struct spa_hook metadata_listener;
	bool fq_applied;
	int shutdown_sync;
	bool shutting_down;
	struct spa_source *shutdown_timer;

	int exit_code;
};

static void request_shutdown(struct graph_host *g, int exit_code)
{
	if (g->shutting_down)
		return;
	g->shutting_down = true;
	g->exit_code = exit_code;

	if (g->force_quantum > 0 && g->fq_applied && g->settings_metadata != NULL) {
		m4_log(M4_LOG_INFO, M4_T("graph-host: resetting clock.force-quantum to 0",
"graph-host: возвращаю clock.force-quantum в 0"));
		pw_metadata_set_property(g->settings_metadata, 0, "clock.force-quantum", NULL, "0");
		g->shutdown_sync = pw_core_sync(g->core, PW_ID_CORE, 0);
		if (g->shutdown_timer != NULL) {
			struct timespec ts;
			ts.tv_sec = SHUTDOWN_TIMEOUT_SEC;
			ts.tv_nsec = 0;
			pw_loop_update_timer(pw_main_loop_get_loop(g->loop),
					      g->shutdown_timer, &ts, NULL, false);
		}
		return;
	}
	pw_main_loop_quit(g->loop);
}

static void do_quit_signal(void *data, int signal_number)
{
	struct graph_host *g = data;

	(void)signal_number;
	request_shutdown(g, 0);
}

static void shutdown_timeout(void *data, uint64_t expirations)
{
	struct graph_host *g = data;

	(void)expirations;
	m4_log(M4_LOG_WARN, M4_T("graph-host: no confirmation of the clock.force-quantum reset within %ds, forcing exit",
"graph-host: подтверждение сброса clock.force-quantum не пришло за %ds, выхожу принудительно"),
	       SHUTDOWN_TIMEOUT_SEC);
	pw_main_loop_quit(g->loop);
}

static void on_core_done(void *data, uint32_t id, int seq)
{
	struct graph_host *g = data;

	if (id == PW_ID_CORE && g->shutting_down && seq == g->shutdown_sync)
		pw_main_loop_quit(g->loop);
}

static void on_core_error(void *data, uint32_t id, int seq, int res, const char *message)
{
	struct graph_host *g = data;

	(void)seq;
	m4_log(M4_LOG_WARN, "graph-host: core error id:%u res:%d (%s): %s",
	       id, res, spa_strerror(res), message);
	if (id == PW_ID_CORE && res == -EPIPE)
		pw_main_loop_quit(g->loop);
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.done = on_core_done,
	.error = on_core_error,
};

static void registry_event_global(void *data, uint32_t id, uint32_t permissions,
				   const char *type, uint32_t version, const struct spa_dict *props)
{
	struct graph_host *g = data;
	const char *name;

	(void)permissions;
	(void)version;
	if (!spa_streq(type, PW_TYPE_INTERFACE_Metadata) || props == NULL)
		return;
	name = spa_dict_lookup(props, PW_KEY_METADATA_NAME);
	if (name == NULL || !spa_streq(name, "settings"))
		return;
	if (g->settings_metadata != NULL)
		return;

	g->settings_metadata = pw_registry_bind(g->registry, id, type, PW_VERSION_METADATA, 0);
	if (g->settings_metadata == NULL) {
		m4_log(M4_LOG_WARN, M4_T("graph-host: failed to bind the \"settings\" metadata: %m",
"graph-host: не удалось привязать metadata \"settings\": %m"));
		return;
	}
	{
		char val[32];

		snprintf(val, sizeof(val), "%ld", g->force_quantum);
		pw_metadata_set_property(g->settings_metadata, 0, "clock.force-quantum", NULL, val);
		g->fq_applied = true;
		m4_log(M4_LOG_INFO, "graph-host: clock.force-quantum = %s", val);
	}
}

static const struct pw_registry_events registry_events = {
	PW_VERSION_REGISTRY_EVENTS,
	.global = registry_event_global,
};

static int start_force_quantum(struct graph_host *g)
{
	g->core = pw_context_connect(g->context, pw_properties_new(NULL, NULL), 0);
	if (g->core == NULL) {
		m4_log(M4_LOG_WARN, M4_T("graph-host: force-quantum: failed to connect to PipeWire (%m), skipping",
"graph-host: force-quantum: не удалось подключиться к PipeWire (%m), пропускаю"));
		return -1;
	}
	pw_core_add_listener(g->core, &g->core_listener, &core_events, g);
	g->registry = pw_core_get_registry(g->core, PW_VERSION_REGISTRY, 0);
	if (g->registry == NULL) {
		m4_log(M4_LOG_WARN, M4_T("graph-host: force-quantum: failed to get the registry (%m)",
"graph-host: force-quantum: не удалось получить registry (%m)"));
		return -1;
	}
	pw_registry_add_listener(g->registry, &g->registry_listener, &registry_events, g);
	g->shutdown_timer = pw_loop_add_timer(pw_main_loop_get_loop(g->loop), shutdown_timeout, g);
	return 0;
}

/* This fires BOTH on a genuine unexpected module crash AND on our OWN
 * deliberate `pw_impl_module_destroy()` calls in the normal shutdown path
 * at the end of m4_cli_graph_host_run() (destroying a pw_impl_module
 * synchronously invokes its own `.destroy` listeners, there is no way to
 * tell "I asked for this" apart from "it just died" other than checking
 * `g->shutting_down` ourselves first) -- confirmed live (isolated PipeWire):
 * without this
 * check, every ordinary SIGTERM shutdown logged a scary, wrong "module ...
 * exited unexpectedly" warning for both modules. Only treat it as the
 * unexpected/crash case when we did NOT already ask to shut down. */
static void module_destroy_generic(struct graph_host *g, const char *which)
{
	if (g->shutting_down)
		return;
	m4_log(M4_LOG_WARN, M4_T("graph-host: module %s exited unexpectedly, exiting (systemd will restart graph-host entirely)",
"graph-host: модуль %s неожиданно завершился, выхожу (systemd перезапустит graph-host целиком)"),
	       which);
	request_shutdown(g, 1);
}

static void combine_module_destroy(void *data)
{
	struct graph_host *g = data;

	spa_hook_remove(&g->combine_module_listener);
	module_destroy_generic(g, "combine");
	g->combine_module = NULL;
}

static const struct pw_impl_module_events combine_module_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = combine_module_destroy,
};

static void mic_module_destroy(void *data)
{
	struct graph_host *g = data;

	spa_hook_remove(&g->mic_module_listener);
	module_destroy_generic(g, "mic-placeholder");
	g->mic_module = NULL;
}

static const struct pw_impl_module_events mic_module_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = mic_module_destroy,
};

/* combine-members="auto" (not a registry default -- an opt-in convenience
 * value users can type in momentum4.conf, documented in docs/user/CONFIG.md):
 * resolves, ONCE at startup, to whatever the CURRENT default Audio/Sink is
 * (via the "default" metadata's "default.audio.sink" key), excluding any
 * bluez5 node (api.bluez5.address present -- M4 is added separately anyway,
 * and combining two different BT sinks is not a sane default). This is a
 * static, one-shot resolution -- if the default sink changes later,
 * combine-members keeps pointing at whatever it resolved to at graph-host
 * startup (restart graph-host to re-resolve; documented limitation, still an
 * open question, not addressed here). Returns true and
 * fills `out` (node.name, single entry, no trailing comma) if a suitable
 * default was found; false (leaves `out` untouched) otherwise -- the caller
 * then falls back to "M4 only" (also a perfectly valid, if trivial, combine
 * sink), never fails outright. */
struct auto_resolve_state {
	struct pw_main_loop *loop;
	struct pw_core *core;
	struct pw_registry *registry;
	struct pw_metadata *metadata;
	struct spa_hook metadata_listener;
	struct spa_hook core_listener;
	char default_sink_name[256];
	bool have_default_sink_name;
	bool bluez_excluded;
	int sync;
	bool done;
};

static int auto_resolve_metadata_property(void *data, uint32_t id, const char *key,
					   const char *type, const char *value)
{
	struct auto_resolve_state *st = data;

	(void)type;
	if (id != 0 || key == NULL || value == NULL)
		return 0;
	if (!spa_streq(key, "default.audio.sink"))
		return 0;
	{
		struct spa_json it[2];
		char name_buf[256];

		spa_json_init(&it[0], value, strlen(value));
		if (spa_json_enter_object(&it[0], &it[1]) <= 0)
			return 0;
		{
			char k[64];
			const char *v;
			int len;

			while ((len = spa_json_object_next(&it[1], k, sizeof(k), &v)) > 0) {
				if (spa_streq(k, "name")) {
					if (spa_json_parse_stringn(v, len, name_buf, sizeof(name_buf)) > 0) {
						snprintf(st->default_sink_name,
							 sizeof(st->default_sink_name), "%s", name_buf);
						st->have_default_sink_name = true;
					}
				}
			}
		}
	}
	return 0;
}

static const struct pw_metadata_events auto_resolve_metadata_events = {
	PW_VERSION_METADATA_EVENTS,
	.property = auto_resolve_metadata_property,
};

static void auto_resolve_registry_global(void *data, uint32_t id, uint32_t permissions,
					  const char *type, uint32_t version, const struct spa_dict *props)
{
	struct auto_resolve_state *st = data;

	(void)permissions;
	(void)version;
	if (spa_streq(type, PW_TYPE_INTERFACE_Metadata) && props != NULL) {
		const char *name = spa_dict_lookup(props, PW_KEY_METADATA_NAME);

		if (name != NULL && spa_streq(name, "default") && st->metadata == NULL) {
			st->metadata = pw_registry_bind(st->registry, id, type, PW_VERSION_METADATA, 0);
			pw_metadata_add_listener(st->metadata, &st->metadata_listener,
						  &auto_resolve_metadata_events, st);
		}
		return;
	}
	if (spa_streq(type, PW_TYPE_INTERFACE_Node) && props != NULL && st->have_default_sink_name) {
		const char *node_name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
		const char *media_class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);

		if (node_name != NULL && spa_streq(node_name, st->default_sink_name) &&
		    media_class != NULL && spa_streq(media_class, "Audio/Sink")) {
			if (spa_dict_lookup(props, "api.bluez5.address") != NULL)
				st->bluez_excluded = true;
		}
	}
}

static const struct pw_registry_events auto_resolve_registry_events = {
	PW_VERSION_REGISTRY_EVENTS,
	.global = auto_resolve_registry_global,
};

static void auto_resolve_core_done(void *data, uint32_t id, int seq)
{
	struct auto_resolve_state *st = data;

	if (id == PW_ID_CORE && seq == st->sync)
		st->done = true;
}

static void auto_resolve_core_error(void *data, uint32_t id, int seq, int res, const char *message)
{
	struct auto_resolve_state *st = data;

	(void)id;
	(void)seq;
	(void)res;
	(void)message;
	st->done = true;
}

static const struct pw_core_events auto_resolve_core_events = {
	PW_VERSION_CORE_EVENTS,
	.done = auto_resolve_core_done,
	.error = auto_resolve_core_error,
};

/* Runs its OWN short-lived pw_main_loop (separate from graph-host's real
 * one, which has not started yet at this point) -- a bounded, synchronous
 * one-shot query, same shape as pw/graph_pw.c's m4_pw_query_snapshot()
 * reuses for other one-shot reads. `timeout_ms` bounds
 * total wall time (default sink may legitimately not exist yet on a very
 * fresh session). */
static bool resolve_auto_combine_member(struct pw_context *context, char *out, size_t out_cap,
					 int timeout_ms)
{
	struct pw_main_loop *loop;
	struct auto_resolve_state st;
	struct pw_core *core;
	struct spa_hook registry_listener;
	struct pw_loop *pl;
	struct timespec deadline, now;
	bool ok = false;

	memset(&st, 0, sizeof(st));
	memset(&registry_listener, 0, sizeof(registry_listener));

	loop = pw_main_loop_new(NULL);
	if (loop == NULL)
		return false;
	st.loop = loop;

	core = pw_context_connect(context, pw_properties_new(NULL, NULL), 0);
	if (core == NULL) {
		pw_main_loop_destroy(loop);
		return false;
	}
	st.core = core;
	pw_core_add_listener(core, &st.core_listener, &auto_resolve_core_events, &st);
	st.registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
	if (st.registry == NULL) {
		pw_core_disconnect(core);
		pw_main_loop_destroy(loop);
		return false;
	}
	pw_registry_add_listener(st.registry, &registry_listener, &auto_resolve_registry_events, &st);

	st.sync = pw_core_sync(core, PW_ID_CORE, 0);

	/* Bounded wait: iterate the loop directly instead of pw_main_loop_run()
	 * + a quitting timer callback -- this is a simple, one-shot
	 * "poll until done-or-timeout", same shape pw/graph_pw.c's own
	 * m4_pw_query_snapshot() uses for its bounded live reads. */
	pl = pw_main_loop_get_loop(loop);
	clock_gettime(CLOCK_MONOTONIC, &deadline);
	deadline.tv_sec += timeout_ms / 1000;
	deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
	if (deadline.tv_nsec >= 1000000000L) {
		deadline.tv_sec++;
		deadline.tv_nsec -= 1000000000L;
	}
	while (!st.done) {
		pw_loop_iterate(pl, 50);
		clock_gettime(CLOCK_MONOTONIC, &now);
		if (now.tv_sec > deadline.tv_sec ||
		    (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec))
			break;
	}

	if (st.have_default_sink_name && !st.bluez_excluded &&
	    strlen(st.default_sink_name) + 1 <= out_cap) {
		snprintf(out, out_cap, "%s", st.default_sink_name);
		ok = true;
	}

	spa_hook_remove(&registry_listener);
	if (st.metadata != NULL) {
		spa_hook_remove(&st.metadata_listener);
		pw_proxy_destroy((struct pw_proxy *)st.metadata);
	}
	pw_proxy_destroy((struct pw_proxy *)st.registry);
	spa_hook_remove(&st.core_listener);
	pw_core_disconnect(core);
	pw_main_loop_destroy(loop);
	return ok;
}

int m4_cli_graph_host_run(int argc, char **argv)
{
	struct graph_host g;
	struct m4_graph_host_config cfg;
	char args[M4_GRAPH_HOST_ARGS_BUF_MIN];
	char resolved_members[512];
	int n;

	(void)argv;
	if (argc != 0) {
		fprintf(stderr, M4_T("usage: momentum4 graph-host\n"
				"  (configured through [graph] in momentum4.conf, no arguments)\n",
"usage: momentum4 graph-host\n"
				"  (конфигурируется через [graph] в momentum4.conf, без аргументов)\n"));
		return 1;
	}

	memset(&g, 0, sizeof(g));
	memset(&cfg, 0, sizeof(cfg));

	cfg.combine_enable = m4_conf_get_bool(m4_config(), "graph", "combine", false);
	cfg.combine_name = m4_conf_get_string(m4_config(), "graph", "combine-name");
	cfg.combine_members_csv = m4_conf_get_string(m4_config(), "graph", "combine-members");
	cfg.combine_latency_compensate =
		m4_conf_get_bool(m4_config(), "graph", "combine-latency-compensate", false);
	cfg.mic_placeholder_enable = m4_conf_get_bool(m4_config(), "graph", "mic-placeholder", false);
	cfg.mic_name = m4_conf_get_string(m4_config(), "graph", "mic-name");
	cfg.m4_mac = m4_config_mac_address();
	g.force_quantum = m4_conf_get_int(m4_config(), "graph", "force-quantum", 0);

	if (!cfg.combine_enable && !cfg.mic_placeholder_enable && g.force_quantum <= 0)
		m4_log(M4_LOG_INFO, M4_T("graph-host: [graph] combine=off mic-placeholder=off force-quantum=0 -- "
				     "nothing to load, just waiting for SIGTERM (systemd unit -- Restart=always, "
				     "not exiting immediately: `momentum4 reload` does not yet re-read [graph] live, "
				     "restarting this unit is the only way to pick up a new config)",
"graph-host: [graph] combine=off mic-placeholder=off force-quantum=0 -- "
				     "нечего загружать, просто жду SIGTERM (systemd unit -- Restart=always, "
				     "не выходим сразу: `momentum4 reload` пока не перечитывает [graph] на лету, "
				     "перезапуск этого юнита -- единственный способ подхватить новый конфиг)"));

	g.loop = pw_main_loop_new(NULL);
	if (g.loop == NULL) {
		m4_log(M4_LOG_ERR, "graph-host: pw_main_loop_new: %m");
		return 2;
	}
	g.context = pw_context_new(pw_main_loop_get_loop(g.loop), NULL, 0);
	if (g.context == NULL) {
		m4_log(M4_LOG_ERR, "graph-host: pw_context_new: %m");
		pw_main_loop_destroy(g.loop);
		return 2;
	}

	pw_loop_add_signal(pw_main_loop_get_loop(g.loop), SIGINT, do_quit_signal, &g);
	pw_loop_add_signal(pw_main_loop_get_loop(g.loop), SIGTERM, do_quit_signal, &g);

	if (cfg.combine_enable && cfg.combine_members_csv != NULL &&
	    strcmp(cfg.combine_members_csv, "auto") == 0) {
		if (resolve_auto_combine_member(g.context, resolved_members, sizeof(resolved_members), 3000)) {
			m4_log(M4_LOG_INFO, "graph-host: combine-members=auto -> \"%s\"", resolved_members);
			cfg.combine_members_csv = resolved_members;
		} else {
			m4_log(M4_LOG_WARN, M4_T("graph-host: combine-members=auto: found no suitable non-bluez "
					     "default Audio/Sink -- the combined sink will contain only the M4",
"graph-host: combine-members=auto: не нашёл подходящий не-bluez "
					     "default Audio/Sink -- комбинированный sink будет содержать только M4"));
			cfg.combine_members_csv = "";
		}
	}

	if (cfg.combine_enable) {
		n = m4_graph_host_build_combine_args(&cfg, args, sizeof(args));
		if (n < 0) {
			m4_log(M4_LOG_ERR, M4_T("graph-host: failed to build the combine-stream arguments (see [graph] combine-members)",
"graph-host: не удалось собрать аргументы combine-stream (см. [graph] combine-members)"));
			goto fail_load;
		}
		g.combine_module = pw_context_load_module(g.context, "libpipewire-module-combine-stream", args, NULL);
		if (g.combine_module == NULL) {
			m4_log(M4_LOG_ERR, M4_T("graph-host: failed to load libpipewire-module-combine-stream: %m",
"graph-host: не удалось загрузить libpipewire-module-combine-stream: %m"));
			goto fail_load;
		}
		pw_impl_module_add_listener(g.combine_module, &g.combine_module_listener, &combine_module_events, &g);
		m4_log(M4_LOG_INFO, M4_T("graph-host: combine-stream loaded (%s)",
"graph-host: combine-stream загружен (%s)"),
		       cfg.combine_name != NULL ? cfg.combine_name : "m4_combined");
	}

	if (cfg.mic_placeholder_enable) {
		n = m4_graph_host_build_mic_args(&cfg, args, sizeof(args));
		if (n < 0) {
			m4_log(M4_LOG_ERR, M4_T("graph-host: failed to build the mic-placeholder arguments",
"graph-host: не удалось собрать аргументы mic-placeholder"));
			goto fail_load;
		}
		g.mic_module = pw_context_load_module(g.context, "libpipewire-module-loopback", args, NULL);
		if (g.mic_module == NULL) {
			m4_log(M4_LOG_ERR, M4_T("graph-host: failed to load libpipewire-module-loopback: %m",
"graph-host: не удалось загрузить libpipewire-module-loopback: %m"));
			goto fail_load;
		}
		pw_impl_module_add_listener(g.mic_module, &g.mic_module_listener, &mic_module_events, &g);
		m4_log(M4_LOG_INFO, M4_T("graph-host: mic-placeholder loaded (%s)",
"graph-host: mic-placeholder загружен (%s)"),
		       cfg.mic_name != NULL ? cfg.mic_name : "momentum4_mic");
	}

	if (g.force_quantum > 0) {
		if (start_force_quantum(&g) != 0)
			m4_log(M4_LOG_WARN, M4_T("graph-host: force-quantum not applied (see the warning above)",
"graph-host: force-quantum не применён (см. предупреждение выше)"));
	}

	sd_notify(0, "READY=1");
	m4_log(M4_LOG_INFO, M4_T("graph-host: started",
"graph-host: запущен"));

	pw_main_loop_run(g.loop);

	if (g.settings_metadata != NULL) {
		spa_hook_remove(&g.metadata_listener);
		pw_proxy_destroy((struct pw_proxy *)g.settings_metadata);
	}
	if (g.registry != NULL) {
		spa_hook_remove(&g.registry_listener);
		pw_proxy_destroy((struct pw_proxy *)g.registry);
	}
	if (g.core != NULL) {
		spa_hook_remove(&g.core_listener);
		pw_core_disconnect(g.core);
	}
	if (g.combine_module != NULL)
		pw_impl_module_destroy(g.combine_module);
	if (g.mic_module != NULL)
		pw_impl_module_destroy(g.mic_module);
	pw_context_destroy(g.context);
	pw_main_loop_destroy(g.loop);
	m4_log(M4_LOG_INFO, M4_T("graph-host: exited (code %d)",
"graph-host: завершён (код %d)"), g.exit_code);
	return g.exit_code;

fail_load:
	if (g.combine_module != NULL)
		pw_impl_module_destroy(g.combine_module);
	pw_context_destroy(g.context);
	pw_main_loop_destroy(g.loop);
	return 3;
}
