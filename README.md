# AALL — aptX Adaptive Low Latency for the Sennheiser MOMENTUM 4

AALL gets a Bluetooth-connected Sennheiser MOMENTUM 4 talking to your Linux computer
over the **aptX Adaptive** codec (Low Latency mode) through your computer's own
Bluetooth adapter — no USB dongle, no phone in the middle. The command-line tool and
daemon it installs are both called `momentum4`.

## What it is, and who it's for

AALL is three parts that together let a Linux computer speak aptX Adaptive
(specifically its Low Latency mode) to compatible headphones over PipeWire/WirePlumber:

1. **A plugin** for WirePlumber/PipeWire (SPA bluez5) that adds the `aptx_adaptive`
   codec to the list PipeWire offers Bluetooth headphones on connect.
2. **An encoder** — Qualcomm's proprietary DSP module (Hexagon ADSP), run under
   user-space QEMU emulation (no phone, no firmware flashing, no root required). This
   module is not distributed with AALL — see "Requirements" and
   `docs/user/obtaining-encoder.md`.
3. **A daemon and CLI, `momentum4`** — install/enable/disable, diagnostics, call
   handling and headphone gestures, one shared config file.

Built and tested against a Sennheiser MOMENTUM 4. By construction it should also work
with any headphones that expose the typical stock "stereo headset" capability profile
many Qualcomm aptX-Adaptive-capable chips declare — by default the plugin only enables
the codec for devices matching that exact fingerprint (`[adaptive] allow-devices =
adk-stereo`), so it won't reach for someone else's headphones with a different
(possibly incompatible) set of Adaptive parameters. An explicit MAC address list, or
turning the filter off entirely, is also supported — see "Configuration".

**If you don't have aptX-Adaptive-capable headphones**, or don't want to deal with the
DSP module, you don't need this project: without the module the codec simply never
appears, and your headphones keep working exactly as they did before (aptX
HD/aptX/SBC — whatever they already support).

## Why I built this

I got tired of wired headphones, and then of wireless ones that only reach low
latency through a USB dongle — dongles that break, get lost, or stop working. I wanted
more: headphones that connect straight to the laptop's own Bluetooth and still keep up
with a fast screen reader. Now there is AALL — aptX Adaptive Low Latency on Linux.

## Status and supported systems

| component | version | how to check |
|---|---|---|
| distribution | Debian 12 (bookworm) | tested; other distributions with the same PipeWire version likely also work, but are untested |
| PipeWire | 1.4.2 | `pipewire --version` / `pkg-config --modversion libpipewire-0.3` |
| WirePlumber | any version compatible with PipeWire 1.4.2 | `wireplumber --version` |
| kernel | any modern 6.x kernel with support for your Bluetooth adapter | — |
| Bluetooth stack | BlueZ 5.6x | `bluetoothctl --version` |

By default the plugin only enables the codec for the Sennheiser MOMENTUM 4's
capability fingerprint, described above. Other Qualcomm-based aptX Adaptive
headphones with the same stock stereo-headset profile are expected to work too, but
have not been tested by the author.

The plugin is built and patched against exactly PipeWire 1.4.2 (see `module/SUPPORTED`).
On a different PipeWire version the build will refuse with a clear message; after a
PipeWire upgrade you need to rebuild and reinstall the plugin (`make module && make
install`).

## How it works

```
application -> PipeWire -> SPA bluez5 plugin (our aptx_adaptive codec)
            -> helper process (qemu-hexagon + DSP module) -> compressed frames
            -> AVDTP (Bluetooth A2DP) -> headphones
```

The plugin registers the `aptx_adaptive` codec in the list PipeWire offers over AVDTP
when headphones connect. If the headphones support Adaptive and accept the
configuration, PipeWire starts encoding through this path instead of the stock aptX
HD/SBC.

Two modes cover the two things you actually want from a Bluetooth headset:

- **AALL** (aptX Adaptive Low Latency, A2DP) — the default, for listening to
  anything, screen reader included.
- **Phone-call mode** (HFP, mSBC with a CVSD fallback) — used automatically (or on
  request) whenever an application needs the headset's microphone, since aptX
  Adaptive's A2DP profile has no return audio channel at all. See "Calls and gestures".

By default, one gesture toggles between the two (a forward swipe on the headset
enters call mode; the headset's own physical hang-up gesture, sent as an `AT+CHUP`
command over HFP, returns you to AALL), and the button on the headset itself
mutes/unmutes the microphone during a call. All of this is remapped through
`[control]` in `momentum4.conf` — see "Configuration".

### QEMU, and why it does not need KVM

The DSP module is compiled for the Hexagon (QDSP6) architecture, not x86_64. To run it
on an ordinary computer, AALL uses **QEMU in user-mode emulation** (`linux-user`, TCG
engine — a software JIT translator): one `qemu-hexagon` process translates Hexagon
instructions into x86-64 on the fly and runs only that one module, not an entire
operating system.

This is **not a virtual machine**: there is no guest kernel, no disk, and `/dev/kvm`
is neither used nor usable — KVM accelerates virtualizing the *same* architecture
(x86-on-x86); here a *different* architecture is being translated (Hexagon on
x86-64), for which no hardware acceleration exists at all. CPU cost is a few percent
of one core (measured: about 6% of one core with a freshly built QEMU; the Debian
`qemu-user-static` package is slower but also works).

A longer, from-first-principles explanation of the whole pipeline is in
`docs/user/HOW-IT-WORKS.md` (`docs/user/HOW-IT-WORKS.ru.md` in Russian).

## Requirements

Build dependencies (build-time only, not needed at runtime) on Debian 12:

```
# common + daemon/CLI + loader (`make`)
sudo apt install meson ninja-build pkg-config gcc clang lld \
    libpipewire-0.3-dev libsystemd-dev

# WirePlumber/PipeWire plugin (`make module`) -- it patches and rebuilds
# bluez5 from source, so it needs headers for ALL the codecs/libraries
# bluez5 supports, not just aptX:
sudo apt install libdbus-1-dev libbluetooth-dev libsbc-dev libopus-dev \
    libusb-1.0-0-dev libldacbt-enc-dev libldacbt-abr-dev \
    libfreeaptx-dev liblc3-dev libglib2.0-dev
```

`libpipewire-0.3-dev`/`libspa-0.2-dev` at **≥1.4.2** are **not in Debian 12's main
repository** (it ships 0.3.65) — install them from `bookworm-backports`:

```
echo "deb http://deb.debian.org/debian bookworm-backports main" | \
    sudo tee /etc/apt/sources.list.d/bookworm-backports.list
sudo apt update
sudo apt install -t bookworm-backports pipewire wireplumber \
    libpipewire-0.3-dev libspa-0.2-dev
```

(Without this step, `meson setup` for the daemon won't find a candidate matching
`-Dpipewire_min_version`.)

Building the encoder loader additionally needs a `clang` with Hexagon target support
(present in the `clang`/`clang-<version>` package from version 14 onward, already
included above) and `lld`. Building your own `qemu-hexagon` is optional — see
`qemu/README.md`.

### The proprietary DSP module (required for sound through Adaptive)

aptX Adaptive is implemented in a proprietary Qualcomm module
(`capi_v2_aptX_CLHDADV_Encoder.so`), shipped as part of the ADSP firmware of certain
Snapdragon-based Android phones. AALL **does not and cannot distribute** this file —
you need to find it yourself, in the firmware of a device you have legal access to.
Step-by-step instructions: `docs/user/obtaining-encoder.md`. Without this file
everything else still works, the Adaptive codec just never appears (headphones use
whatever they support on their own).

## Build

```
git clone <URL> aall && cd aall
make               # common + daemon/CLI + loader (for the DSP module)
make module        # WirePlumber/PipeWire plugin (needs network once -- the PipeWire 1.4.2 tarball)
make qemu          # (optional) build your own qemu-hexagon; without this step
                    # a system qemu-user-static also works, just slower
make check         # tests (common + daemon; the plugin is tested separately,
                    # see the header comment in each module/test/*.c file)
```

Plain `make` doesn't touch the network if you don't need the module or QEMU yet (e.g.
while working on the daemon itself), and it actually BUILDS (not just prints help) —
`make help` prints the target list above without building anything. Build options are
documented in `meson_options.txt` (`-Ddev_build`, `-Dhexagon_cc`,
`-Dpipewire_min_version`).

If `clang` lacks Hexagon target support (or isn't installed), building `loader` fails
with a clear error naming the missing package, not a silent failure.

## Install

```
make install PREFIX=/usr/local     # system-wide (needs root/permissions on PREFIX)
# or, without root:
make install PREFIX=$HOME/.local
```

Then set up the DSP module (see above) and wire everything together with `momentum4`:

```
momentum4 setup --capi /path/to/capi_v2_aptX_CLHDADV_Encoder.so   # see docs/user/obtaining-encoder.md
momentum4 install               # dry run by default -- shows the plan, changes nothing
momentum4 install --yes         # applies it: config, WirePlumber override, units (does not start anything)
momentum4 enable --yes          # starts everything and restarts WirePlumber -- audio will cut out briefly
```

`momentum4 install` without `--yes` **changes nothing** — it only shows what would be
done (updating `momentum4.conf`, a WirePlumber override file, enabling systemd units).
`momentum4 enable` is the only command that restarts WirePlumber (needed for it to
pick up the new plugin), which briefly interrupts audio on **every** device, not just
your headphones; it prints a warning before doing so.

Then pair/connect your headphones as usual (via `bluetoothctl` or your desktop's own
applet) — if your model is supported and the DSP module is in place, PipeWire will
offer it the `aptx_adaptive` codec.

## Enable and disable

- `momentum4 enable [--yes]` — turn on (starts the units, restarts WirePlumber).
- `momentum4 disable [--yes]` — roll back: stop our units, remove the WirePlumber
  override, restart WirePlumber. Headphones fall back to whatever codec they
  supported before (usually aptX HD). This is the fastest way to get audio back to
  normal if something isn't working.
- `momentum4 uninstall [--purge] [--yes]` — `disable`, plus removes the installed unit
  links; with `--purge`, also removes `momentum4.conf` and your copy of the DSP
  module (only the one `momentum4 setup --capi` itself placed there).

Both `enable` and `disable` default to a **dry run**: they print the plan and the
WirePlumber-restart warning, changing nothing until you add `--yes`.

## Calls and gestures

The `momentum4` daemon (runs as a systemd `--user` service) can:

- answer/end calls by driving the HFP/RFCOMM link for headphones that aren't
  themselves connected to a phone app (`AT+CHUP`/`ATA`/`BLDN`/`BVRA` and similar HFP
  headset commands);
- expose playback state to the headphones through its own MPRIS player registered in
  BlueZ, so gestures like pause/next/previous on the headphones themselves reach the
  real player on your desktop (via MPRIS forwarding);
- recover the HFP/SCO link and microphone after a drop, without a manual reconnect.

All of this is configured through the `[hfp]`/`[control]`/`[automode]` sections of
`momentum4.conf` (see below), and by default only applies to the device identified as
your headphones (`[device] address`/`name-match`) — not to any HFP headset in range.

## Configuration

One file, INI-style with sections: `~/.config/momentum4/momentum4.conf` (per-user,
overrides the system-wide `/etc/momentum4/momentum4.conf`, which overrides the
built-in defaults). The full, fully-commented reference of every key, with
descriptions — no need to dig through source:

```
momentum4 config defaults          # prints the reference to stdout
momentum4 config defaults > ~/.config/momentum4/momentum4.conf   # ... as a starting point
momentum4 config show              # what actually applies right now, and why (file:line)
momentum4 config validate          # syntax/range check, without applying anything
```

Changes to `[adaptive]`/`[helper]`/`[stats]` take effect on the next headphone
connection (no WirePlumber restart needed); `[hfp]` takes effect almost immediately;
`[daemon]`/`[call]`/`[control]`/`[earcon]`/`[automode]` need `momentum4 reload`;
`[graph]` needs `momentum4-graph.service` to be restarted. Details are in the
reference itself (`momentum4 config defaults`), which states exactly when each key
applies.

The most likely things a new user wants to change:
- `[adaptive] mode` (`ll0`/`ll1`) and `latency-adjust-ms` — the latency/stability
  trade-off, tuned by ear;
- `[adaptive] allow-devices` (`adk-stereo` by default, `any`, or a comma-separated MAC
  list) — if your headphones aren't recognized by the stock stereo-headset
  fingerprint;
- `[hfp] call-control`/`sco-restore` — turn call automation on or off;
- `[graph] combine`/`mic-placeholder` — for a single combined output across several
  devices, or a persistent virtual microphone;
- `[general] language` (`en` by default, or `ru`) — the language of CLI output
  (`--help`, `status`/`doctor`/`stats`/`config`/`log`) and of daemon messages in the
  journal. Change it with `momentum4 config set general.language ru`, then
  `momentum4 reload` (or restart `momentum4d`) for a running daemon to pick it up —
  a one-shot CLI command re-reads it fresh on every run regardless.

### Migrating from an older, flat config layout

If you still have an old, section-less `momentum4.conf` and/or a separate
`aptx-adaptive.conf`:

```
momentum4 config migrate            # dry run -- shows what would move
momentum4 config migrate --apply    # migrates, keeps *.v0.1 backups alongside the originals
```

## Diagnostics

```
momentum4 doctor     # read-only: PipeWire version, DSP module and its sha256,
                      # qemu/loader, units, overlay -- flags anything that still
                      # points at a development checkout instead of a release install
momentum4 stats       # aptX Adaptive encoder-bridge metrics (latency, buffer under/overruns)
momentum4 stats --json --history 50
momentum4 log -n 100 --follow   # recent momentum4 journald entries
momentum4 status      # short summary: are the headphones connected, and with which codec
```

`doctor` never fixes or restarts anything — it only reports what's wrong and what to
do about it. It does **not** check the state of speech-dispatcher/your screen
reader (see "Accessibility" below) — that's outside its scope.

For specific problems and their fixes, see `docs/user/TROUBLESHOOTING.md`
(`docs/user/TROUBLESHOOTING.ru.md` in Russian).

## Uninstall

```
momentum4 uninstall --purge --yes
make uninstall PREFIX=...     # removes exactly what `make install` placed
```

After `disable`/`uninstall`, headphones use whatever codec they'd have picked without
AALL — nothing is left changed in the headphones' own firmware or in BlueZ.

## Accessibility (for screen reader users)

This project exists primarily because of latency and speech clarity for a screen
reader (Orca/speech-dispatcher) over Bluetooth headphones, so a few things are
deliberate:

- **`install`/`enable`/`disable`/`uninstall` are dry runs by default.** None of them
  touch audio until you explicitly add `--yes`. Before anything that could interrupt
  audio (restarting WirePlumber — `enable`/`disable`), the command prints a warning
  first.
- **If the sound disappears entirely** — the shortest way back:
  ```
  momentum4 disable --yes
  ```
  It stops our part and returns the headphones to their usual codec. If you don't
  have access to text output at that moment, bind the same command to a key/gesture
  on your desktop ahead of time (`momentum4 disable --yes` is a plain shell command,
  no interactive prompts once `--yes` is given).
- **AALL itself does not check or repair speech-dispatcher/Orca.** If, after a
  WirePlumber restart (the only point where this can happen at all), the screen
  reader goes silent on its own, that's a separate, non-AALL-specific problem with
  restarting the audio stack — restart speech-dispatcher or your desktop session the
  usual way for your system.
- Every long-running operation (`install --yes`, `enable --yes`, `disable --yes`,
  `uninstall --yes`) prints a report **after** it finishes — what was done, what
  failed (if anything did), and what to do next — rather than exiting silently.

Note on language: CLI output and daemon messages are in English by default; set
`[general] language = ru` for Russian (see "Configuration"). A Russian mirror of this
README is at `docs/user/README.ru.md`.

## Known limitations

- aptX Adaptive's own Bluetooth profile has no built-in return microphone channel —
  during a call, the regular HFP profile (mSBC/CVSD) is used instead of Adaptive; this
  is a limitation of the A2DP+HFP Bluetooth profiles themselves, not of this project.
- The plugin is built and tested against one specific PipeWire version at a time (see
  "Requirements") — after a distribution upgrade, if the PipeWire version changed, the
  plugin needs a rebuild; if the versions don't match, WirePlumber automatically loads
  its own stock plugins instead of ours (see `momentum4 doctor`), and audio doesn't
  drop, Adaptive is just temporarily unavailable.
- Linux x86_64 only; not tested on other architectures.
- Latency figures mentioned in this project's documentation are rough, on-device
  estimates, not guarantees — actual latency depends on your Bluetooth adapter,
  headphones, and PipeWire graph configuration.

## Acknowledgments

AALL builds on, and was informed by, public work from others:

- [LineageOS `android_vendor_qcom_opensource_system_bt`](https://github.com/LineageOS/android_vendor_qcom_opensource_system_bt) —
  reference for the on-air aptX Adaptive configuration byte layout.
- [`arkq/openaptx`](https://github.com/arkq/openaptx), pull request #12 (Arkadiusz
  Bokowy and contributors) — prior art for parsing the aptX Adaptive OTA transport
  header; see `loader/hexagon/ota.h`.
- [`baizhu945/pipewire`](https://github.com/baizhu945/pipewire) — prior art for an
  aptX Adaptive PipeWire codec plugin.
- [Audioreach](https://github.com/Audioreach) (Qualcomm, BSD-3-Clause) — reference for
  the Hexagon CAPI v2 module vtable layout; see `loader/hexagon/capi_v2.h`.
- The [PipeWire](https://pipewire.org/), [BlueZ](http://www.bluez.org/) and
  [QEMU](https://www.qemu.org/) projects, without which none of this would run at all.

## License and disclaimer

AALL itself is released under the [MIT License](LICENSE).

aptX and Adaptive are trademarks of Qualcomm; Sennheiser and MOMENTUM are trademarks
of Sennheiser. This project is not affiliated with, endorsed by, or sponsored by
Qualcomm or Sennheiser. Obtaining, storing and using the Qualcomm CAPI encoder module
described under "Requirements" is entirely your own responsibility, subject to the
license terms of the firmware you took it from — this project only ever reads that
file locally, on your own machine, to play your own audio, and neither distributes
nor transmits it anywhere.

This software is provided as-is, without warranty of any kind — see `LICENSE` for the
full text.
