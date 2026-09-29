/* core/profile_rules.h -- pure (no I/O) rules for mapping PipeWire bluez5
 * card profile names to the two "modes" momentum4 cares about
 * (AALL = A2DP music path, HFP = phone-call path). No
 * PipeWire/spa headers here on purpose -- this file, and its .c, must be
 * usable and testable with a plain `cc -std=gnu11`, without pkg-config, so
 * the unit tests in tests/test_profile_rules.c stay fast and dependency-free
 * (unit tests for the parsers -- node properties).
 *
 * Profile names themselves come from core/callfsm.h's compiled defaults
 * (PROFILE_MSBC, PROFILE_CVSD, PROFILE_MUSIC_DEFAULT) and from a live EnumProfile dump
 * -- confirmed against a live `pw-dump` read
 * (card 99, bluez_card.00_1B_66_00_00_01): profile "a2dp-sink"
 * is the auto/aggregate A2DP profile (its priority and description track
 * whichever A2DP codec BlueZ+WirePlumber actually negotiated -- aptX
 * Adaptive right now), "headset-head-unit" is HFP/mSBC, "headset-head-unit-cvsd"
 * is the CVSD fallback, "off" is the card disabled. Profiles named
 * "a2dp-sink-<codec>" (e.g. "a2dp-sink-aptx_hd") only appear in EnumProfile,
 * never observed as the *active* Profile in this project's setup, but are
 * mapped to AALL too on the reasonable assumption that they are still the
 * A2DP music path (m4_profile_mode() must not silently return UNKNOWN for a
 * profile family that is exercised elsewhere in the codebase).
 */
#ifndef MOMENTUM4_CORE_PROFILE_RULES_H
#define MOMENTUM4_CORE_PROFILE_RULES_H

#ifdef __cplusplus
extern "C" {
#endif

enum m4_mode {
	M4_MODE_UNKNOWN = 0, /* profile name not recognized (should not happen
			      * with a stock WirePlumber bluez5 monitor, but a
			      * future PipeWire/BlueZ version could rename
			      * things -- callers must handle this, not assert) */
	M4_MODE_OFF,         /* profile "off": card present but disabled */
	M4_MODE_AALL,        /* any "a2dp-sink"/"a2dp-sink-*" profile: A2DP music path */
	M4_MODE_HFP_MSBC,    /* "headset-head-unit": call path, codec mSBC */
	M4_MODE_HFP_CVSD,    /* "headset-head-unit-cvsd": call path, codec CVSD fallback */
};

/* Maps a PipeWire card Profile "name" field (SPA_PARAM_PROFILE_name) to an
 * m4_mode. NULL/empty input -> M4_MODE_UNKNOWN (never crashes). */
enum m4_mode m4_profile_mode(const char *profile_name);

/* Short machine-readable token, for --json ("aall"/"hfp_msbc"/"hfp_cvsd"/"off"/"unknown"). */
const char *m4_mode_token(enum m4_mode mode);

/* Short human string, in the current interface language (see
 * common/m4-i18n.h), for the screen-reader-friendly `status` text
 * (AALL/HFP mSBC/HFP CVSD/off/unknown). */
const char *m4_mode_human(enum m4_mode mode);

/* Case-insensitive Bluetooth address comparison (BlueZ/PipeWire always use
 * upper-case "AA:BB:CC:DD:EE:FF", but this must not depend on that). NULL is
 * only equal to NULL. */
int m4_bt_address_equal(const char *a, const char *b);

#ifdef __cplusplus
}
#endif

#endif /* MOMENTUM4_CORE_PROFILE_RULES_H */
