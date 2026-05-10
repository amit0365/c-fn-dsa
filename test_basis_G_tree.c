/*
 * Verify combined sign API (basis + G + tree) matches single-source signs
 * bit-for-bit. Tests both SIMD and scalar builds via shared source.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "fndsa.h"
#include "inner.h"
#include "sign_inner.h"

#if FNDSA_LOW_RAM

static int
run_test(unsigned logn)
{
	size_t sk_len = FNDSA_SIGN_KEY_SIZE(logn);
	size_t vk_len = FNDSA_VRFY_KEY_SIZE(logn);
	size_t basis_len = FNDSA_BASIS_SIZE(logn);
	size_t g_len = FNDSA_G_SIZE(logn);
	size_t tree_len = FNDSA_LDL_TREE_SIZE(logn);
	size_t tmp_len_full = (((size_t)37 << logn) + 31);
	size_t tmp_len_min  = (((size_t)34 << logn) + 31);
	size_t sig_len = FNDSA_SIGNATURE_SIZE(logn);

	uint8_t *sk = malloc(sk_len);
	uint8_t *vk = malloc(vk_len);
	uint8_t *basis = aligned_alloc(8, (basis_len + 7) & ~(size_t)7);
	uint8_t *G = malloc(g_len);
	uint8_t *tree = aligned_alloc(8, (tree_len + 7) & ~(size_t)7);
	uint8_t *tmp1 = aligned_alloc(8, (tmp_len_full + 7) & ~(size_t)7);
	uint8_t *tmp2 = aligned_alloc(8, (tmp_len_min + 7) & ~(size_t)7);
	uint8_t *sig1 = malloc(sig_len);
	uint8_t *sig2 = malloc(sig_len);

	uint8_t kseed[32];
	for (size_t i = 0; i < sizeof kseed; i++) {
		kseed[i] = (uint8_t)(i + logn * 13);
	}
	fndsa_keygen_seeded(logn, kseed, sizeof kseed, sk, vk);
	if (!fndsa_compute_basis_and_G(sk, sk_len,
		basis, basis_len, G, g_len)) return 1;

	size_t tree_tmp_len = ((size_t)4 << logn) * sizeof(fpr) + 31;
	uint8_t *tree_tmp = aligned_alloc(8, (tree_tmp_len + 7) & ~(size_t)7);
	if (!fndsa_compute_ldl_tree(logn, basis, basis_len,
		tree, tree_len, tree_tmp, tree_tmp_len)) return 1;
	free(tree_tmp);

	const char *msg = "the quick brown fox jumps over the lazy dog";
	uint8_t sigseed[56];
	for (size_t i = 0; i < sizeof sigseed; i++) {
		sigseed[i] = (uint8_t)(0xCC + i);
	}

	size_t s1 = fndsa_sign_seeded_with_basis_temp(
		sk, sk_len, basis,
		NULL, 0, FNDSA_HASH_ID_RAW, msg, strlen(msg),
		sigseed, sizeof sigseed,
		sig1, sig_len, tmp1, tmp_len_full);

	size_t s2 = fndsa_sign_seeded_with_basis_G_and_tree_temp(
		sk, sk_len, basis, G, tree,
		NULL, 0, FNDSA_HASH_ID_RAW, msg, strlen(msg),
		sigseed, sizeof sigseed,
		sig2, sig_len, tmp2, tmp_len_min);

	if (s1 == 0 || s2 == 0) {
		fprintf(stderr, "[logn=%u] sign failed (s1=%zu s2=%zu)\n",
			logn, s1, s2);
		return 1;
	}
	int match = (s1 == s2 && memcmp(sig1, sig2, s1) == 0);
	printf("[logn=%u] basis-only vs basis+G+tree match: %s "
		"(sig=%zu B, tmp_len_min=%zu, tree=%zu B)\n",
		logn, match ? "YES" : "NO", s1, tmp_len_min, tree_len);

	free(sk); free(vk); free(basis); free(G); free(tree);
	free(tmp1); free(tmp2); free(sig1); free(sig2);
	return match ? 0 : 1;
}

int
main(void)
{
	if (run_test(9) != 0) return 1;
	if (run_test(10) != 0) return 1;
	printf("Combined basis+G+tree API verified.\n");
	return 0;
}

#else
int main(void) { printf("SKIP\n"); return 0; }
#endif
