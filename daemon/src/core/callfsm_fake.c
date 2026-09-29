/* core/callfsm_fake.c -- see callfsm_fake.h. */
#include "callfsm_fake.h"

#include <stdio.h>
#include <string.h>

#include "callfsm.h" /* M4_CALLFSM_PROFILE_MUSIC_DEFAULT */

static m4_callfsm_fake_t *self_of(void *ctx)
{
	return (m4_callfsm_fake_t *)ctx;
}

static int fake_card_present(void *ctx, char *card_buf, size_t card_buf_len)
{
	m4_callfsm_fake_t *f = self_of(ctx);
	if (!f->connected)
		return 0;
	snprintf(card_buf, card_buf_len, "bluez_card.TEST");
	return 1;
}

static int fake_get_active_profile(void *ctx, const char *card, char *buf, size_t buflen)
{
	(void)card;
	m4_callfsm_fake_t *f = self_of(ctx);
	snprintf(buf, buflen, "%s", f->profile);
	return 0;
}

static int fake_is_fail_profile(const m4_callfsm_fake_t *f, const char *profile)
{
	for (int i = 0; i < f->fail_profiles_count; i++) {
		if (strcmp(f->fail_profiles[i], profile) == 0)
			return 1;
	}
	return 0;
}

static int fake_is_slow_profile(const m4_callfsm_fake_t *f, const char *profile)
{
	for (int i = 0; i < f->slow_profiles_count; i++) {
		if (strcmp(f->slow_profiles[i], profile) == 0)
			return 1;
	}
	return 0;
}

static int fake_set_profile_confirm(void *ctx, const char *card, const char *profile)
{
	(void)card;
	m4_callfsm_fake_t *f = self_of(ctx);
	f->set_profile_calls++;
	if (!f->connected)
		return 0;
	if (fake_is_fail_profile(f, profile))
		return 0;
	/* a2dp-sink structurally missing from EnumProfile: the
	 * profile literally cannot be applied, matching bluez5-device.c's
	 * set_param(Profile) rejecting it outright -- do NOT change f->profile
	 * either, same as fail_profiles above. */
	if (f->a2dp_sink_missing && strcmp(profile, M4_CALLFSM_PROFILE_MUSIC_DEFAULT) == 0)
		return 0;
	snprintf(f->profile, sizeof(f->profile), "%s", profile);
	if (fake_is_slow_profile(f, profile))
		return 0; /* applied, but "not confirmed in time" -- see slow_profiles note */
	return 1;
}

static int fake_state_write(void *ctx, const char *saved_profile, const char *reason)
{
	m4_callfsm_fake_t *f = self_of(ctx);
	f->state_write_calls++;
	f->state_present = 1;
	snprintf(f->state_profile, sizeof(f->state_profile), "%s", saved_profile != NULL ? saved_profile : "");
	snprintf(f->state_reason, sizeof(f->state_reason), "%s", reason != NULL ? reason : "manual");
	return 0;
}

static int fake_state_present(void *ctx)
{
	return self_of(ctx)->state_present;
}

static int fake_state_read(void *ctx, char *out_profile, size_t out_profile_len,
			    char *out_reason, size_t out_reason_len)
{
	m4_callfsm_fake_t *f = self_of(ctx);
	if (!f->state_present)
		return -1;
	snprintf(out_profile, out_profile_len, "%s", f->state_profile);
	snprintf(out_reason, out_reason_len, "%s", f->state_reason);
	return 0;
}

static void fake_state_remove(void *ctx)
{
	m4_callfsm_fake_t *f = self_of(ctx);
	f->state_remove_calls++;
	f->state_present = 0;
	f->state_profile[0] = '\0';
	f->state_reason[0] = '\0';
}

static int fake_helper_alive(void *ctx)
{
	m4_callfsm_fake_t *f = self_of(ctx);
	f->helper_alive_calls++;
	return f->helper_alive;
}

static int fake_repair_profile_if_missing(void *ctx, const char *card, const char *profile)
{
	(void)card;
	m4_callfsm_fake_t *f = self_of(ctx);

	/* This fake only models a2dp-sink repair (the only profile the real
	 * repair-a2dp path ever targets) -- and, matching the "ONLY if
	 * missing" contract (callfsm_ops.h), does nothing at all (does
	 * not even count as a call) unless the profile is actually missing
	 * right now. */
	if (strcmp(profile, M4_CALLFSM_PROFILE_MUSIC_DEFAULT) != 0 || !f->a2dp_sink_missing)
		return 0;

	f->repair_a2dp_calls++;
	if (!f->repair_a2dp_succeeds)
		return 0;

	f->a2dp_sink_missing = 0;
	snprintf(f->profile, sizeof(f->profile), "%s", profile);
	return 1;
}

static int fake_fix_mic_volume_if_needed(void *ctx, const char *card)
{
	(void)card;
	m4_callfsm_fake_t *f = self_of(ctx);

	f->fix_mic_volume_calls++;
	if (!f->mic_have_route)
		return 0; /* nothing observed right now -- see the real op's own
			   * doc comment on why this is a normal, non-error 0 */
	if (!f->mic_mute && f->mic_volume >= 0.99f)
		return 0; /* already fine -- no write, matches "no-op" contract */

	if (f->fix_mic_volume_fails)
		return -1; /* attempted, but not confirmed -- state left untouched,
			    * same as the real adapter's own bounded-timeout path */

	f->mic_mute = 0;
	f->mic_volume = 1.0f;
	return 1;
}

void m4_callfsm_fake_set_mic_route(m4_callfsm_fake_t *fake, int have_route, float volume, int mute)
{
	fake->mic_have_route = have_route;
	if (have_route) {
		fake->mic_volume = volume;
		fake->mic_mute = mute;
	} else {
		fake->mic_volume = 0.0f;
		fake->mic_mute = 0;
	}
}

void m4_callfsm_fake_init(m4_callfsm_fake_t *fake)
{
	memset(fake, 0, sizeof(*fake));
	fake->connected = 1;
	snprintf(fake->profile, sizeof(fake->profile), "a2dp-sink");
	fake->helper_alive = 1;
}

void m4_callfsm_fake_set_fail_profile(m4_callfsm_fake_t *fake, const char *profile)
{
	if (fake->fail_profiles_count >= M4_CALLFSM_FAKE_MAX_FAIL_PROFILES)
		return;
	snprintf(fake->fail_profiles[fake->fail_profiles_count], M4_CALLFSM_FAKE_PROFILE_LEN, "%s", profile);
	fake->fail_profiles_count++;
}

void m4_callfsm_fake_clear_fail_profiles(m4_callfsm_fake_t *fake)
{
	fake->fail_profiles_count = 0;
}

void m4_callfsm_fake_set_slow_profile(m4_callfsm_fake_t *fake, const char *profile)
{
	if (fake->slow_profiles_count >= M4_CALLFSM_FAKE_MAX_FAIL_PROFILES)
		return;
	snprintf(fake->slow_profiles[fake->slow_profiles_count], M4_CALLFSM_FAKE_PROFILE_LEN, "%s", profile);
	fake->slow_profiles_count++;
}

m4_callfsm_ops_t m4_callfsm_fake_ops(m4_callfsm_fake_t *fake)
{
	m4_callfsm_ops_t ops = {
		.ctx = fake,
		.card_present = fake_card_present,
		.get_active_profile = fake_get_active_profile,
		.set_profile_confirm = fake_set_profile_confirm,
		.state_write = fake_state_write,
		.state_present = fake_state_present,
		.state_read = fake_state_read,
		.state_remove = fake_state_remove,
		.helper_alive = fake_helper_alive,
		.repair_profile_if_missing = fake_repair_profile_if_missing,
		.fix_mic_volume_if_needed = fake_fix_mic_volume_if_needed,
	};
	return ops;
}
