/* core/graph_ops.h -- minimal abstraction the daemon core (callfsm.c,
 * automode.c, developed in parallel) uses to
 * read and change the M4 card's PipeWire Profile, without depending on
 * libpipewire directly.
 *
 * STATUS (this file's own provenance): at the time this was written,
 * core/callfsm.c/graph_fake.c did not
 * exist yet in the tree (`find src -iname '*callfsm*' -o -iname
 * '*graph_fake*'` was empty), so this header is a first proposal, not a
 * contract cast in stone -- if callfsm.c/graph_fake.c already define an
 * equivalent, incompatible interface by the time this is read, the
 * maintainer picks one.
 *
 * Design notes (why it looks like this):
 *  - Callback-based, not poll-based: callfsm.c/automode.c need to react to
 *    events like M4
 *    connect/disconnect, profile changed under us -- e.g. WirePlumber's
 *    autoswitch-to-headset-profile before it was disabled, or a user
 *    running `pactl set-card-profile` by hand) without polling.
 *  - `open()` takes the CALLER's `sd_event` loop
 *    rather than creating a private one -- this implementation is meant to
 *    live inside momentum4d's single event loop, unlike pw/graph_pw.h's
 *    OTHER, one-shot `m4_pw_query_snapshot()` (used by `momentum4 status`),
 *    which deliberately runs its own private loop to completion
 *    because a CLI command has no long-lived loop to plug into. Both live
 *    in pw/graph_pw.c because both need the exact same hard-won lessons
 *    about the live PipeWire ABI: pw_core_sync()'s real seq is
 *    its OWN return value, not the input parameter echoed back, and
 *    registry.global's props are a REDUCED set (for Device AND for Node --
 * re-confirmed against a live read: a client-created
 *    Stream/Input/Audio node's registry.global props only ever contained
 *    object.serial/factory.id/client.id/node.description/node.name/
 *    media.class, never node.passive/stream.monitor/
 *    application.process.binary -- see pw/micdetect.c, which therefore
 *    binds every Stream/Input/Audio candidate node it sees). set_profile's
 *    exact wire construction (SPA_TYPE_OBJECT_ParamProfile with
 *    SPA_PARAM_PROFILE_index + SPA_PARAM_PROFILE_save) is copied from
 *    module-protocol-pulse/pulse-server.c's do_set_profile() (the
 *    "pactl set-card-profile" implementation) --
 *    confirmed by reading that function's body, not just its call site.
 *  - This is the ONLY write path to PipeWire anywhere in
 *    momentum4d, and the only thing it ever writes is Profile on the M4
 *    Device. No node/link creation, no other device's Profile, ever.
 *  - set_profile() is fire-and-forget (issues the request, returns whether
 *    it COULD be issued, not whether PipeWire accepted/applied it).
 *    Confirming the switch actually happened is the caller's job, watching
 *    for a subsequent on_state_changed() with the new profile_name and its
 *    own timeout ("profile confirmation with a timeout" stays in
 *    callfsm.c, not here).
 */
#ifndef MOMENTUM4_CORE_GRAPH_OPS_H
#define MOMENTUM4_CORE_GRAPH_OPS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <systemd/sd-event.h>

#ifdef __cplusplus
extern "C" {
#endif

enum m4_graph_status {
	M4_GRAPH_OK = 0,
	M4_GRAPH_ERR_INVALID,        /* bad arguments (NULL handle, NULL name, ...) */
	M4_GRAPH_ERR_CONNECT,        /* open(): could not connect to PipeWire at all */
	M4_GRAPH_ERR_NOT_FOUND,      /* set_profile(): M4 device not currently bound
				      * (not present in the registry right now) */
	M4_GRAPH_ERR_NO_SUCH_PROFILE,/* set_profile(): `profile_name` is not in the
				      * most recently observed EnumProfile list */
};

#define M4_GRAPH_MAX_PROFILES 32 /* live M4 has 8; generous headroom,
				   * same margin as M4_PW_MAX_SINK_CANDIDATES
				   * in graph_pw.h */

struct m4_graph_profile {
	uint32_t index;    /* SPA_PARAM_PROFILE_index -- what set_profile()
			     * ultimately sends back, looked up by name */
	char name[64];       /* SPA_PARAM_PROFILE_name, e.g. "a2dp-sink",
			     * "headset-head-unit", "headset-head-unit-cvsd", "off" */
	char description[128];
	bool available;      /* SPA_PARAM_PROFILE_available == SPA_PARAM_AVAILABILITY_yes;
			      * "unknown" is also reported as true (never seen "no" on
			      * this device family; "unknown" must not be treated as
			      * unusable) */
};

struct m4_graph_state {
	bool m4_present;    /* a Device global whose device.name is the M4's
			     * "bluez_card.<mac>" currently exists in the registry */
	bool m4_connected;  /* its api.bluez5.connection info prop == "connected";
			     * only meaningful while m4_present */

	bool have_profile;  /* SPA_PARAM_Profile has been observed at least once */
	char profile_name[64];

	size_t n_profiles;
	struct m4_graph_profile profiles[M4_GRAPH_MAX_PROFILES];

	/* M4's own reported HFP mic
	 * gain (AT+VGM) can come back as 0% right after a (re)connect or a
	 * profile switch into headset-head-unit/-cvsd -- bluez5-device.c
	 * mirrors WHATEVER the headset itself reports via AT+VGM verbatim as
	 * this Route's SPA_PROP_channelVolumes (node_set_volume()/
	 * volume_changed(), confirmed by reading bluez5-device.c +
	 * backend-native.c), it is not something WirePlumber's usual
	 * stream-restore can override (SPA_POD_PROP_FLAG_HARDWARE). This is
	 * SPA_PARAM_Route index=ROUTE_INPUT(0)/device=DEVICE_ID_SOURCE(0) on
	 * the M4 Device -- the same object `pactl set-source-volume`/
	 * `set-source-mute` for the M4 microphone would target.
	 * `have_mic_route` is false whenever the current profile has no input
	 * direction at all (AALL/a2dp-sink -- M4 has no A2DP source SEP)
	 * or the route has simply never been observed yet. */
	bool have_mic_route;
	float mic_volume; /* linear 0..1, max(channelVolumes) -- mirrors
			    * bluez5-device.c's node_get_hw_volume(); 0.0 if
			    * have_mic_route == false */
	bool mic_mute;
};

struct m4_graph_callbacks {
	/* Fired after ANY of: m4_present, m4_connected, profile_name, or the
	 * profiles[] list changes -- always with the FULL, self-consistent
	 * new state (never a partial/mid-refresh one: pw/graph_pw.c stages
	 * EnumProfile/Profile rebuilds and only swaps them in atomically,
	 * see its own top-of-file comment). Called from within whatever
	 * sd_event iteration is currently processing the underlying
	 * PipeWire socket -- do not re-enter graph_ops from this callback
	 * (calling set_profile() from it is fine; opening/closing the
	 * handle from it is not). */
	void (*on_state_changed)(void *user_data, const struct m4_graph_state *state);
};

struct graph_ops {
	/* Connects to the default PipeWire remote and starts tracking the
	 * device whose api.bluez5.address/device.name matches `mac_address`
	 * (see pw/graph_pw.c for exactly how, and why not literally
	 * api.bluez5.address). Integrates into `event` (must
	 * outlive the returned handle); never blocks beyond the initial
	 * connect syscalls. Returns NULL on failure to connect (nothing
	 * else fails at open() time -- "M4 not present yet" is a normal,
	 * expected initial state, reported via the first on_state_changed()
	 * call once the registry burst has been processed, not a failure). */
	void *(*open)(sd_event *event, const char *mac_address,
		      const struct m4_graph_callbacks *cb, void *user_data);

	/* Destroys everything opened by open(). Never touches the graph
	 * (issues no writes) -- only closes momentum4d's own PipeWire
	 * connection. Safe to call with NULL. */
	void (*close)(void *handle);

	/* Fills `out` with the current cached state (no round-trip to the
	 * server -- always instant, reflects the last on_state_changed()).
	 * M4_GRAPH_ERR_INVALID on bad arguments only. */
	enum m4_graph_status (*get_state)(void *handle, struct m4_graph_state *out);

	/* Requests the M4 card switch to the profile named `profile_name`
	 * (must be one of the names currently in the cached profiles[]
	 * list -- e.g. "a2dp-sink", "headset-head-unit",
	 * "headset-head-unit-cvsd"), with `save` exactly as pactl/
	 * WirePlumber's own apply-profile.lua always pass it: `true` calls
	 * WirePlumber's store_user_selected_profile_hook, which is what
	 * keeps EnumProfile churn during an active HFP call (M4 does re-emit
	 * EnumProfile while an HFP call is active) from silently reverting
	 * the profile out from under a call -- find-stored-profile
	 * re-selects the SAME name we just saved, and apply-profile.lua only
	 * calls set_param again if the resolved index actually differs from
	 * the active one. `save=false` is accepted (the API doesn't forbid
	 * it) but is not recommended: it loses that protection. */
	enum m4_graph_status (*set_profile)(void *handle, const char *profile_name, bool save);

	/* Bug 2 (see struct m4_graph_state's have_mic_route/mic_volume/mic_mute
	 * comment above): sets the M4 microphone Route's volume/mute via
	 * SPA_PARAM_Route (index=ROUTE_INPUT=0, device=DEVICE_ID_SOURCE=0 --
	 * both FIXED enum constants in bluez5-device.c, not looked up from
	 * state the way set_profile()'s index is -- see pw/graph_pw.c for the
	 * exact construction, mirroring module-protocol-pulse/pulse-server.c's
	 * set_card_volume_mute_delay(), the real implementation behind `pactl
	 * set-source-volume`/`set-source-mute` for a card-backed source).
	 * `volume` is linear 0..1 (clamped), sent as a SINGLE-element
	 * channelVolumes array -- bluez5-device.c's node_set_volume()
	 * broadcasts a shorter array to every real channel via index modulo
	 * (confirmed by reading its body), so this interface deliberately
	 * never needs to know the M4 mic's actual channel count (always mono
	 * in practice, backend-native.c's SCO transport setup). Fire-and-forget,
	 * same contract as set_profile(): the caller confirms via a subsequent
	 * get_state()/on_state_changed(). M4_GRAPH_ERR_NOT_FOUND if the M4
	 * is not currently present (mirrors set_profile()). */
	enum m4_graph_status (*set_mic_volume)(void *handle, float volume, bool mute, bool save);

	/* OPTIONAL (may be NULL -- core/graph_ops_fake.c leaves it NULL
	 * unless a test opts in). Pumps ONLY this handle's own PipeWire loop
	 * for at most `timeout_ms`, WITHOUT re-entering the caller's sd_event.
	 * Exists because sd_event_run() refuses to nest: called from inside
	 * any sd-event callback (ctl.sock request, tick timer, micdetect,
	 * the MPRIS player's D-Bus handler) it returns -EBUSY immediately
	 * -- which made
	 * every bounded confirm-poll in pw/callfsm_graph_adapter.c and
	 * pw/repair_a2dp.c fail instantly in the live daemon while the
	 * fake-graph tests (synchronous confirm, top-level calls) stayed
	 * green. See pw/graph_wait.h for the single helper every such poll
	 * loop goes through. Returns >= 0 on success, < 0 on a loop error. */
	int (*iterate)(void *handle, int timeout_ms);
};

extern const struct graph_ops m4_graph_ops_pw;

/* Pure helper, exposed for tests (tests/test_graph_ops.c): looks up
 * `profile_name` in `state->profiles[0..state->n_profiles)`, returns true
 * and sets *out_index on a match. No I/O -- this is exactly the lookup
 * pw/graph_pw.c's set_profile() does internally before calling
 * pw_device_set_param(), split out so it is testable without a live
 * PipeWire connection (mirrors module-protocol-pulse/pulse-server.c's
 * find_profile_index(), confirmed by reading its body). */
bool m4_graph_find_profile_index(const struct m4_graph_state *state,
				  const char *profile_name, uint32_t *out_index);

#ifdef __cplusplus
}
#endif

#endif /* MOMENTUM4_CORE_GRAPH_OPS_H */
