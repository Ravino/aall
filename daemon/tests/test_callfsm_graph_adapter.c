/* Unit tests for pw/callfsm_graph_adapter.c: the thin
 * synchronous m4_callfsm_ops_t built on top of struct graph_ops, exercised
 * against core/graph_ops_fake.c so no live PipeWire connection is needed.
 * Uses a real sd_event (needed by the adapter's set_profile_confirm() wait
 * loop) and a throwaway mkdtemp() runtime dir for call-state.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <systemd/sd-event.h>

#include "bt/bt_ops_fake.h"
#include "core/graph_ops_fake.h"
#include "core/state.h"
#include "pw/callfsm_graph_adapter.h"

static int failures = 0;

#define CHECK(cond, msg) \
	do { \
		if (!(cond)) { \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, (msg)); \
			failures++; \
		} \
	} while (0)

static void make_tmpdir(char *out, size_t out_len)
{
	snprintf(out, out_len, "/tmp/m4d-test-adapter.XXXXXX");
	CHECK(mkdtemp(out) != NULL, "mkdtemp");
}

struct fixture {
	sd_event *event;
	m4_graph_ops_fake_t fake;
	void *graph_handle;
	char runtime_dir[128];
	struct m4_callfsm_graph_adapter *adapter;
	m4_callfsm_ops_t ops;
};

/* mic-on-enter: `mic_cfg` NULL preserves this file's pre-existing
 * fixture_setup() behaviour byte-for-byte (M4_MIC_ON_ENTER_ON, callfsm_graph_
 * adapter.c's own doc comment on why NULL is a safe default). */
static void fixture_setup_mic(struct fixture *fx, int confirm_timeout_ms,
			       const struct m4_callfsm_graph_adapter_mic_config *mic_cfg)
{
	const struct graph_ops *vtable = m4_graph_ops_fake(&fx->fake);

	CHECK(sd_event_new(&fx->event) >= 0, "sd_event_new");
	m4_graph_ops_fake_init(&fx->fake);
	fx->graph_handle = vtable->open(fx->event, "00:1B:66:00:00:01", NULL, NULL);
	CHECK(fx->graph_handle != NULL, "fake graph open()");
	make_tmpdir(fx->runtime_dir, sizeof(fx->runtime_dir));

	fx->adapter = m4_callfsm_graph_adapter_new(fx->event, vtable, fx->graph_handle,
						    fx->runtime_dir, "", confirm_timeout_ms, NULL, mic_cfg);
	CHECK(fx->adapter != NULL, "adapter_new");
	fx->ops = m4_callfsm_graph_adapter_ops(fx->adapter);
}

static void fixture_setup(struct fixture *fx, int confirm_timeout_ms)
{
	fixture_setup_mic(fx, confirm_timeout_ms, NULL);
}

static void fixture_teardown(struct fixture *fx)
{
	m4_callfsm_graph_adapter_free(fx->adapter);
	m4_graph_ops_fake(&fx->fake)->close(fx->graph_handle);
	sd_event_unref(fx->event);
}

static void test_card_present_and_active_profile(void)
{
	struct fixture fx;
	char card[64], profile[64];

	fixture_setup(&fx, 200);

	CHECK(fx.ops.card_present(fx.ops.ctx, card, sizeof(card)) == 1, "card present");
	CHECK(fx.ops.get_active_profile(fx.ops.ctx, card, profile, sizeof(profile)) == 0, "get profile ok");
	CHECK(strcmp(profile, "a2dp-sink") == 0, "initial profile a2dp-sink");

	fixture_teardown(&fx);
}

static void test_card_absent(void)
{
	struct fixture fx;
	char card[64] = "unset";

	fixture_setup(&fx, 200);
	m4_graph_ops_fake_set_present(&fx.fake, false, false);

	CHECK(fx.ops.card_present(fx.ops.ctx, card, sizeof(card)) == 0, "card absent -> 0");

	fixture_teardown(&fx);
}

static void test_set_profile_confirm_success(void)
{
	struct fixture fx;
	char card[64];

	fixture_setup(&fx, 200);
	fx.ops.card_present(fx.ops.ctx, card, sizeof(card));

	CHECK(fx.ops.set_profile_confirm(fx.ops.ctx, card, "headset-head-unit") == 1,
	      "confirm succeeds (fake confirms synchronously)");
	CHECK(fx.fake.set_profile_calls == 1, "set_profile called once");
	CHECK(strcmp(fx.fake.last_set_profile_name, "headset-head-unit") == 0, "correct profile requested");
	CHECK(fx.fake.last_set_profile_save == true, "save=true always");
	CHECK(strcmp(fx.fake.profile_name, "headset-head-unit") == 0, "fake state actually updated");

	char profile[64];
	fx.ops.get_active_profile(fx.ops.ctx, card, profile, sizeof(profile));
	CHECK(strcmp(profile, "headset-head-unit") == 0, "get_active_profile reflects the change");

	fixture_teardown(&fx);
}

static void test_set_profile_confirm_no_such_profile(void)
{
	struct fixture fx;
	char card[64];

	fixture_setup(&fx, 200);
	fx.ops.card_present(fx.ops.ctx, card, sizeof(card));

	CHECK(fx.ops.set_profile_confirm(fx.ops.ctx, card, "does-not-exist") == 0,
	      "unknown profile name fails immediately");
	CHECK(strcmp(fx.fake.profile_name, "a2dp-sink") == 0, "profile unchanged");

	fixture_teardown(&fx);
}

/* fail_next_set_profile: the fake "issues" the request (mirrors a real
 * PipeWire fire-and-forget accept) but never confirms it -- the adapter
 * must give up after confirm_timeout_ms, not hang. A short timeout keeps
 * this test fast; the real daemon uses a multi-second timeout
 * (the default confirm_timeout_ms). */
static void test_set_profile_confirm_timeout(void)
{
	struct fixture fx;
	char card[64];
	struct timespec t0, t1;
	double elapsed_s;

	fixture_setup(&fx, 80 /* ms */);
	fx.ops.card_present(fx.ops.ctx, card, sizeof(card));
	fx.fake.fail_next_set_profile = true;

	clock_gettime(CLOCK_MONOTONIC, &t0);
	CHECK(fx.ops.set_profile_confirm(fx.ops.ctx, card, "headset-head-unit") == 0,
	      "never-confirmed request times out, returns 0");
	clock_gettime(CLOCK_MONOTONIC, &t1);
	elapsed_s = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
	CHECK(elapsed_s < 2.0, "bounded by confirm_timeout_ms, not an infinite loop");
	CHECK(strcmp(fx.fake.profile_name, "a2dp-sink") == 0, "profile unchanged after a timeout");

	fixture_teardown(&fx);
}

static void test_state_roundtrip_through_adapter(void)
{
	struct fixture fx;
	char profile[64], reason[16];

	fixture_setup(&fx, 200);

	CHECK(fx.ops.state_present(fx.ops.ctx) == 0, "no state initially");
	CHECK(fx.ops.state_write(fx.ops.ctx, "a2dp-sink", "manual") == 0, "state_write ok");
	CHECK(fx.ops.state_present(fx.ops.ctx) == 1, "state present after write");
	CHECK(fx.ops.state_read(fx.ops.ctx, profile, sizeof(profile), reason, sizeof(reason)) == 0,
	      "state_read ok");
	CHECK(strcmp(profile, "a2dp-sink") == 0, "profile roundtrip");
	CHECK(strcmp(reason, "manual") == 0, "reason roundtrip");
	fx.ops.state_remove(fx.ops.ctx);
	CHECK(fx.ops.state_present(fx.ops.ctx) == 0, "state gone after remove");

	fixture_teardown(&fx);
}

static void test_helper_alive_no_stats_file(void)
{
	struct fixture fx;

	fixture_setup(&fx, 200);
	CHECK(fx.ops.helper_alive(fx.ops.ctx) == 0, "no stats.json -> not alive");
	fixture_teardown(&fx);
}

static void test_helper_alive_from_stats_file(void)
{
	sd_event *event;
	m4_graph_ops_fake_t fake;
	const struct graph_ops *vtable = m4_graph_ops_fake(&fake);
	void *handle;
	char runtime_dir[128], stats_path[192];
	struct m4_callfsm_graph_adapter *adapter;
	m4_callfsm_ops_t ops;
	FILE *f;

	CHECK(sd_event_new(&event) >= 0, "sd_event_new");
	m4_graph_ops_fake_init(&fake);
	handle = vtable->open(event, "00:1B:66:00:00:01", NULL, NULL);
	make_tmpdir(runtime_dir, sizeof(runtime_dir));
	snprintf(stats_path, sizeof(stats_path), "%s/stats.json", runtime_dir);

	f = fopen(stats_path, "w");
	CHECK(f != NULL, "create stats.json");
	fputs("{\"alive\":true}", f);
	fclose(f);

	adapter = m4_callfsm_graph_adapter_new(event, vtable, handle, runtime_dir, stats_path, 200, NULL, NULL);
	ops = m4_callfsm_graph_adapter_ops(adapter);
	CHECK(ops.helper_alive(ops.ctx) == 1, "alive:true in stats.json -> helper_alive()==1");

	m4_callfsm_graph_adapter_free(adapter);
	vtable->close(handle);
	sd_event_unref(event);
}

/* --- repair-a2dp - */

static void remove_profile_by_name(m4_graph_ops_fake_t *fake, const char *name)
{
	size_t w = 0;

	for (size_t i = 0; i < fake->n_profiles; i++) {
		if (strcmp(fake->profiles[i].name, name) != 0)
			fake->profiles[w++] = fake->profiles[i];
	}
	fake->n_profiles = w;
}

static void add_profile(m4_graph_ops_fake_t *fake, uint32_t index, const char *name)
{
	struct m4_graph_profile *p = &fake->profiles[fake->n_profiles++];

	memset(p, 0, sizeof(*p));
	p->index = index;
	snprintf(p->name, sizeof(p->name), "%s", name);
	p->available = true;
}

/* Simulates BlueZ's ConnectProfile eventually landing (a2dp-sink reappears
 * in EnumProfile) WHILE the adapter's repair path is pumping the shared
 * sd_event waiting for exactly that -- armed as a one-shot timer on the
 * SAME event loop, the same technique test_set_profile_confirm_timeout()
 * above relies on for the "never resolves" direction. */
static int on_reappear_timer(sd_event_source *s, uint64_t usec, void *userdata)
{
	m4_graph_ops_fake_t *fake = userdata;

	(void)s;
	(void)usec;
	add_profile(fake, 0, "a2dp-sink");
	return 0;
}

static void test_repair_disabled_returns_zero_without_bt_call(void)
{
	struct fixture fx;
	char card[64];

	fixture_setup(&fx, 200); /* repair_cfg == NULL inside fixture_setup() -- repair disabled */
	fx.ops.card_present(fx.ops.ctx, card, sizeof(card));
	remove_profile_by_name(&fx.fake, "a2dp-sink");

	CHECK(fx.ops.repair_profile_if_missing != NULL, "adapter always provides the function pointer");
	CHECK(fx.ops.repair_profile_if_missing(fx.ops.ctx, card, "a2dp-sink") == 0,
	      "repair disabled at construction (no repair_cfg) -> always 0, no crash");

	fixture_teardown(&fx);
}

static void test_repair_not_needed_is_noop(void)
{
	sd_event *event;
	m4_graph_ops_fake_t fake;
	m4_bt_ops_fake_t bt_fake;
	const struct graph_ops *vtable = m4_graph_ops_fake(&fake);
	void *handle;
	char runtime_dir[128];
	struct m4_callfsm_graph_adapter_repair_config rc = {0};
	struct m4_callfsm_graph_adapter *adapter;
	m4_callfsm_ops_t ops;

	CHECK(sd_event_new(&event) >= 0, "sd_event_new");
	m4_graph_ops_fake_init(&fake);
	m4_bt_ops_fake_init(&bt_fake);
	handle = vtable->open(event, "00:1B:66:00:00:01", NULL, NULL);
	make_tmpdir(runtime_dir, sizeof(runtime_dir));

	rc.bt_ops = m4_bt_ops_fake(&bt_fake);
	rc.mac_address = "00:1B:66:00:00:01";
	rc.bt_adapter = "hci0";
	rc.enum_wait_ms = 500;
	rc.connect_timeout_ms = 500;

	adapter = m4_callfsm_graph_adapter_new(event, vtable, handle, runtime_dir, "", 200, &rc, NULL);
	ops = m4_callfsm_graph_adapter_ops(adapter);

	CHECK(ops.repair_profile_if_missing(ops.ctx, "m4", "a2dp-sink") == 0,
	      "a2dp-sink already present in EnumProfile -- repair is a fast no-op");
	CHECK(bt_fake.connect_profile_calls == 0, "ConnectProfile NEVER called when not actually missing");

	m4_callfsm_graph_adapter_free(adapter);
	vtable->close(handle);
	sd_event_unref(event);
}

static void test_repair_missing_succeeds(void)
{
	sd_event *event;
	m4_graph_ops_fake_t fake;
	m4_bt_ops_fake_t bt_fake;
	const struct graph_ops *vtable = m4_graph_ops_fake(&fake);
	void *handle;
	char runtime_dir[128];
	struct m4_callfsm_graph_adapter_repair_config rc = {0};
	struct m4_callfsm_graph_adapter *adapter;
	m4_callfsm_ops_t ops;
	sd_event_source *timer = NULL;

	CHECK(sd_event_new(&event) >= 0, "sd_event_new");
	m4_graph_ops_fake_init(&fake);
	m4_bt_ops_fake_init(&bt_fake);
	handle = vtable->open(event, "00:1B:66:00:00:01", NULL, NULL);
	make_tmpdir(runtime_dir, sizeof(runtime_dir));
	remove_profile_by_name(&fake, "a2dp-sink");
	m4_graph_ops_fake_set_profile_external(&fake, "headset-head-unit");

	rc.bt_ops = m4_bt_ops_fake(&bt_fake);
	rc.mac_address = "00:1B:66:00:00:01";
	rc.bt_adapter = "hci0";
	rc.enum_wait_ms = 2000;
	rc.connect_timeout_ms = 500;

	adapter = m4_callfsm_graph_adapter_new(event, vtable, handle, runtime_dir, "", 500, &rc, NULL);
	ops = m4_callfsm_graph_adapter_ops(adapter);

	CHECK(sd_event_add_time_relative(event, &timer, CLOCK_MONOTONIC, 20 * 1000, 1000,
					  on_reappear_timer, &fake) >= 0,
	      "arm reappear timer");

	CHECK(ops.repair_profile_if_missing(ops.ctx, "m4", "a2dp-sink") == 1,
	      "repair succeeds once EnumProfile catches up (via the armed timer)");
	CHECK(bt_fake.connect_profile_calls == 1, "ConnectProfile called exactly once");
	CHECK(strcmp(bt_fake.last_uuid, M4_BT_UUID_A2DP_SINK) == 0, "correct UUID (A2DP Sink)");
	CHECK(strcmp(bt_fake.last_mac_address, "00:1B:66:00:00:01") == 0, "correct MAC passed through");
	CHECK(strcmp(fake.profile_name, "a2dp-sink") == 0, "fake profile actually switched to a2dp-sink");

	if (timer != NULL)
		sd_event_source_unref(timer);
	m4_callfsm_graph_adapter_free(adapter);
	vtable->close(handle);
	sd_event_unref(event);
}

static void test_repair_enum_timeout(void)
{
	sd_event *event;
	m4_graph_ops_fake_t fake;
	m4_bt_ops_fake_t bt_fake;
	const struct graph_ops *vtable = m4_graph_ops_fake(&fake);
	void *handle;
	char runtime_dir[128];
	struct m4_callfsm_graph_adapter_repair_config rc = {0};
	struct m4_callfsm_graph_adapter *adapter;
	m4_callfsm_ops_t ops;
	struct timespec t0, t1;
	double elapsed_s;

	CHECK(sd_event_new(&event) >= 0, "sd_event_new");
	m4_graph_ops_fake_init(&fake);
	m4_bt_ops_fake_init(&bt_fake);
	handle = vtable->open(event, "00:1B:66:00:00:01", NULL, NULL);
	make_tmpdir(runtime_dir, sizeof(runtime_dir));
	remove_profile_by_name(&fake, "a2dp-sink"); /* nothing ever re-adds it -- must time out, not hang */

	rc.bt_ops = m4_bt_ops_fake(&bt_fake);
	rc.mac_address = "00:1B:66:00:00:01";
	rc.enum_wait_ms = 80;
	rc.connect_timeout_ms = 500;

	adapter = m4_callfsm_graph_adapter_new(event, vtable, handle, runtime_dir, "", 200, &rc, NULL);
	ops = m4_callfsm_graph_adapter_ops(adapter);

	clock_gettime(CLOCK_MONOTONIC, &t0);
	CHECK(ops.repair_profile_if_missing(ops.ctx, "m4", "a2dp-sink") == 0,
	      "a2dp-sink never reappears -- repair gives up, bounded by enum_wait_ms");
	clock_gettime(CLOCK_MONOTONIC, &t1);
	elapsed_s = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
	CHECK(elapsed_s < 2.0, "bounded, not an infinite loop");
	CHECK(bt_fake.connect_profile_calls == 1, "ConnectProfile still attempted exactly once");

	m4_callfsm_graph_adapter_free(adapter);
	vtable->close(handle);
	sd_event_unref(event);
}

/* --- "mic-on-enter" task (`[call] mic-on-enter`) --------------------------- */

static void test_mic_on_enter_on_is_unchanged_default(void)
{
	struct fixture fx;
	char card[64];

	/* NULL mic_cfg -- must behave EXACTLY like the pre-existing "Bug 2"
	 * unconditional-unmute fix (see callfsm_graph_adapter.h's own doc
	 * comment on why NULL means M4_MIC_ON_ENTER_ON). */
	fixture_setup_mic(&fx, 200, NULL);
	fx.ops.card_present(fx.ops.ctx, card, sizeof(card));
	m4_graph_ops_fake_set_mic_route(&fx.fake, true, 0.0f, true);

	CHECK(fx.ops.fix_mic_volume_if_needed(fx.ops.ctx, card) == 1, "on: muted route gets fixed to unmuted");
	CHECK(fx.fake.mic_volume >= 0.99f, "volume 100%");
	CHECK(!fx.fake.mic_mute, "not muted");
	CHECK(fx.ops.fix_mic_volume_if_needed(fx.ops.ctx, card) == 0, "on: already unmuted -- no-op, no write");
	CHECK(fx.fake.set_mic_volume_calls == 1, "exactly one write");

	fixture_teardown(&fx);
}

static void test_mic_on_enter_off_always_mutes(void)
{
	struct fixture fx;
	char card[64];
	struct m4_callfsm_graph_adapter_mic_config mic_cfg = {0};

	mic_cfg.mode = M4_MIC_ON_ENTER_OFF;
	fixture_setup_mic(&fx, 200, &mic_cfg);
	fx.ops.card_present(fx.ops.ctx, card, sizeof(card));
	m4_graph_ops_fake_set_mic_route(&fx.fake, true, 1.0f, false); /* M4 reports full gain */

	CHECK(fx.ops.fix_mic_volume_if_needed(fx.ops.ctx, card) == 1, "off: unmuted route gets muted");
	CHECK(fx.fake.mic_volume <= 0.02f, "volume 0%");
	CHECK(fx.fake.mic_mute, "muted");
	CHECK(fx.ops.fix_mic_volume_if_needed(fx.ops.ctx, card) == 0, "off: already muted -- no-op, no write");
	CHECK(fx.fake.set_mic_volume_calls == 1, "exactly one write");

	fixture_teardown(&fx);
}

static void test_mic_on_enter_remember_no_prior_state_means_on(void)
{
	struct fixture fx;
	char card[64];
	char state_dir[128];
	struct m4_callfsm_graph_adapter_mic_config mic_cfg = {0};

	make_tmpdir(state_dir, sizeof(state_dir));
	mic_cfg.mode = M4_MIC_ON_ENTER_REMEMBER;
	mic_cfg.state_dir = state_dir;
	fixture_setup_mic(&fx, 200, &mic_cfg);
	fx.ops.card_present(fx.ops.ctx, card, sizeof(card));
	m4_graph_ops_fake_set_mic_route(&fx.fake, true, 0.0f, true);

	CHECK(fx.ops.fix_mic_volume_if_needed(fx.ops.ctx, card) == 1,
	      "remember, never recorded -- defaults to \"on\" (mic on)");
	CHECK(fx.fake.mic_volume >= 0.99f, "volume 100%");

	fixture_teardown(&fx);
}

static void test_mic_on_enter_remember_restores_muted(void)
{
	struct fixture fx;
	char card[64];
	char state_dir[128];
	struct m4_callfsm_graph_adapter_mic_config mic_cfg = {0};

	make_tmpdir(state_dir, sizeof(state_dir));
	mic_cfg.mode = M4_MIC_ON_ENTER_REMEMBER;
	mic_cfg.state_dir = state_dir;
	fixture_setup_mic(&fx, 200, &mic_cfg);
	CHECK(m4_mic_state_write(fx.runtime_dir, state_dir, true) == 0,
	      "seed \"last exit was muted\" (as of the last HFP exit)");
	fx.ops.card_present(fx.ops.ctx, card, sizeof(card));
	m4_graph_ops_fake_set_mic_route(&fx.fake, true, 1.0f, false);

	CHECK(fx.ops.fix_mic_volume_if_needed(fx.ops.ctx, card) == 1, "remember restores muted");
	CHECK(fx.fake.mic_volume <= 0.02f, "volume 0%");
	CHECK(fx.fake.mic_mute, "muted");

	fixture_teardown(&fx);
}

/* Stored in $XDG_RUNTIME_DIR/momentum4 and in
 * ~/.local/state/momentum4, to survive a daemon restart -- models a
 * daemon restart as freeing the adapter and constructing a brand new one
 * against the SAME runtime_dir/state_dir (exactly what m4_daemon_new() does
 * on a real restart, core/state.c's own files being the only thing that
 * survives it). */
static void test_mic_on_enter_remember_survives_adapter_recreate(void)
{
	sd_event *event;
	m4_graph_ops_fake_t fake;
	const struct graph_ops *vtable = m4_graph_ops_fake(&fake);
	void *handle;
	char runtime_dir[128], state_dir[128], card[64];
	struct m4_callfsm_graph_adapter_mic_config mic_cfg = {0};
	struct m4_callfsm_graph_adapter *adapter;
	m4_callfsm_ops_t ops;

	CHECK(sd_event_new(&event) >= 0, "sd_event_new");
	m4_graph_ops_fake_init(&fake);
	handle = vtable->open(event, "00:1B:66:00:00:01", NULL, NULL);
	make_tmpdir(runtime_dir, sizeof(runtime_dir));
	make_tmpdir(state_dir, sizeof(state_dir));

	mic_cfg.mode = M4_MIC_ON_ENTER_REMEMBER;
	mic_cfg.state_dir = state_dir;
	adapter = m4_callfsm_graph_adapter_new(event, vtable, handle, runtime_dir, "", 200, NULL, &mic_cfg);
	ops = m4_callfsm_graph_adapter_ops(adapter);
	ops.card_present(ops.ctx, card, sizeof(card));
	m4_graph_ops_fake_set_mic_route(&fake, true, 1.0f, false);
	CHECK(ops.fix_mic_volume_if_needed(ops.ctx, card) == 0,
	      "remember, no prior state -- already unmuted, nothing to do");

	/* "HFP exit" (task: capture happens in daemon.c, not this adapter --
	 * modelled here directly with the same core/state.h call daemon.c
	 * itself uses). */
	CHECK(m4_mic_state_write(runtime_dir, state_dir, true) == 0, "persist muted at exit");
	m4_callfsm_graph_adapter_free(adapter); /* "daemon restart" */

	/* only state_dir survives (models a REBOOT, not just a daemon
	 * restart -- $XDG_RUNTIME_DIR is typically tmpfs): wipe runtime_dir's
	 * own copy first, so this specifically exercises the state_dir
	 * fallback half of m4_mic_state_read()'s own doc comment. */
	{
		char path[300];

		snprintf(path, sizeof(path), "%s/mic-last-state", runtime_dir);
		unlink(path);
	}

	adapter = m4_callfsm_graph_adapter_new(event, vtable, handle, runtime_dir, "", 200, NULL, &mic_cfg);
	ops = m4_callfsm_graph_adapter_ops(adapter);
	m4_graph_ops_fake_set_mic_route(&fake, true, 1.0f, false); /* M4 reconnected unmuted */
	CHECK(ops.fix_mic_volume_if_needed(ops.ctx, card) == 1,
	      "new adapter instance, only state_dir has the record -- still restores muted");
	CHECK(fake.mic_volume <= 0.02f, "volume 0% (survived \"reboot\")");

	m4_callfsm_graph_adapter_free(adapter);
	vtable->close(handle);
	sd_event_unref(event);
}

static void test_mic_on_enter_reload_setter(void)
{
	struct fixture fx;
	char card[64];
	struct m4_callfsm_graph_adapter_mic_config mic_cfg = {0};

	mic_cfg.mode = M4_MIC_ON_ENTER_ON;
	fixture_setup_mic(&fx, 200, &mic_cfg);
	fx.ops.card_present(fx.ops.ctx, card, sizeof(card));
	m4_graph_ops_fake_set_mic_route(&fx.fake, true, 1.0f, false);

	CHECK(fx.ops.fix_mic_volume_if_needed(fx.ops.ctx, card) == 0, "on: already unmuted, no-op");

	m4_callfsm_graph_adapter_set_mic_on_enter(fx.adapter, M4_MIC_ON_ENTER_OFF);
	CHECK(fx.ops.fix_mic_volume_if_needed(fx.ops.ctx, card) == 1,
	      "`momentum4 reload`-equivalent setter takes effect immediately, without recreating the adapter");
	CHECK(fx.fake.mic_volume <= 0.02f, "now muted");

	fixture_teardown(&fx);
}

int main(void)
{
	test_card_present_and_active_profile();
	test_card_absent();
	test_set_profile_confirm_success();
	test_set_profile_confirm_no_such_profile();
	test_set_profile_confirm_timeout();
	test_state_roundtrip_through_adapter();
	test_helper_alive_no_stats_file();
	test_helper_alive_from_stats_file();
	test_repair_disabled_returns_zero_without_bt_call();
	test_repair_not_needed_is_noop();
	test_repair_missing_succeeds();
	test_repair_enum_timeout();
	test_mic_on_enter_on_is_unchanged_default();
	test_mic_on_enter_off_always_mutes();
	test_mic_on_enter_remember_no_prior_state_means_on();
	test_mic_on_enter_remember_restores_muted();
	test_mic_on_enter_remember_survives_adapter_recreate();
	test_mic_on_enter_reload_setter();

	if (failures > 0) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("callfsm_graph_adapter: all checks passed\n");
	return 0;
}
