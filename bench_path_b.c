/* Quick wall-time bench for FN-DSA signing.
 * Compares baseline (FNDSA_PATH_B=0) vs Path B (FNDSA_PATH_B=1).
 * Build twice with different flags and compare nanoseconds/sign. */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "fndsa.h"

static double
ns_now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

static int cmp_dbl(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;
	return (x > y) - (x < y);
}

static void
bench(unsigned logn, int n_iter)
{
	size_t sk_len = FNDSA_SIGN_KEY_SIZE(logn);
	size_t vk_len = FNDSA_VRFY_KEY_SIZE(logn);
	size_t sig_len_max = FNDSA_SIGNATURE_SIZE(logn);
	uint8_t *sk = malloc(sk_len);
	uint8_t *vk = malloc(vk_len);
	uint8_t *sig = malloc(sig_len_max);

	uint8_t seed[8] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x11};
	fndsa_keygen_seeded(logn, seed, sizeof seed, sk, vk);

	double *samples = malloc((size_t)n_iter * sizeof(double));
	uint8_t mseed[8] = {0};
	for (int i = 0; i < n_iter; i++) {
		mseed[0] = (uint8_t)i;
		mseed[1] = (uint8_t)(i >> 8);
		double t0 = ns_now();
		size_t r = fndsa_sign_seeded(sk, sk_len, NULL, 0,
			FNDSA_HASH_ID_RAW, "test", 4,
			mseed, sizeof mseed, sig, sig_len_max);
		double t1 = ns_now();
		if (r == 0) {
			fprintf(stderr, "sign failed at i=%d\n", i);
			exit(1);
		}
		samples[i] = t1 - t0;
	}

	qsort(samples, n_iter, sizeof(double), cmp_dbl);
	double median = samples[n_iter / 2];
	double p10 = samples[n_iter / 10];
	double p90 = samples[(n_iter * 9) / 10];

	printf("logn=%u  n=%u  iters=%d  median=%.0f ns  p10=%.0f  p90=%.0f\n",
		logn, 1u << logn, n_iter, median, p10, p90);

	free(samples); free(sig); free(vk); free(sk);
}

int
main(void)
{
#ifdef FNDSA_PATH_B
	printf("=== FNDSA_PATH_B=%d ===\n", FNDSA_PATH_B);
#else
	printf("=== FNDSA_PATH_B undefined ===\n");
#endif
	bench(9, 200);
	bench(10, 100);
	return 0;
}
