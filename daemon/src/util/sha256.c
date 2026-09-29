/* util/sha256.c -- see sha256.h. Textbook FIPS 180-4 SHA-256, single file,
 * no dependency beyond the C99 standard library.
 */
#include "sha256.h"

#include <stdio.h>
#include <string.h>

static const uint32_t K[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static uint32_t rotr(uint32_t x, int n)
{
	return (x >> n) | (x << (32 - n));
}

static void sha256_transform(m4_sha256_ctx_t *ctx, const unsigned char block[64])
{
	uint32_t w[64];
	uint32_t a, b, c, d, e, f, g, h;
	int i;

	for (i = 0; i < 16; i++) {
		w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
		       ((uint32_t)block[i * 4 + 2] << 8) | (uint32_t)block[i * 4 + 3];
	}
	for (i = 16; i < 64; i++) {
		uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
		uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);

		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}

	a = ctx->state[0];
	b = ctx->state[1];
	c = ctx->state[2];
	d = ctx->state[3];
	e = ctx->state[4];
	f = ctx->state[5];
	g = ctx->state[6];
	h = ctx->state[7];

	for (i = 0; i < 64; i++) {
		uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
		uint32_t ch = (e & f) ^ (~e & g);
		uint32_t temp1 = h + s1 + ch + K[i] + w[i];
		uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
		uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
		uint32_t temp2 = s0 + maj;

		h = g;
		g = f;
		f = e;
		e = d + temp1;
		d = c;
		c = b;
		b = a;
		a = temp1 + temp2;
	}

	ctx->state[0] += a;
	ctx->state[1] += b;
	ctx->state[2] += c;
	ctx->state[3] += d;
	ctx->state[4] += e;
	ctx->state[5] += f;
	ctx->state[6] += g;
	ctx->state[7] += h;
}

void m4_sha256_init(m4_sha256_ctx_t *ctx)
{
	ctx->state[0] = 0x6a09e667;
	ctx->state[1] = 0xbb67ae85;
	ctx->state[2] = 0x3c6ef372;
	ctx->state[3] = 0xa54ff53a;
	ctx->state[4] = 0x510e527f;
	ctx->state[5] = 0x9b05688c;
	ctx->state[6] = 0x1f83d9ab;
	ctx->state[7] = 0x5be0cd19;
	ctx->total_len = 0;
	ctx->buf_len = 0;
}

void m4_sha256_update(m4_sha256_ctx_t *ctx, const void *data, size_t len)
{
	const unsigned char *p = data;

	ctx->total_len += len;
	while (len > 0) {
		size_t take = sizeof(ctx->buf) - ctx->buf_len;

		if (take > len)
			take = len;
		memcpy(ctx->buf + ctx->buf_len, p, take);
		ctx->buf_len += take;
		p += take;
		len -= take;
		if (ctx->buf_len == sizeof(ctx->buf)) {
			sha256_transform(ctx, ctx->buf);
			ctx->buf_len = 0;
		}
	}
}

void m4_sha256_final(m4_sha256_ctx_t *ctx, unsigned char out[M4_SHA256_DIGEST_LEN])
{
	uint64_t bit_len = ctx->total_len * 8;
	unsigned char pad = 0x80;
	int i;

	m4_sha256_update(ctx, &pad, 1);
	/* m4_sha256_update() above already advanced total_len by 1 -- undo
	 * that before computing the length field below (the length field
	 * must reflect the ORIGINAL message length, not including the 0x80
	 * padding byte or the zero padding that follows). */
	ctx->total_len -= 1;

	{
		unsigned char zero = 0;

		while (ctx->buf_len != 56)
			m4_sha256_update(ctx, &zero, 1);
		/* the loop above also perturbs total_len the same way -- restore
		 * it one final time right before appending the length field. */
	}
	ctx->total_len = 0; /* length field appended manually below, not via update() */

	for (i = 7; i >= 0; i--) {
		unsigned char b = (unsigned char)((bit_len >> (i * 8)) & 0xff);

		ctx->buf[ctx->buf_len++] = b;
		if (ctx->buf_len == sizeof(ctx->buf)) {
			sha256_transform(ctx, ctx->buf);
			ctx->buf_len = 0;
		}
	}

	for (i = 0; i < 8; i++) {
		out[i * 4] = (unsigned char)(ctx->state[i] >> 24);
		out[i * 4 + 1] = (unsigned char)(ctx->state[i] >> 16);
		out[i * 4 + 2] = (unsigned char)(ctx->state[i] >> 8);
		out[i * 4 + 3] = (unsigned char)(ctx->state[i]);
	}
}

void m4_sha256_to_hex(const unsigned char digest[M4_SHA256_DIGEST_LEN], char out[M4_SHA256_HEX_LEN])
{
	static const char hex[] = "0123456789abcdef";
	int i;

	for (i = 0; i < M4_SHA256_DIGEST_LEN; i++) {
		out[i * 2] = hex[(digest[i] >> 4) & 0xf];
		out[i * 2 + 1] = hex[digest[i] & 0xf];
	}
	out[M4_SHA256_DIGEST_LEN * 2] = '\0';
}

int m4_sha256_file_hex(const char *path, char out_hex[M4_SHA256_HEX_LEN])
{
	FILE *f;
	m4_sha256_ctx_t ctx;
	unsigned char buf[65536];
	unsigned char digest[M4_SHA256_DIGEST_LEN];
	size_t n;

	f = fopen(path, "rbe");
	if (f == NULL)
		return -1;
	m4_sha256_init(&ctx);
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
		m4_sha256_update(&ctx, buf, n);
	if (ferror(f)) {
		fclose(f);
		return -1;
	}
	fclose(f);
	m4_sha256_final(&ctx, digest);
	m4_sha256_to_hex(digest, out_hex);
	return 0;
}
