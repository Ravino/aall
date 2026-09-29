/* momentum4 :
 * shared, pure device-identity/allow-list policy used by THREE independent
 * gates that all need the same "is this remote peer one we should treat as
 * MOMENTUM-4-compatible" answer, from different amounts of information:
 *   - the aptx_adaptive SPA codec's own select_config()/validate_config()
 *     (patch 0009/0012) -- only ever sees the REMOTE caps bytes, no BlueZ
 *     device object at all (fill_caps() has no per-device context, see
 *     a2dp-codec-aptx.c's own comment) -- uses the FINGERPRINT half below;
 *   - the new endpoint_set_configuration() gate in bluez5-dbus.c (patch
 * 0014) -- has the full struct spa_bt_device
 *     (address, remote_endpoint_list with each peer's actual caps) -- uses
 *     BOTH halves (fingerprint over the real remote SEP caps, or a plain MAC
 *     allow-list if configured);
 *   - the native HFP backend's call-control (patch 0006/0013) and
 *     sco-restore (patch 0008/0013) gates -- have a struct spa_bt_device too
 *     (address + Name/Alias), but no A2DP "caps fingerprint" concept at all
 *     (HFP is not a codec negotiation) -- uses the IDENTITY half only
 *     (name-match prefix, or an exact configured address).
 *
 * SPDX-License-Identifier: MIT
 *
 * NOT upstream PipeWire. Out-of-tree addition, dependency-free (no
 * <spa/...>, no BlueZ/DBus types) like m4-sco-restore.h -- directly
 * unit-testable with a bare `gcc -std=gnu11` (test/device_policy_test.c).
 * The one piece of genuinely device-specific data (the actual byte
 * fingerprint of MOMENTUM 4's aptX Adaptive capabilities) stays where it
 * already lived before this file existed:
 * adaptive_caps_match_m4_fingerprint() in a2dp-codec-aptx.c (patch 0009) --
 * this file only decides WHICH of {always allow, always reject, fingerprint,
 * MAC list, name/address identity} applies, given a config string.
 */
#ifndef M4_DEVICE_POLICY_H
#define M4_DEVICE_POLICY_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* [adaptive] allow-devices : what
 * m4_device_policy_parse_allow_devices() resolved a raw config string to.
 * ANY and FINGERPRINT/MAC_LIST are mutually exclusive -- exactly one gate
 * applies, never layered (the config layering rule's "legacy config... if there is no new
 * one" == either/or is the same one-mode-at-a-time spirit this follows). */
enum m4_device_policy_mode {
	M4_DEVICE_POLICY_ANY = 0,   /* no check at all */
	M4_DEVICE_POLICY_OFF,       /* [hfp] call-control/sco-restore = off only */
	M4_DEVICE_POLICY_FINGERPRINT, /* [adaptive] allow-devices = adk-stereo (default) | m4 (alias) */
	M4_DEVICE_POLICY_MAC_LIST, /* [adaptive] allow-devices = <csv of MACs> */
	M4_DEVICE_POLICY_IDENTITY, /* [hfp] call-control/sco-restore = m4 (default) */
};

#define M4_DEVICE_POLICY_MAC_CSV_MAX 512

/* Parses [adaptive] allow-devices's raw string value:
 *   "any"                          -> M4_DEVICE_POLICY_ANY
 *   "adk-stereo" or "m4" (alias)   -> M4_DEVICE_POLICY_FINGERPRINT
 *   a comma-separated list of MAC addresses (AA:BB:CC:DD:EE:FF, each token
 *     trimmed of surrounding whitespace, validated as exactly 6 hex octets
 *     separated by ':') -> M4_DEVICE_POLICY_MAC_LIST, with the ORIGINAL
 *     (untrimmed-per-token but whitespace-stripped) csv copied verbatim into
 *     `mac_csv_out` for m4_device_policy_mac_list_contains() to consume later
 *   anything else (empty, garbage, a malformed MAC list)
 *                                  -> M4_DEVICE_POLICY_FINGERPRINT (the safe
 *     default), with *out_valid = false so the caller can log a warning --
 *     mirrors every other "best-effort config, never fatal" contract already
 *     used by m4-adaptive-config.c/common/m4-conf.c.
 * `value` may be NULL/empty (treated as unset -> FINGERPRINT, *out_valid =
 * true: "unset" is not itself a malformed value). `out_valid` may be NULL if
 * the caller does not care. */
enum m4_device_policy_mode m4_device_policy_parse_allow_devices(const char *value,
		char *mac_csv_out, size_t mac_csv_cap, bool *out_valid);

/* Parses [hfp] call-control / [hfp] sco-restore's raw string value
 * ("m4"|"all"|"off", the only values common/m4-conf-keys.def's registry
 * validates for those two ENUM keys -- anything else cannot reach this
 * function, so no separate "invalid" outcome is needed here, unlike the
 * allow-devices parser above):
 *   "off" -> M4_DEVICE_POLICY_OFF
 *   "all" -> M4_DEVICE_POLICY_ANY
 *   "m4" or anything else/NULL/empty -> M4_DEVICE_POLICY_IDENTITY (the safe
 *     default, matching common/m4-conf-keys.def's own "m4" default value) */
enum m4_device_policy_mode m4_device_policy_parse_hfp_mode(const char *value);

/* true if `address` (e.g. "00:1B:66:00:00:01", may be NULL) case-
 * insensitively equals one comma-separated token of `mac_csv` (may be NULL/
 * empty -> always false). Each token is trimmed of surrounding whitespace
 * before comparison; a malformed token (not exactly 6 ':'-separated hex
 * octets) is skipped, not treated as an error (the parse step above already
 * validated the whole list once; this function is deliberately still
 * tolerant of a hand-edited csv in case it changes between parse and use). */
bool m4_device_policy_mac_list_contains(const char *mac_csv, const char *address);

/* IDENTITY check for the HFP gates: true if `name` (BlueZ Name or Alias,
 * whichever the caller prefers to pass -- may be NULL) case-insensitively
 * starts with `name_match_prefix` (may be NULL/empty -> this half never
 * matches), OR `address` (may be NULL) case-insensitively equals
 * `configured_address` (only consulted if `configured_address` is non-empty
 * and not "auto" -- [device] address's own "unset" sentinel, common/
 * m4-conf-keys.def). Either half matching is sufficient (OR, not AND) --
 * "identity fingerprint, and optionally MAC" is read as "identity OR an
 * explicit stronger MAC pin", not "both required". */
bool m4_device_policy_identity_matches(const char *name, const char *address,
		const char *name_match_prefix, const char *configured_address);

#ifdef __cplusplus
}
#endif

#endif /* M4_DEVICE_POLICY_H */
