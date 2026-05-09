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

/* End-to-end: sign with the tree-reading ffsamp path and bit-compare
 * against the on-the-fly path. */
static int
run_sign_compare(unsigned logn)
{
	size_t sk_len = FNDSA_SIGN_KEY_SIZE(logn);
	size_t vk_len = FNDSA_VRFY_KEY_SIZE(logn);
	size_t basis_len = FNDSA_BASIS_SIZE(logn);
	size_t tree_len = FNDSA_LDL_TREE_SIZE(logn);
	/* Use platform-aware sizing: SIMD with B2+Phase5 needs only 34n+31;
	   scalar still needs 37n+31. */
#if FNDSA_SSE2 || FNDSA_NEON || FNDSA_RV64D
	size_t tmp_len = (((size_t)34 << logn) + 31);
#else
	size_t tmp_len = (((size_t)37 << logn) + 31);
#endif
	size_t sig_len = FNDSA_SIGNATURE_SIZE(logn);

	uint8_t *sk = malloc(sk_len);
	uint8_t *vk = malloc(vk_len);
	uint8_t *basis = aligned_alloc(8, (basis_len + 7) & ~(size_t)7);
	uint8_t *tree = aligned_alloc(8, (tree_len + 7) & ~(size_t)7);
	uint8_t *tmp1 = aligned_alloc(8, (tmp_len + 7) & ~(size_t)7);
	uint8_t *tmp2 = aligned_alloc(8, (tmp_len + 7) & ~(size_t)7);
	uint8_t *sig1 = malloc(sig_len);
	uint8_t *sig2 = malloc(sig_len);

	uint8_t kseed[32];
	for (size_t i = 0; i < sizeof kseed; i++) {
		kseed[i] = (uint8_t)(i + logn);
	}
	fndsa_keygen_seeded(logn, kseed, sizeof kseed, sk, vk);
	if (!fndsa_compute_basis(sk, sk_len, basis, basis_len)) return 1;

	size_t tree_tmp_len = ((size_t)4 << logn) * sizeof(double) + 31;
	uint8_t *tree_tmp = aligned_alloc(8,
		(tree_tmp_len + 7) & ~(size_t)7);
	if (!fndsa_compute_ldl_tree(logn, basis, basis_len,
		tree, tree_len, tree_tmp, tree_tmp_len)) return 1;
	free(tree_tmp);

	const char *msg = "the quick brown fox jumps over the lazy dog";
	uint8_t sigseed[56];
	for (size_t i = 0; i < sizeof sigseed; i++) {
		sigseed[i] = (uint8_t)(0xAA + i);
	}

	size_t s1 = fndsa_sign_seeded_with_basis_temp(
		sk, sk_len, basis,
		NULL, 0, FNDSA_HASH_ID_RAW, msg, strlen(msg),
		sigseed, sizeof sigseed,
		sig1, sig_len, tmp1, tmp_len);

	size_t s2 = fndsa_sign_seeded_with_basis_and_tree_temp(
		sk, sk_len, basis, tree,
		NULL, 0, FNDSA_HASH_ID_RAW, msg, strlen(msg),
		sigseed, sizeof sigseed,
		sig2, sig_len, tmp2, tmp_len);

	if (s1 == 0 || s2 == 0) {
		fprintf(stderr, "[logn=%u] sign failed (s1=%zu s2=%zu)\n",
			logn, s1, s2);
		return 1;
	}
	if (s1 != s2 || memcmp(sig1, sig2, s1) != 0) {
		fprintf(stderr,
			"[logn=%u] SIGNATURE MISMATCH (s1=%zu s2=%zu)\n",
			logn, s1, s2);
		return 1;
	}
	printf("[logn=%u] sign-with-tree matches sign-on-the-fly (sig=%zu B) PASS\n",
		logn, s1);

	free(sk); free(vk); free(basis); free(tree);
	free(tmp1); free(tmp2); free(sig1); free(sig2);
	return 0;
}

/* Sentinel test (Test 5): poison tmp[] with 0xCC, run sign, find which
 * bytes were NEVER touched during the entire sign. Those bytes are
 * candidates for elimination via buffer overlap or layout reduction.
 *
 * Note: this finds bytes that were never WRITTEN. Bytes that are
 * read-only-then-untouched (e.g. constants we'd want to not allocate)
 * still show as "untouched" if no sign-internal code writes to them.
 * Bytes that are written-then-overwritten still show as "touched". */
static int
run_sentinel_test(unsigned logn)
{
	size_t sk_len = FNDSA_SIGN_KEY_SIZE(logn);
	size_t vk_len = FNDSA_VRFY_KEY_SIZE(logn);
	size_t basis_len = FNDSA_BASIS_SIZE(logn);
	size_t tree_len = FNDSA_LDL_TREE_SIZE(logn);
	/* Use platform-aware sizing: SIMD with B2+Phase5 needs only 34n+31;
	   scalar still needs 37n+31. */
#if FNDSA_SSE2 || FNDSA_NEON || FNDSA_RV64D
	size_t tmp_len = (((size_t)34 << logn) + 31);
#else
	size_t tmp_len = (((size_t)37 << logn) + 31);
#endif
	size_t sig_len = FNDSA_SIGNATURE_SIZE(logn);

	uint8_t *sk = malloc(sk_len);
	uint8_t *vk = malloc(vk_len);
	uint8_t *basis = aligned_alloc(8, (basis_len + 7) & ~(size_t)7);
	uint8_t *tree = aligned_alloc(8, (tree_len + 7) & ~(size_t)7);
	uint8_t *tmp = aligned_alloc(8, (tmp_len + 7) & ~(size_t)7);
	uint8_t *sig = malloc(sig_len);

	uint8_t kseed[32];
	for (size_t i = 0; i < sizeof kseed; i++) {
		kseed[i] = (uint8_t)(i + logn);
	}
	fndsa_keygen_seeded(logn, kseed, sizeof kseed, sk, vk);
	if (!fndsa_compute_basis(sk, sk_len, basis, basis_len)) return 1;

	size_t tree_tmp_len = ((size_t)4 << logn) * sizeof(fpr) + 31;
	uint8_t *tree_tmp = aligned_alloc(8,
		(tree_tmp_len + 7) & ~(size_t)7);
	if (!fndsa_compute_ldl_tree(logn, basis, basis_len,
		tree, tree_len, tree_tmp, tree_tmp_len)) return 1;
	free(tree_tmp);

	const char *msg = "the quick brown fox jumps over the lazy dog";
	uint8_t sigseed[56];
	for (size_t i = 0; i < sizeof sigseed; i++) {
		sigseed[i] = (uint8_t)(0xAA + i);
	}

	/* Poison tmp[] with sentinel before sign. */
	const uint8_t SENTINEL = 0xCC;
	memset(tmp, SENTINEL, tmp_len);

	size_t s = fndsa_sign_seeded_with_basis_and_tree_temp(
		sk, sk_len, basis, tree,
		NULL, 0, FNDSA_HASH_ID_RAW, msg, strlen(msg),
		sigseed, sizeof sigseed,
		sig, sig_len, tmp, tmp_len);
	if (s == 0) {
		fprintf(stderr, "[logn=%u] sign failed\n", logn);
		return 1;
	}

	/* Walk tmp[] and find regions still holding the sentinel. */
	size_t total_untouched = 0;
	size_t longest_run = 0;
	size_t current_run = 0;
	size_t longest_run_start = 0;
	size_t current_run_start = 0;
	size_t n = (size_t)1 << logn;
	for (size_t i = 0; i < tmp_len; i++) {
		if (tmp[i] == SENTINEL) {
			if (current_run == 0) current_run_start = i;
			current_run++;
			total_untouched++;
			if (current_run > longest_run) {
				longest_run = current_run;
				longest_run_start = current_run_start;
			}
		} else {
			current_run = 0;
		}
	}

	printf("[logn=%u, n=%zu] tmp_len=%zu (=37n+31)\n",
		logn, n, tmp_len);
	printf("  Untouched bytes total: %zu (= %.2f%% of tmp[])\n",
		total_untouched, 100.0 * total_untouched / tmp_len);
	printf("  Longest contiguous untouched run: %zu bytes\n",
		longest_run);
	if (longest_run > 0) {
		double byte_offset = (double)longest_run_start;
		printf("    starts at byte %zu (= byte %.2fn, qc(%.2f) at outer)\n",
			longest_run_start,
			byte_offset / (double)n,
			byte_offset / (2.0 * (double)n));
	}

	/* List all untouched runs >= 16 bytes (interesting ones). */
	printf("  Untouched runs >= 16 bytes:\n");
	current_run = 0;
	current_run_start = 0;
	for (size_t i = 0; i <= tmp_len; i++) {
		uint8_t b = (i < tmp_len) ? tmp[i] : 0;
		if (i < tmp_len && b == SENTINEL) {
			if (current_run == 0) current_run_start = i;
			current_run++;
		} else {
			if (current_run >= 16) {
				printf("    bytes %zu..%zu (%zu bytes, %.2f..%.2f n)\n",
					current_run_start, i,
					current_run,
					(double)current_run_start / (double)n,
					(double)i / (double)n);
			}
			current_run = 0;
		}
	}

	free(sk); free(vk); free(basis); free(tree); free(tmp); free(sig);
	return 0;
}

int
main(int argc, char **argv)
{
	int do_compare = (argc > 1 && argv[1][0] == 'c');
	int do_sentinel = (argc > 1 && argv[1][0] == 's');
	if (do_sentinel) {
		fprintf(stderr, "running sentinel logn=9...\n");
		if (run_sentinel_test(9) != 0) return 1;
		fprintf(stderr, "running sentinel logn=10...\n");
		if (run_sentinel_test(10) != 0) return 1;
		printf("Sentinel test passed.\n");
		return 0;
	}
	if (run_test(9) != 0) return 1;
	if (run_test(10) != 0) return 1;
	if (do_compare) {
		fprintf(stderr, "running sign-compare logn=9...\n");
		if (run_sign_compare(9) != 0) return 1;
		fprintf(stderr, "running sign-compare logn=10...\n");
		if (run_sign_compare(10) != 0) return 1;
	}
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
