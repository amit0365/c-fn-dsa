/*
 * Validation test for fndsa_compute_ldl_tree.
 *
 * For both logn=9 and logn=10:
 *   1. Generate a key, compute basis.
 *   2. Compute LDL tree via fndsa_compute_ldl_tree().
 *   3. Walk the full tree with an independent DFS using fpoly_LDL_fft +
 *      fpoly_split_selfadj_fft directly, bit-comparing each level/index
 *      slot against the tree contents.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "fndsa.h"
#include "inner.h"
#include "sign_inner.h"

#if FNDSA_LOW_RAM

/* Per-level base offset in tree_buf (matches kgen_ldl_tree.c). */
static size_t
tree_node_offset(unsigned logn_root, unsigned level, size_t index)
{
	size_t level_base = ((size_t)(logn_root - level)) << (logn_root + 4);
	size_t per_node = ((size_t)1 << (level + 1)) * sizeof(fpr);
	return level_base + index * per_node;
}

/* Recursive validator. Decomposes G in place, compares (l10, d00, d11)
 * against tree contents at (level, index), then splits and recurses. */
static int
validate_subtree(const uint8_t *tree_buf, unsigned logn_root,
	unsigned level, size_t index,
	fpr *g00, fpr *g01, fpr *g11,
	fpr *scratch)
{
	fpoly_LDL_fft(level, g00, g01, g11);

	const fpr *node = (const fpr *)
		(tree_buf + tree_node_offset(logn_root, level, index));
	size_t full = (size_t)1 << level;
	size_t half = (size_t)1 << (level - 1);

	if (memcmp(node, g01, full * sizeof(fpr)) != 0) {
		fprintf(stderr,
			"l10 mismatch at level=%u index=%zu\n", level, index);
		return 0;
	}
	if (memcmp(node + full, g00, half * sizeof(fpr)) != 0) {
		fprintf(stderr,
			"d00 mismatch at level=%u index=%zu\n", level, index);
		return 0;
	}
	if (memcmp(node + full + half, g11, half * sizeof(fpr)) != 0) {
		fprintf(stderr,
			"d11 mismatch at level=%u index=%zu\n", level, index);
		return 0;
	}

	if (level <= 2) {
		return 1;
	}

	unsigned cl = level - 1;
	size_t cf = (size_t)1 << cl;
	size_t ch = (size_t)1 << (cl - 1);

	fpr *r_g00 = scratch;
	fpr *r_g01 = scratch + ch;
	fpr *r_g11 = scratch + ch + cf;
	fpoly_split_selfadj_fft(level, r_g00, r_g01, g11);
	memcpy(r_g11, r_g00, ch * sizeof(fpr));
	fpr *child_scratch = scratch + 2 * ch + cf;

	if (!validate_subtree(tree_buf, logn_root, cl, index * 2,
		r_g00, r_g01, r_g11, child_scratch))
	{
		return 0;
	}

	fpr *l_g00 = scratch;
	fpr *l_g01 = scratch + ch;
	fpr *l_g11 = scratch + ch + cf;
	fpoly_split_selfadj_fft(level, l_g00, l_g01, g00);
	memcpy(l_g11, l_g00, ch * sizeof(fpr));

	return validate_subtree(tree_buf, logn_root, cl, index * 2 + 1,
		l_g00, l_g01, l_g11, child_scratch);
}

/* Count of nodes at level k in the tree: 2^(logn_root - k). */
static size_t
nodes_at_level(unsigned logn_root, unsigned level)
{
	return (size_t)1 << (logn_root - level);
}

static int
run_test(unsigned logn)
{
	size_t n = (size_t)1 << logn;
	size_t sk_len = FNDSA_SIGN_KEY_SIZE(logn);
	size_t vk_len = FNDSA_VRFY_KEY_SIZE(logn);
	size_t basis_len = FNDSA_BASIS_SIZE(logn);
	size_t tree_len = FNDSA_LDL_TREE_SIZE(logn);
	size_t tmp_len = ((size_t)4 << logn) * sizeof(fpr) + 31;

	uint8_t *sk = malloc(sk_len);
	uint8_t *vk = malloc(vk_len);
	/* aligned_alloc requires size be a multiple of alignment. */
	uint8_t *basis = aligned_alloc(8, (basis_len + 7) & ~(size_t)7);
	uint8_t *tree = aligned_alloc(8, (tree_len + 7) & ~(size_t)7);
	uint8_t *tmp = aligned_alloc(8, (tmp_len + 7) & ~(size_t)7);

	uint8_t seed[32];
	for (size_t i = 0; i < sizeof seed; i++) seed[i] = (uint8_t)(i + logn);
	fndsa_keygen_seeded(logn, seed, sizeof seed, sk, vk);

	if (!fndsa_compute_basis(sk, sk_len, basis, basis_len)) {
		fprintf(stderr, "[logn=%u] compute_basis failed\n", logn);
		return 1;
	}

	memset(tree, 0xAA, tree_len);
	int rc = fndsa_compute_ldl_tree(logn,
		basis, basis_len, tree, tree_len, tmp, tmp_len);
	if (rc != 1) {
		fprintf(stderr, "[logn=%u] compute_ldl_tree failed\n", logn);
		return 1;
	}

	printf("[logn=%u] tree_size=%zu bytes ", logn, tree_len);

	/* Independently compute outer Gram and walk the tree. */
	fpr *ref_g00 = aligned_alloc(8, n / 2 * sizeof(fpr));
	fpr *ref_g01 = aligned_alloc(8, n * sizeof(fpr));
	fpr *ref_g11 = aligned_alloc(8, n / 2 * sizeof(fpr));
	fpr *ref_scratch = aligned_alloc(8,
		(((size_t)4 << logn) * sizeof(fpr) + 7) & ~(size_t)7);

	fpoly_gram_fft_dst(logn, ref_g00, ref_g01, ref_g11,
		(const fpr *)basis);

	if (!validate_subtree(tree, logn, logn, 0,
		ref_g00, ref_g01, ref_g11, ref_scratch))
	{
		return 1;
	}

	/* Print nodes-per-level for visibility. */
	size_t total_nodes = 0;
	for (unsigned k = 2; k <= logn; k++) {
		total_nodes += nodes_at_level(logn, k);
	}
	printf("%zu nodes verified ", total_nodes);

	free(sk); free(vk); free(basis); free(tree); free(tmp);
	free(ref_g00); free(ref_g01); free(ref_g11); free(ref_scratch);
	printf("PASS\n");
	return 0;
}

int
main(void)
{
	if (run_test(9) != 0) return 1;
	if (run_test(10) != 0) return 1;
	printf("All tests passed.\n");
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
