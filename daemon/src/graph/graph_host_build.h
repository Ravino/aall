/* graph/graph_host_build.h -- pure string-building logic for `momentum4
 * graph-host` (`[graph]` section, "why graph-host and not
 * pactl/drop-in"). No PipeWire calls here at all -- this
 * file only turns a `[graph]` config snapshot into the exact module-args
 * strings that graph_host_run.c hands to `pw_context_load_module()` for the
 * two stock PipeWire modules this replaces the pactl-driven bash for:
 *
 *   - `libpipewire-module-combine-stream` (replaces a
 * `pactl load-module module-combine-sink ..` invocation)
 *   - `libpipewire-module-loopback`, native args (replaces a
 *     `pactl load-module module-loopback ...` invocation)
 *
 * Traced 1:1 against the ACTUAL translation the installed PipeWire 1.4.2
 * does for the pulse-compat wrappers these replace (confirmed by reading
 * `work/.../src/modules/module-protocol-pulse/modules/module-combine-sink.c`
 * and `.../module-loopback.c`'s own `*_prepare()`: `sinks=`/`sink_properties=`
 * feed straight into the native module's own `stream.rules`/`node.*` args,
 * `sink_input_properties=`/`source_output_properties=` feed straight into
 * `playback.props`/`capture.props` respectively -- same module, same args
 * grammar, this file just skips the pulse-args translation layer and writes
 * the native grammar directly, since graph-host loads the native module
 * itself, not through pipewire-pulse).
 *
 * Every builder here is a pure function: given a `struct m4_graph_host_config`
 * snapshot (already resolved from `[graph]`/`[device]` by the caller, see
 * util/config.h) it writes a NUL-terminated module-args string into a
 * caller-owned buffer and returns its length, or a negative value if the
 * buffer was too small or an input was rejected -- never partial/truncated
 * output (a truncated module-args string could silently parse into
 * something ELSE, e.g. a stray `"` cutting a JSON string short and
 * appending unrelated bytes as new top-level keys; safer to fail loudly
 * than to load an unintended module configuration).
 */
#ifndef MOMENTUM4_GRAPH_HOST_BUILD_H
#define MOMENTUM4_GRAPH_HOST_BUILD_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Recommended buffer size for either builder below -- generous relative to
 * the longest input this registry accepts (`[graph] combine-members` has no
 * length cap in common/m4-conf-keys.def, so callers with very long CSV
 * lists should still check the return value instead of assuming this is
 * always enough; it comfortably covers the documented/expected case of a
 * handful of members). */
#define M4_GRAPH_HOST_ARGS_BUF_MIN 4096

struct m4_graph_host_config {
	bool combine_enable;
	const char *combine_name;		/* "[graph] combine-name", e.g. "m4_combined" */
	const char *combine_members_csv;	/* "[graph] combine-members": comma-separated
						 * node.name patterns for the OTHER members (M4
						 * itself is added automatically below, do not
						 * list it). A token starting with '~' is passed
						 * through as-is as a regex value (same grammar
						 * pw_conf_match_rules() itself accepts, see
						 * module-combine-stream.c's own doc comment:
						 * "~ starts regex"); anything else is matched
						 * for exact node.name equality. Tokens are
						 * trimmed of surrounding whitespace; empty
						 * tokens (",," or leading/trailing comma) are
						 * skipped, not treated as errors. A token
						 * containing '"' is rejected (would break out
						 * of the generated JSON string) -- the whole
						 * builder call fails (-1) rather than silently
						 * dropping just that one token. */
	bool combine_latency_compensate;	/* "[graph] combine-latency-compensate" --
						 * default false: each
						 * member keeps its own natural minimal
						 * latency, no artificial delay buffer in
						 * front of the fast member(s). */
	bool mic_placeholder_enable;
	const char *mic_name;			/* "[graph] mic-name", e.g. "momentum4_mic" */
	const char *m4_mac;			/* "AA:BB:CC:DD:EE:FF", or "" if not yet known
						 * (m4_config_mac_address() can return "" --
						 * see util/config.h). Both builders below still
						 * produce valid, loadable module args when this
						 * is "" -- they just omit the M4-specific
						 * match/target, which means the combine sink
						 * has no M4 member yet (still forwards to
						 * whatever else IS listed) and the mic
						 * placeholder's capture side has nothing to
						 * filter.smart onto yet (a permanently silent
						 * placeholder until graph-host is restarted, or
						 * the caller rebuilds+reconnects the stream,
						 * once the MAC becomes known -- graph_host_run.c's
						 * job, not this file's). */
};

/* "AA:BB:CC:DD:EE:FF" -> "AA_BB_CC_DD_EE_FF" (colons -> underscores) --
 * same transform pw/micdetect.c's own m4_mic_source_is_m4() uses to turn
 * a BlueZ MAC into the corresponding bluez5 PipeWire node-name suffix.
 * Pulled out as its own tiny pure function (rather than reusing micdetect.c,
 * which is not otherwise a dependency of this file) so both builders below
 * and their unit test can exercise it directly.
 *
 * Returns `out` on success, NULL if `mac` is NULL/empty or `out_cap` is too
 * small (need strlen(mac)+1 bytes; a valid "AA:BB:CC:DD:EE:FF" is 18 bytes
 * with the NUL). */
const char *m4_graph_host_mac_underscore(const char *mac, char *out, size_t out_cap);

/* Builds `libpipewire-module-combine-stream`'s args string (native grammar,
 * see PipeWire's own module-combine-stream documentation for the exact
 * syntax this mirrors) into `out` (capacity
 * `out_cap`, recommend >= M4_GRAPH_HOST_ARGS_BUF_MIN).
 *
 * `combine.mode = sink`, `node.name`/`node.description` from cfg,
 * `combine.latency-compensate` from cfg . `stream.rules`
 * gets one match entry per `combine_members_csv` token (media.class=Audio/Sink
 * AND that token's node.name) plus one ALWAYS-appended entry for M4 itself:
 * `node.name = "bluez_output.<mac_>.1"` (the a2dp-sink SEP's fixed output
 * port number, identical across the aptx_hd<->aptx_adaptive codec switch --
 * a stable port number regardless of which codec is active)
 * -- omitted if cfg->m4_mac is "".
 *
 * Returns the length of the string written to `out` (excluding the NUL) on
 * success, or -1 if `out`/`out_cap` cannot hold the result, cfg is NULL, or
 * a `combine_members_csv` token contains a `"`. */
int m4_graph_host_build_combine_args(const struct m4_graph_host_config *cfg,
				      char *out, size_t out_cap);

/* Builds `libpipewire-module-loopback`'s args string (native grammar) for
 * the mic placeholder into `out` (capacity `out_cap`, recommend >=
 * M4_GRAPH_HOST_ARGS_BUF_MIN) -- property-for-property equivalent of
 * the pactl-based module-loopback invocation this replaces, translated
 * from the pulse-compat module's sink_input_properties/source_output_properties
 * to the native module's playback.props/capture.props (confirmed 1:1 mapping,
 * see this header's own top comment).
 *
 * `audio.channels = 1` (mono, matches the pulse args' `channels=1`).
 * `capture.props`: `filter.smart=true`, `filter.smart.target =
 * {"api.bluez5.address":"<mac>","media.class":"Audio/Source"}` (omitted if
 * cfg->m4_mac is ""), `node.passive=true`, `node.name = "<mic_name>_capture"`,
 * `stream.dont-remix=true`, `node.dont-fallback=true`, `state.restore-props=false`.
 * `playback.props`: `media.class=Audio/Source` (the class-override trick that
 * makes this side the public virtual source node, not an actual sink input),
 * `node.name = "<mic_name>"`, `node.description`, `node.virtual=false`,
 * `node.dont-fallback=true`, `node.linger=true`, `state.restore-props=false`.
 *
 * Same return-value contract as m4_graph_host_build_combine_args(). Unlike
 * the combine builder, an empty cfg->m4_mac is NOT an error here (the
 * placeholder node itself always exists; only its filter.smart target is
 * omitted -- it just stays silent, exactly like the live bash version's own
 * "the placeholder only carries real sound while M4 is actually in HFP"
 * behaviour). */
int m4_graph_host_build_mic_args(const struct m4_graph_host_config *cfg,
				  char *out, size_t out_cap);

#ifdef __cplusplus
}
#endif

#endif /* MOMENTUM4_GRAPH_HOST_BUILD_H */
