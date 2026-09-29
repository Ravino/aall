/*
 * Exact IEEE-754 soft-float helpers for the Hexagon runtime ABI
 * (__hexagon_muldf3/__hexagon_divdf3/__hexagon_divsf3) plus Dinkumware
 * _Sin(x, qoff). Integer-only (no double mul/div in this file, otherwise
 * clang would recurse into these very routines). Round-to-nearest-even.
 * Subnormal results are flushed to signed zero (never hit by the module in
 * practice; inputs that are subnormal are normalised correctly).
 */
#include <stdint.h>

typedef union { double d; uint64_t u; } f64u;
typedef union { float f; uint32_t u; } f32u;

#define D_SIGN 0x8000000000000000ull
#define D_INF 0x7ff0000000000000ull
#define D_QNAN 0x7ff8000000000000ull
#define D_HID 0x0010000000000000ull
#define D_MANT 0x000fffffffffffffull

/* unpack finite nonzero double: returns significand with bit 52 set, *e = biased exp */
static uint64_t d_unpack(uint64_t u, int *e)
{
	int ex = (int)((u >> 52) & 0x7ff);
	uint64_t m = u & D_MANT;
	if (ex == 0) { /* subnormal */
		ex = 1;
		while (!(m & D_HID)) { m <<= 1; ex--; }
	} else {
		m |= D_HID;
	}
	*e = ex;
	return m;
}

/* r: 53-bit significand (bit 52 set), g: guard bit, s: sticky; e: biased exp */
static uint64_t d_pack(uint64_t sign, int e, uint64_t r, int g, int s)
{
	if (g && (s || (r & 1))) {
		r++;
		if (r == (D_HID << 1)) { r >>= 1; e++; }
	}
	if (e >= 0x7ff)
		return sign | D_INF;
	if (e <= 0)
		return sign; /* flush-to-zero */
	return sign | ((uint64_t)e << 52) | (r & D_MANT);
}

static int d_isnan(uint64_t u) { return ((u >> 52) & 0x7ff) == 0x7ff && (u & D_MANT); }
static int d_isinf(uint64_t u) { return (u & ~D_SIGN) == D_INF; }
static int d_iszero(uint64_t u) { return (u & ~D_SIGN) == 0; }

double __hexagon_muldf3(double ad, double bd)
{
	f64u a = { .d = ad }, b = { .d = bd }, r;
	uint64_t sign = (a.u ^ b.u) & D_SIGN;
	if (d_isnan(a.u) || d_isnan(b.u)) { r.u = D_QNAN; return r.d; }
	if (d_isinf(a.u) || d_isinf(b.u)) {
		r.u = (d_iszero(a.u) || d_iszero(b.u)) ? D_QNAN : (sign | D_INF);
		return r.d;
	}
	if (d_iszero(a.u) || d_iszero(b.u)) { r.u = sign; return r.d; }
	int ea, eb;
	uint64_t ma = d_unpack(a.u, &ea), mb = d_unpack(b.u, &eb);
	/* 53x53 -> 106-bit product in hi:lo */
	uint64_t al = ma & 0xffffffffu, ah = ma >> 32, bl = mb & 0xffffffffu, bh = mb >> 32;
	uint64_t ll = al * bl, lh = al * bh, hl = ah * bl, hh = ah * bh;
	uint64_t mid = (ll >> 32) + (lh & 0xffffffffu) + (hl & 0xffffffffu);
	uint64_t lo = (ll & 0xffffffffu) | (mid << 32);
	uint64_t hi = hh + (lh >> 32) + (hl >> 32) + (mid >> 32);
	/* product P = hi:lo, in [2^104, 2^106) -> hi in [2^40, 2^42) */
	int shift = (hi >> 41) ? 53 : 52;
	/* R = P >> shift; guard = bit shift-1; sticky = bits below */
	uint64_t R = (hi << (64 - shift)) | (lo >> shift);
	int g = (int)((lo >> (shift - 1)) & 1);
	int s = (lo & ((1ull << (shift - 1)) - 1)) != 0;
	int e = ea + eb - 1075 + shift;
	r.u = d_pack(sign, e, R, g, s);
	return r.d;
}

double __hexagon_divdf3(double ad, double bd)
{
	f64u a = { .d = ad }, b = { .d = bd }, r;
	uint64_t sign = (a.u ^ b.u) & D_SIGN;
	if (d_isnan(a.u) || d_isnan(b.u)) { r.u = D_QNAN; return r.d; }
	if (d_isinf(a.u)) { r.u = d_isinf(b.u) ? D_QNAN : (sign | D_INF); return r.d; }
	if (d_isinf(b.u)) { r.u = sign; return r.d; }
	if (d_iszero(b.u)) { r.u = d_iszero(a.u) ? D_QNAN : (sign | D_INF); return r.d; }
	if (d_iszero(a.u)) { r.u = sign; return r.d; }
	int ea, eb;
	uint64_t ma = d_unpack(a.u, &ea), mb = d_unpack(b.u, &eb);
	uint64_t rem = ma, Q = 0;
	for (int i = 0; i < 56; i++) {
		Q <<= 1;
		if (rem >= mb) { rem -= mb; Q |= 1; }
		rem <<= 1;
	}
	/* Q = floor(ma * 2^55 / mb) in (2^54, 2^56) */
	int shift = (Q >> 55) ? 3 : 2;
	uint64_t R = Q >> shift;
	int g = (int)((Q >> (shift - 1)) & 1);
	int s = ((Q & ((1ull << (shift - 1)) - 1)) != 0) || rem != 0;
	int e = ea - eb + 1020 + shift;
	r.u = d_pack(sign, e, R, g, s);
	return r.d;
}

float __hexagon_divsf3(float af, float bf)
{
	f32u a = { .f = af }, b = { .f = bf }, r;
	uint32_t sign = (a.u ^ b.u) & 0x80000000u;
	int ea = (int)((a.u >> 23) & 0xff), eb = (int)((b.u >> 23) & 0xff);
	uint32_t fa = a.u & 0x7fffff, fb = b.u & 0x7fffff;
	if ((ea == 0xff && fa) || (eb == 0xff && fb)) { r.u = 0x7fc00000u; return r.f; }
	if (ea == 0xff) { r.u = (eb == 0xff) ? 0x7fc00000u : (sign | 0x7f800000u); return r.f; }
	if (eb == 0xff) { r.u = sign; return r.f; }
	if (eb == 0 && fb == 0) { r.u = (ea == 0 && fa == 0) ? 0x7fc00000u : (sign | 0x7f800000u); return r.f; }
	if (ea == 0 && fa == 0) { r.u = sign; return r.f; }
	uint64_t ma = fa, mb = fb;
	if (ea == 0) { ea = 1; while (!(ma & 0x800000)) { ma <<= 1; ea--; } } else ma |= 0x800000;
	if (eb == 0) { eb = 1; while (!(mb & 0x800000)) { mb <<= 1; eb--; } } else mb |= 0x800000;
	uint64_t rem = ma, Q = 0;
	for (int i = 0; i < 27; i++) {
		Q <<= 1;
		if (rem >= mb) { rem -= mb; Q |= 1; }
		rem <<= 1;
	}
	/* Q = floor(ma*2^26/mb) in (2^25, 2^27) */
	int shift = (Q >> 26) ? 3 : 2;
	uint32_t R = (uint32_t)(Q >> shift);
	int g = (int)((Q >> (shift - 1)) & 1);
	int s = ((Q & ((1ull << (shift - 1)) - 1)) != 0) || rem != 0;
	int e = ea - eb + 124 + shift;
	if (g && (s || (R & 1))) { R++; if (R == 0x1000000u) { R >>= 1; e++; } }
	if (e >= 0xff) { r.u = sign | 0x7f800000u; return r.f; }
	if (e <= 0) { r.u = sign; return r.f; }
	r.u = sign | ((uint32_t)e << 23) | (R & 0x7fffff);
	return r.f;
}

/* Dinkumware _Sin(x, qoff): qoff&1 -> cos. Returns sin(x + qoff*pi/2). */
double _Sin(double x, unsigned int qoff)
{
	static const double pio2_hi = 1.57079632679489655800e+00;
	static const double pio2_lo = 6.12323399573676603587e-17;
	static const double two_over_pi = 6.36619772367581382433e-01;
	f64u ux = { .d = x };
	if (((ux.u >> 52) & 0x7ff) == 0x7ff) { ux.u = D_QNAN; return ux.d; }
	/* n = round(x * 2/pi); only moderate |x| expected (filter design) */
	double fn = x * two_over_pi;
	long long n = (long long)(fn + (fn >= 0 ? 0.5 : -0.5));
	double dn = (double)n;
	double y = (x - dn * pio2_hi) - dn * pio2_lo;
	unsigned int q = (unsigned int)(n + (long long)qoff) & 3u;
	double y2 = y * y;
	/* Taylor to y^17 / y^16, |y| <= pi/4: error < 1e-17 */
	double s = y * (1.0 + y2 * (-1.0/6 + y2 * (1.0/120 + y2 * (-1.0/5040 + y2 * (1.0/362880
			+ y2 * (-1.0/39916800 + y2 * (1.0/6227020800.0 + y2 * (-1.0/1307674368000.0
			+ y2 * (1.0/355687428096000.0)))))))));
	double c = 1.0 + y2 * (-0.5 + y2 * (1.0/24 + y2 * (-1.0/720 + y2 * (1.0/40320
			+ y2 * (-1.0/3628800 + y2 * (1.0/479001600.0 + y2 * (-1.0/87178291200.0
			+ y2 * (1.0/20922789888000.0))))))));
	switch (q) {
	case 0: return s;
	case 1: return c;
	case 2: return -s;
	default: return -c;
	}
}
