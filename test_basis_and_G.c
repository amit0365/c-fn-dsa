/*
 * Phase 0 verification for B3:
 * - Generate key, call fndsa_compute_basis_and_G to get basis + G_externally
 * - Independently call sign_step1's G derivation to get G_internal
 * - Bit-compare G_externally with G_internal
 *
 * If they match, B3 is mathematically sound: the G stored at provision
 * time will be identical to the G that sign_step1 would derive each
 * sign, and we can skip the derivation safely.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "fndsa.h"
#include "inner.h"
#include "sign_inner.h"

#if FNDSA_LOW_RAM

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

/* Compute G the way sign_step1 does (independently of fndsa_compute_basis). */
static int
compute_G_reference(unsigned logn,
	const int8_t *f, const int8_t *g, const int8_t *F,
	int8_t *G_out)
{
	size_t n = (size_t)1 << logn;
	uint16_t *t0 = aligned_alloc(8, ((n * 2 + 7) & ~(size_t)7));
	uint16_t *t1 = aligned_alloc(8, ((n * 2 + 7) & ~(size_t)7));

	mqpoly_small_to_int(logn, g, t0);
	mqpoly_small_to_int(logn, f, t1);
	mqpoly_int_to_ntt(logn, t0);
	mqpoly_int_to_ntt(logn, t1);
	if (!mqpoly_div_ntt(logn, t0, t1)) {
		free(t0); free(t1);
		return 0;
	}
	mqpoly_small_to_int(logn, F, t1);
	mqpoly_int_to_ntt(logn, t1);
	mqpoly_mul_ntt(logn, t1, t0);
	mqpoly_ntt_to_int(logn, t1);
	int ok = mqpoly_int_to_small(logn, t1, G_out);
	free(t0); free(t1);
	return ok;
}

static int
run_test(unsigned logn)
{
	size_t n = (size_t)1 << logn;
	size_t sk_len = FNDSA_SIGN_KEY_SIZE(logn);
	size_t vk_len = FNDSA_VRFY_KEY_SIZE(logn);
	size_t basis_len = FNDSA_BASIS_SIZE(logn);
	size_t g_len = FNDSA_G_SIZE(logn);

	uint8_t *sk = malloc(sk_len);
	uint8_t *vk = malloc(vk_len);
	uint8_t *basis = aligned_alloc(8, (basis_len + 7) & ~(size_t)7);
	uint8_t *G_external = malloc(g_len);
	int8_t *f = malloc(n);
	int8_t *g = malloc(n);
	int8_t *F = malloc(n);
	int8_t *G_reference = malloc(n);

	uint8_t kseed[32];
	for (size_t i = 0; i < sizeof kseed; i++) {
		kseed[i] = (uint8_t)(i + logn * 11);
	}
	fndsa_keygen_seeded(logn, kseed, sizeof kseed, sk, vk);

	if (!fndsa_compute_basis_and_G(sk, sk_len,
		basis, basis_len, G_external, g_len))
	{
		fprintf(stderr, "[logn=%u] compute_basis_and_G failed\n", logn);
		return 1;
	}

	decode_sk(sk, logn, f, g, F);
	if (!compute_G_reference(logn, f, g, F, G_reference)) {
		fprintf(stderr, "[logn=%u] compute_G_reference failed\n", logn);
		return 1;
	}

	int match = (memcmp(G_external, G_reference, n) == 0);
	printf("[logn=%u] G match: %s\n", logn, match ? "YES" : "NO");
	if (!match) {
		size_t mismatches = 0;
		for (size_t i = 0; i < n; i++) {
			if (((int8_t *)G_external)[i] != G_reference[i]) {
				mismatches++;
			}
		}
		fprintf(stderr, "  %zu/%zu coefficients differ\n",
			mismatches, n);
	}

	free(sk); free(vk); free(basis); free(G_external);
	free(f); free(g); free(F); free(G_reference);
	return match ? 0 : 1;
}

/* Sign-compare: with basis-only API vs basis-and-G API. Both should
 * produce identical signatures. */
static int
run_sign_compare(unsigned logn)
{
	size_t sk_len = FNDSA_SIGN_KEY_SIZE(logn);
	size_t vk_len = FNDSA_VRFY_KEY_SIZE(logn);
	size_t basis_len = FNDSA_BASIS_SIZE(logn);
	size_t g_len = FNDSA_G_SIZE(logn);
	/* B3 reduces scalar tmp_len from 37n+31 to 36n+31. */
#if FNDSA_SSE2 || FNDSA_NEON || FNDSA_RV64D
	size_t tmp_len_basis_only = (((size_t)34 << logn) + 31);
#else
	size_t tmp_len_basis_only = (((size_t)37 << logn) + 31);
#endif
	/* B3+Phase5 unified: 34n+31 on both SIMD and scalar. */
	size_t tmp_len_b3 = (((size_t)34 << logn) + 31);
	(void)tmp_len_b3;
	size_t sig_len = FNDSA_SIGNATURE_SIZE(logn);

	uint8_t *sk = malloc(sk_len);
	uint8_t *vk = malloc(vk_len);
	uint8_t *basis = aligned_alloc(8, (basis_len + 7) & ~(size_t)7);
	uint8_t *G = malloc(g_len);
	uint8_t *tmp1 = aligned_alloc(8,
		(tmp_len_basis_only + 7) & ~(size_t)7);
	/* B3 path uses smaller buffer to verify the savings actually work. */
	uint8_t *tmp2 = aligned_alloc(8,
		(tmp_len_b3 + 7) & ~(size_t)7);
	uint8_t *sig1 = malloc(sig_len);
	uint8_t *sig2 = malloc(sig_len);

	uint8_t kseed[32];
	for (size_t i = 0; i < sizeof kseed; i++) {
		kseed[i] = (uint8_t)(i + logn * 11);
	}
	fndsa_keygen_seeded(logn, kseed, sizeof kseed, sk, vk);
	if (!fndsa_compute_basis_and_G(sk, sk_len,
		basis, basis_len, G, g_len)) return 1;

	const char *msg = "the quick brown fox jumps over the lazy dog";
	uint8_t sigseed[56];
	for (size_t i = 0; i < sizeof sigseed; i++) {
		sigseed[i] = (uint8_t)(0xAA + i);
	}

	/* Method A: basis-only */
	size_t s1 = fndsa_sign_seeded_with_basis_temp(
		sk, sk_len, basis,
		NULL, 0, FNDSA_HASH_ID_RAW, msg, strlen(msg),
		sigseed, sizeof sigseed,
		sig1, sig_len, tmp1, tmp_len_basis_only);

	/* Method B: basis + G (B3) — uses smaller tmp_len */
	size_t s2 = fndsa_sign_seeded_with_basis_and_G_temp(
		sk, sk_len, basis, G,
		NULL, 0, FNDSA_HASH_ID_RAW, msg, strlen(msg),
		sigseed, sizeof sigseed,
		sig2, sig_len, tmp2, tmp_len_b3);

	if (s1 == 0 || s2 == 0) {
		fprintf(stderr, "[logn=%u] sign failed (s1=%zu s2=%zu)\n",
			logn, s1, s2);
		return 1;
	}
	int match = (s1 == s2 && memcmp(sig1, sig2, s1) == 0);
	printf("[logn=%u] basis-only vs basis-and-G sig match: %s "
		"(sig=%zu B)\n",
		logn, match ? "YES" : "NO", s1);

	free(sk); free(vk); free(basis); free(G);
	free(tmp1); free(tmp2); free(sig1); free(sig2);
	return match ? 0 : 1;
}

int
main(void)
{
	if (run_test(9) != 0) return 1;
	if (run_test(10) != 0) return 1;
	printf("Phase 0 PASSED — G output matches reference.\n");
	if (run_sign_compare(9) != 0) return 1;
	if (run_sign_compare(10) != 0) return 1;
	printf("Phase 2/3 PASSED — sign with B3 API bit-matches sign without.\n");
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
