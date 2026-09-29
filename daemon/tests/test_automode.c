/* Unit tests for core/automode.c -- 13 direct scenario tests, plus a few
 * extra ones. See tests/test_automode_diff.c for the differential runner
 * against a larger, frozen set of reference traces (>=25 scenarios) --
 * this file is the direct-equivalent, subprocess-free counterpart.
 */
#include <stdio.h>
#include <string.h>

#include "core/automode.h"

static int failures = 0;

#define CHECK(cond, msg) \
	do { \
		if (!(cond)) { \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, (msg)); \
			failures++; \
		} \
	} while (0)

#define CHECK_ACTIONS1(d, a0, msg) \
	CHECK((d).action_count == 1 && (d).actions[0] == (a0), msg)
#define CHECK_NO_ACTIONS(d, msg) \
	CHECK((d).action_count == 0, msg)

static void test_mic_on_triggers_call_on_after_debounce(void)
{
	m4_automode_config_t cfg = {.on_debounce_s = 0.3, .off_debounce_s = 2.5, .max_switches = 6, .rate_window_s = 300.0};
	m4_automode_core_t *core = m4_automode_new(&cfg);
	double t = 1000.0;
	m4_automode_start(core, t, "none");

	m4_automode_decision_t d = m4_automode_on_mic_activity_changed(core, 1, t);
	CHECK_NO_ACTIONS(d, "mic on: no immediate action");

	t += 0.1;
	d = m4_automode_tick(core, t);
	CHECK_NO_ACTIONS(d, "debounce has not elapsed yet");

	t += 0.3;
	d = m4_automode_tick(core, t);
	CHECK_ACTIONS1(d, M4_AUTOMODE_ACTION_CALL_ON, "CALL_ON after debounce");

	m4_automode_status_t st;
	m4_automode_status(core, t, &st);
	CHECK(st.call_active, "call_active true");
	CHECK(strcmp(st.call_active_reason, "auto") == 0, "reason auto");

	m4_automode_free(core);
}

static void test_mic_off_triggers_call_off_after_debounce(void)
{
	m4_automode_config_t cfg = {.on_debounce_s = 0.3, .off_debounce_s = 2.5, .max_switches = 6, .rate_window_s = 300.0};
	m4_automode_core_t *core = m4_automode_new(&cfg);
	double t = 1000.0;
	m4_automode_start(core, t, "none");

	m4_automode_on_mic_activity_changed(core, 1, t);
	t += 0.5;
	m4_automode_tick(core, t); /* turned on */

	m4_automode_decision_t d = m4_automode_on_mic_activity_changed(core, 0, t);
	CHECK_NO_ACTIONS(d, "mic off: no immediate action");

	t += 1.0;
	d = m4_automode_tick(core, t);
	CHECK_NO_ACTIONS(d, "off_debounce (2.5s) has not elapsed yet");

	t += 2.0;
	d = m4_automode_tick(core, t);
	CHECK_ACTIONS1(d, M4_AUTOMODE_ACTION_CALL_OFF, "CALL_OFF after debounce");

	m4_automode_status_t st;
	m4_automode_status(core, t, &st);
	CHECK(!st.call_active, "call_active false");

	m4_automode_free(core);
}

static void test_flap_within_debounce_window_cancels_pending_off(void)
{
	m4_automode_config_t cfg = {.on_debounce_s = 0.3, .off_debounce_s = 2.5, .max_switches = 6, .rate_window_s = 300.0};
	m4_automode_core_t *core = m4_automode_new(&cfg);
	double t = 1000.0;
	m4_automode_start(core, t, "none");

	m4_automode_on_mic_activity_changed(core, 1, t);
	t += 0.5;
	m4_automode_tick(core, t);
	m4_automode_on_mic_activity_changed(core, 0, t);
	t += 1.0;
	m4_automode_decision_t d = m4_automode_on_mic_activity_changed(core, 1, t);
	CHECK_NO_ACTIONS(d, "mic grabbed again before off_debounce elapsed");

	t += 3.0;
	d = m4_automode_tick(core, t);
	CHECK_NO_ACTIONS(d, "pending_off should have been cancelled by grabbing the mic again");

	m4_automode_status_t st;
	m4_automode_status(core, t, &st);
	CHECK(st.call_active, "the call should stay on");

	m4_automode_free(core);
}

static void test_idempotent_repeated_same_state(void)
{
	m4_automode_config_t cfg = {.on_debounce_s = 0.3, .off_debounce_s = 2.5, .max_switches = 6, .rate_window_s = 300.0};
	m4_automode_core_t *core = m4_automode_new(&cfg);
	double t = 1000.0;
	m4_automode_start(core, t, "none");

	m4_automode_decision_t d1 = m4_automode_on_mic_activity_changed(core, 1, t);
	m4_automode_decision_t d2 = m4_automode_on_mic_activity_changed(core, 1, t); /* repeat */
	CHECK_NO_ACTIONS(d1, "first mic-on: no immediate action");
	CHECK_NO_ACTIONS(d2, "repeated mic-on: no immediate action");

	t += 1.0;
	m4_automode_decision_t d3 = m4_automode_tick(core, t);
	CHECK_ACTIONS1(d3, M4_AUTOMODE_ACTION_CALL_ON, "only ONE CALL_ON, not two");

	m4_automode_free(core);
}

static void test_manual_call_not_auto_off_on_mic_release(void)
{
	m4_automode_config_t cfg = {.on_debounce_s = 0.1, .off_debounce_s = 1.0, .max_switches = 6, .rate_window_s = 300.0};
	m4_automode_core_t *core = m4_automode_new(&cfg);
	double t = 1000.0;

	m4_automode_start(core, t, "manual");
	m4_automode_status_t st;
	m4_automode_status(core, t, &st);
	CHECK(st.call_active, "call_active true after manual start");
	CHECK(strcmp(st.call_active_reason, "manual") == 0, "reason manual");

	m4_automode_on_mic_activity_changed(core, 1, t);
	t += 0.05;
	m4_automode_decision_t d = m4_automode_on_mic_activity_changed(core, 0, t);
	CHECK_NO_ACTIONS(d, "mic release on manual call: no immediate action");

	t += 5.0;
	d = m4_automode_tick(core, t);
	CHECK_NO_ACTIONS(d, "a manual call should not be ended by automode");

	m4_automode_status(core, t, &st);
	CHECK(st.call_active, "still active");
	CHECK(strcmp(st.call_active_reason, "manual") == 0, "still manual");

	m4_automode_free(core);
}

static void test_external_manual_off_while_mic_still_active_is_respected(void)
{
	m4_automode_config_t cfg = {.on_debounce_s = 0.1, .off_debounce_s = 1.0, .max_switches = 6, .rate_window_s = 300.0};
	m4_automode_core_t *core = m4_automode_new(&cfg);
	double t = 1000.0;
	m4_automode_start(core, t, "none");

	m4_automode_on_mic_activity_changed(core, 1, t);
	t += 0.2;
	m4_automode_tick(core, t);

	m4_automode_status_t st;
	m4_automode_status(core, t, &st);
	CHECK(st.call_active, "auto-activated");
	CHECK(strcmp(st.call_active_reason, "auto") == 0, "reason auto");

	m4_automode_decision_t d = m4_automode_note_external_call_state(core, "none", t);
	CHECK_NO_ACTIONS(d, "note_external_call_state: no action itself");
	m4_automode_status(core, t, &st);
	CHECK(!st.call_active, "call_active false after external none");

	t += 1.0;
	d = m4_automode_tick(core, t);
	CHECK_NO_ACTIONS(d, "must not turn the call back on by themselves without a new mic event");

	m4_automode_free(core);
}

static void test_freeze_after_too_many_switches(void)
{
	m4_automode_config_t cfg = {.on_debounce_s = 0.0, .off_debounce_s = 0.0, .max_switches = 3, .rate_window_s = 60.0};
	m4_automode_core_t *core = m4_automode_new(&cfg);
	double t = 1000.0;
	m4_automode_start(core, t, "none");

	t += 1.0;
	m4_automode_on_mic_activity_changed(core, 1, t);
	m4_automode_decision_t d = m4_automode_tick(core, t);
	CHECK_ACTIONS1(d, M4_AUTOMODE_ACTION_CALL_ON, "switch 1");

	t += 1.0;
	m4_automode_on_mic_activity_changed(core, 0, t);
	d = m4_automode_tick(core, t);
	CHECK_ACTIONS1(d, M4_AUTOMODE_ACTION_CALL_OFF, "switch 2");

	t += 1.0;
	m4_automode_on_mic_activity_changed(core, 1, t);
	d = m4_automode_tick(core, t);
	CHECK_ACTIONS1(d, M4_AUTOMODE_ACTION_CALL_ON, "switch 3");

	t += 1.0;
	m4_automode_on_mic_activity_changed(core, 0, t);
	d = m4_automode_tick(core, t); /* switch 4 -- over the limit */
	CHECK_ACTIONS1(d, M4_AUTOMODE_ACTION_ANNOUNCE_FREEZE, "freeze on 4th switch");

	m4_automode_status_t st;
	m4_automode_status(core, t, &st);
	CHECK(st.frozen, "frozen == true");

	m4_automode_free(core);
}

static void flap_once(m4_automode_core_t *core, double *t)
{
	*t += 1.0;
	m4_automode_on_mic_activity_changed(core, 1, *t);
	m4_automode_tick(core, *t);
	*t += 1.0;
	m4_automode_on_mic_activity_changed(core, 0, *t);
	m4_automode_tick(core, *t);
}

static void test_frozen_ignores_further_mic_events(void)
{
	m4_automode_config_t cfg = {.on_debounce_s = 0.0, .off_debounce_s = 0.0, .max_switches = 3, .rate_window_s = 60.0};
	m4_automode_core_t *core = m4_automode_new(&cfg);
	double t = 1000.0;
	m4_automode_start(core, t, "none");

	for (int i = 0; i < 4; i++)
		flap_once(core, &t);

	m4_automode_status_t st;
	m4_automode_status(core, t, &st);
	CHECK(st.frozen, "frozen after 4 flaps");

	t += 1.0;
	m4_automode_decision_t d = m4_automode_on_mic_activity_changed(core, 1, t);
	CHECK_NO_ACTIONS(d, "frozen: mic event produces no action");
	t += 10.0;
	d = m4_automode_tick(core, t);
	CHECK_NO_ACTIONS(d, "frozen -- mic events should not produce actions");

	m4_automode_free(core);
}

static void test_resume_unfreezes(void)
{
	m4_automode_config_t cfg = {.on_debounce_s = 0.0, .off_debounce_s = 0.0, .max_switches = 3, .rate_window_s = 60.0};
	m4_automode_core_t *core = m4_automode_new(&cfg);
	double t = 1000.0;
	m4_automode_start(core, t, "none");

	for (int i = 0; i < 4; i++)
		flap_once(core, &t);

	m4_automode_status_t st;
	m4_automode_status(core, t, &st);
	CHECK(st.frozen, "frozen before resume");

	m4_automode_resume(core, t);
	m4_automode_status(core, t, &st);
	CHECK(!st.frozen, "resume clears frozen");
	CHECK(st.call_active, "off was not executed (was frozen) -- the call stayed on");

	m4_automode_note_external_call_state(core, "auto", t);
	t += 1.0;
	m4_automode_on_mic_activity_changed(core, 1, t);
	t += 1.0;
	m4_automode_decision_t d = m4_automode_on_mic_activity_changed(core, 0, t);
	CHECK_NO_ACTIONS(d, "no immediate action on mic off");
	t += 1.0;
	d = m4_automode_tick(core, t);
	CHECK_ACTIONS1(d, M4_AUTOMODE_ACTION_CALL_OFF, "after resume+resync, mic reactions are normal again");

	m4_automode_free(core);
}

static void test_failed_call_on_rolls_back_state(void)
{
	m4_automode_config_t cfg = {.on_debounce_s = 0.0, .off_debounce_s = 0.0, .max_switches = 6, .rate_window_s = 300.0};
	m4_automode_core_t *core = m4_automode_new(&cfg);
	double t = 1000.0;
	m4_automode_start(core, t, "none");

	m4_automode_on_mic_activity_changed(core, 1, t);
	m4_automode_decision_t d = m4_automode_tick(core, t);
	CHECK_ACTIONS1(d, M4_AUTOMODE_ACTION_CALL_ON, "CALL_ON issued");

	m4_automode_status_t st;
	m4_automode_status(core, t, &st);
	CHECK(st.call_active, "call_active true before rollback");

	m4_automode_note_call_result(core, M4_AUTOMODE_ACTION_CALL_ON, 0, t);
	m4_automode_status(core, t, &st);
	CHECK(!st.call_active, "call_active should roll back after a failure");

	m4_automode_free(core);
}

static void test_failed_call_off_rolls_back_state(void)
{
	m4_automode_config_t cfg = {.on_debounce_s = 0.0, .off_debounce_s = 0.0, .max_switches = 6, .rate_window_s = 300.0};
	m4_automode_core_t *core = m4_automode_new(&cfg);
	double t = 1000.0;
	m4_automode_start(core, t, "none");

	m4_automode_on_mic_activity_changed(core, 1, t);
	m4_automode_tick(core, t);
	m4_automode_on_mic_activity_changed(core, 0, t);
	m4_automode_decision_t d = m4_automode_tick(core, t);
	CHECK_ACTIONS1(d, M4_AUTOMODE_ACTION_CALL_OFF, "CALL_OFF issued");

	m4_automode_note_call_result(core, M4_AUTOMODE_ACTION_CALL_OFF, 0, t);
	m4_automode_status_t st;
	m4_automode_status(core, t, &st);
	CHECK(st.call_active, "after an off failure, the call should be considered still on");

	m4_automode_free(core);
}

static void test_start_with_existing_manual_call_does_nothing(void)
{
	m4_automode_core_t *core = m4_automode_new(NULL);
	m4_automode_decision_t d = m4_automode_start(core, 1000.0, "manual");
	CHECK_NO_ACTIONS(d, "start manual: no action");
	m4_automode_status_t st;
	m4_automode_status(core, 1000.0, &st);
	CHECK(st.call_active, "call_active true");
	m4_automode_free(core);
}

static void test_start_with_no_call_does_nothing(void)
{
	m4_automode_core_t *core = m4_automode_new(NULL);
	m4_automode_decision_t d = m4_automode_start(core, 1000.0, "none");
	CHECK_NO_ACTIONS(d, "start none: no action");
	m4_automode_status_t st;
	m4_automode_status(core, 1000.0, &st);
	CHECK(!st.call_active, "call_active false");
	m4_automode_free(core);
}

int main(void)
{
	test_mic_on_triggers_call_on_after_debounce();
	test_mic_off_triggers_call_off_after_debounce();
	test_flap_within_debounce_window_cancels_pending_off();
	test_idempotent_repeated_same_state();
	test_manual_call_not_auto_off_on_mic_release();
	test_external_manual_off_while_mic_still_active_is_respected();
	test_freeze_after_too_many_switches();
	test_frozen_ignores_further_mic_events();
	test_resume_unfreezes();
	test_failed_call_on_rolls_back_state();
	test_failed_call_off_rolls_back_state();
	test_start_with_existing_manual_call_does_nothing();
	test_start_with_no_call_does_nothing();

	if (failures > 0) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("automode: all checks passed (13 scenarios)\n");
	return 0;
}
