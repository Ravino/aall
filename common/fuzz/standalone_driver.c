/* common/fuzz/standalone_driver.c -- offline, clang-free fallback driver
 * for fuzz_m4_conf.c's LLVMFuzzerTestOneInput().
 *
 * On a machine with no clang (`which clang*` -> nothing), real libFuzzer
 * (-fsanitize=fuzzer, a clang-only feature) is not available -- see
 * build.sh, which tries clang first and only falls back to this file if
 * clang is absent. This driver is
 * NOT a libFuzzer reimplementation (no coverage-guided corpus scheduling,
 * no crash minimization) -- it is deliberately the simplest thing that (a)
 * replays a seed corpus deterministically (regression) and (b) spends a
 * bounded wall-clock budget throwing random/mutated bytes at
 * LLVMFuzzerTestOneInput() under ASan/UBSan, so a real out-of-bounds
 * access or UB in m4-conf.c still gets caught -- just via brute force
 * instead of coverage feedback. A machine WITH clang should prefer
 * build.sh's real-libFuzzer path (much better bug-finding per CPU-second);
 * this file exists so the parser still gets an offline, time-bounded fuzz
 * pass on a machine that only has gcc.
 *
 * usage: standalone_driver [--seconds N] [--seed N] [corpus-dir-or-file ...]
 *   --seconds N   mutation-fuzzing budget after the corpus replay (default 20)
 *   --seed N      PRNG seed (default: current time, printed so a crash can
 *                 be reproduced by re-running with the same --seed)
 * Exit code: 0 if every corpus file AND every mutated input ran without
 * LLVMFuzzerTestOneInput() itself crashing (a crash terminates this
 * process via signal -- there is no in-process recovery, matching real
 * libFuzzer's own single-input-per-crash model closely enough for this
 * offline use case).
 */
#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

#define MAX_MUTATED 16384

static long run_file(const char *path)
{
	FILE *f = fopen(path, "rb");
	static uint8_t buf[MAX_MUTATED];
	size_t n;

	if (f == NULL) {
		fprintf(stderr, "standalone_driver: %s: cannot open, skipping\n", path);
		return 0;
	}
	n = fread(buf, 1, sizeof(buf), f);
	fclose(f);
	LLVMFuzzerTestOneInput(buf, n);
	return 1;
}

static long run_dir_or_file(const char *path)
{
	struct stat st;
	long count = 0;

	if (stat(path, &st) != 0) {
		fprintf(stderr, "standalone_driver: %s: %s\n", path, strerror(errno));
		return 0;
	}
	if (!S_ISDIR(st.st_mode)) {
		return run_file(path);
	}

	{
		DIR *d = opendir(path);
		struct dirent *ent;

		if (d == NULL)
			return 0;
		while ((ent = readdir(d)) != NULL) {
			char full[4096];

			if (ent->d_name[0] == '.')
				continue;
			snprintf(full, sizeof(full), "%s/%s", path, ent->d_name);
			count += run_file(full);
		}
		closedir(d);
	}
	return count;
}

/* xorshift32 -- fast, good enough for a brute-force mutation loop (not a
 * security context); seeded explicitly and PRINTED so any crash found can
 * be reproduced with `--seed <that number>`. */
static uint32_t rng_state;

static uint32_t xorshift32(void)
{
	uint32_t x = rng_state;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	rng_state = x;
	return x;
}

/* Builds one mutated input into `out` (capacity `out_cap`), returns its
 * length. With no seed corpus: pure random bytes, length up to 512 (a
 * plausible momentum4.conf snippet size -- large enough to explore
 * multi-line/multi-section parsing, small enough to iterate fast).
 * With a seed corpus: copies a random seed then applies a handful of
 * byte-level mutations (flip/insert/delete/duplicate-a-known-token) --
 * cheap, dependency-free approximation of what a real fuzzer's mutator
 * does, sufficient to perturb section headers/keys/values/quoting. */
static size_t mutate(uint8_t *out, size_t out_cap, const uint8_t **seeds, const size_t *seed_lens,
		      size_t n_seeds)
{
	static const char *tokens[] = { "[adaptive]", "[helper]", "[device]", "mode",
					 "=", "\"", "${XDG_RUNTIME_DIR}", "%h", "@libdir@",
					 "\n", "#", ";", "9999999999999999", "-1", "nan", "" };
	size_t len;

	if (n_seeds > 0 && (xorshift32() % 4) != 0) {
		size_t which = xorshift32() % n_seeds;

		len = seed_lens[which] < out_cap ? seed_lens[which] : out_cap;
		memcpy(out, seeds[which], len);
	} else {
		len = xorshift32() % (out_cap < 512 ? out_cap : 512);
		for (size_t i = 0; i < len; i++)
			out[i] = (uint8_t)(xorshift32() & 0x7f); /* printable-ish ASCII */
	}

	{
		int n_mutations = 1 + (int)(xorshift32() % 6);

		for (int m = 0; m < n_mutations && len > 0; m++) {
			switch (xorshift32() % 4) {
			case 0: /* flip a byte */
				out[xorshift32() % len] = (uint8_t)(xorshift32() & 0xff);
				break;
			case 1: /* insert a random token */ {
				const char *tok = tokens[xorshift32() % (sizeof(tokens) / sizeof(tokens[0]))];
				size_t tlen = strlen(tok);
				size_t at = xorshift32() % (len + 1);

				if (len + tlen < out_cap) {
					memmove(out + at + tlen, out + at, len - at);
					memcpy(out + at, tok, tlen);
					len += tlen;
				}
				break;
			}
			case 2: /* delete a byte */
				if (len > 1) {
					size_t at = xorshift32() % len;

					memmove(out + at, out + at + 1, len - at - 1);
					len--;
				}
				break;
			default: /* truncate */
				len = xorshift32() % (len + 1);
				break;
			}
		}
	}
	return len;
}

int main(int argc, char **argv)
{
	long seconds = 20;
	uint32_t seed = (uint32_t)time(NULL);
	const char *corpus_paths[64];
	int n_corpus_paths = 0;
	int i;
	long corpus_ran = 0;
	long mutated_ran = 0;
	time_t start, now;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
			seconds = atol(argv[++i]);
		} else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
			seed = (uint32_t)strtoul(argv[++i], NULL, 10);
		} else if (n_corpus_paths < (int)(sizeof(corpus_paths) / sizeof(corpus_paths[0]))) {
			corpus_paths[n_corpus_paths++] = argv[i];
		}
	}

	rng_state = seed != 0 ? seed : 1;
	printf("standalone_driver: seed=%u seconds=%ld corpus_paths=%d\n", seed, seconds, n_corpus_paths);

	for (i = 0; i < n_corpus_paths; i++)
		corpus_ran += run_dir_or_file(corpus_paths[i]);
	printf("standalone_driver: replayed %ld corpus file(s), no crash\n", corpus_ran);

	/* load the (small) corpus into memory once, for the mutator -- corpus
	 * files here are all well under a few KB (see corpus/ seeds), so this
	 * stays comfortably inside "no malloc for the library itself" (this is
	 * the fuzz HARNESS, not common/m4-conf.c -- a dynamically-sized corpus
	 * cache is normal/expected here, same as libFuzzer's own). */
	{
		static uint8_t seed_bufs[16][MAX_MUTATED];
		const uint8_t *seed_ptrs[16];
		size_t seed_lens[16];
		size_t n_seeds = 0;

		for (i = 0; i < n_corpus_paths && n_seeds < 16; i++) {
			struct stat st;

			if (stat(corpus_paths[i], &st) == 0 && !S_ISDIR(st.st_mode)) {
				FILE *f = fopen(corpus_paths[i], "rb");

				if (f != NULL) {
					seed_lens[n_seeds] = fread(seed_bufs[n_seeds], 1, MAX_MUTATED, f);
					seed_ptrs[n_seeds] = seed_bufs[n_seeds];
					fclose(f);
					n_seeds++;
				}
			} else if (stat(corpus_paths[i], &st) == 0 && S_ISDIR(st.st_mode)) {
				DIR *d = opendir(corpus_paths[i]);
				struct dirent *ent;

				if (d != NULL) {
					while ((ent = readdir(d)) != NULL && n_seeds < 16) {
						char full[4096];
						FILE *f;

						if (ent->d_name[0] == '.')
							continue;
						snprintf(full, sizeof(full), "%s/%s", corpus_paths[i], ent->d_name);
						f = fopen(full, "rb");
						if (f == NULL)
							continue;
						seed_lens[n_seeds] = fread(seed_bufs[n_seeds], 1, MAX_MUTATED, f);
						seed_ptrs[n_seeds] = seed_bufs[n_seeds];
						fclose(f);
						n_seeds++;
					}
					closedir(d);
				}
			}
		}

		start = time(NULL);
		for (;;) {
			static uint8_t mutated[MAX_MUTATED];
			size_t len;

			now = time(NULL);
			if (seconds >= 0 && now - start >= seconds)
				break;
			len = mutate(mutated, sizeof(mutated), seed_ptrs, seed_lens, n_seeds);
			LLVMFuzzerTestOneInput(mutated, len);
			mutated_ran++;
			if (mutated_ran % 200000 == 0)
				printf("standalone_driver: %ld mutated inputs so far, no crash (%lds elapsed)\n",
				       mutated_ran, (long)(now - start));
		}
	}

	printf("standalone_driver: done. %ld corpus + %ld mutated inputs, no crash (seed=%u)\n", corpus_ran,
	       mutated_ran, seed);
	return 0;
}
