# Configuration: momentum4.conf

One file, INI format with sections (`[section]`, `key = value`, `#`/`;` start a comment
line). Layers, from lowest to highest priority:

1. compiled-in defaults;
2. `@datadir@/momentum4/momentum4.conf` (shipped with the package/build -- a fully
   commented reference, the same file `momentum4 config defaults` prints);
3. `/etc/momentum4/momentum4.conf` (system overrides);
4. `~/.config/momentum4/momentum4.conf` (per-user -- usually the file you actually edit).

A higher layer overrides a lower one **key by key**, not the whole file -- you can set
only what you want to change in `~/.config/momentum4/momentum4.conf`.

## How to see every key

```
momentum4 config defaults
```

prints the full list of sections/keys with a description, range and default value --
this is the single source of truth (generated from the same registry the program
itself reads, so it can never drift from actual behavior).

```
momentum4 config show        # what is actually in effect right now, and from which layer/file/line
momentum4 config validate    # checks for typos, unknown keys, out-of-range values
```

An unknown section or key never causes a failure -- only a warning in
`config validate`/`config show`; the program keeps working.

## Substitutions in values

`${XDG_RUNTIME_DIR}`, `${XDG_CONFIG_HOME}`, `${XDG_DATA_HOME}`, `${XDG_STATE_HOME}`,
`${HOME}`, `%h` (same as `${HOME}`, for consistency with the systemd unit file format).

## Main sections

| section | what it covers | when it takes effect |
|---|---|---|
| `[general]` | interface language (`en`/`ru`) | `momentum4d reload`; a one-shot CLI command re-reads it every run |
| `[device]` | headset address/name, Bluetooth adapter | service restart |
| `[adaptive]` | Adaptive mode (`ll0`/`ll1`), latency, device filter, bridge parameters | next connection |
| `[helper]` | qemu/loader/DSP module paths, real-time priority | next helper start |
| `[hfp]` | auto-answer, SCO restore | almost immediately (mtime-cached) |
| `[stats]` | where bridge metrics are written | next helper start |
| `[daemon]`, `[call]`, `[automode]`, `[control]`, `[earcon]` | daemon core, calls, automation, gestures, sound cues | `momentum4 reload` |
| `[graph]` | combined output, microphone placeholder, fixed quantum | restart of `momentum4-graph.service` |
| `[paths]` | install paths (sockets, plugin directory, units) | service restart |
| `[cli]` | timeouts of `momentum4 ...` commands | immediately (read on every CLI invocation) |

Full descriptions and default values for every key are available only through
`momentum4 config defaults` (see above): duplicating them here would mean keeping two
sources of truth that would inevitably drift apart.

## Interface language

`[general] language = en | ru` (default `en`) selects the language of CLI output
(`--help`, `status`/`doctor`/`stats`/`config`/`log`), daemon messages in the journal,
and this registry's own key descriptions (`momentum4 config defaults`). An
unrecognized value falls back to `en` with a warning, same as any other key.
`momentum4 reload` (daemon) or `systemctl --user restart momentum4d` picks up a
change; a one-shot CLI command re-reads it fresh on every run:

```
momentum4 config set general.language ru
momentum4 reload
```

## Migrating from the old layout (two separate files)

If you still have an old sectionless `momentum4.conf` and/or a separate
`aptx-adaptive.conf`:

```
momentum4 config migrate            # dry run -- shows what would be migrated
momentum4 config migrate --apply    # migrates, makes *.v0.1 backups alongside
```
