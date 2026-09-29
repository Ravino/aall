/* Unit tests for core/callfsm.c, against core/callfsm_fake.c: about 40
 * call-mode scenarios covering the call-state machine's scope.
 *
 * Deliberately out of scope for this pure core (belongs to a layer
 * callfsm.c does not own):
 *   - quantum/retarget_speech_streams/speech_health-style audio-routing
 *     side effects -- not callfsm's concern.
 *   - earcon_best_effort (pw-play unavailable/fails) -- callfsm never
 *     calls pw-play; it only returns an m4_callfsm_announce_t hint. Whether
 *     that hint's playback succeeds is the CLI/daemon I/O layer's problem,
 *     not callfsm's -- tested there, not here.
 *   - CLI argument parsing (`--bogus` and friends) --
 *     not part of the pure core's API (m4_callfsm_on/off take already-
 *     parsed `quiet`/`reason`, no flag string parsing here at all).
 *   - restoring call-state from an emergency-rollback tool -- that is a
 *     separate, standalone integration, out of scope for this pure core.
 */
#include <stdio.h>
#include <string.h>

#include "core/callfsm.h"
#include "core/callfsm_fake.h"

#include "../../common/m4-i18n.h"

static int failures = 0;

#define CHECK(cond, msg) \
	do { \
		if (!(cond)) { \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, (msg)); \
			failures++; \
		} \
	} while (0)

static void setup(m4_callfsm_fake_t *fake, m4_callfsm_ops_t *ops)
{
	m4_callfsm_fake_init(fake);
	*ops = m4_callfsm_fake_ops(fake);
}

/* on_no_m4 */
static void test_on_no_m4(void)
{
	m4_callfsm_fake_t fake;
	m4_callfsm_ops_t ops;
	setup(&fake, &ops);
	fake.connected = 0;

	m4_callfsm_result_t r = m4_callfsm_on(&ops, 0, "manual");
	CHECK(r.rc == M4_CALLFSM_RC_FAILED, "exit code 2");
	CHECK(strstr(r.message, M4_T("not connected",
"не подключены")) != NULL, M4_T("text: headset not connected",
"текст: наушники не подключены"));
	CHECK(!fake.state_present, M4_T("call-state not created",
"call-state не создан"));
	CHECK(strcmp(fake.profile, "a2dp-sink") == 0, M4_T("profile unchanged",
"профиль не менялся"));
	CHECK(r.announce == M4_CALLFSM_ANNOUNCE_NONE, M4_T("earcon did not play",
"earcon не играл"));
}

/* on_success_msbc */
static void test_on_success_msbc(void)
{
	m4_callfsm_fake_t fake;
	m4_callfsm_ops_t ops;
	setup(&fake, &ops);

	m4_callfsm_result_t r = m4_callfsm_on(&ops, 0, "manual");
	CHECK(r.rc == M4_CALLFSM_RC_OK, "exit code 0");
	CHECK(strcmp(fake.profile, M4_CALLFSM_PROFILE_MSBC) == 0, M4_T("card profile -> headset-head-unit",
"профиль карты -> headset-head-unit"));
	CHECK(strcmp(r.message, M4_T("Call mode enabled: profile headset-head-unit (codec mSBC).",
"Режим звонка включён: профиль headset-head-unit (кодек mSBC).")) == 0,
	      M4_T("text: exact mSBC phrase",
"текст: точная фраза про mSBC"));
	CHECK(r.announce == M4_CALLFSM_ANNOUNCE_ON, M4_T("earcon on played",
"earcon on сыгран"));
	CHECK(fake.state_present, M4_T("call-state created",
"call-state создан"));
	CHECK(strcmp(fake.state_profile, "a2dp-sink") == 0, M4_T("call-state: PROFILE=a2dp-sink (original)",
"call-state: PROFILE=a2dp-sink (исходный)"));
	CHECK(!r.cvsd_fallback_warning, M4_T("not the fallback codec",
"не резервный кодек"));
}

/* on_idempotent */
static void test_on_idempotent(void)
{
	m4_callfsm_fake_t fake;
	m4_callfsm_ops_t ops;
	setup(&fake, &ops);
	m4_callfsm_on(&ops, 0, "manual");
	char saved_before[M4_CALLFSM_FAKE_PROFILE_LEN];
	strcpy(saved_before, fake.state_profile);
	int writes_before = fake.state_write_calls;

	m4_callfsm_result_t r2 = m4_callfsm_on(&ops, 0, "manual");
	CHECK(r2.rc == M4_CALLFSM_RC_OK, M4_T("repeat on: exit 0",
"повторный on: exit 0"));
	CHECK(strstr(r2.message, M4_T("Already in call mode",
"Уже в режиме звонка")) != NULL, M4_T("repeat on: idempotency message",
"повторный on: сообщение об идемпотентности"));
	CHECK(strcmp(fake.state_profile, saved_before) == 0, M4_T("repeat on: call-state NOT rewritten (value)",
"повторный on: call-state НЕ перезаписан (значение)"));
	CHECK(fake.state_write_calls == writes_before, M4_T("repeat on: state_write NOT called again",
"повторный on: state_write НЕ вызывался снова"));
	CHECK(r2.announce == M4_CALLFSM_ANNOUNCE_NONE, M4_T("repeat on: earcon did NOT play (no-op)",
"повторный on: earcon НЕ играл (no-op)"));
}

/* off_success */
static void test_off_success(void)
{
	m4_callfsm_fake_t fake;
	m4_callfsm_ops_t ops;
	setup(&fake, &ops);
	m4_callfsm_on(&ops, 0, "manual");

	m4_callfsm_result_t r = m4_callfsm_off(&ops, 0);
	CHECK(r.rc == M4_CALLFSM_RC_OK, "exit code 0");
	CHECK(strcmp(fake.profile, "a2dp-sink") == 0, M4_T("card profile -> a2dp-sink",
"профиль карты -> a2dp-sink"));
	CHECK(strcmp(r.message, M4_T("Call disabled, profile restored to a2dp-sink.",
"Звонок выключен, профиль возвращён на a2dp-sink.")) == 0,
	      M4_T("text: exact off phrase",
"текст: точная фраза о выключении"));
	CHECK(r.announce == M4_CALLFSM_ANNOUNCE_OFF, M4_T("earcon off played",
"earcon off сыгран"));
	CHECK(!fake.state_present, M4_T("call-state removed",
"call-state удалён"));
}

/* on_fallback_cvsd */
static void test_on_fallback_cvsd(void)
{
	m4_callfsm_fake_t fake;
	m4_callfsm_ops_t ops;
	setup(&fake, &ops);
	m4_callfsm_fake_set_fail_profile(&fake, M4_CALLFSM_PROFILE_MSBC);

	m4_callfsm_result_t r = m4_callfsm_on(&ops, 0, "manual");
	CHECK(r.rc == M4_CALLFSM_RC_OK, "exit code 0");
	CHECK(strcmp(fake.profile, M4_CALLFSM_PROFILE_CVSD) == 0, M4_T("card profile -> headset-head-unit-cvsd",
"профиль карты -> headset-head-unit-cvsd"));
	CHECK(r.cvsd_fallback_warning, M4_T("warning about the CVSD fallback",
"предупреждение про резервный CVSD"));
	CHECK(strcmp(r.message, M4_T("Call mode enabled: profile headset-head-unit-cvsd (codec CVSD).",
"Режим звонка включён: профиль headset-head-unit-cvsd (кодек CVSD).")) == 0,
	      M4_T("text: final phrase with CVSD",
"текст: финальная фраза с CVSD"));
	CHECK(r.announce == M4_CALLFSM_ANNOUNCE_ON, M4_T("earcon on still played (signals the transition, not the codec)",
"earcon on всё равно сыгран (сигнал про переход, не про кодек)"));
}

/* mSBC's
 * set_profile_confirm() times out (adapter's confirm loop never saw a
 * PARAMS-changed info event in time) even though the switch DID apply --
 * callfsm must cross-check the actual profile instead of declaring failure
 * and racing a second, different (CVSD) switch request. */
static void test_on_msbc_confirm_times_out_but_actually_landed(void)
{
	m4_callfsm_fake_t fake;
	m4_callfsm_ops_t ops;
	setup(&fake, &ops);
	m4_callfsm_fake_set_slow_profile(&fake, M4_CALLFSM_PROFILE_MSBC);

	m4_callfsm_result_t r = m4_callfsm_on(&ops, 0, "manual");
	CHECK(r.rc == M4_CALLFSM_RC_OK, M4_T("exit code 0 (confirmed by the actual profile)",
"exit code 0 (подтверждено фактическим профилем)"));
	CHECK(strcmp(fake.profile, M4_CALLFSM_PROFILE_MSBC) == 0, M4_T("card profile stayed headset-head-unit",
"профиль карты остался headset-head-unit"));
	CHECK(strcmp(r.message, M4_T("Call mode enabled: profile headset-head-unit (codec mSBC).",
"Режим звонка включён: профиль headset-head-unit (кодек mSBC).")) == 0,
	      M4_T("text: same as a normal mSBC success (not 'ERROR')",
"текст: как при обычном успехе mSBC (не 'ОШИБКА')"));
	CHECK(!r.cvsd_fallback_warning, M4_T("NOT flagged as CVSD fallback -- CVSD was never requested",
"НЕ помечено как резервный CVSD -- CVSD вообще не запрашивался"));
	CHECK(r.announce == M4_CALLFSM_ANNOUNCE_ON, M4_T("earcon on played (success, not failure)",
"earcon on сыгран (успех, не провал)"));
	/* Key part of the regression: there was NO second, competing request for
	 * CVSD on top of a not-yet-confirmed mSBC (which is what caused a2dp-sink
	 * to disappear from EnumProfile in production). fake_set_profile_confirm()
	 * increments set_profile_calls on EVERY confirm call -- exactly 1,
	 * if CVSD was never requested. */
	CHECK(fake.set_profile_calls == 1, M4_T("set_profile_confirm() called EXACTLY once (CVSD never requested)",
"set_profile_confirm() вызван РОВНО один раз (CVSD не запрашивался)"));
}

/* Symmetric case for CVSD (mSBC genuinely rejected, CVSD "hung" but
 * actually got applied). */
static void test_on_cvsd_confirm_times_out_but_actually_landed(void)
{
	m4_callfsm_fake_t fake;
	m4_callfsm_ops_t ops;
	setup(&fake, &ops);
	m4_callfsm_fake_set_fail_profile(&fake, M4_CALLFSM_PROFILE_MSBC);
	m4_callfsm_fake_set_slow_profile(&fake, M4_CALLFSM_PROFILE_CVSD);

	m4_callfsm_result_t r = m4_callfsm_on(&ops, 0, "manual");
	CHECK(r.rc == M4_CALLFSM_RC_OK, M4_T("exit code 0 (CVSD confirmed by the actual profile)",
"exit code 0 (CVSD подтверждён фактическим профилем)"));
	CHECK(strcmp(fake.profile, M4_CALLFSM_PROFILE_CVSD) == 0, M4_T("card profile -> headset-head-unit-cvsd",
"профиль карты -> headset-head-unit-cvsd"));
	CHECK(r.cvsd_fallback_warning, M4_T("warning about the CVSD fallback (mSBC was a real failure)",
"предупреждение про резервный CVSD (mSBC был настоящим отказом)"));
}

/* off(): the same race on the a2dp-sink recovery path. */
static void test_off_music_default_confirm_times_out_but_actually_landed(void)
{
	m4_callfsm_fake_t fake;
	m4_callfsm_ops_t ops;
	setup(&fake, &ops);
	m4_callfsm_on(&ops, 0, "manual");
	m4_callfsm_fake_set_slow_profile(&fake, "a2dp-sink");

	m4_callfsm_result_t r = m4_callfsm_off(&ops, 0);
	CHECK(r.rc == M4_CALLFSM_RC_OK, M4_T("exit code 0 (confirmed by the actual profile)",
"exit code 0 (подтверждено фактическим профилем)"));
	CHECK(strcmp(fake.profile, "a2dp-sink") == 0, M4_T("card profile -> a2dp-sink",
"профиль карты -> a2dp-sink"));
	CHECK(!fake.state_present, M4_T("call-state removed (not 'kept for retry')",
"call-state удалён (не 'сохранён для повтора')"));
}

/* on_both_fail */
static void test_on_both_fail(void)
{
	m4_callfsm_fake_t fake;
	m4_callfsm_ops_t ops;
	setup(&fake, &ops);
	m4_callfsm_fake_set_fail_profile(&fake, M4_CALLFSM_PROFILE_MSBC);
	m4_callfsm_fake_set_fail_profile(&fake, M4_CALLFSM_PROFILE_CVSD);

	m4_callfsm_result_t r = m4_callfsm_on(&ops, 0, "manual");
	CHECK(r.rc == M4_CALLFSM_RC_FAILED, "exit code 2");
	CHECK(strcmp(fake.profile, "a2dp-sink") == 0, M4_T("profile stayed a2dp-sink (rollback)",
"профиль остался a2dp-sink (откат)"));
	CHECK(!fake.state_present, M4_T("call-state did not stay",
"call-state не остался"));
	CHECK(strstr(r.message, M4_T("failed to switch the Momentum 4 into call mode",
"не удалось переключить Momentum 4 в режим звонка")) != NULL,
	      M4_T("text: failure message",
"текст: сообщение о неудаче"));
	CHECK(r.announce == M4_CALLFSM_ANNOUNCE_NONE, M4_T("earcon did NOT play (failure, not a transition)",
"earcon НЕ играл (провал, не переход)"));
}

/* off_no_state */
static void test_off_no_state(void)
{
	m4_callfsm_fake_t fake;
	m4_callfsm_ops_t ops;
	setup(&fake, &ops);

	m4_callfsm_result_t r = m4_callfsm_off(&ops, 0);
	CHECK(r.rc == M4_CALLFSM_RC_OK, "exit code 0");
	CHECK(strstr(r.message, M4_T("not active",
"не активен")) != NULL, M4_T("message: call is not active",
"сообщение: звонок не активен"));
	CHECK(r.announce == M4_CALLFSM_ANNOUNCE_NONE, M4_T("earcon did not play",
"earcon не играл"));
}

/* (variant of status_manual_hfp, but for off) -- card already
 * on a call profile with no saved state -- off() restores the default. */
static void test_off_manual_profile_without_state(void)
{
	m4_callfsm_fake_t fake;
	m4_callfsm_ops_t ops;
	setup(&fake, &ops);
	strcpy(fake.profile, M4_CALLFSM_PROFILE_MSBC); /* switched by hand, not via on() */

	m4_callfsm_result_t r = m4_callfsm_off(&ops, 0);
	CHECK(r.rc == M4_CALLFSM_RC_OK, "exit code 0");
	CHECK(strcmp(fake.profile, "a2dp-sink") == 0, M4_T("profile restored without call-state",
"профиль восстановлен без call-state"));
	CHECK(r.announce == M4_CALLFSM_ANNOUNCE_OFF, M4_T("earcon off played",
"earcon off сыгран"));
}

/* off_profile_confirm_fail (+ retry) */
static void test_off_profile_confirm_fail_then_retry(void)
{
	m4_callfsm_fake_t fake;
	m4_callfsm_ops_t ops;
	setup(&fake, &ops);
	m4_callfsm_on(&ops, 0, "manual");
	m4_callfsm_fake_set_fail_profile(&fake, "a2dp-sink");

	m4_callfsm_result_t r = m4_callfsm_off(&ops, 0);
	CHECK(r.rc == M4_CALLFSM_RC_FAILED, "exit code 2");
	CHECK(fake.state_present, M4_T("call-state KEPT (for retry)",
"call-state СОХРАНЁН (для повтора)"));
	CHECK(strcmp(fake.profile, M4_CALLFSM_PROFILE_MSBC) == 0, M4_T("profile stayed headset-head-unit",
"профиль остался headset-head-unit"));
	CHECK(strstr(r.message, M4_T("profile did not confirm as a2dp-sink",
"профиль не подтвердился как a2dp-sink")) != NULL,
	      M4_T("text: failure-to-return message",
"текст: сообщение о неудаче возврата"));
	CHECK(r.announce == M4_CALLFSM_ANNOUNCE_NONE, M4_T("earcon off did NOT play (failure, not a transition)",
"earcon off НЕ играл (провал, не переход)"));

	CHECK(fake.repair_a2dp_calls == 0,
	      M4_T("repair-a2dp NOT called (a2dp-sink is present in EnumProfile -- a different failure reason)",
"repair-a2dp НЕ вызывался (a2dp-sink присутствует в EnumProfile -- отказ по другой причине)"));

	m4_callfsm_fake_clear_fail_profiles(&fake);
	m4_callfsm_result_t r2 = m4_callfsm_off(&ops, 0);
	CHECK(r2.rc == M4_CALLFSM_RC_OK, M4_T("retry off after the cause is fixed: exit 0",
"повтор off после устранения причины: exit 0"));
	CHECK(!fake.state_present, M4_T("retry off: call-state removed",
"повтор off: call-state удалён"));
	CHECK(r2.announce == M4_CALLFSM_ANNOUNCE_OFF, M4_T("retry off: earcon off played",
"повтор off: earcon off сыгран"));
}

/* a2dp-sink is structurally MISSING from EnumProfile (not just a slow/failed
 * set_param) -- off() must attempt repair-a2dp exactly once, and succeed if
 * the repair itself succeeds, without ever needing a second attempt or
 * touching call-state twice. */
static void test_off_a2dp_sink_missing_repaired(void)
{
	m4_callfsm_fake_t fake;
	m4_callfsm_ops_t ops;
	setup(&fake, &ops);
	m4_callfsm_on(&ops, 0, "manual");
	fake.a2dp_sink_missing = 1;
	fake.repair_a2dp_succeeds = 1;

	m4_callfsm_result_t r = m4_callfsm_off(&ops, 0);
	CHECK(r.rc == M4_CALLFSM_RC_OK, M4_T("exit code 0 (repair-a2dp restored the profile)",
"exit code 0 (repair-a2dp восстановил профиль)"));
	CHECK(strcmp(fake.profile, "a2dp-sink") == 0, M4_T("card profile -> a2dp-sink (via repair)",
"профиль карты -> a2dp-sink (через repair)"));
	CHECK(fake.repair_a2dp_calls == 1, M4_T("repair-a2dp called EXACTLY once",
"repair-a2dp вызван РОВНО один раз"));
	CHECK(!fake.a2dp_sink_missing, M4_T("a2dp-sink is no longer flagged missing (fake side effect)",
"a2dp-sink больше не отмечен отсутствующим (побочный эффект фейка)"));
	CHECK(!fake.state_present, M4_T("call-state removed (successful completion)",
"call-state удалён (успешное завершение)"));
	CHECK(r.announce == M4_CALLFSM_ANNOUNCE_OFF, M4_T("earcon off played (success, not failure)",
"earcon off сыгран (успех, не провал)"));
}

/* Symmetric case: a2dp-sink is missing, but repair-a2dp itself did not
 * succeed (BlueZ refused/did not have time) -- off() reports the normal
 * "profile not confirmed" error, call-state is saved for a retry, repair
 * is called exactly once (not in a loop). */
static void test_off_a2dp_sink_missing_repair_fails(void)
{
	m4_callfsm_fake_t fake;
	m4_callfsm_ops_t ops;
	setup(&fake, &ops);
	m4_callfsm_on(&ops, 0, "manual");
	fake.a2dp_sink_missing = 1;
	fake.repair_a2dp_succeeds = 0;

	m4_callfsm_result_t r = m4_callfsm_off(&ops, 0);
	CHECK(r.rc == M4_CALLFSM_RC_FAILED, M4_T("exit code 2 (repair-a2dp did not manage it)",
"exit code 2 (repair-a2dp не справился)"));
	CHECK(fake.state_present, M4_T("call-state KEPT (for retry)",
"call-state СОХРАНЁН (для повтора)"));
	CHECK(fake.repair_a2dp_calls == 1, M4_T("repair-a2dp called EXACTLY once (not looping)",
"repair-a2dp вызван РОВНО один раз (не зациклен)"));
	CHECK(fake.a2dp_sink_missing, M4_T("a2dp-sink is still flagged missing",
"a2dp-sink всё ещё отмечен отсутствующим"));
	CHECK(strstr(r.message, M4_T("profile did not confirm as a2dp-sink",
"профиль не подтвердился как a2dp-sink")) != NULL,
	      M4_T("text: same error message as without repair-a2dp",
"текст: то же сообщение об ошибке, что и без repair-a2dp"));
	CHECK(r.announce == M4_CALLFSM_ANNOUNCE_NONE, M4_T("earcon off did NOT play (failure)",
"earcon off НЕ играл (провал)"));

	/* A repeated off after repair-a2dp finally succeeds (not within
	 * this same call -- the next off() call, the same way a manual
	 * user retry already handles other kinds of failure). */
	fake.repair_a2dp_succeeds = 1;
	m4_callfsm_result_t r2 = m4_callfsm_off(&ops, 0);
	CHECK(r2.rc == M4_CALLFSM_RC_OK, M4_T("retry off after the fix: exit 0",
"повтор off после починки: exit 0"));
	CHECK(!fake.state_present, M4_T("retry off: call-state removed",
"повтор off: call-state удалён"));
}

/* (variant of off_manual_profile_without_state) -- card is on a
 * call profile with no saved call-state (manual switch outside callfsm),
 * AND a2dp-sink happens to be missing from EnumProfile too -- the SAME
 * repair-a2dp path must fire on this branch of off() as well (callfsm.c's
 * OTHER confirm_or_repair() call site). */
static void test_off_manual_no_state_a2dp_missing_repaired(void)
{
	m4_callfsm_fake_t fake;
	m4_callfsm_ops_t ops;
	setup(&fake, &ops);
	strcpy(fake.profile, M4_CALLFSM_PROFILE_MSBC); /* switched by hand, not via on() */
	fake.a2dp_sink_missing = 1;
	fake.repair_a2dp_succeeds = 1;

	m4_callfsm_result_t r = m4_callfsm_off(&ops, 0);
	CHECK(r.rc == M4_CALLFSM_RC_OK, M4_T("exit code 0 (repair-a2dp restored the profile)",
"exit code 0 (repair-a2dp восстановил профиль)"));
	CHECK(strcmp(fake.profile, "a2dp-sink") == 0, M4_T("profile restored via repair, without call-state",
"профиль восстановлен через repair, без call-state"));
	CHECK(fake.repair_a2dp_calls == 1, M4_T("repair-a2dp called EXACTLY once",
"repair-a2dp вызван РОВНО один раз"));
	CHECK(r.announce == M4_CALLFSM_ANNOUNCE_OFF, M4_T("earcon off played",
"earcon off сыгран"));
}

/* off_m4_disconnected_mid_call */
static void test_off_m4_disconnected_mid_call(void)
{
	m4_callfsm_fake_t fake;
	m4_callfsm_ops_t ops;
	setup(&fake, &ops);
	m4_callfsm_on(&ops, 0, "manual");
	fake.connected = 0;

	m4_callfsm_result_t r = m4_callfsm_off(&ops, 0);
	CHECK(r.rc == M4_CALLFSM_RC_FAILED, "exit code 2");
	CHECK(fake.state_present, M4_T("call-state KEPT (M4 not present)",
"call-state СОХРАНЁН (M4 отсутствуют)"));
	CHECK(strstr(r.message, M4_T("not connected -- call state KEPT",
"не подключены -- состояние звонка СОХРАНЕНО")) != NULL,
	      M4_T("text: warning about the missing headset",
"текст: предупреждение про отсутствующие наушники"));
	CHECK(r.announce == M4_CALLFSM_ANNOUNCE_NONE, M4_T("earcon did NOT play (did not complete normally)",
"earcon НЕ играл (не завершился штатно)"));

	fake.connected = 1;
	m4_callfsm_result_t r2 = m4_callfsm_off(&ops, 0);
	CHECK(r2.rc == M4_CALLFSM_RC_OK, M4_T("after the M4 reconnects: repeat off exit 0",
"после возврата M4: повторный off exit 0"));
	CHECK(strcmp(fake.profile, "a2dp-sink") == 0, M4_T("after the M4 reconnects: profile restored",
"после возврата M4: профиль восстановлен"));
	CHECK(!fake.state_present, M4_T("after the M4 reconnects: call-state removed",
"после возврата M4: call-state удалён"));
	CHECK(r2.announce == M4_CALLFSM_ANNOUNCE_OFF, M4_T("after the M4 reconnects: earcon off played",
"после возврата M4: earcon off сыгран"));
}

/* off_helper_not_alive */
static void test_off_helper_not_alive(void)
{
	m4_callfsm_fake_t fake;
	m4_callfsm_ops_t ops;
	setup(&fake, &ops);
	m4_callfsm_on(&ops, 0, "manual");
	fake.helper_alive = 0;

	m4_callfsm_result_t r = m4_callfsm_off(&ops, 0);
	CHECK(r.rc == M4_CALLFSM_RC_OK, M4_T("exit code 0 (not fatal)",
"exit code 0 (не фатально)"));
	CHECK(!fake.state_present, M4_T("call-state removed despite the helper",
"call-state удалён несмотря на helper"));
	CHECK(r.announce == M4_CALLFSM_ANNOUNCE_OFF, M4_T("earcon off still played",
"earcon off всё равно сыгран"));
	CHECK(fake.helper_alive_calls >= 1, M4_T("helper_alive() was called (not fatal, but checked)",
"helper_alive() был вызван (не фатально, но проверялся)"));
}

/* quiet */
static void test_quiet_suppresses_announce(void)
{
	m4_callfsm_fake_t fake;
	m4_callfsm_ops_t ops;
	setup(&fake, &ops);

	m4_callfsm_result_t r_on = m4_callfsm_on(&ops, 1, "manual");
	CHECK(r_on.announce == M4_CALLFSM_ANNOUNCE_NONE, M4_T("quiet on: earcon did NOT play",
"quiet on: earcon НЕ играл"));
	m4_callfsm_result_t r_off = m4_callfsm_off(&ops, 1);
	CHECK(r_off.announce == M4_CALLFSM_ANNOUNCE_NONE, M4_T("quiet off: earcon did NOT play",
"quiet off: earcon НЕ играл"));
}

/* status_variants (status_off) */
static void test_status_off(void)
{
	m4_callfsm_fake_t fake;
	m4_callfsm_ops_t ops;
	setup(&fake, &ops);

	m4_callfsm_status_t s = m4_callfsm_status(&ops);
	CHECK(s.m4_present, "m4 present");
	CHECK(!s.call_active, M4_T("call is not active",
"звонок не активен"));
	CHECK(!s.state_present, "no state");
	CHECK(!s.stale_warning, "no stale warning");
}

/* status_variants (status_on) */
static void test_status_on(void)
{
	m4_callfsm_fake_t fake;
	m4_callfsm_ops_t ops;
	setup(&fake, &ops);
	m4_callfsm_on(&ops, 0, "manual");

	m4_callfsm_status_t s = m4_callfsm_status(&ops);
	CHECK(s.call_active, M4_T("ACTIVE",
"АКТИВЕН"));
	CHECK(s.state_present, "state present");
	CHECK(!s.stale_warning, "no stale warning");
	CHECK(strcmp(s.state_reason, "manual") == 0, "reason manual");
}

/* status_stale_state (profile changed by something else entirely, e.g. `pactl`) */
static void test_status_stale_state(void)
{
	m4_callfsm_fake_t fake;
	m4_callfsm_ops_t ops;
	setup(&fake, &ops);
	m4_callfsm_on(&ops, 0, "manual");
	strcpy(fake.profile, "a2dp-sink"); /* changed behind callfsm's back */

	m4_callfsm_status_t s = m4_callfsm_status(&ops);
	CHECK(s.state_present, M4_T("call-state exists",
"call-state есть"));
	CHECK(!s.call_active, M4_T("card is not on the call profile",
"карта не на профиле звонка"));
	CHECK(s.stale_warning, M4_T("warning about state/profile mismatch",
"предупреждение про рассинхрон state/профиля"));
}

/* status_manual_hfp (no saved state) */
static void test_status_manual_hfp_no_state(void)
{
	m4_callfsm_fake_t fake;
	m4_callfsm_ops_t ops;
	setup(&fake, &ops);
	strcpy(fake.profile, M4_CALLFSM_PROFILE_MSBC);

	m4_callfsm_status_t s = m4_callfsm_status(&ops);
	CHECK(s.call_active, M4_T("card is on the call profile",
"карта на профиле звонка"));
	CHECK(!s.state_present, M4_T("without saved state",
"без сохранённого состояния"));
	CHECK(s.stale_warning, M4_T("warning about manual HFP without state",
"предупреждение про ручной HFP без состояния"));
}

/* --auto / reason (reason_none) */
static void test_reason_none(void)
{
	m4_callfsm_fake_t fake;
	m4_callfsm_ops_t ops;
	setup(&fake, &ops);

	char out[M4_CALLFSM_REASON_LEN];
	m4_callfsm_reason(&ops, out, sizeof(out));
	CHECK(strcmp(out, "none") == 0, M4_T("reason: none, when the call is not active",
"reason: none, когда звонок не активен"));
}

/* auto_reason_manual */
static void test_reason_manual_default(void)
{
	m4_callfsm_fake_t fake;
	m4_callfsm_ops_t ops;
	setup(&fake, &ops);
	m4_callfsm_on(&ops, 0, "manual");

	char out[M4_CALLFSM_REASON_LEN];
	m4_callfsm_reason(&ops, out, sizeof(out));
	CHECK(strcmp(out, "manual") == 0, M4_T("reason: manual by default (without --auto)",
"reason: manual по умолчанию (без --auto)"));
	CHECK(strcmp(fake.state_reason, "manual") == 0, M4_T("call-state contains REASON=manual",
"call-state содержит REASON=manual"));
}

/* auto_reason_auto */
static void test_reason_auto(void)
{
	m4_callfsm_fake_t fake;
	m4_callfsm_ops_t ops;
	setup(&fake, &ops);
	m4_callfsm_on(&ops, 1, "auto");

	char out[M4_CALLFSM_REASON_LEN];
	m4_callfsm_reason(&ops, out, sizeof(out));
	CHECK(strcmp(out, "auto") == 0, M4_T("reason: auto after 'on --auto'",
"reason: auto после 'on --auto'"));
	CHECK(strcmp(fake.state_reason, "auto") == 0, M4_T("call-state contains REASON=auto",
"call-state содержит REASON=auto"));
}

/* auto_old_state_no_reason (backward compatibility) */
static void test_reason_old_state_no_reason_backcompat(void)
{
	m4_callfsm_fake_t fake;
	m4_callfsm_ops_t ops;
	setup(&fake, &ops);
	m4_callfsm_on(&ops, 1, "manual");
	fake.state_reason[0] = '\0'; /* simulate an old call-state without REASON */

	char out[M4_CALLFSM_REASON_LEN];
	m4_callfsm_reason(&ops, out, sizeof(out));
	CHECK(strcmp(out, "manual") == 0,
	      M4_T("reason: manual by default for old call-state without REASON (backward compat)",
"reason: manual по умолчанию для старого call-state без REASON (обратная совместимость)"));
}

/* auto_off_after_auto_on */
static void test_auto_off_after_auto_on(void)
{
	m4_callfsm_fake_t fake;
	m4_callfsm_ops_t ops;
	setup(&fake, &ops);
	m4_callfsm_on(&ops, 1, "auto");
	m4_callfsm_off(&ops, 1);

	CHECK(strcmp(fake.profile, "a2dp-sink") == 0, M4_T("'off' after 'on --auto' still restores the profile",
"'off' после 'on --auto' всё равно восстанавливает профиль"));
	CHECK(!fake.state_present, M4_T("'off' removes call-state (including one created by --auto)",
"'off' удаляет call-state (в т.ч. созданный --auto)"));
}

int main(void)
{
	test_on_no_m4();
	test_on_success_msbc();
	test_on_idempotent();
	test_off_success();
	test_on_fallback_cvsd();
	test_on_msbc_confirm_times_out_but_actually_landed();
	test_on_cvsd_confirm_times_out_but_actually_landed();
	test_off_music_default_confirm_times_out_but_actually_landed();
	test_on_both_fail();
	test_off_no_state();
	test_off_manual_profile_without_state();
	test_off_profile_confirm_fail_then_retry();
	test_off_a2dp_sink_missing_repaired();
	test_off_a2dp_sink_missing_repair_fails();
	test_off_manual_no_state_a2dp_missing_repaired();
	test_off_m4_disconnected_mid_call();
	test_off_helper_not_alive();
	test_quiet_suppresses_announce();
	test_status_off();
	test_status_on();
	test_status_stale_state();
	test_status_manual_hfp_no_state();
	test_reason_none();
	test_reason_manual_default();
	test_reason_auto();
	test_reason_old_state_no_reason_backcompat();
	test_auto_off_after_auto_on();

	if (failures > 0) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("callfsm: all checks passed (27 call-mode scenarios"
	       " + 3 live-bug (confirm) regressions + 3 repair-a2dp scenarios)\n");
	return 0;
}
