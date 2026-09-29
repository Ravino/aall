/*
 * Stub implementations for the 21 imports of capi_v2_aptX_CLHDADV_Encoder.so
 *, plus the two data objects resolved via
 * R_HEX_GLOB_DAT (HAP_debug_v2, _Stdout). Freestanding: no libc underneath.
 *
 * Design notes:
 *  - memcpy/memset/memmove/__hexagon_memcpy_likely_aligned_* are exact.
 *  - __hexagon_{u,}{div,mod}si3: exact 32-bit restoring binary division.
 *  - __hexagon_{div,mul}df3, __hexagon_divsf3: soft float, REDUCED PRECISION
 *    (~24-bit mantissa kept, the rest of the 52-bit double mantissa is
 *    zero-filled). This is a deliberate simplification: G2 measures timing
 *    and structural stability (frame sizes, crash-freedom), not bit-exact
 *    audio, and Hexagon V66 has native hardware for sf add/sub/mul/compare
 *    (seen as `sfadd`/`sfmpy`/`sfcmp` in the disassembly) -- only *division*
 *    and all *double* arithmetic go through these software routines, and
 *    only at initialisation / table-generation time as far as we observed
 *    in capi_v2_aptx_scalable_enc_init() and _process(). If a future pass
 *    needs bit-exact output, replace these with a real IEEE-754 soft-float
 *    implementation (e.g. port compiler-rt's fp_div_impl.inc).
 *  - HAP_debug/HAP_debug_v2/printf/vsnprintf: minimal formatter, logged to
 *    fd 2 (stderr) so the host can see what the module is doing during
 *    bring-up; never trusts arg counts beyond what each ABI guarantees.
 *  - qurt_elite_memory_malloc/free: bump allocator with a tiny free-list
 *    over a static arena (no brk/mmap growth -- keeps the loader simple).
 */
#include "capi_v2.h"
#include "elfload.h"
#include "hx_string.h"
#include "hx_syscall.h"

#include <stdarg.h>
#include <stdint.h>

/* ---------------- logging ---------------- */

static void hx_log_str(const char *s)
{
	hx_write_full(2, s, hx_strlen(s));
}

static void hx_log_uint_hex(unsigned int v)
{
	char buf[10];
	for (int i = 7; i >= 0; i--) {
		int nib = (v >> (i * 4)) & 0xf;
		buf[7 - i] = nib < 10 ? (char)('0' + nib) : (char)('a' + nib - 10);
	}
	buf[8] = '\0';
	hx_log_str(buf);
}

/* ---------------- mem* ---------------- */

void *memcpy(void *dst, const void *src, hx_size_t n) { return hx_memcpy(dst, src, n); }
void *memset(void *dst, int c, hx_size_t n) { return hx_memset(dst, c, n); }
void *memmove(void *dst, const void *src, hx_size_t n) { return hx_memmove(dst, src, n); }
void *__hexagon_memcpy_likely_aligned_min32bytes_mult8bytes(void *dst,
		const void *src, hx_size_t n)
{
	return hx_memcpy(dst, src, n);
}

/* ---------------- integer div/mod ---------------- */

static unsigned int u32_divmod(unsigned int num, unsigned int den, unsigned int *rem_out)
{
	unsigned int quot = 0, rem = 0;

	if (den == 0) {
		if (rem_out)
			*rem_out = 0;
		return 0;
	}
	for (int i = 31; i >= 0; i--) {
		rem = (rem << 1) | ((num >> i) & 1u);
		if (rem >= den) {
			rem -= den;
			quot |= (1u << i);
		}
	}
	if (rem_out)
		*rem_out = rem;
	return quot;
}

unsigned int __hexagon_udivsi3(unsigned int a, unsigned int b)
{
	return u32_divmod(a, b, 0);
}

unsigned int __hexagon_umodsi3(unsigned int a, unsigned int b)
{
	unsigned int r;
	u32_divmod(a, b, &r);
	return r;
}

int __hexagon_divsi3(int a, int b)
{
	int neg = (a < 0) ^ (b < 0);
	unsigned int ua = a < 0 ? (unsigned int)(-a) : (unsigned int)a;
	unsigned int ub = b < 0 ? (unsigned int)(-b) : (unsigned int)b;
	unsigned int q = u32_divmod(ua, ub, 0);
	return neg ? -(int)q : (int)q;
}

int __hexagon_modsi3(int a, int b)
{
	int neg = (a < 0);
	unsigned int ua = a < 0 ? (unsigned int)(-a) : (unsigned int)a;
	unsigned int ub = b < 0 ? (unsigned int)(-b) : (unsigned int)b;
	unsigned int r;
	u32_divmod(ua, ub, &r);
	return neg ? -(int)r : (int)r;
}

/* soft-float + _Sin moved to softfloat.c (exact) */

/* ---------------- tiny printf/vsnprintf ---------------- */

static int u_to_buf(char *out, unsigned int v, int base, int upper)
{
	static const char *digits_lo = "0123456789abcdef";
	static const char *digits_hi = "0123456789ABCDEF";
	const char *digits = upper ? digits_hi : digits_lo;
	char tmp[32];
	int n = 0;

	if (v == 0) {
		out[0] = '0';
		return 1;
	}
	while (v != 0) {
		tmp[n++] = digits[v % (unsigned)base];
		v /= (unsigned)base;
	}
	for (int i = 0; i < n; i++)
		out[i] = tmp[n - 1 - i];
	return n;
}

int vsnprintf(char *buf, hx_size_t size, const char *fmt, va_list ap)
{
	hx_size_t pos = 0;
#define PUT(c) do { if (pos + 1 < size) buf[pos] = (char)(c); pos++; } while (0)

	for (const char *p = fmt; *p; p++) {
		if (*p != '%') {
			PUT(*p);
			continue;
		}
		p++;
		if (*p == '\0')
			break;
		char tmp[32];
		int n;
		switch (*p) {
		case 'd': {
			int v = va_arg(ap, int);
			unsigned int u = v < 0 ? (unsigned int)(-v) : (unsigned int)v;
			if (v < 0)
				PUT('-');
			n = u_to_buf(tmp, u, 10, 0);
			for (int i = 0; i < n; i++)
				PUT(tmp[i]);
			break;
		}
		case 'u': {
			n = u_to_buf(tmp, va_arg(ap, unsigned int), 10, 0);
			for (int i = 0; i < n; i++)
				PUT(tmp[i]);
			break;
		}
		case 'x':
		case 'X': {
			n = u_to_buf(tmp, va_arg(ap, unsigned int), 16, *p == 'X');
			for (int i = 0; i < n; i++)
				PUT(tmp[i]);
			break;
		}
		case 'c':
			PUT((char)va_arg(ap, int));
			break;
		case 's': {
			const char *s = va_arg(ap, const char *);
			if (!s)
				s = "(null)";
			while (*s)
				PUT(*s++);
			break;
		}
		case '%':
			PUT('%');
			break;
		default:
			PUT('%');
			PUT(*p);
			break;
		}
	}
	if (size > 0)
		buf[pos < size ? pos : size - 1] = '\0';
#undef PUT
	return (int)pos;
}

int printf(const char *fmt, ...)
{
	char buf[256];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	/* fd 2, never fd 1: stdout carries the host<->guest protocol */
	hx_write_full(2, buf, (hx_size_t)(n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1));
	return n;
}

int fflush(void *stream)
{
	(void)stream;
	return 0;
}

static int hx_snprintf(char *buf, hx_size_t size, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf, size, fmt, ap);
	va_end(ap);
	return n;
}

/* ---------------- Qualcomm FARF-style logging ---------------- */

/* Set to 0 by main.c after init to silence the module's per-packet chatter
 * (it logs ~6 lines per process() call). */
int hx_module_log_enabled = 1;

void HAP_debug(unsigned int level, const char *file, unsigned int line,
		const char *fmt, ...)
{
	/* True varargs: never consume them (unknown count/types), just log the
	 * raw format string -- see file header. */
	(void)level;
	if (!hx_module_log_enabled)
		return;
	char buf[256];
	int n = hx_snprintf(buf, sizeof(buf), "HAP_debug %s:%u: ", file, line);
	hx_write_full(2, buf, (hx_size_t)n);
	hx_log_str(fmt ? fmt : "(null)");
	hx_log_str("\n");
}

/*
 * The module is built for the QuRT/standalone Hexagon ABI, where ALL
 * variadic arguments go on the stack (confirmed at every call site, e.g.
 * 0x11d58 `memw(r29+#0) = r17` before `call HAP_debug_v2`), whereas r4/r5
 * are garbage. The two dummy register params below absorb r4/r5 so that
 * `a0` is the first stack slot; the remaining varargs follow contiguously.
 * %f / %lld consume an 8-byte aligned slot and are printed as hex.
 */
void HAP_debug_v2(unsigned int level, const char *file, unsigned int line,
		const char *fmt, unsigned int r4_unused, unsigned int r5_unused,
		unsigned int a0, unsigned int a1, unsigned int a2,
		unsigned int a3, unsigned int a4, unsigned int a5)
{
	(void)level; (void)r4_unused; (void)r5_unused;
	(void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
	if (!hx_module_log_enabled)
		return;
	const unsigned int *args = &a0; /* incoming stack argument area */
	unsigned int argi = 0;
	char buf[256];
	int n = hx_snprintf(buf, sizeof(buf), "HAP_debug_v2 %s:%u: ", file, line);
	hx_write_full(2, buf, (hx_size_t)n);

	if (!fmt) {
		hx_log_str("(null)\n");
		return;
	}
	for (const char *p = fmt; *p; p++) {
		if (*p != '%' || argi >= 8) {
			char c[2] = { *p, 0 };
			hx_log_str(c);
			continue;
		}
		p++;
		int is_ll = 0;
		while (*p == 'l') { is_ll++; p++; }
		if (*p == 'f' || is_ll >= 2) {
			argi = (argi + 1) & ~1u;
			char tmp[24];
			int nn = u_to_buf(tmp, args[argi + 1], 16, 0);
			tmp[nn] = 0;
			hx_log_str("0x"); hx_log_str(tmp); hx_log_str(":");
			nn = u_to_buf(tmp, args[argi], 16, 0);
			tmp[nn] = 0;
			hx_log_str(tmp);
			argi += 2;
		} else if (*p == 'd' || *p == 'i') {
			char tmp[16];
			int v = (int)args[argi++];
			if (v < 0) { hx_log_str("-"); v = -v; }
			int nn = u_to_buf(tmp, (unsigned int)v, 10, 0);
			tmp[nn] = 0;
			hx_log_str(tmp);
		} else if (*p == 'u' || *p == 'x' || *p == 'X') {
			char tmp[16];
			int nn = u_to_buf(tmp, args[argi++], *p == 'u' ? 10 : 16, 0);
			tmp[nn] = 0;
			hx_log_str(tmp);
		} else if (*p == 's') {
			const char *sp = (const char *)(uintptr_t)args[argi++];
			hx_log_str(sp ? sp : "(null)");
		} else if (*p == '%') {
			hx_log_str("%");
		} else {
			char c[3] = { '%', *p, 0 };
			hx_log_str(c);
			if (!*p)
				break;
		}
	}
	hx_log_str("\n");
}

/* ---------------- misc no-ops ---------------- */

void __register_frame_info_bases(const void *a, void *b, void *c, void *d)
{
	(void)a; (void)b; (void)c; (void)d;
}

void __cxa_finalize(void *d)
{
	(void)d;
}

static unsigned char hx_stdout_dummy[64];
void *hx_stdout_object(void) { return hx_stdout_dummy; }

/* ---------------- qurt_elite_memory_{malloc,free} ---------------- */

#define ARENA_SIZE (4u * 1024u * 1024u)
static unsigned char arena[ARENA_SIZE] __attribute__((aligned(16)));
static hx_size_t arena_used;

struct alloc_hdr {
	uint32_t size;
	uint32_t magic;
};
#define ALLOC_MAGIC 0x41504d31u /* "APM1" */

void *qurt_elite_memory_malloc(unsigned int size, unsigned int heap_id)
{
	(void)heap_id;
	hx_size_t total = ((hx_size_t)size + sizeof(struct alloc_hdr) + 15u) & ~(hx_size_t)15u;
	if (arena_used + total > ARENA_SIZE) {
		hx_log_str("qurt_elite_memory_malloc: arena exhausted, size=");
		hx_log_uint_hex(size);
		hx_log_str("\n");
		return 0;
	}
	struct alloc_hdr *h = (struct alloc_hdr *)(arena + arena_used);
	h->size = size;
	h->magic = ALLOC_MAGIC;
	arena_used += total;
	return (void *)(h + 1);
}

void qurt_elite_memory_free(void *ptr)
{
	/* Bump allocator: no reclamation. This is fine for a bounded-duration
	 * benchmark harness (see ARENA_SIZE) and keeps the loader simple; a
	 * production helper would want a real free list here. */
	(void)ptr;
}

/* ---------------- unresolved-import trap ---------------- */

void hx_unresolved_import_trap(void)
{
	hx_log_str("FATAL: call to unresolved import\n");
	hx_exit_group(97);
}
