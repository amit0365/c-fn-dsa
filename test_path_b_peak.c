/* Find the EXACT internal peak of ffsamp_fft_inner at each logn, by
 * painting individual outer-quarters and detecting which ones get touched.
 *
 * This tells us T(L) directly — the tightest possible tmp[] requirement
 * for ffsamp at each level. Combined with recurrence math:
 *
 *   T(L) = 10 (Path B persistent) + ceil(T(L-1) / 2)
 *
 * If T(2) = 24, T(L) converges to 21 outer-q (= 5.25n FLR asymptotic).
 * If T(2) = 22, same convergence.
 * If T(2) = 20, T(L) converges to 20 outer-q (= 5.0n FLR asymptotic).
 *
 * The 0.25n FLR delta at logn=10 = 2 KiB of dormant savings IF T(2) ≤ 22.
 *
 * Strategy:
 *   For each logn, allocate tmp at baseline 28 outer-q (= 7n FLR).
 *   Paint each outer-quarter [qc(K), qc(K+1)) individually with sentinel.
 *   Run ffsamp_fft_inner.
 *   Report the highest K that was modified.
 *   T(L) = max_K + 1 (since K is 0-indexed and we count outer-q).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "sign_inner.h"
#include "sign_sampler.c"

#define SENTINEL_FPR ((fpr)0xDEADBEEFCAFEBABEULL)

static void
setup_input(unsigned logn, fpr *tmp)
{
	size_t n = (size_t)1 << logn;
	size_t hn = n >> 1;
	fpr *t0  = tmp;
	fpr *t1  = tmp + n;
	fpr *g01 = tmp + 2 * n;
	fpr *g00 = tmp + 3 * n;
	fpr *g11 = tmp + 3 * n + hn;
	const fpr POS = FPR(4503599627370496LL, -50);
	for (size_t i = 0; i < n; i++) { t0[i] = FPR_ZERO; t1[i] = FPR_ZERO; }
	for (size_t i = 0; i < n; i++) g01[i] = FPR_ZERO;
	for (size_t i = 0; i < hn; i++) { g00[i] = POS; g11[i] = POS; }
}

static int
test_one_level(unsigned logn)
{
	size_t n = (size_t)1 << logn;
	size_t qsize = n >> 2;  /* outer-quarter size in FLR */

	size_t tmp_flr = 7 * n;  /* baseline 28 outer-q allocation */
	fpr *tmp = (fpr *)calloc(tmp_flr, sizeof(fpr));
	if (!tmp) return -1;

	int max_touched = -1;

	/* For each outer-quarter K from 27 down to 0, paint just that
	 * quarter and run ffsamp. Largest K with violations = T(L) - 1. */
	for (int k = 27; k >= 0; k--) {
		setup_input(logn, tmp);

		fpr *paint_start = tmp + (size_t)k * qsize;
		for (size_t i = 0; i < qsize; i++) {
			paint_start[i] = SENTINEL_FPR;
		}

		sampler_state ss;
		uint8_t seed[56];
		memset(seed, 0xCD ^ (uint8_t)k, sizeof seed);
		sampler_init(&ss, logn, seed, sizeof seed);

		ffsamp_fft_inner(&ss, logn, tmp);

		size_t v = 0;
		for (size_t i = 0; i < qsize; i++) {
			if (paint_start[i] != SENTINEL_FPR) v++;
		}
		if (v > 0) {
			if (k > max_touched) max_touched = k;
			break;  /* highest k with any violations; deeper k will all be touched */
		}
	}

	free(tmp);

	if (max_touched < 0) {
		printf("logn=%u: all outer-q [0..27] untouched? (suspicious — check input)\n", logn);
		return 0;
	}

	int T_L = max_touched + 1;
	double frac = (double)T_L / 4.0;  /* T(L) outer-q = T(L)/4 in n-units */
	printf("logn=%2u  T(L) = %2d outer-q = %.2fn FLR  (highest touched: qc(%d))\n",
		logn, T_L, frac, max_touched);
	return T_L;
}

int main(void)
{
	printf("=== ffsamp_fft_inner internal peak per logn ===\n");
	printf("Finds the actual highest outer-quarter touched at each level.\n");
	printf("T(L) <= 21 means the recursive Path B 5.25n asymptote holds.\n");
	printf("T(L) <= 20 means recursive Path B reaches its 5.0n asymptote.\n\n");

	for (unsigned logn = 2; logn <= 10; logn++) {
		test_one_level(logn);
	}

	printf("\nReference (Path B body's claimed footprint): 24 outer-q = 6.00n FLR\n");
	return 0;
}
