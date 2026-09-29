# Small, dependency-free unit tests for the two parsers this phase adds
# (unit tests for the parsers: stats.json, node
# properties). No external test framework -- each binary is a plain `main()`
# that exits non-zero (with a message on stderr) on the first failure.

test('profile_rules',
  executable('test_profile_rules',
    ['test_profile_rules.c', '../src/core/profile_rules.c', common_dir / 'm4-conf.c', common_dir / 'm4-i18n.c'],
    include_directories : include_directories('../src'),
  ),
)

test('stats',
  executable('test_stats',
    ['test_stats.c', '../src/metrics/stats.c'],
    include_directories : include_directories('../src'),
    dependencies : [spa_dep],
  ),
)

# RT status: m4_rt_status_matches()/_policy_name() are
# pure (no I/O) -- unit-tested directly, same convention as test_stats
# above. m4_rt_status_find_helper_pid()/_query_pid() (the /proc-reading
# halves) are exercised live against the test process itself (its own PID
# is always readable) instead, same file -- no fake qemu-hexagon binary
# needed.
test('rt_status',
  executable('test_rt_status',
    ['test_rt_status.c', '../src/util/rt_status.c'],
    include_directories : include_directories('../src'),
  ),
)

# util/systemd_unit.c's pure, offline m4_systemd_unit_dir_in_search_path()
# -- links the whole .c file (needs libsystemd only to satisfy
# <systemd/sd-bus.h>'s types; no D-Bus call happens in this test).
test('systemd_unit_search',
  executable('test_systemd_unit_search',
    ['test_systemd_unit_search.c', '../src/util/systemd_unit.c', common_dir / 'm4-conf.c', common_dir / 'm4-i18n.c'],
    include_directories : include_directories('../src'),
    dependencies : [systemd_dep],
  ),
)

# graph_pw.c's pure helpers only (m4_pw_build_device_name/m4_pw_media_class_is_sink)
# -- links the whole .c file (and its own dependencies: loop_glue.c,
# profile_rules.c, log.c) but never calls m4_pw_query_snapshot(), so this
# needs no live PipeWire connection.
test('graph_props',
  executable('test_graph_props',
    [
      'test_graph_props.c',
      '../src/pw/graph_pw.c',
      '../src/pw/loop_glue.c',
      '../src/core/profile_rules.c',
      common_dir / 'm4-conf.c',
      common_dir / 'm4-i18n.c',
      '../src/util/log.c',
    ],
    include_directories : include_directories('../src'),
    dependencies : [pipewire_dep, systemd_dep],
  ),
)

# core/graph_ops.h + pw/graph_pw.c's struct graph_ops
# implementation: only the one pure helper (m4_graph_find_profile_index())
# needs/gets a live-PipeWire-free unit test -- see test_graph_ops.c's own
# top comment for why the rest is verified manually instead.
test('graph_ops',
  executable('test_graph_ops',
    [
      'test_graph_ops.c',
      '../src/pw/graph_pw.c',
      '../src/pw/loop_glue.c',
      '../src/core/profile_rules.c',
      common_dir / 'm4-conf.c',
      common_dir / 'm4-i18n.c',
      '../src/util/log.c',
    ],
    include_directories : include_directories('../src'),
    dependencies : [pipewire_dep, systemd_dep],
  ),
)

# pw/micdetect.c: pure decision logic
# (m4_mic_decide/m4_mic_source_is_m4/m4_mic_binary_excluded/
# m4_mic_placeholder_capture_name) -- no live PipeWire connection, same
# convention as test_graph_props above.
test('micdetect',
  executable('test_micdetect',
    [
      'test_micdetect.c',
      '../src/pw/micdetect.c',
      '../src/pw/loop_glue.c',
      '../src/util/log.c',
      common_dir / 'm4-conf.c',
      common_dir / 'm4-i18n.c',
    ],
    include_directories : include_directories('../src'),
    dependencies : [pipewire_dep, systemd_dep],
  ),
)

# core/automode.c -- 13 direct-equivalent unit tests (test_automode.c) plus a
# differential runner against a frozen set of reference traces below.
test('automode',
  executable('test_automode',
    ['test_automode.c', '../src/core/automode.c', common_dir / 'm4-conf.c', common_dir / 'm4-i18n.c'],
    include_directories : include_directories('../src'),
  ),
)

# core/callfsm.c against core/callfsm_fake.c (in-memory
# m4_callfsm_ops_t), ~20 scenarios / ~50 checks covering the call-mode
# state machine (about 40 out of 75 scenarios from the original shell tool).
test('callfsm',
  executable('test_callfsm',
    ['test_callfsm.c', '../src/core/callfsm.c', '../src/core/callfsm_fake.c', common_dir / 'm4-conf.c', common_dir / 'm4-i18n.c'],
    include_directories : include_directories('../src'),
  ),
)

# Golden-trace runner: core/automode.c against
# tests/golden/*.{trace,expected}, a one-time, frozen capture of the
# reference behaviour. No external interpreter dependency at test time --
# test_automode_diff.c does the comparison in C; see that
# file's own header comment for details, including the (developer-only,
# optional) regeneration path for the golden files.
test_automode_diff_exe = executable('test_automode_diff',
  ['test_automode_diff.c', '../src/core/automode.c', common_dir / 'm4-conf.c', common_dir / 'm4-i18n.c'],
  include_directories : include_directories('../src'),
)

test('automode_diff',
  test_automode_diff_exe,
  args : [meson.current_source_dir() / 'golden'],
  timeout : 60,
)

# core/state.c (call-state file format, safe-latch, mode
# persistence) -- pure file I/O, no
# PipeWire/systemd dependency.
test('state',
  executable('test_state',
    ['test_state.c', '../src/core/state.c'],
    include_directories : include_directories('../src'),
  ),
)

# pw/callfsm_graph_adapter.c (the thin synchronous
# m4_callfsm_ops_t adapter) against core/graph_ops_fake.c -- no live
# PipeWire, but needs a real sd_event (systemd_dep) for the adapter's
# set_profile_confirm() wait loop, and spa_dep for metrics/stats.c's
# spa/utils/json.h (helper_alive()). Also links pw/repair_a2dp.c (+
# pw/graph_pw.c for m4_graph_find_profile_index) and bt/bt_ops_fake.c --
# repair_a2dp.c and graph resilience: the adapter's
# repair_profile_if_missing() now calls into repair_a2dp.c, exercised here
# against a fake struct m4_bt_ops (no real D-Bus), hence pipewire_dep too
# (graph_pw.c needs it).
test('callfsm_graph_adapter',
  executable('test_callfsm_graph_adapter',
    [
      'test_callfsm_graph_adapter.c',
      '../src/pw/callfsm_graph_adapter.c',
      '../src/pw/repair_a2dp.c',
      '../src/pw/graph_wait.c',
      '../src/pw/graph_pw.c',
      '../src/pw/loop_glue.c',
      '../src/core/graph_ops_fake.c',
      '../src/core/profile_rules.c',
      common_dir / 'm4-conf.c',
      common_dir / 'm4-i18n.c',
      '../src/core/state.c',
      '../src/metrics/stats.c',
      '../src/util/log.c',
      '../src/bt/bt_ops_fake.c',
    ],
    include_directories : include_directories('../src'),
    dependencies : [pipewire_dep, systemd_dep, spa_dep],
  ),
)

# pw/repair_a2dp.c against core/graph_ops_fake.c + bt/bt_ops_fake.c -- no
# live PipeWire, no real D-Bus.
test('repair_a2dp',
  executable('test_repair_a2dp',
    [
      'test_repair_a2dp.c',
      '../src/pw/repair_a2dp.c',
      '../src/pw/graph_wait.c',
      '../src/pw/graph_pw.c',
      '../src/pw/loop_glue.c',
      '../src/core/graph_ops_fake.c',
      '../src/core/profile_rules.c',
      common_dir / 'm4-conf.c',
      common_dir / 'm4-i18n.c',
      '../src/util/log.c',
      '../src/bt/bt_ops_fake.c',
    ],
    include_directories : include_directories('../src'),
    dependencies : [pipewire_dep, systemd_dep],
  ),
)

# ipc/ctl.c ($runtime_dir/ctl.sock transport) against a
# real AF_UNIX socket in a throwaway tmpdir -- no PipeWire.
test('ctl',
  executable('test_ctl',
    ['test_ctl.c', '../src/ipc/ctl.c', '../src/util/log.c', common_dir / 'm4-conf.c', common_dir / 'm4-i18n.c'],
    include_directories : include_directories('../src'),
    dependencies : [systemd_dep],
  ),
)

# ipc/hfp_events.c's pure parser and its $runtime_dir/hfp-events.sock
# receiver, against a real AF_UNIX SOCK_DGRAM socket in a throwaway tmpdir
# -- no PipeWire, no SPA plugin (see this test's own top comment).
test('hfp_events',
  executable('test_hfp_events',
    ['test_hfp_events.c', '../src/ipc/hfp_events.c', '../src/util/log.c', common_dir / 'm4-conf.c', common_dir / 'm4-i18n.c'],
    include_directories : include_directories('../src'),
    dependencies : [systemd_dep],
  ),
)

# daemon/daemon.c's write-gating matrix (observe/manual/auto x safe-latch) against
# core/graph_ops_fake.c. Links pw/micdetect.c (needed at link time --
# daemon.c calls m4_micdetect_open() when cfg.enable_micdetect, which every
# test here leaves false, but the symbol still needs to resolve) and hence
# needs pipewire_dep/pw/loop_glue.c too, even though no test in this file
# ever touches a live PipeWire connection (enable_micdetect/enable_ctl/
# enable_tick_timer are all false in every fixture).
test('daemon',
  executable('test_daemon',
    [
      'test_daemon.c',
      '../src/daemon/daemon.c',
      '../src/control/gesture.c',
      '../src/control/mpris_player.c',
      '../src/control/passthrough.c',
      '../src/util/settings.c',
      common_dir / 'm4-conf.c',
      common_dir / 'm4-i18n.c',
      '../src/util/config.c',
      '../src/bt/bt_discover.c',
      '../src/util/legacy_conf.c',
      '../src/util/earcon.c',
      '../src/core/graph_ops_fake.c',
      '../src/core/state.c',
      '../src/core/automode.c',
      '../src/core/callfsm.c',
      '../src/core/profile_rules.c',
      '../src/pw/callfsm_graph_adapter.c',
      '../src/pw/repair_a2dp.c',
      '../src/pw/graph_wait.c',
      '../src/pw/graph_pw.c',
      '../src/pw/micdetect.c',
      '../src/pw/loop_glue.c',
      '../src/ipc/ctl.c',
      '../src/ipc/hfp_events.c',
      '../src/metrics/stats.c',
      '../src/util/log.c',
    ],
    include_directories : include_directories('../src'),
    dependencies : [pipewire_dep, systemd_dep],
  ),
)

# Scenario-level integration bench :
# a small text DSL (tests/scenarios/*.scn) driven against the daemon's
# public API + its existing fakes (core/graph_ops_fake.c, bt/bt_ops_fake.c) --
# see tests/scenarios/scenario_runner.c's own top comment for the DSL and
# scope. One executable, one meson test per .scn file so `meson test` reports
# them individually and a single failing scenario does not hide the others.
scenario_runner_exe = executable('scenario_runner',
  [
    'scenarios/scenario_runner.c',
    '../src/daemon/daemon.c',
      '../src/control/gesture.c',
      '../src/control/mpris_player.c',
      '../src/control/passthrough.c',
      '../src/util/settings.c',
      common_dir / 'm4-conf.c',
      common_dir / 'm4-i18n.c',
      '../src/util/config.c',
      '../src/bt/bt_discover.c',
      '../src/util/legacy_conf.c',
      '../src/util/earcon.c',
    '../src/core/graph_ops_fake.c',
    '../src/core/state.c',
    '../src/core/automode.c',
    '../src/core/callfsm.c',
    '../src/core/profile_rules.c',
    '../src/pw/callfsm_graph_adapter.c',
    '../src/pw/repair_a2dp.c',
      '../src/pw/graph_wait.c',
    '../src/pw/graph_pw.c',
    '../src/pw/micdetect.c',
    '../src/pw/loop_glue.c',
    '../src/ipc/ctl.c',
    '../src/ipc/hfp_events.c',
    '../src/metrics/stats.c',
    '../src/util/log.c',
    '../src/bt/bt_ops_fake.c',
  ],
  include_directories : include_directories('../src'),
  dependencies : [pipewire_dep, systemd_dep, spa_dep],
)

scenario_files = [
  'scenarios/connect_disconnect.scn',
  'scenarios/multi_app_mic.scn',
  'scenarios/hfp_confirm_delay.scn',
  'scenarios/a2dp_missing_after_hfp.scn',
  'scenarios/safe_latch.scn',
  'scenarios/restart_mid_call.scn',
  'scenarios/rapid_repeat_debounce.scn',
  'scenarios/mic_volume_after_reconnect.scn', # Bug 2
  'scenarios/hfp_chup_call_off.scn', # CHUP hangs up but keeps the call-off mic policy
  'scenarios/mic_toggle.scn', # follow-up: "tap = mic"
  'scenarios/mic_on_enter_on.scn', # "mic-on-enter" task
  'scenarios/mic_on_enter_off.scn', # "mic-on-enter" task
  'scenarios/mic_on_enter_remember.scn', # "mic-on-enter" task
  'scenarios/mic_on_enter_remember_reboot.scn', # "mic-on-enter" task
]

foreach scn : scenario_files
  test('scenario_' + scn.split('/')[1].split('.')[0],
    scenario_runner_exe,
    args : [meson.current_source_dir() / scn],
  )
endforeach

# momentum4.conf parser, gesture names/status policy,
# passthrough target choice -- pure, no I/O beyond one tmp file.
test('settings',
  executable('test_settings',
    [
      'test_settings.c',
      '../src/util/settings.c',
      common_dir / 'm4-conf.c',
      common_dir / 'm4-i18n.c',
      '../src/util/legacy_conf.c',
      '../src/util/config.c',
      '../src/bt/bt_discover.c',
      '../src/util/log.c',
      '../src/control/gesture.c',
      '../src/control/passthrough.c',
      '../src/core/state.c',
    ],
    include_directories : include_directories('../src'),
    dependencies : [systemd_dep],
  ),
)

# daemon side of gestures (map, gate, cooldown, earcon,
# passthrough, PLAYER/RELOAD, mode from config, nested-loop regression)
# against the fake graph -- no PipeWire, no D-Bus.
test('gestures',
  executable('test_gestures',
    [
      'test_gestures.c',
      '../src/daemon/daemon.c',
      '../src/control/gesture.c',
      '../src/control/mpris_player.c',
      '../src/control/passthrough.c',
      '../src/util/settings.c',
      common_dir / 'm4-conf.c',
      common_dir / 'm4-i18n.c',
      '../src/util/config.c',
      '../src/bt/bt_discover.c',
      '../src/util/legacy_conf.c',
      '../src/util/earcon.c',
      '../src/core/graph_ops_fake.c',
      '../src/core/state.c',
      '../src/core/automode.c',
      '../src/core/callfsm.c',
      '../src/core/profile_rules.c',
      '../src/pw/callfsm_graph_adapter.c',
      '../src/pw/repair_a2dp.c',
      '../src/pw/graph_wait.c',
      '../src/pw/graph_pw.c',
      '../src/pw/micdetect.c',
      '../src/pw/loop_glue.c',
      '../src/ipc/ctl.c',
      '../src/ipc/hfp_events.c',
      '../src/metrics/stats.c',
      '../src/util/log.c',
    ],
    include_directories : include_directories('../src'),
    dependencies : [pipewire_dep, systemd_dep],
  ),
)

# control/mpris_player.c + control/passthrough.c against a
# PRIVATE dbus-daemon spawned by the test (fake BlueZ in-process, fake session
# players in a child) -- never the system/session bus. Skipped (77) without
# dbus-daemon.
test('mpris_player',
  executable('test_mpris_player',
    [
      'test_mpris_player.c',
      '../src/control/gesture.c',
      '../src/control/mpris_player.c',
      '../src/control/passthrough.c',
      '../src/util/log.c',
      common_dir / 'm4-conf.c',
      common_dir / 'm4-i18n.c',
    ],
    include_directories : include_directories('../src'),
    dependencies : [systemd_dep],
  ),
  timeout : 60,
)

# Player status after reconnect: PlaybackStatus re-send
# (Stopped -> real) in control/mpris_player.c, its triggers (registration,
# M4 Device1/MediaControl1 Connected, MediaPlayer1, MediaTransport1 active)
# and the daemon wiring (taps with a fixed status, PLAYER RESYNC/STATUS,
# RELOAD) -- PRIVATE dbus-daemon spawned by the test + fake graph; never the
# system/session bus, no PipeWire. Skipped (77) without dbus-daemon.
test('player_resync',
  executable('test_player_resync',
    [
      'test_player_resync.c',
      '../src/daemon/daemon.c',
      '../src/control/gesture.c',
      '../src/control/mpris_player.c',
      '../src/control/passthrough.c',
      '../src/util/settings.c',
      common_dir / 'm4-conf.c',
      common_dir / 'm4-i18n.c',
      '../src/util/config.c',
      '../src/bt/bt_discover.c',
      '../src/util/legacy_conf.c',
      '../src/util/earcon.c',
      '../src/core/graph_ops_fake.c',
      '../src/core/state.c',
      '../src/core/automode.c',
      '../src/core/callfsm.c',
      '../src/core/profile_rules.c',
      '../src/pw/callfsm_graph_adapter.c',
      '../src/pw/repair_a2dp.c',
      '../src/pw/graph_wait.c',
      '../src/pw/graph_pw.c',
      '../src/pw/micdetect.c',
      '../src/pw/loop_glue.c',
      '../src/ipc/ctl.c',
      '../src/ipc/hfp_events.c',
      '../src/metrics/stats.c',
      '../src/util/log.c',
    ],
    include_directories : include_directories('../src'),
    dependencies : [pipewire_dep, systemd_dep],
  ),
  timeout : 90,
)

# patch 0008: M4 closes the HFP audio link on a double
# tap; hfp-sco-drop policy (restore | call-off | mic-toggle | none), the
# SCODROP/SCOUP/SCOFAIL wire verbs and the daemon reaction on the fake graph.
test('sco_drop',
  executable('test_sco_drop',
    [
      'test_sco_drop.c',
      '../src/daemon/daemon.c',
      '../src/control/gesture.c',
      '../src/control/mpris_player.c',
      '../src/control/passthrough.c',
      '../src/util/settings.c',
      common_dir / 'm4-conf.c',
      common_dir / 'm4-i18n.c',
      '../src/util/config.c',
      '../src/bt/bt_discover.c',
      '../src/util/legacy_conf.c',
      '../src/util/earcon.c',
      '../src/core/graph_ops_fake.c',
      '../src/core/state.c',
      '../src/core/automode.c',
      '../src/core/callfsm.c',
      '../src/core/profile_rules.c',
      '../src/pw/callfsm_graph_adapter.c',
      '../src/pw/repair_a2dp.c',
      '../src/pw/graph_wait.c',
      '../src/pw/graph_pw.c',
      '../src/pw/micdetect.c',
      '../src/pw/loop_glue.c',
      '../src/ipc/ctl.c',
      '../src/ipc/hfp_events.c',
      '../src/metrics/stats.c',
      '../src/util/log.c',
    ],
    include_directories : include_directories('../src'),
    dependencies : [pipewire_dep, systemd_dep],
  ),
)

# `momentum4 config show/validate/defaults/
# migrate` end-to-end against the real compiled binary (executable() below
# comes from ../meson.build -- momentum4_exe was not previously named, so
# name it there instead of re-declaring the same sources here).
test('config_cli',
  find_program('test_config_cli.sh'),
  args : [momentum4_exe],
  timeout : 60,
)

# `momentum4 install|enable|disable|uninstall`
# end to end -- dry-run checks always run (no D-Bus at all, env -i); --yes
# checks run against tests/systemd_stub.c (a minimal fake
# org.freedesktop.systemd1.Manager) under `dbus-run-session`, skipped
# (exit 77) if dbus-run-session is not installed. NEVER run any part of
# this test against a real session bus -- see util/systemd_unit.h's own
# header comment.
systemd_stub_exe = executable('systemd_stub', 'systemd_stub.c',
  dependencies : [systemd_dep],
  build_by_default : false,
)
test('lifecycle_cli',
  find_program('test_lifecycle_cli.sh'),
  args : [momentum4_exe, systemd_stub_exe],
  timeout : 60,
)

# a separate scenario list here (so the main scenario_files list above is
# not edited concurrently)
foreach scn : ['scenarios/hfp_sco_drop_restore.scn', 'scenarios/hfp_sco_drop_fallback.scn']
  test('scenario_' + scn.split('/')[1].split('.')[0],
    scenario_runner_exe,
    args : [meson.current_source_dir() / scn],
  )
endforeach

# graph/graph_host_build.c's pure string
# builders (no PipeWire, no live system -- see that file's own header).
test('graph_host_build',
  executable('test_graph_host_build',
    [
      'test_graph_host_build.c',
      '../src/graph/graph_host_build.c',
    ],
    include_directories : include_directories('../src'),
  ),
)

# cli/wp_env.c's pure m4_wp_env_decide()
# (the file-I/O wrapper, m4_cli_wp_env_run(), is checked by an isolated
# live harness instead).
test('wp_env',
  executable('test_wp_env',
    [
      'test_wp_env.c',
      '../src/cli/wp_env.c',
      '../src/util/build_info.c',
      '../src/util/config.c',
      '../src/util/legacy_conf.c',
      '../src/bt/bt_discover.c',
      '../src/util/log.c',
      common_dir / 'm4-conf.c',
      common_dir / 'm4-i18n.c',
    ],
    include_directories : include_directories('../src'),
    dependencies : [pipewire_dep, systemd_dep],
  ),
)
