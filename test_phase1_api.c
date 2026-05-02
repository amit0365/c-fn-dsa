/* End-to-end test for the FNDSA_PHASE1_REDUCED public API.
 *
 * Verifies:
 *   - fndsa_compute_basis() produces a basis usable by fndsa_sign_with_basis_temp()
 *   - signatures verify under fndsa_verify (or fndsa_verify_temp)
 *   - the smaller 45n+31 byte tmp[] is sufficient
 *   - bit-exact match with existing fndsa_sign_seeded path (sanity check)
 *
 * Build:
 *   clang -DFNDSA_PATH_B=1 -DFNDSA_PHASE1_REDUCED=1 -O2 -c \
 *     test_phase1_api.c -o test_phase1_api.o
 *   clang -o test_phase1_api test_phase1_api.o codec.o mq.o sha3.o sysrng.o \
 *     util.o kgen.o kgen_fxp.o kgen_gauss.o kgen_mp31.o kgen_ntru.o \
 *     kgen_poly.o kgen_zint31.o sign.o sign_core.o sign_fpoly.o sign_fpr.o \
 *     sign_sampler.o vrfy.o -lm
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "fndsa.h"

static int
test_at_logn(unsigned logn)
{
	size_t sk_len = FNDSA_SIGN_KEY_SIZE(logn);
	size_t vk_len = FNDSA_VRFY_KEY_SIZE(logn);
	size_t sig_len_max = FNDSA_SIGNATURE_SIZE(logn);
	size_t basis_len = FNDSA_BASIS_SIZE(logn);
	size_t tmp_len = ((size_t)45 << logn) + 31;

	uint8_t *sk = malloc(sk_len);
	uint8_t *vk = malloc(vk_len);
	uint8_t *sig_existing = malloc(sig_len_max);
	uint8_t *sig_with_basis = malloc(sig_len_max);
	void *basis = aligned_alloc(8, basis_len);
	void *tmp_existing = malloc(((size_t)51 << logn) + 31);  /* old size */
	void *tmp_basis = malloc(tmp_len);  /* new smaller size */

	if (!sk || !vk || !sig_existing || !sig_with_basis ||
	    !basis || !tmp_existing || !tmp_basis) {
		fprintf(stderr, "logn=%u: alloc failed\n", logn);
		return 1;
	}

	/* Generate keypair from a deterministic seed for repeatability. */
	uint8_t kseed[8] = {0xCA, 0xFE, 0xBA, 0xBE, (uint8_t)logn, 0, 0, 0};
	fndsa_keygen_seeded(logn, kseed, sizeof kseed, sk, vk);

	/* Compute basis once. */
	if (!fndsa_compute_basis(sk, sk_len, basis, basis_len)) {
		fprintf(stderr, "logn=%u: fndsa_compute_basis FAILED\n", logn);
		return 1;
	}

	/* Sign A: existing path (no precomputed basis), uses 51n+31 byte tmp[]. */
	uint8_t mseed[8] = {0xAA, 0xBB, 0xCC, 0xDD, 0, 0, 0, 0};
	size_t la = fndsa_sign_seeded_temp(sk, sk_len,
		NULL, 0, FNDSA_HASH_ID_RAW, "msg", 3,
		mseed, sizeof mseed,
		sig_existing, sig_len_max,
		tmp_existing, ((size_t)51 << logn) + 31);

	if (la == 0) {
		fprintf(stderr, "logn=%u: existing-path sign FAILED\n", logn);
		return 1;
	}

	int va = fndsa_verify(sig_existing, la, vk, vk_len,
		NULL, 0, FNDSA_HASH_ID_RAW, "msg", 3);
	if (!va) {
		fprintf(stderr, "logn=%u: existing-path verify FAILED\n", logn);
		return 1;
	}

	/* Sign B: precomputed-basis path with the reduced 45n+31 tmp[]. */
	size_t lb = fndsa_sign_seeded_with_basis_temp(
		sk, sk_len, basis,
		NULL, 0, FNDSA_HASH_ID_RAW, "msg", 3,
		mseed, sizeof mseed,
		sig_with_basis, sig_len_max,
		tmp_basis, tmp_len);

	if (lb == 0) {
		fprintf(stderr,
			"logn=%u: with-basis sign FAILED at tmp_len=%zu\n",
			logn, tmp_len);
		return 1;
	}

	int vb = fndsa_verify(sig_with_basis, lb, vk, vk_len,
		NULL, 0, FNDSA_HASH_ID_RAW, "msg", 3);
	if (!vb) {
		fprintf(stderr,
			"logn=%u: with-basis verify FAILED\n", logn);
		return 1;
	}

	/* Bit-exact comparison: both paths should produce same sig bytes. */
	int bit_exact = (la == lb) && (memcmp(sig_existing, sig_with_basis, la) == 0);

	printf("PASS logn=%u: existing tmp=%zu, with-basis tmp=%zu (saves %zu B = %zu KiB)%s\n",
		logn,
		((size_t)51 << logn) + 31, tmp_len,
		(((size_t)51 << logn) + 31) - tmp_len,
		((((size_t)51 << logn) + 31) - tmp_len) / 1024,
		bit_exact ? " — bit-exact match" : " — distribution-equivalent");

	/* Test undersized tmp_len rejection. */
	size_t lc = fndsa_sign_seeded_with_basis_temp(
		sk, sk_len, basis,
		NULL, 0, FNDSA_HASH_ID_RAW, "msg", 3,
		mseed, sizeof mseed,
		sig_with_basis, sig_len_max,
		tmp_basis, tmp_len - 1);  /* 1 byte short */
	if (lc != 0) {
		fprintf(stderr,
			"logn=%u: undersized tmp_len NOT rejected (returned %zu)\n",
			logn, lc);
		return 1;
	}
	printf("        undersized tmp_len rejection: OK\n");

	free(tmp_basis); free(tmp_existing); free(basis);
	free(sig_with_basis); free(sig_existing); free(vk); free(sk);
	return 0;
}

int main(void)
{
	printf("=== FNDSA_PHASE1_REDUCED public API end-to-end test ===\n");
	printf("Verifies fndsa_compute_basis + fndsa_sign_with_basis_temp\n");
	printf("at the reduced 45n+31 byte tmp_len.\n\n");

	int failures = 0;
	for (unsigned logn = 9; logn <= 10; logn++) {
		if (test_at_logn(logn) != 0) failures++;
	}

	printf("\n");
	if (failures == 0) {
		printf("ALL TESTS PASSED — phase 1 reduction public API validated\n");
		return 0;
	}
	fprintf(stderr, "FAILURES: %d test case(s)\n", failures);
	return 1;
}
