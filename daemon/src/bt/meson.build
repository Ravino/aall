# src/bt/meson.build -- the ONE BlueZ D-Bus call this project makes
# directly (org.bluez.Device1.ConnectProfile, "repair-a2dp"), split out
# per this project's established convention (see src/core/meson.build's
# comment) of one subdir per concern.
bt_sources = files(
  'bt_ops_sdbus.c',
  'bt_discover.c',
)

# In-memory test double (struct m4_bt_ops) -- test-only, never linked into
# the `momentum4` binary itself, same convention as core/callfsm_fake.c.
bt_fake_sources = files(
  'bt_ops_fake.c',
)
