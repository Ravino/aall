/* momentum4: aptX Adaptive NQ2Q TTP heartbeat/resync state machine.
 * SPDX-License-Identifier: MIT
 *
 * NOT upstream PipeWire. Deliberately dependency-free (no <spa/...>, no
 * pthread, no syscalls): this header/its .c file must be buildable and
 * testable with a bare `gcc -std=gnu11`. Caller (a2dp-codec-aptx.c) supplies
 * all timing/PCM data; this module does no I/O, no clock reads, no locking.
 *
 * ---------------------------------------------------------------------------
 * Why this exists:
 *
 *   MOMENTUM 4, in NQ2Q mode, anchors its TTP (time-to-play) clock from the
 *   FIRST packet of a new RTP SSRC. If that anchor packet happens to arrive
 *   late (BT controller/USB queue backed up, helper hiccup, ...), the
 *   headset's whole playout delay jumps up and only crawls back down over
 *   ~50 s (observed on a live M4; common among aptX Adaptive headsets,
 *   which run a slow sample-rate-adjust servo to smooth out playout delay
 *   changes rather than jump instantly). The fix is to deliberately
 *   re-anchor the TTP by sending ONE packet with the OTHER LL SSRC
 *   (0xA1 <-> 0xA2) whose latency_adjust_ms field is chosen so the *target*
 *   delay does not actually change (target = base_target(ssrc) +
 *   latency_adjust_ms). This forces two clean TTP resets (current->alt->
 *   current) and re-anchors on a packet we control the timing of, PROVIDED
 *   we only do it while the stream has been flowing cleanly for a while and
 *   during a silent gap (so the audible discontinuity, if any, lands in
 *   silence).
 *
 *   Resync is only armed after >= stable_s seconds with NONE of {stall,
 *   late, btq} observed (firing right after the very trigger it is
 *   reacting to would just re-anchor on ANOTHER late packet), and only
 *   fires once the stream has also been quiet for >= quiet_ms. A periodic
 *   safety resync fires at most once every period_s regardless of
 *   triggers, gated by the same stability+quiet conditions (maintenance,
 *   so a very long session without any trigger still gets re-anchored
 *   occasionally).
 * ---------------------------------------------------------------------------
 */
#ifndef SPA_BLUEZ5_M4_ADAPTIVE_HEARTBEAT_H_
#define SPA_BLUEZ5_M4_ADAPTIVE_HEARTBEAT_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* SSRC values and NQ2Q base TTP targets, confirmed against a live M4.
 * Fixed for MOMENTUM 4's two LL profiles; HQ (0xA3) is out of scope for
 * this codec. */
#define M4_AH_SSRC_LL0 0xA1u
#define M4_AH_SSRC_LL1 0xA2u
#define M4_AH_BASE_TARGET_LL0_MS 55
#define M4_AH_BASE_TARGET_LL1_MS 106

/* NQ2Q RTP timestamp sample-count field for a 10 ms / 480-sample LL packet
 * : ts = (int16_t)latency_adjust_ms << 16 | samples. */
#define M4_AH_NQ2Q_SAMPLES_LL 480u

enum m4_ah_mode {
	M4_AH_MODE_LL0 = 0, /* SSRC 0xA1, base target 55 ms */
	M4_AH_MODE_LL1 = 1, /* SSRC 0xA2, base target 106 ms */
};

struct m4_ah_heartbeat_cfg {
	bool enabled;
	/* Seconds of a fully clean stream (no stall/late/btq trigger) required
	 * before ANY resync (triggered or periodic) is allowed to fire.
	 * Default: 2.0 s. */
	double stable_s;
	/* Milliseconds of continuously-quiet PCM (peak below quiet_amp)
	 * required, on top of stability, before a resync fires. Default:
	 * 150 ms (also acts as the minimum silence the
	 * cross-fade/overlap at the headset needs for live tuning, `--now`). */
	double quiet_ms;
	/* Minimum time between two auto-resyncs, regardless of reason.
	 * Default 10 s . */
	double min_interval_s;
	/* Maintenance resync period when nothing else triggers one. Default
	 * 600 s . */
	double period_s;
	/* Peak abs S16 PCM amplitude below which a block counts as "quiet".
	 * Default ~103 (~ -50 dBFS of INT16_MAX), matching the standalone
	 * mirror's --quiet-dbfs -50 default . */
	int16_t quiet_amp;
};

struct m4_ah_heartbeat {
	struct m4_ah_heartbeat_cfg cfg;
	enum m4_ah_mode mode;
	int32_t latency_adjust_ms;

	double clean_since_ms;    /* monotonic ms; reset to "now" by any trigger */
	double quiet_since_ms;    /* monotonic ms when the quiet run started; <0 = not quiet */
	double last_resync_ms;    /* monotonic ms of the last resync (auto or manual) */
	double period_deadline_ms;/* next time a periodic resync is due */

	bool pending;             /* a trigger fired and is still awaiting stability+quiet */
	const char *pending_reason; /* "stall" | "late" | "btq", static string, no alloc */

	/* Diagnostics only, never read by decide()/observe() themselves. */
	uint32_t n_triggers, n_resyncs, n_periodic;
};

void m4_ah_heartbeat_cfg_defaults(struct m4_ah_heartbeat_cfg *cfg);

/* base_target_ms(): the two NQ2Q per-mode TTP targets before latency_adjust,
 * exposed separately so callers (and tests) can compute expected blip values
 * without duplicating the mapping. */
static inline int32_t m4_ah_heartbeat_base_target_ms(enum m4_ah_mode mode)
{
	return mode == M4_AH_MODE_LL0 ? M4_AH_BASE_TARGET_LL0_MS : M4_AH_BASE_TARGET_LL1_MS;
}

static inline uint32_t m4_ah_heartbeat_ssrc(enum m4_ah_mode mode)
{
	return mode == M4_AH_MODE_LL0 ? M4_AH_SSRC_LL0 : M4_AH_SSRC_LL1;
}

static inline enum m4_ah_mode m4_ah_heartbeat_other_mode(enum m4_ah_mode mode)
{
	return mode == M4_AH_MODE_LL0 ? M4_AH_MODE_LL1 : M4_AH_MODE_LL0;
}

/* now_ms is a monotonic clock in milliseconds, caller-supplied (e.g.
 * CLOCK_MONOTONIC via clock_gettime() in the real codec, or a synthetic
 * counter in tests). Starts in the "assumed clean, not quiet, no resync
 * pending" state with the first periodic deadline period_s away (so it never
 * fires an immediate resync right after a fresh transport comes up). */
void m4_ah_heartbeat_init(struct m4_ah_heartbeat *hb, const struct m4_ah_heartbeat_cfg *cfg,
		enum m4_ah_mode mode, int32_t latency_adjust_ms, double now_ms);

/* Live parameter changes ("set mode"/"set adj"-style), but there
 * is currently no live control channel wired up for the SPA codec -- these
 * are here so codec_init_adaptive()/tests can (re)configure before/at
 * start-up, and so a future control channel has a ready entry point.
 * Resets the stability timer, since changing the target is itself a
 * discontinuity. */
void m4_ah_heartbeat_set_mode(struct m4_ah_heartbeat *hb, enum m4_ah_mode mode, double now_ms);
void m4_ah_heartbeat_set_latency_adjust(struct m4_ah_heartbeat *hb, int32_t latency_adjust_ms,
		double now_ms);

/* Called once per completed 10 ms block, AFTER encode()/abr_process() for
 * that block are done (i.e. with fresh data about the packet that was just
 * built) -- see a2dp-codec-aptx.c for exactly where. Never returns anything;
 * purely updates internal timers/counters that the NEXT decide() call reads.
 *
 *   peak_amplitude: max abs S16 sample seen in this block's PCM (both
 *     channels), for the quiet-gate.
 *   trigger_stall: this codec's own encode() call arrived unusually late
 *     relative to the previous one (RT thread starved/descheduled).
 *   trigger_late: the helper did not have a finished frame ready for this
 *     block despite being alive (an actual gap sent to the headset) --
 *     the "late" trigger, marking a real delivery gap.
 *   trigger_btq: the outgoing BT socket's kernel send queue (TIOCOUTQ) was
 *     at or above the "packet reached M4 late" threshold for this block. */
void m4_ah_heartbeat_observe(struct m4_ah_heartbeat *hb, double now_ms,
		int16_t peak_amplitude, bool trigger_stall, bool trigger_late, bool trigger_btq);

/* Called once per outgoing packet, BEFORE its RTP header is built (i.e. in
 * codec_start_encode_adaptive(), using data observed through the END of the
 * previous packet -- see the file-level comment in a2dp-codec-aptx.c for why
 * this one-packet lag is intentional and harmless).
 *
 * If this returns true, the caller MUST build THIS ONE packet's header with
 * *out_ssrc / a timestamp field of ((int16_t)*out_adjust_ms << 16) |
 * M4_AH_NQ2Q_SAMPLES_LL instead of the normal mode SSRC/latency_adjust_ms.
 * The very next call (for the next packet) automatically resumes reporting
 * the normal values -- there is no separate "revert" step, by design (see
 * file header: the second ttp_reset that re-anchors correctly IS the next
 * normal packet). */
bool m4_ah_heartbeat_decide(struct m4_ah_heartbeat *hb, double now_ms,
		uint32_t *out_ssrc, int32_t *out_adjust_ms, const char **out_reason);

/* Bypasses ALL gating (stability/quiet/min-interval) and fires a blip right
 * now. For manual/test use only, e.g. an operator-triggered forced resync;
 * never called from the normal per-block path.
 * Still resets the internal timers exactly like a normal resync so it
 * interacts correctly with subsequent auto-resync gating. */
void m4_ah_heartbeat_force(struct m4_ah_heartbeat *hb, double now_ms,
		uint32_t *out_ssrc, int32_t *out_adjust_ms);

#ifdef __cplusplus
}
#endif

#endif /* SPA_BLUEZ5_M4_ADAPTIVE_HEARTBEAT_H_ */
