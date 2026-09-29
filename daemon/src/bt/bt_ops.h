/* bt/bt_ops.h -- abstract interface for the ONE BlueZ D-Bus call this
 * project makes directly (not through PipeWire/WirePlumber):
 * org.bluez.Device1.ConnectProfile(), used by pw/repair_a2dp.c to recover
 * the A2DP Sink profile after it structurally disappears from the M4's
 * EnumProfile . Same reason
 * core/graph_ops.h exists instead of pw/graph_pw.c's struct graph_ops being
 * hardcoded everywhere: this abstraction (deliberately requested by the
 * task that added it: "D-Bus through an abstraction, with a fake for
 * tests") lets
 * pw/repair_a2dp.c, core/callfsm.c's repair path (via the adapter), and
 * their tests all run WITHOUT a real system D-Bus connection.
 */
#ifndef MOMENTUM4_BT_BT_OPS_H
#define MOMENTUM4_BT_BT_OPS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A2DP Sink UUID (Bluetooth SIG-assigned, 16-bit UUID 0x110B expanded to
 * the full 128-bit Bluetooth Base UUID form -- the only form
 * org.bluez.Device1.ConnectProfile() accepts, confirmed by reading BlueZ
 * 5.66's src/device.c device_connect_profile()/find_service() against
 * work/src/bluez-5.66). */
#define M4_BT_UUID_A2DP_SINK "0000110b-0000-1000-8000-00805f9b34fb"

struct m4_bt_ops {
	void *ctx;

	/* Issues org.bluez.Device1.ConnectProfile(uuid) on the device at
	 * "/org/bluez/<bt_adapter>/dev_<mac with ':' -> '_'>" over the D-Bus
	 * SYSTEM bus, waiting up to timeout_ms (<=0 -> implementation
	 * default) for BlueZ's reply. Returns 0 on success -- INCLUDING a
	 * benign "this profile is already connected"-shaped BlueZ error,
	 * which this project treats as success (the desired end state
	 * already holds, see bt_ops_sdbus.c for exactly which error
	 * shapes qualify). Returns -1 on any other failure (device
	 * absent, D-Bus unreachable, genuinely refused, timed out).
	 * `err_buf` (capacity `err_buf_len`, NUL-terminated, may be left
	 * "" on success) is filled with a short human-readable reason
	 * either way -- best-effort, never a substitute for the return
	 * code. Never blocks longer than timeout_ms plus a small,
	 * bounded connection-setup overhead. */
	int (*connect_profile)(void *ctx, const char *mac_address, const char *bt_adapter,
				const char *uuid, int timeout_ms, char *err_buf, size_t err_buf_len);
};

/* Real implementation (sd-bus, system bus). Opens and closes a fresh bus
 * connection inside each connect_profile() call -- this is a rare, manual
 * recovery action (`momentum4 repair-a2dp`, an emergency-rollback tool, or
 * one automatic attempt from callfsm_off()), not a hot path, so a few extra milliseconds
 * per call is a good trade for having no persistent connection lifecycle to
 * manage. `ctx` is unused (NULL). */
extern const struct m4_bt_ops m4_bt_ops_sdbus;

#ifdef __cplusplus
}
#endif

#endif /* MOMENTUM4_BT_BT_OPS_H */
