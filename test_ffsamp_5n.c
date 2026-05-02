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
 * the bytes [37n, 51n+31) — which Path A's 4n FLR peak claim says it does not
 * need — with a sentinel pattern, then run a full signing round via the public
 * fndsa_sign_seeded_with_basis_temp API, passing 37n+31 as tmp_len. After the
 * call, verify the painted region is byte-identical to the sentinel.
 *
 * Path A's outer-level body uses fpoly_mac_fft (per-coefficient complex
 * multiply-accumulate) in step 2 to compute c1 += t1·l10 in place at qc(0..3),
 * eliminating the qc(16..19) t1*l10 product slot that the un-fused chain
 * needed. This is what brings the function-internal peak from 5n FLR to 4n
 * FLR. The 37n+31 boundary = 4n FLR ffsamp + 2n bytes FP-stays-extra +
 * 2n bytes hm + n bytes G + 31. */

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
	   pass 37n+31 as the API tmp_len. The bytes [37n, 51n+31) should
	   be untouched by the new outer-level body — that's the load-bearing
	   correctness claim for Path A's 4n FLR peak. */
	size_t large_tmp_len = ((size_t)51 << logn) + 31;
	size_t reduced_tmp_len = ((size_t)37 << logn) + 31;
	size_t paint_offset = (size_t)37 << logn;
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
					"logn=%u: violation at byte %zu (offset 37n+%zu): "
					"got 0x%02x\n",
					logn, paint_offset + i, i,
					tmp[paint_offset + i]);
			}
		}
	}

	free(tmp); free(basis); free(sig); free(vk); free(sk);

	if (violations > 0) {
		fprintf(stderr,
			"logn=%u: FAIL — %d/%zu bytes in [37n, 51n+31) modified\n",
			logn, violations, paint_size);
		return 1;
	}

	double saved_kib = (double)((((size_t)51 << logn) + 31)
	                          - (((size_t)43 << logn) + 31)) / 1024.0;
	printf("PASS logn=%u: bytes [37n, 51n+31) untouched "
		"(%zu bytes verified, %.1f KiB)\n",
		logn, paint_size, saved_kib);
	return 0;
}

int main(void)
{
	printf("=== FNDSA_FFSAMP_5N_REDUCED paint-and-check (Day 3+4) ===\n");
	printf("Verifies the new outer-level Path A body at the tightened\n");
	printf("37n+31 byte tmp_len boundary. Path A uses fpoly_mac_fft (Day 4)\n");
	printf("for in-place c1 = t0 + t1·l10, eliminating qc(16..19) scratch and\n");
	printf("achieving 4n FLR ffsamp peak.\n\n");

	int failures = 0;
	for (unsigned logn = 9; logn <= 10; logn++) {
		if (test_at_logn(logn) != 0) failures++;
	}

	printf("\n");
	if (failures == 0) {
		printf("ALL TESTS PASSED — Path A respects 37n+31 boundary; new outer body\n");
		printf("produces verifying signatures at logn 9, 10\n");
		return 0;
	}
	fprintf(stderr, "FAILURES: %d test case(s)\n", failures);
	return 1;
}
