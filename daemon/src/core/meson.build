# daemon/src/core/meson.build -- pure-logic sources (no PipeWire/D-Bus/
# file I/O), kept in their own subdir on purpose: profile_rules.c,
# automode.c, callfsm.c and its test double callfsm_fake.c live here,
# independent of pw/* and core/graph_ops.h (real PipeWire I/O) -- see
# callfsm_ops.h for why callfsm.c has its own, narrower interface than
# struct graph_ops.

core_sources = files(
  'profile_rules.c',
  'automode.c',
  'callfsm.c',
  'state.c',
)

# callfsm_fake.c / graph_ops_fake.c are deterministic in-memory test doubles
# (m4_callfsm_ops_t / struct graph_ops respectively) used ONLY by tests/ --
# never linked into the `momentum4` binary itself.
core_fake_sources = files(
  'callfsm_fake.c',
  'graph_ops_fake.c',
)
