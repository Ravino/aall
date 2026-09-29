/* Unit tests for daemon/daemon.c against a FAKE graph -- the central safety property
 * under test is daemon.h's write-gating matrix (observe/manual/auto x
 * safe-latch), asserted directly against core/graph_ops_fake.c's
 * set_profile_calls counter, which is the ONLY place in the entire
 * momentum4d write path that ever reaches PipeWire (I11/I12). No live
 * PipeWire, no real sockets, no real clock -- every test drives
 * m4_daemon_mic_active_changed()/m4_daemon_tick() with synthetic
 * timestamps and m4_daemon_handle_request() directly (no ctl.sock).
 *
 * "kill -9 on the daemon changes nothing about the sound" (item 6) is an architectural
 * property (I2: no audio nodes/links ever held in momentum4d's own
 * pw_context -- graph_ops_fake.c mirrors the REAL m4_graph_ops_pw's shape
 * exactly: open/get_state/set_profile, nothing that creates or owns
 * anything) rather than something a unit test can directly observe (you
 * cannot unit-test "the process no longer exists"). What IS directly
 * testable, and tested below (test_free_does_not_touch_graph): destroying
 * the daemon (standing in for the process exiting/being killed, which is
 * strictly less graceful than an orderly free()) never calls
 * set_profile() -- there is no "restore/cleanup" write path to accidentally
 * trigger on shutdown, which is exactly what makes an UNGRACEFUL shutdown
 * (kill -9, no free() at all) equally harmless: the graceful path already
 * does nothing.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <systemd/sd-event.h>

#include "core/graph_ops_fake.h"
#include "daemon/daemon.h"
#include "ipc/ctl.h"

static int failures = 0;

#define CHECK(cond, msg) \
	do { \
		if (!(cond)) { \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, (msg)); \
			failures++; \
		} \
	} while (0)

#define M4_TEST_MAC "00:1B:66:00:00:01"

static void make_tmpdir(char *out, size_t out_len)
{
	snprintf(out, out_len, "/tmp/m4d-test-daemon.XXXXXX");
	CHECK(mkdtemp(out) != NULL, "mkdtemp");
}

struct fixture {
	sd_event *event;
	m4_graph_ops_fake_t fake;
	char runtime_dir[128];
	struct m4_daemon *d;
};

static void fixture_setup(struct fixture *fx)
{
	struct m4_daemon_config cfg = {0};

	CHECK(sd_event_new(&fx->event) >= 0, "sd_event_new");
	m4_graph_ops_fake_init(&fx->fake);
	make_tmpdir(fx->runtime_dir, sizeof(fx->runtime_dir));

	cfg.graph_ops = m4_graph_ops_fake(&fx->fake); /* also arms the fake for open() below */
	cfg.mac_address = M4_TEST_MAC;
	cfg.runtime_dir = fx->runtime_dir;
	cfg.stats_path = "";
	cfg.confirm_timeout_ms = 200;
	cfg.enable_micdetect = false;
	cfg.enable_ctl = false;
	cfg.enable_tick_timer = false;

	fx->d = m4_daemon_new(fx->event, &cfg);
	CHECK(fx->d != NULL, "daemon_new");
}

static void fixture_teardown(struct fixture *fx)
{
	m4_daemon_free(fx->d);
	sd_event_unref(fx->event);
}

static void set_mode(struct fixture *fx, const char *mode_word)
{
	char req[32], response[M4_CTL_MAX_RESPONSE];

	snprintf(req, sizeof(req), "MODE %s", mode_word);
	m4_daemon_handle_request(fx->d, req, response, sizeof(response));
	CHECK(strncmp(response, "OK", 2) == 0, "MODE request accepted");
}

static void touch_safe_latch(struct fixture *fx)
{
	char path[256];
	FILE *f;

	snprintf(path, sizeof(path), "%s/safe-latch", fx->runtime_dir);
	f = fopen(path, "w");
	CHECK(f != NULL, "create safe-latch");
	fclose(f);
}

/* --- default mode is observe -------------------------------------------- */

static void test_default_mode_is_observe(void)
{
	struct fixture fx;
	struct m4_daemon_status st;

	fixture_setup(&fx);
	m4_daemon_get_status(fx.d, &st);
	CHECK(st.mode == M4_DAEMON_MODE_OBSERVE, "fresh daemon defaults to observe");
	fixture_teardown(&fx);
}

/* --- observe: neither manual nor automode writes ------------------------ */

static void test_observe_blocks_manual_call(void)
{
	struct fixture fx;
	char response[M4_CTL_MAX_RESPONSE];

	fixture_setup(&fx);
	m4_daemon_handle_request(fx.d, "CALL ON", response, sizeof(response));
	CHECK(strncmp(response, "ERR", 3) == 0, "observe: manual CALL ON rejected");
	CHECK(fx.fake.set_profile_calls == 0, "observe: no graph write from manual call");
	fixture_teardown(&fx);
}

static void test_observe_blocks_automode_call(void)
{
	struct fixture fx;

	fixture_setup(&fx);
	/* mic goes active at t=0 -> automode schedules CALL_ON at t=0.3
	 * (default on_debounce_s); tick() past that deadline. */
	m4_daemon_mic_active_changed(fx.d, true, 0.0);
	m4_daemon_tick(fx.d, 1.0);
	CHECK(fx.fake.set_profile_calls == 0,
	      "observe: automode computed a decision but never wrote to the graph");
	fixture_teardown(&fx);
}

/* --- manual: manual writes, automode does not --------------------------- */

static void test_manual_mode_allows_manual_call(void)
{
	struct fixture fx;
	char response[M4_CTL_MAX_RESPONSE];

	fixture_setup(&fx);
	set_mode(&fx, "manual");

	m4_daemon_handle_request(fx.d, "CALL ON", response, sizeof(response));
	CHECK(strncmp(response, "OK", 2) == 0, "manual: CALL ON succeeds");
	CHECK(fx.fake.set_profile_calls == 1, "manual: exactly one graph write");
	CHECK(strcmp(fx.fake.profile_name, "headset-head-unit") == 0, "manual: profile switched to mSBC");
	CHECK(fx.fake.last_set_profile_save == true, "manual: save=true");

	fixture_teardown(&fx);
}

static void test_manual_mode_blocks_automode_call(void)
{
	struct fixture fx;

	fixture_setup(&fx);
	set_mode(&fx, "manual");

	m4_daemon_mic_active_changed(fx.d, true, 0.0);
	m4_daemon_tick(fx.d, 1.0);
	CHECK(fx.fake.set_profile_calls == 0, "manual: automode decision computed but not executed");

	fixture_teardown(&fx);
}

/* --- auto: both manual and automode write -------------------------------- */

static void test_auto_mode_allows_automode_call(void)
{
	struct fixture fx;

	fixture_setup(&fx);
	set_mode(&fx, "auto");

	m4_daemon_mic_active_changed(fx.d, true, 0.0);
	CHECK(fx.fake.set_profile_calls == 0, "no write yet -- still inside on_debounce_s");
	m4_daemon_tick(fx.d, 1.0);
	CHECK(fx.fake.set_profile_calls == 1, "auto: automode's CALL_ON was executed");
	CHECK(strcmp(fx.fake.profile_name, "headset-head-unit") == 0, "auto: profile switched to mSBC");

	/* Mic goes inactive -- automode should eventually switch back
	 * (off_debounce_s = 2.5s default) -- I4 does not apply here (reason
	 * is "auto", not "manual"). */
	m4_daemon_mic_active_changed(fx.d, false, 1.1);
	m4_daemon_tick(fx.d, 4.0);
	CHECK(fx.fake.set_profile_calls == 2, "auto: automode's CALL_OFF was executed");
	CHECK(strcmp(fx.fake.profile_name, "a2dp-sink") == 0, "auto: profile switched back to A2DP");

	fixture_teardown(&fx);
}

static void test_auto_mode_allows_manual_call_too(void)
{
	struct fixture fx;
	char response[M4_CTL_MAX_RESPONSE];

	fixture_setup(&fx);
	set_mode(&fx, "auto");

	m4_daemon_handle_request(fx.d, "CALL ON", response, sizeof(response));
	CHECK(strncmp(response, "OK", 2) == 0, "auto: manual CALL ON also works");
	CHECK(fx.fake.set_profile_calls == 1, "auto: manual write happened");

	fixture_teardown(&fx);
}

/* --- safe-latch overrides every mode, unconditionally -------------------- */

static void test_safe_latch_blocks_manual_even_in_auto_mode(void)
{
	struct fixture fx;
	char response[M4_CTL_MAX_RESPONSE];

	fixture_setup(&fx);
	set_mode(&fx, "auto");
	touch_safe_latch(&fx);

	m4_daemon_handle_request(fx.d, "CALL ON", response, sizeof(response));
	CHECK(strncmp(response, "ERR", 3) == 0, "safe-latch: manual CALL ON rejected even in auto mode");
	CHECK(fx.fake.set_profile_calls == 0, "safe-latch: no graph write");

	fixture_teardown(&fx);
}

static void test_safe_latch_blocks_automode_even_in_auto_mode(void)
{
	struct fixture fx;

	fixture_setup(&fx);
	set_mode(&fx, "auto");
	touch_safe_latch(&fx);

	m4_daemon_mic_active_changed(fx.d, true, 0.0);
	m4_daemon_tick(fx.d, 1.0);
	CHECK(fx.fake.set_profile_calls == 0, "safe-latch: automode decision computed but not executed");

	fixture_teardown(&fx);
}

static void test_resume_unlatches(void)
{
	struct fixture fx;
	char path[256];
	char response[M4_CTL_MAX_RESPONSE];

	fixture_setup(&fx);
	set_mode(&fx, "auto");
	touch_safe_latch(&fx);

	m4_daemon_handle_request(fx.d, "CALL ON", response, sizeof(response));
	CHECK(fx.fake.set_profile_calls == 0, "still latched -- no write");

	/* `momentum4 resume` is a plain unlink() (cli/resume.c) -- simulate it
	 * directly rather than going through the CLI binary. */
	snprintf(path, sizeof(path), "%s/safe-latch", fx.runtime_dir);
	CHECK(unlink(path) == 0, "unlink safe-latch");

	m4_daemon_handle_request(fx.d, "CALL ON", response, sizeof(response));
	CHECK(strncmp(response, "OK", 2) == 0, "unlatched -- CALL ON now succeeds");
	CHECK(fx.fake.set_profile_calls == 1, "write happened after resume");

	fixture_teardown(&fx);
}

/* --- kill -9 semantics: destruction never writes ------------------------- */

static void test_free_does_not_touch_graph(void)
{
	struct fixture fx;

	fixture_setup(&fx);
	set_mode(&fx, "auto");
	m4_daemon_mic_active_changed(fx.d, true, 0.0);
	m4_daemon_tick(fx.d, 1.0);
	CHECK(fx.fake.set_profile_calls == 1, "sanity: one write happened before teardown");

	int calls_before_free = fx.fake.set_profile_calls;
	m4_daemon_free(fx.d);
	fx.d = NULL; /* fixture_teardown would double-free otherwise */
	CHECK(fx.fake.set_profile_calls == calls_before_free,
	      "destroying the daemon issues NO further graph writes (I2: kill -9 changes nothing)");

	sd_event_unref(fx.event);
}

/* --- Bug 1 fix: hold_changed governs exit, not entry -- */

static void test_hold_changed_never_arms_entry(void)
{
	struct fixture fx;

	fixture_setup(&fx);
	set_mode(&fx, "auto");

	m4_daemon_mic_hold_changed(fx.d, true, 0.0);
	m4_daemon_tick(fx.d, 1.0);
	CHECK(fx.fake.set_profile_calls == 0, "hold_active alone never starts a call (only mic_active does)");

	fixture_teardown(&fx);
}

static void test_hold_changed_exits_call_holder_still_present(void)
{
	struct fixture fx;

	fixture_setup(&fx);
	set_mode(&fx, "auto");

	/* mic_active true+stays true for the whole test (models signal-desktop
	 * never releasing the stream) -- entry via mic_active as usual. */
	m4_daemon_mic_active_changed(fx.d, true, 0.0);
	m4_daemon_tick(fx.d, 1.0);
	CHECK(fx.fake.set_profile_calls == 1, "entry via mic_active (holder alone is enough to enter)");
	CHECK(strcmp(fx.fake.profile_name, "headset-head-unit") == 0, "profile switched to mSBC");

	/* A real (non-holder) app joins, then leaves -- hold_active true then
	 * false, mic_active NEVER goes false (the holder is still there). */
	m4_daemon_mic_hold_changed(fx.d, true, 1.5);
	m4_daemon_mic_hold_changed(fx.d, false, 2.0);
	m4_daemon_tick(fx.d, 5.0); /* past off_debounce_s (2.5s default) from t=2.0 */

	CHECK(fx.fake.set_profile_calls == 2,
	      "hold_active going false (while mic_active stays true) still exits HFP");
	CHECK(strcmp(fx.fake.profile_name, "a2dp-sink") == 0, "profile switched back to A2DP");

	fixture_teardown(&fx);
}

/* --- Bug 1 gap fix: M4 reappearing --- */

static void test_reappear_retries_lost_attempt(void)
{
	struct fixture fx;

	fixture_setup(&fx);
	set_mode(&fx, "auto");

	/* mic goes active, but M4 disappears before the on-debounce fires --
	 * the eventual CALL_ON attempt fails (card_present()==0), rolling
	 * call_active back; automode's OWN mic_active stays true throughout
	 * (it is not "call state"). */
	m4_daemon_mic_active_changed(fx.d, true, 0.0);
	m4_graph_ops_fake_set_present(&fx.fake, false, false);
	m4_daemon_tick(fx.d, 1.0);
	CHECK(fx.fake.set_profile_calls == 0, "attempt failed -- M4 was absent, no write landed");

	struct m4_daemon_status st;
	m4_daemon_get_status(fx.d, &st);
	CHECK(!st.call_active, "call_active rolled back to false after the failed attempt");

	/* M4 reappears -- WITHOUT any new mic edge, the daemon must retry on
	 * its own (the fix) rather than waiting forever for a fresh start/stop
	 * that may never come. The edge is detected inside the NEXT
	 * m4_daemon_tick() call (not synchronously in set_present() -- see
	 * daemon.c's on_graph_state_changed()/m4_daemon_tick() comments on why
	 * it needs a real `now`), which re-arms a fresh on_debounce_s (0.3s
	 * default) from THAT tick's `now` -- so a second tick past the new
	 * deadline is needed before the retry actually fires. */
	m4_graph_ops_fake_set_present(&fx.fake, true, true);
	CHECK(fx.fake.set_profile_calls == 0, "no write yet -- present() alone does not act, only tick() does");
	m4_daemon_tick(fx.d, 2.0); /* detects the reappear edge here, arms deadline 2.3 */
	CHECK(fx.fake.set_profile_calls == 0, "reappear just armed a fresh on_debounce_s -- not due yet");
	m4_daemon_tick(fx.d, 2.5); /* past the freshly-armed deadline */
	CHECK(fx.fake.set_profile_calls == 1, "reappear alone re-armed and executed the retry");
	CHECK(strcmp(fx.fake.profile_name, "headset-head-unit") == 0, "profile switched to mSBC on retry");

	fixture_teardown(&fx);
}

static void test_reappear_is_noop_when_mic_not_active(void)
{
	struct fixture fx;

	fixture_setup(&fx);
	set_mode(&fx, "auto");

	/* No mic activity at all -- M4 disappearing and reappearing must not
	 * spuriously start a call. An intermediate tick() resyncs
	 * present_seen to false first, so the second tick() genuinely sees
	 * (and correctly no-ops on) a false->true edge, not just "never
	 * diverged in the first place". */
	m4_graph_ops_fake_set_present(&fx.fake, false, false);
	m4_daemon_tick(fx.d, 0.5);
	m4_graph_ops_fake_set_present(&fx.fake, true, true);
	m4_daemon_tick(fx.d, 1.0);
	CHECK(fx.fake.set_profile_calls == 0, "reappear with no prior mic activity does nothing");

	fixture_teardown(&fx);
}

/* --- ctl.sock STATUS/GET reflect internal state -------------------------- */

static void test_get_reflects_state(void)
{
	struct fixture fx;
	char response[M4_CTL_MAX_RESPONSE];
	char field[64];

	fixture_setup(&fx);
	set_mode(&fx, "manual");

	m4_daemon_handle_request(fx.d, "GET", response, sizeof(response));
	CHECK(strncmp(response, "OK", 2) == 0, "GET ok");
	CHECK(m4_ctl_response_field(response, "mode", field, sizeof(field)) &&
		      strcmp(field, "manual") == 0,
	      "GET reflects mode");
	CHECK(m4_ctl_response_field(response, "m4_present", field, sizeof(field)) &&
		      strcmp(field, "true") == 0,
	      "GET reflects m4_present from the fake");
	CHECK(m4_ctl_response_field(response, "safe_latched", field, sizeof(field)) &&
		      strcmp(field, "false") == 0,
	      "GET reflects safe_latched=false");

	fixture_teardown(&fx);
}

int main(void)
{
	test_default_mode_is_observe();
	test_observe_blocks_manual_call();
	test_observe_blocks_automode_call();
	test_manual_mode_allows_manual_call();
	test_manual_mode_blocks_automode_call();
	test_auto_mode_allows_automode_call();
	test_auto_mode_allows_manual_call_too();
	test_safe_latch_blocks_manual_even_in_auto_mode();
	test_safe_latch_blocks_automode_even_in_auto_mode();
	test_resume_unlatches();
	test_free_does_not_touch_graph();
	test_hold_changed_never_arms_entry();
	test_hold_changed_exits_call_holder_still_present();
	test_reappear_retries_lost_attempt();
	test_reappear_is_noop_when_mic_not_active();
	test_get_reflects_state();

	if (failures > 0) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("daemon: all checks passed (write-gating matrix, safe-latch, resume, kill-semantics)\n");
	return 0;
}
