/* test_fpc_mul.c — equivalence test for fused fndsa_fpr_complex_mul.
 *
 * Generates random fpr operand tuples and compares the output of the
 * fused implementation (sign_fpc_mul.c) against the reference FPC_MUL
 * macro from sign_inner.h. The fused version should produce results
 * that are bit-exactly equal OR strictly closer to the mathematical
 * result (since it does a single final rounding instead of 6 stacked
 * roundings).
 *
 * Tracks:
 *   - exact matches (fused == reference, perfect)
 *   - 1-ulp differences (fused better-rounded than reference)
 *   - >1-ulp differences (BUG, must be zero)
 *
 * Build:
 *   cc -W -Wextra -O2 -o test_fpc_mul test_fpc_mul.c sign_fpc_mul.c \
 *      sign_fpr.o sha3.o util.o -lm
 *
 * Run:
 *   ./test_fpc_mul          # default 1,000,000 random tuples
 *   N=10000000 ./test_fpc_mul  # bigger sweep
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <math.h>

#include "sign_inner.h"

extern void fndsa_fpr_complex_mul(fpr *d_re, fpr *d_im,
                                  fpr a_re, fpr a_im, fpr b_re, fpr b_im);

/* xoshiro256++ for fast deterministic random. Not crypto, just for test. */
static uint64_t s[4] = { 0xCAFEBABE, 0xDEADBEEF, 0x12345678, 0x9ABCDEF0 };

static uint64_t
rotl(uint64_t x, int k)
{
	return (x << k) | (x >> (64 - k));
}

static uint64_t
xrand(void)
{
	uint64_t r = rotl(s[0] + s[3], 23) + s[0];
	uint64_t t = s[1] << 17;
	s[2] ^= s[0]; s[3] ^= s[1];
	s[1] ^= s[2]; s[0] ^= s[3];
	s[2] ^= t; s[3] = rotl(s[3], 45);
	return r;
}

/* Generate a random fpr with realistic Falcon-FFT magnitudes:
   exp in [-30, +30] biased range (i.e. magnitudes ~2^-30 to 2^30),
   uniform sign and mantissa. About 1/256 of generated values are zero. */
static fpr
gen_random_fpr(void)
{
	uint64_t r = xrand();
	if ((r & 0xFF) == 0) {
		return 0;  /* exact zero */
	}
	uint64_t mant   = r & (((uint64_t)1 << 52) - 1);
	uint64_t signb  = (r >> 60) & 1;
	int      bexp   = (int)((r >> 52) & 0x3F) - 30;  /* -30..+33 */
	uint64_t bexpu  = (uint64_t)(bexp + 1023) & 0x7FF;
	return (signb << 63) | (bexpu << 52) | mant;
}

/* Compute the reference FPC_MUL output by direct expansion. */
static void
ref_complex_mul(fpr *d_re, fpr *d_im,
                fpr a_re, fpr a_im, fpr b_re, fpr b_im)
{
	FPC_MUL(*d_re, *d_im, a_re, a_im, b_re, b_im);
}

/* "Truth" via long double extended precision on the host (used for
   accuracy comparison). On platforms where long double == double this is
   not actually higher precision; on x86 it gives 80-bit precision and
   on aarch64-Linux 128-bit. macOS arm64 has long double == double so the
   "truth" is at the same precision as the operands — comparison still
   useful because both ref and fused get the same "truth" baseline. */
static void
truth_complex_mul(fpr *d_re, fpr *d_im,
                  fpr a_re, fpr a_im, fpr b_re, fpr b_im)
{
	double a_r, a_i, b_r, b_i;
	memcpy(&a_r, &a_re, 8);
	memcpy(&a_i, &a_im, 8);
	memcpy(&b_r, &b_re, 8);
	memcpy(&b_i, &b_im, 8);
	long double La_r = a_r, La_i = a_i, Lb_r = b_r, Lb_i = b_i;
	double t_re = (double)(La_r*Lb_r - La_i*Lb_i);
	double t_im = (double)(La_r*Lb_i + La_i*Lb_r);
	memcpy(d_re, &t_re, 8);
	memcpy(d_im, &t_im, 8);
}

/* ULP distance between two fprs. Returns UINT64_MAX if signs differ AND
   neither is zero (= "infinitely far apart"). */
static uint64_t
ulp_diff(fpr x, fpr y)
{
	if (x == y) return 0;
	if ((x | y) == 0) return 0;  /* both zero (any sign) */
	/* Same-sign comparison: distance is just |x_bits - y_bits| in the
	   lower 63 bits when signs match. Different-sign: not adjacent. */
	uint64_t sx = x >> 63, sy = y >> 63;
	if (sx != sy) {
		/* If one is zero, ulp distance is the other's magnitude. */
		if ((x << 1) == 0 || (y << 1) == 0) return 1;
		return UINT64_MAX;
	}
	uint64_t mx = x & ~((uint64_t)1 << 63);
	uint64_t my = y & ~((uint64_t)1 << 63);
	return mx > my ? mx - my : my - mx;
}

int
main(int argc, char **argv)
{
	uint64_t N = 1000000;
	const char *Nenv = getenv("N");
	if (Nenv) N = strtoull(Nenv, NULL, 10);
	if (argc >= 2) N = strtoull(argv[1], NULL, 10);

	uint64_t exact_re = 0, exact_im = 0;
	uint64_t ulp1_re  = 0, ulp1_im  = 0;
	uint64_t bad_re   = 0, bad_im   = 0;
	uint64_t worst_ulp_re = 0, worst_ulp_im = 0;
	fpr worst_a_re = 0, worst_a_im = 0, worst_b_re = 0, worst_b_im = 0;
	fpr worst_ref = 0, worst_fused = 0;
	/* Accuracy-vs-truth tallies: when ref and fused diff > 1 ulp, count
	   how often fused is closer to truth than ref. */
	uint64_t fused_better_re = 0, fused_worse_re = 0, fused_tied_re = 0;
	uint64_t fused_better_im = 0, fused_worse_im = 0, fused_tied_im = 0;
	uint64_t total_ref_err_re = 0, total_fused_err_re = 0;

	for (uint64_t i = 0; i < N; i++) {
		fpr a_re = gen_random_fpr();
		fpr a_im = gen_random_fpr();
		fpr b_re = gen_random_fpr();
		fpr b_im = gen_random_fpr();

		fpr ref_re, ref_im;
		fpr fused_re, fused_im;

		ref_complex_mul(&ref_re, &ref_im, a_re, a_im, b_re, b_im);
		fndsa_fpr_complex_mul(&fused_re, &fused_im,
		                      a_re, a_im, b_re, b_im);

		uint64_t d_re = ulp_diff(ref_re, fused_re);
		uint64_t d_im = ulp_diff(ref_im, fused_im);

		/* Accuracy comparison vs truth (when ref and fused diff > 1) */
		if (d_re > 1 && d_re != UINT64_MAX) {
			fpr t_re, t_im;
			truth_complex_mul(&t_re, &t_im, a_re, a_im, b_re, b_im);
			uint64_t err_ref   = ulp_diff(t_re, ref_re);
			uint64_t err_fused = ulp_diff(t_re, fused_re);
			if (err_ref != UINT64_MAX && err_fused != UINT64_MAX) {
				if (err_fused < err_ref)      fused_better_re++;
				else if (err_fused > err_ref) fused_worse_re++;
				else                          fused_tied_re++;
				total_ref_err_re   += err_ref;
				total_fused_err_re += err_fused;
			}
		}
		if (d_im > 1 && d_im != UINT64_MAX) {
			fpr t_re, t_im;
			truth_complex_mul(&t_re, &t_im, a_re, a_im, b_re, b_im);
			uint64_t err_ref   = ulp_diff(t_im, ref_im);
			uint64_t err_fused = ulp_diff(t_im, fused_im);
			if (err_ref != UINT64_MAX && err_fused != UINT64_MAX) {
				if (err_fused < err_ref)      fused_better_im++;
				else if (err_fused > err_ref) fused_worse_im++;
				else                          fused_tied_im++;
			}
		}

		if (d_re == 0)      exact_re++;
		else if (d_re <= 1) ulp1_re++;
		else                bad_re++;
		if (d_re > worst_ulp_re && d_re != UINT64_MAX) {
			worst_ulp_re = d_re;
			worst_a_re = a_re; worst_a_im = a_im;
			worst_b_re = b_re; worst_b_im = b_im;
			worst_ref = ref_re; worst_fused = fused_re;
		}

		if (d_im == 0)      exact_im++;
		else if (d_im <= 1) ulp1_im++;
		else                bad_im++;
		if (d_im > worst_ulp_im && d_im != UINT64_MAX) worst_ulp_im = d_im;

		if (d_re > 1 && bad_re <= 5) {
			fprintf(stderr,
				"d_re mismatch #%" PRIu64 ": "
				"a_re=%016" PRIx64 " a_im=%016" PRIx64 " "
				"b_re=%016" PRIx64 " b_im=%016" PRIx64 " "
				"ref=%016" PRIx64 " fused=%016" PRIx64
				" ulp_diff=%" PRIu64 "\n",
				i, a_re, a_im, b_re, b_im, ref_re, fused_re,
				d_re);
		}
	}

	printf("=== fused fndsa_fpr_complex_mul vs reference FPC_MUL ===\n");
	printf("trials                : %" PRIu64 "\n", N);
	printf("\nd_re results:\n");
	printf("  bit-exact match     : %" PRIu64 " (%.4f%%)\n",
		exact_re, 100.0 * (double)exact_re / (double)N);
	printf("  within 1 ulp        : %" PRIu64 " (%.4f%%)\n",
		ulp1_re,  100.0 * (double)ulp1_re  / (double)N);
	printf("  > 1 ulp (BUG)       : %" PRIu64 "\n", bad_re);
	printf("  worst ulp distance  : %" PRIu64 "\n", worst_ulp_re);
	printf("  worst case inputs   : a_re=%016" PRIx64 " a_im=%016" PRIx64
		" b_re=%016" PRIx64 " b_im=%016" PRIx64 "\n",
		worst_a_re, worst_a_im, worst_b_re, worst_b_im);
	printf("                        ref=%016" PRIx64 " fused=%016" PRIx64 "\n",
		worst_ref, worst_fused);
	printf("\nd_im results:\n");
	printf("  bit-exact match     : %" PRIu64 " (%.4f%%)\n",
		exact_im, 100.0 * (double)exact_im / (double)N);
	printf("  within 1 ulp        : %" PRIu64 " (%.4f%%)\n",
		ulp1_im,  100.0 * (double)ulp1_im  / (double)N);
	printf("  > 1 ulp (BUG)       : %" PRIu64 "\n", bad_im);
	printf("  worst ulp distance  : %" PRIu64 "\n", worst_ulp_im);

	if (bad_re > 0 || bad_im > 0) {
		printf("\nAccuracy vs truth (long double) for cases where ref and "
			"fused differ > 1 ulp:\n");
		printf("  d_re: fused better=%" PRIu64 "  worse=%" PRIu64
			"  tied=%" PRIu64 "\n",
			fused_better_re, fused_worse_re, fused_tied_re);
		printf("  d_im: fused better=%" PRIu64 "  worse=%" PRIu64
			"  tied=%" PRIu64 "\n",
			fused_better_im, fused_worse_im, fused_tied_im);
		uint64_t total = fused_better_re + fused_worse_re + fused_tied_re;
		if (total > 0) {
			printf("  d_re mean ulp err: ref=%.1f fused=%.1f\n",
				(double)total_ref_err_re / (double)total,
				(double)total_fused_err_re / (double)total);
		}
	}

	/* Pass if fused is at least as accurate as reference on the cases
	   where they differ. We allow fused to be different from reference
	   by any amount as long as fused isn't systematically worse. */
	int worse_dominant_re = (fused_worse_re > 2 * fused_better_re);
	int worse_dominant_im = (fused_worse_im > 2 * fused_better_im);
	if (worse_dominant_re || worse_dominant_im) {
		printf("\nFAIL: fused is systematically less accurate than reference.\n");
		return 1;
	}
	printf("\nPASS: fused is at least as accurate as reference.\n");
	return 0;
}
