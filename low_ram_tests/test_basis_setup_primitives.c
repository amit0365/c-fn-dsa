/* Correctness test for the FNDSA_LOW_RAM primitives.
 *
 * fpoly_gram_fft_dst should produce mathematically identical output
 * to fpoly_gram_fft. This test verifies that for several logn values
 * by:
 *   1. Generating a random basis (b00..b11 in FFT-domain)
 *   2. Running fpoly_gram_fft in-place over a copy of the basis (reference)
 *   3. Running fpoly_gram_fft_dst against the original basis (test)
 *   4. Comparing reference vs test outputs bit-exactly
 *
 * Build:
 *   clang -DFNDSA_LOW_RAM=1 -DFNDSA_LOW_RAM=1 -O2 \
 *     -c test_basis_setup_primitives.c -o test_basis_setup_primitives.o
 *   clang -o test_basis_setup_primitives test_basis_setup_primitives.o \
 *     codec.o mq.o sha3.o sysrng.o util.o sign.o sign_core.o sign_fpoly.o \
 *     sign_fpr.o sign_sampler.o vrfy.o -lm
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "../sign_inner.h"

#define MAX_LOGN  10

/* Generate a deterministic pseudo-random basis (b00..b11) for testing.
   Values are chosen to be in a reasonable fpr range: roughly [0.1, 10]
   in absolute value, with mixed signs. */
static void
make_test_basis(unsigned logn, uint32_t seed, fpr *basis)
{
	size_t n = (size_t)1 << logn;
	uint32_t state = seed;
	for (size_t i = 0; i < 4 * n; i++) {
		state = state * 1664525u + 1013904223u;
		uint32_t bits = state;
		/* Map to a small fpr value. We use the FPR macro convention:
		   FPR(mantissa, exponent) = mantissa * 2^exponent.
		   For variety, encode a number in [-8, 8) range.
		   Mantissa in [2^52, 2^53-1], exponent in [-55, -50] */
		int64_t mant = ((int64_t)(bits & 0x1FFFFFFFFFFFFF))
			| ((int64_t)1 << 52);
		if (bits & 0x80000000) mant = -mant;
		int exp_val = -55 + ((int)((bits >> 24) & 0x7));
		basis[i] = (mant < 0)
			? (((uint64_t)1 << 63) | (uint64_t)(uint32_t)((-mant) & 0x000FFFFFFFFFFFFFLL) | ((uint64_t)((exp_val + 1075) & 0x7FF) << 52))
			: ((uint64_t)(mant & 0x000FFFFFFFFFFFFFLL) | ((uint64_t)((exp_val + 1075) & 0x7FF) << 52));
	}
}

static int
test_one_logn(unsigned logn)
{
	size_t n = (size_t)1 << logn;
	size_t hn = n >> 1;

	fpr *basis_orig = malloc(4 * n * sizeof(fpr));
	fpr *basis_copy = malloc(4 * n * sizeof(fpr));
	fpr *g00_dst = malloc(hn * sizeof(fpr));
	fpr *g01_dst = malloc(n * sizeof(fpr));
	fpr *g11_dst = malloc(hn * sizeof(fpr));

	make_test_basis(logn, 0xDEADBEEF + logn, basis_orig);
	memcpy(basis_copy, basis_orig, 4 * n * sizeof(fpr));

	/* Reference: in-place gram on basis_copy */
	fpr *b00 = basis_copy;
	fpr *b01 = b00 + n;
	fpr *b10 = b01 + n;
	fpr *b11 = b10 + n;
	fpoly_gram_fft(logn, b00, b01, b10, b11);
	/* After this:
	   b00 holds gram's g00 (full n fpr, but only first hn elements meaningful;
	     second hn should be zero per fpoly_gram_fft's output semantics)
	   b01 holds gram's g01 (full n fpr — re at b01[0..hn], im at b01[hn..n])
	   b10 holds gram's g11 (full n fpr, only first hn meaningful) */

	/* Test: fpoly_gram_fft_dst on basis_orig */
	fpoly_gram_fft_dst(logn, g00_dst, g01_dst, g11_dst, basis_orig);

	/* Compare. g00 and g11 from gram_fft are full n fpr but only the
	   first hn are meaningful. g00_dst and g11_dst are the compact half-size form. */
	int errors = 0;

	/* Check g00: first hn elements should match exactly */
	for (size_t i = 0; i < hn; i++) {
		if (b00[i] != g00_dst[i]) {
			fprintf(stderr,
				"logn=%u g00 mismatch at i=%zu: ref=0x%016llx test=0x%016llx\n",
				logn, i,
				(unsigned long long)b00[i],
				(unsigned long long)g00_dst[i]);
			errors++;
			if (errors >= 3) break;
		}
	}

	/* Check g01: full n elements (real at [0..hn), imag at [hn..n)) */
	for (size_t i = 0; i < n; i++) {
		if (b01[i] != g01_dst[i]) {
			fprintf(stderr,
				"logn=%u g01 mismatch at i=%zu: ref=0x%016llx test=0x%016llx\n",
				logn, i,
				(unsigned long long)b01[i],
				(unsigned long long)g01_dst[i]);
			errors++;
			if (errors >= 6) break;
		}
	}

	/* Check g11: first hn elements should match */
	for (size_t i = 0; i < hn; i++) {
		if (b10[i] != g11_dst[i]) {
			fprintf(stderr,
				"logn=%u g11 mismatch at i=%zu: ref=0x%016llx test=0x%016llx\n",
				logn, i,
				(unsigned long long)b10[i],
				(unsigned long long)g11_dst[i]);
			errors++;
			if (errors >= 9) break;
		}
	}

	/* Sanity: gram_fft should leave basis_orig untouched */
	int basis_modified = 0;
	for (size_t i = 0; i < 4 * n; i++) {
		uint32_t saved_state = 0xDEADBEEF + logn;
		(void)saved_state;
		/* Re-derive expected and compare — use make_test_basis again */
	}
	/* Easier: re-derive and compare */
	fpr *expected = malloc(4 * n * sizeof(fpr));
	make_test_basis(logn, 0xDEADBEEF + logn, expected);
	for (size_t i = 0; i < 4 * n; i++) {
		if (basis_orig[i] != expected[i]) {
			basis_modified = 1;
			break;
		}
	}
	free(expected);

	if (basis_modified) {
		fprintf(stderr,
			"logn=%u: fpoly_gram_fft_dst MODIFIED basis_orig (should be const)\n",
			logn);
		errors++;
	}

	free(g11_dst); free(g01_dst); free(g00_dst);
	free(basis_copy); free(basis_orig);

	if (errors == 0) {
		printf("PASS logn=%u (n=%zu): %zu g00 + %zu g01 + %zu g11 elements verified\n",
			logn, n, hn, n, hn);
		return 0;
	}
	return 1;
}

int main(void)
{
	printf("=== fpoly_gram_fft_dst correctness test ===\n");
	printf("Verifies fpoly_gram_fft_dst produces bit-identical output to fpoly_gram_fft.\n\n");

	int total_failures = 0;
	for (unsigned logn = 2; logn <= MAX_LOGN; logn++) {
		if (test_one_logn(logn) != 0) {
			total_failures++;
		}
	}

	printf("\n");
	if (total_failures == 0) {
		printf("ALL TESTS PASSED — primitives ready for sign_core wiring (Day 4)\n");
		return 0;
	} else {
		fprintf(stderr, "FAILURES: %d test case(s) failed\n", total_failures);
		return 1;
	}
}
