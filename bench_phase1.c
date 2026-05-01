/* Phase 1 micro-bench: time basis_to_FFT and fpoly_gram_fft separately,
 * report each as a fraction of total sign time.
 *
 * Why this matters: if Path B + phase 1 reduction (option 5) is deployed
 * with the basis in flash (memory-mapped), per-sign work is:
 *   - SKIP basis_to_FFT  (was per-sign cost)
 *   - gram_fft reads basis FROM FLASH (slower than RAM)
 *
 * Net perf delta = (basis_to_FFT time saved) - (gram_fft basis-read overhead).
 *
 * On host (macOS arm64) we can't simulate flash slowness directly. This
 * bench reports the absolute time of each phase. To estimate per-sign perf
 * on actual hardware:
 *
 *   sign_with_flash_basis_estimate(K_flash_to_ram_slowdown)
 *     ≈ baseline_sign
 *       - basis_to_FFT_time                           // saved
 *       + (gram_fft_time × K_flash_to_ram_slowdown)   // slower reads
 *
 * Typical K values:
 *   ST33K1M5 on-chip flash: K ≈ 2-3 (close to RAM speed)
 *   External SPI flash:     K ≈ 50-100 (much slower)
 *
 * If K = 3 (typical on-chip flash) and basis_to_FFT >> gram_fft, even
 * 3x slower gram is a net win. The ratio reported by this bench tells
 * you the breakeven K.
 *
 * Build:
 *   clang -O3 -DFNDSA_PATH_B=1 -c bench_phase1.c -o bench_phase1.o
 *   clang -O3 -o bench_phase1 bench_phase1.o codec.o mq.o sha3.o sysrng.o \
 *     util.o kgen.o kgen_fxp.o kgen_gauss.o kgen_mp31.o kgen_ntru.o \
 *     kgen_poly.o kgen_zint31.o sign.o sign_core.o sign_fpoly.o sign_fpr.o \
 *     sign_sampler.o vrfy.o -lm
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "fndsa.h"
#include "sign_inner.h"

/* Forward declarations of internal functions we'll bench. */
static void basis_to_FFT_local(unsigned logn,
	const int8_t *f, const int8_t *g, const int8_t *F, const int8_t *G,
	fpr *dst);

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

/* Reproduce basis_to_FFT here (it's static in sign_core.c). */
static void
basis_to_FFT_local(unsigned logn,
	const int8_t *f, const int8_t *g, const int8_t *F, const int8_t *G,
	fpr *dst)
{
	size_t n = (size_t)1 << logn;
	fpr *b00 = dst;
	fpr *b01 = b00 + n;
	fpr *b10 = b01 + n;
	fpr *b11 = b10 + n;
	fpoly_set_small(logn, b01, f);
	fpoly_set_small(logn, b00, g);
	fpoly_set_small(logn, b11, F);
	fpoly_set_small(logn, b10, G);
	fpoly_FFT(logn, b01);
	fpoly_FFT(logn, b00);
	fpoly_FFT(logn, b11);
	fpoly_FFT(logn, b10);
	fpoly_neg(logn, b01);
	fpoly_neg(logn, b11);
}

/* Time basis_to_FFT alone, repeated, report median. */
static double
bench_basis_to_FFT(unsigned logn, int n_iter)
{
	size_t n = (size_t)1 << logn;
	int8_t *f = malloc(n);
	int8_t *g = malloc(n);
	int8_t *F = malloc(n);
	int8_t *G = malloc(n);
	fpr *dst = malloc(4 * n * sizeof(fpr));

	/* Fill with arbitrary small values (basis_to_FFT just does FFT,
	   doesn't care about validity). */
	for (size_t i = 0; i < n; i++) {
		f[i] = (int8_t)((i & 7) - 3);
		g[i] = (int8_t)(((i + 1) & 7) - 3);
		F[i] = (int8_t)(((i + 2) & 7) - 3);
		G[i] = (int8_t)(((i + 3) & 7) - 3);
	}

	double *samples = malloc((size_t)n_iter * sizeof(double));
	for (int i = 0; i < n_iter; i++) {
		double t0 = ns_now();
		basis_to_FFT_local(logn, f, g, F, G, dst);
		double t1 = ns_now();
		samples[i] = t1 - t0;
	}
	qsort(samples, n_iter, sizeof(double), cmp_dbl);
	double median = samples[n_iter / 2];
	free(samples); free(dst); free(G); free(F); free(g); free(f);
	return median;
}

/* Time fpoly_gram_fft alone, repeated, report median. */
static double
bench_gram_fft(unsigned logn, int n_iter)
{
	size_t n = (size_t)1 << logn;
	fpr *b00 = malloc(n * sizeof(fpr));
	fpr *b01 = malloc(n * sizeof(fpr));
	fpr *b10 = malloc(n * sizeof(fpr));
	fpr *b11 = malloc(n * sizeof(fpr));

	/* Fill with arbitrary FLR values. gram_fft is pure arithmetic
	   (no validation), timing is independent of input distribution. */
	for (size_t i = 0; i < n; i++) {
		b00[i] = FPR(4503599627370496LL, -52);
		b01[i] = FPR(4503599627370496LL, -52);
		b10[i] = FPR(4503599627370496LL, -52);
		b11[i] = FPR(4503599627370496LL, -52);
	}

	double *samples = malloc((size_t)n_iter * sizeof(double));
	for (int i = 0; i < n_iter; i++) {
		/* gram is destructive on b00, b01, b10. Restore before each
		   iteration so timing is uniform. */
		for (size_t j = 0; j < n; j++) {
			b00[j] = FPR(4503599627370496LL, -52);
			b01[j] = FPR(4503599627370496LL, -52);
			b10[j] = FPR(4503599627370496LL, -52);
		}
		double t0 = ns_now();
		fpoly_gram_fft(logn, b00, b01, b10, b11);
		double t1 = ns_now();
		samples[i] = t1 - t0;
	}
	qsort(samples, n_iter, sizeof(double), cmp_dbl);
	double median = samples[n_iter / 2];
	free(samples); free(b11); free(b10); free(b01); free(b00);
	return median;
}

/* Time full sign for context. */
static double
bench_sign(unsigned logn, int n_iter)
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
		fndsa_sign_seeded(sk, sk_len, NULL, 0,
			FNDSA_HASH_ID_RAW, "test", 4,
			mseed, sizeof mseed, sig, sig_len_max);
		double t1 = ns_now();
		samples[i] = t1 - t0;
	}
	qsort(samples, n_iter, sizeof(double), cmp_dbl);
	double median = samples[n_iter / 2];
	free(samples); free(sig); free(vk); free(sk);
	return median;
}

static void
report(unsigned logn)
{
	int sign_iter = (logn == 10) ? 50 : 100;
	int piece_iter = 200;

	double t_sign  = bench_sign(logn, sign_iter);
	double t_basis = bench_basis_to_FFT(logn, piece_iter);
	double t_gram  = bench_gram_fft(logn, piece_iter);

	double frac_basis = t_basis / t_sign * 100.0;
	double frac_gram  = t_gram  / t_sign * 100.0;

	printf("logn=%u  n=%u\n", logn, 1u << logn);
	printf("  Total sign:        %8.0f ns  (100.0%%)\n", t_sign);
	printf("  basis_to_FFT:      %8.0f ns  (%5.2f%%)  [SAVED with precomputed basis]\n",
		t_basis, frac_basis);
	printf("  fpoly_gram_fft:    %8.0f ns  (%5.2f%%)  [SLOWER if basis in flash]\n",
		t_gram, frac_gram);

	/* Estimate sign time under option 5 with various flash slowdown
	   factors K (i.e. flash access is K times slower than RAM). */
	printf("  Estimated sign with option 5 (basis in flash):\n");
	for (int K = 1; K <= 10; K++) {
		double est = t_sign - t_basis + t_gram * K;
		double delta_pct = (est - t_sign) / t_sign * 100.0;
		const char *verdict = (est < t_sign) ? "WIN" :
			(est < t_sign * 1.05) ? "wash" : "LOSS";
		printf("    K=%2d:  %8.0f ns  (%+5.1f%%)  %s\n",
			K, est, delta_pct, verdict);
	}
	printf("\n");

	/* Compute breakeven K analytically: K_break = (basis_time / gram_time) + 1
	   (sign-time at this K equals baseline). */
	double k_break = (t_basis / t_gram) + 1.0;
	printf("  Breakeven K (perf neutral):  K = %.2f\n", k_break);
	printf("  (At K below breakeven: option 5 is a perf win.)\n");
	printf("\n");
}

int
main(void)
{
	printf("=== Phase 1 micro-bench (option 5 perf estimation) ===\n");
	printf("Timing basis_to_FFT and fpoly_gram_fft separately to estimate\n");
	printf("per-sign perf when basis is precomputed and stored in flash.\n");
	printf("(macOS arm64 host; ARM PMU is privileged so wall-time only.)\n\n");

	report(9);
	report(10);

	printf("Interpretation:\n");
	printf("  If your target's flash:RAM access ratio K is below the breakeven\n");
	printf("  shown above, option 5 (precomputed basis in flash) is a perf win\n");
	printf("  AND saves 6n bytes (3/6 KiB at logn=9/10) of per-sign RAM.\n");
	printf("  Typical K values:\n");
	printf("    ST33K1M5 on-chip flash, basic read: K ≈ 2-3\n");
	printf("    External SPI flash, cached:         K ≈ 5-20\n");
	printf("    External SPI flash, uncached:       K ≈ 50-100\n");

	return 0;
}
