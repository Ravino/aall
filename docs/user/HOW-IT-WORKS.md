# How this works (short version, for the curious)

Russian version: `docs/user/HOW-IT-WORKS.ru.md`.

## The codec

aptX Adaptive is a Qualcomm codec. Its encoder is not ordinary code for the main
CPU -- it is a module for a separate signal processor (the Hexagon ADSP) that
Qualcomm phones ship next to the main CPU specifically for audio/telephony.
That module lives in the firmware of phones on Snapdragon 855 and newer (wherever
Adaptive is supported in hardware).

Because it is not ordinary x86/ARM code but code for a different, specialized
processor, it cannot simply be linked and called directly on a regular computer.
momentum4 works around this with emulation: a small loader program runs under
`qemu-hexagon` (user-mode emulation, see README.md's "QEMU" section), loads the
DSP module itself as a shared library inside the emulated process, and calls its
encoding API. From PipeWire's point of view this looks like an ordinary external
helper process: PCM audio goes in, compressed aptX Adaptive frames come out, with
a latency of a few milliseconds.

## The audio path

```
application -> PipeWire -> SPA bluez5 plugin (our aptx_adaptive codec)
            -> helper (qemu-hexagon + DSP module) -> compressed frames
            -> AVDTP (Bluetooth A2DP) -> headset
```

The plugin registers the `aptx_adaptive` codec in the list of codecs PipeWire
offers over AVDTP when the headset connects. If the headset supports Adaptive
and accepts the configuration, PipeWire starts encoding through this path
instead of the stock aptX HD/SBC.

## Why a separate helper process instead of part of WirePlumber itself

QEMU's emulation generates executable code on the fly (JIT) -- it needs the
right to mark memory executable (`mprotect(PROT_EXEC)`). The stock WirePlumber
unit on modern distributions runs with hardening (`MemoryDenyWriteExecute=yes`)
that forbids exactly that -- correct for WirePlumber itself, but incompatible
with JIT emulation running inside it. That is why the helper is a separate
systemd `--user` unit without this restriction, while WirePlumber itself keeps
its full stock hardening.

## Why the default filter is not by device name

Different headsets on Qualcomm chips can advertise Adaptive support, but with
different parameters of their own (latency, sample rate, bitrate). momentum4
only knows one parameter set, verified against the Sennheiser Momentum 4 (in
practice this is a common factory "stereo headset" capability profile that
many Qualcomm chips declare, not something specific to one model only). So by
default the codec is only offered to receivers with the same capability
fingerprint (`allow-devices = adk-stereo`) -- not by device name, but by what
it actually declares in its AVDTP capabilities. For a headset from another
maker with a different parameter set, this same codec might play silence or
distorted audio -- hence the cautious default, with an explicit way to turn
it off (`allow-devices = any`, or a MAC list) if you know exactly what you are
doing.

## Protection against a PipeWire version mismatch

The plugin is built and tested against one specific PipeWire version. On
startup WirePlumber compares the installed plugin's version against
PipeWire's own -- if they do not match (or the plugin is not installed at
all), WirePlumber loads its stock plugins, as if momentum4 were not installed
at all. In that case the headset uses the ordinary codec -- there is never
silence.
