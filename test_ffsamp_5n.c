/* Path A (FNDSA_FFSAMP_5N_REDUCED) paint-and-check test.
 *
 * Build:
 *   clang -DFNDSA_PATH_B=1 -DFNDSA_PHASE1_REDUCED=1 -DFNDSA_FFSAMP_5N_REDUCED=1 \
 *     -O2 -c test_ffsamp_5n.c -o test_ffsamp_5n.o
 *   clang -o test_ffsamp_5n test_ffsamp_5n.o codec.o mq.o sha3.o sysrng.o util.o \
 *     kgen.o kgen_fxp.o kgen_gauss.o kgen_mp31.o kgen_ntru.o kgen_poly.o \
 *     kgen_zint31.o sign.o sign_core.o sign_fpoly.o sign_fpr.o sign_sampler.o \
 *     vrfy.o -lm
 *
 * Strategy: allocate tmp[] at the LARGER PATH_B-only size (51n+31 bytes), paint
 * the bytes [43n, 51n+31) — which Path A's API claims it does not need — with
 * a sentinel pattern, then run a full signing round via the public
 * fndsa_sign_seeded_with_basis_temp API, passing 43n+31 as tmp_len. After the
 * call, verify the painted region is byte-identical to the sentinel.
 *
 * NOTE: Path A's CURRENT implementation (Day 1+2) uses qc(16..19) as t1*l10
 * scratch in step 2 of the outer body, keeping the function-internal peak at
 * 5n FLR (same as PATH_B+PHASE1_REDUCED). Achieving the documented 4n peak
 * and the corresponding 35n+31 byte tmp_len requires a new fpoly_mac_fft
 * fused primitive — see kill plan Day 4+. This test verifies the CURRENT
 * boundary (43n+31) is honored, not the future tighter one.
 *
 * Once the fpoly_mac_fft primitive lands, this test will be tightened to
 * paint at offset 35n (the actual 4n FLR boundary) instead of 43n. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "fndsa.h"

#define SENTINEL_BYTE  0xA5

static int
test_at_logn(unsigned logn)
{
	size_t sk_len = FNDSA_SIGN_KEY_SIZE(logn);
	size_t vk_len = FNDSA_VRFY_KEY_SIZE(logn);
	size_t sig_len_max = FNDSA_SIGNATURE_SIZE(logn);
	size_t basis_len = FNDSA_BASIS_SIZE(logn);

	/* Allocate at the PATH_B-only (no phase 1 reduction) size 51n+31,
	   pass 43n+31 as the API tmp_len. The bytes [43n, 51n+31) should
	   be untouched by the new outer-level body — that's the load-bearing
	   correctness claim. */
	size_t large_tmp_len = ((size_t)51 << logn) + 31;
	size_t reduced_tmp_len = ((size_t)43 << logn) + 31;
	size_t paint_offset = (size_t)43 << logn;
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

	memset(tmp, SENTINEL_BYTE, large_tmp_len);

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

	int v = fndsa_verify(sig, l, vk, vk_len,
		NULL, 0, FNDSA_HASH_ID_RAW, "msg", 3);
	if (!v) {
		fprintf(stderr, "logn=%u: verify FAILED\n", logn);
		return 1;
	}

	int violations = 0;
	for (size_t i = 0; i < paint_size; i++) {
		if (tmp[paint_offset + i] != SENTINEL_BYTE) {
			violations++;
			if (violations <= 3) {
				fprintf(stderr,
					"logn=%u: violation at byte %zu (offset 43n+%zu): "
					"got 0x%02x\n",
					logn, paint_offset + i, i,
					tmp[paint_offset + i]);
			}
		}
	}

	free(tmp); free(basis); free(sig); free(vk); free(sk);

	if (violations > 0) {
		fprintf(stderr,
			"logn=%u: FAIL — %d/%zu bytes in [43n, 51n+31) modified\n",
			logn, violations, paint_size);
		return 1;
	}

	double saved_kib = (double)((((size_t)51 << logn) + 31)
	                          - (((size_t)43 << logn) + 31)) / 1024.0;
	printf("PASS logn=%u: bytes [43n, 51n+31) untouched "
		"(%zu bytes verified, %.1f KiB)\n",
		logn, paint_size, saved_kib);
	return 0;
}

int main(void)
{
	printf("=== FNDSA_FFSAMP_5N_REDUCED paint-and-check (Day 3) ===\n");
	printf("Verifies the new outer-level Path A body at FNDSA_PHASE1_REDUCED's\n");
	printf("43n+31 byte tmp_len boundary. Path A's currently-shipped peak is\n");
	printf("5n FLR (same as PATH_B+PHASE1) because step 2 uses qc(16..19) as\n");
	printf("t1*l10 scratch. A tighter 35n+31 boundary requires a new fused\n");
	printf("fpoly_mac_fft primitive — Day 4+ work.\n\n");

	int failures = 0;
	for (unsigned logn = 9; logn <= 10; logn++) {
		if (test_at_logn(logn) != 0) failures++;
	}

	printf("\n");
	if (failures == 0) {
		printf("ALL TESTS PASSED — Path A respects 43n+31 boundary; new outer body\n");
		printf("produces verifying signatures at logn 9, 10\n");
		return 0;
	}
	fprintf(stderr, "FAILURES: %d test case(s)\n", failures);
	return 1;
}
