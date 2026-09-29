/* momentum4 patch 0008: unit test for the pure SCO-restore
 * episode/retry/rate-limit timing state machine in m4-src/m4-sco-restore.c
 * (the glue in backend-native.c needs a real SCO socket and is only
 * covered by manual testing against real hardware). Mode-string parsing ("m4"|"all"|"off") and
 * the MOMENTUM-4-identity check moved to m4-device-policy.c
 * -- see test/device_policy_test.c for their coverage now.
 *
 * Build/run (plain and ASan/UBSan):
 *   gcc -std=gnu11 -Wall -Wextra -Werror -I../m4-src sco_restore_test.c ../m4-src/m4-sco-restore.c -o build/sco_restore_test && build/sco_restore_test
 *   gcc -std=gnu11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -g \
 *       -I../m4-src sco_restore_test.c ../m4-src/m4-sco-restore.c -o build-asan/sco_restore_test_asan && build-asan/sco_restore_test_asan
 */
#include <stdio.h>
#include <string.h>

#include "m4-sco-restore.h"

static int checks, failures;

#define CHECK(c, msg)                                                             \
	do {                                                                      \
		checks++;                                                         \
		if (!(c)) {                                                       \
			failures++;                                               \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); \
		}                                                                 \
	} while (0)

/* double tap -> drop -> restore succeeds */
static void test_single_episode_ok(void)
{
	struct m4_sco_restore r;
	int delay = -1;

	m4_sco_restore_init(&r);
	CHECK(m4_sco_restore_on_drop(&r, 1000, &delay) == M4_SCO_RESTORE_SCHEDULE, "first drop schedules");
	CHECK(delay == M4_SCO_RESTORE_FIRST_DELAY_MS, "first delay");
	CHECK(r.active, "episode active");
	CHECK(m4_sco_restore_on_drop(&r, 1001, &delay) == M4_SCO_RESTORE_DONE, "drop during episode ignored");
	CHECK(m4_sco_restore_on_result(&r, true, &delay) == M4_SCO_RESTORE_DONE, "ok -> done");
	CHECK(!r.active, "episode over");
	CHECK(m4_sco_restore_on_result(&r, true, &delay) == M4_SCO_RESTORE_DONE, "stray result -> done");
	CHECK(m4_sco_restore_on_result(&r, false, &delay) == M4_SCO_RESTORE_DONE, "stray fail -> done");
}

/* headset refuses: one retry, then give up */
static void test_retry_then_give_up(void)
{
	struct m4_sco_restore r;
	int delay = -1;

	m4_sco_restore_init(&r);
	CHECK(m4_sco_restore_on_drop(&r, 5000, &delay) == M4_SCO_RESTORE_SCHEDULE, "drop");
	CHECK(m4_sco_restore_on_result(&r, false, &delay) == M4_SCO_RESTORE_SCHEDULE, "fail 1 -> retry");
	CHECK(delay == M4_SCO_RESTORE_RETRY_DELAY_MS, "retry delay");
	CHECK(m4_sco_restore_on_result(&r, false, &delay) == M4_SCO_RESTORE_GIVE_UP, "fail 2 -> give up");
	CHECK(!r.active, "episode over after give up");
	/* a later drop is a new episode */
	CHECK(m4_sco_restore_on_drop(&r, 6000, &delay) == M4_SCO_RESTORE_SCHEDULE, "new episode");
	CHECK(m4_sco_restore_on_result(&r, false, &delay) == M4_SCO_RESTORE_SCHEDULE, "retry again");
	CHECK(m4_sco_restore_on_result(&r, true, &delay) == M4_SCO_RESTORE_DONE, "retry ok");
}

/* headset keeps dropping: rate limit, then window slides */
static void test_rate_limit(void)
{
	struct m4_sco_restore r;
	int delay;
	uint64_t t = 100000;

	m4_sco_restore_init(&r);
	for (int i = 0; i < M4_SCO_RESTORE_MAX_EPISODES; i++) {
		CHECK(m4_sco_restore_on_drop(&r, t + (uint64_t)i * 2000, &delay) == M4_SCO_RESTORE_SCHEDULE,
		      "episode within limit");
		CHECK(m4_sco_restore_on_result(&r, true, &delay) == M4_SCO_RESTORE_DONE, "restored");
	}
	CHECK(m4_sco_restore_on_drop(&r, t + 8000, &delay) == M4_SCO_RESTORE_GIVE_UP, "limit hit -> give up");
	CHECK(!r.active, "no episode after give up");
	CHECK(m4_sco_restore_on_drop(&r, t + 9000, &delay) == M4_SCO_RESTORE_GIVE_UP, "still limited");
	/* first episode (t) leaves the window at t + WINDOW */
	CHECK(m4_sco_restore_on_drop(&r, t + M4_SCO_RESTORE_WINDOW_MS, &delay) == M4_SCO_RESTORE_SCHEDULE,
	      "window slid -> allowed again");
	CHECK(m4_sco_restore_on_result(&r, true, &delay) == M4_SCO_RESTORE_DONE, "ok");
	CHECK(m4_sco_restore_on_drop(&r, t + M4_SCO_RESTORE_WINDOW_MS + 1, &delay) == M4_SCO_RESTORE_GIVE_UP,
	      "window full again");
	/* much later: everything expired */
	CHECK(m4_sco_restore_on_drop(&r, t + 10 * M4_SCO_RESTORE_WINDOW_MS, &delay) == M4_SCO_RESTORE_SCHEDULE,
	      "all expired");
	CHECK(r.n_episodes == 1, "pruned to 1");
}

/* transport released / switched away while waiting */
static void test_cancel(void)
{
	struct m4_sco_restore r;
	int delay;

	m4_sco_restore_init(&r);
	CHECK(m4_sco_restore_on_drop(&r, 1, &delay) == M4_SCO_RESTORE_SCHEDULE, "drop");
	m4_sco_restore_cancel(&r);
	CHECK(!r.active, "cancelled");
	CHECK(m4_sco_restore_on_result(&r, false, &delay) == M4_SCO_RESTORE_DONE, "late fail after cancel ignored");
	CHECK(r.n_episodes == 1, "cancelled episode still counts for rate limit");
}

/* [hfp] sco-restore-* config values, not the
 * compiled defaults -- exercises m4_sco_restore_init_with_timing() and the
 * max_episodes clamp (a value above the fixed M4_SCO_RESTORE_MAX_EPISODES
 * ring-buffer capacity must not overflow episode_ms[]). */
static void test_custom_timing(void)
{
	struct m4_sco_restore_timing t;
	struct m4_sco_restore r;
	int delay = -1;

	m4_sco_restore_timing_defaults(&t);
	CHECK(t.first_delay_ms == M4_SCO_RESTORE_FIRST_DELAY_MS, "defaults() first_delay matches macro");
	CHECK(t.max_episodes == M4_SCO_RESTORE_MAX_EPISODES, "defaults() max_episodes matches macro");

	t.first_delay_ms = 111;
	t.retry_delay_ms = 222;
	t.max_retries = 3;
	t.max_episodes = 1000; /* above the compiled ring-buffer capacity -- must clamp */
	m4_sco_restore_init_with_timing(&r, &t);
	CHECK(r.timing.max_episodes == M4_SCO_RESTORE_MAX_EPISODES, "max_episodes clamped to compiled cap");

	CHECK(m4_sco_restore_on_drop(&r, 1000, &delay) == M4_SCO_RESTORE_SCHEDULE, "drop schedules");
	CHECK(delay == 111, "custom first_delay_ms used");
	CHECK(m4_sco_restore_on_result(&r, false, &delay) == M4_SCO_RESTORE_SCHEDULE, "fail 1 -> retry (of 3)");
	CHECK(delay == 222, "custom retry_delay_ms used");
	CHECK(m4_sco_restore_on_result(&r, false, &delay) == M4_SCO_RESTORE_SCHEDULE, "fail 2 -> retry (of 3)");
	CHECK(m4_sco_restore_on_result(&r, false, &delay) == M4_SCO_RESTORE_SCHEDULE, "fail 3 -> retry (of 3)");
	CHECK(m4_sco_restore_on_result(&r, false, &delay) == M4_SCO_RESTORE_GIVE_UP, "fail 4 -> give up (max_retries=3)");

	/* max_episodes clamped to 0 or negative -> at least 1, never division by zero. */
	t.max_episodes = 0;
	m4_sco_restore_init_with_timing(&r, &t);
	CHECK(r.timing.max_episodes == 1, "max_episodes=0 clamped to 1");
	CHECK(m4_sco_restore_on_drop(&r, 1, &delay) == M4_SCO_RESTORE_SCHEDULE, "still works with 1-slot ring");
}

int main(void)
{
	test_single_episode_ok();
	test_retry_then_give_up();
	test_rate_limit();
	test_cancel();
	test_custom_timing();
	printf("sco_restore_test: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
