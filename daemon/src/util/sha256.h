/* util/sha256.h -- minimal, dependency-free SHA-256, used for
 * sha256 verification against data/capi-sha256.txt (by
 * `setup --capi`). No OpenSSL/libgcrypt dependency (this
 * project's own "no unnecessary dependencies" rule) -- this is a one-shot CLI
 * admin command (`momentum4 setup --capi`), never a hot/RT path, so a
 * straightforward textbook implementation (FIPS 180-4) is the right trade,
 * not a hand-optimised one.
 */
#ifndef MOMENTUM4_UTIL_SHA256_H
#define MOMENTUM4_UTIL_SHA256_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define M4_SHA256_DIGEST_LEN 32
#define M4_SHA256_HEX_LEN (M4_SHA256_DIGEST_LEN * 2 + 1) /* + NUL */

typedef struct {
	uint32_t state[8];
	uint64_t total_len;
	unsigned char buf[64];
	size_t buf_len;
} m4_sha256_ctx_t;

void m4_sha256_init(m4_sha256_ctx_t *ctx);
void m4_sha256_update(m4_sha256_ctx_t *ctx, const void *data, size_t len);
void m4_sha256_final(m4_sha256_ctx_t *ctx, unsigned char out[M4_SHA256_DIGEST_LEN]);

/* Lowercase hex, NUL-terminated (M4_SHA256_HEX_LEN bytes total). */
void m4_sha256_to_hex(const unsigned char digest[M4_SHA256_DIGEST_LEN], char out[M4_SHA256_HEX_LEN]);

/* Hashes the whole file at `path` (streamed, bounded memory -- fine for a
 * multi-MB CAPI .so). Returns 0 and fills `out_hex` (M4_SHA256_HEX_LEN
 * bytes) on success, -1 on any I/O error (errno set). */
int m4_sha256_file_hex(const char *path, char out_hex[M4_SHA256_HEX_LEN]);

#ifdef __cplusplus
}
#endif

#endif /* MOMENTUM4_UTIL_SHA256_H */
