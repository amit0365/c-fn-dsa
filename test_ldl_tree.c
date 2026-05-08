/*
 * Smoke test for fndsa_compute_ldl_tree.
 *
 * Generates a key, computes its basis, then computes the LDL tree.
 * Verifies:
 *   1. fndsa_compute_ldl_tree returns success
 *   2. tree buffer is non-zero (something was written)
 *   3. l10 values at the root match what fpoly_LDL_fft would produce
 *      from the basis directly
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "fndsa.h"
#include "inner.h"
#include "sign_inner.h"

#define LOGN 9
#define N    (1u << LOGN)

#if FNDSA_LOW_RAM

static int
all_zero(const uint8_t *buf, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		if (buf[i] != 0) return 0;
	}
	return 1;
}

int
main(void)
{
	uint8_t sk[FNDSA_SIGN_KEY_SIZE(LOGN)];
	uint8_t vk[FNDSA_VRFY_KEY_SIZE(LOGN)];

	/* Generate a key. */
	uint8_t seed[32];
	for (size_t i = 0; i < sizeof seed; i++) seed[i] = (uint8_t)i;
	fndsa_keygen_seeded(LOGN, seed, sizeof seed, sk, vk);

	/* Compute basis. */
	__attribute__((aligned(8)))
	uint8_t basis[FNDSA_BASIS_SIZE(LOGN)];
	if (!fndsa_compute_basis(sk, sizeof sk, basis, sizeof basis)) {
		fprintf(stderr, "compute_basis failed\n");
		return 1;
	}

	/* Compute tree. */
	printf("FNDSA_LDL_TREE_SIZE(%d) = %zu bytes\n",
		LOGN, FNDSA_LDL_TREE_SIZE(LOGN));

	__attribute__((aligned(8)))
	uint8_t tree[FNDSA_LDL_TREE_SIZE(LOGN)];
	__attribute__((aligned(8)))
	uint8_t tmp[((size_t)4 << LOGN) * sizeof(fpr) + 31];

	memset(tree, 0, sizeof tree);
	int rc = fndsa_compute_ldl_tree(LOGN,
		basis, sizeof basis,
		tree, sizeof tree,
		tmp, sizeof tmp);
	if (rc != 1) {
		fprintf(stderr, "compute_ldl_tree failed (rc=%d)\n", rc);
		return 1;
	}

	if (all_zero(tree, sizeof tree)) {
		fprintf(stderr, "tree buffer is all zero — nothing was written\n");
		return 1;
	}

	printf("compute_ldl_tree succeeded, tree non-empty\n");

	/* Cross-check root node: independently compute LDL of outer Gram and
	   compare l10. */
	__attribute__((aligned(8)))
	fpr ref_g00[N / 2];
	__attribute__((aligned(8)))
	fpr ref_g01[N];
	__attribute__((aligned(8)))
	fpr ref_g11[N / 2];
	fpoly_gram_fft_dst(LOGN, ref_g00, ref_g01, ref_g11,
		(const fpr *)basis);
	fpoly_LDL_fft(LOGN, ref_g00, ref_g01, ref_g11);

	/* Root node lives at offset 0; its layout is l10 (n fpr) + d00 (n/2)
	   + d11 (n/2). */
	const fpr *root_l10 = (const fpr *)tree;
	if (memcmp(root_l10, ref_g01, N * sizeof(fpr)) != 0) {
		fprintf(stderr, "root l10 MISMATCH\n");
		return 1;
	}
	const fpr *root_d00 = root_l10 + N;
	if (memcmp(root_d00, ref_g00, (N / 2) * sizeof(fpr)) != 0) {
		fprintf(stderr, "root d00 MISMATCH\n");
		return 1;
	}
	const fpr *root_d11 = root_d00 + (N / 2);
	if (memcmp(root_d11, ref_g11, (N / 2) * sizeof(fpr)) != 0) {
		fprintf(stderr, "root d11 MISMATCH\n");
		return 1;
	}
	printf("root node (l10, d00, d11) bit-matches reference\n");

	printf("PASS\n");
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
