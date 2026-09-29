/* core/automode.c -- see automode.h.
 * The differential test (tests/test_automode_diff.c) against a frozen set
 * of reference traces is the authoritative behavioural check; the comments
 * below are for readability.
 *
 * Why tick()'s two branches never both produce an action in the same call
 * (see automode.h M4_AUTOMODE_MAX_ACTIONS comment): _pending_on_at and
 * _pending_off_at are never both non-"unset" at once, because every place
 * that sets one of them (on_mic_activity_changed, note_external_call_state,
 * resume) unconditionally clears the other in the same call. tick() itself
 * only clears, never sets, either field. So at most one of the two `if`
 * blocks in tick() can have a live deadline to act on, and the freeze path
 * returns immediately without falling into the second `if`.
 */
#include "automode.h"

#include "../../../common/m4-i18n.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -- sliding-window rate limiter ------------------------------------------ */

typedef struct {
	int max_events;
	double window_s;
	double *events;
	size_t count;
	size_t capacity;
} m4_ratelimiter_t;

static void ratelimiter_init(m4_ratelimiter_t *rl, int max_events, double window_s)
{
	rl->max_events = max_events;
	rl->window_s = window_s;
	rl->events = NULL;
	rl->count = 0;
	rl->capacity = 0;
}

static void ratelimiter_destroy(m4_ratelimiter_t *rl)
{
	free(rl->events);
	rl->events = NULL;
	rl->count = 0;
	rl->capacity = 0;
}

static void ratelimiter_reset(m4_ratelimiter_t *rl, int max_events, double window_s)
{
	ratelimiter_destroy(rl);
	ratelimiter_init(rl, max_events, window_s);
}

/* record_and_check(now) -- appends `now`, trims everything older
 * than now-window_s (list comprehension there, in-place compaction here),
 * returns whether the just-recorded event pushed the count over
 * max_events. abort() on OOM is acceptable here: this struct never holds
 * more than a handful of doubles in any realistic rate_window_s/event rate
 * (a control-plane debounce counter, not user data), and there is no sane
 * fallback for "can't grow a several-KB array" in a daemon whose whole job
 * is telling the truth about how many times it switched profiles. */
static int ratelimiter_record_and_check(m4_ratelimiter_t *rl, double now)
{
	if (rl->count == rl->capacity) {
		size_t new_cap = rl->capacity ? rl->capacity * 2 : 8;
		double *grown = realloc(rl->events, new_cap * sizeof(*grown));
		if (grown == NULL)
			abort();
		rl->events = grown;
		rl->capacity = new_cap;
	}
	rl->events[rl->count++] = now;

	double cutoff = now - rl->window_s;
	size_t kept = 0;
	for (size_t i = 0; i < rl->count; i++) {
		if (rl->events[i] >= cutoff)
			rl->events[kept++] = rl->events[i];
	}
	rl->count = kept;
	return (int)rl->count > rl->max_events;
}

static size_t ratelimiter_snapshot_count(const m4_ratelimiter_t *rl, double now)
{
	double cutoff = now - rl->window_s;
	size_t n = 0;
	for (size_t i = 0; i < rl->count; i++) {
		if (rl->events[i] >= cutoff)
			n++;
	}
	return n;
}

/* -- AutoModeCore ---------------------------------------------------------- */

struct m4_automode_core {
	m4_automode_config_t cfg;
	m4_ratelimiter_t limiter;

	int frozen;
	char freeze_reason[160]; /* log-only, not part of any comparable state */

	int mic_active;
	int hold_active; /* long-lived-holder fix, see automode.h */

	int has_pending_on;
	double pending_on_at;
	int has_pending_off;
	double pending_off_at;

	int call_active;
	char call_active_reason[M4_AUTOMODE_REASON_LEN]; /* "none"/"auto"/"manual" */
};

const char *m4_automode_action_name(m4_automode_action_t action)
{
	switch (action) {
	case M4_AUTOMODE_ACTION_CALL_ON: return "CALL_ON";
	case M4_AUTOMODE_ACTION_CALL_OFF: return "CALL_OFF";
	case M4_AUTOMODE_ACTION_ANNOUNCE_FREEZE: return "ANNOUNCE_FREEZE";
	case M4_AUTOMODE_ACTION_NONE:
	default:
		return "NONE";
	}
}

void m4_automode_config_default(m4_automode_config_t *cfg)
{
	cfg->on_debounce_s = 0.3;
	cfg->off_debounce_s = 2.5;
	cfg->max_switches = 6;
	cfg->rate_window_s = 300.0;
}

static void set_reason(char *dst, size_t dst_len, const char *src)
{
	if (src == NULL) {
		dst[0] = '\0';
		return;
	}
	snprintf(dst, dst_len, "%s", src);
}

m4_automode_core_t *m4_automode_new(const m4_automode_config_t *cfg)
{
	m4_automode_core_t *core = calloc(1, sizeof(*core));
	if (core == NULL)
		return NULL;

	if (cfg != NULL)
		core->cfg = *cfg;
	else
		m4_automode_config_default(&core->cfg);

	ratelimiter_init(&core->limiter, core->cfg.max_switches, core->cfg.rate_window_s);
	set_reason(core->call_active_reason, sizeof(core->call_active_reason), "none");
	return core;
}

void m4_automode_free(m4_automode_core_t *core)
{
	if (core == NULL)
		return;
	ratelimiter_destroy(&core->limiter);
	free(core);
}

/* Startup reconciliation. */
m4_automode_decision_t m4_automode_start(m4_automode_core_t *core, double now, const char *current_reason)
{
	(void)now;
	m4_automode_decision_t d = {0};
	set_reason(core->call_active_reason, sizeof(core->call_active_reason), current_reason);
	core->call_active = (current_reason != NULL && strcmp(current_reason, "none") != 0);
	return d;
}

/* External call-state notification. */
m4_automode_decision_t m4_automode_note_external_call_state(m4_automode_core_t *core, const char *reason, double now)
{
	(void)now;
	m4_automode_decision_t d = {0};
	int changed = (strcmp(core->call_active_reason, reason != NULL ? reason : "") != 0);

	set_reason(core->call_active_reason, sizeof(core->call_active_reason), reason);
	core->call_active = (reason != NULL && strcmp(reason, "none") != 0);

	if (changed) {
		core->has_pending_on = 0;
		core->has_pending_off = 0;
	}
	return d;
}

/* Mic-activity edge handling. */
m4_automode_decision_t m4_automode_on_mic_activity_changed(m4_automode_core_t *core, int active, double now)
{
	m4_automode_decision_t d = {0};
	active = active ? 1 : 0;

	if (core->frozen)
		return d;
	if (active == core->mic_active)
		return d; /* idempotent, no change */

	core->mic_active = active;

	if (active) {
		core->has_pending_off = 0;
		if (core->call_active)
			return d; /* already on (manual or auto) -- nothing to do */
		core->has_pending_on = 1;
		core->pending_on_at = now + core->cfg.on_debounce_s;
		return d;
	}

	core->has_pending_on = 0;
	if (!core->call_active || strcmp(core->call_active_reason, "manual") == 0)
		return d; /* I4: never auto-off a manual call */
	core->has_pending_off = 1;
	core->pending_off_at = now + core->cfg.off_debounce_s;
	return d;
}

/* Bug 1 fix (see core/automode.h's own doc comment on this function):
 * only ever touches an already
 * active reason=="auto" call's exit debounce -- never arms entry, never
 * touches a manual call. */
m4_automode_decision_t m4_automode_on_mic_hold_changed(m4_automode_core_t *core, int hold_active, double now)
{
	m4_automode_decision_t d = {0};
	hold_active = hold_active ? 1 : 0;

	if (core->frozen)
		return d;
	if (hold_active == core->hold_active)
		return d; /* idempotent, no change */

	core->hold_active = hold_active;

	if (!core->call_active || strcmp(core->call_active_reason, "auto") != 0)
		return d; /* not our (auto) call to manage -- I4-equivalent guard */

	if (hold_active) {
		/* Non-holder activity resumed -- cancel a pending exit, the
		 * call continues (mirrors on_mic_activity_changed()'s own
		 * "active -> cancel pending_off" branch). */
		core->has_pending_off = 0;
	} else {
		core->has_pending_off = 1;
		core->pending_off_at = now + core->cfg.off_debounce_s;
	}
	return d;
}

static void freeze(m4_automode_core_t *core, m4_automode_decision_t *d)
{
	core->frozen = 1;
	snprintf(core->freeze_reason, sizeof(core->freeze_reason),
		M4_T("more than %d switches in %.0f s -- likely flapping",
"больше %d переключений за %.0f с -- вероятный флаппинг"),
		core->cfg.max_switches, core->cfg.rate_window_s);
	d->actions[0] = M4_AUTOMODE_ACTION_ANNOUNCE_FREEZE;
	d->action_count = 1;
}

/* Periodic debounce/rate-limit tick. */
m4_automode_decision_t m4_automode_tick(m4_automode_core_t *core, double now)
{
	m4_automode_decision_t d = {0};
	if (core->frozen)
		return d;

	if (core->has_pending_on && now >= core->pending_on_at) {
		core->has_pending_on = 0;
		if (!core->call_active) {
			if (ratelimiter_record_and_check(&core->limiter, now)) {
				freeze(core, &d);
				return d;
			}
			d.actions[d.action_count++] = M4_AUTOMODE_ACTION_CALL_ON;
			core->call_active = 1;
			set_reason(core->call_active_reason, sizeof(core->call_active_reason), "auto");
		}
	}

	if (core->has_pending_off && now >= core->pending_off_at) {
		core->has_pending_off = 0;
		if (core->call_active && strcmp(core->call_active_reason, "auto") == 0) {
			if (ratelimiter_record_and_check(&core->limiter, now)) {
				freeze(core, &d);
				return d;
			}
			d.actions[d.action_count++] = M4_AUTOMODE_ACTION_CALL_OFF;
			core->call_active = 0;
			set_reason(core->call_active_reason, sizeof(core->call_active_reason), "none");
		}
	}

	return d;
}

/* Call-result rollback. */
void m4_automode_note_call_result(m4_automode_core_t *core, m4_automode_action_t action, int ok, double now)
{
	(void)now;
	if (action == M4_AUTOMODE_ACTION_CALL_ON && !ok) {
		core->call_active = 0;
		set_reason(core->call_active_reason, sizeof(core->call_active_reason), "none");
	} else if (action == M4_AUTOMODE_ACTION_CALL_OFF && !ok) {
		core->call_active = 1;
		set_reason(core->call_active_reason, sizeof(core->call_active_reason), "auto");
	}
}

/* Clears the flapping freeze latch. */
m4_automode_decision_t m4_automode_resume(m4_automode_core_t *core, double now)
{
	(void)now;
	m4_automode_decision_t d = {0};
	core->frozen = 0;
	core->freeze_reason[0] = '\0';
	ratelimiter_reset(&core->limiter, core->cfg.max_switches, core->cfg.rate_window_s);
	core->has_pending_on = 0;
	core->has_pending_off = 0;
	return d;
}

/* Status snapshot (minus pending_*_in_s, see header). */
void m4_automode_status(const m4_automode_core_t *core, double now, m4_automode_status_t *out)
{
	out->frozen = core->frozen;
	out->mic_active = core->mic_active;
	out->hold_active = core->hold_active;
	out->call_active = core->call_active;
	set_reason(out->call_active_reason, sizeof(out->call_active_reason), core->call_active_reason);
	out->switches_in_window = (int)ratelimiter_snapshot_count(&core->limiter, now);
}
