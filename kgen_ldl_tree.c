/*
 * LDL tree precomputation (FNDSA_LDL_TREE / B0).
 *
 * Walks the same recursive structure as ffsamp_fft_inner but emits each
 * level's decomposed (l10, d00, d11) values to a flash-resident output
 * buffer instead of doing the work on-the-fly during sign.
 *
 * Tree layout (level-major BFS for indexed access):
 *
 *   Level k holds 2^(logn-k) nodes, each of size 2^(k+1) fpr:
 *     l10   2^k    fpr  (full polynomial in FFT representation)
 *     d00   2^(k-1) fpr (self-adjoint half)
 *     d11   2^(k-1) fpr (self-adjoint half)
 *
 *   Per-level total is 2n fpr = 16n bytes (constant across levels).
 *   Stored levels are 2..logn; level 1 is handled inline by the deepest
 *   sampler at sign time (no tree node needed).
 *
 *   Index within level: 0 = first right-descended sub-tree, 1 = first
 *   left-descended sub-tree, etc. Parent index i at level k+1 has
 *   children 2i (right) and 2i+1 (left) at level k.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "fndsa.h"
#include "inner.h"
#include "sign_inner.h"

#if FNDSA_LOW_RAM

/* Compute the byte offset within tree_buf for a given (level, index).
 * Each level occupies 16n bytes; nodes within a level are contiguous. */
static size_t
tree_node_offset(unsigned logn_root, unsigned level, size_t index)
{
	size_t level_base = ((size_t)(logn_root - level)) << (logn_root + 4);
	size_t per_node = ((size_t)1 << (level + 1)) * sizeof(fpr);
	return level_base + index * per_node;
}

/* Emit the decomposed values (l10, d00, d11) for one level/index slot. */
static void
emit_node(uint8_t *tree_buf, unsigned logn_root,
	unsigned level, size_t index,
	const fpr *l10, const fpr *d00, const fpr *d11)
{
	fpr *node = (fpr *)(tree_buf + tree_node_offset(logn_root, level, index));
	size_t full = (size_t)1 << level;
	size_t half = (size_t)1 << (level - 1);
	memcpy(node, l10, full * sizeof(fpr));
	memcpy(node + full, d00, half * sizeof(fpr));
	memcpy(node + full + half, d11, half * sizeof(fpr));
}

/* Recursive tree-emission helper.
 *
 * On entry, (g00, g01, g11) hold the sub-Gram for this (level, index)
 * node — g00 and g11 self-adjoint (only first 2^(level-1) coeffs read),
 * g01 full (2^level fpr). All in FFT representation.
 *
 * Decomposes in place: g01 becomes l10, g11 becomes d11, g00 stays as d00.
 * Writes node to tree_buf, then splits d00 and d11 into child sub-Gram
 * values and recurses on the right and left sub-trees.
 *
 * scratch must be at least 2 * 2^level fpr (used for children's sub-Gram
 * during recursion; reused across right/left calls). */
static void
emit_subtree(uint8_t *tree_buf, unsigned logn_root,
	unsigned level, size_t index,
	fpr *g00, fpr *g01, fpr *g11,
	fpr *scratch)
{
	/* In-place LDL: g01 -> l10 (full), g11 -> d11 (self-adj),
	   g00 unchanged as d00 (self-adj). */
	fpoly_LDL_fft(level, g00, g01, g11);

	/* Persist (l10, d00, d11) to flash buffer. */
	emit_node(tree_buf, logn_root, level, index, g01, g00, g11);

	if (level <= 2) {
		/* Bottom stored level. Level 1 is handled by ffsamp_fft_deepest
		   inline at sign time using the d11 we just emitted. */
		return;
	}

	unsigned child_level = level - 1;
	size_t child_full = (size_t)1 << child_level;
	size_t child_half = (size_t)1 << (child_level - 1);

	/* Right sub-tree: split d11 self-adj into right_g00 (self-adj)
	   and right_g01 (full). The right sub-Gram has g11 = g00 by
	   self-adjoint symmetry, so we copy g00 into the g11 slot. */
	fpr *r_g00 = scratch;
	fpr *r_g01 = scratch + child_half;
	fpr *r_g11 = scratch + child_half + child_full;
	fpoly_split_selfadj_fft(level, r_g00, r_g01, g11);
	memcpy(r_g11, r_g00, child_half * sizeof(fpr));

	/* Child's scratch follows our work region. */
	fpr *child_scratch = scratch + 2 * child_half + child_full;

	emit_subtree(tree_buf, logn_root, child_level, index * 2,
		r_g00, r_g01, r_g11, child_scratch);

	/* Left sub-tree: split d00 self-adj into left_g00 / left_g01.
	   Reuse the same scratch region — right recursion has returned
	   and its sub-Gram is no longer needed. */
	fpr *l_g00 = scratch;
	fpr *l_g01 = scratch + child_half;
	fpr *l_g11 = scratch + child_half + child_full;
	fpoly_split_selfadj_fft(level, l_g00, l_g01, g00);
	memcpy(l_g11, l_g00, child_half * sizeof(fpr));

	emit_subtree(tree_buf, logn_root, child_level, index * 2 + 1,
		l_g00, l_g01, l_g11, child_scratch);
}

/* Public entry point: precompute the LDL tree for a given basis.
 *
 * The basis is the 4n fpr buffer produced by fndsa_compute_basis().
 * On success, tree_buf holds the full LDL tree in the format described
 * at the top of this file.
 *
 * tmp layout:
 *   [0..3n fpr]       outer-level Gram (g00 self-adj n/2 + g01 full n + g11 self-adj n/2)
 *                     plus working space — total 2n fpr active at outer
 *   [3n..]            recursion scratch for child sub-Gram allocations */
int
fndsa_compute_ldl_tree(unsigned logn,
	const void *basis, size_t basis_len,
	void *tree_buf, size_t tree_buf_len,
	void *tmp, size_t tmp_len)
{
	if (logn < 2 || logn > FNDSA_LOGN_1024) {
		return 0;
	}
	if (basis_len < FNDSA_BASIS_SIZE(logn)) {
		return 0;
	}
	if (tree_buf_len < FNDSA_LDL_TREE_SIZE(logn)) {
		return 0;
	}
	/* Working memory: outer (g00 + g01 + g11) = 2n fpr +
	   recursion scratch ~2n fpr at outer. Use 4n+31 as the bound. */
	size_t need = ((size_t)4 << logn) * sizeof(fpr) + 31;
	if (tmp == NULL || tmp_len < need) {
		return 0;
	}
	if (tree_buf == NULL || basis == NULL) {
		return 0;
	}

	/* Align tmp to 8 bytes. */
	uintptr_t addr = (uintptr_t)tmp;
	size_t pad = (8 - (addr & 7)) & 7;
	uint8_t *t = (uint8_t *)tmp + pad;

	/* Lay out: g00 (n/2 fpr), g01 (n fpr), g11 (n/2 fpr) for outer level,
	   then scratch for recursion follows at offset 2n fpr. */
	size_t n = (size_t)1 << logn;
	fpr *g00 = (fpr *)t;
	fpr *g01 = g00 + (n / 2);
	fpr *g11 = g01 + n;
	fpr *scratch = g11 + (n / 2);

	/* Compute outer Gram from basis. fpoly_gram_fft_dst writes the
	   compact form (n/2 + n + n/2 fpr) directly. */
	fpoly_gram_fft_dst(logn, g00, g01, g11, (const fpr *)basis);

	/* Walk and emit. */
	emit_subtree((uint8_t *)tree_buf, logn, logn, /*index=*/0,
		g00, g01, g11, scratch);

	return 1;
}

#else /* !FNDSA_LOW_RAM */

int
fndsa_compute_ldl_tree(unsigned logn,
	const void *basis, size_t basis_len,
	void *tree_buf, size_t tree_buf_len,
	void *tmp, size_t tmp_len)
{
	(void)logn;
	(void)basis;
	(void)basis_len;
	(void)tree_buf;
	(void)tree_buf_len;
	(void)tmp;
	(void)tmp_len;
	return 0;
}

#endif
