/* pw/repair_a2dp.c -- see repair_a2dp.h. */
#include "repair_a2dp.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "../util/log.h"
#include "../../../common/m4-i18n.h"
#include "graph_wait.h"

static double elapsed_since(const struct timespec *t0)
{
	struct timespec t1;

	clock_gettime(CLOCK_MONOTONIC, &t1);
	return (double)(t1.tv_sec - t0->tv_sec) + (double)(t1.tv_nsec - t0->tv_nsec) / 1e9;
}

/* Bounded poll of graph_ops->get_state(), pumping `event` reentrantly until
 * `pred` returns true or `deadline_usec` (CLOCK_MONOTONIC) passes -- the
 * exact same technique as pw/callfsm_graph_adapter.c's
 * adapter_set_profile_confirm() (see that file's own comment for the
 * bounded-nesting caveat this shares: daemon.c's action_in_progress guard
 * is what keeps this from nesting unboundedly when called from the
 * daemon's ctl.sock path). */
typedef bool (*m4_repair_poll_pred)(const struct m4_graph_state *st, const void *pred_ctx);

static bool poll_until(sd_event *event, const struct graph_ops *ops, void *handle,
			uint64_t deadline_usec, m4_repair_poll_pred pred, const void *pred_ctx)
{
	for (;;) {
		struct m4_graph_state st;
		uint64_t now;

		if (ops->get_state(handle, &st) == M4_GRAPH_OK && pred(&st, pred_ctx))
			return true;
		/* pw/graph_wait.h: clock_gettime, not sd_event_now() (frozen
		 * while dispatching), and no nested sd_event_run() when called
		 * from inside a daemon callback (-EBUSY). */
		now = m4_graph_wait_now_usec();
		if (now >= deadline_usec)
			return false;
		if (m4_graph_wait_step(ops, handle, event, deadline_usec - now) < 0)
			return false;
	}
}

static bool pred_a2dp_sink_enumerable(const struct m4_graph_state *st, const void *pred_ctx)
{
	uint32_t idx;

	(void)pred_ctx;
	return m4_graph_find_profile_index(st, M4_REPAIR_A2DP_PROFILE, &idx);
}

static bool pred_profile_active(const struct m4_graph_state *st, const void *pred_ctx)
{
	const char *want = pred_ctx;

	return st->have_profile && strcmp(st->profile_name, want) == 0;
}

struct m4_repair_a2dp_result m4_repair_a2dp_run(const struct m4_repair_a2dp_config *cfg)
{
	struct m4_repair_a2dp_result r = {0};
	struct m4_graph_state st;
	struct timespec t0;
	char bt_err[192] = {0};
	int connect_timeout_ms, enum_wait_ms, confirm_timeout_ms;
	uint64_t now, deadline;
	int bt_rc;

	if (cfg == NULL || cfg->graph_ops == NULL || cfg->graph_handle == NULL ||
	    cfg->bt_ops == NULL || cfg->event == NULL || cfg->mac_address == NULL ||
	    cfg->mac_address[0] == '\0') {
		r.status = M4_REPAIR_A2DP_ERR_BAD_CONFIG;
		snprintf(r.message, sizeof(r.message), M4_T("repair-a2dp: internal error (incomplete configuration)",
"repair-a2dp: внутренняя ошибка (неполная конфигурация)"));
		return r;
	}
	connect_timeout_ms = cfg->connect_timeout_ms > 0 ? cfg->connect_timeout_ms
							  : M4_REPAIR_A2DP_DEFAULT_CONNECT_TIMEOUT_MS;
	enum_wait_ms = cfg->enum_wait_ms > 0 ? cfg->enum_wait_ms : M4_REPAIR_A2DP_DEFAULT_ENUM_WAIT_MS;
	confirm_timeout_ms = cfg->confirm_timeout_ms > 0 ? cfg->confirm_timeout_ms
							  : M4_REPAIR_A2DP_DEFAULT_CONFIRM_TIMEOUT_MS;

	if (cfg->graph_ops->get_state(cfg->graph_handle, &st) != M4_GRAPH_OK || !st.m4_present) {
		r.status = M4_REPAIR_A2DP_ERR_NOT_PRESENT;
		snprintf(r.message, sizeof(r.message), M4_T("Momentum 4 not found in PipeWire -- nothing to repair",
"Momentum 4 не найдены в PipeWire -- нечего чинить"));
		m4_log(M4_LOG_WARN, "repair-a2dp: %s", r.message);
		return r;
	}

	m4_log(M4_LOG_INFO, M4_T("repair-a2dp: M4 present, calling ConnectProfile(A2DP Sink) for %s",
"repair-a2dp: M4 присутствует, вызываю ConnectProfile(A2DP Sink) для %s"),
	       cfg->mac_address);
	bt_rc = cfg->bt_ops->connect_profile(cfg->bt_ops->ctx, cfg->mac_address,
					      cfg->bt_adapter != NULL ? cfg->bt_adapter : "hci0",
					      M4_BT_UUID_A2DP_SINK, connect_timeout_ms, bt_err,
					      sizeof(bt_err));
	if (bt_rc != 0) {
		r.connect_profile_failed = 1;
		m4_log(M4_LOG_WARN,
		       M4_T("repair-a2dp: ConnectProfile returned an error (%s) -- still waiting for EnumProfile "
		       "(it may have recovered independently)",
"repair-a2dp: ConnectProfile вернул ошибку (%s) -- всё равно жду EnumProfile "
		       "(могло восстановиться независимо)"),
		       bt_err);
	} else {
		m4_log(M4_LOG_INFO, M4_T("repair-a2dp: ConnectProfile completed (%s)",
"repair-a2dp: ConnectProfile выполнен (%s)"), bt_err);
	}

	clock_gettime(CLOCK_MONOTONIC, &t0);
	now = m4_graph_wait_now_usec();
	deadline = now + (uint64_t)enum_wait_ms * 1000;

	if (!poll_until(cfg->event, cfg->graph_ops, cfg->graph_handle, deadline,
			pred_a2dp_sink_enumerable, NULL)) {
		r.enum_wait_elapsed_s = elapsed_since(&t0);
		r.status = M4_REPAIR_A2DP_ERR_ENUM_TIMEOUT;
		snprintf(r.message, sizeof(r.message),
			 M4_T("a2dp-sink did not appear in EnumProfile within %d ms%s%s",
"a2dp-sink не появился в EnumProfile за %d мс%s%s"), enum_wait_ms,
			 bt_rc != 0 ? "; ConnectProfile: " : "", bt_rc != 0 ? bt_err : "");
		m4_log(M4_LOG_WARN, "repair-a2dp: %s", r.message);
		return r;
	}
	r.enum_wait_elapsed_s = elapsed_since(&t0);
	m4_log(M4_LOG_INFO, M4_T("repair-a2dp: a2dp-sink appeared in EnumProfile after %.3f s",
"repair-a2dp: a2dp-sink появился в EnumProfile за %.3f с"),
	       r.enum_wait_elapsed_s);

	if (cfg->graph_ops->set_profile(cfg->graph_handle, M4_REPAIR_A2DP_PROFILE, true) != M4_GRAPH_OK) {
		r.status = M4_REPAIR_A2DP_ERR_SET_PROFILE;
		snprintf(r.message, sizeof(r.message), M4_T("repair-a2dp: set_profile(a2dp-sink) failed to issue",
"repair-a2dp: set_profile(a2dp-sink) не удалось выпустить"));
		m4_log(M4_LOG_WARN, "%s", r.message);
		return r;
	}

	clock_gettime(CLOCK_MONOTONIC, &t0);
	now = m4_graph_wait_now_usec();
	deadline = now + (uint64_t)confirm_timeout_ms * 1000;
	if (!poll_until(cfg->event, cfg->graph_ops, cfg->graph_handle, deadline, pred_profile_active,
			M4_REPAIR_A2DP_PROFILE)) {
		r.confirm_elapsed_s = elapsed_since(&t0);
		r.status = M4_REPAIR_A2DP_ERR_SET_PROFILE;
		snprintf(r.message, sizeof(r.message),
			 M4_T("a2dp-sink profile requested, but not confirmed within %d ms",
"профиль a2dp-sink запрошен, но не подтвердился за %d мс"), confirm_timeout_ms);
		m4_log(M4_LOG_WARN, "repair-a2dp: %s", r.message);
		return r;
	}
	r.confirm_elapsed_s = elapsed_since(&t0);

	r.status = M4_REPAIR_A2DP_OK;
	snprintf(r.message, sizeof(r.message),
		 M4_T("a2dp-sink restored (EnumProfile: %.3f s, profile confirmation: %.3f s)",
"a2dp-sink восстановлен (EnumProfile: %.3f с, подтверждение профиля: %.3f с)"),
		 r.enum_wait_elapsed_s, r.confirm_elapsed_s);
	m4_log(M4_LOG_INFO, "repair-a2dp: %s", r.message);
	return r;
}

struct m4_repair_a2dp_result m4_repair_a2dp_run_standalone(
	const struct graph_ops *graph_ops, const struct m4_bt_ops *bt_ops, const char *mac_address,
	const char *bt_adapter, int open_settle_ms, int connect_timeout_ms, int enum_wait_ms,
	int confirm_timeout_ms)
{
	struct m4_repair_a2dp_result r = {0};
	sd_event *event = NULL;
	void *handle = NULL;
	struct m4_repair_a2dp_config cfg = {0};

	if (graph_ops == NULL || bt_ops == NULL || mac_address == NULL) {
		r.status = M4_REPAIR_A2DP_ERR_BAD_CONFIG;
		snprintf(r.message, sizeof(r.message), M4_T("repair-a2dp: internal error (incomplete configuration)",
"repair-a2dp: внутренняя ошибка (неполная конфигурация)"));
		return r;
	}
	if (open_settle_ms <= 0)
		open_settle_ms = 3000;

	if (sd_event_new(&event) < 0) {
		r.status = M4_REPAIR_A2DP_ERR_BAD_CONFIG;
		snprintf(r.message, sizeof(r.message), M4_T("repair-a2dp: sd_event_new() failed",
"repair-a2dp: sd_event_new() не удался"));
		return r;
	}

	handle = graph_ops->open(event, mac_address, NULL, NULL);
	if (handle == NULL) {
		r.status = M4_REPAIR_A2DP_ERR_NOT_PRESENT;
		snprintf(r.message, sizeof(r.message), M4_T("repair-a2dp: PipeWire unavailable",
"repair-a2dp: PipeWire недоступен"));
		m4_log(M4_LOG_WARN, "%s", r.message);
		sd_event_unref(event);
		return r;
	}

	/* Give the fresh connection a moment to receive its initial registry
	 * burst before m4_repair_a2dp_run() checks m4_present -- graph_pw.c's
	 * graph_on_registry_global() fires on_state_changed() as soon as the
	 * M4 Device global itself is seen, well before EnumProfile/Profile
	 * settle (which m4_repair_a2dp_run() below waits for separately, on
	 * its own, longer budget) -- this is only "has the very first
	 * registry burst arrived at all", not a full settle. */
	{
		uint64_t now, deadline;

		if (sd_event_now(event, CLOCK_MONOTONIC, &now) == 0) {
			deadline = now + (uint64_t)open_settle_ms * 1000;
			for (;;) {
				struct m4_graph_state st;

				if (graph_ops->get_state(handle, &st) == M4_GRAPH_OK && st.m4_present)
					break;
				if (sd_event_now(event, CLOCK_MONOTONIC, &now) < 0 || now >= deadline)
					break;
				if (sd_event_run(event, deadline - now) < 0)
					break;
			}
		}
	}

	cfg.graph_ops = graph_ops;
	cfg.graph_handle = handle;
	cfg.bt_ops = bt_ops;
	cfg.event = event;
	cfg.mac_address = mac_address;
	cfg.bt_adapter = bt_adapter;
	cfg.connect_timeout_ms = connect_timeout_ms;
	cfg.enum_wait_ms = enum_wait_ms;
	cfg.confirm_timeout_ms = confirm_timeout_ms;

	r = m4_repair_a2dp_run(&cfg);

	graph_ops->close(handle);
	sd_event_unref(event);
	return r;
}
