#!/usr/bin/env python3
"""tools/build/meson-option.py -- prints one resolved meson build option.

Build-time helper only (the packaging pipeline's own carve-out:
Python is needed anyway, since meson itself runs on it -- this script is in the
same category, never shipped/run on an end user's machine after install,
only invoked by ../../Makefile while it IS installing). Reads
<builddir>/meson-info/intro-buildoptions.json (written by `meson setup`
itself) instead of shelling out to `meson introspect` again, to avoid a
second, slower meson invocation per option.

Usage: meson-option.py BUILDDIR OPTION_NAME
"""
import json
import sys


def main() -> int:
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} BUILDDIR OPTION_NAME", file=sys.stderr)
        return 2
    builddir, name = sys.argv[1], sys.argv[2]
    path = f"{builddir}/meson-info/intro-buildoptions.json"
    try:
        with open(path, encoding="utf-8") as f:
            opts = json.load(f)
    except OSError as exc:
        print(f"{path}: {exc}", file=sys.stderr)
        return 1
    for opt in opts:
        if opt.get("name") == name:
            print(opt["value"])
            return 0
    print(f"{path}: no such option {name!r}", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
