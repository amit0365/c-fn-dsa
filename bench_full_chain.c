/* bench_full_chain.c — measure end-to-end signing time under whichever
 * configuration this binary was built with. Prints one line per logn:
 *
 *   <logn> <ns_per_sign> <iterations>
 *
 * Build/run all four configurations via run_bench.sh (companion script).
 *
 * Reports the WHOLE sign call (not just a sub-step) — the apples-to-apples
 * comparison the upstream PR description needs.
 *
 * Configuration auto-selection at build time:
 *   FNDSA_LOW_RAM defined → use fndsa_sign_seeded_with_basis_temp
 *                                   (the with-basis API; activates Path A
 *                                   if FNDSA_LOW_RAM also set)
 *   else                          → use fndsa_sign_seeded_temp (no-basis
 *                                   path; baseline or PATH_B alone) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#include "fndsa.h"

#define ITERS_LOGN_9   1000
#define ITERS_LOGN_10   500

static uint64_t
ns_now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int
bench_at_logn(unsigned logn, int iters)
{
	size_t sk_len = FNDSA_SIGN_KEY_SIZE(logn);
	size_t vk_len = FNDSA_VRFY_KEY_SIZE(logn);
	size_t sig_len_max = FNDSA_SIGNATURE_SIZE(logn);

#if FNDSA_LOW_RAM
	size_t basis_len = FNDSA_BASIS_SIZE(logn);
	void *basis = aligned_alloc(8, basis_len);
#  if FNDSA_LOW_RAM
	size_t tmp_len = ((size_t)37 << logn) + 31;
#  else
	size_t tmp_len = ((size_t)43 << logn) + 31;
#  endif
#else
#  if FNDSA_LOW_RAM
	size_t tmp_len = ((size_t)51 << logn) + 31;
#  else
	size_t tmp_len = ((size_t)59 << logn) + 31;
#  endif
#endif

	uint8_t *sk = malloc(sk_len);
	uint8_t *vk = malloc(vk_len);
	uint8_t *sig = malloc(sig_len_max);
	uint8_t *tmp = malloc(tmp_len);
	if (!sk || !vk || !sig || !tmp) {
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

	/* Warm-up: 5 signs to settle any first-time caching / branch prediction. */
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

	uint64_t t0 = ns_now();
	for (int i = 0; i < iters; i++) {
		mseed[4] = (uint8_t)i;
		mseed[5] = (uint8_t)(i >> 8);
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
	uint64_t t1 = ns_now();

	double ns_per_sign = (double)(t1 - t0) / (double)iters;
	printf("%u %.0f %d %zu\n", logn, ns_per_sign, iters, tmp_len);

	free(tmp); free(sig); free(vk); free(sk);
#if FNDSA_LOW_RAM
	free(basis);
#endif
	return 0;
}

int main(void)
{
	/* Output format: one line per logn, parsable by run_bench.sh:
	     <logn> <ns_per_sign> <iters> <tmp_len>
	   Example:  9 213487 1000 22047 */
	int failures = 0;
	if (bench_at_logn(9,  ITERS_LOGN_9) != 0) failures++;
	if (bench_at_logn(10, ITERS_LOGN_10) != 0) failures++;
	return failures;
}
