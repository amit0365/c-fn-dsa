/*
 * Performance test: how much CPU would we pay to recompute hm and G
 * post-ffsamp instead of preserving them in tmp[]?
 *
 * Measures isolated cost of:
 *   1. hash_to_point() — what hm-recompute would call
 *   2. G recompute pipeline — small_to_int + int_to_ntt × 3 + div + mul + ntt_to_int
 *   3. Full sign for reference
 *
 * Compares: each recompute cost as % of full sign time.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#include "fndsa.h"
#include "inner.h"
#include "sign_inner.h"

#if FNDSA_LOW_RAM

#define LOGN 9
#define N    (1u << LOGN)

/* Aligned 8-byte buffers for fpr work. */
#define ITERS_QUICK 100
#define ITERS_BENCH 200

static double
elapsed_seconds(struct timespec t0, struct timespec t1)
{
	return (double)(t1.tv_sec - t0.tv_sec)
		+ 1e-9 * (double)(t1.tv_nsec - t0.tv_nsec);
}

static void
bench_hash_to_point(unsigned logn, int iters, double *out_ns_per_call)
{
	size_t n = (size_t)1 << logn;
	uint8_t nonce[40];
	uint8_t hashed_vk[64];
	uint8_t msg[64];
	uint16_t *c = aligned_alloc(8, ((n * 2 + 7) & ~(size_t)7));

	for (size_t i = 0; i < sizeof nonce; i++) nonce[i] = (uint8_t)i;
	for (size_t i = 0; i < sizeof hashed_vk; i++) hashed_vk[i] = (uint8_t)(i * 3);
	for (size_t i = 0; i < sizeof msg; i++) msg[i] = (uint8_t)(i + 0x40);

	/* Warmup */
	for (int i = 0; i < 10; i++) {
		hash_to_point(logn, nonce, hashed_vk,
			NULL, 0, FNDSA_HASH_ID_RAW, msg, sizeof msg, c);
	}

	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (int i = 0; i < iters; i++) {
		hash_to_point(logn, nonce, hashed_vk,
			NULL, 0, FNDSA_HASH_ID_RAW, msg, sizeof msg, c);
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);

	double total_s = elapsed_seconds(t0, t1);
	*out_ns_per_call = total_s * 1e9 / (double)iters;
	free(c);
}

/* G recompute pipeline: derive G from f, F, h via NTT.
 * Mirrors the work that sign_step1 currently does to produce G. */
static void
bench_g_recompute(unsigned logn, int iters, double *out_ns_per_call,
	const int8_t *f_in, const int8_t *g_in, const int8_t *F_in)
{
	size_t n = (size_t)1 << logn;
	uint16_t *t0 = aligned_alloc(8, ((n * 2 + 7) & ~(size_t)7));
	uint16_t *t1 = aligned_alloc(8, ((n * 2 + 7) & ~(size_t)7));
	int8_t *G_out = malloc(n);

	/* Warmup */
	for (int i = 0; i < 10; i++) {
		mqpoly_small_to_int(logn, g_in, t0);
		mqpoly_small_to_int(logn, f_in, t1);
		mqpoly_int_to_ntt(logn, t0);
		mqpoly_int_to_ntt(logn, t1);
		(void)mqpoly_div_ntt(logn, t0, t1);
		mqpoly_small_to_int(logn, F_in, t1);
		mqpoly_int_to_ntt(logn, t1);
		mqpoly_mul_ntt(logn, t1, t0);
		mqpoly_ntt_to_int(logn, t1);
		(void)mqpoly_int_to_small(logn, t1, G_out);
	}

	struct timespec t0_clk, t1_clk;
	clock_gettime(CLOCK_MONOTONIC, &t0_clk);
	for (int i = 0; i < iters; i++) {
		mqpoly_small_to_int(logn, g_in, t0);
		mqpoly_small_to_int(logn, f_in, t1);
		mqpoly_int_to_ntt(logn, t0);
		mqpoly_int_to_ntt(logn, t1);
		(void)mqpoly_div_ntt(logn, t0, t1);
		mqpoly_small_to_int(logn, F_in, t1);
		mqpoly_int_to_ntt(logn, t1);
		mqpoly_mul_ntt(logn, t1, t0);
		mqpoly_ntt_to_int(logn, t1);
		(void)mqpoly_int_to_small(logn, t1, G_out);
	}
	clock_gettime(CLOCK_MONOTONIC, &t1_clk);

	double total_s = elapsed_seconds(t0_clk, t1_clk);
	*out_ns_per_call = total_s * 1e9 / (double)iters;
	free(t0); free(t1); free(G_out);
}

static void
bench_full_sign(unsigned logn, int iters, double *out_ns_per_call)
{
	size_t n = (size_t)1 << logn;
	(void)n;
	size_t sk_len = FNDSA_SIGN_KEY_SIZE(logn);
	size_t vk_len = FNDSA_VRFY_KEY_SIZE(logn);
	size_t basis_len = FNDSA_BASIS_SIZE(logn);
	size_t tree_len = FNDSA_LDL_TREE_SIZE(logn);
	size_t tmp_len = (((size_t)37 << logn) + 31);
	size_t sig_len = FNDSA_SIGNATURE_SIZE(logn);

	uint8_t *sk = malloc(sk_len);
	uint8_t *vk = malloc(vk_len);
	uint8_t *basis = aligned_alloc(8, (basis_len + 7) & ~(size_t)7);
	uint8_t *tree = aligned_alloc(8, (tree_len + 7) & ~(size_t)7);
	uint8_t *tmp = aligned_alloc(8, (tmp_len + 7) & ~(size_t)7);
	uint8_t *sig = malloc(sig_len);

	uint8_t kseed[32];
	for (size_t i = 0; i < sizeof kseed; i++) kseed[i] = (uint8_t)i;
	fndsa_keygen_seeded(logn, kseed, sizeof kseed, sk, vk);
	(void)fndsa_compute_basis(sk, sk_len, basis, basis_len);
	size_t tree_tmp_len = ((size_t)4 << logn) * sizeof(fpr) + 31;
	uint8_t *tree_tmp = aligned_alloc(8,
		(tree_tmp_len + 7) & ~(size_t)7);
	(void)fndsa_compute_ldl_tree(logn, basis, basis_len,
		tree, tree_len, tree_tmp, tree_tmp_len);
	free(tree_tmp);

	const char *msg = "the quick brown fox jumps over the lazy dog";
	uint8_t sigseed[56];
	for (size_t i = 0; i < sizeof sigseed; i++) sigseed[i] = 0xAA;

	/* Warmup */
	for (int i = 0; i < 10; i++) {
		(void)fndsa_sign_seeded_with_basis_and_tree_temp(
			sk, sk_len, basis, tree,
			NULL, 0, FNDSA_HASH_ID_RAW, msg, strlen(msg),
			sigseed, sizeof sigseed,
			sig, sig_len, tmp, tmp_len);
	}

	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (int i = 0; i < iters; i++) {
		(void)fndsa_sign_seeded_with_basis_and_tree_temp(
			sk, sk_len, basis, tree,
			NULL, 0, FNDSA_HASH_ID_RAW, msg, strlen(msg),
			sigseed, sizeof sigseed,
			sig, sig_len, tmp, tmp_len);
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);

	double total_s = elapsed_seconds(t0, t1);
	*out_ns_per_call = total_s * 1e9 / (double)iters;

	free(sk); free(vk); free(basis); free(tree); free(tmp); free(sig);
}

/* Decode an encoded sign_key to recover f, g, F for the G-recompute test. */
static void
decode_sk(const uint8_t *sk, size_t sk_len, unsigned logn,
	int8_t *f, int8_t *g, int8_t *F)
{
	(void)sk_len;
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

static int
run_perf_test(unsigned logn)
{
	size_t n = (size_t)1 << logn;
	double ns_h2p, ns_g_recomp, ns_sign;

	bench_hash_to_point(logn, ITERS_BENCH, &ns_h2p);

	/* Need a valid sign_key for G-recompute test. */
	size_t sk_len = FNDSA_SIGN_KEY_SIZE(logn);
	size_t vk_len = FNDSA_VRFY_KEY_SIZE(logn);
	uint8_t *sk = malloc(sk_len);
	uint8_t *vk = malloc(vk_len);
	uint8_t kseed[32];
	for (size_t i = 0; i < sizeof kseed; i++) kseed[i] = (uint8_t)i;
	fndsa_keygen_seeded(logn, kseed, sizeof kseed, sk, vk);

	int8_t *f = malloc(n);
	int8_t *g = malloc(n);
	int8_t *F = malloc(n);
	decode_sk(sk, sk_len, logn, f, g, F);

	bench_g_recompute(logn, ITERS_BENCH, &ns_g_recomp, f, g, F);
	bench_full_sign(logn, ITERS_QUICK, &ns_sign);

	free(sk); free(vk); free(f); free(g); free(F);

	printf("\n[logn=%u, n=%zu]\n", logn, n);
	printf("  full sign (with_basis_and_tree): %10.0f ns/call\n", ns_sign);
	printf("  hash_to_point isolated:           %10.0f ns/call (%.2f%% of sign)\n",
		ns_h2p, 100.0 * ns_h2p / ns_sign);
	printf("  G recompute pipeline:             %10.0f ns/call (%.2f%% of sign)\n",
		ns_g_recomp, 100.0 * ns_g_recomp / ns_sign);

	double total_overhead = ns_h2p + ns_g_recomp;
	printf("  hm-recompute alone overhead:      %10.0f ns (%.2f%%)\n",
		ns_h2p, 100.0 * ns_h2p / ns_sign);
	printf("  G-recompute alone overhead:       %10.0f ns (%.2f%%)\n",
		ns_g_recomp, 100.0 * ns_g_recomp / ns_sign);
	printf("  hm + G recompute combined:        %10.0f ns (%.2f%%)\n",
		total_overhead, 100.0 * total_overhead / ns_sign);

	return 0;
}

int
main(void)
{
	if (run_perf_test(9) != 0) return 1;
	if (run_perf_test(10) != 0) return 1;
	printf("\nNote: timings on host (x86_64). Cortex-M3 will be slower\n"
		"in absolute terms but ratios should be roughly similar.\n");
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
