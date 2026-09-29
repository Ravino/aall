/* core/graph_ops_fake.h -- deterministic in-memory struct graph_ops
 * (core/graph_ops.h, pw/graph_pw.c's m4_graph_ops_pw) implementation for
 * tests, mirroring the role core/callfsm_fake.c already plays for the
 * narrower m4_callfsm_ops_t . Used by
 * tests/test_callfsm_graph_adapter.c and tests/test_daemon.c to
 * cover the daemon with a test on the fake graph -- the write-path
 * invariants and the mode/safe-latch write-gating are exactly the properties this fake makes
 * testable without a live PipeWire connection).
 *
 * Unlike m4_graph_ops_pw, open() here is fully synchronous: set_profile()
 * fires on_state_changed() before returning (configurable to instead fail
 * or "hang" -- never confirm -- for adapter timeout tests), so tests never
 * need to pump a real sd_event loop for the success path. `event` is
 * accepted (interface compatibility) and ignored.
 */
#ifndef MOMENTUM4_CORE_GRAPH_OPS_FAKE_H
#define MOMENTUM4_CORE_GRAPH_OPS_FAKE_H

#include "graph_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	bool m4_present;
	bool m4_connected;

	size_t n_profiles;
	struct m4_graph_profile profiles[M4_GRAPH_MAX_PROFILES];
	bool have_profile;
	char profile_name[64];

	/* Bug 2: mirrors struct m4_graph_state's
	 * have_mic_route/mic_volume/mic_mute -- set directly by
	 * m4_graph_ops_fake_set_mic_route() to model "M4 reconnected /
	 * switched into HFP with AT+VGM already reporting some gain", the
	 * fake never derives this from `profile_name` itself (unlike real
	 * bluez5-device.c, which gates the Route on the profile's direction
	 * mask -- the fake is deliberately simpler, same level of
	 * simplification as fake_set_profile() not checking direction masks
	 * either). */
	bool mic_have_route;
	float mic_volume;
	bool mic_mute;
	int set_mic_volume_calls;
	float last_set_mic_volume;
	bool last_set_mic_mute;
	bool last_set_mic_save;

	/* If true, the NEXT set_profile() call for ANY profile name silently
	 * fails to confirm: the state is not changed and on_state_changed()
	 * is not fired for it -- models "profile rejected" / "never
	 * confirmed in time" for adapter timeout tests. Auto-clears after
	 * one use (like m4_callfsm_fake_t's fail_profiles, but simpler: the
	 * adapter tests only ever need "the next call fails", not a
	 * persistent deny-list). */
	bool fail_next_set_profile;

	/* nested sd_event_run() -> -EBUSY: when
	 * `async_set_profile` is true, set_profile() only records the request
	 * as pending; it is applied (state change + on_state_changed()) by the
	 * NEXT iterate() call -- modelling the real graph, where confirmation
	 * arrives later through the PipeWire loop. iterate() itself only works
	 * when `iterate_enabled` is true (otherwise returns -ENOTSUP, which is
	 * exactly what the graph_wait helper did for every caller before the
	 * fix: "cannot wait"). Both default false -> old synchronous fake. */
	bool async_set_profile;
	bool iterate_enabled;
	bool pending_set_profile;
	char pending_profile_name[64];
	int iterate_calls;

	struct m4_graph_callbacks cb;
	void *user_data;

	int open_calls;
	int close_calls;
	int set_profile_calls;
	char last_set_profile_name[64];
	bool last_set_profile_save;
} m4_graph_ops_fake_t;

/* m4_present=true, m4_connected=true, profiles = {"a2dp-sink" (index 0),
 * "headset-head-unit" (index 1), "headset-head-unit-cvsd" (index 2)},
 * active profile "a2dp-sink" -- a minimal but realistic stand-in for the
 * live M4's EnumProfile (8 profiles live, only these three matter
 * to callfsm/automode). */
void m4_graph_ops_fake_init(m4_graph_ops_fake_t *fake);

/* Directly sets the fake's "current" profile AND fires on_state_changed()
 * if a handle is open -- for simulating a profile change that did NOT come
 * from this adapter's own set_profile() (e.g. someone ran `pactl
 * set-card-profile` by hand, or WirePlumber's autoswitch), and for seeding
 * scenarios where the initial active profile is not "a2dp-sink". */
void m4_graph_ops_fake_set_profile_external(m4_graph_ops_fake_t *fake, const char *profile_name);

/* Simulates the M4 disappearing/reappearing (bluez5 device unbound/rebound)
 * -- fires on_state_changed() if a handle is open. */
void m4_graph_ops_fake_set_present(m4_graph_ops_fake_t *fake, bool present, bool connected);

/* Bug 2: directly sets the fake's mic Route state and
 * fires on_state_changed() if a handle is open -- for seeding "M4 just
 * (re)connected / switched into HFP and AT+VGM already reports 0%/muted"
 * scenarios. `have_route=false` models "no input direction in the current
 * profile" (AALL) -- mic_volume/mic_mute are ignored and reset to 0/false
 * in that case, matching real bluez5-device.c's build_route() returning no
 * props at all outside HFP/HSP. */
void m4_graph_ops_fake_set_mic_route(m4_graph_ops_fake_t *fake, bool have_route, float volume, bool mute);

/* Returns a `struct graph_ops` bound to `fake`. `fake` must outlive every
 * handle opened through it (no allocation happens in graph_ops_fake.c
 * beyond the opaque handle itself, which IS heap-allocated by open() and
 * freed by close() -- unlike callfsm_fake.c, this interface's open()/close()
 * shape requires a real handle object distinct from `fake` itself, since
 * `struct graph_ops.open()` takes no `ctx`-like parameter, only an
 * sd_event*; `fake` is captured by the returned struct's function pointers
 * via a thread-unsafe global-free trick: see graph_ops_fake.c). */
const struct graph_ops *m4_graph_ops_fake(m4_graph_ops_fake_t *fake);

#ifdef __cplusplus
}
#endif

#endif /* MOMENTUM4_CORE_GRAPH_OPS_FAKE_H */
