/*
 * Raw Linux syscall wrappers for freestanding Hexagon (QDSP6) user code.
 *
 * There is no musl/libc sysroot for hexagon-unknown-linux-musl in this
 * environment, so this program is built fully
 * freestanding (-ffreestanding -nostdlib) and talks to the kernel directly
 * via trap0(#1). Confirmed ABI (measured against qemu-hexagon-static 7.2,
 * work/tools): syscall number in r6, args in r0..r5, return value in r0.
 * Syscall numbers are the generic Linux syscall table (asm-generic/unistd.h),
 * which arch/hexagon uses like riscv32/arc/csky/nds32.
 *
 * `long` and `void *` are both 32-bit on this target (confirmed:
 * sizeof(long)==sizeof(void*)==4 under hexagon-unknown-linux-musl clang 14).
 */
#ifndef HX_SYSCALL_H
#define HX_SYSCALL_H

typedef unsigned int hx_u32;
typedef int hx_i32;
typedef unsigned long hx_size_t;
typedef long hx_ssize_t;

#define HX_SYS_close 57
#define HX_SYS_lseek 62
#define HX_SYS_read 63
#define HX_SYS_write 64
#define HX_SYS_openat 56
#define HX_SYS_exit_group 94
#define HX_SYS_mmap 222
#define HX_SYS_fstat 80

#define HX_AT_FDCWD (-100)
#define HX_O_RDONLY 0

#define HX_PROT_READ 0x1
#define HX_PROT_WRITE 0x2
#define HX_PROT_EXEC 0x4
#define HX_MAP_PRIVATE 0x02
#define HX_MAP_ANONYMOUS 0x20

static inline long hx_syscall6(long nr, long a0, long a1, long a2, long a3,
		long a4, long a5)
{
	register long r0 asm("r0") = a0;
	register long r1 asm("r1") = a1;
	register long r2 asm("r2") = a2;
	register long r3 asm("r3") = a3;
	register long r4 asm("r4") = a4;
	register long r5 asm("r5") = a5;
	register long r6 asm("r6") = nr;

	asm volatile("trap0(#1)"
			: "+r"(r0)
			: "r"(r1), "r"(r2), "r"(r3), "r"(r4), "r"(r5), "r"(r6)
			: "memory");
	return r0;
}

static inline long hx_read(int fd, void *buf, hx_size_t count)
{
	return hx_syscall6(HX_SYS_read, fd, (long)buf, (long)count, 0, 0, 0);
}

static inline long hx_write(int fd, const void *buf, hx_size_t count)
{
	return hx_syscall6(HX_SYS_write, fd, (long)buf, (long)count, 0, 0, 0);
}

static inline long hx_openat(const char *path, long flags, long mode)
{
	return hx_syscall6(HX_SYS_openat, HX_AT_FDCWD, (long)path, flags,
			mode, 0, 0);
}

static inline long hx_close(int fd)
{
	return hx_syscall6(HX_SYS_close, fd, 0, 0, 0, 0, 0);
}

static inline long hx_lseek(int fd, long offset, int whence)
{
	return hx_syscall6(HX_SYS_lseek, fd, offset, whence, 0, 0, 0);
}

static inline void *hx_mmap_anon(hx_size_t length, long prot)
{
	long r = hx_syscall6(HX_SYS_mmap, 0, (long)length, prot,
			HX_MAP_PRIVATE | HX_MAP_ANONYMOUS, -1, 0);
	if (r < 0 && r > -4096)
		return (void *)0;
	return (void *)r;
}

static inline void hx_exit_group(int code)
{
	hx_syscall6(HX_SYS_exit_group, code, 0, 0, 0, 0, 0);
	for (;;)
		;
}

/* Blocking full read/write helpers (stdin/stdout are pipes: short
 * reads/writes are normal and must be retried). */
static inline int hx_read_full(int fd, void *buf, hx_size_t size)
{
	unsigned char *p = (unsigned char *)buf;
	while (size > 0) {
		long n = hx_read(fd, p, size);
		if (n == 0)
			return 0; /* EOF */
		if (n < 0)
			return -1;
		p += (hx_size_t)n;
		size -= (hx_size_t)n;
	}
	return 1;
}

static inline int hx_write_full(int fd, const void *buf, hx_size_t size)
{
	const unsigned char *p = (const unsigned char *)buf;
	while (size > 0) {
		long n = hx_write(fd, p, size);
		if (n <= 0)
			return -1;
		p += (hx_size_t)n;
		size -= (hx_size_t)n;
	}
	return 0;
}

#endif
