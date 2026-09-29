/* core/callfsm_fake.h -- deterministic in-memory m4_callfsm_ops_t
 * (callfsm_ops.h) implementation for tests/test_callfsm.c.
 * Fields directly model the pieces of PipeWire/BlueZ state callfsm.c
 * actually reads and writes, so scenarios can be built without a live
 * PipeWire connection:
 *   - connected/profile: the fake card's current connection state and
 *     active profile name.
 *   - fail_profiles: a `set_profile_confirm()` call for a profile in this
 *     list fails (returns 0) without changing `profile` -- models a
 *     profile switch BlueZ/PipeWire itself rejects.
 *   - helper_alive: the fake's answer to callfsm_ops.h's helper_alive().
 * No profile-confirmation delay is modelled: a
 * successful set_profile_confirm() always applies+confirms in one call.
 *
 * slow_profiles (added after the live CALL_ON bug): models the real
 * pw/callfsm_graph_adapter.c + bluez5-device.c race where a profile switch
 * IS actually applied server-side (bluez5-device.c's set_profile() sets
 * `this->profile` synchronously) but the adapter's confirm loop, which only
 * learns about it via a LATER `PW_DEVICE_CHANGE_MASK_PARAMS` info event
 * (fired from codec_switched(), once BlueZ's async HFP codec negotiation
 * actually completes), gives up first -- i.e. `set_profile_confirm()`
 * returns 0 (not confirmed in time) even though the profile really did
 * change. A profile in `slow_profiles` behaves exactly like that:
 * `set_profile_confirm()` still applies `profile` (unlike fail_profiles,
 * which does not apply it at all) but returns 0; a subsequent
 * `get_active_profile()` call correctly reports the new value, matching
 * live PipeWire's `SPA_PARAM_Profile` enum handler (which always reflects
 * `this->profile` immediately, synchronously -- see bluez5-device.c
 * impl_enum_params()/build_profile()).
 */
#ifndef MOMENTUM4_CORE_CALLFSM_FAKE_H
#define MOMENTUM4_CORE_CALLFSM_FAKE_H

#include "callfsm_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

#define M4_CALLFSM_FAKE_MAX_FAIL_PROFILES 4
#define M4_CALLFSM_FAKE_PROFILE_LEN 32
#define M4_CALLFSM_FAKE_REASON_LEN 16

typedef struct {
	int connected;
	char profile[M4_CALLFSM_FAKE_PROFILE_LEN];

	char fail_profiles[M4_CALLFSM_FAKE_MAX_FAIL_PROFILES][M4_CALLFSM_FAKE_PROFILE_LEN];
	int fail_profiles_count;

	/* see this header's top-of-file note on slow_profiles */
	char slow_profiles[M4_CALLFSM_FAKE_MAX_FAIL_PROFILES][M4_CALLFSM_FAKE_PROFILE_LEN];
	int slow_profiles_count;

	int helper_alive;

	int state_present;
	char state_profile[M4_CALLFSM_FAKE_PROFILE_LEN];
	char state_reason[M4_CALLFSM_FAKE_REASON_LEN];

	/* "repair-a2dp" (core/callfsm_ops.h's repair_profile_if_missing,
	 * doc comment). Models a2dp-sink being structurally MISSING
	 * from EnumProfile, matching bluez5-device.c's real behaviour
	 * (matching bluez5-device.c): while true, set_profile_confirm()/get_active_profile()
	 * for M4_CALLFSM_PROFILE_MUSIC_DEFAULT can NEVER succeed (mirrors
	 * "structurally absent, no amount of retrying helps"), exactly like
	 * fail_profiles -- the only way out is a successful
	 * repair_profile_if_missing() call, which flips this back to 0 (and
	 * applies the profile) if repair_a2dp_succeeds is true. */
	int a2dp_sink_missing;
	int repair_a2dp_succeeds; /* what a repair attempt achieves, IF one is actually made */
	int repair_a2dp_calls;    /* observability: how many times
				    * repair_profile_if_missing() was actually
				    * invoked with a2dp_sink_missing true (calls
				    * made while it was already false, i.e. "not
				    * needed", are NOT counted here -- see
				    * callfsm_fake.c, matches the "ONLY if
				    * missing" contract) */

	/* observability for tests -- e.g. to assert helper_alive() really was
	 * consulted even though its result never affects rc/message. */
	int set_profile_calls;
	int helper_alive_calls;
	int state_write_calls;
	int state_remove_calls;

	/* Bug 2 (see callfsm_ops.h's fix_mic_volume_if_needed
	 * doc comment) -- mirrors core/graph_ops_fake.h's have_mic_route/
	 * mic_volume/mic_mute at this narrower interface's level. */
	int mic_have_route;
	float mic_volume;
	int mic_mute;
	int fix_mic_volume_fails; /* if true, a fix that WOULD be needed
				    * returns -1 (not confirmed) instead of 1,
				    * and does not change mic_volume/mic_mute */
	int fix_mic_volume_calls; /* every call, including "nothing needed" */
} m4_callfsm_fake_t;

/* connected=1, profile="a2dp-sink", helper_alive=1, no fail_profiles, no
 * saved state -- a plain, healthy starting point for a scenario to build
 * on. */
void m4_callfsm_fake_init(m4_callfsm_fake_t *fake);

void m4_callfsm_fake_set_fail_profile(m4_callfsm_fake_t *fake, const char *profile);
void m4_callfsm_fake_clear_fail_profiles(m4_callfsm_fake_t *fake);

/* see this header's top-of-file note on slow_profiles */
void m4_callfsm_fake_set_slow_profile(m4_callfsm_fake_t *fake, const char *profile);

/* Bug 2: seeds the fake's mic Route state, mirroring
 * core/graph_ops_fake.h's m4_graph_ops_fake_set_mic_route(). */
void m4_callfsm_fake_set_mic_route(m4_callfsm_fake_t *fake, int have_route, float volume, int mute);

/* Returns an ops struct bound to `fake` via ctx. `fake` must outlive the
 * returned ops (no allocation happens here). */
m4_callfsm_ops_t m4_callfsm_fake_ops(m4_callfsm_fake_t *fake);

#ifdef __cplusplus
}
#endif

#endif /* MOMENTUM4_CORE_CALLFSM_FAKE_H */
