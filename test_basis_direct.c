/*
 * Phase 0: math verification for basis-direct post-ffsamp.
 *
 * Proves that:
 *   v0 = t0 * b00 + t1 * b10   (basis-direct)
 *      = t0 * FFT(g) + t1 * FFT(G)   (current method)
 *   v1 = t0 * b01 + t1 * b11   (basis-direct)
 *      = -t0 * FFT(f) - t1 * FFT(F)   (current method)
 *
 * Generates a real key, computes basis via fndsa_compute_basis (matches
 * how production sign builds basis), then runs both methods on identical
 * t0/t1 inputs and bit-compares the (v0, v1) outputs in FFT form.
 *
 * If this test passes, the basis-direct optimization is mathematically
 * sound and we can proceed to Phase 1 (sign_core.c integration). If it
 * fails, we abort the kill plan.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "fndsa.h"
#include "inner.h"
#include "sign_inner.h"

#if FNDSA_LOW_RAM

#define LOGN 9
#define N    (1u << LOGN)

/* Decode f, g, F from encoded sk (mirrors sign_step1's logic). */
static void
decode_sk(const uint8_t *sk, unsigned logn,
	int8_t *f, int8_t *g, int8_t *F)
{
	unsigned nbits;
	switch (logn) {
	case 8: case 9: nbits = 6; break;
	case 10: nbits = 5; break;
	default: nbits = 8; break;
	}
	size_t k, j = 1;
	k = trim_i8_decode(logn, sk + j, f, nbits);
	j += k;
	k = trim_i8_decode(logn, sk + j, g, nbits);
	j += k;
	(void)trim_i8_decode(logn, sk + j, F, 8);
}

/* Method A: current sign_core post-ffsamp logic. Computes v0, v1 in FFT
 * form via int8 → set_small → FFT → mul. */
static void
method_a_current(unsigned logn,
	const int8_t *f, const int8_t *g,
	const int8_t *F, const int8_t *G,
	const fpr *t0_in, const fpr *t1_in,
	fpr *v0_out, fpr *v1_out,
	fpr *scratch_w0, fpr *scratch_w1,
	fpr *scratch_t0, fpr *scratch_t1)
{
	size_t n = (size_t)1 << logn;

	/* Copy inputs (we'll mutate them) */
	memcpy(scratch_t0, t0_in, n * sizeof(fpr));
	memcpy(scratch_t1, t1_in, n * sizeof(fpr));

	/* Mirror sign_core.c lines 439-453 exactly */
	fpoly_set_small(logn, scratch_w0, g);
	fpoly_set_small(logn, scratch_w1, f);
	fpoly_FFT(logn, scratch_w0);
	fpoly_FFT(logn, scratch_w1);
	fpoly_mul_fft(logn, scratch_w1, scratch_t0);
	fpoly_mul_fft(logn, scratch_t0, scratch_w0);
	fpoly_set_small(logn, scratch_w0, G);
	fpoly_FFT(logn, scratch_w0);
	fpoly_mul_fft(logn, scratch_w0, scratch_t1);
	fpoly_add(logn, scratch_t0, scratch_w0);
	/* scratch_t0 = t0*g + t1*G = v0 */
	memcpy(v0_out, scratch_t0, n * sizeof(fpr));

	fpoly_set_small(logn, scratch_w0, F);
	fpoly_FFT(logn, scratch_w0);
	fpoly_mul_fft(logn, scratch_t1, scratch_w0);
	fpoly_add(logn, scratch_t1, scratch_w1);
	fpoly_neg(logn, scratch_t1);
	/* scratch_t1 = -(t1*F + f*t0) = -t0*f - t1*F = v1 */
	memcpy(v1_out, scratch_t1, n * sizeof(fpr));
}

/* Method B: basis-direct. Read basis components from external_basis
 * (FFT form already), multiply directly. */
static void
method_b_basis_direct(unsigned logn,
	const fpr *basis,
	const fpr *t0_in, const fpr *t1_in,
	fpr *v0_out, fpr *v1_out,
	fpr *scratch_t0, fpr *scratch_t1_a, fpr *scratch_t1_b)
{
	size_t n = (size_t)1 << logn;
	const fpr *b00 = basis;
	const fpr *b01 = basis + n;
	const fpr *b10 = basis + 2 * n;
	const fpr *b11 = basis + 3 * n;

	/* v0 = t0*b00 + t1*b10 */
	memcpy(scratch_t0, t0_in, n * sizeof(fpr));
	memcpy(scratch_t1_a, t1_in, n * sizeof(fpr));
	fpoly_mul_fft(logn, scratch_t0, b00);
	fpoly_mul_fft(logn, scratch_t1_a, b10);
	fpoly_add(logn, scratch_t0, scratch_t1_a);
	memcpy(v0_out, scratch_t0, n * sizeof(fpr));

	/* v1 = t0*b01 + t1*b11 */
	memcpy(scratch_t0, t0_in, n * sizeof(fpr));
	memcpy(scratch_t1_b, t1_in, n * sizeof(fpr));
	fpoly_mul_fft(logn, scratch_t0, b01);
	fpoly_mul_fft(logn, scratch_t1_b, b11);
	fpoly_add(logn, scratch_t0, scratch_t1_b);
	memcpy(v1_out, scratch_t0, n * sizeof(fpr));
}

/* Compute G the way sign_step1 does, given f, g, F. */
static int
compute_G_from_fgF(unsigned logn,
	const int8_t *f, const int8_t *g, const int8_t *F,
	int8_t *G_out, uint16_t *t0_scratch, uint16_t *t1_scratch)
{
	mqpoly_small_to_int(logn, g, t0_scratch);
	mqpoly_small_to_int(logn, f, t1_scratch);
	mqpoly_int_to_ntt(logn, t0_scratch);
	mqpoly_int_to_ntt(logn, t1_scratch);
	if (!mqpoly_div_ntt(logn, t0_scratch, t1_scratch)) {
		return 0;
	}
	mqpoly_small_to_int(logn, F, t1_scratch);
	mqpoly_int_to_ntt(logn, t1_scratch);
	mqpoly_mul_ntt(logn, t1_scratch, t0_scratch);
	mqpoly_ntt_to_int(logn, t1_scratch);
	if (!mqpoly_int_to_small(logn, t1_scratch, G_out)) {
		return 0;
	}
	return 1;
}

static int
run_test(unsigned logn)
{
	size_t n = (size_t)1 << logn;
	size_t sk_len = FNDSA_SIGN_KEY_SIZE(logn);
	size_t vk_len = FNDSA_VRFY_KEY_SIZE(logn);
	size_t basis_len = FNDSA_BASIS_SIZE(logn);

	uint8_t *sk = malloc(sk_len);
	uint8_t *vk = malloc(vk_len);
	uint8_t *basis = aligned_alloc(8, (basis_len + 7) & ~(size_t)7);

	uint8_t kseed[32];
	for (size_t i = 0; i < sizeof kseed; i++) {
		kseed[i] = (uint8_t)(i + logn * 7);
	}
	fndsa_keygen_seeded(logn, kseed, sizeof kseed, sk, vk);

	if (!fndsa_compute_basis(sk, sk_len, basis, basis_len)) {
		fprintf(stderr, "[logn=%u] compute_basis failed\n", logn);
		return 1;
	}

	/* Decode f, g, F from sk and compute G (mirrors sign_step1). */
	int8_t *f = malloc(n);
	int8_t *g = malloc(n);
	int8_t *F = malloc(n);
	int8_t *G = malloc(n);
	decode_sk(sk, logn, f, g, F);

	uint16_t *t0_scratch = aligned_alloc(8,
		((n * 2 + 7) & ~(size_t)7));
	uint16_t *t1_scratch = aligned_alloc(8,
		((n * 2 + 7) & ~(size_t)7));
	if (!compute_G_from_fgF(logn, f, g, F, G,
		t0_scratch, t1_scratch))
	{
		fprintf(stderr, "[logn=%u] G derivation failed\n", logn);
		return 1;
	}

	/* Construct test t0, t1 inputs in FFT form. We use deterministic
	 * pseudo-random fpr values that resemble what ffsamp would output
	 * (small integer-ish lattice points after the sampler). */
	fpr *t0 = aligned_alloc(8, n * sizeof(fpr));
	fpr *t1 = aligned_alloc(8, n * sizeof(fpr));
	for (size_t i = 0; i < n; i++) {
		double v0 = (double)((int)((i * 31 + logn) % 17) - 8);
		double v1 = (double)((int)((i * 47 + logn) % 19) - 9);
		memcpy(&t0[i], &v0, sizeof(fpr));
		memcpy(&t1[i], &v1, sizeof(fpr));
	}
	/* Convert to FFT form */
	fpoly_FFT(logn, t0);
	fpoly_FFT(logn, t1);

	/* Method A scratch */
	fpr *a_v0 = aligned_alloc(8, n * sizeof(fpr));
	fpr *a_v1 = aligned_alloc(8, n * sizeof(fpr));
	fpr *a_w0 = aligned_alloc(8, n * sizeof(fpr));
	fpr *a_w1 = aligned_alloc(8, n * sizeof(fpr));
	fpr *a_t0 = aligned_alloc(8, n * sizeof(fpr));
	fpr *a_t1 = aligned_alloc(8, n * sizeof(fpr));

	/* Method B scratch */
	fpr *b_v0 = aligned_alloc(8, n * sizeof(fpr));
	fpr *b_v1 = aligned_alloc(8, n * sizeof(fpr));
	fpr *b_t0 = aligned_alloc(8, n * sizeof(fpr));
	fpr *b_t1a = aligned_alloc(8, n * sizeof(fpr));
	fpr *b_t1b = aligned_alloc(8, n * sizeof(fpr));

	method_a_current(logn, f, g, F, G, t0, t1,
		a_v0, a_v1, a_w0, a_w1, a_t0, a_t1);
	method_b_basis_direct(logn, (const fpr *)basis, t0, t1,
		b_v0, b_v1, b_t0, b_t1a, b_t1b);

	/* Bit-compare outputs */
	int v0_match = (memcmp(a_v0, b_v0, n * sizeof(fpr)) == 0);
	int v1_match = (memcmp(a_v1, b_v1, n * sizeof(fpr)) == 0);

	if (!v0_match) {
		fprintf(stderr, "[logn=%u] v0 MISMATCH\n", logn);
		size_t mismatch_count = 0;
		double max_abs_diff = 0.0;
		for (size_t i = 0; i < n; i++) {
			double a_d, b_d;
			memcpy(&a_d, &a_v0[i], sizeof(fpr));
			memcpy(&b_d, &b_v0[i], sizeof(fpr));
			double diff = a_d - b_d;
			if (a_v0[i] != b_v0[i]) {
				mismatch_count++;
				double abs_diff =
					(diff < 0) ? -diff : diff;
				if (abs_diff > max_abs_diff) {
					max_abs_diff = abs_diff;
				}
			}
		}
		fprintf(stderr, "  %zu/%zu coeffs differ, max |diff|=%g\n",
			mismatch_count, n, max_abs_diff);
	}
	if (!v1_match) {
		fprintf(stderr, "[logn=%u] v1 MISMATCH\n", logn);
		size_t mismatch_count = 0;
		double max_abs_diff = 0.0;
		for (size_t i = 0; i < n; i++) {
			double a_d, b_d;
			memcpy(&a_d, &a_v1[i], sizeof(fpr));
			memcpy(&b_d, &b_v1[i], sizeof(fpr));
			double diff = a_d - b_d;
			if (a_v1[i] != b_v1[i]) {
				mismatch_count++;
				double abs_diff =
					(diff < 0) ? -diff : diff;
				if (abs_diff > max_abs_diff) {
					max_abs_diff = abs_diff;
				}
			}
		}
		fprintf(stderr, "  %zu/%zu coeffs differ, max |diff|=%g\n",
			mismatch_count, n, max_abs_diff);
	}

	int ok = v0_match && v1_match;
	printf("[logn=%u] v0_match=%s v1_match=%s -> %s\n",
		logn, v0_match ? "YES" : "NO", v1_match ? "YES" : "NO",
		ok ? "PASS" : "FAIL");

	free(sk); free(vk); free(basis);
	free(f); free(g); free(F); free(G);
	free(t0_scratch); free(t1_scratch);
	free(t0); free(t1);
	free(a_v0); free(a_v1); free(a_w0); free(a_w1); free(a_t0); free(a_t1);
	free(b_v0); free(b_v1); free(b_t0); free(b_t1a); free(b_t1b);

	return ok ? 0 : 1;
}

int
main(void)
{
	if (run_test(9) != 0) return 1;
	if (run_test(10) != 0) return 1;
	printf("Phase 0 PASSED — basis-direct math is correct.\n");
	return 0;
}

#else

int
main(void)
{
	printf("SKIP: requires FNDSA_LOW_RAM=1\n");
	return 0;
}

#endif
