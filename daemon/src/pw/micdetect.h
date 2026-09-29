/* pw/micdetect.h -- "is anything actually listening to the M4 microphone
 * right now": this is the live PipeWire glue half, see m4_mic_decide()
 * below for the pure, testable half. Feeds automode.c.
 *
 * Behaviour matches the already-fixed reference implementation this was
 * ported from -- same exclusion
 * order, same default exclude-binaries set, same placeholder-capture
 * special case:
 *
 *   1. application.process.binary in a fixed exclude set (pavucontrol,
 *      pw-mon, pw-cat) -- these are OUR OWN or generic inspection tools,
 *      never "the user is on a call/dictating" (a future tool could be
 *      added to this list, e.g. a latency probe).
 *   2. stream.monitor == true -- peak-meter/VU-meter taps (pavucontrol's
 *      own meters, and anyone else's), never real capture.
 *   3. node.passive == true -- our own placeholder's passive self-capture
 *      link, AND any future passive tap.
 *   4. node.name == "<placeholder_name>_capture" -- belt-and-suspenders for
 *      #3 (defense-in-depth -- node.passive catches any future
 *      passive capture, node.name catches our own placeholder specifically,
 *      even if node.passive is ever removed from it).
 *   Anything left whose Link's OUTPUT (source) node is either the virtual
 *   placeholder (node.name == placeholder_name, e.g. "momentum4_mic") or a
 *   real M4 HFP source node (node.name matching "bluez_input.<mac>." with
 *   ':' -> '_') counts as "microphone in use".
 *
 * Live glue design note (why this binds every Stream/Input/Audio node it
 * sees): registry.global's `props` for a Node global is a REDUCED set --
 * re-confirmed with a live, read-only registry dump: a real client-created
 * "Stream/Input/Audio" node's registry.global props contained only
 * object.serial/factory.id/client.id/node.description/node.name/
 * media.class -- no node.passive, no stream.monitor, no
 * application.process.binary, ever. Those three only ever showed up after
 * BINDING the node and reading its `info` event's props (exactly
 * pw/graph_pw.c's own established pattern). Link globals are the OPPOSITE:
 * their registry.global props DID already contain link.output.node and
 * link.input.node directly (also live-confirmed) -- pipewire's core sets
 * these unconditionally when it creates the link (impl-link.c), so no Link
 * binding is needed at all, only Node binding.
 */
#ifndef MOMENTUM4_PW_MICDETECT_H
#define MOMENTUM4_PW_MICDETECT_H

#include <stdbool.h>
#include <stddef.h>

#include <systemd/sd-event.h>

#ifdef __cplusplus
extern "C" {
#endif

struct m4_micdetect_config {
	const char *mac_address;    /* M4 MAC, e.g. "00:1B:66:00:00:01" -- for
				      * matching real "bluez_input.<mac_>.N" source
				      * nodes (HFP mic), mac_ = mac_address with
				      * ':' replaced by '_', same rule as
				      * m4_pw_build_device_name() in graph_pw.h,
				      * but with prefix "bluez_input." instead of
				      * "bluez_card." (deliberately re-implemented
				      * locally rather than shared with graph_pw.c
				      * -- this module has no other dependency on
				      * graph_pw.c/graph_ops.h and is meant to be
				      * usable/testable/deployable completely on
				      * its own. */
	const char *placeholder_name; /* virtual mic node, e.g. "momentum4_mic"
					* (created by `momentum4 graph-host`); its
					* own capture stream is
					* "<placeholder_name>_capture" and is
					* EXCLUDED, see above. NULL/empty:
					* placeholder matching disabled (only
					* the real bluez_input.<mac>.* source is
					* recognized). */
};

/* `hold_active` is the SAME underlying candidate set as `mic_active`, minus
 * any binary in the "holder" blacklist (m4_mic_is_holder() below) -- see
 * this header's "Bug 1: long-lived holders" section further down for the
 * full rationale. Both are delivered together on every callback (whichever
 * one(s) actually changed) so the caller (daemon.c) never has to reason
 * about which fired -- it simply feeds BOTH into automode.c's two
 * corresponding event methods every time, which are individually
 * idempotent on a repeated value (see core/automode.h). */
typedef void (*m4_micdetect_cb)(void *user_data, bool mic_active, bool hold_active);

/* Connects to the default PipeWire remote and starts tracking mic usage,
 * integrated into the caller's `event` (must outlive the handle, same
 * convention as core/graph_ops.h's struct graph_ops -- this is a SEPARATE
 * PipeWire connection from m4_graph_ops_pw's, not a view onto it --
 * sharing one connection instead was considered and rejected: micdetect
 * needs its own independent registry subscription regardless of graph_ops'
 * own connection lifecycle). `cb` fires once right after the initial
 * registry burst settles (even if mic_active/hold_active are still false --
 * so the caller always gets an authoritative first value, never has to
 * assume false-until-proven-otherwise) and again every time EITHER value
 * flips. Never fires twice in a row with the same (mic_active, hold_active)
 * pair. Returns NULL on failure to connect to PipeWire. */
void *m4_micdetect_open(sd_event *event, const struct m4_micdetect_config *cfg,
			 m4_micdetect_cb cb, void *user_data);

/* Destroys everything opened by open(). Issues no writes (read-only
 * client, like pw/graph_pw.c's m4_pw_query_snapshot() -- this module never
 * appears on the write side of the read-only invariant at all).
 * Safe to call with NULL. */
void m4_micdetect_close(void *handle);

/* Cached last-reported value (no round-trip). False before the first
 * callback fires (a handle that has not settled yet is treated as "no mic
 * use", the same conservative default automode.c should assume while
 * starting up). */
bool m4_micdetect_get_active(const void *handle);

/* Same, for hold_active (Bug 1, "long-lived holders" below). */
bool m4_micdetect_get_hold_active(const void *handle);

/* --- pure decision logic, exposed for tests/test_micdetect.c --- */

#define M4_MIC_MAX_CANDIDATES 128 /* generous desktop-scale cap; see graph_pw.h's
				    * own M4_PW_MAX_SINK_CANDIDATES for the same
				    * margin convention */

/* One Link whose OUTPUT (source) node is a candidate mic source, paired
 * with everything m4_mic_decide() needs to know about its INPUT (sink/
 * consumer) node to apply the exclusion rules above. `source_is_m4_mic`
 * is computed by the caller via m4_mic_source_is_m4() below (kept
 * separate from the node-name string itself so this struct, and
 * m4_mic_decide(), never need to know the MAC/placeholder name -- keeps
 * the pure decision function trivially unit-testable). Only links whose
 * source is a candidate need to be included at all -- the live glue
 * never builds a candidate entry for a link whose output node isn't a mic
 * source in the first place. */
struct m4_mic_candidate {
	bool source_is_m4_mic;
	char sink_media_class[64];    /* the consumer node's media.class; must be
					* exactly "Stream/Input/Audio" to count --
					* anything else (e.g. some other Sink, in
					* the admittedly bizarre case a link
					* somehow ran mic -> sink) is ignored */
	char sink_node_name[128];
	bool sink_passive;
	bool sink_stream_monitor;
	char sink_app_binary[128];    /* application.process.binary of the
					* consumer, "" if unknown/absent */
};

/* True if ANY candidate whose source_is_m4_mic is true, whose
 * sink_media_class is "Stream/Input/Audio", and which passes none of the
 * exclusions above, is present. `placeholder_capture_name` is the fully
 * built "<placeholder_name>_capture" string (or NULL/"" to disable that
 * specific exclusion, e.g. if the config had no placeholder configured at
 * all) -- built once by the live glue with m4_mic_placeholder_capture_name()
 * below, not re-derived here, so this function stays pure string
 * comparison with no "_capture" string literal baked into two places. */
bool m4_mic_decide(const struct m4_mic_candidate *candidates, size_t n_candidates,
		    const char *placeholder_capture_name);

/* True if `app_process_binary` is one of the fixed, always-excluded tool
 * binaries (pavucontrol,
 * pw-mon, pw-cat). NULL/"" is never excluded (an unset/unknown binary name
 * is not evidence of anything -- absence of information is not "our own
 * tool"; only an exact, non-empty match ever excludes). This is folded into m4_mic_decide() via
 * sink_app_binary; exposed separately only so tests can pin the exact set
 * without a live snapshot. */
bool m4_mic_binary_excluded(const char *app_process_binary);

/* Overrides the exclude-binaries set used by m4_mic_binary_excluded() above
 * for the rest of the process, from a comma-separated list
 * (`[automode] exclude-apps`) -- NULL or "" reverts to
 * the compiled default (pavucontrol,pw-mon,pw-cat). Entries longer than 63
 * bytes or past the 16th are silently dropped (generous for a list of tool
 * process names; never fatal). Called once, at daemon startup, by
 * cli/daemon_cmd.c; m4_mic_decide()'s own pure-logic test in
 * tests/test_micdetect.c never calls this, so its assertions against the
 * compiled default are unaffected. */
void m4_mic_set_exclude_apps(const char *comma_separated);

/* True if `node_name` names either the virtual placeholder mic
 * (`placeholder_name`, exact match) or a real HFP microphone source node
 * of the M4 (`"bluez_input." + mac_with_underscores + "."`, prefix match --
 * matches "bluez_input.<mac>.0"/".1"/etc.). NULL `node_name` is
 * never a match. `placeholder_name` NULL/"" disables that half only. */
bool m4_mic_source_is_m4(const char *node_name, const char *mac_address,
			  const char *placeholder_name);

/* Builds "<placeholder_name>_capture" into `out` (truncates, still
 * NUL-terminated, if `out_cap` is too small). `placeholder_name` NULL/""
 * produces an empty string. */
void m4_mic_placeholder_capture_name(const char *placeholder_name, char *out, size_t out_cap);

/* --- Bug 1: long-lived holders ---
 *
 * Observed in production: Signal Desktop (ringrtc,
 * application.process.binary == "signal-desktop") keeps its
 * Stream/Input/Audio capture on the M4 mic RUNNING indefinitely after a
 * voice message recording finishes -- not corked, not closed, just left
 * open (a known class of Chromium/Electron behaviour: the underlying OS
 * capture device is kept warm across distinct recording/call sessions,
 * independent of whether a call/recording is actually in progress). Since
 * AALL's virtual mic (momentum4_mic) has no real backing and always reads
 * digital silence, there is no
 * audio LEVEL to distinguish "Signal is actually in a call" from "Signal is
 * just holding the device open" while still in AALL -- the only signal
 * available before HFP is even entered is presence/absence of the stream
 * itself. m4_mic_decide() (unchanged, above) therefore stays a simple "is
 * ANY candidate present" -- it is exactly right for ENTRY (a genuine
 * Signal-only call, or Signal recording a voice message, DOES need to
 * trigger HFP entry the same way any other app would) and is what
 * m4_micdetect_get_active()/mic_active keeps reporting.
 *
 * The bug is specifically that automode used the SAME single boolean to
 * decide when to LEAVE HFP again -- so once signal-desktop's leftover
 * stream is present, automode never saw mic_active go false again, even
 * long after a real call in another app (e.g. librewolf) had genuinely
 * ended, and never returned to AALL. Fix: a SECOND, narrower boolean,
 * hold_active, computed identically to mic_active but with known
 * long-lived-holder binaries (M4_MIC_HOLDER_BINARIES, "signal-desktop" for
 * now) excluded in addition to the existing tool exclusions
 * (m4_mic_binary_excluded()) -- automode.c (core/automode.h's
 * on_mic_hold_changed()) uses hold_active, not mic_active, to decide
 * whether to ARM/CANCEL the exit debounce while a call is active, while
 * ENTRY keeps using mic_active/on_mic_activity_changed() unchanged. Losing
 * mic_active entirely (stream truly gone, holder included) still also
 * arms the exit debounce the same way it always has -- the "stream
 * disappeared" half of the combination -- hold_active only adds the
 * "silence/pause while a holder is still present" half. This is a
 * fixed, compile-time list today (mirrors M4_MIC_EXCLUDE_BINARIES's own
 * level of configurability); making it config-file driven
 * (ignore-apps=/hold-apps=) is a natural future extension, not done here.
 *
 * NOTE on the harder, more general version of this bug (documented as a
 * fork for the maintainer, not implemented): this exclude-list approach
 * cannot distinguish "signal-desktop's leftover idle stream" from
 * "signal-desktop is the app ACTUALLY in a real, ongoing ringrtc call" --
 * for a standalone Signal call (no other app's stream ever appears
 * alongside it), hold_active is false for the WHOLE call, so automode will
 * arm (and, after off_debounce_s, fire) an exit while the user is still on
 * the phone. The architecturally correct fix is real VAD/RMS silence
 * detection on the ACTUAL hfp audio once in HFP (the level is real once
 * in HFP) -- that requires a small, self-excluding,
 * HFP-lifetime-only monitor pw_stream reading real samples, which is a new
 * PipeWire client/node the daemon would create (in tension with the
 * "the daemon does not create nodes" invariant) and was intentionally NOT built in this
 * pass (no live PipeWire node was created or tested) -- recommended as a
 * follow-up task with explicit maintainer sign-off on that exception
 * before any live wiring. */
#define M4_MIC_HOLDER_BINARIES_COUNT 1
extern const char *const M4_MIC_HOLDER_BINARIES[M4_MIC_HOLDER_BINARIES_COUNT];

/* True if `app_process_binary` is a known long-lived holder (exact match,
 * same NULL/"" contract as m4_mic_binary_excluded()). */
bool m4_mic_is_holder(const char *app_process_binary);

/* Same override mechanism as m4_mic_set_exclude_apps() above, for the
 * holder set (`[automode] holder-apps`, compiled default "signal-desktop"). */
void m4_mic_set_holder_apps(const char *comma_separated);

/* Same contract as m4_mic_decide() (including "never fires on a candidate
 * whose sink_media_class isn't exactly Stream/Input/Audio" etc.), but ALSO
 * treats any candidate whose sink_app_binary is a known holder
 * (m4_mic_is_holder()) as excluded -- i.e. hold_active. Implemented as a
 * thin filter + m4_mic_decide() call (tests/test_micdetect.c exercises both
 * independently); does not mutate `candidates`. */
bool m4_mic_decide_hold(const struct m4_mic_candidate *candidates, size_t n_candidates,
			 const char *placeholder_capture_name);

#ifdef __cplusplus
}
#endif

#endif /* MOMENTUM4_PW_MICDETECT_H */
