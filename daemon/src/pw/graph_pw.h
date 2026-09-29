/* pw/graph_pw.h -- minimal, READ-ONLY PipeWire registry client: connect to
 * PipeWire, find the M4 card by
 * api.bluez5.address, and read the current profile and EnumProfile. Issues no
 * pw_device_set_param()/pw_core_create_object()/etc. call, ever -- the
 * read-only invariant applies from day one, not just once the daemon exists.
 *
 * Caller must have already called pw_init() once for the process (main.c
 * does this) before calling m4_pw_query_snapshot(); this file never calls
 * pw_deinit() either, for the same reason.
 */
#ifndef MOMENTUM4_PW_GRAPH_PW_H
#define MOMENTUM4_PW_GRAPH_PW_H

#include <stdbool.h>
#include <stddef.h>

#include "../core/graph_ops.h"
#include "../core/profile_rules.h"

#ifdef __cplusplus
extern "C" {
#endif

enum m4_pw_query_status {
	M4_PW_OK = 0,      /* query ran to completion; check out->m4_present etc. */
	M4_PW_ERR_CONNECT, /* could not connect to PipeWire at all (not running,
			    * no permission, no PIPEWIRE_REMOTE/XDG_RUNTIME_DIR) --
			    * `out` is zeroed */
};

#define M4_PW_STR_LEN 128

struct m4_pw_snapshot {
	bool m4_present;   /* a Device global with our MAC exists in the registry */
	bool m4_connected; /* its api.bluez5.connection prop == "connected" */
	char device_description[M4_PW_STR_LEN];

	bool have_profile;
	enum m4_mode mode;
	char profile_name[64];
	char profile_description[M4_PW_STR_LEN];
	unsigned enum_profile_count; /* number of SPA_PARAM_EnumProfile entries seen */

	bool have_codec;
	char codec[32]; /* api.bluez5.codec of the matching bluez5 Node, e.g. "aptx_adaptive" */

	bool timed_out; /* true if `timeout_ms` elapsed before all info arrived;
			  * whatever was collected so far is still returned */
};

/* Connects to the default PipeWire remote, looks up the device whose
 * api.bluez5.address matches `mac_address` (case-insensitive), and fills
 * `out`. Blocks the calling thread for at most ~timeout_ms (runs a private
 * sd_event loop to completion inside this call -- this is a one-shot
 * query, not meant to be nested inside a caller's own event loop). Always
 * zeroes and then fills `out` before returning M4_PW_OK; returns
 * M4_PW_ERR_CONNECT (and leaves `out` zeroed) only if the connection to
 * PipeWire itself could not be established. */
enum m4_pw_query_status m4_pw_query_snapshot(const char *mac_address, int timeout_ms,
					      struct m4_pw_snapshot *out);

/* Pure helpers, exposed (and not `static`) purely so tests/test_graph_props.c
 * can exercise them without a live PipeWire connection -- the node
 * property matching rules m4_pw_query_snapshot() uses
 * to identify the M4 Device/Node from registry.global's REDUCED property set
 * (see graph_pw.c's top-of-file comment for why: it does not include
 * api.bluez5.address/codec at all, only after binding). */

/* Builds the PW_KEY_DEVICE_NAME bluez5-device.c gives the M4's card Device:
 * "bluez_card." + mac_address with every ':' replaced by '_' (confirmed
 * against a live `pw-dump` read: "bluez_card.AA_BB_CC_DD_EE_FF"
 * for "AA:BB:CC:DD:EE:FF"). Truncates (still NUL-terminated) if out_cap is
 * too small for "bluez_card." + mac_address. */
void m4_pw_build_device_name(const char *mac_address, char *out, size_t out_cap);

/* True if a Node's PW_KEY_MEDIA_CLASS property names it a playback sink
 * (substring match: PipeWire's own media class values are e.g. "Audio/Sink",
 * and this project has never seen a bare "Sink" appear in an unrelated
 * class). NULL is never a sink. */
bool m4_pw_media_class_is_sink(const char *media_class);

/* True if (res, message) is the benign "client destroyed a resource the
 * server had already destroyed itself" race, seen while switching a live
 * call on --
 * see m4_graph_error_is_benign_stale_destroy()'s own comment in graph_pw.c
 * for the full confirmed mechanism. Pure string/int matching, no I/O --
 * exposed for tests/test_graph_props.c, same convention as
 * m4_pw_media_class_is_sink() above. */
bool m4_graph_error_is_benign_stale_destroy(int res, const char *message);

/* --- long-lived struct graph_ops implementation ---
 *
 * `m4_graph_ops_pw` (declared in ../core/graph_ops.h, defined in
 * graph_pw.c) is a SEPARATE code path from m4_pw_query_snapshot() above:
 * it keeps a persistent PipeWire connection alive inside a caller-supplied
 * sd_event loop and pushes on_state_changed() callbacks, instead of
 * blocking once to completion. It shares only the pure helpers on this
 * page (m4_pw_build_device_name/m4_pw_media_class_is_sink) and the two
 * live-tested lessons documented at the top of graph_pw.c (pw_core_sync's
 * real seq; registry.global's reduced props) -- it does NOT reuse or
 * modify m4_pw_query_snapshot()'s internal state machine, so `momentum4
 * status` is unaffected by this addition. */

#ifdef __cplusplus
}
#endif

#endif /* MOMENTUM4_PW_GRAPH_PW_H */
