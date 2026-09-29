#include "stats.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <spa/utils/json.h>

/* Cap on the file we're willing to read (real files are a few hundred
 * bytes, see build_stats_json()'s `char json[768]` -- this is generous
 * headroom, not a tight fit, purely to keep a corrupt/hostile file from
 * making us allocate unbounded memory). */
#define M4_STATS_MAX_FILE_SIZE (64u * 1024u)

/* spa_json only parses int/float/bool/string tokens; ts_unix_ms (a Unix ms
 * timestamp) overflows a 32-bit int well before 2026, and uptime_ms is
 * formatted as a plain (no-exponent) decimal by build_stats_json()'s
 * "%.0f" -- both need their own tiny strtoX-based parser instead of
 * spa_json_parse_int()/parse_float(). */
static int parse_ll(const char *val, int len, long long *out)
{
	char buf[32];
	char *end;

	if (len <= 0 || len >= (int)sizeof(buf))
		return 0;
	memcpy(buf, val, (size_t)len);
	buf[len] = '\0';
	*out = strtoll(buf, &end, 10);
	return end == buf + len;
}

static int parse_double(const char *val, int len, double *out)
{
	char buf[64];
	char *end;

	if (len <= 0 || len >= (int)sizeof(buf))
		return 0;
	memcpy(buf, val, (size_t)len);
	buf[len] = '\0';
	*out = strtod(buf, &end);
	return end == buf + len;
}

/* int -> unsigned helper: negative values from a well-behaved bridge never
 * happen (all counters are atomic_uint on the writer side), but a
 * corrupted/hostile file could contain one -- clamp to 0 rather than wrap. */
static void set_uint(unsigned *field, const char *val, int len)
{
	int v;
	if (spa_json_parse_int(val, len, &v) > 0)
		*field = v < 0 ? 0u : (unsigned)v;
}

int m4_stats_parse(const char *buf, size_t len, struct m4_stats *out)
{
	struct spa_json it;
	char key[64];
	const char *val;
	int vlen;

	if (out == NULL)
		return -EINVAL;
	memset(out, 0, sizeof(*out));

	if (buf == NULL || spa_json_begin_object(&it, buf, len) <= 0)
		return -EINVAL;

	while ((vlen = spa_json_object_next(&it, key, sizeof(key), &val)) > 0) {
		if (strcmp(key, "ts_unix_ms") == 0) {
			parse_ll(val, vlen, &out->ts_unix_ms);
		} else if (strcmp(key, "uptime_ms") == 0) {
			parse_double(val, vlen, &out->uptime_ms);
		} else if (strcmp(key, "alive") == 0) {
			spa_json_parse_bool(val, vlen, &out->alive);
		} else if (strcmp(key, "mode") == 0) {
			spa_json_parse_stringn(val, vlen, out->mode, sizeof(out->mode));
		} else if (strcmp(key, "latency_adjust_ms") == 0) {
			spa_json_parse_int(val, vlen, &out->latency_adjust_ms);
		} else if (strcmp(key, "heartbeat_enabled") == 0) {
			spa_json_parse_bool(val, vlen, &out->heartbeat_enabled);
		} else if (strcmp(key, "resync_period_min") == 0) {
			set_uint(&out->resync_period_min, val, vlen);
		} else if (strcmp(key, "encode_calls") == 0) {
			set_uint(&out->encode_calls, val, vlen);
		} else if (strcmp(key, "frames_produced") == 0) {
			set_uint(&out->frames_produced, val, vlen);
		} else if (strcmp(key, "input_drops_ring_full") == 0) {
			set_uint(&out->input_drops_ring_full, val, vlen);
		} else if (strcmp(key, "input_drops_trylock") == 0) {
			set_uint(&out->input_drops_trylock, val, vlen);
		} else if (strcmp(key, "helper_late") == 0) {
			set_uint(&out->helper_late, val, vlen);
		} else if (strcmp(key, "heartbeat_late_triggers") == 0) {
			set_uint(&out->heartbeat_late_triggers, val, vlen);
		} else if (strcmp(key, "stall_events") == 0) {
			set_uint(&out->stall_events, val, vlen);
		} else if (strcmp(key, "heartbeat_events") == 0) {
			set_uint(&out->heartbeat_events, val, vlen);
		} else if (strcmp(key, "output_ring_full_drops") == 0) {
			set_uint(&out->output_ring_full_drops, val, vlen);
		} else if (strcmp(key, "helper_restarts") == 0) {
			set_uint(&out->helper_restarts, val, vlen);
		} else if (strcmp(key, "ring_in") == 0) {
			if (!spa_json_is_null(val, vlen)) {
				set_uint(&out->ring_in, val, vlen);
				out->have_ring_in = true;
			}
		} else if (strcmp(key, "ring_in_cap") == 0) {
			set_uint(&out->ring_in_cap, val, vlen);
		} else if (strcmp(key, "ring_out") == 0) {
			if (!spa_json_is_null(val, vlen)) {
				set_uint(&out->ring_out, val, vlen);
				out->have_ring_out = true;
			}
		} else if (strcmp(key, "ring_out_cap") == 0) {
			set_uint(&out->ring_out_cap, val, vlen);
		} else if (strcmp(key, "out_ring_trimmed_frames") == 0) {
			set_uint(&out->out_ring_trimmed_frames, val, vlen);
		} else if (strcmp(key, "in_ring_trimmed_ms") == 0) {
			set_uint(&out->in_ring_trimmed_ms, val, vlen);
		} else if (strcmp(key, "out_ring_max_window") == 0) {
			set_uint(&out->out_ring_max_window, val, vlen);
		} else if (strcmp(key, "in_ring_max_window") == 0) {
			set_uint(&out->in_ring_max_window, val, vlen);
		} else if (strcmp(key, "bridge_rt_policy") == 0) {
			spa_json_parse_stringn(val, vlen, out->bridge_rt_policy, sizeof(out->bridge_rt_policy));
		} else if (strcmp(key, "bridge_rt_prio") == 0) {
			spa_json_parse_int(val, vlen, &out->bridge_rt_prio);
		}
		/* unknown keys: ignored on purpose (forward compatibility) */
	}

	return 0;
}

int m4_stats_load(const char *path, struct m4_stats *out)
{
	FILE *f;
	char *buf;
	size_t cap, n;
	struct stat st;
	int rc;

	if (path == NULL || out == NULL)
		return -EINVAL;

	if (stat(path, &st) != 0)
		return -errno;
	if (!S_ISREG(st.st_mode))
		return -EINVAL;
	if (st.st_size < 0 || (unsigned long)st.st_size > M4_STATS_MAX_FILE_SIZE)
		return -EFBIG;

	f = fopen(path, "re");
	if (f == NULL)
		return -errno;

	/* +1 for a defensive NUL; not required by m4_stats_parse() (it is
	 * length-bounded, no NUL-termination assumed) but harmless and cheap. */
	cap = (size_t)st.st_size + 1;
	buf = malloc(cap);
	if (buf == NULL) {
		fclose(f);
		return -ENOMEM;
	}

	n = fread(buf, 1, (size_t)st.st_size, f);
	rc = ferror(f) ? -EIO : 0;
	fclose(f);
	if (rc != 0) {
		free(buf);
		return rc;
	}
	buf[n] = '\0';

	rc = m4_stats_parse(buf, n, out);
	free(buf);
	return rc;
}
