/* momentum4: aptX Adaptive SPA codec <-> qemu-hexagon helper bridge.
 * SPDX-License-Identifier: MIT
 *
 * This is NOT upstream PipeWire code. It is our own addition, built only in
 * an out-of-tree copy of spa/plugins/bluez5. It
 * must never be loaded by a production WirePlumber without an explicit,
 * reviewed decision.
 *
 * Design :
 *
 *   The real-time data thread (WirePlumber's bluez5 media-sink data loop)
 *   calls struct media_codec::encode() once per ~10 ms PCM block -- but NOT
 *   at a steady 10 ms cadence. media-sink.c's flush_data() deliberately
 *   pre-encodes ahead of its own send timer ("encode next packet already
 *   now; it will be flushed later on timer", media-sink.c ~line 974) whenever
 *   more source PCM is already available in the *same* graph cycle, which
 *   happens on essentially every cycle once the graph quantum (512-1024
 *   samples, see ~/.config/pipewire/pipewire.conf.d clock.min-quantum, vs.
 *   this codec's fixed 480-sample/10 ms block) exceeds one block. That means
 *   encode() can legitimately be called two or more times back-to-back, with
 *   next to no wall-clock time between calls (same thread, no scheduling gap
 *   at all) -- found live (crackle/lag from back-to-back
 *   calls). Each call MUST still return in a bounded, sub-millisecond
 *   time: it must never block on the qemu-hexagon child process, which can
 *   stall for tens of ms (TCG warm-up, page faults, host scheduling) or
 *   simply die.
 *
 *   So encode() never talks to the child directly. A separate, non-RT "IO
 *   thread" owns the two pipes to `qemu-hexagon <hx_adaptive_loader> <capi.so>`
 *   and does the blocking read()/write() dance from
 *   loader/PROTOCOL.h (mirrored here as m4-adaptive-protocol.h).
 *   It exchanges PCM blocks and encoded frames with the RT thread through two
 *   small bounded ring buffers ("in" and "out", M4_AH_RING_SLOTS_MAX slots each,
 *   in FIFO order), each guarded by its own pthread_mutex_t:
 *
 *     - RT thread (producer for "in", consumer for "out"): only ever calls
 *       pthread_mutex_trylock(). If the lock is held (by the IO thread,
 *       which only ever holds it for the duration of one memcpy -- never
 *       across a read()/write() syscall to the child), the RT thread simply
 *       skips that ring operation this cycle. Bounded to a handful of CPU
 *       instructions in all cases; never sleeps, never retries, never calls
 *       into libc functions that can block.
 *     - IO thread (producer for "out", consumer for "in"): may use a normal
 *       blocking pthread_mutex_lock(), but only ever holds it across a
 *       memcpy of <= AH_FRAME_BYTES_MAX bytes -- never while touching the
 *       child's pipes.
 *
 *   IMPORTANT (post-mortem of the original v1/v2 design, which used a
 *   *single-slot* mailbox instead of a ring): a single slot is only safe if
 *   submit_pcm() is called at most once per IO-thread round trip. That
 * assumption held for the offline harnesses (which
 *   drive the bridge from a steady 10 ms timer) but does NOT hold for
 *   media-sink.c's real call pattern described above: two submit_pcm() calls
 *   a few microseconds apart, with no time for the IO thread to have drained
 *   the first one yet, would silently overwrite block N with block N+1 before
 *   the helper ever saw block N -- feeding the stateful/predictive Adaptive
 *   encoder a discontinuous PCM stream (not just an occasional dropped
 *   *output* packet). That is audible as exactly the crackle/desync symptom
 *   this ring buffer fixes. The ring is deep enough (M4_AH_RING_SLOTS_MAX, chosen
 *   with margin over ceil(max realistic quantum / 480)+scheduling jitter) to
 *   absorb any such burst losslessly; if it is ever completely full (helper
 *   genuinely cannot keep up, a distinct and much rarer failure mode),
 *   submit_pcm() rejects the new block (counted, see
 *   m4_ah_bridge_get_stats()) rather than corrupting an in-flight one.
 *
 *   Net effect: the helper's round trip (including any qemu-hexagon jitter)
 *   is fully decoupled from the RT thread. The
 *   price is at least one full block (10 ms) of extra pipeline latency (more
 *   during a burst, self-correcting on the next quiet cycle): encode() for
 *   PCM block N submits block N to the bridge's input ring and, in the same
 *   call, picks up whatever frame is at the head of the output ring -- which
 *   is block N's own frame once the pipeline is caught up, or an older
 *   still-catching-up frame while the helper works through a backlog. Frames
 *   are delivered in the same order they were submitted; none are skipped
 *   just because a later one became ready sooner (there is no "later one
 *   became ready sooner": the helper processes strictly in FIFO order).
 *
 *   If no frame is available yet (still warming up right after ah_bridge
 *   start) or the helper has died, ah_bridge_take_frame() returns 0 and the
 *   caller (codec_encode_adaptive in a2dp-codec-aptx.c) skips sending an
 *   A2DP packet for that interval (silence gap), per the explicit
 *   "silence or skip a frame, but never crash or hang" requirement. It never
 *   fabricates a fake codec frame.
 */

#ifndef SPA_BLUEZ5_M4_ADAPTIVE_BRIDGE_H_
#define SPA_BLUEZ5_M4_ADAPTIVE_BRIDGE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <spa/support/log.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One PCM block: 480 samples/channel, stereo, S16LE = 1920 bytes. This is
 * the aptX Adaptive LL frame period for MOMENTUM 4 (10 ms @ 48 kHz).
 * Fixed, not negotiated: this whole plugin
 * targets one specific headset. */
#define M4_AH_PCM_SAMPLES_PER_BLOCK 480u
#define M4_AH_PCM_CHANNELS 2u
#define M4_AH_PCM_BYTES_PER_SAMPLE 2u /* S16LE */
#define M4_AH_PCM_BLOCK_BYTES \
	(M4_AH_PCM_SAMPLES_PER_BLOCK * M4_AH_PCM_CHANNELS * M4_AH_PCM_BYTES_PER_SAMPLE)
/* Wall-clock duration of one PCM block (480 samples @ 48 kHz = 10 ms exactly).
 * Used to report input-ring trim counts (whole blocks) as milliseconds in the
 * public stats API/JSON file (task: "in_ring_trimmed_ms"). */
#define M4_AH_PCM_BLOCK_MS 10u

/* Upper bound on a single encoded frame we accept from the helper, before
 * any OTA-header stripping. Must match/exceed AH_FRAME_BYTES_MAX in
 * m4-adaptive-protocol.h. HQ frames (656 + 8 B OTA header = 664 B) are the
 * largest we know of ; leave generous room. */
#define M4_AH_MAX_FRAME_BYTES 4096u

#define M4_AH_OTA_HEADER_BYTES 8u

/* Compile-time UPPER BOUND on the PCM-in / frame-out ring buffers' depth --
 * sizes the fixed in_pcm/out_frame/out_len/out_silent arrays in struct
 * m4_ah_bridge below (see the file header for why a ring, not a single-slot
 * mailbox, in the first place). Working capacity comes from config,
 * capped at 32: the ACTUAL, working depth is now a runtime value,
 * `m4_ah_bridge_config.ring_slots` ([adaptive] ring-slots,
 * common/m4-conf-keys.def range 1..32, default M4_AH_RING_SLOTS_DEFAULT
 * below), clamped again in m4_ah_bridge_start() to [1, M4_AH_RING_SLOTS_MAX]
 * regardless of what the config layer already enforced, so this constant is
 * a hard safety ceiling, not just a default. 32 has ample margin over the
 * worst case ever measured on this project (an unusually large graph
 * quantum of 4096 samples only produces ceil(4096/480) = 9 back-to-back
 * encode() calls, see M4_AH_RING_SLOTS_DEFAULT's own comment for the
 * previously-fixed default of 8 that value still matches). */
#define M4_AH_RING_SLOTS_MAX 32u

/* Default working ring depth (== the compile-time constant this replaced,
 * unchanged): covers the realistic range (quantum 512-1024 -> 2-3 calls)
 * with headroom for host scheduling jitter between the RT and IO threads,
 * while staying small (8 * ~2 KiB per ring = tens of KiB, and at most
 * ~80 ms of extra worst-case latency if the helper is genuinely behind,
 * which self-corrects as soon as it catches up -- see
 * m4_ah_bridge_get_stats() to observe this in practice instead of
 * guessing). Used when m4_ah_bridge_config.ring_slots is <= 0 (unset). */
#define M4_AH_RING_SLOTS_DEFAULT 8u

/* "Latency drift in the output ring" : after
 * ANY event that makes the IO thread fall behind and then catch up in a
 * burst (helper round-trip stall, "late", auto-restart), take_frame() used to
 * only ever remove exactly ONE frame per call -- symmetric with
 * submit_pcm()'s one-call-one-block. That symmetry means an extra frame the
 * catch-up burst leaves behind is never removed again by anything: production
 * and consumption both return to a steady 1:1 rate once the burst is over, so
 * whatever backlog existed at that moment just sits there forever (confirmed
 * live: ring_out observed stepping 1 -> 3 -> 4 -> 5 across sessions and never
 * coming back down on its own, each step lining up with a stall/late/restart
 * event). M4_AH_RING_TARGET_FRAMES_DEFAULT is the occupancy BOTH rings are trimmed
 * back down to (see m4_ah_bridge_take_frame()'s and the IO thread's own trim
 * loops in m4-adaptive-bridge.c) -- 1 matches the steady-state baseline
 * observed when nothing is going wrong, i.e. the
 * one-block pipeline latency inherent to this bridge's own design (file
 * header above), not something to chase away, only anything ABOVE it.
 *
 * Trimming is gated purely on how LONG a ring has been above target (a
 * monotonic "over_since" timestamp per ring, see M4_AH_RING_TRIM_SUSTAIN_MS_DEFAULT/
 * M4_AH_RING_TRIM_HARD_CAP_MS_DEFAULT in the .c), never on how far above target it
 * currently is -- there is deliberately no "instant trim once N frames
 * behind" shortcut. media-sink.c's own built-in "encode next packet already,
 * flush later" one-packet lookahead can legitimately push occupancy to 2 (or,
 * for an unusually large graph quantum, several more at once, see
 * M4_AH_RING_SLOTS_MAX above) for a brief moment with zero trimming bugs
 * whatsoever, and that whole burst is expected to drain losslessly via
 * ordinary one-frame-per-call delivery well within the sustain window (see
 * test/ring_burst_test.c, which requires exactly this) -- an instant,
 * depth-only trigger would wrongly treat that legitimate, self-resolving
 * burst the same as the persistent, never-resolving backlog this mechanism
 * actually targets. Exposed here (not just a .c-local #define) so
 * a2dp-codec-aptx.c can gate heartbeat resync on "ring is back at target"
 * without duplicating the constant (see codec_start_encode_adaptive(), and
 * the maintainer note below). */
#define M4_AH_RING_TARGET_FRAMES_DEFAULT 1u

struct m4_ah_bridge;

/* "helper as a user-service": the chosen design
 * (variant 2) is that the qemu-hexagon child normally runs OUTSIDE
 * WirePlumber's process tree entirely, as a socket-activated systemd --user
 * service (momentum4-helper.socket + momentum4-helper@.service), so that
 * WirePlumber's own MemoryDenyWriteExecute=yes hardening ("diagnostics
 * session 4") never has to be relaxed. SPAWN mode (fork+exec+pipe)
 * is kept as an explicit fallback for offline tests
 * (resilience_test.c, unit_test.c) and for anyone who cannot/does not want to
 * install the systemd units -- selectable per m4-adaptive-config.h
 * ("helper-mode = spawn | socket", default socket) or
 * $M4_ADAPTIVE_HELPER_MODE. Both modes speak the exact same wire protocol
 * (m4-adaptive-protocol.h) and present the exact same RT-safety contract to
 * codec_encode_adaptive() -- everything below this comment in this header is
 * unchanged between the two modes except where noted. */
enum m4_ah_helper_mode {
	M4_AH_HELPER_MODE_SPAWN = 0,  /* fork+exec qemu_path/loader_path/capi_path, two pipes */
	M4_AH_HELPER_MODE_SOCKET = 1, /* connect(AF_UNIX, socket_path), one duplex fd */
};

struct m4_ah_bridge_config {
	enum m4_ah_helper_mode mode; /* default (zero-init) is SPAWN for source
	                              * compatibility with existing callers/tests
	                              * that memset(&cfg, 0, ...) and only set
	                              * qemu/loader/capi_path; a2dp-codec-aptx.c
	                              * explicitly sets SOCKET as its own default
	                              * per m4-adaptive-config.h. */

	/* MODE == SPAWN only. */
	const char *qemu_path;   /* qemu-hexagon(-static) binary */
	const char *loader_path; /* hx_adaptive_loader ELF */
	const char *capi_path;   /* capi_v2_aptX_CLHDADV_Encoder.so */

	/* MODE == SOCKET only: AF_UNIX path, e.g.
	 * "$XDG_RUNTIME_DIR/momentum4/helper.sock" (matches
	 * ListenStream=%t/momentum4/helper.sock in
	 * systemd/momentum4-helper.socket.in). sun_path length limit
	 * (typically 108 bytes) applies, checked at connect time. */
	const char *socket_path;
	/* Non-blocking connect() timeout. 0 => built-in default (see
	 * M4_AH_CONNECT_TIMEOUT_MS_DEFAULT in m4-adaptive-bridge.c). Deliberately
	 * separate from startup_timeout_ms below: connect() itself is normally
	 * near-instant once the .socket unit is listening (systemd queues the
	 * connection immediately even before the service instance has finished
	 * starting -- that is the entire point of socket activation), the
	 * handshake read is what actually waits out cold-start (qemu boot). */
	uint32_t connect_timeout_ms;

	bool strip_ota_header;   /* true: RTP variant A (default), false: variant B */
	uint32_t helper_timeout_ms; /* IO-thread-side round trip timeout */
	uint32_t startup_timeout_ms; /* 0 => use the built-in default (5000 ms) */

	/* "restart the helper on a silent failure instead of a crash".
	 * Default true. If the child dies (or times out) AFTER the initial
	 * successful start, the IO thread keeps retrying spawn+handshake with
	 * exponential backoff (see m4-adaptive-bridge.c) instead of giving up
	 * forever. submit_pcm()/take_frame() are safe no-ops the whole time
	 * (silence, per the "never crash or hang" requirement); is_alive()
	 * reports true again once a restart succeeds. Does NOT affect the very
	 * first m4_ah_bridge_start() call: that one still fails closed exactly
	 * as before (codec_init_adaptive() falls back to aptX HD) if the helper
	 * cannot come up at all -- auto-restart only kicks in for a helper that
	 * was working and later died. */
	bool auto_restart;

	/* "RT priority for the helper (chrt without sudo, rlimit),
	 * taskset." MODE == SPAWN ONLY: applied to the qemu-hexagon child (not to
	 * WirePlumber's own threads) right after fork(), before exec. 0 = do not
	 * touch scheduling (plain SCHED_OTHER). > 0 = attempt
	 * sched_setscheduler(SCHED_FIFO, {rt_priority}); best-effort, EPERM is
	 * silently tolerated (falls back to SCHED_OTHER) -- this requires either
	 * running as root or an RLIMIT_RTPRIO soft limit >= rt_priority, which a
	 * systemd user unit can grant via `LimitRTPRIO=`, no sudo/capabilities
	 * needed at runtime. Also best-effort mlockall(MCL_CURRENT|MCL_FUTURE)
	 * for the child when rt_priority > 0 (needs RLIMIT_MEMLOCK, same
	 * story). MODE == SOCKET: ignored by the bridge itself -- the equivalent
	 * knobs are `CPUSchedulingPolicy=`/`CPUSchedulingPriority=`/`Nice=`/
	 * `LimitRTPRIO=` in systemd/momentum4-helper@.service.in (or
	 * `momentum4 helper-exec`'s own sched_setscheduler() fallback for SPAWN
	 * mode), since in socket mode the bridge never
	 * forks the process that needs them. */
	int rt_priority;

	/* "taskset": comma-separated CPU indices (e.g. "10" or "8,9,10") the
	 * child is pinned to via sched_setaffinity(), or NULL/empty for no
	 * pinning. Best-effort; malformed input is ignored (no pinning). MODE ==
	 * SOCKET: ignored, same reasoning as rt_priority above (use
	 * `CPUAffinity=` in the .service unit instead). */
	const char *cpu_affinity_list;

	/* [helper] debug-log: resolved by the CALLER (a2dp-codec-aptx.c, from
	 * momentum4.conf) and forwarded here rather than read directly by
	 * child_apply_rt_settings() in m4-adaptive-bridge.c, because that
	 * function runs AFTER fork(), BEFORE exec() -- it may only call
	 * async-signal-safe functions, and the config loader (fopen/malloc) is
	 * not one of them. MODE == SOCKET: unused (the equivalent knob for the
	 * systemd-managed service instance is `[helper] debug-log` read by
	 * `momentum4 helper-exec` itself, a separate process). */
	bool debug_log;

	/* "RT for the bridge's IO thread" (see m4-adaptive-config.h's "rt-priority" key
	 * for the full rationale). Unlike rt_priority/cpu_affinity_list above
	 * (SPAWN-mode-only, applied to the qemu-hexagon CHILD), this applies to
	 * THIS PROCESS's own IO thread (io_thread_main() in
	 * m4-adaptive-bridge.c, i.e. inside WirePlumber) in BOTH modes --
	 * SPAWN and SOCKET alike, since the IO thread's job (bounded read()/
	 * write() round trips + ring bookkeeping) is identical either way.
	 *
	 * PipeWire's own data-loop.0 gets its RT priority through
	 * spa_thread_utils_acquire_rt() (module-rt/rtkit), via a
	 * struct spa_thread_utils obtained from pw_context_get_object(context,
	 * SPA_TYPE_INTERFACE_ThreadUtils) -- a struct pw_context*-scoped API.
	 * confirmed by reading the installed PipeWire 1.4.2 source
	 * (src/pipewire/context.c, src/pipewire/thread.c, src/modules/
	 * module-rt.c) that this codec plugin has NO path to that same
	 * interface: it is loaded as a bare `struct spa_handle` (media-codecs.c
	 * impl_init(factory, handle, info, support, n_support)), and the
	 * `support`/`n_support` array every spa_handle_factory receives via
	 * pw_context_load_spa_handle()/context_get_support() is a small FIXED
	 * set (System, Loop, LoopUtils, PluginLoader, DBus, plus DataSystem/
	 * DataLoop of whichever pw_data_loop was acquired) built once at
	 * pw_context_new() time -- it never includes objects registered later
	 * via pw_context_set_object() (Core, ThreadUtils, ...), which live in a
	 * completely separate `context->objects` array reachable ONLY through
	 * pw_context_get_object(context, type), a struct pw_context* API this
	 * plugin is never handed (struct media_codec::init()'s signature has no
	 * such parameter, nor does the codec .so's own spa_handle_factory
	 * impl_init() forward one). `pw_thread_utils_set()`, the old
	 * process-wide-singleton escape hatch, is now a documented no-op
	 * ("deprecated and does nothing anymore", thread.c) in this version, so
	 * there is also no viable process-global fallback. In short: the
	 * "how does the codec get SPA_TYPE_INTERFACE_ThreadUtils" question this
	 * field might suggest has the answer "it structurally cannot, in
	 * PipeWire 1.4.2's bluez5 SPA plugin architecture" -- this is not a bug
	 * in our plugin, it is how upstream separates "context-level" C API
	 * objects from the SPA `support` vector by design.
	 *
	 * So this field's ONLY implementation is the sanctioned fallback:
	 * pthread_setschedparam(io_thread, SCHED_FIFO, {io_thread_rt_priority})
	 * called once, right after pthread_create() in m4_ah_bridge_start().
	 * 0 (default) = do not touch scheduling (leave whatever the calling
	 * thread's default is, normally SCHED_OTHER). > 0 = attempt SCHED_FIFO
	 * at that priority; EPERM (no RLIMIT_RTPRIO on wireplumber.service, or
	 * running outside systemd entirely) is silently tolerated -- falls back
	 * to SCHED_OTHER, exactly like rt_priority's child-side comment above,
	 * and for the identical reason (this must never be why the bridge fails
	 * to start). The REAL, currently-effective outcome (not just whether
	 * pthread_setschedparam() returned 0) is queried back via
	 * pthread_getschedparam() and exposed as "bridge_rt_policy"/
	 * "bridge_rt_prio" in m4_ah_bridge_stats/the JSON stats file, so an
	 * operator can see what actually happened without grepping the log. */
	int io_thread_rt_priority;

	/* Live metrics file (task: "metrics ... $XDG_RUNTIME_DIR/momentum4/
	 * adaptive-stats.json"). Written periodically, ONLY by the IO thread
	 * (never the RT thread -- see m4-adaptive-bridge.c's maybe_write_stats(),
	 * called from io_thread_main()/io_thread_steady_state()'s own loops),
	 * using the ordinary blocking libc I/O the IO thread already uses for
	 * everything else. NULL (default, zero-init) disables the JSON file
	 * entirely -- zero extra I/O, one NULL check per IO-thread loop
	 * iteration, no behaviour change for any existing caller/test that does
	 * not set this. Written atomically: "<stats_path>.tmp" then rename() to
	 * stats_path, so a reader (`momentum4 stats`) never observes a
	 * half-written file. */
	const char *stats_path;
	/* Optional short rolling history, one compact JSON object per line
	 * (JSON Lines), appended every stats_interval_ms and periodically
	 * trimmed to the last M4_AH_STATS_HISTORY_MAX_LINES_DEFAULT lines (see
	 * m4-adaptive-bridge.c). NULL (default) => no history file even if
	 * stats_path is set (the single current-snapshot file above is still
	 * written). */
	const char *stats_history_path;
	/* 0 => built-in default (10000 ms, matches the "once every 10s" requirement and
	 * the pre-existing spa_log_info-based stats/10s window this replaces/
	 * complements, see a2dp-codec-aptx.c). */
	uint32_t stats_interval_ms;

	/* Sourced from common/m4-conf-keys.def [adaptive]: every field below
	 * follows the SAME "<= 0 => built-in default" convention already used
	 * above by connect_timeout_ms/startup_timeout_ms/stats_interval_ms --
	 * every existing caller/test that memset()s this whole struct to 0 and
	 * never sets these gets EXACTLY the same behaviour as before these
	 * fields existed (see m4_ah_bridge_start()'s own comment on this). */
	bool fade_in_enabled;         /* [adaptive] fade-in */
	int32_t fade_in_frames;       /* [adaptive] fade-in-frames */
	int32_t ring_slots;           /* [adaptive] ring-slots, clamped to
	                                * [1, M4_AH_RING_SLOTS_MAX] regardless of
	                                * what is passed here -- see
	                                * m4_ah_bridge_start(). */
	int32_t ring_target_frames;   /* [adaptive] ring-target-frames */
	double ring_trim_sustain_ms;  /* [adaptive] ring-trim-sustain-ms */
	double ring_trim_hard_cap_ms; /* [adaptive] ring-trim-hard-cap-ms */
	int32_t ring_quiet_amp;       /* [adaptive] ring-quiet-amp */
	int32_t idle_poll_ms;         /* [adaptive] idle-poll-ms */
	int32_t restart_backoff_min_ms; /* [adaptive] helper-restart-backoff-min-ms */
	int32_t restart_backoff_max_ms; /* [adaptive] helper-restart-backoff-max-ms */
	int32_t stats_history_lines;    /* [stats] history-lines */
	int32_t stats_history_rotate_every; /* [stats] history-rotate-every */
};

/* Default cap on stats_history_path's line count (docs: "short
 * rotation"). 360 lines * 10 s = one hour of history at the default
 * interval. Exposed here (not just as a .c-local #define) so
 * callers and tests can reason about/verify the bound. */
#define M4_AH_STATS_HISTORY_MAX_LINES_DEFAULT 360u

/* Cheap, non-blocking availability probe ("fill_caps
 * pre-check"): three access() calls, no fork/exec, no CAPI/ENC_INIT
 * handshake. Intended to be called from struct media_codec::fill_caps(),
 * which runs synchronously inside BlueZ's org.freedesktop.DBus.ObjectManager
 * GetManagedObjects handler (WirePlumber's D-Bus/main thread) -- a full
 * m4_ah_bridge_start() probe there would block that thread for up to
 * M4_AH_STARTUP_TIMEOUT_MS_DEFAULT (qemu-hexagon boot + CAPI init), which is not
 * acceptable on the continuity-of-sound path. This catches the two most
 * likely deployment failures ("no Qualcomm module" =
 * capi_path missing/unreadable, "cannot start qemu" = qemu_path
 * missing/not executable) without that risk. It does NOT catch a present
 * but corrupt/incompatible CAPI module (that still only fails later, at
 * codec_init_adaptive() time -- a full switch to
 * another codec is not possible at that point once BlueZ has already
 * negotiated aptx_adaptive with the peer).
 *
 * Returns true iff qemu_path is executable and loader_path/capi_path are
 * readable. Any NULL argument returns false. */
bool m4_ah_bridge_paths_available(const char *qemu_path, const char *loader_path,
		const char *capi_path);

/* MODE == SOCKET equivalent of m4_ah_bridge_paths_available(): a single
 * stat() call, no connect(), no fork/exec, no handshake. Checks that
 * `socket_path` exists AND is actually a socket (S_ISSOCK) -- both are true
 * the moment `systemctl --user start momentum4-helper.socket` (or
 * `enable --now`, or boot-time user-session activation) has run, regardless
 * of whether any momentum4-helper@N.service instance is currently
 * connected/running, which is exactly why this is safe to call from
 * fill_caps() unconditionally: systemd creates and binds the listening
 * socket file as part of activating the .socket unit itself, independent of
 * (and generally much cheaper/faster than) spawning the first service
 * instance. Does NOT verify anything is actually listening/accepting on it
 * correctly (a socket file can exist while the .socket unit is stopped, if
 * e.g. a previous run crashed without cleanup) -- same class of residual gap
 * as m4_ah_bridge_paths_available() not catching a corrupt-but-present CAPI
 * module, see that function's caller in a2dp-codec-aptx.c for the accepted
 * trade-off. Returns false if socket_path is NULL/empty. */
bool m4_ah_bridge_socket_available(const char *socket_path);

/*
 * MODE == SPAWN: forks+execs qemu-hexagon <loader> <capi.so> (two pipes).
 * MODE == SOCKET: non-blocking connect(AF_UNIX, socket_path) within
 * connect_timeout_ms, then the identical startup handshake read over the one
 * resulting fd -- systemd (momentum4-helper.socket) does the
 * fork+exec+pipe-equivalent work on the other end, entirely outside this
 * process. Either way, never called from the RT thread (only from struct
 * media_codec::init(), which runs on the WirePlumber main/config thread when
 * a transport is set up).
 *
 * Returns NULL on any failure (missing binaries / socket not
 * present-and-a-socket, fork/exec or connect() failure, no startup reply
 * from the other end within helper_timeout_ms / startup_timeout_ms, remote
 * reported CAPI/ENC_INIT failure). Never partially succeeds: either a fully
 * usable bridge or NULL, so the caller can decide whether to fail
 * codec_init() outright (current choice, see a2dp-codec-aptx.c) or degrade
 * to always-skip.
 */
struct m4_ah_bridge *m4_ah_bridge_start(struct spa_log *log,
		const struct m4_ah_bridge_config *cfg);

/* Stops the IO thread, closes the pipes (which asks the qemu-hexagon guest
 * to exit cleanly via EOF, see hexagon/main.c), joins the thread, and as a
 * safety net SIGKILLs + reaps the child if it did not exit within a short
 * grace period. Safe to call at any time, including after the helper has
 * already died on its own. Not RT-safe (blocks up to ~helper_timeout_ms);
 * only called from media_codec::deinit(). */
void m4_ah_bridge_stop(struct m4_ah_bridge *b);

/* RT-safe (bounded to a trylock + memcpy(<=1920B) + one eventfd write, no
 * blocking syscalls, no unbounded loops). Enqueues one PCM block onto the
 * bridge's input ring (M4_AH_RING_SLOTS_MAX deep, FIFO) and wakes the IO thread.
 * `bytes` MUST equal M4_AH_PCM_BLOCK_BYTES. Safe, and expected, to be called
 * more than once per graph cycle (see file header) -- each call enqueues a
 * distinct block, none are overwritten as long as the ring is not full.
 *
 * Returns false if the block was NOT accepted (input ring mutex contended by
 * the IO thread this instant, the ring is completely full -- i.e. the helper
 * is genuinely behind by M4_AH_RING_SLOTS_MAX blocks, not just a transient
 * within-cycle burst -- or the bridge has been marked dead). This is not an
 * error the caller needs to handle specially beyond dropping that one block
 * of audio input to the helper -- see file header. Callers that want to
 * report/diagnose this should count `false` returns themselves; aggregate
 * ring-level counters are also available via m4_ah_bridge_get_stats(). */
bool m4_ah_bridge_submit_pcm(struct m4_ah_bridge *b, const void *pcm, size_t bytes);

/* RT-safe (bounded to a trylock + memcpy(<=M4_AH_MAX_FRAME_BYTES)). If the
 * output ring is non-empty, dequeues its oldest completed frame (FIFO -- the
 * same order blocks were submitted in) into `dst` (caller-provided buffer of
 * at least dst_cap bytes) and returns its length in bytes (already with the
 * OTA header stripped/kept per cfg.strip_ota_header). Returns 0 if no frame
 * is ready (still warming up, output ring empty or contended, or the frame
 * would not fit in dst_cap -- the latter should never happen in practice
 * given dst_cap is sized from the A2DP MTU). Each completed frame is
 * delivered at most once. */
size_t m4_ah_bridge_take_frame(struct m4_ah_bridge *b, uint8_t *dst, size_t dst_cap);

/* Diagnostics only (task: "counters ... ring fill level ... once every
 * 10s"). RT-safe: bounded to two trylocks (skips whichever ring is
 * momentarily contended -- fields for that ring are then left at their
 * previous value in *out, which the caller should treat as "unknown this
 * call" rather than "zero"; harmless for a once-per-10s log). Never blocks,
 * never fails outright -- if `b` is NULL, *out is zeroed and the ring
 * occupancy fields report 0. */
struct m4_ah_bridge_stats {
	/* Monotonically increasing life-of-bridge counters (atomic, always
	 * fresh regardless of lock contention). */
	uint32_t submitted;           /* submit_pcm() calls that enqueued successfully */
	uint32_t input_ring_full_drops; /* submit_pcm() calls rejected: ring full */
	uint32_t frames_produced;     /* frames the IO thread finished encoding */
	uint32_t output_starved;      /* take_frame() calls that found the output ring empty (while alive) */
	uint32_t output_ring_full_drops; /* IO thread had to drop a finished frame: output ring full (should stay 0 in practice) */
	/* "Latency drift in the output ring" (see
	 * M4_AH_RING_TARGET_FRAMES_DEFAULT above): lifetime counts of frames/PCM
	 * deliberately discarded by the ring-trim logic to bring occupancy back
	 * down to target, as opposed to output_ring_full_drops/
	 * input_ring_full_drops above (which are the OLD, much rarer "ring is
	 * completely full at all 8 slots" failure -- unaffected by this). */
	uint32_t output_ring_trimmed_frames; /* already-encoded frames trimmed from the output ring, never sent to M4 */
	uint32_t input_ring_trimmed_ms;      /* PCM milliseconds trimmed from the input ring before ever being encoded */
	/* High-water mark of ring occupancy since the last JSON stats window
	 * reset (m4_ah_bridge_config.stats_interval_ms), or since bridge start if
	 * JSON stats are disabled (stats_path == NULL): read-only snapshot here,
	 * never reset by this call (see m4-adaptive-bridge.c's maybe_write_stats()
	 * for the actual once-per-window read-and-reset used by the JSON file). */
	uint32_t out_ring_max_window;
	uint32_t in_ring_max_window;
	/* Point-in-time ring occupancy (best-effort, see trylock note above). */
	unsigned in_ring_count;
	unsigned out_ring_count;

	/* "RT for the bridge's IO thread": the IO thread's ACTUALLY-effective scheduling
	 * policy/priority, queried once via pthread_getschedparam() right after
	 * m4_ah_bridge_start() attempted to apply cfg.io_thread_rt_priority
	 * (see that field's doc comment) -- never re-queried afterward (the
	 * policy does not change again for the life of the bridge). policy is
	 * a raw `SCHED_*` value from <sched.h> (SCHED_OTHER == 0, SCHED_FIFO ==
	 * 1 on Linux) so callers that do not want to #include <sched.h> just to
	 * read this struct can still print/compare it numerically; the JSON
	 * stats file renders it as the human string "fifo"/"other"/"rr"/
	 * "unknown". Both fields are 0 (SCHED_OTHER, priority 0) if `b` is NULL
	 * or the bridge never started an IO thread. */
	int bridge_rt_policy;
	int bridge_rt_prio;
};
void m4_ah_bridge_get_stats(struct m4_ah_bridge *b, struct m4_ah_bridge_stats *out);

/* RT-safe (single atomic_fetch_add each, unconditional, no branching beyond
 * a NULL check on `b`). Called from codec_encode_adaptive() in
 * a2dp-codec-aptx.c at the exact points those events are already detected
 * for the pre-existing per-10s spa_log_info diagnostic (see
 * adaptive_maybe_log_stats() there) -- these calls exist ONLY so the (non-RT)
 * IO thread can fold the same counts into the periodic JSON stats file
 * (m4_ah_bridge_config.stats_path, see m4-adaptive-bridge.c's
 * write_stats_json()) without the RT thread ever touching a FILE handle,
 * mutex, or the filesystem itself. Safe to call even when stats_path is NULL (the counters
 * are simply never read by anyone in that case -- negligible, an atomic
 * increment either way). */
void m4_ah_bridge_note_encode_call(struct m4_ah_bridge *b);
void m4_ah_bridge_note_late(struct m4_ah_bridge *b);
void m4_ah_bridge_note_stall(struct m4_ah_bridge *b);
void m4_ah_bridge_note_heartbeat_event(struct m4_ah_bridge *b);

/* NOT RT-safe -- snprintf()s into fixed internal buffers, no allocation, no
 * I/O, but not meant to be called from codec_encode_adaptive(). Call exactly
 * once, from codec_init_adaptive() right after a successful
 * m4_ah_bridge_start() returns (same thread init() itself runs on --
 * WirePlumber's main/config thread -- strictly before that same thread ever
 * lets the RT data thread start calling encode(), so there is no race with
 * the IO thread's periodic reads of this metadata either: by the time any
 * frame/JSON write can happen, this has already been called). Purely
 * descriptive metadata folded into the JSON stats file; never read by any
 * encode-path decision. mode_str is copied (safe to pass a stack/temporary
 * string); NULL is treated as "?". */
void m4_ah_bridge_set_meta(struct m4_ah_bridge *b, const char *mode_str,
		int32_t latency_adjust_ms, bool heartbeat_enabled,
		uint32_t resync_period_min);

/* RT-safe (single atomic load). True until the IO thread observes a fatal
 * error talking to the child (short read/write, timeout, or the child
 * exiting/crashing) or m4_ah_bridge_stop() is called. Purely informational
 * for the caller (e.g. to log a one-time warning); callers must keep
 * calling submit_pcm/take_frame normally regardless -- both are safe no-ops
 * once the bridge is dead. */
bool m4_ah_bridge_is_alive(const struct m4_ah_bridge *b);

/* Test/debug only -- not RT-safe, not used by codec_encode_adaptive. Lets
 * module/test/resilience_test.c signal/kill the helper
 * process directly to exercise the "helper dies mid-stream" and "helper
 * hangs" fault scenarios without depending on real qemu-hexagon timing.
 * MODE == SOCKET: always returns -1 (there is no local child process to
 * signal -- the systemd-managed service instance is a sibling process, not
 * a child of this one; socket-mode fault-injection tests instead kill/hang
 * the *server* side directly, see test/socket_bridge_test.c). */
#include <sys/types.h>
pid_t m4_ah_bridge_test_child_pid(const struct m4_ah_bridge *b);

/* RT-safe (single atomic load). Number of times the IO thread has
 * successfully respawned the helper after it died (0 if it never died, or
 * cfg.auto_restart was false). For tests/diagnostics only. */
uint32_t m4_ah_bridge_restart_count(const struct m4_ah_bridge *b);

#ifdef __cplusplus
}
#endif

#endif /* SPA_BLUEZ5_M4_ADAPTIVE_BRIDGE_H_ */
