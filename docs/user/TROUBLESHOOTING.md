# Troubleshooting

Russian version: `docs/user/TROUBLESHOOTING.ru.md`.

The first step is almost always the same:

```
momentum4 doctor
```

This is a read-only command -- it changes and restarts nothing. Every `[WARN]`
line is a specific problem with an explanation of what to do. `[OK]`/`[--]`
mean everything is fine, or a not-yet-installed component is normally absent.

## The headset connected, but the codec is not Adaptive

1. `momentum4 doctor` -- check the line about the DSP module (is the sha256
   known?) and about the PipeWire version/plugin (`wp-env`, "version
   matches"?).
2. If there is no DSP module -- this is expected: without it, Adaptive never
   turns on, and the headset runs on the ordinary codec. See
   `docs/user/obtaining-encoder.md`.
3. If the installed PipeWire version does not match the one the plugin was
   built against -- rebuild the plugin (`make module && make install`)
   against your installed PipeWire version.
4. If your headset is not a Sennheiser Momentum 4 and does not pass the
   `allow-devices = adk-stereo` filter -- try `allow-devices = any` in
   `[adaptive]` temporarily (PipeWire will then offer Adaptive to any
   compatible receiver), or list its MAC address(es) explicitly.

## `doctor` says the plugin is built and in place, but `aptx_adaptive` does not show up in WirePlumber

This is most likely `[paths] spa-system-dir` -- the path to your
distribution's own (not ours) `spa-0.2` plugins, determined at daemon build
time from `pkg-config --variable=plugindir libspa-0.2`. `momentum4 wp-env`
(run automatically before WirePlumber starts, via our overlay) deliberately
**does not touch** `SPA_PLUGIN_DIR` if this path is empty or does not exist --
because replacing `SPA_PLUGIN_DIR` with only our own directory (without the
system one) crashes WirePlumber with SIGSEGV on startup (it loses access to
its stock `support.*` plugins). Check:

```
momentum4 config show | grep spa-system-dir
pkg-config --variable=plugindir libspa-0.2
```

If the second command prints something, but the first is empty or shows a
different path, set it explicitly in `momentum4.conf`:

```
[paths]
spa-system-dir = /usr/lib/x86_64-linux-gnu/spa-0.2   # your path
```

then restart WirePlumber (`momentum4 enable` does this for you). If
`pkg-config --variable=plugindir libspa-0.2` also prints nothing, you
probably do not have the `libspa-0.2-dev`/`pipewire-dev` package installed
(see "Build dependencies" in README.md); rebuild `make daemon`/`make install`
after installing the right package.

## Sound is gone entirely (not just Adaptive)

```
momentum4 disable --yes
```

Rolls back our part and restarts WirePlumber -- sound should return on the
ordinary codec afterward. If this does not help either, the problem is not
momentum4 but PipeWire/WirePlumber/BlueZ itself; check
`journalctl --user -u wireplumber -u pipewire`.

## The screen reader went silent after `enable`/`disable`

This is a rare side effect of the WirePlumber restart itself (not specific to
momentum4 -- the same thing sometimes happens after a manual `systemctl
--user restart wireplumber`). momentum4 deliberately does not try to fix
speech-dispatcher/Orca -- restart speech-dispatcher or your session the usual
way for your desktop.

## The helper (qemu-hexagon) does not start / restarts frequently

```
momentum4 stats
momentum4 log -n 200
```

`stats` shows bridge metrics (buffer underruns, encoder latency); `log` shows
the latest messages. Frequent restarts usually mean either the DSP module is
the wrong one (check the sha256 via `doctor`), or real-time privileges are
missing (`ulimit -r`, see `[helper] rt-priority` in `momentum4 config
defaults`) -- this is not critical for sound, the bridge simply runs without
real-time priority.

## The build cannot find a Hexagon-capable clang

The build error for `loader` names the specific versions it checks
(`clang-14` .. `clang-19`, then plain `clang`), and what to do. If you have a
different clang version installed at a non-standard path, point at it
explicitly:

```
meson configure build-top -Dhexagon_cc=/path/to/clang
```

## No network access / no access to download.qemu.org for `make qemu`

Building your own `qemu-hexagon` is an optional step. The system package
`qemu-user-static`/`qemu-user` (if it provides `qemu-hexagon`) also works,
just slower (higher CPU load per packet). Point at it explicitly in `[helper]
qemu` if it is not where `momentum4` looks by default (`doctor` will tell
you).
