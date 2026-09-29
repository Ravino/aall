/* momentum4: aptX Adaptive SPA codec <-> qemu-hexagon helper bridge.
 * SPDX-License-Identifier: MIT
 * See m4-adaptive-bridge.h for the design rationale (RT-safety contract).
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "m4-adaptive-bridge.h"
#include "m4-adaptive-protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <spa/utils/string.h>

/* Startup (fork+exec+CAPI init+ENC_INIT) can legitimately take longer than
 * one steady-state block round trip -- ELF load + relocation + module
 * init() under TCG. Generous fixed budget, independent of
 * cfg.helper_timeout_ms. */
#define M4_AH_STARTUP_TIMEOUT_MS_DEFAULT 5000
/* How long the IO thread's poll() waits for a wake-up before re-checking
 * the stop flag. Only affects worst-case shutdown latency, not steady
 * state (the eventfd wakes it immediately when there is real work). */
#define M4_AH_IDLE_POLL_MS_DEFAULT 200
/* Grace period for the child to exit after we close its stdin (EOF), before
 * we escalate to SIGKILL, in m4_ah_bridge_stop(). */
#define M4_AH_STOP_GRACE_MS 300
/* "reconnect-grace" + task option (c): 5 ms at 48 kHz. See the
 * `fade_in_frames_left` field comment for the full rationale. */
#define M4_AH_FADE_IN_FRAMES_DEFAULT 240u

/* Auto-restart backoff. Deliberately conservative: a helper
 * that keeps dying immediately (e.g. missing binary that got deleted
 * mid-session) should not spin the IO thread hot re-forking qemu-hexagon;
 * doubling from 500 ms up to a 10 s cap gives up to a handful of quick
 * retries (covers a transient fork/exec hiccup) and then settles into a slow
 * poll that will not meaningfully compete for CPU with the RT audio path. */
#define M4_AH_RESTART_BACKOFF_MIN_MS_DEFAULT 500
#define M4_AH_RESTART_BACKOFF_MAX_MS_DEFAULT 10000

/* MODE == SOCKET default non-blocking connect() timeout (m4-adaptive-bridge.h
 * cfg.connect_timeout_ms == 0 means "use this"). Deliberately short relative
 * to M4_AH_STARTUP_TIMEOUT_MS_DEFAULT: once momentum4-helper.socket is active,
 * accept()-queueing a new connection is a kernel-level operation that
 * completes in well under a millisecond even if no service instance has
 * finished (or even started) booting yet -- that is the whole point of
 * socket activation . A connect() that does not complete
 * quickly almost certainly means the .socket unit itself is not
 * listening/does not exist, which is a different, more fundamental failure
 * than "qemu is still booting" and should surface faster than the 5 s
 * startup budget. */
#define M4_AH_CONNECT_TIMEOUT_MS_DEFAULT 1000

/* "Latency drift in the output ring": see M4_AH_RING_TARGET_FRAMES_DEFAULT/
 * M4_AH_RING_MAX_TOLERATED_FRAMES in m4-adaptive-bridge.h for the occupancy
 * thresholds. These two control the TIMING of the trim once occupancy is
 * between target+1 and the tolerated ceiling (above the ceiling, trimming is
 * immediate regardless of either of these -- see the trim loops):
 *  - a brief burst up to M4_AH_RING_MAX_TOLERATED_FRAMES is given this long
 *    to resolve itself on its own (e.g. media-sink.c's own one-packet
 *    lookahead settling back to 1) before the trimmer starts removing
 *    anything from it;
 *  - once that grace period has elapsed, only slots flagged "silent" (see
 *    pcm_block_is_silent() below) are trimmed, UNLESS a given slot's own age
 *    reaches the hard cap, in which case it is trimmed regardless of
 *    loudness -- the explicit requirement of "not a single extra frame longer than ~1s"
 *    bound. */
#define M4_AH_RING_TRIM_SUSTAIN_MS_DEFAULT 200.0
#define M4_AH_RING_TRIM_HARD_CAP_MS_DEFAULT 1000.0
/* Peak abs S16 amplitude below which a PCM block counts as "silent" for
 * ring-trim purposes -- same default as m4-adaptive-heartbeat.h's
 * quiet_amp . Not
 * wired to the heartbeat's own (possibly live-tunable, if that is ever added)
 * value on purpose: these are two independent consumers of "is this block
 * quiet", each with its own correctness requirement, and this one only ever
 * needs to be conservative, not exactly match the heartbeat's gate. */
#define M4_AH_RING_QUIET_AMP_DEFAULT 103

/* Moved up from just above rotate_stats_history()/append_stats_history()
 * (still where it is documented/used) so m4_ah_bridge_start() above those
 * two functions can also reference it when resolving cfg->stats_history_rotate_every. */
#define M4_AH_STATS_HISTORY_ROTATE_EVERY_DEFAULT 30u /* ~5 min at the default 10 s interval */

struct m4_ah_bridge {
	struct spa_log *log;
	struct m4_ah_bridge_config cfg;
	char *qemu_path, *loader_path, *capi_path; /* MODE == SPAWN only */
	char *socket_path;                          /* MODE == SOCKET only */

	/* Resolved once in m4_ah_bridge_start() from cfg (see that
	 * function's own comment on the "<= 0 => default" convention shared with
	 * connect_timeout_ms/startup_timeout_ms above) -- never touched again
	 * afterward, so every read site below (RT and IO thread alike) is a
	 * plain, lock-free field read, exactly as cheap as the compile-time
	 * macros these replace. `ring_slots` <= M4_AH_RING_SLOTS_MAX always
	 * (clamped at assignment time), so it is always a safe modulus/bound for
	 * the fixed-size in_pcm/out_frame/out_len/out_silent arrays below. */
	unsigned ring_slots;
	unsigned ring_target_frames;
	double ring_trim_sustain_ms;
	double ring_trim_hard_cap_ms;
	int32_t ring_quiet_amp;
	uint32_t idle_poll_ms;
	unsigned restart_backoff_min_ms;
	unsigned restart_backoff_max_ms;
	unsigned stats_history_lines;
	unsigned stats_history_rotate_every;

	pid_t child_pid;
	/* MODE == SPAWN: child_in != child_out, two pipe fds, child_pid > 0.
	 * MODE == SOCKET: child_in == child_out (one connected AF_UNIX
	 * SOCK_STREAM fd), child_pid == -1 always (no local process to reap),
	 * child_err == -1 always (no local stderr to capture -- diagnostics for
	 * the server side live in `journalctl --user -u
	 * momentum4-helper@*.service`, see log_handshake_failure()/
	 * mark_dead()'s socket-mode branch). Every read()/write() call site in
	 * this file already treats child_in/child_out as opaque fds and does not
	 * care whether they are equal, so no other code needed to change for
	 * this. */
	int child_in;  /* write PCM blocks here (parent -> child stdin, or the socket) */
	int child_out; /* read replies here (child stdout -> parent, or the socket) */
	int child_err; /* child stderr, always piped (non-blocking) in SPAWN mode,
	                * always -1 in SOCKET mode. See
	                * drain_child_stderr_nonblocking() -- diagnostics only,
	                * never on the RT path. */
	int wake_efd;

	/* First bytes of the current/last child's stderr, captured so a
	 * handshake failure or crash is diagnosable from the journal alone
	 * without re-running with M4_ADAPTIVE_HELPER_DEBUG_LOG set (task:
	 * "so the next error is immediately visible"). Reset at the start of
	 * every spawn_child_process()/connect_socket() call. Kept small and first-N-bytes (not a ring
	 * buffer) on purpose: the interesting content is always the first
	 * lines printed before the child dies, not whatever it might spew
	 * after that. */
	char err_buf[2048];
	size_t err_len;

	pthread_t io_thread;
	bool io_thread_started;

	/* "RT for the bridge's IO thread": set once by apply_io_thread_rt(), called from
	 * m4_ah_bridge_start() right after pthread_create() succeeds -- never
	 * touched again afterward (io_thread_main() itself never re-applies
	 * scheduling, including across auto-restart cycles: those
	 * reuse the SAME io_thread, only the child/socket connection is
	 * re-established). Atomic only so a concurrent maybe_write_stats() call
	 * (which can race the very first write against apply_io_thread_rt(),
	 * both happening around bridge start) never reads a torn/half-set
	 * value; there is exactly one writer, ever. */
	atomic_int io_rt_policy;
	atomic_int io_rt_prio;

	atomic_bool stop_requested;
	atomic_bool alive;
	atomic_uint restart_count; /* auto-restart diagnostics */

	/* Diagnostics (task: counters for a periodic "once every 10s" log). Atomic:
	 * updated from both the RT thread (submitted/input_ring_full_drops) and
	 * the IO thread (frames_produced/output_ring_full_drops), read from the
	 * RT thread via m4_ah_bridge_get_stats(). */
	atomic_uint stat_submitted;
	atomic_uint stat_input_ring_full_drops;
	atomic_uint stat_input_trylock_drops; /* task: "metrics" -- submit_pcm()
		* rejected because the IO thread briefly held in_lock, distinct from
		* a genuinely full ring; was previously not counted anywhere at all. */
	atomic_uint stat_frames_produced;
	atomic_uint stat_output_starved;
	atomic_uint stat_output_ring_full_drops;

	/* Codec-side counters (task: "metrics", forwarded via
	 * m4_ah_bridge_note_*() from codec_encode_adaptive() in
	 * a2dp-codec-aptx.c) folded into the periodic JSON stats file alongside
	 * the bridge-native counters above. Atomic for the same reason as the
	 * ones above: written from the RT thread, read from the IO thread. */
	atomic_uint stat_encode_calls;
	atomic_uint stat_late;
	atomic_uint stat_stall;
	atomic_uint stat_heartbeat_events;

	/* Metadata for the JSON stats file only (m4_ah_bridge_set_meta()); never
	 * read by any encode-path decision. Plain fields, not atomic: set once
	 * by codec_init_adaptive() strictly before the RT thread/IO thread can
	 * start reading them, see m4_ah_bridge_set_meta()'s doc comment. */
	char meta_mode[16];
	int32_t meta_latency_adjust_ms;
	bool meta_heartbeat_enabled;
	uint32_t meta_resync_period_min;

	/* JSON stats file. NULL (both) => disabled, see
	 * m4_ah_bridge_config. IO-thread-private after m4_ah_bridge_start(): only
	 * ever touched from the IO thread's own loops (maybe_write_stats()), so
	 * no lock is needed for last_stats_write_ms/stats_history_writes despite
	 * looking like "shared" state -- the RT thread never reads or writes any
	 * of these fields. */
	char *stats_path;
	char *stats_history_path;
	uint32_t stats_interval_ms;
	double start_ms;
	double last_stats_write_ms;
	unsigned stats_history_writes; /* counts history appends, for periodic rotation */

	/* Input ring: RT thread (producer, trylock) -> IO thread (consumer,
	 * blocking lock, brief hold). FIFO, M4_AH_RING_SLOTS_MAX deep -- see the file
	 * header for why a single slot was not safe against media-sink.c's
	 * within-cycle burst of encode() calls. */
	pthread_mutex_t in_lock;
	uint8_t in_pcm[M4_AH_RING_SLOTS_MAX][M4_AH_PCM_BLOCK_BYTES];
	unsigned in_head, in_tail, in_count;
	/* "Latency drift": 0 => in_count is currently <= M4_AH_RING_TARGET_FRAMES_DEFAULT
	 * (healthy). > 0 => monotonic ms timestamp when in_count first exceeded
	 * target; the IO thread's trim loop uses this to implement
	 * M4_AH_RING_TRIM_SUSTAIN_MS_DEFAULT ("give a brief burst a chance to resolve on
	 * its own before trimming anything"). Touched only under in_lock, by the
	 * IO thread alone (submit_pcm() itself never reads or writes it). */
	double in_over_since_ms;

	/* "reconnect-grace" + task option (c): linear input gain
	 * ramp (0 -> 1) applied to the first M4_AH_FADE_IN_SAMPLES PCM frames
	 * ever submitted to a freshly-started bridge (set in m4_ah_bridge_start(),
	 * counted down and applied in m4_ah_bridge_submit_pcm() under in_lock --
	 * single-writer, no atomics needed). Together with
	 * APTX_AD_START_MUTE_PKTS=0 (systemd/momentum4-helper@.service.in
	 * and build_child_envp_with_mute_default() for SPAWN mode) this replaces
	 * the encoder's own 200 ms hard mute with a much shorter, inaudible
	 * fade-in instead of a dead-silence gap, for every cold ENC_INIT that
	 * still happens after adaptive_park_for_reuse() (a2dp-codec-aptx.c) --
	 * M4's very first connection, a helper crash-restart, a WirePlumber
	 * restart. A bridge that is RECLAIMED from the park slot (no ENC_INIT
	 * happened) does NOT get a fresh fade-in -- see m4_ah_bridge_start() is
	 * simply never called again in that path, so this field is left at 0
	 * from the ORIGINAL m4_ah_bridge_start() call, i.e. no fade at all on
	 * reclaim, which is correct: there is no cold-start transient to mask. */
	uint32_t fade_in_frames_left;
	uint32_t fade_in_frames_total; /* the value fade_in_frames_left started at
					 * (0 if fade-in is disabled) -- needed
					 * as the ramp's denominator now that this is
					 * a runtime cfg value, not a fixed macro. */

	/* Output ring: IO thread (producer, blocking lock, brief hold) -> RT
	 * thread (consumer, trylock). Same FIFO discipline as the input ring. */
	pthread_mutex_t out_lock;
	uint8_t out_frame[M4_AH_RING_SLOTS_MAX][M4_AH_MAX_FRAME_BYTES];
	size_t out_len[M4_AH_RING_SLOTS_MAX];
	bool out_silent[M4_AH_RING_SLOTS_MAX]; /* true iff the SOURCE pcm block that
		* produced this frame was below M4_AH_RING_QUIET_AMP_DEFAULT (computed by the
		* IO thread from the same `pcm[]` it just encoded, see
		* pcm_block_is_silent() -- never recomputed from the encoded frame
		* bytes themselves, which are opaque aptX Adaptive codewords). Only
		* meaningful for trim decisions; never read for anything else. */
	unsigned out_head, out_tail, out_count;
	/* Same role as in_over_since_ms above, for the output ring. Touched only
	 * under out_lock, by the RT thread alone (take_frame() is only ever
	 * called from the single RT data thread, never concurrently). */
	double out_over_since_ms;

	/* "Latency drift in the output ring" (see
	 * M4_AH_RING_TARGET_FRAMES_DEFAULT in the header): trim counters, folded into
	 * m4_ah_bridge_get_stats()/the JSON stats file alongside the
	 * pre-existing *_full_drops counters (a DIFFERENT, much rarer condition
	 * -- ring completely full at M4_AH_RING_SLOTS_MAX, not "above target"). */
	atomic_uint stat_output_ring_trimmed;       /* frames, written by the RT thread (take_frame()) */
	atomic_uint stat_input_ring_trimmed_blocks; /* PCM blocks, written by the IO thread */
	/* Running high-water-mark of ring occupancy, read-and-reset exactly once
	 * per JSON stats window by maybe_write_stats() (see
	 * last_window_out_ring_hwm/last_window_in_ring_hwm below) --
	 * "max out-ring over the window". stat_out_ring_hwm is updated by the RT thread
	 * (take_frame()); stat_in_ring_hwm by the IO thread (io_thread_steady_state()). */
	atomic_uint stat_out_ring_hwm;
	atomic_uint stat_in_ring_hwm;
	/* IO-thread-private: the values maybe_write_stats() last read-and-reset
	 * from the two atomics above, i.e. "high-water mark during the window
	 * that just ended". build_stats_json() reads these plain fields (not the
	 * atomics directly) so the current-snapshot file and the history line
	 * written moments apart by the very same maybe_write_stats() call always
	 * report the identical number for that window, instead of racing to
	 * reset the same atomic twice. */
	uint32_t last_window_out_ring_hwm;
	uint32_t last_window_in_ring_hwm;

	/* IO-thread-private scratch, never touched by the RT thread. */
	uint8_t scratch[M4_AH_MAX_FRAME_BYTES];
};

static double now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

/* "Latency drift in the output ring": is this whole PCM block quiet enough
 * that dropping it (input-ring trim) or the frame it produced (output-ring
 * trim) is preferable to dropping a louder one? Bounded scan (960 int16
 * samples for one M4_AH_PCM_BLOCK_BYTES block); byte-wise memcpy per sample
 * rather than a direct `(const int16_t *)pcm_bytes` cast, same reasoning as
 * the existing fade-in code just above in this file: `in_pcm`/the `pcm[]`
 * copy on the IO thread's stack are only byte-aligned, not guaranteed
 * int16_t-aligned. Deliberately a self-contained copy of the same logic as
 * a2dp-codec-aptx.c's adaptive_pcm_peak() (not a cross-file call): this file
 * has no dependency on that one (it is also exercised directly by tests that
 * never build a2dp-codec-aptx.c at all), and the two callers below only ever
 * need a bool, not the actual peak value. Called from the IO thread only
 * (io_thread_steady_state(), both for input-ring trim decisions and to tag a
 * freshly-produced output frame) -- never on the RT path, so the 960-iteration
 * cost here is irrelevant to encode()'s RT budget. */
static bool pcm_block_is_silent(const uint8_t *pcm_bytes, size_t bytes, int32_t quiet_amp)
{
	size_t n_samples = bytes / sizeof(int16_t);
	for (size_t i = 0; i < n_samples; i++) {
		int16_t v;
		memcpy(&v, pcm_bytes + i * sizeof(v), sizeof(v));
		int32_t a = v < 0 ? -(int32_t)v : (int32_t)v;
		if (a >= quiet_amp)
			return false;
	}
	return true;
}

/* poll()-bounded full read/write. Returns true on success, false on any
 * error, HUP, or overall timeout. Only ever called from the IO thread
 * (never RT). SIGPIPE is blocked for this thread by io_thread_main() before
 * any of this runs, so a write to a dead child's pipe/socket fails with
 * EPIPE instead of killing the whole WirePlumber process -- that per-thread
 * sigmask alone is already sufficient and is what the SPAWN-mode (pipe) path
 * has always relied on. `is_socket` additionally makes the SOCKET-mode path
 * use send(..., MSG_NOSIGNAL) instead of write(): this is redundant with the
 * sigmask on this specific thread today, but the task explicitly asked for
 * MSG_NOSIGNAL as defense-in-depth for the socket transport (belt-and-
 * suspenders against e.g. a future refactor that moves this call to a
 * thread that does not block SIGPIPE, or a signal handler installed
 * elsewhere in the process temporarily unblocking it) -- write() cannot take
 * MSG_NOSIGNAL and send() cannot be used on a plain pipe fd (ENOTSOCK), so
 * the two modes need two different syscalls here, not just a flag. */
static bool io_write_all(int fd, const void *buf, size_t len, uint32_t timeout_ms,
		bool is_socket)
{
	const uint8_t *p = buf;
	double deadline = now_ms() + timeout_ms;
	while (len > 0) {
		double left = deadline - now_ms();
		if (left <= 0)
			return false;
		struct pollfd pfd = { .fd = fd, .events = POLLOUT };
		int pr = poll(&pfd, 1, (int)left);
		if (pr < 0) {
			if (errno == EINTR)
				continue;
			return false;
		}
		if (pr == 0)
			return false; /* timeout */
		if (pfd.revents & (POLLERR | POLLNVAL))
			return false;
		ssize_t n = is_socket ? send(fd, p, len, MSG_NOSIGNAL) : write(fd, p, len);
		if (n < 0) {
			if (errno == EINTR || errno == EAGAIN)
				continue;
			return false; /* incl. EPIPE: peer died */
		}
		if (n == 0)
			return false;
		p += (size_t)n;
		len -= (size_t)n;
	}
	return true;
}

static bool io_read_all(int fd, void *buf, size_t len, uint32_t timeout_ms)
{
	uint8_t *p = buf;
	double deadline = now_ms() + timeout_ms;
	while (len > 0) {
		double left = deadline - now_ms();
		if (left <= 0)
			return false;
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		int pr = poll(&pfd, 1, (int)left);
		if (pr < 0) {
			if (errno == EINTR)
				continue;
			return false;
		}
		if (pr == 0)
			return false; /* timeout */
		if (pfd.revents & (POLLERR | POLLNVAL))
			return false;
		ssize_t n = read(fd, p, len);
		if (n < 0) {
			if (errno == EINTR || errno == EAGAIN)
				continue;
			return false;
		}
		if (n == 0)
			return false; /* EOF: child exited */
		p += (size_t)n;
		len -= (size_t)n;
	}
	return true;
}

/* Forward declarations: io_thread_main()'s auto-restart path
 * needs these, but they are only defined further down (reap_child() next to
 * close_fd(), try_spawn_and_handshake() next to spawn_child_process()/connect_socket()/
 * child_apply_rt_settings()) -- kept in that order because that grouping
 * reads better top-to-bottom for the non-restart-related code too. */
static void reap_child(struct m4_ah_bridge *b);
static bool try_spawn_and_handshake(struct m4_ah_bridge *b);
static void drain_child_stderr_nonblocking(struct m4_ah_bridge *b);
static void log_captured_stderr(struct m4_ah_bridge *b);

/* Forward declaration for the periodic stats writer ("metrics"). Called from both of
 * io_thread_main()'s loops (steady-state AND idle/backoff/dead), defined
 * further down next to the rest of the public m4_ah_bridge_* API since it
 * shares helpers with m4_ah_bridge_get_stats(). Deliberately takes `now` from
 * the caller rather than calling now_ms() itself -- both call sites already
 * have a fresh timestamp or are about to loop back and get one cheaply. */
static void maybe_write_stats(struct m4_ah_bridge *b, double now);

static void mark_dead(struct m4_ah_bridge *b, const char *why)
{
	bool was_alive = atomic_exchange(&b->alive, false);
	if (was_alive && b->log) {
		if (b->cfg.mode == M4_AH_HELPER_MODE_SOCKET)
			spa_log_warn(b->log, "m4-adaptive-bridge: helper marked dead: %s "
					"(socket=%s; check `journalctl --user -u "
					"'momentum4-helper@*.service'` for the service "
					"instance's own error, not visible to this process)",
					why, b->socket_path ? b->socket_path : "?");
		else
			spa_log_warn(b->log, "m4-adaptive-bridge: helper marked dead: %s", why);
		/* Same "capture+log the first stderr bytes" diagnostic as a
		 * startup handshake failure, for a helper that
		 * was working and then died mid-session -- forward declared here
		 * because these two functions are defined further down, next to
		 * spawn_child_process()/reap_child() where they logically belong
		 * (see the forward-declaration comment above
		 * try_spawn_and_handshake()). No-op in socket mode (child_err is
		 * always -1 there, see the struct field comment). */
		drain_child_stderr_nonblocking(b);
		log_captured_stderr(b);
	}
}

/* One "generation" of steady-state IO with a live child: runs until either
 * m4_ah_bridge_stop() is requested or the child is marked dead (write/read
 * failure, timeout, oversized frame report -- anything that means the pipe
 * protocol can no longer be trusted). Returns to the caller (io_thread_main)
 * either way; it is io_thread_main's job to decide what "dead" means next
 * (give up, per the original v1 design, or attempt a restart).
 * This function itself is unchanged from the original v1 loop body
 * except every `break` is now a `return` out of this whole function instead
 * of just this for(;;). */
static void io_thread_steady_state(struct m4_ah_bridge *b)
{
	for (;;) {
		if (atomic_load(&b->stop_requested))
			return;
		if (!atomic_load(&b->alive))
			return;

		maybe_write_stats(b, now_ms());

		/* Try to dequeue a queued block first, WITHOUT waiting on wake_efd:
		 * if media-sink.c's RT thread enqueued a burst of N>1 blocks within
		 * one graph cycle (see the file header), we must drain the whole
		 * backlog back-to-back, not wait out a fresh M4_AH_IDLE_POLL_MS_DEFAULT timer
		 * per block (eventfd's counter is drained to zero by a single read()
		 * below regardless of how many blocks are actually queued, so a
		 * naive "poll once per block" loop would stall on stale/absent
		 * wake-ups for every block after the first in a burst). */
		uint8_t pcm[M4_AH_PCM_BLOCK_BYTES];
		bool have_pcm = false;
		bool pcm_was_silent = false;
		double io_now = now_ms();
		pthread_mutex_lock(&b->in_lock);
		/* "Latency drift", input-ring half (see M4_AH_RING_TARGET_FRAMES_DEFAULT in
		 * m4-adaptive-bridge.h and pcm_block_is_silent() above): trim stale
		 * queued PCM BEFORE processing the next block, so a backlog left by
		 * an earlier stall is discarded here -- as raw, not-yet-encoded audio
		 * -- rather than going on to also overfill the output ring with a
		 * burst of freshly-produced frames nobody asked for at this rate
		 * (which is what used to happen: see the trim loop in
		 * m4_ah_bridge_take_frame() for the symmetric belt-and-suspenders
		 * case, e.g. a backlog that already existed before this build was
		 * ever deployed, or one that built up despite this). Bounded to
		 * M4_AH_RING_SLOTS_MAX iterations worst case; touches only index/count
		 * bookkeeping for skipped slots (no memcpy of the discarded PCM). */
		{
			unsigned trimmed = 0;
			bool sustained, hard_cap;

			/* Arm/refresh the RING-LEVEL "has been over target since" timer
			 * BEFORE computing sustained/hard_cap below, so a ring that JUST
			 * now exceeded target this call (over_since_ms was 0) always
			 * gets the full grace period, never an instant trim -- this is
			 * what keeps a legitimate, briefly-over-target burst (e.g. 6
			 * blocks arriving back-to-back within the SAME graph cycle, see
			 * the file header) from ever being touched by this loop: it is
			 * gone again (drained by ordinary one-per-call processing) long
			 * before M4_AH_RING_TRIM_SUSTAIN_MS_DEFAULT can elapse. Deliberately NOT
			 * gated on how far over target the ring is (no separate "instant
			 * trim if far enough over" shortcut) -- see
			 * test/ring_burst_test.c, which requires a burst of up to
			 * M4_AH_RING_SLOTS_MAX blocks to be delivered losslessly regardless
			 * of peak depth, as long as it drains promptly. */
			if (b->in_count > b->ring_target_frames) {
				if (b->in_over_since_ms <= 0.0)
					b->in_over_since_ms = io_now;
			} else {
				b->in_over_since_ms = 0.0;
			}
			sustained = b->in_over_since_ms > 0.0 &&
				(io_now - b->in_over_since_ms) >= b->ring_trim_sustain_ms;
			hard_cap = b->in_over_since_ms > 0.0 &&
				(io_now - b->in_over_since_ms) >= b->ring_trim_hard_cap_ms;

			while (b->in_count > b->ring_target_frames && trimmed < b->ring_slots) {
				unsigned head = b->in_head;
				bool ok_to_drop_now = hard_cap ||
					(sustained && pcm_block_is_silent(b->in_pcm[head], sizeof(b->in_pcm[head]), b->ring_quiet_amp));
				if (!ok_to_drop_now)
					break;
				b->in_head = (head + 1) % b->ring_slots;
				b->in_count--;
				trimmed++;
			}
			if (trimmed > 0) {
				atomic_fetch_add(&b->stat_input_ring_trimmed_blocks, trimmed);
				if (b->in_count <= b->ring_target_frames)
					b->in_over_since_ms = 0.0;
			}
			{
				unsigned cur_hwm = atomic_load(&b->stat_in_ring_hwm);
				if (b->in_count > cur_hwm)
					atomic_store(&b->stat_in_ring_hwm, b->in_count);
			}
		}
		if (b->in_count > 0) {
			memcpy(pcm, b->in_pcm[b->in_head], sizeof(pcm));
			pcm_was_silent = pcm_block_is_silent(pcm, sizeof(pcm), b->ring_quiet_amp);
			b->in_head = (b->in_head + 1) % b->ring_slots;
			b->in_count--;
			have_pcm = true;
		}
		pthread_mutex_unlock(&b->in_lock);

		if (!have_pcm) {
			struct pollfd pfd = { .fd = b->wake_efd, .events = POLLIN };
			int pr = poll(&pfd, 1, b->idle_poll_ms);
			if (pr < 0) {
				if (errno == EINTR)
					continue;
				return;
			}
			if (pr == 0)
				continue; /* idle timeout, re-check stop flag */
			if (pfd.revents & POLLIN) {
				uint64_t val;
				ssize_t r = read(b->wake_efd, &val, sizeof(val));
				(void)r; /* best effort drain; spurious wake is harmless */
			}
			continue; /* go back and try to dequeue again */
		}

		bool is_socket = (b->cfg.mode == M4_AH_HELPER_MODE_SOCKET);
		uint32_t len_hdr = (uint32_t)sizeof(pcm);
		if (!io_write_all(b->child_in, &len_hdr, sizeof(len_hdr), b->cfg.helper_timeout_ms, is_socket) ||
				!io_write_all(b->child_in, pcm, sizeof(pcm), b->cfg.helper_timeout_ms, is_socket)) {
			mark_dead(b, "write to helper timed out or failed");
			return;
		}

		uint32_t reply[2];
		if (!io_read_all(b->child_out, reply, sizeof(reply), b->cfg.helper_timeout_ms)) {
			mark_dead(b, "no reply from helper (timeout or EOF)");
			return;
		}
		uint32_t status = reply[0];
		uint32_t frame_bytes = reply[1];

		if (frame_bytes > 0) {
			if (frame_bytes > sizeof(b->scratch)) {
				mark_dead(b, "helper reported an oversized frame");
				return;
			}
			if (!io_read_all(b->child_out, b->scratch, frame_bytes, b->cfg.helper_timeout_ms)) {
				mark_dead(b, "short read of frame body");
				return;
			}

			const uint8_t *out_ptr = b->scratch;
			size_t out_len = frame_bytes;
			if (b->cfg.strip_ota_header) {
				if (frame_bytes >= M4_AH_OTA_HEADER_BYTES) {
					out_ptr += M4_AH_OTA_HEADER_BYTES;
					out_len -= M4_AH_OTA_HEADER_BYTES;
				} else {
					/* Malformed: shorter than the header we were told to
					 * strip. Drop it silently rather than send garbage. */
					out_len = 0;
				}
			}
			if (out_len > 0 && out_len <= sizeof(b->out_frame[0])) {
				pthread_mutex_lock(&b->out_lock);
				if (b->out_count < b->ring_slots) {
					memcpy(b->out_frame[b->out_tail], out_ptr, out_len);
					b->out_len[b->out_tail] = out_len;
					/* "Latency drift": tag this frame with the SOURCE pcm
					 * block's silence, from the exact same `pcm[]` this
					 * frame was just encoded from (FIFO 1:1 pairing, see the
					 * file header) -- consumed only by
					 * m4_ah_bridge_take_frame()'s trim loop. */
					b->out_silent[b->out_tail] = pcm_was_silent;
					b->out_tail = (b->out_tail + 1) % b->ring_slots;
					b->out_count++;
					pthread_mutex_unlock(&b->out_lock);
					atomic_fetch_add(&b->stat_frames_produced, 1);
				} else {
					/* RT thread has fallen behind draining finished frames by
					 * a full M4_AH_RING_SLOTS_MAX -- should not happen in
					 * practice (take_frame() is called once per encode(),
					 * i.e. at least as often as submit_pcm()), but drop the
					 * new frame rather than block the IO thread or overwrite
					 * an older not-yet-sent one out of FIFO order. */
					pthread_mutex_unlock(&b->out_lock);
					atomic_fetch_add(&b->stat_output_ring_full_drops, 1);
				}
			}
		}
		/* status != 0 with frame_bytes == 0 means the module is still
		 * buffering or hit a soft error on this block; not fatal, we just
		 * produce no frame this round and try again next block. */
		(void)status;
	}
}

/* Idle-waits for up to `ms`, interruptible by m4_ah_bridge_stop() via
 * wake_efd (same eventfd steady-state uses). Returns early (without
 * necessarily having waited the full `ms`) if stop_requested becomes true or
 * a spurious wake happens -- callers that need the FULL delay to have
 * elapsed check stop_requested themselves afterwards, they do not rely on
 * this having blocked for exactly `ms`. */
static void io_thread_idle_wait(struct m4_ah_bridge *b, unsigned ms)
{
	struct pollfd pfd = { .fd = b->wake_efd, .events = POLLIN };
	int pr = poll(&pfd, 1, (int)ms);
	if (pr > 0 && (pfd.revents & POLLIN)) {
		uint64_t val;
		ssize_t r = read(b->wake_efd, &val, sizeof(val));
		(void)r;
	}
}

static void *io_thread_main(void *arg)
{
	struct m4_ah_bridge *b = arg;
	unsigned backoff_ms = b->restart_backoff_min_ms;

	/* Block SIGPIPE for this thread only (per-thread signal mask, POSIX):
	 * a write() to the child's stdin after it has died must return -1/EPIPE,
	 * not deliver a process-wide SIGPIPE and kill WirePlumber. See the
	 * file header for why this is thread-local and not a global
	 * sigaction() change. Applies across restarts too: this mask is set
	 * once for the lifetime of this thread. */
	sigset_t set;
	sigemptyset(&set);
	sigaddset(&set, SIGPIPE);
	pthread_sigmask(SIG_BLOCK, &set, NULL);

	for (;;) {
		if (atomic_load(&b->stop_requested))
			break;

		/* Runs even while dead/backing off/restarting (
		 * "resilience... recovers"): the stats file staying
		 * fresh with alive=false and a growing helper_restarts count IS the
		 * observable signal that something is wrong, right up until the
		 * moment a restart succeeds -- it must not go stale/stop updating
		 * exactly when it matters most. io_thread_steady_state() has its own
		 * call for the (much more common) alive/streaming case. */
		maybe_write_stats(b, now_ms());

		if (atomic_load(&b->alive)) {
			/* Runs until stopped or the helper dies (mark_dead() flips
			 * `alive` to false from inside here); either way we land back
			 * here and the checks below decide what happens next. */
			io_thread_steady_state(b);
			continue;
		}

		if (atomic_load(&b->stop_requested))
			break;

		if (!b->cfg.auto_restart) {
			/* v1 behaviour, preserved: give up for good, stay dead until
			 * m4_ah_bridge_stop(). Idle-poll only so this thread does not
			 * busy-loop while waiting to be joined. */
			io_thread_idle_wait(b, b->idle_poll_ms);
			continue;
		}

		/* "restart the helper on a silent failure instead
		 * of a crash": wait out the backoff (interruptible), make sure the dead
		 * child is fully reaped (mark_dead() itself does not reap -- the
		 * fds/pid are still whatever io_thread_steady_state() left them
		 * as), drop any stale output frame from before the crash, then try
		 * again. submit_pcm()/take_frame() are safe no-ops on the RT side
		 * this whole time -- silence, never a crash (see m4-adaptive-bridge.h). */
		io_thread_idle_wait(b, backoff_ms);
		if (atomic_load(&b->stop_requested))
			break;

		reap_child(b);
		pthread_mutex_lock(&b->out_lock);
		b->out_head = b->out_tail = b->out_count = 0;
		b->out_over_since_ms = 0.0; /* "Latency drift": ring is empty, nothing pending to trim */
		pthread_mutex_unlock(&b->out_lock);
		/* Also drop any input backlog queued before the crash: the helper
		 * restart just reset the encoder's predictive state (fresh
		 * ENC_INIT), so replaying stale pre-crash PCM as if contiguous with
		 * whatever arrives next would be at least as discontinuous as
		 * dropping it outright, and holding onto it only delays recovery. */
		pthread_mutex_lock(&b->in_lock);
		b->in_head = b->in_tail = b->in_count = 0;
		b->in_over_since_ms = 0.0;
		pthread_mutex_unlock(&b->in_lock);

		if (try_spawn_and_handshake(b)) {
			atomic_store(&b->alive, true);
			atomic_fetch_add(&b->restart_count, 1);
			backoff_ms = b->restart_backoff_min_ms;
			if (b->log)
				spa_log_info(b->log, "m4-adaptive-bridge: helper restarted "
						"(pid %d, restart #%u)", (int)b->child_pid,
						(unsigned)atomic_load(&b->restart_count));
		} else {
			backoff_ms *= 2;
			if (backoff_ms > b->restart_backoff_max_ms)
				backoff_ms = b->restart_backoff_max_ms;
		}
	}

	return NULL;
}

static void close_fd(int *fd)
{
	if (*fd >= 0) {
		close(*fd);
		*fd = -1;
	}
}

/* MODE == SOCKET counterpart of the pipe-mode reap_child() below: there is no
 * local child process to wait for -- the peer is a sibling process managed
 * by systemd (momentum4-helper@N.service), reachable only through
 * child_in == child_out (one fd, both directions of the same AF_UNIX
 * SOCK_STREAM socket). shutdown(SHUT_WR) sends the socket-mode equivalent of
 * "EOF on stdin" (asks the guest's read loop to exit(0) cleanly, same as the
 * pipe-mode comment below describes) before close() -- close() alone would
 * also eventually get there (kernel sends a FIN either way once the last fd
 * referencing this socket closes), but the explicit shutdown() makes the
 * "we are done, drain remaining input then exit" signal immediate rather
 * than dependent on this process not holding some other duplicate of the fd. */
static void reap_child_socket(struct m4_ah_bridge *b)
{
	if (b->child_in < 0)
		return;
	shutdown(b->child_in, SHUT_WR);
	close_fd(&b->child_in);
	b->child_out = -1; /* same fd as child_in, already closed above */
}

static void reap_child(struct m4_ah_bridge *b)
{
	if (b->cfg.mode == M4_AH_HELPER_MODE_SOCKET) {
		reap_child_socket(b);
		return;
	}

	if (b->child_pid <= 0)
		return;

	/* Graceful path: EOF on stdin asks the guest's read loop to exit(0)
	 * cleanly (see hexagon/main.c). */
	close_fd(&b->child_in);

	int status;
	double deadline = now_ms() + M4_AH_STOP_GRACE_MS;
	for (;;) {
		pid_t r = waitpid(b->child_pid, &status, WNOHANG);
		if (r == b->child_pid)
			break;
		if (r < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (now_ms() >= deadline) {
			kill(b->child_pid, SIGKILL);
			waitpid(b->child_pid, &status, 0);
			break;
		}
		struct timespec ts = { .tv_sec = 0, .tv_nsec = 10 * 1000 * 1000 };
		nanosleep(&ts, NULL);
	}
	b->child_pid = -1;
	close_fd(&b->child_out);
	close_fd(&b->child_err);
}

/* Drains whatever is currently sitting in the child's stderr pipe into
 * b->err_buf, without ever blocking (child_err is O_NONBLOCK) and without
 * ever blocking the CHILD either: we keep reading (and discarding once
 * err_buf is full) until the fd reports EAGAIN/EOF/error, so a child that
 * writes more than sizeof(err_buf) before dying still never sees a full
 * pipe and stalls on write(). Safe to call at any time (child_err == -1,
 * e.g. before any spawn or after reap_child(), is a silent no-op). Not
 * RT-safe (not called from the RT thread), not a hot path (only called
 * right after a handshake failure or a mark_dead()). */
static void drain_child_stderr_nonblocking(struct m4_ah_bridge *b)
{
	if (b->child_err < 0)
		return;
	for (;;) {
		char tmp[512];
		ssize_t n = read(b->child_err, tmp, sizeof(tmp));
		if (n <= 0)
			return; /* EAGAIN (nothing more right now), EOF, or error */
		size_t space = sizeof(b->err_buf) - b->err_len;
		size_t copy = (size_t)n < space ? (size_t)n : space;
		if (copy > 0) {
			memcpy(b->err_buf + b->err_len, tmp, copy);
			b->err_len += copy;
		}
		/* Bytes beyond err_buf's capacity are intentionally dropped --
		 * see the struct field comment: we only care about the first
		 * lines. Draining the fd regardless (not just when there is
		 * room left) is what matters for not blocking the child. */
	}
}

/* Logs whatever drain_child_stderr_nonblocking() captured, one spa_log line
 * per source line, so a multi-line qemu-hexagon error (the common case)
 * shows up as readable separate journal entries
 * instead of one line with embedded newlines. No-op if nothing was
 * captured (e.g. the child died silently, or M4_ADAPTIVE_HELPER_DEBUG_LOG
 * behaviour previously relied on -- that env var no longer changes where
 * stderr goes, see spawn_child_process() (SPAWN mode)). Does not clear err_buf: callers reset it
 * themselves on the next spawn_child_process(). */
static void log_captured_stderr(struct m4_ah_bridge *b)
{
	size_t start;

	if (!b->log || b->err_len == 0)
		return;

	spa_log_warn(b->log, "m4-adaptive-bridge: helper stderr (first %zu bytes captured):",
			b->err_len);
	start = 0;
	for (size_t i = 0; i < b->err_len; i++) {
		if (b->err_buf[i] != '\n' && i != b->err_len - 1)
			continue;
		size_t end = (b->err_buf[i] == '\n') ? i : i + 1;
		if (end > start)
			spa_log_warn(b->log, "m4-adaptive-bridge: helper stderr: %.*s",
					(int)(end - start), b->err_buf + start);
		start = i + 1;
	}
}

/* Non-blocking waitpid() + human-readable exit reason, used right after a
 * handshake failure to tell apart two very different situations that used
 * to produce the exact same log message during debugging:
 * the child already exited (crash, e.g. denied mprotect() under a
 * MemoryDenyWriteExecute=yes systemd sandbox -- fast, well under the
 * configured timeout budget) vs. the child is still running and we are
 * hitting a genuine startup_timeout_ms budget exhaustion (slow qemu boot on
 * a loaded host). Must be called BEFORE reap_child(), which itself calls
 * waitpid() again for cleanup -- that second call is harmless (ECHILD once
 * we already reaped here, or reaps a child that only exited in the window
 * between the two calls). */
static void log_handshake_failure(struct m4_ah_bridge *b, double elapsed_ms,
		uint32_t budget_ms)
{
	int status = 0;
	pid_t r;

	if (!b->log)
		return;

	if (b->cfg.mode == M4_AH_HELPER_MODE_SOCKET) {
		/* No local child to waitpid()/kill() -- the peer is a
		 * systemd-managed service instance in a different process tree.
		 * "connect() succeeded but the handshake never arrived" here most
		 * likely means the service instance itself crashed/exited before
		 * writing its startup reply (e.g. the exact qemu_mprotect__osdep
		 * failure this whole redesign exists to avoid, but now happening in
		 * momentum4-helper@N.service instead of inside WirePlumber) --
		 * point the reader at the one place that DOES have that process's
		 * stderr, since we cannot capture it locally in this mode (see the
		 * struct m4_ah_bridge field comments). */
		spa_log_error(b->log, "m4-adaptive-bridge: no startup handshake over "
				"the socket after %.0f ms of a %u ms budget (socket=%s) -- "
				"check `journalctl --user -u 'momentum4-helper@*.service'` "
				"for the service instance's own error (this process cannot "
				"see its stderr directly in socket mode)",
				elapsed_ms, budget_ms, b->socket_path ? b->socket_path : "?");
		return;
	}

	r = (b->child_pid > 0) ? waitpid(b->child_pid, &status, WNOHANG) : -1;
	if (r == b->child_pid) {
		if (WIFEXITED(status))
			spa_log_error(b->log, "m4-adaptive-bridge: helper exited early "
					"(after %.0f ms of a %u ms startup budget) with exit "
					"code %d -- not a timeout, the child gave up on its own",
					elapsed_ms, budget_ms, WEXITSTATUS(status));
		else if (WIFSIGNALED(status))
			spa_log_error(b->log, "m4-adaptive-bridge: helper was killed by "
					"signal %d (%s) after %.0f ms of a %u ms startup budget "
					"-- not a timeout, the child died on its own",
					WTERMSIG(status), strsignal(WTERMSIG(status)),
					elapsed_ms, budget_ms);
		else
			spa_log_error(b->log, "m4-adaptive-bridge: helper stopped "
					"(raw wait status 0x%x) after %.0f ms of a %u ms "
					"startup budget", (unsigned)status, elapsed_ms, budget_ms);
	} else {
		spa_log_error(b->log, "m4-adaptive-bridge: helper did not answer the "
				"startup handshake and was still running after the full "
				"%u ms budget (elapsed %.0f ms) -- killing it",
				budget_ms, elapsed_ms);
	}

	drain_child_stderr_nonblocking(b);
	log_captured_stderr(b);
}

/* Best-effort RT priority + CPU pinning for the qemu-hexagon child, task
 * item 4 ("RT priority for the helper (chrt without sudo, rlimit), taskset").
 * Runs AFTER fork(), BEFORE execl(), i.e. still running our own (tiny,
 * stack-only, async-signal-safe-ish) code in the child -- deliberately not
 * using spa_log/malloc here for the same reason glibc fork() handlers avoid
 * them. Every step is independently best-effort: EPERM (no RLIMIT_RTPRIO/
 * CAP_SYS_NICE) or a malformed cpu list just falls back to the unprivileged
 * default (SCHED_OTHER, no pinning) -- this must never be why the helper
 * fails to start. Errors are only reported if `debug` is true ([helper]
 * debug-log, resolved by the PARENT before fork() -- this function runs
 * after fork(), before exec(), so it must not itself call the config loader
 * (fopen/malloc, neither async-signal-safe); child's stderr is otherwise
 * /dev/null, see the caller). */
static void child_apply_rt_settings(int rt_priority, const char *cpu_affinity_list, bool debug)
{
	if (rt_priority > 0) {
		struct sched_param sp = { .sched_priority = rt_priority };
		if (sched_setscheduler(0, SCHED_FIFO, &sp) != 0 && debug)
			fprintf(stderr, "m4-adaptive-bridge(child): sched_setscheduler(FIFO,%d) "
					"failed: %m (continuing at normal priority)\n", rt_priority);
		/* mlockall so the RT-scheduled child does not take page faults for
		 * its (small, fixed) working set once it is hot. */
		if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0 && debug)
			fprintf(stderr, "m4-adaptive-bridge(child): mlockall failed: %m "
					"(continuing without memory locking)\n");
	}

	if (cpu_affinity_list && *cpu_affinity_list) {
		cpu_set_t set;
		char buf[256];
		char *tok, *save = NULL;
		bool any = false;

		CPU_ZERO(&set);
		snprintf(buf, sizeof(buf), "%s", cpu_affinity_list);
		for (tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
			char *end;
			long cpu = strtol(tok, &end, 10);
			if (end == tok || cpu < 0 || cpu >= CPU_SETSIZE)
				continue;
			CPU_SET((int)cpu, &set);
			any = true;
		}
		if (any && sched_setaffinity(0, sizeof(set), &set) != 0 && debug)
			fprintf(stderr, "m4-adaptive-bridge(child): sched_setaffinity(%s) "
					"failed: %m (continuing unpinned)\n", cpu_affinity_list);
	}
}

/* "RT for the bridge's IO thread": best-effort SCHED_FIFO for THIS process's own IO
 * thread (see cfg.io_thread_rt_priority's doc comment in
 * m4-adaptive-bridge.h for why this -- pthread_setschedparam() -- is the
 * only viable mechanism here, not spa_thread_utils_acquire_rt()). Called
 * from m4_ah_bridge_start(), on the caller's thread (WirePlumber's
 * main/config thread, same as every other m4_ah_bridge_start() step), AFTER
 * pthread_create() has returned a valid `b->io_thread` handle -- unlike
 * child_apply_rt_settings() above (which runs INSIDE the forked child
 * between fork()/exec() and is therefore restricted to async-signal-safe
 * calls), this function runs as ordinary code in the parent and may freely
 * use spa_log/fprintf.
 *
 * Every step is independently best-effort, same philosophy as
 * child_apply_rt_settings(): a priority of 0 (default, disabled) or any
 * failure here must never be why m4_ah_bridge_start() itself fails -- this
 * is a latency/jitter optimization for the IO thread's helper round trips
 * under host contention, not a correctness requirement (io_thread_main()'s
 * own design, see the file header, already tolerates arbitrary IO-thread
 * scheduling delay without corrupting data, only adding latency). */
static void apply_io_thread_rt(struct m4_ah_bridge *b)
{
	int policy = SCHED_OTHER;
	struct sched_param sp;

	memset(&sp, 0, sizeof(sp));

	if (b->cfg.io_thread_rt_priority > 0) {
		struct sched_param want = { .sched_priority = b->cfg.io_thread_rt_priority };
		int rc = pthread_setschedparam(b->io_thread, SCHED_FIFO, &want);
		if (rc != 0 && b->log) {
			/* EPERM is the expected, unremarkable outcome when
			 * wireplumber.service has no LimitRTPRIO= (or this bridge is
			 * being run outside of any systemd unit at all, e.g. a test
			 * harness) -- logged at info, not warn, for exactly that
			 * reason; any OTHER errno (e.g. EINVAL from an out-of-range
			 * priority, which m4-adaptive-config.c's 0..99 clamp should
			 * already prevent, but env-var overrides bypass that clamp --
			 * see a2dp-codec-aptx.c) is logged at warn since it points at
			 * a real configuration mistake worth fixing. */
			if (rc == EPERM)
				spa_log_info(b->log, "m4-adaptive-bridge: IO thread stays "
						"SCHED_OTHER (SCHED_FIFO prio %d needs "
						"LimitRTPRIO= >= %d on wireplumber.service, or "
						"CAP_SYS_NICE): %s", b->cfg.io_thread_rt_priority,
						b->cfg.io_thread_rt_priority, strerror(rc));
			else
				spa_log_warn(b->log, "m4-adaptive-bridge: pthread_setschedparam"
						"(SCHED_FIFO, %d) for the IO thread failed: %s "
						"(continuing SCHED_OTHER)",
						b->cfg.io_thread_rt_priority, strerror(rc));
		}
	}

	/* Query back the REAL, currently-effective policy/priority regardless
	 * of the branch above (including io_thread_rt_priority == 0, where this
	 * simply confirms SCHED_OTHER/0 -- deliberately not just assumed) --
	 * this is what m4_ah_bridge_get_stats()/the JSON file report, so an
	 * operator can see the actual outcome without cross-referencing the
	 * log. Best-effort: on the vanishingly unlikely failure of
	 * pthread_getschedparam() itself, leave the SCHED_OTHER/0 default set
	 * just above rather than report a stale or fabricated value. */
	if (pthread_getschedparam(b->io_thread, &policy, &sp) != 0) {
		policy = SCHED_OTHER;
		sp.sched_priority = 0;
	}
	atomic_store(&b->io_rt_policy, policy);
	atomic_store(&b->io_rt_prio, sp.sched_priority);
}

/* Async-signal-safe-ish ("explicit FD_CLOEXEC and closing
 * extra fds in the child before exec'ing qemu (close_range)"). WirePlumber is multi-threaded, so
 * between fork() and execve() this child may only call functions that do not
 * touch libc-internal locks/allocators -- the same constraint that already
 * keeps child_apply_rt_settings() below away from malloc()/spa_log() (see its
 * comment). close_range() is a single raw syscall with no such side effects,
 * so it is safe here; a directory-scan-based fallback (readdir() on
 * /proc/self/fd) would call malloc() internally and is deliberately NOT used.
 *
 * Closes every fd >= 3 in the child (stdin/stdout/stderr, already dup2()'d to
 * the pipe ends/devnull by the caller, are left alone). This is
 * defense-in-depth on top of modern glib/pipewire already creating most of
 * its own descriptors O_CLOEXEC/SOCK_CLOEXEC (not fully verifiable from
 * outside the process) -- after this call, whatever the
 * qemu-hexagon child inherits or does not inherit no longer depends on that
 * being true. Best-effort: on a kernel/libc without close_range() (pre-5.9
 * kernel or glibc < 2.34) this is silently a no-op, same fallback philosophy
 * as the rest of this function's neighbours in this file. */
static void child_close_extra_fds(void)
{
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 34))
	close_range(3, ~0U, 0);
#endif
}

extern char **environ;

/* "reconnect-grace" + task option (c): make
 * APTX_AD_START_MUTE_PKTS=0 the effective default for SPAWN-mode children
 * too (SOCKET mode gets the same default via Environment= in
 * systemd/momentum4-helper@.service.in -- this is SPAWN mode's
 * equivalent, since that unit file does not apply when WirePlumber forks
 * the helper directly). Only ever ADDS the variable if the parent's own
 * environment does not already define it, so an operator's explicit
 * `APTX_AD_START_MUTE_PKTS=N` (env or a systemd overlay Environment=) is
 * never overridden. Must be built in the PARENT, before fork(): the child
 * between fork() and execve() may only call async-signal-safe functions
 * (see child_close_extra_fds()'s comment on the same constraint), and
 * malloc()/strncmp() here are not among them. Returns NULL on allocation
 * failure -- callers must then fall back to passing `environ` unmodified
 * (execve() with the parent's own environ is exactly what execl() used to
 * do), never fail the spawn over this. */
static char **build_child_envp_with_mute_default(void)
{
	size_t n = 0;
	bool need_mute = true;
	char **envp;

	while (environ[n] != NULL) {
		if (need_mute && strncmp(environ[n], "APTX_AD_START_MUTE_PKTS=", 25) == 0)
			need_mute = false;
		n++;
	}

	envp = malloc((n + (need_mute ? 1 : 0) + 1) * sizeof(char *));
	if (envp == NULL)
		return NULL;

	for (size_t i = 0; i < n; i++)
		envp[i] = environ[i];
	if (need_mute)
		envp[n++] = (char *)"APTX_AD_START_MUTE_PKTS=0";
	envp[n] = NULL;
	return envp;
}

static bool spawn_child_process(struct m4_ah_bridge *b)
{
	int in_pipe[2], out_pipe[2], err_pipe[2];
	char *child_argv[4];
	char **child_envp;

	/* New spawn attempt: whatever stderr we captured from a previous
	 * child (if any, e.g. an auto-restart cycle) is no longer relevant. */
	b->err_len = 0;

	if (pipe(in_pipe) != 0)
		return false;
	if (pipe(out_pipe) != 0) {
		close(in_pipe[0]); close(in_pipe[1]);
		return false;
	}
	if (pipe(err_pipe) != 0) {
		close(in_pipe[0]); close(in_pipe[1]);
		close(out_pipe[0]); close(out_pipe[1]);
		return false;
	}

	/* Built in the parent, before fork() -- see
	 * build_child_envp_with_mute_default()'s comment for why this cannot be
	 * done between fork() and execve(). A NULL child_envp (allocation
	 * failure) is not fatal: falls back to execve(..., environ), i.e. the
	 * exact same behaviour execl() had before this change. */
	child_argv[0] = b->qemu_path;
	child_argv[1] = b->loader_path;
	child_argv[2] = b->capi_path;
	child_argv[3] = NULL;
	child_envp = build_child_envp_with_mute_default();

	pid_t pid = fork();
	if (pid < 0) {
		close(in_pipe[0]); close(in_pipe[1]);
		close(out_pipe[0]); close(out_pipe[1]);
		close(err_pipe[0]); close(err_pipe[1]);
		free(child_envp);
		return false;
	}
	if (pid == 0) {
		dup2(in_pipe[0], STDIN_FILENO);
		dup2(out_pipe[1], STDOUT_FILENO);
		/* Always piped to the parent now: a plain /dev/null
		 * redirect made every startup/crash failure
		 * silent unless M4_ADAPTIVE_HELPER_DEBUG_LOG was set for that run,
		 * which is exactly backwards -- the one time you need the child's
		 * stderr is the failure you did not expect. The parent captures
		 * the first bytes (drain_child_stderr_nonblocking()) and logs them
		 * via spa_log on any handshake failure or death, so this ends up
		 * in the journal either way; M4_ADAPTIVE_HELPER_DEBUG_LOG still
		 * separately controls child_apply_rt_settings()'s own fprintf()s
		 * (unchanged). */
		dup2(err_pipe[1], STDERR_FILENO);
		close(in_pipe[0]); close(in_pipe[1]);
		close(out_pipe[0]); close(out_pipe[1]);
		close(err_pipe[0]); close(err_pipe[1]);
		child_close_extra_fds();
		child_apply_rt_settings(b->cfg.rt_priority, b->cfg.cpu_affinity_list, b->cfg.debug_log);
		execve(b->qemu_path, child_argv, child_envp != NULL ? child_envp : environ);
		_exit(127);
	}

	free(child_envp);

	close(in_pipe[0]);
	close(out_pipe[1]);
	close(err_pipe[1]);
	b->child_pid = pid;
	b->child_in = in_pipe[1];
	b->child_out = out_pipe[0];
	b->child_err = err_pipe[0];

	/* child_out/child_in: non-blocking-capable fds, our poll()-based
	 * helpers rely on POLLIN/POLLOUT readiness, plain blocking fds work
	 * fine with poll() too, no O_NONBLOCK needed there (poll() + the fact
	 * we only read/write after a ready event keeps this correct and
	 * simple). child_err is different: drain_child_stderr_nonblocking()
	 * deliberately never poll()s it first (it is only ever read
	 * opportunistically, right after a failure, not on the main wait
	 * path), so it MUST be O_NONBLOCK or it could block the IO thread
	 * indefinitely if the child never writes anything else. */
	int flags = fcntl(b->child_err, F_GETFL, 0);
	if (flags != -1)
		fcntl(b->child_err, F_SETFL, flags | O_NONBLOCK);

	return true;
}

/* MODE == SOCKET counterpart of spawn_child_process(): non-blocking
 * connect(AF_UNIX, b->socket_path) bounded by cfg.connect_timeout_ms (or
 * M4_AH_CONNECT_TIMEOUT_MS_DEFAULT if 0), using poll(POLLOUT)+SO_ERROR (the standard
 * non-blocking-connect idiom -- connect() on a non-blocking socket returns
 * -1/EINPROGRESS immediately, and completion is signalled by the fd becoming
 * writable; SO_ERROR then tells us whether it actually succeeded or failed).
 * SOCK_CLOEXEC is set at socket() time so this fd never leaks into any other
 * child WirePlumber itself might fork/exec (the socket-mode equivalent of
 * this project's own close_range() helper -- there, defense-in-depth for a child WE
 * fork; here, there is no child to fork, so the only leak surface left is
 * this one fd, closed the ordinary way). On any failure, returns false with
 * child_in/child_out/child_pid left at -1 (nothing to reap). */
static bool connect_socket(struct m4_ah_bridge *b)
{
	struct sockaddr_un addr;
	int fd;
	uint32_t timeout_ms = b->cfg.connect_timeout_ms ?
			b->cfg.connect_timeout_ms : M4_AH_CONNECT_TIMEOUT_MS_DEFAULT;

	b->err_len = 0; /* nothing to capture in socket mode, kept zeroed for symmetry */

	if (b->socket_path == NULL || *b->socket_path == '\0')
		return false;
	if (strlen(b->socket_path) >= sizeof(addr.sun_path))
		return false; /* AF_UNIX path length limit, fail closed rather than truncate */

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	memcpy(addr.sun_path, b->socket_path, strlen(b->socket_path));

	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return false;

	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		if (errno != EINPROGRESS) {
			close(fd);
			return false;
		}
		struct pollfd pfd = { .fd = fd, .events = POLLOUT };
		double deadline = now_ms() + timeout_ms;
		for (;;) {
			double left = deadline - now_ms();
			if (left <= 0) {
				close(fd);
				return false;
			}
			int pr = poll(&pfd, 1, (int)left);
			if (pr < 0) {
				if (errno == EINTR)
					continue;
				close(fd);
				return false;
			}
			if (pr == 0) {
				close(fd); /* connect() timeout */
				return false;
			}
			break;
		}
		int soerr = 0;
		socklen_t soerr_len = sizeof(soerr);
		if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &soerr_len) != 0 || soerr != 0) {
			close(fd);
			return false;
		}
	}

	b->child_pid = -1;
	b->child_in = fd;
	b->child_out = fd;
	b->child_err = -1;
	return true;
}

/* Establishes the transport (fork+exec+pipes in SPAWN mode, connect(AF_UNIX)
 * in SOCKET mode -- see the two functions above) and performs the identical
 * synchronous startup handshake (read {status, detail}, see
 * m4-adaptive-protocol.h) either way. Used both by m4_ah_bridge_start() (the
 * very first attempt, where failure must be reported to the caller so
 * codec_init() can fail closed) and by io_thread_main()'s auto-restart loop
 *, where failure just means "try again after backoff, keep
 * reporting silence". Reaps/closes the transport itself on any failure path,
 * so callers never need to. */
static bool try_spawn_and_handshake(struct m4_ah_bridge *b)
{
	uint32_t startup_timeout_ms = b->cfg.startup_timeout_ms ?
			b->cfg.startup_timeout_ms : M4_AH_STARTUP_TIMEOUT_MS_DEFAULT;
	uint32_t startup[2];
	double t0;
	bool established = (b->cfg.mode == M4_AH_HELPER_MODE_SOCKET) ?
			connect_socket(b) : spawn_child_process(b);

	if (!established)
		return false;

	t0 = now_ms();
	if (!io_read_all(b->child_out, startup, sizeof(startup), startup_timeout_ms)) {
		/* Distinguishes "child already died" (fast, well under budget --
		 * e.g. denied mprotect() PROT_EXEC under a MemoryDenyWriteExecute=
		 * yes systemd sandbox) from "child is still
		 * running and we genuinely burned the whole budget" -- the old
		 * message here was identical ("...within N ms") for both, which is
		 * what made the live failure look like a plain timeout when it was
		 * actually an instant crash. */
		log_handshake_failure(b, now_ms() - t0, startup_timeout_ms);
		reap_child(b);
		return false;
	}
	if (startup[0] != 0) {
		if (b->log)
			spa_log_error(b->log, "m4-adaptive-bridge: helper reported CAPI/ENC_INIT "
					"failure, status=0x%x", startup[0]);
		drain_child_stderr_nonblocking(b);
		log_captured_stderr(b);
		reap_child(b);
		return false;
	}
	return true;
}

bool m4_ah_bridge_paths_available(const char *qemu_path, const char *loader_path,
		const char *capi_path)
{
	if (!qemu_path || !*qemu_path || !loader_path || !*loader_path ||
			!capi_path || !*capi_path)
		return false;
	/* Same three checks as m4_ah_bridge_start()'s SPAWN-mode branch below,
	 * deliberately duplicated rather than shared: this function must stay a
	 * pure, side-effect-free (no struct m4_ah_bridge allocation, no logging
	 * object required) probe callable from fill_caps(), so keeping it
	 * independent avoids coupling its contract to bridge_start()'s
	 * internals. See m4-adaptive-bridge.h for why fill_caps() only gets
	 * this cheap check and not a full spawn+handshake probe. */
	if (access(qemu_path, X_OK) != 0)
		return false;
	if (access(loader_path, R_OK) != 0)
		return false;
	if (access(capi_path, R_OK) != 0)
		return false;
	return true;
}

bool m4_ah_bridge_socket_available(const char *socket_path)
{
	struct stat st;

	if (!socket_path || !*socket_path)
		return false;
	if (stat(socket_path, &st) != 0)
		return false;
	return S_ISSOCK(st.st_mode);
}

struct m4_ah_bridge *m4_ah_bridge_start(struct spa_log *log,
		const struct m4_ah_bridge_config *cfg)
{
	struct m4_ah_bridge *b;
	bool socket_mode;

	if (cfg == NULL)
		return NULL;
	socket_mode = (cfg->mode == M4_AH_HELPER_MODE_SOCKET);
	if (socket_mode) {
		if (cfg->socket_path == NULL || *cfg->socket_path == '\0')
			return NULL;
	} else if (cfg->qemu_path == NULL || cfg->loader_path == NULL ||
			cfg->capi_path == NULL) {
		return NULL;
	}

	b = calloc(1, sizeof(*b));
	if (b == NULL)
		return NULL;

	b->log = log;
	b->cfg = *cfg;
	b->child_pid = -1;
	b->child_in = -1;
	b->child_out = -1;
	b->child_err = -1;
	b->err_len = 0;
	b->wake_efd = -1;
	atomic_init(&b->stop_requested, false);
	atomic_init(&b->alive, false);
	atomic_init(&b->restart_count, 0);
	/* "RT for the bridge's IO thread": SCHED_OTHER/0 until apply_io_thread_rt() runs
	 * (right after pthread_create() below) -- a correct, honest default for
	 * the brief window between calloc() and that call, and the permanent
	 * value if pthread_create() itself never succeeds (goto fail path). */
	atomic_init(&b->io_rt_policy, SCHED_OTHER);
	atomic_init(&b->io_rt_prio, 0);
	pthread_mutex_init(&b->in_lock, NULL);
	pthread_mutex_init(&b->out_lock, NULL);
	/* "reconnect-grace" + task option (c): armed once per
	 * m4_ah_bridge_start() (i.e. once per real ENC_INIT), consumed by the
	 * first cfg->fade_in_frames frames submitted afterwards. 0 (disabled)
	 * if cfg->fade_in_enabled is false ([adaptive] fade-in = off) -- the
	 * caller (a2dp-codec-aptx.c) resolves this from momentum4.conf now,
	 * this bridge no longer reads any environment variable itself
	 * ("Env -- for tests only"). */
	b->fade_in_frames_left = cfg->fade_in_enabled ?
			(cfg->fade_in_frames > 0 ? (uint32_t)cfg->fade_in_frames : M4_AH_FADE_IN_FRAMES_DEFAULT) : 0;
	b->fade_in_frames_total = b->fade_in_frames_left;

	/* Ring: working capacity from config, capped at
	 * M4_AH_RING_SLOTS_MAX -- every cfg field below follows the same
	 * "<= 0 => built-in default" convention this struct already used for
	 * connect_timeout_ms/startup_timeout_ms -- so every existing caller/test
	 * that memset()s cfg to 0 and never sets these gets EXACTLY the same
	 * behaviour as before this field existed. `ring_slots` is additionally
	 * clamped to [1, M4_AH_RING_SLOTS_MAX] regardless of what cfg said
	 * (common/m4-conf-keys.def's own registry range already enforces this
	 * for a real momentum4.conf value, but a test or a future caller
	 * bypassing that registry must not be able to overrun the fixed-size
	 * in_pcm/out_frame/out_len/out_silent arrays above). */
	b->ring_slots = (cfg->ring_slots > 0) ? (unsigned)cfg->ring_slots : M4_AH_RING_SLOTS_DEFAULT;
	if (b->ring_slots < 1)
		b->ring_slots = 1;
	else if (b->ring_slots > M4_AH_RING_SLOTS_MAX)
		b->ring_slots = M4_AH_RING_SLOTS_MAX;
	b->ring_target_frames = (cfg->ring_target_frames > 0) ?
			(unsigned)cfg->ring_target_frames : M4_AH_RING_TARGET_FRAMES_DEFAULT;
	b->ring_trim_sustain_ms = (cfg->ring_trim_sustain_ms > 0.0) ?
			cfg->ring_trim_sustain_ms : M4_AH_RING_TRIM_SUSTAIN_MS_DEFAULT;
	b->ring_trim_hard_cap_ms = (cfg->ring_trim_hard_cap_ms > 0.0) ?
			cfg->ring_trim_hard_cap_ms : M4_AH_RING_TRIM_HARD_CAP_MS_DEFAULT;
	b->ring_quiet_amp = (cfg->ring_quiet_amp > 0) ? cfg->ring_quiet_amp : M4_AH_RING_QUIET_AMP_DEFAULT;
	b->idle_poll_ms = (cfg->idle_poll_ms > 0) ? (uint32_t)cfg->idle_poll_ms : M4_AH_IDLE_POLL_MS_DEFAULT;
	b->restart_backoff_min_ms = (cfg->restart_backoff_min_ms > 0) ?
			(unsigned)cfg->restart_backoff_min_ms : M4_AH_RESTART_BACKOFF_MIN_MS_DEFAULT;
	b->restart_backoff_max_ms = (cfg->restart_backoff_max_ms > 0) ?
			(unsigned)cfg->restart_backoff_max_ms : M4_AH_RESTART_BACKOFF_MAX_MS_DEFAULT;
	b->stats_history_lines = (cfg->stats_history_lines > 0) ?
			(unsigned)cfg->stats_history_lines : M4_AH_STATS_HISTORY_MAX_LINES_DEFAULT;
	b->stats_history_rotate_every = (cfg->stats_history_rotate_every > 0) ?
			(unsigned)cfg->stats_history_rotate_every : M4_AH_STATS_HISTORY_ROTATE_EVERY_DEFAULT;

	/* JSON stats file. Own copies (strdup), same reasoning as
	 * qemu_path/loader_path/capi_path/socket_path just below: the caller's
	 * cfg->stats_path may point at a short-lived stack buffer
	 * (adaptive_resolve_helper_paths()-style resolution in
	 * a2dp-codec-aptx.c), so b->cfg (a shallow struct copy) is not safe to
	 * dereference here past this call. NULL stats_path is a valid,
	 * fully-supported "disabled" state -- strdup(NULL) is never attempted. */
	if (cfg->stats_path != NULL) {
		b->stats_path = strdup(cfg->stats_path);
		if (!b->stats_path)
			goto fail;
	}
	if (cfg->stats_history_path != NULL) {
		b->stats_history_path = strdup(cfg->stats_history_path);
		if (!b->stats_history_path)
			goto fail;
	}
	b->stats_interval_ms = cfg->stats_interval_ms ? cfg->stats_interval_ms : 10000;
	b->start_ms = now_ms();
	b->last_stats_write_ms = b->start_ms;
	b->stats_history_writes = 0;
	snprintf(b->meta_mode, sizeof(b->meta_mode), "?");

	if (socket_mode) {
		b->socket_path = strdup(cfg->socket_path);
		if (!b->socket_path)
			goto fail;
		/* Cheap pre-check (same probe fill_caps() already used, see
		 * m4_ah_bridge_socket_available() -- deliberately re-checked here
		 * too so a caller that skips fill_caps() entirely, e.g. every test
		 * in this repo, still gets a clean fail-closed NULL instead of
		 * spending a full connect_timeout_ms finding out connect() itself
		 * fails). A missing/wrong-type socket file after this check but
		 * before the real connect() a few lines below (TOCTOU) just falls
		 * through to a normal connect() failure -- not a safety issue, only
		 * changes which log line explains it. */
		if (!m4_ah_bridge_socket_available(b->socket_path)) {
			if (log)
				spa_log_error(log, "m4-adaptive-bridge: helper socket not "
						"present (or not a socket): %s -- is "
						"`systemctl --user status momentum4-helper.socket` "
						"active?", b->socket_path);
			goto fail;
		}
	} else {
		b->qemu_path = strdup(cfg->qemu_path);
		b->loader_path = strdup(cfg->loader_path);
		b->capi_path = strdup(cfg->capi_path);
		if (!b->qemu_path || !b->loader_path || !b->capi_path)
			goto fail;

		if (access(b->qemu_path, X_OK) != 0) {
			if (log)
				spa_log_error(log, "m4-adaptive-bridge: qemu binary not executable: %s (%m)",
						b->qemu_path);
			goto fail;
		}
		if (access(b->loader_path, R_OK) != 0) {
			if (log)
				spa_log_error(log, "m4-adaptive-bridge: loader ELF not readable: %s (%m)",
						b->loader_path);
			goto fail;
		}
		if (access(b->capi_path, R_OK) != 0) {
			if (log)
				spa_log_error(log, "m4-adaptive-bridge: CAPI .so not readable: %s (%m)",
						b->capi_path);
			goto fail;
		}
	}

	b->wake_efd = eventfd(0, EFD_NONBLOCK);
	if (b->wake_efd < 0)
		goto fail;

	/* The very first attempt still fails m4_ah_bridge_start() outright on
	 * failure (returns NULL) -- codec_init_adaptive() relies on this to fall
	 * back to aptX HD . Auto-restart
	 * only applies to a helper that came up successfully here
	 * and later died; see io_thread_main(). */
	if (!try_spawn_and_handshake(b))
		goto fail;

	atomic_store(&b->alive, true);
	if (pthread_create(&b->io_thread, NULL, io_thread_main, b) != 0) {
		atomic_store(&b->alive, false);
		reap_child(b);
		goto fail;
	}
	b->io_thread_started = true;

	/* "RT for the bridge's IO thread": after the thread exists, before the caller
	 * (codec_init_adaptive()) can observe/log/stats-report anything about
	 * it -- see apply_io_thread_rt()'s doc comment for why
	 * pthread_setschedparam() (not spa_thread_utils_acquire_rt()) is the
	 * only viable mechanism here, and cfg.io_thread_rt_priority's doc
	 * comment in m4-adaptive-bridge.h for the full architectural reasoning. */
	apply_io_thread_rt(b);

	if (log) {
		if (socket_mode)
			spa_log_info(log, "m4-adaptive-bridge: helper ready (socket=%s)",
					b->socket_path);
		else
			spa_log_info(log, "m4-adaptive-bridge: helper ready (pid %d, qemu=%s)",
					(int)b->child_pid, b->qemu_path);
	}
	return b;

fail:
	free(b->qemu_path);
	free(b->loader_path);
	free(b->capi_path);
	free(b->socket_path);
	if (b->wake_efd >= 0)
		close(b->wake_efd);
	pthread_mutex_destroy(&b->in_lock);
	pthread_mutex_destroy(&b->out_lock);
	free(b);
	return NULL;
}

void m4_ah_bridge_stop(struct m4_ah_bridge *b)
{
	if (b == NULL)
		return;

	atomic_store(&b->stop_requested, true);
	if (b->wake_efd >= 0) {
		uint64_t one = 1;
		ssize_t r = write(b->wake_efd, &one, sizeof(one));
		(void)r; /* best effort; the idle poll timeout also wakes it */
	}
	if (b->io_thread_started)
		pthread_join(b->io_thread, NULL);

	reap_child(b);
	if (b->wake_efd >= 0)
		close(b->wake_efd);

	pthread_mutex_destroy(&b->in_lock);
	pthread_mutex_destroy(&b->out_lock);
	free(b->qemu_path);
	free(b->loader_path);
	free(b->capi_path);
	free(b->socket_path);
	free(b->stats_path);
	free(b->stats_history_path);
	free(b);
}

bool m4_ah_bridge_submit_pcm(struct m4_ah_bridge *b, const void *pcm, size_t bytes)
{
	if (b == NULL || pcm == NULL || bytes != M4_AH_PCM_BLOCK_BYTES)
		return false;
	if (!atomic_load(&b->alive))
		return false;

	if (pthread_mutex_trylock(&b->in_lock) != 0) {
		/* Metrics: distinct from a genuinely full ring
		 * (below) -- this is the IO thread holding the lock for a memcpy
		 * (bounded to microseconds, see the file header), not the helper
		 * being behind. Was previously not counted anywhere. */
		atomic_fetch_add(&b->stat_input_trylock_drops, 1);
		return false;
	}
	if (b->in_count >= b->ring_slots) {
		/* Ring genuinely full: the helper is behind by a full
		 * M4_AH_RING_SLOTS_MAX blocks, not just a transient within-cycle burst
		 * (see file header). Reject rather than overwrite an unconsumed
		 * slot out of FIFO order. */
		pthread_mutex_unlock(&b->in_lock);
		atomic_fetch_add(&b->stat_input_ring_full_drops, 1);
		return false;
	}
	{
		unsigned slot = b->in_tail;
		memcpy(b->in_pcm[slot], pcm, bytes);
		if (b->fade_in_frames_left > 0) {
			/* "reconnect-grace" + task option (c): linear
			 * 0 -> 1 gain ramp on the ring COPY only (never on the caller's
			 * `pcm`, which is `const` and may still be read afterwards by
			 * the caller, e.g. a2dp-codec-aptx.c's heartbeat quiet-gate peak
			 * detection on the pre-fade signal -- unaffected by design).
			 * Byte-at-a-time memcpy in/out of a local int16_t, not a direct
			 * cast of the uint8_t ring slot: `in_pcm` is only byte-aligned,
			 * not guaranteed int16_t-aligned. Bounded to at most
			 * M4_AH_FADE_IN_FRAMES_DEFAULT (240) frame-pairs total across the
			 * lifetime of this bridge, i.e. at most one call to this block
			 * ever does meaningful work; cost is negligible next to the
			 * encode() RT budget . */
			uint8_t *dst = b->in_pcm[slot];
			uint32_t nframes = (uint32_t)(bytes / (M4_AH_PCM_CHANNELS * M4_AH_PCM_BYTES_PER_SAMPLE));
			for (uint32_t i = 0; i < nframes && b->fade_in_frames_left > 0;
					i++, b->fade_in_frames_left--) {
				double gain = 1.0 - (double)b->fade_in_frames_left / (double)b->fade_in_frames_total;
				for (uint32_t ch = 0; ch < M4_AH_PCM_CHANNELS; ch++) {
					size_t off = (size_t)(i * M4_AH_PCM_CHANNELS + ch) * M4_AH_PCM_BYTES_PER_SAMPLE;
					int16_t v;
					memcpy(&v, dst + off, sizeof(v));
					v = (int16_t)((double)v * gain);
					memcpy(dst + off, &v, sizeof(v));
				}
			}
		}
		b->in_tail = (slot + 1) % b->ring_slots;
	}
	b->in_count++;
	pthread_mutex_unlock(&b->in_lock);
	atomic_fetch_add(&b->stat_submitted, 1);

	if (b->wake_efd >= 0) {
		uint64_t one = 1;
		ssize_t r = write(b->wake_efd, &one, sizeof(one));
		/* EAGAIN (counter would overflow) is fine: the IO thread is
		 * already going to wake up and will see in_count > 0 regardless. */
		(void)r;
	}
	return true;
}

size_t m4_ah_bridge_take_frame(struct m4_ah_bridge *b, uint8_t *dst, size_t dst_cap)
{
	size_t len = 0;
	double now;

	if (b == NULL || dst == NULL)
		return 0;

	if (pthread_mutex_trylock(&b->out_lock) != 0)
		return 0;

	now = now_ms();

	{
		unsigned cur_hwm = atomic_load(&b->stat_out_ring_hwm);
		if (b->out_count > cur_hwm)
			atomic_store(&b->stat_out_ring_hwm, b->out_count);
	}

	/* "Latency drift in the output ring" (confirmed live: ring_out observed
	 * stepping 1 -> 3 -> 4 across a session and never coming back down --
	 * see M4_AH_RING_TARGET_FRAMES_DEFAULT in m4-adaptive-bridge.h for the full
	 * story). Drop the oldest excess frame(s) BEFORE deciding what to hand
	 * back to the caller this call: take_frame() has always removed exactly
	 * one frame per call, matched 1:1 with submit_pcm(), so with no trimming
	 * ANY backlog left behind by an earlier stall/late/restart burst is
	 * permanent. Bounded to M4_AH_RING_SLOTS_MAX iterations worst case, no
	 * memcpy of the dropped frame bodies -- cheap, RT-safe.
	 *
	 * Gating is entirely RING-LEVEL (how long has out_count been above
	 * target, via out_over_since_ms), never based on how far above target it
	 * currently is: test/ring_burst_test.c requires a burst of up to
	 * M4_AH_RING_SLOTS_MAX frames (media-sink.c pre-encoding several blocks
	 * within one graph cycle, see the file header) to be delivered
	 * losslessly, regardless of peak depth, as long as it drains promptly --
	 * an "instant trim once above N" shortcut would wrongly discard exactly
	 * that legitimate burst before it has a chance to drain via ordinary
	 * one-frame-per-call delivery (below), which is what actually happens in
	 * well under M4_AH_RING_TRIM_SUSTAIN_MS_DEFAULT for a real burst. */
	{
		unsigned trimmed = 0;
		bool sustained, hard_cap;

		if (b->out_count > b->ring_target_frames) {
			if (b->out_over_since_ms <= 0.0)
				b->out_over_since_ms = now;
		} else {
			b->out_over_since_ms = 0.0;
		}
		sustained = b->out_over_since_ms > 0.0 &&
			(now - b->out_over_since_ms) >= b->ring_trim_sustain_ms;
		hard_cap = b->out_over_since_ms > 0.0 &&
			(now - b->out_over_since_ms) >= b->ring_trim_hard_cap_ms;

		while (b->out_count > b->ring_target_frames && trimmed < b->ring_slots) {
			unsigned head = b->out_head;
			bool ok_to_drop_now = hard_cap || (sustained && b->out_silent[head]);
			if (!ok_to_drop_now)
				break;
			b->out_head = (head + 1) % b->ring_slots;
			b->out_count--;
			trimmed++;
		}
		if (trimmed > 0) {
			atomic_fetch_add(&b->stat_output_ring_trimmed, trimmed);
			if (b->out_count <= b->ring_target_frames)
				b->out_over_since_ms = 0.0;
		}
	}

	if (b->out_count > 0 && b->out_len[b->out_head] <= dst_cap) {
		memcpy(dst, b->out_frame[b->out_head], b->out_len[b->out_head]);
		len = b->out_len[b->out_head];
		b->out_head = (b->out_head + 1) % b->ring_slots;
		b->out_count--;
	} else if (b->out_count > 0) {
		/* Would not fit -- drop it so we do not keep re-offering an
		 * unusable frame forever; should not happen given dst_cap is
		 * sized from the A2DP MTU. */
		b->out_head = (b->out_head + 1) % b->ring_slots;
		b->out_count--;
	} else if (atomic_load(&b->alive)) {
		atomic_fetch_add(&b->stat_output_starved, 1);
	}
	pthread_mutex_unlock(&b->out_lock);
	return len;
}

void m4_ah_bridge_get_stats(struct m4_ah_bridge *b, struct m4_ah_bridge_stats *out)
{
	if (out == NULL)
		return;
	memset(out, 0, sizeof(*out));
	if (b == NULL)
		return;

	out->submitted = atomic_load(&b->stat_submitted);
	out->input_ring_full_drops = atomic_load(&b->stat_input_ring_full_drops);
	out->frames_produced = atomic_load(&b->stat_frames_produced);
	out->output_starved = atomic_load(&b->stat_output_starved);
	out->output_ring_full_drops = atomic_load(&b->stat_output_ring_full_drops);
	out->output_ring_trimmed_frames = atomic_load(&b->stat_output_ring_trimmed);
	out->input_ring_trimmed_ms = atomic_load(&b->stat_input_ring_trimmed_blocks) * M4_AH_PCM_BLOCK_MS;
	/* Raw (never reset by this call) running high-water mark -- see the
	 * struct field doc comment in m4-adaptive-bridge.h. The JSON stats file's
	 * "out_ring_max_window"/"in_ring_max_window" read-and-reset the same two
	 * atomics once per JSON window instead (maybe_write_stats()); this call
	 * (used by a2dp-codec-aptx.c's own periodic journal log) intentionally
	 * does not race with that reset. */
	out->out_ring_max_window = atomic_load(&b->stat_out_ring_hwm);
	out->in_ring_max_window = atomic_load(&b->stat_in_ring_hwm);

	/* Best-effort point-in-time occupancy: skip (leave at 0) rather than
	 * block a caller that must stay RT-safe (codec_encode_adaptive()). */
	if (pthread_mutex_trylock(&b->in_lock) == 0) {
		out->in_ring_count = b->in_count;
		pthread_mutex_unlock(&b->in_lock);
	}
	if (pthread_mutex_trylock(&b->out_lock) == 0) {
		out->out_ring_count = b->out_count;
		pthread_mutex_unlock(&b->out_lock);
	}

	/* "RT for the bridge's IO thread": set once by apply_io_thread_rt(), see the
	 * struct field comments (m4-adaptive-bridge.c/.h) for why this is safe
	 * to read without a lock (single writer, atomics). */
	out->bridge_rt_policy = atomic_load(&b->io_rt_policy);
	out->bridge_rt_prio = atomic_load(&b->io_rt_prio);
}

/* Metrics: best-effort, single-level `mkdir()` of `path`'s
 * parent directory (e.g. "$XDG_RUNTIME_DIR/momentum4" for
 * ".../momentum4/adaptive-stats.json") -- covers the SPAWN-mode/no-socket-
 * unit case where nothing else has created that directory yet (in SOCKET
 * mode, systemd's `RuntimeDirectory=momentum4` on momentum4-helper.socket
 * already creates it). Deliberately NOT a full recursive "mkdir -p":
 * $XDG_RUNTIME_DIR itself is always created by the session manager
 * (pam_systemd) long before this code ever runs, so one level is enough for
 * every path this file actually constructs. EEXIST and any other error are
 * silently ignored (best-effort only -- the subsequent fopen() failing is
 * what actually gets logged, once, by the caller). */
static void ensure_parent_dir(const char *path)
{
	char dir[512];
	char *slash;

	snprintf(dir, sizeof(dir), "%s", path);
	slash = strrchr(dir, '/');
	if (slash == NULL || slash == dir)
		return;
	*slash = '\0';
	mkdir(dir, 0700);
}

/* Renders the current snapshot as one compact JSON object (no
 * trailing newline in `buf` itself -- write_stats_json() and
 * append_stats_history() each add whatever line/whitespace convention suits
 * their own file). Not RT-safe; only ever called from the IO thread via
 * maybe_write_stats(). Field choices, for the next reader:
 *  - "helper_late" is the bridge's own, unconditional count of
 *    take_frame() finding the output ring empty while alive (see
 *    m4_ah_bridge_take_frame()'s stat_output_starved) -- the most literal
 *    reading of "helper lateness".
 *  - "heartbeat_late_triggers"/"stall_events"/"heartbeat_events" are the
 *    RT-thread-forwarded counters from a2dp-codec-aptx.c's own heartbeat
 *    logic (m4_ah_bridge_note_late/note_stall/note_heartbeat_event) -- these
 *    can lag "helper_late" slightly (a ~200 ms warmup grace period after
 *    init(), see [adaptive] warmup-blocks / struct impl_adaptive's own
 *    warmup_blocks field in a2dp-codec-aptx.c) and only
 *    exist at all when a bridge is actually driven by that codec (this
 *    file's own tests, which drive the bridge directly, will show these as
 *    always 0 -- that is correct, not a bug). */
/* "RT for the bridge's IO thread": raw <sched.h> SCHED_* value -> human string for
 * the JSON stats file (bridge_rt_policy). Covers every policy Linux
 * actually defines, not just the two this codec ever requests (SCHED_OTHER
 * default, SCHED_FIFO if io_thread_rt_priority > 0) -- pthread_getschedparam()
 * in apply_io_thread_rt() reports whatever is ACTUALLY in effect, which in
 * principle could be something else entirely (e.g. an operator's own cgroup/
 * chrt wrapper around the whole WirePlumber process). "unknown" is only
 * reachable if a future kernel adds a policy value this function does not
 * yet recognize -- never a sign of an internal error. */
static const char *sched_policy_name(int policy)
{
	switch (policy) {
	case SCHED_OTHER: return "other";
	case SCHED_FIFO: return "fifo";
	case SCHED_RR: return "rr";
#ifdef SCHED_BATCH
	case SCHED_BATCH: return "batch";
#endif
#ifdef SCHED_IDLE
	case SCHED_IDLE: return "idle";
#endif
#ifdef SCHED_DEADLINE
	case SCHED_DEADLINE: return "deadline";
#endif
	default: return "unknown";
	}
}

static int build_stats_json(struct m4_ah_bridge *b, double now, char *buf, size_t buf_cap)
{
	unsigned in_count = 0, out_count = 0;
	bool have_in = false, have_out = false;
	char in_val[16], out_val[16];
	struct timespec rt;
	long long ts_unix_ms;

	if (pthread_mutex_trylock(&b->in_lock) == 0) {
		in_count = b->in_count;
		have_in = true;
		pthread_mutex_unlock(&b->in_lock);
	}
	if (pthread_mutex_trylock(&b->out_lock) == 0) {
		out_count = b->out_count;
		have_out = true;
		pthread_mutex_unlock(&b->out_lock);
	}
	if (have_in)
		snprintf(in_val, sizeof(in_val), "%u", in_count);
	else
		snprintf(in_val, sizeof(in_val), "null");
	if (have_out)
		snprintf(out_val, sizeof(out_val), "%u", out_count);
	else
		snprintf(out_val, sizeof(out_val), "null");

	clock_gettime(CLOCK_REALTIME, &rt);
	ts_unix_ms = (long long)rt.tv_sec * 1000 + rt.tv_nsec / 1000000;

	return snprintf(buf, buf_cap,
			"{"
			"\"ts_unix_ms\":%lld,"
			"\"uptime_ms\":%.0f,"
			"\"alive\":%s,"
			"\"mode\":\"%s\","
			"\"latency_adjust_ms\":%d,"
			"\"heartbeat_enabled\":%s,"
			"\"resync_period_min\":%u,"
			"\"encode_calls\":%u,"
			"\"frames_produced\":%u,"
			"\"input_drops_ring_full\":%u,"
			"\"input_drops_trylock\":%u,"
			"\"helper_late\":%u,"
			"\"heartbeat_late_triggers\":%u,"
			"\"stall_events\":%u,"
			"\"heartbeat_events\":%u,"
			"\"output_ring_full_drops\":%u,"
			"\"helper_restarts\":%u,"
			"\"ring_in\":%s,"
			"\"ring_in_cap\":%u,"
			"\"ring_out\":%s,"
			"\"ring_out_cap\":%u,"
			"\"out_ring_trimmed_frames\":%u,"
			"\"in_ring_trimmed_ms\":%u,"
			"\"out_ring_max_window\":%u,"
			"\"in_ring_max_window\":%u,"
			"\"bridge_rt_policy\":\"%s\","
			"\"bridge_rt_prio\":%d"
			"}",
			ts_unix_ms,
			now - b->start_ms,
			atomic_load(&b->alive) ? "true" : "false",
			b->meta_mode,
			b->meta_latency_adjust_ms,
			b->meta_heartbeat_enabled ? "true" : "false",
			(unsigned)b->meta_resync_period_min,
			atomic_load(&b->stat_encode_calls),
			atomic_load(&b->stat_frames_produced),
			atomic_load(&b->stat_input_ring_full_drops),
			atomic_load(&b->stat_input_trylock_drops),
			atomic_load(&b->stat_output_starved),
			atomic_load(&b->stat_late),
			atomic_load(&b->stat_stall),
			atomic_load(&b->stat_heartbeat_events),
			atomic_load(&b->stat_output_ring_full_drops),
			atomic_load(&b->restart_count),
			in_val, (unsigned)b->ring_slots,
			out_val, (unsigned)b->ring_slots,
			/* "Latency drift in the output ring": lifetime
			 * trim counters + this window's high-water marks -- see
			 * maybe_write_stats() for the once-per-window read-and-reset of
			 * the latter two. */
			atomic_load(&b->stat_output_ring_trimmed),
			atomic_load(&b->stat_input_ring_trimmed_blocks) * M4_AH_PCM_BLOCK_MS,
			b->last_window_out_ring_hwm,
			b->last_window_in_ring_hwm,
			sched_policy_name(atomic_load(&b->io_rt_policy)),
			atomic_load(&b->io_rt_prio));
}

/* Atomic write (an explicit requirement) -- write to
 * "<stats_path>.tmp", then rename() over stats_path. rename(2) within the
 * same filesystem (both paths are always in the same directory here) is
 * atomic: a concurrent reader (`momentum4 stats`) either sees the
 * old file or the fully-written new one, never a partial write. Logs at
 * most once per distinct failure reason (via `warned_*` static-in-struct-
 * would need extra state; kept simple here -- best-effort, silent on error,
 * since this is diagnostics-only and must never affect the audio path). */
static void write_stats_json(struct m4_ah_bridge *b, double now)
{
	char json[768];
	char tmp_path[560];
	int fd;
	FILE *f;

	if (b->stats_path == NULL)
		return;

	build_stats_json(b, now, json, sizeof(json));

	snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", b->stats_path);
	fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0) {
		ensure_parent_dir(b->stats_path);
		fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
		if (fd < 0)
			return; /* best-effort; not fatal to the audio path */
	}
	f = fdopen(fd, "w");
	if (f == NULL) {
		close(fd);
		return;
	}
	fputs(json, f);
	fputc('\n', f);
	fclose(f);
	rename(tmp_path, b->stats_path);
}

/* "+ short-history rotation": appends one compact JSON line to
 * stats_history_path (JSON Lines format -- one object per line, easy to
 * `tail -f | jq` or parse incrementally), then every
 * M4_AH_STATS_HISTORY_ROTATE_EVERY_DEFAULT writes, trims the file down to the last
 * M4_AH_STATS_HISTORY_MAX_LINES_DEFAULT lines. The trim itself reads the whole file
 * into memory and rewrites it via the same tmp+rename pattern as
 * write_stats_json() -- bounded and cheap (at most a few hundred KB, see
 * M4_AH_STATS_HISTORY_MAX_LINES_DEFAULT in the header), and only happens once every
 * few minutes, never on the RT path. M4_AH_STATS_HISTORY_ROTATE_EVERY_DEFAULT
 * itself is defined near the top of this file (with the other *_DEFAULT
 * constants) since m4_ah_bridge_start() now also needs it, earlier in
 * this file than this comment. */

static void rotate_stats_history(struct m4_ah_bridge *b)
{
	FILE *in;
	char *data = NULL;
	size_t cap = 0, len = 0;
	size_t n_lines = 0;
	size_t i;
	char tmp_path[560];
	FILE *out;

	in = fopen(b->stats_history_path, "r");
	if (in == NULL)
		return;
	for (;;) {
		char chunk[4096];
		size_t r = fread(chunk, 1, sizeof(chunk), in);
		if (r == 0)
			break;
		if (len + r > cap) {
			size_t new_cap = cap * 2;
			if (new_cap < len + r)
				new_cap = len + r;
			char *nd = realloc(data, new_cap);
			if (nd == NULL) {
				free(data);
				fclose(in);
				return; /* best-effort: skip this rotation, try again later */
			}
			data = nd;
			cap = new_cap;
		}
		memcpy(data + len, chunk, r);
		len += r;
	}
	fclose(in);
	if (data == NULL)
		return;

	for (i = 0; i < len; i++)
		if (data[i] == '\n')
			n_lines++;
	if (n_lines <= b->stats_history_lines) {
		free(data);
		return; /* nothing to trim yet */
	}

	/* Find the start offset of the (n_lines - MAX_LINES)'th line from the
	 * start -- i.e. skip that many newlines, keep everything after. */
	size_t skip_lines = n_lines - b->stats_history_lines;
	size_t off = 0, seen = 0;
	for (i = 0; i < len && seen < skip_lines; i++) {
		if (data[i] == '\n') {
			seen++;
			off = i + 1;
		}
	}

	snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", b->stats_history_path);
	out = fopen(tmp_path, "w");
	if (out != NULL) {
		fwrite(data + off, 1, len - off, out);
		fclose(out);
		rename(tmp_path, b->stats_history_path);
	}
	free(data);
}

static void append_stats_history(struct m4_ah_bridge *b, double now)
{
	char json[768];
	FILE *f;

	if (b->stats_history_path == NULL)
		return;

	build_stats_json(b, now, json, sizeof(json));

	f = fopen(b->stats_history_path, "a");
	if (f == NULL) {
		ensure_parent_dir(b->stats_history_path);
		f = fopen(b->stats_history_path, "a");
		if (f == NULL)
			return; /* best-effort */
	}
	fputs(json, f);
	fputc('\n', f);
	fclose(f);

	if (++b->stats_history_writes >= b->stats_history_rotate_every) {
		b->stats_history_writes = 0;
		rotate_stats_history(b);
	}
}

/* The single entry point both of io_thread_main()'s loops call
 * every iteration (see the two call sites and their comments). Cheap when
 * not due (one subtraction + comparison); IO-thread-private state
 * (last_stats_write_ms/stats_history_writes), no locking needed for that
 * bookkeeping itself (only build_stats_json()'s trylock on the rings, same
 * as m4_ah_bridge_get_stats()). No-op entirely if stats_path is NULL
 * (disabled, the default). */
static void maybe_write_stats(struct m4_ah_bridge *b, double now)
{
	if (b == NULL || b->stats_path == NULL)
		return;
	if (now - b->last_stats_write_ms < (double)b->stats_interval_ms)
		return;
	b->last_stats_write_ms = now;

	/* "max out-ring over the window": read-and-reset the running
	 * high-water-mark atomics into these IO-thread-private fields exactly
	 * ONCE per window, before either build_stats_json() call below (one for
	 * the current-snapshot file, one for the history line) reads them --
	 * both must report the identical number for this window, not two
	 * different partial maxima from racing to reset the same atomic twice. */
	b->last_window_out_ring_hwm = atomic_exchange(&b->stat_out_ring_hwm, 0);
	b->last_window_in_ring_hwm = atomic_exchange(&b->stat_in_ring_hwm, 0);

	write_stats_json(b, now);
	append_stats_history(b, now);
}

void m4_ah_bridge_note_encode_call(struct m4_ah_bridge *b)
{
	if (b) atomic_fetch_add(&b->stat_encode_calls, 1);
}

void m4_ah_bridge_note_late(struct m4_ah_bridge *b)
{
	if (b) atomic_fetch_add(&b->stat_late, 1);
}

void m4_ah_bridge_note_stall(struct m4_ah_bridge *b)
{
	if (b) atomic_fetch_add(&b->stat_stall, 1);
}

void m4_ah_bridge_note_heartbeat_event(struct m4_ah_bridge *b)
{
	if (b) atomic_fetch_add(&b->stat_heartbeat_events, 1);
}

void m4_ah_bridge_set_meta(struct m4_ah_bridge *b, const char *mode_str,
		int32_t latency_adjust_ms, bool heartbeat_enabled,
		uint32_t resync_period_min)
{
	if (b == NULL)
		return;
	snprintf(b->meta_mode, sizeof(b->meta_mode), "%s", mode_str ? mode_str : "?");
	b->meta_latency_adjust_ms = latency_adjust_ms;
	b->meta_heartbeat_enabled = heartbeat_enabled;
	b->meta_resync_period_min = resync_period_min;
}

bool m4_ah_bridge_is_alive(const struct m4_ah_bridge *b)
{
	if (b == NULL)
		return false;
	return atomic_load(&((struct m4_ah_bridge *)b)->alive);
}

pid_t m4_ah_bridge_test_child_pid(const struct m4_ah_bridge *b)
{
	return b ? b->child_pid : -1;
}

uint32_t m4_ah_bridge_restart_count(const struct m4_ah_bridge *b)
{
	if (b == NULL)
		return 0;
	return atomic_load(&((struct m4_ah_bridge *)b)->restart_count);
}
