# Where to get the aptX Adaptive encoder module

Russian version: `docs/user/obtaining-encoder.ru.md`.

momentum4 does not, and cannot, ship the codec file. It is a proprietary
Qualcomm module, part of the firmware of specific Android phones on
Snapdragon 855/865/888 and newer. You need to find it yourself in the
firmware of your own device (or a phone you have legal access to) and point
the program at it.

Legal note: the file belongs to Qualcomm and the phone manufacturer.
momentum4 only reads it locally, on your own machine, for your own audio
playback; the file is never transmitted or redistributed anywhere.
Complying with the firmware's license is the responsibility of whoever
downloads and uses it. Do not publish or forward this file to third parties.

## What to look for

File name: `capi_v2_aptX_CLHDADV_Encoder.so`.
Its path inside the firmware image is usually:
`vendor/lib/rfsa/adsp/capi_v2_aptX_CLHDADV_Encoder.so`.

This is not an ordinary ARM library but audio DSP (Hexagon) firmware, which
is why it lives separately from the usual `/system/lib64` and
`/vendor/lib64`.

## Step 1. Download your phone's firmware

Get the official firmware package (an OTA archive or a fastboot package)
from your device manufacturer's site, or a public firmware dump from an open
mirror (for example, a repository with an already-unpacked "file tree" dump
on GitHub/GitLab, where the image is unpacked page by page and you can
download a single file without pulling the whole image). You need firmware
for a device on Snapdragon 855 or newer that supports aptX Adaptive.

## Step 2. Extract the ADSP partition

The image format depends on the manufacturer, usually one of:

- `payload.bin` (Google/AOSP-based OTA packages) -- unpack it with
  `payload-dumper-go` or `payload_dumper` (Python) to get `vendor.img`.
- `super.img` (dynamic partitions) -- unpack with `lpunpack`, then look for
  `vendor.img` inside.
- `new.dat.br` (the old Android sparse format) -- first `brotli -d`, then
  `sdat2img.py`.
- The `vendor.img`/`system.img` itself might be `ext4` (`simg2img` to get an
  ordinary image, then mount read-only or extract files with `debugfs -R`)
  or `erofs` (extract with `fsck.erofs --extract=out/` or `7z x`; recent
  7-Zip versions understand EROFS).

## Step 3. Find the file and verify its hash

Inside the unpacked `vendor` tree, find it:

```
find . -iname 'capi_v2_aptX_CLHDADV_Encoder.so'
```

Compute its sha256:

```
sha256sum capi_v2_aptX_CLHDADV_Encoder.so
```

Compare it against the known hashes in this repository's
`data/capi-sha256.txt`. If the hash matches a line marked `LOADER-TESTED`,
this exact file has been verified to work with our loader. If the hash is
unknown, the program refuses to use it without the explicit `--force` flag
(see below), because an incompatible ABI can crash the helper process (this
does not affect sound: if the module fails to start, the ordinary aptX HD
codec is used instead).

## Step 4. Point the program at the file

```
momentum4 setup --capi /path/to/capi_v2_aptX_CLHDADV_Encoder.so
```

The program checks that it is a Hexagon ELF (`EM_HEXAGON`), verifies its
sha256 against the known list, and copies the file into
`~/.local/share/momentum4/capi/`. If the hash is not in the list, the command
fails; to use the file anyway at your own risk, add `--force`.

## If it does not work out

That is fine: without this file, aptX Adaptive simply never turns on, and
the phone/headset keep working on the ordinary aptX HD -- a stock, verified
mode, sound never stops.
