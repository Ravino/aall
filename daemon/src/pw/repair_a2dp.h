/* pw/repair_a2dp.h -- "repair-a2dp": recovers the a2dp-sink profile after
 * it structurally disappears from the M4's EnumProfile (the live CALL_ON
 * bug: bluez5-device.c's build_profile() refuses to
 * even LIST a2dp-sink once `connected_profiles` loses the A2DP_SINK bit --
 * at that point no amount of retrying set_profile(a2dp-sink) can ever
 * succeed; BlueZ has to be asked to reconnect that ONE profile first).
 *
 * Sequence (exactly what the task that added this specified, no more): (1)
 * org.bluez.Device1.ConnectProfile(A2DP Sink UUID) over D-Bus (via the
 * caller-supplied struct m4_bt_ops -- never a disconnect/reconnect of the
 * whole device, never anything beyond this one profile); (2) wait up to
 * `enum_wait_ms` (task default 10000) for "a2dp-sink" to (re)appear in the
 * M4's EnumProfile (via the caller-supplied struct graph_ops's cached,
 * push-updated state -- the SAME mechanism pw/callfsm_graph_adapter.c
 * already uses, see its adapter_set_profile_confirm()); (3)
 * set_profile("a2dp-sink", save=true) and wait up to `confirm_timeout_ms`
 * for it to become the ACTIVE profile. Never issues a disconnect of any
 * kind (I11/I12 still apply: the only PipeWire write this file ever makes
 * is that one set_profile() call, via the SAME `graph_ops` abstraction
 * everything else in momentum4d uses).
 */
#ifndef MOMENTUM4_PW_REPAIR_A2DP_H
#define MOMENTUM4_PW_REPAIR_A2DP_H

#include <systemd/sd-event.h>

#include "../bt/bt_ops.h"
#include "../core/graph_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

#define M4_REPAIR_A2DP_PROFILE "a2dp-sink"

/* Task-specified default: waits for a2dp-sink to reappear in EnumProfile
 * (up to 10s). */
#define M4_REPAIR_A2DP_DEFAULT_ENUM_WAIT_MS 10000
/* ConnectProfile's own D-Bus call bound -- generous (BlueZ's own SDP/L2CAP
 * reconnect for a single profile is normally sub-second to a few seconds;
 * this is a ceiling, not an expected duration). */
#define M4_REPAIR_A2DP_DEFAULT_CONNECT_TIMEOUT_MS 15000
/* set_profile()'s own confirm bound, once a2dp-sink IS enumerable -- same
 * order of magnitude as pw/callfsm_graph_adapter.c's own default (that
 * file's confirm_timeout_ms is normally overridden to 10000 by
 * cli/daemon_cmd.c; repair-a2dp reuses whatever the caller passes in,
 * falling back to this file's own default if 0/negative). */
#define M4_REPAIR_A2DP_DEFAULT_CONFIRM_TIMEOUT_MS 5000

enum m4_repair_a2dp_status {
	M4_REPAIR_A2DP_OK = 0,
	M4_REPAIR_A2DP_ERR_BAD_CONFIG,     /* NULL graph_ops/bt_ops/event/mac_address */
	M4_REPAIR_A2DP_ERR_NOT_PRESENT,    /* M4 device not currently in the registry */
	M4_REPAIR_A2DP_ERR_ENUM_TIMEOUT,   /* a2dp-sink never appeared in EnumProfile */
	M4_REPAIR_A2DP_ERR_SET_PROFILE,    /* set_profile() issued but never confirmed */
};

struct m4_repair_a2dp_config {
	const struct graph_ops *graph_ops; /* m4_graph_ops_pw or a fake */
	void *graph_handle;                /* an ALREADY OPEN handle (open()ed by the caller) */
	const struct m4_bt_ops *bt_ops;    /* m4_bt_ops_sdbus or a fake */
	sd_event *event;                   /* pumped reentrantly while waiting -- same technique
					     * and the same bounded-nesting caveat as
					     * pw/callfsm_graph_adapter.c's set_profile_confirm() */
	const char *mac_address;
	const char *bt_adapter;            /* NULL -> "hci0" (bt_ops_sdbus.c's own default) */

	int connect_timeout_ms;   /* <=0 -> M4_REPAIR_A2DP_DEFAULT_CONNECT_TIMEOUT_MS */
	int enum_wait_ms;         /* <=0 -> M4_REPAIR_A2DP_DEFAULT_ENUM_WAIT_MS */
	int confirm_timeout_ms;   /* <=0 -> M4_REPAIR_A2DP_DEFAULT_CONFIRM_TIMEOUT_MS */
};

struct m4_repair_a2dp_result {
	enum m4_repair_a2dp_status status;
	char message[256]; /* human-readable (Russian), suitable to print/log as-is */

	int connect_profile_failed; /* observability for logs/tests: ConnectProfile itself
				      * returned an error, but the sequence continued anyway
				      * (see repair_a2dp.c's top comment on why) */
	double enum_wait_elapsed_s;
	double confirm_elapsed_s;
};

/* Runs against an ALREADY OPEN graph_ops handle and the CALLER's sd_event
 * (production: the daemon's own long-lived ones; tests: a fake graph +
 * a throwaway sd_event). Never opens or closes anything graph_ops-related
 * itself -- that is `cfg->graph_ops`'s open()/close(), the caller's job
 * (mirrors pw/callfsm_graph_adapter.c sharing the daemon's handle instead
 * of opening a second connection). */
struct m4_repair_a2dp_result m4_repair_a2dp_run(const struct m4_repair_a2dp_config *cfg);

/* Self-contained variant for a one-shot CLI process (no daemon, no
 * caller-owned event loop) -- mirrors pw/graph_pw.h's
 * m4_pw_query_snapshot()'s one-shot pattern, but for a write action:
 * creates its own private sd_event + graph_ops connection, waits up to
 * `open_settle_ms` (<=0 -> 3000) for the M4 device to even show up in the
 * registry, then calls m4_repair_a2dp_run(), then closes everything before
 * returning. `graph_ops`/`bt_ops` are still caller-supplied (tests pass
 * fakes; cli/repair_a2dp.c passes m4_graph_ops_pw/m4_bt_ops_sdbus) -- only
 * the graph_ops CONNECTION is private to this call. */
struct m4_repair_a2dp_result m4_repair_a2dp_run_standalone(
	const struct graph_ops *graph_ops, const struct m4_bt_ops *bt_ops, const char *mac_address,
	const char *bt_adapter, int open_settle_ms, int connect_timeout_ms, int enum_wait_ms,
	int confirm_timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* MOMENTUM4_PW_REPAIR_A2DP_H */
