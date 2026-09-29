/* momentum4: resolves the [hfp] section of
 * momentum4.conf (common/m4-conf.c) plus the [device] identity keys the HFP
 * gates need, for backend-native.c's call-control (patch 0006/0013),
 * raw-at-capture (patch 0007) and sco-restore (patch 0008/0013) hooks.
 * SPDX-License-Identifier: MIT
 *
 * NOT upstream PipeWire. Built into libspa-bluez5.so (bluez5_sources) next
 * to m4-hfp-events.c/m4-sco-restore.c/m4-device-policy.c.
 *
 * "cache by mtime, read in the main loop": m4_hfp_config_get()
 * is meant to be called from backend-native.c's rfcomm_hfp_ag() (AT command
 * receipt) and the SCO-drop path (m4_sco_restore_on_hup()) -- BOTH already
 * run on PipeWire's main/D-Bus loop thread, never the RT audio thread, so
 * the stat()+occasional-fopen cost here is acceptable (same "not RT" budget
 * class as codec_init_adaptive()'s own config load, m4-adaptive-conf-
 * bridge.h). Internally: stat()s the two real override files momentum4.conf
 * can live at (/etc/momentum4/momentum4.conf and $XDG_CONFIG_HOME or
 * $HOME/.config/momentum4/momentum4.conf) on EVERY call (two cheap stat(2)
 * syscalls, no open/read), and only re-parses the whole layered config
 * (common/m4-conf.c: default < @datadir@ < /etc < user, several fopen()s)
 * when at least one of those two files' mtime (or existence) actually
 * changed since the last call -- i.e. editing momentum4.conf takes effect
 * on the very next HFP event, with no WirePlumber restart, at the cost of
 * two stat() calls per event in the (overwhelmingly common) unchanged case. */
#ifndef M4_HFP_CONFIG_H
#define M4_HFP_CONFIG_H

#include <stdbool.h>

#include "m4-device-policy.h"
#include "m4-sco-restore.h"

#ifdef __cplusplus
extern "C" {
#endif

struct m4_hfp_config {
	/* [hfp] call-control: OFF (never intercept), ANY (any device),
	 * IDENTITY (only a device matching name_match/device_address below). */
	enum m4_device_policy_mode call_control_mode;
	bool raw_at_capture;          /* [hfp] raw-at-capture */
	int raw_min_interval_ms;      /* [hfp] raw-min-interval-ms */

	/* [hfp] sco-restore, same three-value meaning as call_control_mode. */
	enum m4_device_policy_mode sco_restore_mode;
	struct m4_sco_restore_timing sco_timing;

	/* [device] name-match / address -- consulted by the IDENTITY mode
	 * above via m4_device_policy_identity_matches(). */
	char name_match[128];
	char device_address[64]; /* "auto" if [device] address is unset */

	char events_socket[512]; /* [hfp] events-socket */
};

/* Returns a pointer to a process-wide cached, up-to-date snapshot (see this
 * header's own doc comment above for the mtime-cache rule). Never NULL,
 * never fails -- an unreadable/absent momentum4.conf simply means every
 * field below is the compiled-in default from common/m4-conf-keys.def.
 * NOT thread-safe (no lock): correct as used today because every caller
 * (rfcomm AT-command receipt, SCO HUP handling) runs on the SAME PipeWire
 * main-loop thread, never concurrently with itself or the (separate) RT
 * audio thread, which never calls this at all. */
const struct m4_hfp_config *m4_hfp_config_get(void);

#ifdef __cplusplus
}
#endif

#endif /* M4_HFP_CONFIG_H */
