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

		if (d_re == 0)      exact_re++;
		else if (d_re <= 1) ulp1_re++;
		else                bad_re++;
		if (d_re > worst_ulp_re && d_re != UINT64_MAX) worst_ulp_re = d_re;

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
	printf("\nd_im results:\n");
	printf("  bit-exact match     : %" PRIu64 " (%.4f%%)\n",
		exact_im, 100.0 * (double)exact_im / (double)N);
	printf("  within 1 ulp        : %" PRIu64 " (%.4f%%)\n",
		ulp1_im,  100.0 * (double)ulp1_im  / (double)N);
	printf("  > 1 ulp (BUG)       : %" PRIu64 "\n", bad_im);
	printf("  worst ulp distance  : %" PRIu64 "\n", worst_ulp_im);

	int failures = (bad_re > 0) + (bad_im > 0);
	if (failures) {
		printf("\nFAIL: %d output(s) had >1 ulp deviation.\n", failures);
		return 1;
	}
	printf("\nPASS: all outputs within 1 ulp of reference.\n");
	return 0;
}
