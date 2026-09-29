/* Unit tests for pw/repair_a2dp.c against core/graph_ops_fake.c +
 * bt/bt_ops_fake.c ("repair-a2dp and graph resilience"
 * task) -- no live PipeWire, no real D-Bus/system bus.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <systemd/sd-event.h>

#include "bt/bt_ops_fake.h"
#include "core/graph_ops_fake.h"
#include "pw/repair_a2dp.h"

static int failures = 0;

#define CHECK(cond, msg) \
	do { \
		if (!(cond)) { \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, (msg)); \
			failures++; \
		} \
	} while (0)

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

struct fixture {
	sd_event *event;
	m4_graph_ops_fake_t fake;
	m4_bt_ops_fake_t bt_fake;
	void *handle;
};

static void fixture_setup(struct fixture *fx)
{
	const struct graph_ops *vtable = m4_graph_ops_fake(&fx->fake);

	CHECK(sd_event_new(&fx->event) >= 0, "sd_event_new");
	m4_graph_ops_fake_init(&fx->fake);
	m4_bt_ops_fake_init(&fx->bt_fake);
	fx->handle = vtable->open(fx->event, "00:1B:66:00:00:01", NULL, NULL);
	CHECK(fx->handle != NULL, "fake graph open()");
}

static void fixture_teardown(struct fixture *fx)
{
	m4_graph_ops_fake(&fx->fake)->close(fx->handle);
	sd_event_unref(fx->event);
}

static struct m4_repair_a2dp_config base_config(struct fixture *fx)
{
	struct m4_repair_a2dp_config cfg = {0};

	cfg.graph_ops = m4_graph_ops_fake(&fx->fake);
	cfg.graph_handle = fx->handle;
	cfg.bt_ops = m4_bt_ops_fake(&fx->bt_fake);
	cfg.event = fx->event;
	cfg.mac_address = "00:1B:66:00:00:01";
	cfg.bt_adapter = "hci0";
	cfg.connect_timeout_ms = 500;
	cfg.enum_wait_ms = 500;
	cfg.confirm_timeout_ms = 500;
	return cfg;
}

/* M4 not present at all -- must fail fast, no ConnectProfile attempted. */
static void test_not_present(void)
{
	struct fixture fx;
	struct m4_repair_a2dp_result r;

	fixture_setup(&fx);
	m4_graph_ops_fake_set_present(&fx.fake, false, false);

	struct m4_repair_a2dp_config cfg = base_config(&fx);
	r = m4_repair_a2dp_run(&cfg);

	CHECK(r.status == M4_REPAIR_A2DP_ERR_NOT_PRESENT, "status: not present");
	CHECK(fx.bt_fake.connect_profile_calls == 0, "ConnectProfile never attempted -- M4 not even present");

	fixture_teardown(&fx);
}

/* a2dp-sink already present and active -- ConnectProfile is still called
 * (task text: no "already fine, skip" special case), but everything
 * resolves on the very first poll, fast. */
static void test_already_fine(void)
{
	struct fixture fx;
	struct m4_repair_a2dp_result r;

	fixture_setup(&fx);

	struct m4_repair_a2dp_config cfg = base_config(&fx);
	r = m4_repair_a2dp_run(&cfg);

	CHECK(r.status == M4_REPAIR_A2DP_OK, "status: OK");
	CHECK(fx.bt_fake.connect_profile_calls == 1, "ConnectProfile called once (the required sequence)");
	CHECK(strcmp(fx.bt_fake.last_uuid, M4_BT_UUID_A2DP_SINK) == 0, "correct UUID");
	CHECK(strcmp(fx.fake.profile_name, "a2dp-sink") == 0, "profile is (still) a2dp-sink");

	fixture_teardown(&fx);
}

/* a2dp-sink missing from EnumProfile, ConnectProfile succeeds, and a timer
 * simulates BlueZ making it reappear shortly after -- full happy path. */
static int on_reappear_timer(sd_event_source *s, uint64_t usec, void *userdata)
{
	m4_graph_ops_fake_t *fake = userdata;

	(void)s;
	(void)usec;
	add_profile(fake, 0, "a2dp-sink");
	return 0;
}

static void test_missing_then_reappears(void)
{
	struct fixture fx;
	struct m4_repair_a2dp_result r;
	sd_event_source *timer = NULL;

	fixture_setup(&fx);
	remove_profile_by_name(&fx.fake, "a2dp-sink");
	m4_graph_ops_fake_set_profile_external(&fx.fake, "headset-head-unit");

	CHECK(sd_event_add_time_relative(fx.event, &timer, CLOCK_MONOTONIC, 20 * 1000, 1000,
					  on_reappear_timer, &fx.fake) >= 0,
	      "arm reappear timer");

	struct m4_repair_a2dp_config cfg = base_config(&fx);
	cfg.enum_wait_ms = 2000;
	r = m4_repair_a2dp_run(&cfg);

	CHECK(r.status == M4_REPAIR_A2DP_OK, "status: OK");
	CHECK(fx.bt_fake.connect_profile_calls == 1, "ConnectProfile called exactly once");
	CHECK(strcmp(fx.fake.profile_name, "a2dp-sink") == 0, "profile switched to a2dp-sink");
	CHECK(r.enum_wait_elapsed_s > 0.0, "enum_wait_elapsed_s recorded (timing, item 3)");
	CHECK(r.confirm_elapsed_s >= 0.0, "confirm_elapsed_s recorded (timing, item 3)");

	if (timer != NULL)
		sd_event_source_unref(timer);
	fixture_teardown(&fx);
}

/* a2dp-sink never reappears -- must time out, bounded by enum_wait_ms, not
 * hang, regardless of what ConnectProfile itself returned. */
static void test_enum_timeout(void)
{
	struct fixture fx;
	struct m4_repair_a2dp_result r;
	struct timespec t0, t1;
	double elapsed_s;

	fixture_setup(&fx);
	remove_profile_by_name(&fx.fake, "a2dp-sink");

	struct m4_repair_a2dp_config cfg = base_config(&fx);
	cfg.enum_wait_ms = 80;

	clock_gettime(CLOCK_MONOTONIC, &t0);
	r = m4_repair_a2dp_run(&cfg);
	clock_gettime(CLOCK_MONOTONIC, &t1);
	elapsed_s = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;

	CHECK(r.status == M4_REPAIR_A2DP_ERR_ENUM_TIMEOUT, "status: enum timeout");
	CHECK(elapsed_s < 2.0, "bounded, not an infinite loop");
	CHECK(fx.bt_fake.connect_profile_calls == 1, "ConnectProfile still attempted once");

	fixture_teardown(&fx);
}

/* ConnectProfile itself errors (e.g. device busy), but a2dp-sink still
 * reappears independently shortly after (BlueZ's own retry, or a concurrent
 * external fix) -- overall result must still be success: this file does not
 * treat a ConnectProfile error as automatically fatal, see repair_a2dp.c's
 * top comment. */
static void test_connect_profile_errors_but_recovers_anyway(void)
{
	struct fixture fx;
	struct m4_repair_a2dp_result r;
	sd_event_source *timer = NULL;

	fixture_setup(&fx);
	remove_profile_by_name(&fx.fake, "a2dp-sink");
	m4_graph_ops_fake_set_profile_external(&fx.fake, "headset-head-unit");
	fx.bt_fake.result = -1;
	snprintf(fx.bt_fake.result_message, sizeof(fx.bt_fake.result_message), "org.bluez.Error.InProgress");

	CHECK(sd_event_add_time_relative(fx.event, &timer, CLOCK_MONOTONIC, 20 * 1000, 1000,
					  on_reappear_timer, &fx.fake) >= 0,
	      "arm reappear timer");

	struct m4_repair_a2dp_config cfg = base_config(&fx);
	cfg.enum_wait_ms = 2000;
	r = m4_repair_a2dp_run(&cfg);

	CHECK(r.status == M4_REPAIR_A2DP_OK, "status: OK despite ConnectProfile itself erroring");
	CHECK(r.connect_profile_failed == 1, "observability: ConnectProfile error is recorded");

	if (timer != NULL)
		sd_event_source_unref(timer);
	fixture_teardown(&fx);
}

/* set_profile() succeeds (issued) but never actually confirms -- bounded by
 * confirm_timeout_ms, not the enum wait. Models the fake's own
 * fail_next_set_profile equivalent: here, remove the profile from
 * EnumProfile again right as set_profile() would look it up -- simplest way
 * to make graph_ops_fake's set_profile() itself fail structurally after the
 * enum wait already succeeded once. */
static void test_set_profile_confirm_timeout(void)
{
	struct fixture fx;
	struct m4_repair_a2dp_result r;
	struct timespec t0, t1;
	double elapsed_s;

	fixture_setup(&fx);
	/* a2dp-sink present in EnumProfile (so the enum-wait phase succeeds
	 * immediately) but the fake's ACTIVE profile is something else (must
	 * NOT default to "a2dp-sink" already, or the confirm phase would
	 * trivially pass without ever exercising set_profile() at all), and
	 * the NEXT set_profile() call is armed to fail to confirm. */
	m4_graph_ops_fake_set_profile_external(&fx.fake, "headset-head-unit");
	fx.fake.fail_next_set_profile = true;

	struct m4_repair_a2dp_config cfg = base_config(&fx);
	cfg.confirm_timeout_ms = 80;

	clock_gettime(CLOCK_MONOTONIC, &t0);
	r = m4_repair_a2dp_run(&cfg);
	clock_gettime(CLOCK_MONOTONIC, &t1);
	elapsed_s = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;

	CHECK(r.status == M4_REPAIR_A2DP_ERR_SET_PROFILE, "status: set_profile never confirmed");
	CHECK(elapsed_s < 2.0, "bounded by confirm_timeout_ms");

	fixture_teardown(&fx);
}

/* Standalone (self-contained sd_event + graph_ops connection) variant --
 * exercised against the SAME fakes (m4_repair_a2dp_run_standalone() only
 * privatizes the graph_ops connection, graph_ops/bt_ops themselves are
 * still caller-supplied per its own doc comment). */
static void test_standalone_already_fine(void)
{
	m4_graph_ops_fake_t fake;
	m4_bt_ops_fake_t bt_fake;
	struct m4_repair_a2dp_result r;

	m4_graph_ops_fake_init(&fake);
	m4_bt_ops_fake_init(&bt_fake);

	r = m4_repair_a2dp_run_standalone(m4_graph_ops_fake(&fake), m4_bt_ops_fake(&bt_fake),
					   "00:1B:66:00:00:01", "hci0", 500, 500, 500, 500);

	CHECK(r.status == M4_REPAIR_A2DP_OK, "standalone: status OK");
	CHECK(bt_fake.connect_profile_calls == 1, "standalone: ConnectProfile called once");
	CHECK(fake.close_calls == 1, "standalone: graph_ops handle closed before returning");
}

static void test_bad_config(void)
{
	struct m4_repair_a2dp_result r;

	r = m4_repair_a2dp_run(NULL);
	CHECK(r.status == M4_REPAIR_A2DP_ERR_BAD_CONFIG, "NULL cfg -> bad config, no crash");

	r = m4_repair_a2dp_run_standalone(NULL, NULL, NULL, NULL, 0, 0, 0, 0);
	CHECK(r.status == M4_REPAIR_A2DP_ERR_BAD_CONFIG, "standalone: NULL args -> bad config, no crash");
}

int main(void)
{
	test_not_present();
	test_already_fine();
	test_missing_then_reappears();
	test_enum_timeout();
	test_connect_profile_errors_but_recovers_anyway();
	test_set_profile_confirm_timeout();
	test_standalone_already_fine();
	test_bad_config();

	if (failures > 0) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("repair_a2dp: all checks passed\n");
	return 0;
}
