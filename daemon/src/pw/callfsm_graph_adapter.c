/* pw/callfsm_graph_adapter.c -- see callfsm_graph_adapter.h. */
#include "callfsm_graph_adapter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../core/state.h"
#include "../metrics/stats.h"
#include "../util/log.h"
#include "../../../common/m4-i18n.h"
#include "graph_wait.h"
#include "repair_a2dp.h"

struct m4_callfsm_graph_adapter {
	sd_event *event;
	const struct graph_ops *ops;
	void *handle;
	char runtime_dir[512];
	char stats_path[512];
	int confirm_timeout_ms;

	/* "repair-a2dp" wiring -- repair_bt_ops == NULL
	 * means repair is disabled (m4_callfsm_graph_adapter_repair_config
	 * was NULL at construction). */
	const struct m4_bt_ops *repair_bt_ops;
	char repair_mac_address[32];
	char repair_bt_adapter[16];
	int repair_connect_timeout_ms;
	int repair_enum_wait_ms;

	/* "mic-on-enter" task -- see callfsm_graph_adapter.h's struct
	 * m4_callfsm_graph_adapter_mic_config doc comment. */
	m4_mic_on_enter_t mic_on_enter;
	char mic_state_dir[512];
	float mic_mute_threshold;
};

/* Same value/rationale as daemon.c's M4_DAEMON_DEFAULT_MIC_ROUTE_QUIET_EPS
 * (that macro's own comment has the full confirmed-by-code AT+VGM
 * quantisation rationale) -- kept as this file's own compiled fallback so a
 * NULL/<=0 mic_cfg (every pre-existing caller) behaves byte-for-byte the
 * same, without this file depending on daemon.h at all. */
#define M4_CALLFSM_MIC_QUIET_EPS 0.02f

struct m4_callfsm_graph_adapter *m4_callfsm_graph_adapter_new(
	sd_event *event, const struct graph_ops *ops, void *graph_handle,
	const char *runtime_dir, const char *stats_path, int confirm_timeout_ms,
	const struct m4_callfsm_graph_adapter_repair_config *repair_cfg,
	const struct m4_callfsm_graph_adapter_mic_config *mic_cfg)
{
	struct m4_callfsm_graph_adapter *a = calloc(1, sizeof(*a));

	if (a == NULL)
		return NULL;
	a->event = event;
	a->ops = ops;
	a->handle = graph_handle;
	if (runtime_dir != NULL)
		snprintf(a->runtime_dir, sizeof(a->runtime_dir), "%s", runtime_dir);
	if (stats_path != NULL)
		snprintf(a->stats_path, sizeof(a->stats_path), "%s", stats_path);
	a->confirm_timeout_ms = confirm_timeout_ms > 0 ? confirm_timeout_ms : 5000;

	if (repair_cfg != NULL && repair_cfg->bt_ops != NULL) {
		a->repair_bt_ops = repair_cfg->bt_ops;
		if (repair_cfg->mac_address != NULL)
			snprintf(a->repair_mac_address, sizeof(a->repair_mac_address), "%s",
				 repair_cfg->mac_address);
		if (repair_cfg->bt_adapter != NULL)
			snprintf(a->repair_bt_adapter, sizeof(a->repair_bt_adapter), "%s",
				 repair_cfg->bt_adapter);
		a->repair_connect_timeout_ms = repair_cfg->connect_timeout_ms;
		a->repair_enum_wait_ms = repair_cfg->enum_wait_ms;
	}

	a->mic_on_enter = M4_MIC_ON_ENTER_ON; /* NULL mic_cfg -> pre-existing behaviour, unchanged */
	a->mic_mute_threshold = M4_CALLFSM_MIC_QUIET_EPS;
	if (mic_cfg != NULL) {
		a->mic_on_enter = mic_cfg->mode;
		if (mic_cfg->state_dir != NULL)
			snprintf(a->mic_state_dir, sizeof(a->mic_state_dir), "%s", mic_cfg->state_dir);
		if (mic_cfg->mute_threshold > 0)
			a->mic_mute_threshold = mic_cfg->mute_threshold;
	}
	return a;
}

void m4_callfsm_graph_adapter_free(struct m4_callfsm_graph_adapter *a)
{
	free(a);
}

void m4_callfsm_graph_adapter_set_mic_on_enter(struct m4_callfsm_graph_adapter *a, m4_mic_on_enter_t mode)
{
	if (a == NULL)
		return;
	a->mic_on_enter = mode;
}

static int adapter_card_present(void *ctx, char *card_buf, size_t card_buf_len)
{
	struct m4_callfsm_graph_adapter *a = ctx;
	struct m4_graph_state st;

	if (a->ops->get_state(a->handle, &st) != M4_GRAPH_OK || !st.m4_present)
		return 0;
	/* Opaque token -- this project has exactly one card (the M4); nothing
	 * downstream parses this string, see this file's header comment. */
	snprintf(card_buf, card_buf_len, "m4");
	return 1;
}

static int adapter_get_active_profile(void *ctx, const char *card, char *buf, size_t buflen)
{
	struct m4_callfsm_graph_adapter *a = ctx;
	struct m4_graph_state st;

	(void)card;
	if (a->ops->get_state(a->handle, &st) != M4_GRAPH_OK || !st.have_profile) {
		if (buflen > 0)
			buf[0] = '\0';
		return 0;
	}
	snprintf(buf, buflen, "%s", st.profile_name);
	return 0;
}

/* Item 3: every profile switch, with its actual result and
 * confirmation time, is logged (info). This is the ONE place in the whole daemon that
 * actually issues+waits for a profile switch (core/callfsm.c's
 * set_profile_confirm() calls always funnel through here), so it is the
 * natural single place to log every attempt -- avoids scattering
 * ad hoc logging across every callfsm.c call site. */
static int adapter_set_profile_confirm_inner(struct m4_callfsm_graph_adapter *a, const char *profile,
					      uint64_t *elapsed_usec_out)
{
	uint64_t start, now, deadline;

	start = m4_graph_wait_now_usec();
	if (a->ops->set_profile(a->handle, profile, true) != M4_GRAPH_OK) {
		*elapsed_usec_out = 0;
		return 0;
	}

	now = start;
	deadline = start + (uint64_t)a->confirm_timeout_ms * 1000;

	for (;;) {
		struct m4_graph_state st;

		if (a->ops->get_state(a->handle, &st) == M4_GRAPH_OK && st.have_profile &&
		    strcmp(st.profile_name, profile) == 0) {
			now = m4_graph_wait_now_usec();
			*elapsed_usec_out = now - start;
			return 1;
		}

		now = m4_graph_wait_now_usec();
		if (now >= deadline) {
			*elapsed_usec_out = now - start;
			return 0;
		}

		/* One bounded wait step; returns as soon as something was
		 * processed (maybe not our update -- the loop re-checks
		 * get_state() either way) or the slice/timeout elapsed.
		 * a nested sd_event_run() returns -EBUSY
		 * when this runs inside a daemon callback (always, live) --
		 * pw/graph_wait.h pumps the graph's own PipeWire loop instead. */
		if (m4_graph_wait_step(a->ops, a->handle, a->event, deadline - now) < 0) {
			now = m4_graph_wait_now_usec();
			*elapsed_usec_out = now - start;
			return 0;
		}
	}
}

static int adapter_set_profile_confirm(void *ctx, const char *card, const char *profile)
{
	struct m4_callfsm_graph_adapter *a = ctx;
	uint64_t elapsed_usec = 0;
	int confirmed;

	(void)card;
	confirmed = adapter_set_profile_confirm_inner(a, profile, &elapsed_usec);
	m4_log(confirmed ? M4_LOG_INFO : M4_LOG_WARN,
	       M4_T("callfsm: profile switch -> \"%s\": %s (%.3f s, limit %d ms)",
"callfsm: смена профиля -> \"%s\": %s (%.3f с, лимит %d мс)"), profile,
	       confirmed ? M4_T("confirmed",
"подтверждена") : M4_T("NOT confirmed (timeout)",
"НЕ подтверждена (таймаут)"), (double)elapsed_usec / 1e6,
	       a->confirm_timeout_ms);
	return confirmed;
}

static int adapter_state_write(void *ctx, const char *saved_profile, const char *reason)
{
	struct m4_callfsm_graph_adapter *a = ctx;

	return m4_state_write(a->runtime_dir, saved_profile, reason);
}

static int adapter_state_present(void *ctx)
{
	struct m4_callfsm_graph_adapter *a = ctx;

	return m4_state_present(a->runtime_dir) ? 1 : 0;
}

static int adapter_state_read(void *ctx, char *out_profile, size_t out_profile_len,
			       char *out_reason, size_t out_reason_len)
{
	struct m4_callfsm_graph_adapter *a = ctx;

	return m4_state_read(a->runtime_dir, out_profile, out_profile_len, out_reason, out_reason_len);
}

static void adapter_state_remove(void *ctx)
{
	struct m4_callfsm_graph_adapter *a = ctx;

	m4_state_remove(a->runtime_dir);
}

static int adapter_helper_alive(void *ctx)
{
	struct m4_callfsm_graph_adapter *a = ctx;
	struct m4_stats st;

	if (a->stats_path[0] == '\0')
		return 0;
	if (m4_stats_load(a->stats_path, &st) != 0)
		return 0;
	return st.alive ? 1 : 0;
}

/* callfsm_ops.h's repair_profile_if_missing -- see its doc comment for the
 * full contract this must uphold ("ONLY if absent from EnumProfile",
 * "leave the profile confirmed active before returning 1"). This adapter
 * only knows how to repair ONE profile (M4_REPAIR_A2DP_PROFILE, "a2dp-sink"
 * -- the only thing pw/repair_a2dp.c implements), matching every current
 * caller in core/callfsm.c (both of its confirm_or_repair() call sites
 * target M4_CALLFSM_PROFILE_MUSIC_DEFAULT, which is the exact same string,
 * see callfsm.h -- this file intentionally does not #include callfsm.h to
 * keep pw/ decoupled from core/callfsm.c specifically, only from the
 * generic callfsm_ops.h interface). */
static int adapter_repair_profile_if_missing(void *ctx, const char *card, const char *profile)
{
	struct m4_callfsm_graph_adapter *a = ctx;
	struct m4_graph_state st;
	uint32_t idx;
	struct m4_repair_a2dp_config cfg = {0};
	struct m4_repair_a2dp_result r;

	(void)card;
	if (a->repair_bt_ops == NULL || strcmp(profile, M4_REPAIR_A2DP_PROFILE) != 0)
		return 0;
	if (a->ops->get_state(a->handle, &st) != M4_GRAPH_OK)
		return 0;
	if (m4_graph_find_profile_index(&st, profile, &idx))
		return 0; /* not actually missing -- nothing to repair (see doc comment) */

	m4_log(M4_LOG_WARN,
	       M4_T("callfsm: \"%s\" is missing from EnumProfile -- trying to restore it via repair-a2dp "
	       "(ConnectProfile)",
"callfsm: \"%s\" отсутствует в EnumProfile -- пробую восстановить через repair-a2dp "
	       "(ConnectProfile)"),
	       profile);

	cfg.graph_ops = a->ops;
	cfg.graph_handle = a->handle;
	cfg.bt_ops = a->repair_bt_ops;
	cfg.event = a->event;
	cfg.mac_address = a->repair_mac_address;
	cfg.bt_adapter = a->repair_bt_adapter[0] != '\0' ? a->repair_bt_adapter : NULL;
	cfg.connect_timeout_ms = a->repair_connect_timeout_ms;
	cfg.enum_wait_ms = a->repair_enum_wait_ms;
	cfg.confirm_timeout_ms = a->confirm_timeout_ms;

	r = m4_repair_a2dp_run(&cfg);
	m4_log(r.status == M4_REPAIR_A2DP_OK ? M4_LOG_INFO : M4_LOG_WARN, "callfsm: repair-a2dp -> %s",
	       r.message);
	return r.status == M4_REPAIR_A2DP_OK ? 1 : 0;
}

/* mic-on-enter: what M4_MIC_ON_ENTER_REMEMBER's target actually is at
 * this particular HFP entry -- reads the mic state persisted at the LAST
 * HFP exit (core/state.h's m4_mic_state_read(), tried `a->runtime_dir` then
 * `a->mic_state_dir`). No prior record at all (first-ever entry in
 * remember mode, or the persisted files were never written/were cleared,
 * e.g. a reboot with no `[paths] state-dir` configured) -> false (mic ON) --
 * `[call] mic-on-enter`'s own doc comment: the same safe default as
 * the key itself (the key's own default value is "on"). */
static bool mic_on_enter_remember_target_muted(struct m4_callfsm_graph_adapter *a)
{
	bool muted = false;

	if (m4_mic_state_read(a->runtime_dir, a->mic_state_dir, &muted) == 0)
		return muted;
	return false;
}

/* Bug 2 (see callfsm_ops.h's fix_mic_volume_if_needed doc
 * comment) + "mic-on-enter" task (`[call] mic-on-enter`, common/
 * m4-conf-keys.def): reads the M4 mic Route from the cached graph state,
 * decides the TARGET mute state from `a->mic_on_enter` (on -> always
 * unmuted, Bug 2's original behaviour, byte-for-byte; off -> always muted;
 * remember -> mic_on_enter_remember_target_muted() above), and if the
 * Route is not already there, issues ONE set_mic_volume() + a bounded
 * confirm poll -- same overall shape as adapter_set_profile_confirm_inner()
 * above (nested sd_event_run() on the SAME event loop the daemon already
 * pumps), just against a different pair of fields (mic_volume/mic_mute
 * instead of profile_name). Reuses `confirm_timeout_ms` rather than adding
 * a fifth timeout knob -- this is a local param write (no BlueZ AT-command
 * round-trip expected to be anywhere near as slow as an HFP codec
 * negotiation), so the existing budget is generous, not tight. */
static int adapter_fix_mic_volume_if_needed(void *ctx, const char *card)
{
	struct m4_callfsm_graph_adapter *a = ctx;
	struct m4_graph_state st;
	uint64_t start, now, deadline;
	bool want_muted;
	float target_volume;
	const char *target_label;

	(void)card;

	if (a->ops->set_mic_volume == NULL || a->ops->get_state(a->handle, &st) != M4_GRAPH_OK)
		return 0;
	if (!st.have_mic_route) {
		/* Nothing observed right now -- see graph_pw.c's own (corrected,
		 * "M4 button = mute (VGM)" task) comment on graph_start_refresh()
		 * for when Route gets (re-)pushed. Not an error: callfsm_ops.h's
		 * doc comment covers this exact case as a normal 0, distinct from
		 * "checked, already fine" only in the log line below. */
		m4_log(M4_LOG_INFO,
		       M4_T("callfsm: M4 microphone state not checked -- the Route is not currently observed",
"callfsm: состояние микрофона M4 не проверено -- маршрут (Route) сейчас не наблюдается"));
		return 0;
	}

	switch (a->mic_on_enter) {
	case M4_MIC_ON_ENTER_OFF:
		want_muted = true;
		break;
	case M4_MIC_ON_ENTER_REMEMBER:
		want_muted = mic_on_enter_remember_target_muted(a);
		break;
	case M4_MIC_ON_ENTER_ON:
	default:
		want_muted = false;
		break;
	}
	target_volume = want_muted ? 0.0f : 1.0f;
	target_label = want_muted ? M4_T("off",
"выключен") : M4_T("on",
"включён");

	/* M4 button = mute (VGM): volume-only, like daemon.c's
	 * M4_MIC_ROUTE_QUIET_EPS/daemon_check_mic_route_transition() (their own
	 * comments have the full confirmed-by-code rationale) -- `st.mic_mute`
	 * is a PipeWire-local soft flag the M4 itself never sets or reads, so
	 * it must not gate this decision (a stale mute=true from an earlier
	 * write, e.g. an old `momentum4 mic off`, must not force a rewrite here
	 * once the ONLY thing that matters -- volume -- already matches the
	 * target; daemon.c's own daemon_force_mic_unmute() already clears both
	 * together at every call-off in this project's own write paths anyway,
	 * so in practice this rarely differs, but a third party -- e.g.
	 * `pactl set-source-mute` -- could still leave mute=true here). */
	if (want_muted ? (st.mic_volume <= a->mic_mute_threshold)
			: (st.mic_volume >= 1.0f - a->mic_mute_threshold))
		return 0; /* already at target -- no write, matches set_profile()'s
			   * own "do nothing if nothing to do" style */

	m4_log(M4_LOG_WARN, M4_T("callfsm: M4 microphone %.0f%%%s -- setting it to the \"mic-on-enter=%s\" state (%s)",
"callfsm: микрофон M4 %.0f%%%s -- привожу к состоянию \"mic-on-enter=%s\" (%s)"),
	       (double)(st.mic_volume * 100.0f), st.mic_mute ? " (mute)" : "", m4_mic_on_enter_name(a->mic_on_enter),
	       target_label);

	if (a->ops->set_mic_volume(a->handle, target_volume, want_muted, true) != M4_GRAPH_OK)
		return -1;

	start = m4_graph_wait_now_usec();
	deadline = start + (uint64_t)a->confirm_timeout_ms * 1000;
	now = start;

	for (;;) {
		if (a->ops->get_state(a->handle, &st) == M4_GRAPH_OK && st.have_mic_route &&
		    (want_muted ? (st.mic_volume <= a->mic_mute_threshold)
				: (st.mic_volume >= 1.0f - a->mic_mute_threshold))) {
			m4_log(M4_LOG_INFO, M4_T("callfsm: M4 microphone confirmed (%s)",
"callfsm: микрофон M4 подтверждён (%s)"), target_label);
			return 1;
		}
		now = m4_graph_wait_now_usec();
		if (now >= deadline) {
			m4_log(M4_LOG_WARN, M4_T("callfsm: M4 microphone NOT confirmed (timeout %d ms)",
"callfsm: микрофон M4 НЕ подтверждён (таймаут %d мс)"),
			       a->confirm_timeout_ms);
			return -1;
		}
		/* Same nested sd_event_run() technique as
		 * adapter_set_profile_confirm_inner() above -- see this file's
		 * header comment and daemon.c's action_in_progress guard, which
		 * is what keeps this bounded. */
		if (m4_graph_wait_step(a->ops, a->handle, a->event, deadline - now) < 0)
			return -1;
	}
}

m4_callfsm_ops_t m4_callfsm_graph_adapter_ops(struct m4_callfsm_graph_adapter *a)
{
	m4_callfsm_ops_t ops = {
		.ctx = a,
		.card_present = adapter_card_present,
		.get_active_profile = adapter_get_active_profile,
		.set_profile_confirm = adapter_set_profile_confirm,
		.state_write = adapter_state_write,
		.state_present = adapter_state_present,
		.state_read = adapter_state_read,
		.state_remove = adapter_state_remove,
		.repair_profile_if_missing = adapter_repair_profile_if_missing,
		.helper_alive = adapter_helper_alive,
		.fix_mic_volume_if_needed = adapter_fix_mic_volume_if_needed,
	};
	return ops;
}
