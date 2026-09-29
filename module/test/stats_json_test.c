/* momentum4 "metrics": offline test for the JSON stats file
 * written by m4-adaptive-bridge.c's IO thread (write_stats_json()/
 * append_stats_history()/rotate_stats_history(), see maybe_write_stats()).
 *
 * Runs entirely in an isolated temp directory (mkdtemp under $TMPDIR or
 * /tmp), never touches $XDG_RUNTIME_DIR/momentum4/ (unlike unit_test.c,
 * which uses the bridge's built-in default path when M4_ADAPTIVE_STATS_PATH
 * is not set -- this test always sets it explicitly). Uses evil_helper.c in
 * "normal" mode (fast, deterministic, no qemu-hexagon dependency) since this
 * test is about the stats machinery, not the real encoder -- see
 * unit_test.c/resilience_test.c for real-helper and fault-injection
 * coverage respectively.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#include "m4-adaptive-bridge.h"

static int g_checks = 0, g_failed = 0;

#define CHECK(cond, msg) do { \
	g_checks++; \
	if (cond) { printf("PASS: %s\n", msg); } \
	else { printf("FAIL: %s\n", msg); g_failed++; } \
} while (0)

static char *slurp_file(const char *path, long *out_len)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *buf = malloc((size_t)len + 1);
	if (!buf) { fclose(f); return NULL; }
	size_t r = fread(buf, 1, (size_t)len, f);
	buf[r] = '\0';
	fclose(f);
	if (out_len) *out_len = (long)r;
	return buf;
}

static int count_lines(const char *s)
{
	int n = 0;
	for (; *s; s++)
		if (*s == '\n') n++;
	return n;
}

static void sleep_ms(unsigned ms)
{
	struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L };
	nanosleep(&ts, NULL);
}

int main(void)
{
	char tmpdir[] = "/tmp/m4_stats_test_XXXXXX";
	if (mkdtemp(tmpdir) == NULL) {
		fprintf(stderr, "mkdtemp failed\n");
		return 1;
	}

	char stats_path[512], history_path[512], dummy_loader[512], dummy_capi[512];
	snprintf(stats_path, sizeof(stats_path), "%s/adaptive-stats.json", tmpdir);
	snprintf(history_path, sizeof(history_path), "%s/adaptive-stats-history.jsonl", tmpdir);
	snprintf(dummy_loader, sizeof(dummy_loader), "%s/loader.bin", tmpdir);
	snprintf(dummy_capi, sizeof(dummy_capi), "%s/capi.so", tmpdir);
	FILE *f;
	f = fopen(dummy_loader, "w"); fputs("x", f); fclose(f);
	f = fopen(dummy_capi, "w"); fputs("x", f); fclose(f);

	setenv("EVIL_HELPER_MODE", "normal", 1);

	struct m4_ah_bridge_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.mode = M4_AH_HELPER_MODE_SPAWN;
	cfg.qemu_path = "./test/build/evil_helper";
	cfg.loader_path = dummy_loader;
	cfg.capi_path = dummy_capi;
	cfg.strip_ota_header = true;
	cfg.helper_timeout_ms = 2000;
	cfg.startup_timeout_ms = 2000;
	cfg.auto_restart = false;
	cfg.stats_path = stats_path;
	cfg.stats_history_path = history_path;
	cfg.stats_interval_ms = 50; /* fast, for a short test */

	struct m4_ah_bridge *b = m4_ah_bridge_start(NULL, &cfg);
	CHECK(b != NULL, "bridge starts against evil_helper 'normal' mode with stats enabled");
	if (b == NULL)
		return g_failed ? 1 : 0;

	m4_ah_bridge_set_meta(b, "ll1", -7, true, 3);

	uint8_t pcm[M4_AH_PCM_BLOCK_BYTES];
	memset(pcm, 0, sizeof(pcm));
	uint8_t frame[M4_AH_MAX_FRAME_BYTES];

	/* Drive ~300 ms of traffic (>= 6 stats intervals at 50 ms) plus a
	 * handful of synthetic codec-side events, exactly the way
	 * a2dp-codec-aptx.c's codec_encode_adaptive() would call these. */
	for (int i = 0; i < 30; i++) {
		m4_ah_bridge_submit_pcm(b, pcm, sizeof(pcm));
		m4_ah_bridge_take_frame(b, frame, sizeof(frame));
		m4_ah_bridge_note_encode_call(b);
		if (i == 5) m4_ah_bridge_note_late(b);
		if (i == 10) m4_ah_bridge_note_stall(b);
		if (i == 15) m4_ah_bridge_note_heartbeat_event(b);
		sleep_ms(10);
	}
	/* One more full interval so the LAST batch of counters is flushed to
	 * disk before we read the files below (maybe_write_stats() only writes
	 * when >= stats_interval_ms has elapsed since the previous write). */
	sleep_ms(80);
	m4_ah_bridge_submit_pcm(b, pcm, sizeof(pcm));
	m4_ah_bridge_take_frame(b, frame, sizeof(frame));

	struct stat st;
	CHECK(stat(stats_path, &st) == 0, "adaptive-stats.json exists after >= one interval");

	char tmp_leftover[560];
	snprintf(tmp_leftover, sizeof(tmp_leftover), "%s.tmp", stats_path);
	CHECK(stat(tmp_leftover, &st) != 0, "no leftover .tmp file after atomic rename (write_stats_json)");

	long len = 0;
	char *content = slurp_file(stats_path, &len);
	CHECK(content != NULL && len > 0, "adaptive-stats.json is non-empty");
	if (content) {
		CHECK(strstr(content, "\"mode\":\"ll1\"") != NULL, "JSON contains meta: mode=ll1 (m4_ah_bridge_set_meta)");
		CHECK(strstr(content, "\"latency_adjust_ms\":-7") != NULL, "JSON contains meta: latency_adjust_ms=-7");
		CHECK(strstr(content, "\"heartbeat_enabled\":true") != NULL, "JSON contains meta: heartbeat_enabled=true");
		CHECK(strstr(content, "\"resync_period_min\":3") != NULL, "JSON contains meta: resync_period_min=3");
		CHECK(strstr(content, "\"alive\":true") != NULL, "JSON reports alive=true (evil_helper 'normal' mode never dies)");
		CHECK(strstr(content, "\"encode_calls\":") != NULL, "JSON contains encode_calls field");
		CHECK(strstr(content, "\"frames_produced\":") != NULL, "JSON contains frames_produced field");
		CHECK(strstr(content, "\"helper_late\":") != NULL, "JSON contains helper_late field");
		CHECK(strstr(content, "\"heartbeat_late_triggers\":1") != NULL, "JSON reflects the one note_late() call");
		CHECK(strstr(content, "\"stall_events\":1") != NULL, "JSON reflects the one note_stall() call");
		CHECK(strstr(content, "\"heartbeat_events\":1") != NULL, "JSON reflects the one note_heartbeat_event() call");
		CHECK(strstr(content, "\"helper_restarts\":0") != NULL, "JSON reports helper_restarts=0 (no restart happened)");
		CHECK(content[len - 1] == '\n', "adaptive-stats.json ends with a newline");
		free(content);
	}

	long hlen = 0;
	char *hcontent = slurp_file(history_path, &hlen);
	CHECK(hcontent != NULL && hlen > 0, "adaptive-stats-history.jsonl is non-empty");
	if (hcontent) {
		int lines = count_lines(hcontent);
		CHECK(lines >= 2, "history file has at least 2 lines (multiple 50ms-spaced writes)");
		CHECK(hcontent[0] == '{', "history file's first line looks like a JSON object");
		free(hcontent);
	}

	m4_ah_bridge_stop(b);

	/* Cleanup: best-effort, do not fail the test if this does not fully
	 * succeed (leftover files in /tmp are harmless, never in a live path). */
	unlink(stats_path);
	unlink(tmp_leftover);
	unlink(history_path);
	snprintf(tmp_leftover, sizeof(tmp_leftover), "%s.tmp", history_path);
	unlink(tmp_leftover);
	unlink(dummy_loader);
	unlink(dummy_capi);
	rmdir(tmpdir);

	printf("\n%d check(s) failed\n", g_failed);
	return g_failed ? 1 : 0;
}
