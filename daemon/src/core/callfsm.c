/* core/callfsm.c -- see callfsm.h. Implements the same call-toggle logic
 * a predecessor tool used to implement directly with pactl/wait loops.
 */
#include "callfsm.h"

#include "../../../common/m4-i18n.h"

#include <stdio.h>
#include <string.h>

/* See callfsm.h's own comment on M4_CALLFSM_PROFILE_MSBC/CVSD/MUSIC_DEFAULT:
 * these three module-level buffers start at the compiled defaults and are
 * the ONLY thing m4_callfsm_set_profile_names() ever touches -- every use
 * site below reads g_profile_* (never the raw macros directly, except right
 * here for the initial value), so a config-driven override applies
 * uniformly to is_call_profile()'s own classification too. */
static char g_profile_msbc[M4_CALLFSM_PROFILE_LEN] = M4_CALLFSM_PROFILE_MSBC;
static char g_profile_cvsd[M4_CALLFSM_PROFILE_LEN] = M4_CALLFSM_PROFILE_CVSD;
static char g_profile_music[M4_CALLFSM_PROFILE_LEN] = M4_CALLFSM_PROFILE_MUSIC_DEFAULT;

void m4_callfsm_set_profile_names(const char *msbc, const char *cvsd, const char *music_default)
{
	if (msbc != NULL && msbc[0] != '\0')
		snprintf(g_profile_msbc, sizeof(g_profile_msbc), "%s", msbc);
	if (cvsd != NULL && cvsd[0] != '\0')
		snprintf(g_profile_cvsd, sizeof(g_profile_cvsd), "%s", cvsd);
	if (music_default != NULL && music_default[0] != '\0')
		snprintf(g_profile_music, sizeof(g_profile_music), "%s", music_default);
}

int m4_callfsm_is_call_profile(const char *profile)
{
	if (profile == NULL)
		return 0;
	return strcmp(profile, g_profile_msbc) == 0 ||
	       strcmp(profile, g_profile_cvsd) == 0;
}

/* Wraps ops->set_profile_confirm(): a `0` return from it means "not
 * confirmed within the implementation's own timeout", NOT necessarily "the
 * switch failed" ("the live CALL_ON bug" observed in production):
 * pw/callfsm_graph_adapter.c's confirm loop only learns about a
 * profile change via graph_pw.c's cached state, which is only refreshed on
 * a `PW_DEVICE_CHANGE_MASK_PARAMS` info event -- and for an HSP/HFP switch,
 * bluez5-device.c only fires that event from codec_switched(), once
 * BlueZ's async SCO/AT-command codec negotiation actually completes, which
 * can legitimately take longer than one confirm window. The card's
 * SPA_PARAM_Profile itself, however, already reflects the new profile
 * synchronously (bluez5-device.c sets `this->profile` before starting the
 * async negotiation) -- so a fresh get_active_profile() can catch a switch
 * that genuinely landed just after (or independently of) the confirm
 * timeout, without re-issuing a second, different set_profile() request
 * that would race/interrupt a switch that may still be converging (a
 * likely cause of the M4's A2DP transport disappearing afterwards, in a
 * live capture). This is a mitigation, not a full fix for the underlying
 * race -- a longer confirm_timeout_ms for call profiles would be the
 * complementary fix. */
static int confirm_or_verify(const m4_callfsm_ops_t *ops, const char *card, const char *profile)
{
	char actual[M4_CALLFSM_PROFILE_LEN] = {0};

	if (ops->set_profile_confirm(ops->ctx, card, profile))
		return 1;
	if (ops->get_active_profile(ops->ctx, card, actual, sizeof(actual)) == 0 &&
	    strcmp(actual, profile) == 0)
		return 1;
	return 0;
}

/* "repair-a2dp" (see callfsm_ops.h's
 * repair_profile_if_missing doc comment): a LAST resort after
 * confirm_or_verify() has already given up on `profile`. Used only by
 * m4_callfsm_off()'s two restore-to-a2dp-sink call sites below (never by
 * on(), and never for mSBC/CVSD) -- ops->repair_profile_if_missing() itself
 * is responsible for checking whether `profile` is ACTUALLY missing from
 * EnumProfile before doing anything (see that field's own doc comment), so
 * this wrapper can call it unconditionally here without callfsm.c needing
 * to know anything about EnumProfile itself (staying free of graph-shaped
 * state, per this file's own top comment). On a successful repair, re-reads
 * the active profile directly (get_active_profile()) rather than calling
 * confirm_or_verify() again, which would re-issue ANOTHER
 * set_profile_confirm() -- redundant, since repair_profile_if_missing()'s
 * own contract already requires it to leave `profile` confirmed active
 * before returning 1. */
static int confirm_or_repair(const m4_callfsm_ops_t *ops, const char *card, const char *profile)
{
	char actual[M4_CALLFSM_PROFILE_LEN] = {0};

	if (confirm_or_verify(ops, card, profile))
		return 1;
	if (ops->repair_profile_if_missing == NULL)
		return 0;
	if (!ops->repair_profile_if_missing(ops->ctx, card, profile))
		return 0;
	return ops->get_active_profile(ops->ctx, card, actual, sizeof(actual)) == 0 &&
	       strcmp(actual, profile) == 0;
}

m4_callfsm_result_t m4_callfsm_on(const m4_callfsm_ops_t *ops, int quiet, const char *reason)
{
	m4_callfsm_result_t r = {0};
	char card[M4_CALLFSM_CARD_LEN] = {0};

	if (ops->card_present(ops->ctx, card, sizeof(card)) != 1) {
		r.rc = M4_CALLFSM_RC_FAILED;
		snprintf(r.message, sizeof(r.message), M4_T("MOMENTUM 4 is not connected -- nothing changed.",
"MOMENTUM 4 не подключены -- ничего не изменено."));
		return r;
	}

	char cur_profile[M4_CALLFSM_PROFILE_LEN] = {0};
	if (ops->get_active_profile(ops->ctx, card, cur_profile, sizeof(cur_profile)) != 0)
		cur_profile[0] = '\0';

	if (m4_callfsm_is_call_profile(cur_profile)) {
		if (ops->state_present(ops->ctx)) {
			/* already in call mode, call-state present -- idempotent no-op:
			 * touch nothing, no announce. */
			r.rc = M4_CALLFSM_RC_OK;
			snprintf(r.message, sizeof(r.message), M4_T("Already in call mode (%s).",
"Уже в режиме звонка (%s)."), cur_profile);
			return r;
		}
		/* card manually switched to a call profile out of band
		 * (e.g. `pactl set-card-profile`): create a fallback rollback state,
		 * do not touch the profile itself. */
		ops->state_write(ops->ctx, g_profile_music, reason);
		r.rc = M4_CALLFSM_RC_OK;
		snprintf(r.message, sizeof(r.message),
			M4_T("Manual call mode detected with no saved state. "
			"Rollback state created (target: profile %s).",
"Обнаружен ручной режим звонка без сохранённого состояния. "
			"Состояние отката создано (цель: профиль %s)."),
			g_profile_music);
		return r;
	}

	/* An existing stale call-state is overwritten,
	 * not treated as an error -- this "on" proceeds as a normal one. */
	ops->state_write(ops->ctx, cur_profile, reason);

	const char *chosen = NULL;
	const char *codec_label = NULL;
	if (confirm_or_verify(ops, card, g_profile_msbc)) {
		chosen = g_profile_msbc;
		codec_label = "mSBC";
	} else if (confirm_or_verify(ops, card, g_profile_cvsd)) {
		chosen = g_profile_cvsd;
		codec_label = "CVSD";
	}

	if (chosen == NULL) {
		/* Best-effort rollback, its result is
		 * intentionally ignored. */
		(void)ops->set_profile_confirm(ops->ctx, card, cur_profile);
		ops->state_remove(ops->ctx);
		r.rc = M4_CALLFSM_RC_FAILED;
		snprintf(r.message, sizeof(r.message),
			M4_T("ERROR: failed to switch the Momentum 4 into call mode (neither mSBC nor CVSD).",
"ОШИБКА: не удалось переключить Momentum 4 в режим звонка (ни mSBC, ни CVSD)."));
		return r;
	}

	r.rc = M4_CALLFSM_RC_OK;
	r.announce = quiet ? M4_CALLFSM_ANNOUNCE_NONE : M4_CALLFSM_ANNOUNCE_ON;
	snprintf(r.codec_label, sizeof(r.codec_label), "%s", codec_label);
	r.cvsd_fallback_warning = (strcmp(chosen, g_profile_msbc) != 0);
	snprintf(r.message, sizeof(r.message), M4_T("Call mode enabled: profile %s (codec %s).",
"Режим звонка включён: профиль %s (кодек %s)."), chosen, codec_label);

	/* Bug 2 (see callfsm_ops.h's fix_mic_volume_if_needed
	 * doc comment): best-effort, right at HFP entry -- NEVER changes rc
	 * above (already M4_CALLFSM_RC_OK), only appends to the human-readable
	 * message so the caller's log line (daemon.c logs r.message verbatim)
	 * carries the outcome without a second, separate log call. */
	if (ops->fix_mic_volume_if_needed != NULL) {
		/* mic-on-enter: fix_mic_volume_if_needed()'s target is no
		 * longer always "100%/unmuted" (`[call] mic-on-enter = off`/
		 * `remember` can target muted instead, callfsm_graph_adapter.c's
		 * own doc comment) -- the message stays direction-neutral rather
		 * than guessing which way it went; the adapter's own log line
		 * already states the concrete target. */
		r.mic_volume_fix = ops->fix_mic_volume_if_needed(ops->ctx, card);
		if (r.mic_volume_fix == 1) {
			size_t len = strlen(r.message);
			snprintf(r.message + len, sizeof(r.message) - len,
				M4_T(" M4 microphone set to the mic-on-enter policy state.",
" Микрофон M4 приведён к состоянию по политике mic-on-enter."));
		} else if (r.mic_volume_fix < 0) {
			size_t len = strlen(r.message);
			snprintf(r.message + len, sizeof(r.message) - len,
				M4_T(" WARNING: failed to confirm the M4 microphone state.",
" ПРЕДУПРЕЖДЕНИЕ: не удалось подтвердить состояние микрофона M4."));
		}
	}
	return r;
}

m4_callfsm_result_t m4_callfsm_off(const m4_callfsm_ops_t *ops, int quiet)
{
	m4_callfsm_result_t r = {0};
	char card[M4_CALLFSM_CARD_LEN] = {0};
	int have_card = (ops->card_present(ops->ctx, card, sizeof(card)) == 1);

	if (!ops->state_present(ops->ctx)) {
		if (have_card) {
			char active[M4_CALLFSM_PROFILE_LEN] = {0};
			if (ops->get_active_profile(ops->ctx, card, active, sizeof(active)) != 0)
				active[0] = '\0';
			if (m4_callfsm_is_call_profile(active)) {
				/* Manual switch to a call profile,
				 * no callfsm state -- restore the music default.
				 * confirm_or_repair() (not just confirm_or_verify()): see
				 * this file's own comment on it and callfsm_ops.h's
				 * repair_profile_if_missing doc -- a last-resort
				 * repair-a2dp attempt, only actually triggered if
				 * a2dp-sink is structurally missing from EnumProfile. */
				int ok = confirm_or_repair(ops, card, g_profile_music);
				if (ok) {
					r.rc = M4_CALLFSM_RC_OK;
					r.announce = quiet ? M4_CALLFSM_ANNOUNCE_NONE : M4_CALLFSM_ANNOUNCE_OFF;
					snprintf(r.message, sizeof(r.message),
						M4_T("Momentum 4 profile restored to %s.",
"Профиль Momentum 4 возвращён на %s."), g_profile_music);
					return r;
				}
				r.rc = M4_CALLFSM_RC_FAILED;
				snprintf(r.message, sizeof(r.message),
					M4_T("ERROR: profile did not confirm as %s.",
"ОШИБКА: профиль не подтвердился как %s."), g_profile_music);
				return r;
			}
		}
		/* Idempotent no-op. */
		r.rc = M4_CALLFSM_RC_OK;
		snprintf(r.message, sizeof(r.message), M4_T("Call is not active -- nothing to turn off.",
"Звонок не активен -- нечего выключать."));
		return r;
	}

	char saved_profile[M4_CALLFSM_PROFILE_LEN] = {0};
	char saved_reason[M4_CALLFSM_REASON_LEN] = {0};
	if (ops->state_read(ops->ctx, saved_profile, sizeof(saved_profile), saved_reason, sizeof(saved_reason)) != 0 ||
	    saved_profile[0] == '\0') {
		snprintf(saved_profile, sizeof(saved_profile), "%s", g_profile_music);
	}

	if (!have_card) {
		/* State is deliberately KEPT --
		 * this is the only trace that M4 may still be in call mode. */
		r.rc = M4_CALLFSM_RC_FAILED;
		snprintf(r.message, sizeof(r.message),
			M4_T("Momentum 4 is not connected -- call state KEPT until the headset reconnects "
			"(run 'momentum4 call off' again once it is visible).",
"Momentum 4 не подключены -- состояние звонка СОХРАНЕНО до подключения наушников "
			"(запустите 'momentum4 call off' ещё раз, когда они будут видны)."));
		return r;
	}

	/* confirm_or_repair() (not confirm_or_verify()): `saved_profile` is
	 * g_profile_music ("a2dp-sink") in every scenario
	 * this project's on() ever records it as -- confirm_or_repair()'s own
	 * repair op no-ops for anything else, so this is safe even in the
	 * (currently unreached) case saved_profile were something else. */
	int profile_ok = confirm_or_repair(ops, card, saved_profile);
	if (!profile_ok) {
		/* State is kept for a retry. */
		r.rc = M4_CALLFSM_RC_FAILED;
		snprintf(r.message, sizeof(r.message),
			M4_T("ERROR: profile did not confirm as %s -- call-state KEPT, retry 'momentum4 call off'.",
"ОШИБКА: профиль не подтвердился как %s -- call-state СОХРАНЁН, повторите 'momentum4 call off'."),
			saved_profile);
		return r;
	}

	if (strcmp(saved_profile, g_profile_music) == 0) {
		/* helper_alive() is consulted only for the
		 * log, never affects rc/message on this path (result deliberately
		 * discarded); graph_fake.c counts calls
		 * so tests can assert it was actually consulted. */
		(void)ops->helper_alive(ops->ctx);
	}

	ops->state_remove(ops->ctx);
	r.rc = M4_CALLFSM_RC_OK;
	r.announce = quiet ? M4_CALLFSM_ANNOUNCE_NONE : M4_CALLFSM_ANNOUNCE_OFF;
	snprintf(r.message, sizeof(r.message), M4_T("Call disabled, profile restored to %s.",
"Звонок выключен, профиль возвращён на %s."), saved_profile);
	return r;
}

m4_callfsm_status_t m4_callfsm_status(const m4_callfsm_ops_t *ops)
{
	m4_callfsm_status_t s = {0};
	char card[M4_CALLFSM_CARD_LEN] = {0};

	s.m4_present = (ops->card_present(ops->ctx, card, sizeof(card)) == 1);
	if (!s.m4_present)
		return s;

	if (ops->get_active_profile(ops->ctx, card, s.active_profile, sizeof(s.active_profile)) != 0)
		s.active_profile[0] = '\0';
	s.call_active = m4_callfsm_is_call_profile(s.active_profile);
	s.state_present = ops->state_present(ops->ctx);

	if (s.state_present) {
		if (ops->state_read(ops->ctx, s.state_saved_profile, sizeof(s.state_saved_profile),
				     s.state_reason, sizeof(s.state_reason)) != 0) {
			s.state_saved_profile[0] = '\0';
			s.state_reason[0] = '\0';
		}
		s.stale_warning = !s.call_active;
	} else {
		s.stale_warning = s.call_active;
	}
	return s;
}

void m4_callfsm_reason(const m4_callfsm_ops_t *ops, char *out, size_t outlen)
{
	if (!ops->state_present(ops->ctx)) {
		snprintf(out, outlen, "none");
		return;
	}
	char profile[M4_CALLFSM_PROFILE_LEN] = {0};
	char reason[M4_CALLFSM_REASON_LEN] = {0};
	if (ops->state_read(ops->ctx, profile, sizeof(profile), reason, sizeof(reason)) != 0 || reason[0] == '\0')
		snprintf(out, outlen, "manual"); /* backward compat: old state files had no REASON field */
	else
		snprintf(out, outlen, "%s", reason);
}
