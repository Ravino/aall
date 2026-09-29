/* momentum4:
 * offline p50/p99 round-trip latency comparison between the two IPC
 * transports m4-adaptive-bridge.c can use for one 10 ms audio block:
 *
 * PIPE: two anonymous pipes (MODE == SPAWN
 *           fork+exec design) -- parent writes on pipe A, child echoes back
 *           on pipe B.
 *   SOCKET: one AF_UNIX SOCK_STREAM socketpair() (MODE == SOCKET's
 *           steady-state shape once a connection is already established --
 *           a single fd, both directions) -- parent writes, child echoes
 *           back on the same fd.
 *
 * Both children do the exact same "read length-prefixed payload, write back
 * a small fixed reply" work with the same buffer sizes as the real protocol
 * (m4-adaptive-protocol.h: PCM_BLOCK_BYTES out, an 8-byte {status,
 * frame_bytes} header + a small frame back), so the comparison isolates the
 * TRANSPORT'S own kernel-level cost (context switch + copy_to/from_user +
 * scheduler wakeup latency) from everything else in the real bridge
 * (mailboxes, IO thread, qemu-hexagon itself) -- this deliberately does NOT
 * use m4-adaptive-bridge.c/qemu-hexagon at all, so it can run in well under
 * a second and give a clean apples-to-apples number for the one variable
 * that actually changes between the two modes.
 *
 * Cadence: iterations are paced at a fixed 10 ms period (the real audio
 * block period, M4_AH_PCM_SAMPLES_PER_BLOCK/48000) via a monotonic
 * deadline, not back-to-back -- this matters because a round trip issued
 * right after the kernel has gone idle (the realistic steady-state case)
 * can differ from a tight busy-loop's, and 10 ms is long enough that the
 * scheduler will have let both processes go fully idle between iterations
 * on a lightly loaded machine.
 *
 * Usage: latency_bench_pipe_vs_socket [n_iterations, default 500]
 * Prints p50/p90/p99/p99.9/max for each transport in milliseconds. No
 * pass/fail assertions -- this is a measurement tool,
 * not a correctness test (see socket_bridge_test.c for that).
 */
#define _GNU_SOURCE
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define PCM_BLOCK_BYTES 1920u /* M4_AH_PCM_BLOCK_BYTES, kept as a literal so
                                * this file has zero dependency on
                                * m4-adaptive-bridge.h -- see file header. */
#define PERIOD_MS 10.0

static double now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static int write_all(int fd, const void *buf, size_t len)
{
	const uint8_t *p = buf;
	while (len > 0) {
		ssize_t n = write(fd, p, len);
		if (n <= 0)
			return -1;
		p += (size_t)n;
		len -= (size_t)n;
	}
	return 0;
}

static int read_all(int fd, void *buf, size_t len)
{
	uint8_t *p = buf;
	while (len > 0) {
		ssize_t n = read(fd, p, len);
		if (n <= 0)
			return -1;
		p += (size_t)n;
		len -= (size_t)n;
	}
	return 0;
}

/* Child-side echo loop, identical for both transports: read a u32 length +
 * that many payload bytes, write back a fixed 8-byte {status=0,
 * frame_bytes=4} header + 4 bytes -- same shape as the real protocol's
 * per-block reply (m4-adaptive-protocol.h), on whichever fd(s) it is given.
 * `in_fd`/`out_fd` are equal for the socketpair case, different for the pipe
 * case -- the child does not need to know or care which. */
static void child_echo_loop(int in_fd, int out_fd)
{
	uint8_t pcm[PCM_BLOCK_BYTES];
	for (;;) {
		uint32_t len;
		if (read_all(in_fd, &len, sizeof(len)) != 0)
			_exit(0); /* EOF: parent is done */
		if (len > sizeof(pcm))
			_exit(1);
		if (read_all(in_fd, pcm, len) != 0)
			_exit(0);
		uint32_t reply[2] = { 0u, 4u };
		uint8_t frame[4] = { 0xAA, 0xBB, 0xCC, 0xDD };
		if (write_all(out_fd, reply, sizeof(reply)) != 0)
			_exit(1);
		if (write_all(out_fd, frame, sizeof(frame)) != 0)
			_exit(1);
	}
}

static int cmp_double(const void *a, const void *b)
{
	double da = *(const double *)a, db = *(const double *)b;
	return (da > db) - (da < db);
}

static void report(const char *label, double *samples, int n)
{
	qsort(samples, (size_t)n, sizeof(double), cmp_double);
	double p50 = samples[(int)(0.50 * (n - 1))];
	double p90 = samples[(int)(0.90 * (n - 1))];
	double p99 = samples[(int)(0.99 * (n - 1))];
	double p999 = samples[(int)(0.999 * (n - 1)) < n ? (int)(0.999 * (n - 1)) : n - 1];
	double max = samples[n - 1];
	double sum = 0;
	for (int i = 0; i < n; i++)
		sum += samples[i];
	printf("%-8s n=%-5d mean=%.4f ms  p50=%.4f  p90=%.4f  p99=%.4f  p99.9=%.4f  max=%.4f ms\n",
			label, n, sum / n, p50, p90, p99, p999, max);
}

/* Runs n_iter round trips at PERIOD_MS cadence over (parent_write_fd,
 * parent_read_fd) (equal for the socket case), storing each round trip's
 * wall-clock duration (submit write + full reply read) into `out_ms[i]`. */
static void run_bench(int parent_write_fd, int parent_read_fd, int n_iter, double *out_ms)
{
	uint8_t pcm[PCM_BLOCK_BYTES];
	memset(pcm, 0, sizeof(pcm));
	double next_deadline = now_ms();
	for (int i = 0; i < n_iter; i++) {
		/* Pace at a fixed period like the real audio graph would, rather
		 * than hammering the transport back-to-back -- see file header. */
		double left = next_deadline - now_ms();
		if (left > 0) {
			struct timespec ts = { .tv_sec = (time_t)(left / 1000.0),
				.tv_nsec = (long)(fmod(left, 1000.0) * 1e6) };
			nanosleep(&ts, NULL);
		}
		next_deadline += PERIOD_MS;

		double t0 = now_ms();
		uint32_t len = sizeof(pcm);
		write_all(parent_write_fd, &len, sizeof(len));
		write_all(parent_write_fd, pcm, sizeof(pcm));
		uint32_t reply[2];
		read_all(parent_read_fd, reply, sizeof(reply));
		uint8_t frame[4];
		read_all(parent_read_fd, frame, sizeof(frame));
		out_ms[i] = now_ms() - t0;
	}
}

static void bench_pipe(int n_iter, double *out_ms)
{
	int to_child[2], from_child[2];
	if (pipe(to_child) != 0 || pipe(from_child) != 0) {
		fprintf(stderr, "pipe() failed\n");
		exit(1);
	}
	pid_t pid = fork();
	if (pid < 0) {
		fprintf(stderr, "fork() failed\n");
		exit(1);
	}
	if (pid == 0) {
		close(to_child[1]);
		close(from_child[0]);
		child_echo_loop(to_child[0], from_child[1]);
		_exit(0);
	}
	close(to_child[0]);
	close(from_child[1]);
	run_bench(to_child[1], from_child[0], n_iter, out_ms);
	close(to_child[1]);
	close(from_child[0]);
	int status;
	waitpid(pid, &status, 0);
}

static void bench_socket(int n_iter, double *out_ms)
{
	int sv[2];
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
		fprintf(stderr, "socketpair() failed\n");
		exit(1);
	}
	pid_t pid = fork();
	if (pid < 0) {
		fprintf(stderr, "fork() failed\n");
		exit(1);
	}
	if (pid == 0) {
		close(sv[0]);
		child_echo_loop(sv[1], sv[1]);
		_exit(0);
	}
	close(sv[1]);
	run_bench(sv[0], sv[0], n_iter, out_ms);
	close(sv[0]);
	int status;
	waitpid(pid, &status, 0);
}

int main(int argc, char **argv)
{
	int n_iter = argc > 1 ? atoi(argv[1]) : 500;
	if (n_iter < 10)
		n_iter = 10;

	double *pipe_ms = calloc((size_t)n_iter, sizeof(double));
	double *sock_ms = calloc((size_t)n_iter, sizeof(double));
	if (!pipe_ms || !sock_ms) {
		fprintf(stderr, "out of memory\n");
		return 1;
	}

	printf("Round-trip latency, %d iterations at a %.0f ms nominal cadence "
			"(one 10 ms audio block period each), %u-byte PCM out / "
			"12-byte reply back, no qemu-hexagon involved (pure transport "
			"comparison, see file header):\n\n", n_iter, PERIOD_MS,
			(unsigned)PCM_BLOCK_BYTES);

	bench_pipe(n_iter, pipe_ms);
	report("PIPE", pipe_ms, n_iter);

	bench_socket(n_iter, sock_ms);
	report("SOCKET", sock_ms, n_iter);

	free(pipe_ms);
	free(sock_ms);
	return 0;
}
