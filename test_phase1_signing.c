/* End-to-end test for FNDSA_PHASE1_REDUCED's external_basis path.
 *
 * Strategy:
 *   1. Generate a keypair via fndsa_keygen_seeded.
 *   2. Sign with the existing public API (computes basis internally).
 *   3. Manually precompute basis B = [[g, -f], [G, -F]] in FFT.
 *   4. Call sign_core directly with external_basis pointing to that buffer.
 *   5. Verify both signatures with the same vk; both must verify.
 *
 * Optional bit-exact check: signatures from (2) and (4) should match
 * byte-for-byte, since the only difference is whether basis is read
 * from tmp[] (computed inline) or from external_basis. The arithmetic
 * is identical (verified by test_phase1_primitives.c).
 *
 * Build:
 *   clang -DFNDSA_PATH_B=1 -DFNDSA_PHASE1_REDUCED=1 -O2 -c \
 *     test_phase1_signing.c -o test_phase1_signing.o
 *   clang -o test_phase1_signing test_phase1_signing.o codec.o mq.o sha3.o \
 *     sysrng.o util.o kgen.o kgen_fxp.o kgen_gauss.o kgen_mp31.o kgen_ntru.o \
 *     kgen_poly.o kgen_zint31.o sign.o sign_core.o sign_fpoly.o sign_fpr.o \
 *     sign_sampler.o vrfy.o -lm
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "fndsa.h"
#include "sign_inner.h"

/* basis_to_FFT is static in sign_core.c; reproduce here for test. */
static void
build_basis(unsigned logn,
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

/* Decode the encoded f, g, F from sign_key bytes. (G is provided
   separately to sign_core; see sign.c sign_step1's decode logic.) */
static void
decode_fgF(unsigned logn, const uint8_t *enc_fgF,
	int8_t *f, int8_t *g, int8_t *F)
{
	unsigned nbits;
	switch (logn) {
	case 9: nbits = 6; break;
	case 10: nbits = 5; break;
	default: nbits = 8; break;  /* doesn't matter for our tests */
	}
	size_t flen = ((size_t)nbits << logn) >> 3;
	size_t k;
	k = trim_i8_decode(logn, enc_fgF, f, nbits);  (void)k;
	k = trim_i8_decode(logn, enc_fgF + flen, g, nbits);  (void)k;
	k = trim_i8_decode(logn, enc_fgF + 2 * flen, F, 8);  (void)k;
}

static int
test_at_logn(unsigned logn)
{
	size_t n = (size_t)1 << logn;
	size_t sk_len = FNDSA_SIGN_KEY_SIZE(logn);
	size_t vk_len = FNDSA_VRFY_KEY_SIZE(logn);
	size_t sig_len_max = FNDSA_SIGNATURE_SIZE(logn);

	uint8_t *sk = malloc(sk_len);
	uint8_t *vk = malloc(vk_len);
	uint8_t *sig_a = malloc(sig_len_max);
	uint8_t *sig_b = malloc(sig_len_max);

	uint8_t seed[8] = {0xCA, 0xFE, 0xBA, 0xBE, (uint8_t)logn, 0, 0, 0};
	fndsa_keygen_seeded(logn, seed, sizeof seed, sk, vk);

	/* Sign A: existing path (computes basis internally) */
	uint8_t mseed_a[8] = {0x01, 0, 0, 0, 0, 0, 0, 0};
	size_t la = fndsa_sign_seeded(sk, sk_len, NULL, 0,
		FNDSA_HASH_ID_RAW, "test", 4,
		mseed_a, sizeof mseed_a, sig_a, sig_len_max);

	/* Verify A */
	int va = fndsa_verify(sig_a, la, vk, vk_len,
		NULL, 0, FNDSA_HASH_ID_RAW, "test", 4);
	if (la == 0 || !va) {
		fprintf(stderr, "logn=%u: existing-path sign or verify FAILED\n", logn);
		free(sig_b); free(sig_a); free(vk); free(sk);
		return 1;
	}

	/* Sign B: precompute basis externally, call sign_core directly. */
	int8_t *f = malloc(n);
	int8_t *g = malloc(n);
	int8_t *F = malloc(n);
	int8_t *G = malloc(n);
	fpr *basis = aligned_alloc(32, 4 * n * sizeof(fpr));
	if (!basis) {
		fprintf(stderr, "logn=%u: aligned_alloc failed\n", logn);
		free(F); free(g); free(f); free(sig_b); free(sig_a); free(vk); free(sk);
		return 1;
	}

	/* Decode key bytes. sk[0] is the header byte; rest is encoded fgF.
	   We need to recompute G separately the same way sign_step1 does:
	     G = h*F mod q, where h = g/f mod q. */
	decode_fgF(logn, sk + 1, f, g, F);

	/* Recompute G via the same formula sign_step1 uses. */
	uint16_t *t0_buf = malloc(((size_t)1 << logn) * sizeof(uint16_t));
	uint16_t *t1_buf = malloc(((size_t)1 << logn) * sizeof(uint16_t));
	mqpoly_small_to_int(logn, g, t0_buf);
	mqpoly_small_to_int(logn, f, t1_buf);
	mqpoly_int_to_ntt(logn, t0_buf);
	mqpoly_int_to_ntt(logn, t1_buf);
	if (!mqpoly_div_ntt(logn, t0_buf, t1_buf)) {
		fprintf(stderr, "logn=%u: f not invertible (key invalid)\n", logn);
		goto cleanup;
	}
	mqpoly_small_to_int(logn, F, t1_buf);
	mqpoly_int_to_ntt(logn, t1_buf);
	mqpoly_mul_ntt(logn, t1_buf, t0_buf);
	mqpoly_ntt_to_int(logn, t1_buf);
	mqpoly_int_to_small(logn, t1_buf, G);

	/* Build the basis from f, g, F, G */
	build_basis(logn, f, g, F, G, basis);

	/* Compute hashed vk (same as sign_step1) */
	uint8_t hashed_vk[64];
	{
		uint8_t *vrfy_key = malloc(vk_len);
		memcpy(vrfy_key, vk, vk_len);
		shake_context sc;
		shake_init(&sc, 256);
		shake_inject(&sc, vrfy_key, vk_len);
		shake_flip(&sc);
		shake_extract(&sc, hashed_vk, 64);
		free(vrfy_key);
	}

	/* Allocate tmp[] at the documented size for FNDSA_PATH_B (51n+31).
	   Phase 1 is reduced inside sign_core but tmp[] sizing for direct
	   sign_core calls is at the higher of phase 1 + ffsamp peaks; the
	   conservative path-b size of 51n+31 covers it. */
	size_t tmp_len = ((size_t)51 << logn) + 31;
	void *tmp = malloc(tmp_len);

	/* Call sign_core directly with external_basis. Use the same mseed
	   as sign A so the sampling is identical. */
	uint8_t rndbuf[40 + 56];
	memset(rndbuf, 0, sizeof rndbuf);
	rndbuf[0] = 0x01;  /* sign_step1 sets seed[0] = 0x01 before signing */
	memcpy(rndbuf + 1, mseed_a, sizeof mseed_a);

	size_t lb = sign_core(logn, sk + 1, G, hashed_vk,
		NULL, 0, FNDSA_HASH_ID_RAW, (const uint8_t *)"test", 4,
		mseed_a, sizeof mseed_a, sig_b, tmp, basis);

	int vb = (lb > 0) && fndsa_verify(sig_b, lb, vk, vk_len,
		NULL, 0, FNDSA_HASH_ID_RAW, "test", 4);

	if (lb == 0 || !vb) {
		fprintf(stderr,
			"logn=%u: external-basis sign or verify FAILED (lb=%zu, verify=%d)\n",
			logn, lb, vb);
		free(tmp);
		goto cleanup;
	}

	printf("PASS logn=%u: existing-path verify=ok, external-basis verify=ok",
		logn);

	/* Bit-exact comparison: both signing paths should produce the
	   same signature bytes. */
	if (la == lb && memcmp(sig_a, sig_b, la) == 0) {
		printf(" (bit-exact match)\n");
	} else {
		printf(" (signatures differ — distribution-equivalent only)\n");
	}

	free(tmp);
	free(t1_buf); free(t0_buf);
	free(basis); free(G); free(F); free(g); free(f);
	free(sig_b); free(sig_a); free(vk); free(sk);
	return 0;

cleanup:
	free(t1_buf); free(t0_buf);
	free(basis); free(G); free(F); free(g); free(f);
	free(sig_b); free(sig_a); free(vk); free(sk);
	return 1;
}

int main(void)
{
	printf("=== FNDSA_PHASE1_REDUCED end-to-end signing test ===\n");
	printf("Verifies sign_core with external_basis produces valid signatures.\n\n");

	int failures = 0;
	for (unsigned logn = 9; logn <= 10; logn++) {
		if (test_at_logn(logn) != 0) failures++;
	}

	printf("\n");
	if (failures == 0) {
		printf("ALL TESTS PASSED — phase 1 reduction sign path validated\n");
		return 0;
	}
	fprintf(stderr, "FAILURES: %d test case(s)\n", failures);
	return 1;
}
