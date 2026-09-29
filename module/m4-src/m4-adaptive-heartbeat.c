/* momentum4: aptX Adaptive NQ2Q TTP heartbeat/resync state machine.
 * SPDX-License-Identifier: MIT
 * See m4-adaptive-heartbeat.h for the design rationale. Deliberately free of
 * any SPA/pthread/syscall dependency so it can be unit-tested with a bare
 * `gcc -std=gnu11` (see module/test/heartbeat_unit_test.c).
 */
#include "m4-adaptive-heartbeat.h"

#include <stdlib.h> /* abs() */

void m4_ah_heartbeat_cfg_defaults(struct m4_ah_heartbeat_cfg *cfg)
{
	cfg->enabled = true;
	cfg->stable_s = 2.0;
	cfg->quiet_ms = 150.0;
	cfg->min_interval_s = 10.0;
	cfg->period_s = 600.0;
	cfg->quiet_amp = 103; /* ~ -50 dBFS of INT16_MAX, matches --quiet-dbfs -50 */
}

static void reset_stability(struct m4_ah_heartbeat *hb, double now_ms)
{
	hb->clean_since_ms = now_ms;
	hb->quiet_since_ms = -1.0;
	hb->pending = false;
	hb->pending_reason = NULL;
}

void m4_ah_heartbeat_init(struct m4_ah_heartbeat *hb, const struct m4_ah_heartbeat_cfg *cfg,
		enum m4_ah_mode mode, int32_t latency_adjust_ms, double now_ms)
{
	hb->cfg = *cfg;
	hb->mode = mode;
	hb->latency_adjust_ms = latency_adjust_ms;
	reset_stability(hb, now_ms);
	/* Start far in the past so the first *actual* trigger/period is what
	 * gates the first resync, not min_interval_s left over from "startup". */
	hb->last_resync_ms = now_ms - hb->cfg.min_interval_s * 1000.0 - 1.0;
	hb->period_deadline_ms = now_ms + hb->cfg.period_s * 1000.0;
	hb->n_triggers = hb->n_resyncs = hb->n_periodic = 0;
}

void m4_ah_heartbeat_set_mode(struct m4_ah_heartbeat *hb, enum m4_ah_mode mode, double now_ms)
{
	hb->mode = mode;
	reset_stability(hb, now_ms);
}

void m4_ah_heartbeat_set_latency_adjust(struct m4_ah_heartbeat *hb, int32_t latency_adjust_ms,
		double now_ms)
{
	hb->latency_adjust_ms = latency_adjust_ms;
	reset_stability(hb, now_ms);
}

void m4_ah_heartbeat_observe(struct m4_ah_heartbeat *hb, double now_ms,
		int16_t peak_amplitude, bool trigger_stall, bool trigger_late, bool trigger_btq)
{
	if (trigger_stall || trigger_late || trigger_btq) {
		hb->n_triggers++;
		hb->clean_since_ms = now_ms;
		hb->pending = true;
		/* Priority is cosmetic (diagnostics string only); all three equally
		 * reset the stability timer. */
		hb->pending_reason = trigger_stall ? "stall" : trigger_late ? "late" : "btq";
	}

	{
		/* Callers are expected to pass an already-non-negative peak (see
		 * a2dp-codec-aptx.c's adaptive_pcm_peak()), but compute abs() in
		 * int32_t regardless: peak_amplitude == INT16_MIN negated as an
		 * int16_t would overflow (32768 does not fit), which previously
		 * wrapped back around to a NEGATIVE amplitude and was
		 * mis-classified as "quiet" -- caught by
		 * test/heartbeat_unit_test.c's test_quiet_gate_edges(). */
		int32_t amp32 = peak_amplitude < 0 ? -(int32_t)peak_amplitude : (int32_t)peak_amplitude;
		bool quiet_now = amp32 < (int32_t)hb->cfg.quiet_amp;
		if (quiet_now) {
			if (hb->quiet_since_ms < 0.0)
				hb->quiet_since_ms = now_ms;
		} else {
			hb->quiet_since_ms = -1.0;
		}
	}
}

static void compute_blip(const struct m4_ah_heartbeat *hb, uint32_t *out_ssrc,
		int32_t *out_adjust_ms)
{
	enum m4_ah_mode alt = m4_ah_heartbeat_other_mode(hb->mode);
	int32_t target_cur = m4_ah_heartbeat_base_target_ms(hb->mode) + hb->latency_adjust_ms;
	int32_t blip_adjust = target_cur - m4_ah_heartbeat_base_target_ms(alt);

	/* ts upper 16 bits are read by the headset as an int16_t
	 * ;
	 * clamp defensively so we never emit something that would wrap. This
	 * should not trigger for any latency_adjust_ms in the documented
	 * -100.+500 range . */
	if (blip_adjust > 32767)
		blip_adjust = 32767;
	if (blip_adjust < -32768)
		blip_adjust = -32768;

	*out_ssrc = m4_ah_heartbeat_ssrc(alt);
	*out_adjust_ms = blip_adjust;
}

bool m4_ah_heartbeat_decide(struct m4_ah_heartbeat *hb, double now_ms,
		uint32_t *out_ssrc, int32_t *out_adjust_ms, const char **out_reason)
{
	bool stable, quiet, due_periodic, interval_ok;

	if (!hb->cfg.enabled)
		return false;

	stable = (now_ms - hb->clean_since_ms) >= hb->cfg.stable_s * 1000.0;
	quiet = hb->quiet_since_ms >= 0.0 &&
		(now_ms - hb->quiet_since_ms) >= hb->cfg.quiet_ms;
	due_periodic = now_ms >= hb->period_deadline_ms;
	interval_ok = (now_ms - hb->last_resync_ms) >= hb->cfg.min_interval_s * 1000.0;

	if (!(stable && quiet && interval_ok && (hb->pending || due_periodic)))
		return false;

	compute_blip(hb, out_ssrc, out_adjust_ms);
	if (out_reason)
		*out_reason = hb->pending ? hb->pending_reason : "period";

	if (!hb->pending)
		hb->n_periodic++;
	hb->n_resyncs++;
	hb->last_resync_ms = now_ms;
	hb->period_deadline_ms = now_ms + hb->cfg.period_s * 1000.0;
	/* A resync is itself a (harmless, in-silence) discontinuity: require a
	 * fresh stability run before the NEXT one. */
	reset_stability(hb, now_ms);
	return true;
}

void m4_ah_heartbeat_force(struct m4_ah_heartbeat *hb, double now_ms,
		uint32_t *out_ssrc, int32_t *out_adjust_ms)
{
	compute_blip(hb, out_ssrc, out_adjust_ms);
	hb->n_resyncs++;
	hb->last_resync_ms = now_ms;
	hb->period_deadline_ms = now_ms + hb->cfg.period_s * 1000.0;
	reset_stability(hb, now_ms);
}
