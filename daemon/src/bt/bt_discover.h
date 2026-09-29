/* bt/bt_discover.h -- read-only BlueZ auto-discovery for `[device] address =
 * auto` / `adapter = auto`.
 *
 * Deliberately the SECOND, independent D-Bus call this project makes
 * directly (the first is bt_ops.h's ConnectProfile) -- this one is
 * READ-ONLY (org.freedesktop.DBus.ObjectManager.GetManagedObjects on
 * org.bluez), never writes anything, never powers an adapter, never pairs/
 * connects/removes a device. Used exactly once per process, lazily, by
 * util/config.c when the effective `[device] address` or `adapter` config
 * value is the literal string "auto" -- see that file's own doc comment.
 *
 * Selection rule: the first (BlueZ's own GetManagedObjects order, not
 * sorted) org.bluez.Device1 that is Paired AND whose Alias (falling back
 * to Name if Alias is empty/absent) has `name_match` as a case-insensitive
 * PREFIX. Compatibility with the aptX Adaptive SEP is NOT checked here:
 * SEP/codec capability enumeration lives entirely inside the SPA plugin's
 * own MediaEndpoint objects, which are not exposed on a per-Device1 basis
 * this file could cheaply filter on without depending on bluez5's own
 * internal codec byte layout (that filter, `[adaptive] allow-devices`, is
 * already the plugin's own job). Matching by
 * name-match here is the same signal already relied on
 * for the HFP-side filters ([hfp] call-control/sco-restore = m4) and is
 * sufficient to distinguish "my M4" from an unrelated paired device in the
 * overwhelmingly common single-headset case; a stronger, codec-aware check
 * was not attempted here.
 */
#ifndef MOMENTUM4_BT_BT_DISCOVER_H
#define MOMENTUM4_BT_BT_DISCOVER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Queries the system D-Bus (org.bluez) for the first PAIRED Device1 whose
 * Alias (or, if Alias is empty/absent, Name) starts with `name_match`
 * (case-insensitive; empty/NULL `name_match` matches nothing -- an empty
 * prefix would match every paired device, which is never the intent of
 * "auto"). On success (0), writes the device's Address ("AA:BB:CC:DD:EE:FF")
 * into `out_mac` and the adapter's bare name ("hci0", from the device's own
 * Adapter property, falling back to parsing its own object path if that
 * property is somehow absent) into `out_adapter` -- either output pointer
 * may be NULL if the caller only wants the other one. Returns -1 if BlueZ is
 * unreachable, no matching device exists, or any D-Bus error occurs --
 * NEVER blocks longer than a single synchronous D-Bus round trip's own
 * kernel/socket timeout, and never modifies any BlueZ state -- discovery
 * is strictly read-only. `err_buf` (capacity `err_buf_cap`, may be NULL/0) is filled
 * with a short, human-readable reason on failure -- best-effort, never a
 * substitute for the return code. */
int m4_bt_discover_auto(const char *name_match, char *out_mac, size_t out_mac_cap, char *out_adapter,
			 size_t out_adapter_cap, char *err_buf, size_t err_buf_cap);

#ifdef __cplusplus
}
#endif

#endif /* MOMENTUM4_BT_BT_DISCOVER_H */
