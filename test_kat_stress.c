/*
 * test_kat_stress: extended-coverage stress harness for the FN-DSA
 * keygen / sign / verify pipeline.
 *
 * Why this exists: the canonical KAT in test_fndsa.c exercises 10 seeds
 * per logn — enough to detect a bit-exact divergence from Pornin's
 * reference, but not enough to surface bugs that fire at <~10% per
 * signature. In particular the discrete-Gaussian sampler's rejection-
 * and-resample loop is rarely entered with only 10 vectors, and arch-
 * specific divergences (NEON / SSE2 / RV64D / scalar) are not exercised
 * by a single host build.
 *
 * What this catches:
 *   - sign returning 0 within max attempts (sampler / norm-bound bugs)
 *   - verify rejecting a freshly generated signature (encoder, fpoly,
 *     numerical divergence)
 *   - asserts / OOB on rare RNG paths
 *   - cross-build divergence (LOW_RAM vs baseline, NEON vs scalar) when
 *     the same arguments are run on two binaries and the per-logn
 *     fingerprints are diffed
 *
 * Usage:
 *   ./test_kat_stress                     # default schedule (see below)
 *   ./test_kat_stress N                   # N iterations per logn (small)
 *   ./test_kat_stress N9 N10              # N9 for logn=9, N10 for logn=10,
 *                                         # default for logn<=8
 *   ./test_kat_stress N3_8 N9 N10         # full per-tier override
 *
 * Default schedule trades coverage against runtime so a `make stress`
 * lands in ~1-2 minutes on a laptop:
 *
 *   logn  default iters   approx wallclock per iter (NEON laptop)
 *   ---- ------------------------------------------------------
 *   2-8     1000             <1 ms keygen, <1 ms sign+verify
 *   9        100             ~5 ms total (FN-DSA-512)
 *   10        20             ~30 ms total (FN-DSA-1024)
 *
 * Per-logn output: a single SHA3-256 fingerprint over (sk||vk||sig)
 * for every iteration in order. Two builds that produce bit-identical
 * signatures (which Pornin's spec guarantees for logn>=3 across the
 * LOW_RAM / baseline / arch variants) print the same fingerprint —
 * the harness becomes a one-line differential check.
 *
 * Failure mode: on the first sign / verify failure the seed (logn, j,
 * which is the 6-byte input to SHAKE that drives both keygen and sign)
 * is printed, then exit(EXIT_FAILURE). The seed is sufficient to
 * reproduce the bug under a debugger.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "inner.h"

#if defined __GNUC__ || defined __clang__
#define NOINLINE   __attribute__ ((noinline))
#else
#define NOINLINE
#endif

static void *
xmalloc(size_t len)
{
	if (len == 0) {
		return NULL;
	}
	void *buf = malloc(len);
	if (buf == NULL) {
		fprintf(stderr, "memory allocation error (size=%zu)\n", len);
		exit(EXIT_FAILURE);
	}
	return buf;
}

static void
xfree(void *buf)
{
	if (buf != NULL) {
		free(buf);
	}
}

static void
fail_with_seed(const char *what, unsigned logn, uint32_t j,
	const uint8_t seed[6])
{
	fprintf(stderr,
		"\n[FAIL] %s at logn=%u j=%u seed=%02x%02x%02x%02x%02x%02x\n",
		what, logn, j,
		seed[0], seed[1], seed[2], seed[3], seed[4], seed[5]);
	fprintf(stderr,
		"  reproduce: derive 32-byte keygen seed via SHAKE256(seed),\n"
		"  then call fndsa_keygen_seeded_temp(logn, kseed, 32, ...).\n"
		"  Sign seed = same 6 bytes with seed[0] flipped to 0x01.\n");
	exit(EXIT_FAILURE);
}

NOINLINE
static void
stress_one_logn(unsigned logn, uint32_t iters)
{
	size_t sk_len = FNDSA_SIGN_KEY_SIZE(logn);
	size_t vk_len = FNDSA_VRFY_KEY_SIZE(logn);
	size_t sig_len = FNDSA_SIGNATURE_SIZE(logn);
	uint8_t *sk = xmalloc(sk_len);
	uint8_t *vk = xmalloc(vk_len);
	uint8_t *sig = xmalloc(sig_len);

	size_t kgentmp_len = ((size_t)26 << logn) + 31;
	size_t signtmp_len = ((size_t)59 << logn) + 31;
	size_t vrfytmp_len = ((size_t)4 << logn) + 31;
	size_t tmp_len = signtmp_len > kgentmp_len ? signtmp_len : kgentmp_len;
	if (vrfytmp_len > tmp_len) {
		tmp_len = vrfytmp_len;
	}
	void *tmp = xmalloc(tmp_len);

	shake_context pc;
	sha3_context msg_sc;
	sha3_context fp_sc;
	sha3_init(&fp_sc, 256);

	uint8_t kseed[32];
	uint8_t hashed_msg[32];
	uint8_t fp[32];

	printf("[%u/%u]", logn, iters);
	fflush(stdout);

	uint32_t step = iters / 20 ? iters / 20 : 1;

	for (uint32_t j = 0; j < iters; j ++) {
		uint8_t seed[6];
		seed[0] = 0x00;
		seed[1] = (uint8_t)logn;
		seed[2] = (uint8_t)j;
		seed[3] = (uint8_t)(j >> 8);
		seed[4] = (uint8_t)(j >> 16);
		seed[5] = (uint8_t)(j >> 24);

		shake_init(&pc, 256);
		shake_inject(&pc, seed, sizeof seed);
		shake_flip(&pc);
		shake_extract(&pc, kseed, 32);

		if (!fndsa_keygen_seeded_temp(logn, kseed, 32,
			sk, vk, tmp, kgentmp_len))
		{
			fail_with_seed("keygen", logn, j, seed);
		}

		seed[0] = 0x01;
		const char *id;
		const void *msg;
		size_t msg_len;
		const char *raw_msg = "stress-message";
		if ((j & 1) == 0) {
			id = FNDSA_HASH_ID_RAW;
			msg = raw_msg;
			msg_len = strlen(raw_msg);
		} else {
			id = FNDSA_HASH_ID_SHA3_256;
			sha3_init(&msg_sc, 256);
			sha3_update(&msg_sc, raw_msg, strlen(raw_msg));
			sha3_close(&msg_sc, hashed_msg);
			msg = hashed_msg;
			msg_len = 32;
		}

		size_t r;
		if (logn <= 8) {
			r = fndsa_sign_weak_seeded_temp(sk, sk_len,
				"domain", 6, id, msg, msg_len,
				seed, sizeof seed, sig, sig_len,
				tmp, signtmp_len);
		} else {
			r = fndsa_sign_seeded_temp(sk, sk_len,
				"domain", 6, id, msg, msg_len,
				seed, sizeof seed, sig, sig_len,
				tmp, signtmp_len);
		}
		if (r != sig_len) {
			fail_with_seed("sign", logn, j, seed);
		}

		int t;
		if (logn <= 8) {
			t = fndsa_verify_weak_temp(sig, sig_len, vk, vk_len,
				"domain", 6, id, msg, msg_len,
				tmp, vrfytmp_len);
		} else {
			t = fndsa_verify_temp(sig, sig_len, vk, vk_len,
				"domain", 6, id, msg, msg_len,
				tmp, vrfytmp_len);
		}
		if (!t) {
			fail_with_seed("verify", logn, j, seed);
		}

		sha3_update(&fp_sc, sk, sk_len);
		sha3_update(&fp_sc, vk, vk_len);
		sha3_update(&fp_sc, sig, sig_len);

		if ((j + 1) % step == 0 || j + 1 == iters) {
			printf(".");
			fflush(stdout);
		}
	}

	sha3_close(&fp_sc, fp);
	printf(" fingerprint=");
	for (int i = 0; i < 32; i ++) {
		printf("%02x", fp[i]);
	}
	printf("\n");
	fflush(stdout);

	xfree(sk);
	xfree(vk);
	xfree(sig);
	xfree(tmp);
}

static uint32_t
parse_iter(const char *s, const char *what)
{
	char *end;
	unsigned long v = strtoul(s, &end, 10);
	if (*end != 0 || v == 0 || v > 1000000UL) {
		fprintf(stderr,
			"bad %s iteration count: %s (must be 1..1000000)\n",
			what, s);
		exit(EXIT_FAILURE);
	}
	return (uint32_t)v;
}

int
main(int argc, char *argv[])
{
	uint32_t iter_low = 1000;
	uint32_t iter_9 = 100;
	uint32_t iter_10 = 20;

	if (argc == 2) {
		uint32_t v = parse_iter(argv[1], "global");
		iter_low = v;
		iter_9 = v;
		iter_10 = v;
	} else if (argc == 3) {
		iter_9 = parse_iter(argv[1], "logn=9");
		iter_10 = parse_iter(argv[2], "logn=10");
	} else if (argc == 4) {
		iter_low = parse_iter(argv[1], "logn<=8");
		iter_9 = parse_iter(argv[2], "logn=9");
		iter_10 = parse_iter(argv[3], "logn=10");
	} else if (argc != 1) {
		fprintf(stderr,
			"usage: %s [N | N9 N10 | N_low N9 N10]\n",
			argv[0]);
		return EXIT_FAILURE;
	}

	printf("FN-DSA stress harness "
		"(low=%u, n=512=%u, n=1024=%u)\n", iter_low, iter_9, iter_10);
#if FNDSA_LOW_RAM
	printf("build: FNDSA_LOW_RAM=1\n");
#else
	printf("build: FNDSA_LOW_RAM=0\n");
#endif
#if FNDSA_NEON
	printf("arch: NEON enabled\n");
#elif FNDSA_AVX2
	printf("arch: AVX2 compiled (runtime-gated)\n");
#elif FNDSA_SSE2
	printf("arch: SSE2 enabled\n");
#elif FNDSA_RV64D
	printf("arch: RV64D enabled\n");
#else
	printf("arch: scalar\n");
#endif
	fflush(stdout);

	stress_one_logn(2, iter_low);
	stress_one_logn(3, iter_low);
	stress_one_logn(4, iter_low);
	stress_one_logn(5, iter_low);
	stress_one_logn(6, iter_low);
	stress_one_logn(7, iter_low);
	stress_one_logn(8, iter_low);
	stress_one_logn(9, iter_9);
	stress_one_logn(10, iter_10);

	printf("stress: all signatures verified.\n");
	return EXIT_SUCCESS;
}
