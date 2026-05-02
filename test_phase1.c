/* Day 6 paint-and-check test for FNDSA_PHASE1_REDUCED.
 *
 * Strategy: allocate tmp[] at the LARGER PATH_B size (51n+31 bytes), paint
 * the bytes [45n, 51n+31) — which phase-1-reduced signing claims it does
 * not need — with a sentinel pattern, then run a full signing round via
 * the public _with_basis API. After the call, verify the painted region
 * is byte-identical to the sentinel.
 *
 * Why this is meaningful beyond ASAN:
 *   ASAN catches writes BEYOND the allocated buffer (heap-buffer-overflow).
 *   This test catches writes WITHIN the allocated buffer that fall in the
 *   region the new layout promises to leave untouched. ASAN can't see them
 *   because they're "in bounds" of a larger allocation.
 *
 * Companion to test_path_b.c which validates the same property at the
 * ffsamp_fft_inner internal level. This test validates the property at
 * the public-API / sign_core level.
 *
 * Build:
 *   clang -DFNDSA_PATH_B=1 -DFNDSA_PHASE1_REDUCED=1 -O2 -c \
 *     test_phase1.c -o test_phase1.o
 *   clang -o test_phase1 test_phase1.o codec.o mq.o sha3.o sysrng.o util.o \
 *     kgen.o kgen_fxp.o kgen_gauss.o kgen_mp31.o kgen_ntru.o kgen_poly.o \
 *     kgen_zint31.o sign.o sign_core.o sign_fpoly.o sign_fpr.o \
 *     sign_sampler.o vrfy.o -lm
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "fndsa.h"

/* Sentinel byte. Any non-zero value works; choosing one that's
   unlikely to appear in legitimate FLR/integer data makes accidental
   hits less likely. */
#define SENTINEL_BYTE  0xA5

static int
test_at_logn(unsigned logn)
{
	size_t sk_len = FNDSA_SIGN_KEY_SIZE(logn);
	size_t vk_len = FNDSA_VRFY_KEY_SIZE(logn);
	size_t sig_len_max = FNDSA_SIGNATURE_SIZE(logn);
	size_t basis_len = FNDSA_BASIS_SIZE(logn);

	/* Allocate tmp[] at the PATH_B size (51n+31), but pass the smaller
	   45n+31 to the API. The API is only documented to need 45n+31, so
	   any writes beyond that are violations of the layout contract. */
	size_t large_tmp_len = ((size_t)51 << logn) + 31;
	size_t reduced_tmp_len = ((size_t)45 << logn) + 31;
	size_t paint_offset = (size_t)45 << logn;
	size_t paint_size = large_tmp_len - paint_offset;

	uint8_t *sk = malloc(sk_len);
	uint8_t *vk = malloc(vk_len);
	uint8_t *sig = malloc(sig_len_max);
	void *basis = aligned_alloc(8, basis_len);
	uint8_t *tmp = malloc(large_tmp_len);

	if (!sk || !vk || !sig || !basis || !tmp) {
		fprintf(stderr, "logn=%u: alloc failed\n", logn);
		return 1;
	}

	uint8_t kseed[8] = {0xCA, 0xFE, 0xBA, 0xBE, (uint8_t)logn, 0, 0, 0};
	fndsa_keygen_seeded(logn, kseed, sizeof kseed, sk, vk);

	if (!fndsa_compute_basis(sk, sk_len, basis, basis_len)) {
		fprintf(stderr, "logn=%u: fndsa_compute_basis FAILED\n", logn);
		return 1;
	}

	/* Paint the entire tmp[] with sentinel first, then let the signing
	   call overwrite the [0, 45n+31) region. The bytes at [45n, 51n+31)
	   should remain at SENTINEL_BYTE if phase 1 reduced signing respects
	   its 45n+31 layout claim. */
	memset(tmp, SENTINEL_BYTE, large_tmp_len);

	/* Save a snapshot of the painted region for post-sign comparison. */
	uint8_t *snapshot = malloc(paint_size);
	memcpy(snapshot, tmp + paint_offset, paint_size);

	/* Sign via the _with_basis API, telling it the buffer is only
	   45n+31 bytes (so it must not write beyond that). */
	uint8_t mseed[8] = {0xDE, 0xAD, 0xBE, 0xEF, 0, 0, 0, 0};
	size_t l = fndsa_sign_seeded_with_basis_temp(
		sk, sk_len, basis,
		NULL, 0, FNDSA_HASH_ID_RAW, "msg", 3,
		mseed, sizeof mseed,
		sig, sig_len_max,
		tmp, reduced_tmp_len);

	if (l == 0) {
		fprintf(stderr, "logn=%u: sign FAILED\n", logn);
		return 1;
	}

	/* Verify signature is correct. */
	int v = fndsa_verify(sig, l, vk, vk_len,
		NULL, 0, FNDSA_HASH_ID_RAW, "msg", 3);
	if (!v) {
		fprintf(stderr, "logn=%u: verify FAILED\n", logn);
		return 1;
	}

	/* Check the painted region is intact. */
	int violations = 0;
	for (size_t i = 0; i < paint_size; i++) {
		if (tmp[paint_offset + i] != SENTINEL_BYTE) {
			violations++;
			if (violations <= 3) {
				fprintf(stderr,
					"logn=%u: violation at byte %zu (offset 45n+%zu): "
					"got 0x%02x, expected 0x%02x\n",
					logn, paint_offset + i, i,
					tmp[paint_offset + i], SENTINEL_BYTE);
			}
		}
	}

	free(snapshot);
	free(tmp); free(basis); free(sig); free(vk); free(sk);

	if (violations > 0) {
		fprintf(stderr,
			"logn=%u: FAIL — %d/%zu bytes in [45n, 51n+31) modified\n",
			logn, violations, paint_size);
		return 1;
	}

	double saved_kib = (double)((((size_t)51 << logn) + 31)
	                          - (((size_t)45 << logn) + 31)) / 1024.0;
	printf("PASS logn=%u: bytes [45n, 51n+31) untouched (%zu bytes verified, %.1f KiB saved)\n",
		logn, paint_size, saved_kib);
	return 0;
}

int main(void)
{
	printf("=== FNDSA_PHASE1_REDUCED paint-and-check (Day 6) ===\n");
	printf("Allocates tmp[] at 51n+31 bytes (PATH_B size), passes 45n+31 to\n");
	printf("the with-basis API, paints bytes [45n, 51n+31) with sentinel,\n");
	printf("and verifies sentinel is intact post-sign. Catches in-buffer\n");
	printf("layout violations that ASAN can't see.\n\n");

	int failures = 0;
	for (unsigned logn = 9; logn <= 10; logn++) {
		if (test_at_logn(logn) != 0) failures++;
	}

	printf("\n");
	if (failures == 0) {
		printf("ALL TESTS PASSED — phase 1 reduction respects its 45n+31 layout\n");
		return 0;
	}
	fprintf(stderr, "FAILURES: %d test case(s)\n", failures);
	return 1;
}
