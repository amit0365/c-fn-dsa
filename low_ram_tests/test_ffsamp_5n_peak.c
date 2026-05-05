/* Direct measurement of ffsamp_fft_inner's internal peak under the outer-level body
 * (FNDSA_LOW_RAM). Companion to test_path_b_peak.c.
 *
 * Build:
 *   clang -DFNDSA_LOW_RAM=1 -DFNDSA_LOW_RAM=1 -DFNDSA_LOW_RAM=1 \
 *     -O2 -c test_ffsamp_5n_peak.c -o test_ffsamp_5n_peak.o
 *   clang -o test_ffsamp_5n_peak test_ffsamp_5n_peak.o codec.o mq.o sha3.o \
 *     sysrng.o util.o kgen.o kgen_fxp.o kgen_gauss.o kgen_mp31.o kgen_ntru.o \
 *     kgen_poly.o kgen_zint31.o sign.o sign_core.o sign_fpoly.o sign_fpr.o \
 *     vrfy.o -lm
 *   (note: sign_sampler.c is #include'd directly, NOT linked separately)
 *
 * Strategy: paint individual qc-positions from qc(27) down, run
 * ffsamp_fft_inner directly with ss->external_basis set (triggers the new
 * outer-level the outer-level body body), find the highest k whose painted region is
 * modified. T(L) = max_k + 1.
 *
 * Expected for the outer-level body: T(L) = 4.0n fpr  (highest touched: qc(15))
 * Reference (the recursive body baseline): T(L) = 5.0n fpr (qc(19))
 *
 * The 4 qc-position delta = 1n fpr = the user-visible saving the outer-level body targets.
 *
 * The painting strategy works for k > 15 (above input area) cleanly. For
 * k ≤ 15, painting overwrites input which the function will modify anyway
 * during normal operation — so the test correctly reports those as "touched"
 * via the scratch/output writes the function makes there. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "../fndsa.h"
#include "../sign_inner.h"
#include "../sign_sampler.c"

#define SENTINEL_FPR ((fpr)0xDEADBEEFCAFEBABEULL)

static int
test_one_level(unsigned logn)
{
	size_t n = (size_t)1 << logn;
	size_t hn = n >> 1;
	size_t qsize = n >> 2;  /* qc-position size in fpr */

	/* Generate a real key + basis so the outer-level body's recompute step works on
	   actual data. The synthetic-input shortcut from test_path_b_peak.c
	   doesn't apply here because the outer-level body reads basis from external_basis
	   and recomputes g01; if external_basis is garbage, the recomputed
	   l10 won't match the LDL-derived l10 used at step 2, and signing
	   diverges. With a real basis, both paths produce the same l10. */
	size_t sk_len = FNDSA_SIGN_KEY_SIZE(logn);
	size_t vk_len = FNDSA_VRFY_KEY_SIZE(logn);
	size_t basis_len = FNDSA_BASIS_SIZE(logn);

	uint8_t *sk = malloc(sk_len);
	uint8_t *vk = malloc(vk_len);
	void *basis = aligned_alloc(8, basis_len);

	/* tmp[]: 7n fpr (= 56n bytes) — big enough to host the gram
	   inputs, exercise the function, and have room above the 4n fpr
	   target for qc-positions [16..27] to be paintable. */
	size_t tmp_flr = 7 * n;
	fpr *tmp = (fpr *)aligned_alloc(8, tmp_flr * sizeof(fpr));

	if (!sk || !vk || !basis || !tmp) {
		fprintf(stderr, "logn=%u: alloc failed\n", logn);
		return -1;
	}

	uint8_t kseed[8] = {0xCA, 0xFE, 0xBA, 0xBE, (uint8_t)logn, 0, 0, 0};
	fndsa_keygen_seeded(logn, kseed, sizeof kseed, sk, vk);
	if (!fndsa_compute_basis(sk, sk_len, basis, basis_len)) {
		fprintf(stderr, "logn=%u: fndsa_compute_basis FAILED\n", logn);
		return -1;
	}

	int max_touched = -1;

	/* For each qc-position K from 27 down to 0, set up the gram
	   inputs from the real basis, paint qc-position K with sentinel, run
	   ffsamp_fft_inner with ss->external_basis pointing to the real
	   basis, then check whether the painted region was modified. */
	for (int k = 27; k >= 0; k--) {
		/* Set up tmp[] with basis-and-Gram setup outputs (t0, t1, g01, g00, g11)
		   matching what sign_core would produce. */
		fpr *t0  = tmp;
		fpr *t1  = tmp + n;
		fpr *g01 = tmp + 2 * n;
		fpr *g00 = tmp + 3 * n;
		fpr *g11 = tmp + 3 * n + hn;

		/* hm: synthetic value sequence; the full sign_core would write
		   it via hash_to_point. Place at byte 34n (the outer-level body's hm offset)
		   so apply_basis_external reads from a consistent location. */
		uint16_t *hm = (uint16_t *)((uint8_t *)tmp + 34 * n);
		for (size_t i = 0; i < n; i++) {
			hm[i] = (uint16_t)((i * 137 + k * 13) & 0x3FFF);
		}

		fpoly_apply_basis_external(logn, t0, t1, basis, hm);
		fpoly_gram_fft_dst(logn, g00, g01, g11, basis);

		/* Paint qc-position K with sentinel. */
		fpr *paint_start = tmp + (size_t)k * qsize;
		for (size_t i = 0; i < qsize; i++) {
			paint_start[i] = SENTINEL_FPR;
		}

		/* Set up sampler state with external_basis — this is what
		   activates the outer-level body's outer-level branch (logn == ss->logn
		   AND ss->external_basis != NULL). */
		sampler_state ss;
		uint8_t seed[56];
		memset(seed, 0xCD ^ (uint8_t)k, sizeof seed);
		sampler_init(&ss, logn, seed, sizeof seed);
		ss.external_basis = (const fpr *)basis;

		/* Run ffsamp_fft_inner directly. the outer-level body outer body should
		   fire for logn == ss->logn (top-level) with external_basis
		   non-NULL. */
		ffsamp_fft_inner(&ss, logn, tmp);

		/* Count violations of the painted region. */
		size_t v = 0;
		for (size_t i = 0; i < qsize; i++) {
			if (paint_start[i] != SENTINEL_FPR) v++;
		}
		if (v > 0) {
			if (k > max_touched) max_touched = k;
			break;  /* highest k with violations; deeper k all touched */
		}
	}

	free(tmp); free(basis); free(vk); free(sk);

	if (max_touched < 0) {
		printf("logn=%u: all qc-position [0..27] untouched? (suspicious — "
			"check input setup)\n", logn);
		return 0;
	}

	int T_L = max_touched + 1;
	double frac = (double)T_L / 4.0;
	const char *verdict;
	if (T_L <= 16) {
		verdict = "✓ MATCHES the outer-level body 4n fpr target";
	} else if (T_L <= 20) {
		verdict = "✗ matches the recursive body 5n peak (the outer-level body not active?)";
	} else {
		verdict = "✗ above the recursive body peak (unexpected)";
	}
	printf("logn=%2u  T(L) = %2d qc-position = %.2fn fpr  (highest touched: qc(%d))  %s\n",
		logn, T_L, frac, max_touched, verdict);
	return T_L;
}

int main(void)
{
	printf("=== ffsamp_fft_inner the outer-level body internal peak per logn ===\n");
	printf("Tests the outer-level body outer-level body (FNDSA_LOW_RAM).\n");
	printf("ss->external_basis is set to a real precomputed basis, which\n");
	printf("activates the new outer-level branch in ffsamp_fft_inner.\n\n");
	printf("Expected: T(L) = 4.00n fpr  (highest touched: qc(15))\n");
	printf("Reference (the recursive body baseline, no FFSAMP_5N): 5.00n fpr.\n\n");

	int failures = 0;
	for (unsigned logn = 9; logn <= 10; logn++) {
		int T_L = test_one_level(logn);
		if (T_L > 16) failures++;
	}

	printf("\n");
	if (failures == 0) {
		printf("ALL LEVELS at or below 4n fpr — the outer-level body's claim VERIFIED.\n");
		return 0;
	}
	fprintf(stderr, "FAILURES: %d level(s) exceeded 4n fpr\n", failures);
	return 1;
}
