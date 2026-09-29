/* cli/graph_host.h -- `momentum4 graph-host`: one long-running process
 * that replaces three separate bash+pactl scripts that used to load
 * modules into the live PipeWire graph from the outside --
 *
 *   - the combined output sink (M4 + other members), via
 *     `libpipewire-module-combine-stream` loaded into graph-host's OWN
 *     pw_context (same trick pw-loopback.c itself uses -- see
 *     graph/graph_host_build.h's top comment for the 1:1
 *     property-translation proof).
 *   - the permanent virtual microphone, via `libpipewire-module-loopback`.
 *   - `clock.force-quantum` on the "settings" metadata object of the REAL
 *     pipewire core (a second, separate connection graph-host makes for
 *     exactly this one purpose -- module-combine-stream/module-loopback
 *     never touch metadata themselves).
 *
 * Entirely config-driven (`[graph]`, common/m4-conf-keys.def) -- no CLI
 * flags beyond `-h`/`--help`; matches this whole project's "one config
 * file" convention. Runs in the foreground until SIGINT/SIGTERM
 * (sd_notify(0,"READY=1") once whatever is configured has been applied --
 * see graph_host.c's own top comment for the exact shutdown handshake).
 * If NEITHER combine NOR the mic placeholder NOR force-quantum is enabled
 * (`[graph]` community defaults: all off), this does NOT exit early --
 * `systemd/momentum4-graph.service.in` uses `Restart=always` (so that a
 * real crash of an enabled module is respawned quickly), and an immediate
 * exit(0) in that state would hit systemd's
 * StartLimitBurst within ~20s and leave the unit looking "failed" for
 * every user who simply has not turned any `[graph]` feature on yet
 * (confirmed this exact interaction while cross-checking against the unit
 * template). Instead, with nothing to load, it just connects no PipeWire
 * client at all and waits for SIGTERM -- effectively idle, near-zero
 * resource use, and systemd sees a normal long-running service.
 *
 * graph-host never binds/reads anything about speech, other apps' streams,
 * or non-M4/non-configured-member nodes -- it stays inside its own
 * `[graph]`-configured scope, the same way momentum4d never creates
 * PipeWire nodes outside its own zone -- its own client connections + the two loaded
 * modules' streams are the only graph objects it ever creates, and process
 * exit (clean or `kill -9`) tears every one of them down automatically (the
 * real pipewire server removes a disconnected client's nodes itself, same
 * resilience property already exercised by the helper process's own
 * kill -9 test).
 */
#ifndef MOMENTUM4_CLI_GRAPH_HOST_H
#define MOMENTUM4_CLI_GRAPH_HOST_H

#ifdef __cplusplus
extern "C" {
#endif

/* Exit codes: 0 clean shutdown (SIGINT/SIGTERM); 1 usage error OR a loaded
 * module died unexpectedly (systemd's Restart=always will start a fresh
 * graph-host); 2 could not even create a local pw_main_loop/pw_context
 * (never happens for "nothing configured" -- see this header's own top
 * comment); 3 a configured module failed to load (bad args, PipeWire
 * rejected the module). */
int m4_cli_graph_host_run(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif /* MOMENTUM4_CLI_GRAPH_HOST_H */
