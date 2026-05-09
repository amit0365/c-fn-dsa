/*
 * Top-level signature generation functions.
 */

#include "sign_inner.h"

/* Verified properties at this point:
      degree is acceptable
      encoded signing key has the proper size
      signature buffer is large enough to receive the result
      tmp is large enough (but not necessarily aligned)  */
static size_t
sign_step1(unsigned logn, const uint8_t *sign_key,
	const uint8_t *ctx, size_t ctx_len,
	const char *id, const uint8_t *hv, size_t hv_len,
	const uint8_t *seed, size_t seed_len,
	uint8_t *sig, void *tmp
#if FNDSA_LOW_RAM
	, const fpr *external_basis  /* NULL = compute internally */
	, const uint8_t *external_tree /* NULL = on-the-fly LDL */
#endif
	)
{
	size_t n = (size_t)1 << logn;

	/* Align tmp to a 32-byte boundary. */
	tmp = (void *)(((uintptr_t)tmp + 31) & ~(uintptr_t)31);

	/* We decode f, g and F into a temporary area, and use them
	   to recompute G. Only G will be provided in decoded format
	   to sign_core(); f, g and F can be redecoded cheaply from
	   the encoded key when needed. */
	int8_t *f = (int8_t *)tmp + 4 * n;
	int8_t *g = f + n;
	int8_t *F = g + n;
	/* G's offset depends on the chosen tmp[] layout:
	     baseline (59n+31):                       58n bytes
	     FNDSA_LOW_RAM with basis (37n+31):       36n bytes
	         (G placed after hm at 34n + 2n; ffsamp outer peak is
	         4n fpr via fpoly_muladd_fft + l10 recompute.)
	     FNDSA_LOW_RAM no-basis sign path:        50n bytes */
	size_t G_offset_n;
#if FNDSA_LOW_RAM
	G_offset_n = (external_basis != NULL) ? 36 : 50;
#else
	G_offset_n = 58;
#endif
	int8_t *G = (int8_t *)tmp + (G_offset_n << logn);

	/* Decode the private key. Header byte and length have already
	   been verified. */
	unsigned nbits;
	switch (logn) {
	case 2: case 3: case 4: case 5:
		nbits = 8;
		break;
	case 6: case 7:
		nbits = 7;
		break;
	case 8: case 9:
		nbits = 6;
		break;
	default:
		nbits = 5;
		break;
	}
	size_t k, j = 1;
	k = trim_i8_decode(logn, sign_key + j, f, nbits);
	if (k == 0) {
		return 0;
	}
	j += k;
	k = trim_i8_decode(logn, sign_key + j, g, nbits);
	if (k == 0) {
		return 0;
	}
	j += k;
	k = trim_i8_decode(logn, sign_key + j, F, 8);
	if (k == 0) {
		return 0;
	}

	/* Rebuild G and the public polynomial h:
	      h = g/f mod X^n+1 mod q
	      G = h*F mod X^n+1 mod q
	   We also compute the SHAKE256 hash of the verifying key.
	   Under FNDSA_LOW_RAM with external_basis, sign_core's post-ffsamp
	   uses basis-direct multiplication (B2) and never reads G[]; we
	   skip the G derivation/storage. The basis was already validated
	   by fndsa_compute_basis() so G's range check isn't needed here. */
	uint16_t *t1 = (uint16_t *)tmp;
	uint16_t *t0 = t1 + n;
	/* t0 <- h = g/f */
	mqpoly_small_to_int(logn, g, t0);
	mqpoly_small_to_int(logn, f, t1);
	mqpoly_int_to_ntt(logn, t0);
	mqpoly_int_to_ntt(logn, t1);
	if (!mqpoly_div_ntt(logn, t0, t1)) {
		/* f is not invertible; the key is not valid */
		return 0;
	}
#if FNDSA_LOW_RAM
	if (external_basis == NULL)
#endif
	{
		/* t1 <- G = h*F (skipped when external_basis is provided
		   since sign_core's basis-direct path doesn't need G[]). */
		mqpoly_small_to_int(logn, F, t1);
		mqpoly_int_to_ntt(logn, t1);
		mqpoly_mul_ntt(logn, t1, t0);
		mqpoly_ntt_to_int(logn, t1);
		if (!mqpoly_int_to_small(logn, t1, G)) {
			/* coefficients of G are out-of-range */
			return 0;
		}
	}
	/* t0 contains h (in ntt representation), we encode and hash
	   the verifying key.
	   TODO: if the original Falcon mode is retained, then we can
	   skip both encoding and hashing. */
	mqpoly_ntt_to_int(logn, t0);
	mqpoly_int_to_ext(logn, t0);
	uint8_t *vrfy_key = (uint8_t *)t1;
	vrfy_key[0] = 0x00 + logn;
	mqpoly_encode(logn, t0, vrfy_key + 1);

	/* The shake_context (208 bytes) lives on the stack rather than in
	   tmp[]. The earlier scheme placed it at t0 = tmp+2n bytes, which
	   under FNDSA_LOW_RAM at logn=2 (G_offset = 50n = 200) overlapped G
	   by 16 bytes (gap was 48n = 192 < 208), corrupting G before
	   sign_core read it. */
	uint8_t hashed_key[64];
	shake_context sc;
	shake_init(&sc, 256);
	shake_inject(&sc, vrfy_key, FNDSA_VRFY_KEY_SIZE(logn));
	shake_flip(&sc);
	shake_extract(&sc, hashed_key, sizeof hashed_key);

	/* We now have G, and we checked that f, g and F can be decoded
	   successfully (no out-of-range element). Hashed public key is in
	   hashed_key[]. We can proceed to the main signing loop. */
	return sign_core(logn, sign_key + 1, G, hashed_key,
		ctx, ctx_len, id, hv, hv_len,
		seed, seed_len, sig, tmp
#if FNDSA_LOW_RAM
		, external_basis
		, external_tree
#endif
		);

	/* TODO: maybe explicitly overwrite the whole temporary area with
	   zeros? Arguably this is mostly wasted time if the area is
	   allocated on the stack, and if tmp is provided explicitly then
	   it is the responsibility of the caller to do any appropriate
	   zeroizing. */
}

/* Custom wrappers to allocate the temporary buffers on the stack. Several
   wrappers are defined so that stack allocation is not always worst-case. */
#if FNDSA_LOW_RAM
#define SIGN_WRAP_TMP_FACTOR  51
#else
#define SIGN_WRAP_TMP_FACTOR  59
#endif
#if FNDSA_LOW_RAM
#define SIGN_STEP1_NO_BASIS_ARG  , NULL, NULL
#else
#define SIGN_STEP1_NO_BASIS_ARG
#endif
#define SIGN_WRAP(sz)   \
	static size_t sign_ ## sz(unsigned logn, \
		const uint8_t *sign_key, \
		const uint8_t *ctx, size_t ctx_len, \
		const char *id, const uint8_t *hv, size_t hv_len, \
		const uint8_t *seed, size_t seed_len, \
		uint8_t *sig) \
	{ \
		uint8_t tmp[(sz) * SIGN_WRAP_TMP_FACTOR + 31]; \
		return sign_step1(logn, \
			sign_key, ctx, ctx_len, id, hv, hv_len, \
			seed, seed_len, sig, tmp \
			SIGN_STEP1_NO_BASIS_ARG); \
	}

SIGN_WRAP(32)
SIGN_WRAP(64)
SIGN_WRAP(128)
SIGN_WRAP(256)
SIGN_WRAP(512)
SIGN_WRAP(1024)

static size_t
sign_wrapper(int weak,
	const uint8_t *sign_key, size_t sign_key_len,
	const uint8_t *ctx, size_t ctx_len,
	const char *id, const uint8_t *hv, size_t hv_len,
	const uint8_t *seed, size_t seed_len,
	uint8_t *sig, size_t max_sig_len,
	void *tmp, size_t tmp_len)
{
	/* Signing key defines the degree to use. */
	if (sign_key_len == 0) {
		return 0;
	}
	unsigned head = sign_key[0];
	if ((head & 0xF0) != 0x50) {
		return 0;
	}
	unsigned logn = head & 0x0F;
	if (weak) {
		if (logn < 2 || logn > 8) {
			return 0;
		}
	} else {
		if (logn < 9 || logn > 10) {
			return 0;
		}
	}
	if (sign_key_len != FNDSA_SIGN_KEY_SIZE(logn)) {
		return 0;
	}
	if (sig == NULL) {
		return FNDSA_SIGNATURE_SIZE(logn);
	}
	if (max_sig_len < FNDSA_SIGNATURE_SIZE(logn)) {
		return 0;
	}

	/* We have checked that the degree is acceptable, the signing key
	   size is correct, and the signature will fit in the output buffer. */
	if (tmp == NULL) {
		switch (logn) {
		case 6:
			return sign_64(logn,
				sign_key, ctx, ctx_len, id, hv, hv_len,
				seed, seed_len, sig);
		case 7:
			return sign_128(logn,
				sign_key, ctx, ctx_len, id, hv, hv_len,
				seed, seed_len, sig);
		case 8:
			return sign_256(logn,
				sign_key, ctx, ctx_len, id, hv, hv_len,
				seed, seed_len, sig);
		case 9:
			return sign_512(logn,
				sign_key, ctx, ctx_len, id, hv, hv_len,
				seed, seed_len, sig);
		case 10:
			return sign_1024(logn,
				sign_key, ctx, ctx_len, id, hv, hv_len,
				seed, seed_len, sig);
		default:
			return sign_32(logn,
				sign_key, ctx, ctx_len, id, hv, hv_len,
				seed, seed_len, sig);
		}
	} else {
#if FNDSA_LOW_RAM
		if (tmp_len < (((size_t)51 << logn) + 31)) {
			return 0;
		}
#else
		if (tmp_len < (((size_t)59 << logn) + 31)) {
			return 0;
		}
#endif
		return sign_step1(logn,
			sign_key, ctx, ctx_len, id, hv, hv_len,
			seed, seed_len, sig, tmp
			SIGN_STEP1_NO_BASIS_ARG);
	}
}

/* see fndsa.h */
size_t
fndsa_sign(const void *sign_key, size_t sign_key_len,
	const void *ctx, size_t ctx_len,
	const char *id, const void *hv, size_t hv_len,
	void *sig, size_t max_sig_len)
{
	return sign_wrapper(0, sign_key, sign_key_len,
		ctx, ctx_len, id, hv, hv_len,
		NULL, 0, sig, max_sig_len, NULL, 0);
}

/* see fndsa.h */
size_t
fndsa_sign_seeded(const void *sign_key, size_t sign_key_len,
	const void *ctx, size_t ctx_len,
	const char *id, const void *hv, size_t hv_len,
	const void *seed, size_t seed_len,
	void *sig, size_t max_sig_len)
{
	return sign_wrapper(0, sign_key, sign_key_len,
		ctx, ctx_len, id, hv, hv_len,
		seed, seed_len, sig, max_sig_len, NULL, 0);
}

/* see fndsa.h */
size_t
fndsa_sign_temp(const void *sign_key, size_t sign_key_len,
	const void *ctx, size_t ctx_len,
	const char *id, const void *hv, size_t hv_len,
	void *sig, size_t max_sig_len,
	void *tmp, size_t tmp_len)
{
	return sign_wrapper(0, sign_key, sign_key_len,
		ctx, ctx_len, id, hv, hv_len,
		NULL, 0, sig, max_sig_len, tmp, tmp_len);
}

/* see fndsa.h */
size_t
fndsa_sign_seeded_temp(const void *sign_key, size_t sign_key_len,
	const void *ctx, size_t ctx_len,
	const char *id, const void *hv, size_t hv_len,
	const void *seed, size_t seed_len,
	void *sig, size_t max_sig_len,
	void *tmp, size_t tmp_len)
{
	return sign_wrapper(0, sign_key, sign_key_len,
		ctx, ctx_len, id, hv, hv_len,
		seed, seed_len, sig, max_sig_len, tmp, tmp_len);
}

/* see fndsa.h */
size_t
fndsa_sign_weak(const void *sign_key, size_t sign_key_len,
	const void *ctx, size_t ctx_len,
	const char *id, const void *hv, size_t hv_len,
	void *sig, size_t max_sig_len)
{
	return sign_wrapper(1, sign_key, sign_key_len,
		ctx, ctx_len, id, hv, hv_len,
		NULL, 0, sig, max_sig_len, NULL, 0);
}

/* see fndsa.h */
size_t
fndsa_sign_weak_seeded(const void *sign_key, size_t sign_key_len,
	const void *ctx, size_t ctx_len,
	const char *id, const void *hv, size_t hv_len,
	const void *seed, size_t seed_len,
	void *sig, size_t max_sig_len)
{
	return sign_wrapper(1, sign_key, sign_key_len,
		ctx, ctx_len, id, hv, hv_len,
		seed, seed_len, sig, max_sig_len, NULL, 0);
}

/* see fndsa.h */
size_t
fndsa_sign_weak_temp(const void *sign_key, size_t sign_key_len,
	const void *ctx, size_t ctx_len,
	const char *id, const void *hv, size_t hv_len,
	void *sig, size_t max_sig_len,
	void *tmp, size_t tmp_len)
{
	return sign_wrapper(1, sign_key, sign_key_len,
		ctx, ctx_len, id, hv, hv_len,
		NULL, 0, sig, max_sig_len, tmp, tmp_len);
}

/* see fndsa.h */
size_t
fndsa_sign_weak_seeded_temp(const void *sign_key, size_t sign_key_len,
	const void *ctx, size_t ctx_len,
	const char *id, const void *hv, size_t hv_len,
	const void *seed, size_t seed_len,
	void *sig, size_t max_sig_len,
	void *tmp, size_t tmp_len)
{
	return sign_wrapper(1, sign_key, sign_key_len,
		ctx, ctx_len, id, hv, hv_len,
		seed, seed_len, sig, max_sig_len, tmp, tmp_len);
}

#if FNDSA_LOW_RAM
/* ====================================================================
 * FNDSA_LOW_RAM: precomputed-basis API
 * ==================================================================== */

/* see fndsa.h */
int
fndsa_compute_basis(
	const void *sign_key, size_t sign_key_len,
	void *basis_buf, size_t basis_buf_len)
{
	if (sign_key == NULL || sign_key_len < 1) {
		return 0;
	}
	unsigned head = ((const uint8_t *)sign_key)[0];
	if ((head & 0xF0) != 0x50) {
		return 0;  /* invalid header byte */
	}
	unsigned logn = head & 0x0F;
	if (logn < 9 || logn > 10) {
		return 0;
	}
	size_t n = (size_t)1 << logn;

	if (basis_buf == NULL || basis_buf_len < FNDSA_BASIS_SIZE(logn)) {
		return 0;
	}
	if ((uintptr_t)basis_buf & 7) {
		return 0;  /* fpr requires 8-byte alignment */
	}

	/* Stack scratch: 8n bytes total. At logn=10, n=1024, that's 8 KiB.
	   At logn=9, 4 KiB. Acceptable for provisioning-time stack. */
	uint8_t scratch[(size_t)8 << 10];
	if (8 * n > sizeof scratch) {
		return 0;  /* defensive — shouldn't happen at logn 9, 10 */
	}
	int8_t *f = (int8_t *)scratch;
	int8_t *g = f + n;
	int8_t *F_buf = g + n;
	int8_t *G_buf = F_buf + n;
	uint16_t *t0_buf = (uint16_t *)(G_buf + n);
	uint16_t *t1_buf = t0_buf + n;

	/* Decode key. The gate above restricts logn to {9, 10}, so the
	   switch only needs those two cases. */
	unsigned nbits;
	switch (logn) {
	case 9: nbits = 6; break;
	case 10: nbits = 5; break;
	default: return 0;  /* unreachable, defensive */
	}
	size_t flen = (nbits << logn) >> 3;
	/* Expected sign_key layout: 1 byte header + flen + flen + n bytes. */
	if (sign_key_len < (size_t)1 + flen + flen + n) {
		return 0;
	}

	const uint8_t *sk_bytes = (const uint8_t *)sign_key;
	size_t k, j = 1;
	k = trim_i8_decode(logn, sk_bytes + j, f, nbits);
	if (k == 0) return 0;
	j += k;
	k = trim_i8_decode(logn, sk_bytes + j, g, nbits);
	if (k == 0) return 0;
	j += k;
	k = trim_i8_decode(logn, sk_bytes + j, F_buf, 8);
	if (k == 0) return 0;

	/* Recompute G via NTT (mirrors sign_step1's logic). */
	mqpoly_small_to_int(logn, g, t0_buf);
	mqpoly_small_to_int(logn, f, t1_buf);
	mqpoly_int_to_ntt(logn, t0_buf);
	mqpoly_int_to_ntt(logn, t1_buf);
	if (!mqpoly_div_ntt(logn, t0_buf, t1_buf)) {
		return 0;
	}
	mqpoly_small_to_int(logn, F_buf, t1_buf);
	mqpoly_int_to_ntt(logn, t1_buf);
	mqpoly_mul_ntt(logn, t1_buf, t0_buf);
	mqpoly_ntt_to_int(logn, t1_buf);
	if (!mqpoly_int_to_small(logn, t1_buf, G_buf)) {
		return 0;
	}

	/* Build basis B = [[g, -f], [G, -F]] in FFT representation,
	   matching basis_to_FFT in sign_core.c. */
	fpr *basis = (fpr *)basis_buf;
	fpr *b00 = basis;
	fpr *b01 = b00 + n;
	fpr *b10 = b01 + n;
	fpr *b11 = b10 + n;
	fpoly_set_small(logn, b01, f);
	fpoly_set_small(logn, b00, g);
	fpoly_set_small(logn, b11, F_buf);
	fpoly_set_small(logn, b10, G_buf);
	fpoly_FFT(logn, b01);
	fpoly_FFT(logn, b00);
	fpoly_FFT(logn, b11);
	fpoly_FFT(logn, b10);
	fpoly_neg(logn, b01);
	fpoly_neg(logn, b11);

	return 1;
}

/* Internal helper: validates and dispatches to sign_step1 with the
   external basis. Mirrors sign_wrapper but for the precomputed-basis
   variant; uses the 37n+31 tmp_len threshold.
   Optional `tree` parameter (NULL when not used) selects the tree-reading
   ffsamp variant (ffsamp_fft_with_tree) instead of on-the-fly LDL.
   The tree variant retains the same 37n+31 minimum: while the tree
   path doesn't store l10/d00/d11 in qc[] (B1 step 2 eliminated those
   memcpys), the qc slots that hold them are interleaved within the
   ffsamp layout (not at the end), so the buffer cannot be shrunk
   without an algorithmic restructure of the recursive body's qc
   offsets. */
static size_t
sign_with_basis_wrapper(
	const uint8_t *sign_key, size_t sign_key_len,
	const fpr *basis, const uint8_t *tree,
	const uint8_t *ctx, size_t ctx_len,
	const char *id, const uint8_t *hv, size_t hv_len,
	const uint8_t *seed, size_t seed_len,
	uint8_t *sig, size_t max_sig_len,
	void *tmp, size_t tmp_len)
{
	if (sign_key == NULL || sign_key_len < 1 || basis == NULL) {
		return 0;
	}
	if ((uintptr_t)basis & 7) {
		return 0;
	}
	if (tree != NULL && ((uintptr_t)tree & 7)) {
		return 0;
	}
	unsigned head = sign_key[0];
	if ((head & 0xF0) != 0x50) {
		return 0;
	}
	unsigned logn = head & 0x0F;
	if (logn < 9 || logn > 10) {
		return 0;
	}
	if (sign_key_len != FNDSA_SIGN_KEY_SIZE(logn)) {
		return 0;
	}
	if (max_sig_len < FNDSA_SIGNATURE_SIZE(logn)) {
		return 0;
	}
	/* tmp[] layout under FNDSA_LOW_RAM (with basis, B2 active):
	     ffsamp peak (34n) + hm (2n) + 31 align = 36n+31 bytes
	     G is no longer stored: sign_step1 skips its derivation when
	     external_basis is provided, and sign_core's basis-direct
	     post-ffsamp reads basis in FFT form instead of int8 G[].
	     Saves n bytes vs the previous 37n+31 lower bound.
	     The tree variant uses the same minimum (the qc-layout
	     interleavings prevent shrinking further within ffsamp).
	     Further reduction requires hm-recompute (separate optimization). */
	if (tmp == NULL || tmp_len < (((size_t)36 << logn) + 31)) {
		return 0;
	}

	return sign_step1(logn,
		sign_key, ctx, ctx_len, id, hv, hv_len,
		seed, seed_len, sig, tmp, basis, tree);
}

/* see fndsa.h */
size_t
fndsa_sign_with_basis_temp(
	const void *sign_key, size_t sign_key_len,
	const void *basis,
	const void *ctx, size_t ctx_len,
	const char *id, const void *hv, size_t hv_len,
	void *sig, size_t max_sig_len,
	void *tmp, size_t tmp_len)
{
	return sign_with_basis_wrapper(
		sign_key, sign_key_len,
		(const fpr *)basis, NULL,
		ctx, ctx_len, id, hv, hv_len,
		NULL, 0, sig, max_sig_len, tmp, tmp_len);
}

/* see fndsa.h */
size_t
fndsa_sign_seeded_with_basis_temp(
	const void *sign_key, size_t sign_key_len,
	const void *basis,
	const void *ctx, size_t ctx_len,
	const char *id, const void *hv, size_t hv_len,
	const void *seed, size_t seed_len,
	void *sig, size_t max_sig_len,
	void *tmp, size_t tmp_len)
{
	return sign_with_basis_wrapper(
		sign_key, sign_key_len,
		(const fpr *)basis, NULL,
		ctx, ctx_len, id, hv, hv_len,
		seed, seed_len, sig, max_sig_len, tmp, tmp_len);
}

/* see fndsa.h */
size_t
fndsa_sign_with_basis_and_tree_temp(
	const void *sign_key, size_t sign_key_len,
	const void *basis, const void *tree,
	const void *ctx, size_t ctx_len,
	const char *id, const void *hv, size_t hv_len,
	void *sig, size_t max_sig_len,
	void *tmp, size_t tmp_len)
{
	return sign_with_basis_wrapper(
		sign_key, sign_key_len,
		(const fpr *)basis, (const uint8_t *)tree,
		ctx, ctx_len, id, hv, hv_len,
		NULL, 0, sig, max_sig_len, tmp, tmp_len);
}

/* see fndsa.h */
size_t
fndsa_sign_seeded_with_basis_and_tree_temp(
	const void *sign_key, size_t sign_key_len,
	const void *basis, const void *tree,
	const void *ctx, size_t ctx_len,
	const char *id, const void *hv, size_t hv_len,
	const void *seed, size_t seed_len,
	void *sig, size_t max_sig_len,
	void *tmp, size_t tmp_len)
{
	return sign_with_basis_wrapper(
		sign_key, sign_key_len,
		(const fpr *)basis, (const uint8_t *)tree,
		ctx, ctx_len, id, hv, hv_len,
		seed, seed_len, sig, max_sig_len, tmp, tmp_len);
}

#endif /* FNDSA_LOW_RAM */
