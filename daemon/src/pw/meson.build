# src/pw/meson.build -- the "talks to live PipeWire" sources,
# split out of the root meson.build (add your
# own files via src/pw/meson.build as a subdir, touch the root meson.build
# as little as possible). files() (not plain strings) so `pw_sources` resolves
# correctly for the caller (root meson.build's executable('momentum4', ...))
# regardless of subdir() nesting.
pw_sources = files(
  'graph_pw.c',
  'loop_glue.c',
  'micdetect.c',
  'callfsm_graph_adapter.c',
  'repair_a2dp.c',
  'graph_wait.c',
)
