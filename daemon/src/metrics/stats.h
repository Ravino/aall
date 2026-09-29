/* metrics/stats.h -- reader/parser for $XDG_RUNTIME_DIR/momentum4/adaptive-stats.json,
 * written by the SPA bluez5 aptX Adaptive plugin's bridge
 * (module/m4-src/m4-adaptive-bridge.c:build_stats_json(), see
 * that function for the authoritative field list/meaning). Read-only,
 * momentum4d/momentum4 never writes this file.
 *
 * Split in two on purpose (for unit tests of the parsers):
 *  - m4_stats_parse() is pure (buffer in, struct out, no I/O) and is what
 *    the unit tests exercise directly, including the live sample captured
 *    from this machine and deliberately malformed/partial variants.
 *  - m4_stats_load() does the actual file I/O (bounded read) and calls
 *    m4_stats_parse().
 */
#ifndef MOMENTUM4_METRICS_STATS_H
#define MOMENTUM4_METRICS_STATS_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define M4_STATS_MODE_LEN 16
#define M4_STATS_RT_POLICY_LEN 16

struct m4_stats {
	long long ts_unix_ms;
	double uptime_ms;
	bool alive;
	char mode[M4_STATS_MODE_LEN];             /* "ll0"/"ll1"/"hq"/... */
	int latency_adjust_ms;
	bool heartbeat_enabled;
	unsigned resync_period_min;

	unsigned encode_calls;
	unsigned frames_produced;
	unsigned input_drops_ring_full;
	unsigned input_drops_trylock;
	unsigned helper_late;
	unsigned heartbeat_late_triggers;
	unsigned stall_events;
	unsigned heartbeat_events;
	unsigned output_ring_full_drops;
	unsigned helper_restarts;

	/* ring_in/ring_out are JSON `null` (not a number) when the bridge's
	 * own trylock on the ring failed while building this snapshot (see
	 * build_stats_json()) -- have_ring_in/out is false in that case and
	 * ring_in/ring_out are left at 0, not treated as an error. */
	bool have_ring_in;
	unsigned ring_in;
	unsigned ring_in_cap;
	bool have_ring_out;
	unsigned ring_out;
	unsigned ring_out_cap;

	unsigned out_ring_trimmed_frames;
	unsigned in_ring_trimmed_ms;
	unsigned out_ring_max_window;
	unsigned in_ring_max_window;

	char bridge_rt_policy[M4_STATS_RT_POLICY_LEN]; /* "fifo"/"other"/"rr"/"unknown"/... */
	int bridge_rt_prio;
};

/* Parses one JSON object (as produced by build_stats_json(), i.e. a single
 * flat `{"key":value,...}` with no nesting except the two nullable ring
 * fields) out of `buf`[0..len). Unknown keys are ignored (forward
 * compatible); missing keys leave the corresponding field at its zero value.
 * Returns 0 if `buf` parses as a JSON object at all (even if some/all fields
 * were missing or had the wrong type -- callers that need to distinguish
 * "empty object" from "fully populated" should check fields they care
 * about), -EINVAL if `buf` is not a JSON object to begin with. Never reads
 * out of bounds, never requires NUL-termination. */
int m4_stats_parse(const char *buf, size_t len, struct m4_stats *out);

/* Reads and parses `path` (bounded: rejects files above a sane cap so a
 * corrupt/hostile file can't make momentum4 allocate unbounded memory).
 * Returns 0 on success, -errno on I/O failure (including ENOENT: helper
 * has never run / stats file not written yet -- an expected, non-fatal
 * condition for `status`), -EINVAL if the file's content does not parse. */
int m4_stats_load(const char *path, struct m4_stats *out);

#ifdef __cplusplus
}
#endif

#endif /* MOMENTUM4_METRICS_STATS_H */
