# qemu/ — building `qemu-hexagon` (user-mode emulation)

`build-qemu-hexagon.sh` builds `qemu-hexagon` (`linux-user` only, only the
`hexagon-linux-user` target; `qemu-system-*` is not built) from the official
QEMU sources. This is an optional step: a system `qemu-user-static`/`qemu-user`
package works too (if it provides `qemu-hexagon`), just slower (higher
per-packet CPU load — see the root README.md, "QEMU" section).

## Usage

```
./build-qemu-hexagon.sh                          # default version (11.1.0), checked sha256
./build-qemu-hexagon.sh --version 12.0.0 --sha256 <hex>   # another version -- sha256 is required
./build-qemu-hexagon.sh --skip-verify            # skip the sha256 check (not recommended)
./build-qemu-hexagon.sh --prefix /wherever
```

Result: `<prefix>/bin/qemu-hexagon` (default `qemu/build/install/bin/qemu-hexagon`).
`make qemu` (from the repository root) does the same thing with default
parameters; `make install` then picks up the built binary automatically if it
exists at that path (otherwise it looks for the system `/usr/bin/qemu-hexagon`).

## Dependencies

Nothing here is installed automatically (no `apt install` is run by the
script) — it only checks explicitly and reports, in a clear message, which
packages are missing.

| package (Debian/Ubuntu) | why |
|---|---|
| `python3` | needed by QEMU's own `configure`/build system (meson runs on Python) |
| `python3-venv` | QEMU's `configure` creates its own private virtual environment and installs `meson` into it from a wheel file **bundled with the QEMU sources** (`python/wheels/meson-*.whl`) — no PyPI/network access involved. `python3-venv` is the only system requirement for this mechanism; the script itself never calls `pip install` |
| `ninja-build` | the build backend QEMU uses |
| `libglib2.0-dev` | a mandatory dependency of `qemu-user` for any target |
| `flex`, `bison` | the instruction-decoder generator for new targets (including hexagon) |
| `curl` | to download the source tarball |

The script never runs `pip install` or `apt install` itself — if something is
missing, it stops with a message like "missing: `<package>`,
`sudo apt install <package>`".

## Integrity check

By default the script checks the sha256 of the downloaded tarball against a
known value for version 11.1.0 (pinned in the script itself; verified by
independently re-downloading from `download.qemu.org` on two different days —
both times byte-for-byte identical). The official QEMU site does not publish
a separate checksum/signature file to verify against — so this only confirms
download reproducibility (the same file on repeated downloads), not an
independent third-party signature. For any other version, `--sha256` must be
given explicitly (compute `sha256sum` yourself after manual verification, or
use `--skip-verify` at your own risk).

## License

QEMU is distributed under GPLv2 (and other licenses for individual files) as
part of the QEMU Project. This script only downloads and builds the official
release tarball — momentum4 does not redistribute the built `qemu-hexagon`
binary and does not include the QEMU sources in its repository or package, so
GPLv2 source-distribution obligations do not apply here: anyone running this
script has direct access to the same official sources from the same official
site.
