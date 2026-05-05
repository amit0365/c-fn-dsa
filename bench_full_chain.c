/* bench_full_chain.c — end-to-end signing microbenchmark in the
 * SUPERCOP / pq-crystals/dilithium test_speed.c shape: NTESTS individual
 * cpucycles() snapshots around each sign call, reported as
 *
 *   sign logn=N: median: <c>, average: <c>, min: <c>, max: <c> (UNIT, n=...)
 *   tmp logn=N:  <bytes>
 *
 * Build/run all configurations via run_bench.sh.
 *
 * Configuration auto-selection at build time:
 *   FNDSA_LOW_RAM defined → fndsa_sign_seeded_with_basis_temp (with-basis API)
 *   else                  → fndsa_sign_seeded_temp (baseline, no-basis API) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "fndsa.h"
#include "cpucycles.h"
#include "speed_print.h"

/* Mirror inner.h's default so -Wundef is satisfied without exposing
   inner.h to a benchmark TU. */
#ifndef FNDSA_LOW_RAM
#define FNDSA_LOW_RAM 0
#endif

/* NTESTS matches the convention used in pq-crystals/dilithium and
   pq-crystals/kyber's ref/test/test_speed.c. 10000 individual op timings
   give a rock-solid median; outliers from OS preempts / DVFS / cache
   effects sit in the right tail and don't move the middle. */
#ifndef NTESTS
#define NTESTS 10000
#endif

static int
bench_at_logn(unsigned logn)
{
	size_t sk_len = FNDSA_SIGN_KEY_SIZE(logn);
	size_t vk_len = FNDSA_VRFY_KEY_SIZE(logn);
	size_t sig_len_max = FNDSA_SIGNATURE_SIZE(logn);

#if FNDSA_LOW_RAM
	size_t basis_len = FNDSA_BASIS_SIZE(logn);
	void *basis = aligned_alloc(8, basis_len);
	size_t tmp_len = ((size_t)37 << logn) + 31;
#else
	size_t tmp_len = ((size_t)59 << logn) + 31;
#endif

	uint8_t *sk = malloc(sk_len);
	uint8_t *vk = malloc(vk_len);
	uint8_t *sig = malloc(sig_len_max);
	uint8_t *tmp = malloc(tmp_len);
	uint64_t *t = malloc((NTESTS + 1) * sizeof *t);
	if (!sk || !vk || !sig || !tmp || !t) {
		fprintf(stderr, "logn=%u: alloc failed\n", logn);
		return 1;
	}

	uint8_t kseed[8] = {0xCA, 0xFE, 0xBA, 0xBE, (uint8_t)logn, 0, 0, 0};
	fndsa_keygen_seeded(logn, kseed, sizeof kseed, sk, vk);

#if FNDSA_LOW_RAM
	if (!fndsa_compute_basis(sk, sk_len, basis, basis_len)) {
		fprintf(stderr, "logn=%u: fndsa_compute_basis FAILED\n", logn);
		return 1;
	}
#endif

	uint8_t mseed[8] = {0xDE, 0xAD, 0xBE, 0xEF, 0, 0, 0, 0};

	/* Warm-up — settles caches, branch predictor, turbo state. Same
	   convention as Dilithium / Kyber test_speed.c (a few "burn-in"
	   iterations before the measured loop). */
	for (int i = 0; i < 5; i++) {
		mseed[4] = (uint8_t)i;
#if FNDSA_LOW_RAM
		(void)fndsa_sign_seeded_with_basis_temp(
			sk, sk_len, basis,
			NULL, 0, FNDSA_HASH_ID_RAW, "msg", 3,
			mseed, sizeof mseed,
			sig, sig_len_max,
			tmp, tmp_len);
#else
		(void)fndsa_sign_seeded_temp(
			sk, sk_len,
			NULL, 0, FNDSA_HASH_ID_RAW, "msg", 3,
			mseed, sizeof mseed,
			sig, sig_len_max,
			tmp, tmp_len);
#endif
	}

	/* Measurement loop: NTESTS+1 cpucycles() snapshots taken immediately
	   before each op. Successive differences give NTESTS individual
	   per-sign cycle counts. */
	for (int i = 0; i < NTESTS; i++) {
		mseed[4] = (uint8_t)i;
		mseed[5] = (uint8_t)(i >> 8);
		t[i] = cpucycles();
#if FNDSA_LOW_RAM
		size_t l = fndsa_sign_seeded_with_basis_temp(
			sk, sk_len, basis,
			NULL, 0, FNDSA_HASH_ID_RAW, "msg", 3,
			mseed, sizeof mseed,
			sig, sig_len_max,
			tmp, tmp_len);
#else
		size_t l = fndsa_sign_seeded_temp(
			sk, sk_len,
			NULL, 0, FNDSA_HASH_ID_RAW, "msg", 3,
			mseed, sizeof mseed,
			sig, sig_len_max,
			tmp, tmp_len);
#endif
		if (l == 0) {
			fprintf(stderr, "logn=%u iter=%d: sign FAILED\n", logn, i);
			return 1;
		}
	}
	t[NTESTS] = cpucycles();

	char label[64];
	snprintf(label, sizeof label, "sign logn=%u:", logn);
	print_results(label, t, NTESTS + 1);
	printf("tmp  logn=%u: %zu bytes\n", logn, tmp_len);

	free(t); free(tmp); free(sig); free(vk); free(sk);
#if FNDSA_LOW_RAM
	free(basis);
#endif
	return 0;
}

int main(void)
{
	/* Build flag → config string for log readability. */
#if FNDSA_LOW_RAM
	const char *cfg = "FNDSA_LOW_RAM=1 (with-basis API)";
#else
	const char *cfg = "baseline (no-basis API)";
#endif
	printf("# bench_full_chain  config=%s  NTESTS=%d  unit=%s\n",
		cfg, NTESTS, CPUCYCLES_UNIT);

	int failures = 0;
	if (bench_at_logn(9)  != 0) failures++;
	if (bench_at_logn(10) != 0) failures++;
	return failures;
}
