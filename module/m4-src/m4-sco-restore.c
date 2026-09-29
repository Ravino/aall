/* momentum4 patch 0008: see m4-sco-restore.h.
 * SPDX-License-Identifier: MIT
 */
#include "m4-sco-restore.h"

#include <string.h>

void m4_sco_restore_timing_defaults(struct m4_sco_restore_timing *t)
{
	t->first_delay_ms = M4_SCO_RESTORE_FIRST_DELAY_MS;
	t->retry_delay_ms = M4_SCO_RESTORE_RETRY_DELAY_MS;
	t->connect_timeout_ms = M4_SCO_RESTORE_CONNECT_TIMEOUT_MS;
	t->max_retries = M4_SCO_RESTORE_MAX_RETRIES;
	t->window_ms = M4_SCO_RESTORE_WINDOW_MS;
	t->max_episodes = M4_SCO_RESTORE_MAX_EPISODES;
}

static void clamp_timing(struct m4_sco_restore_timing *t)
{
	if (t->max_episodes < 1)
		t->max_episodes = 1;
	else if (t->max_episodes > M4_SCO_RESTORE_MAX_EPISODES)
		t->max_episodes = M4_SCO_RESTORE_MAX_EPISODES;
}

void m4_sco_restore_init_with_timing(struct m4_sco_restore *r, const struct m4_sco_restore_timing *timing)
{
	memset(r, 0, sizeof(*r));
	r->timing = *timing;
	clamp_timing(&r->timing);
}

void m4_sco_restore_set_timing(struct m4_sco_restore *r, const struct m4_sco_restore_timing *timing)
{
	r->timing = *timing;
	clamp_timing(&r->timing);
}

void m4_sco_restore_init(struct m4_sco_restore *r)
{
	struct m4_sco_restore_timing t;

	m4_sco_restore_timing_defaults(&t);
	m4_sco_restore_init_with_timing(r, &t);
}

static void prune(struct m4_sco_restore *r, uint64_t now_ms)
{
	uint64_t keep[M4_SCO_RESTORE_MAX_EPISODES];
	unsigned n = 0;

	for (unsigned i = 0; i < r->n_episodes; i++) {
		uint64_t t = r->episode_ms[i];

		/* t > now_ms: clock went backwards (cannot with MONOTONIC) -- keep, conservative */
		if (t > now_ms || now_ms - t < (uint64_t)r->timing.window_ms)
			keep[n++] = t;
	}
	memcpy(r->episode_ms, keep, n * sizeof(keep[0]));
	r->n_episodes = n;
	r->next = n % (unsigned)r->timing.max_episodes;
}

enum m4_sco_restore_verdict m4_sco_restore_on_drop(struct m4_sco_restore *r, uint64_t now_ms,
						    int *delay_ms)
{
	/* Defensive lazy default: a `struct m4_sco_restore` embedded in a
	 * caller's own calloc()'d struct (e.g. backend-native.c's
	 * struct transport_data, which PipeWire's own transport allocation
	 * zero-initializes, not this file) may never have gone through
	 * m4_sco_restore_init()/_init_with_timing() explicitly -- a real risk
	 * introduced by making timing.max_episodes a runtime field (used as
	 * a modulus below) instead of the old compile-time
	 * M4_SCO_RESTORE_MAX_EPISODES macro, which was safe with a bare
	 * zero-initialized struct. Guard against timing.max_episodes == 0 here
	 * (the only field whose zero value would be unsafe, as a modulus)
	 * rather than trust every call site to have called an init function
	 * first -- matches this whole plugin's "never fatal on a config/
	 * lifecycle mistake" contract. */
	if (r->timing.max_episodes <= 0)
		m4_sco_restore_timing_defaults(&r->timing);

	if (r->active)
		return M4_SCO_RESTORE_DONE;

	prune(r, now_ms);
	if (r->n_episodes >= (unsigned)r->timing.max_episodes)
		return M4_SCO_RESTORE_GIVE_UP;

	r->episode_ms[r->next] = now_ms;
	r->next = (r->next + 1) % (unsigned)r->timing.max_episodes;
	r->n_episodes++;
	r->retries_left = r->timing.max_retries;
	r->active = true;
	if (delay_ms)
		*delay_ms = r->timing.first_delay_ms;
	return M4_SCO_RESTORE_SCHEDULE;
}

enum m4_sco_restore_verdict m4_sco_restore_on_result(struct m4_sco_restore *r, bool ok,
						      int *delay_ms)
{
	if (!r->active)
		return M4_SCO_RESTORE_DONE;
	if (ok) {
		r->active = false;
		return M4_SCO_RESTORE_DONE;
	}
	if (r->retries_left > 0) {
		r->retries_left--;
		if (delay_ms)
			*delay_ms = r->timing.retry_delay_ms;
		return M4_SCO_RESTORE_SCHEDULE;
	}
	r->active = false;
	return M4_SCO_RESTORE_GIVE_UP;
}

void m4_sco_restore_cancel(struct m4_sco_restore *r)
{
	r->active = false;
	r->retries_left = 0;
}

const char *m4_sco_restore_verdict_name(enum m4_sco_restore_verdict v)
{
	switch (v) {
	case M4_SCO_RESTORE_DONE: return "done";
	case M4_SCO_RESTORE_SCHEDULE: return "schedule";
	case M4_SCO_RESTORE_GIVE_UP: return "give-up";
	default: return "?";
	}
}
