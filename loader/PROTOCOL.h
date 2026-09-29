/*
 * momentum4 adaptive-helper: host <-> qemu-hexagon guest wire protocol.
 *
 * Shared between an internal x86_64 measurement tool and
 * loader/hexagon/main.c (Hexagon guest, runs under a qemu-hexagon
 * linux-user build). Deliberately tiny and offline-only: no network, no
 * PipeWire/BlueZ.
 *
 * Framing (all integers little-endian u32, matches host and guest byte
 * order since both are LE):
 *
 *   guest -> host, once at startup:
 *     u32 status            (0 = loaded + CAPI init + ENC_INIT ok)
 *     u32 detail            (0, reserved)
 *
 *   host -> guest, per audio block:
 *     u32 pcm_bytes          (payload size, <= PCM_BYTES_MAX)
 *     u8  pcm[pcm_bytes]     interleaved S16LE, 48000 Hz, stereo
 *
 *   guest -> host, per audio block:
 *     u32 status             (0 = ok, nonzero = errno-style code)
 *     u32 frame_bytes        (0 if the module is still buffering, i.e. no
 *                             output produced yet for this call)
 *     u8  frame[frame_bytes] raw bytes returned by capi_v2_..._process()
 *                             on output port 0 (may or may not include an
 *                             8-byte OTA transport header depending on the
 *                             transport variant in use).
 *
 *   host -> guest, control (an alternative to the per-block PCM path,
 *   framed the same as PCM but with pcm_bytes == CONTROL_MARKER):
 *     u32 CONTROL_MARKER (0xffffffff)
 *     u32 command
 *     u32 payload_bytes
 *     u8  payload[payload_bytes]
 *   guest replies with the same {status, 0} shape as the startup reply.
 *
 * This is our own minimal protocol; it is NOT the openaptx PR#12 protocol
 * (github.com/arkq/openaptx, different CAPI generation, different config
 * payloads), only inspired by its overall shape.
 *
 * Transport-agnostic: the framing above only assumes a byte stream with
 * independent read/write directions, which both a pair of anonymous pipes
 * (fd 0=in/1=out of a fork()+exec()'d qemu-hexagon, the original design)
 * and a single connected AF_UNIX SOCK_STREAM socket (systemd
 * socket-activated momentum4-helper@.service,
 * StandardInput=socket/StandardOutput=socket, i.e. "inetd style":
 * read()/write() on fd 0/1 both refer to the same socket) satisfy
 * identically. Nothing in this file or in loader/hexagon/main.c's
 * read/write loop needs to change between the two transports -- only the
 * host-side code that establishes the stream (fork+exec+pipe() vs.
 * connect(AF_UNIX)) differs, see
 * module/pipewire-src/spa/plugins/bluez5/m4-adaptive-bridge.c.
 */

#ifndef MOMENTUM4_ADAPTIVE_HELPER_PROTOCOL_H
#define MOMENTUM4_ADAPTIVE_HELPER_PROTOCOL_H

#define AH_CONTROL_MARKER 0xffffffffu
#define AH_PCM_BYTES_MAX (64u * 1024u)
#define AH_FRAME_BYTES_MAX 4096u

#define AH_CMD_SET_BITRATE_LEVEL 1u /* payload: u32 level 1..5 */

#endif
