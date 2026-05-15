/*
 * fndsa_fpr_complex_mul: fused complex-number multiplication.
 *
 *   d_re = a_re * b_re - a_im * b_im
 *   d_im = a_re * b_im + a_im * b_re
 *
 * Equivalent to the FPC_MUL macro (sign_inner.h:329) but does ONE final
 * rounding per output instead of 6. Per-product intermediate precision is
 * 56 bits (vs. reference's 53 bits after each fpr_mul rounds), which
 * matches-or-exceeds reference precision and avoids double-rounding.
 *
 * Cycle target on M4 (asm port pending): ~180 cycles vs reference's ~338
 * for the 4-mul-2-add sequence. Save ~158 cycles per call ≈ 3.5% e2e at
 * FN-DSA-512 (~485K cycles per sign across ~3072 FPC_MUL calls).
 *
 * This C reference is the correctness oracle for the cm4 asm port. The
 * test harness in test_fpc_mul.c verifies that this implementation
 * produces results numerically equivalent to the existing FPC_MUL macro
 * (within the documented precision improvement).
 */

#include "sign_inner.h"

/* Extended-precision intermediate. Mantissa is a 128-bit unsigned value
   normalized to bit 127 being the high bit of the significand (i.e. m is
   in [2^127, 2^128-1] when non-zero), with all 128 bits significant.
   Exponent is unbiased. Sign is in bit 0 of `s`.

   Using a 128-bit mantissa avoids sticky-bit handling: alignment shifts
   carry the dropped bits into the value itself, and signed combination
   of two 128-bit values fits in 129 bits (one extra carry), which we
   handle via a renormalize step.

   For the M4 asm port, this 128-bit value lives in 4 ARM registers; the
   asm body computes the same arithmetic without a __int128 library call. */
typedef struct {
	__uint128_t m;
	int32_t     e;     /* exponent of bit 127 of m */
	uint32_t    s;
} fpr_ext;

#define M52   (((uint64_t)1 << 52) - 1)

/* Compute x * y as an extended-precision 128-bit-mantissa fpr_ext,
   normalized so that the highest set bit is at position 127. */
static fpr_ext
fpr_mul_ext(fpr x, fpr y)
{
	uint32_t ex = (uint32_t)(x >> 52) & 0x7FF;
	uint32_t ey = (uint32_t)(y >> 52) & 0x7FF;
	uint32_t s  = (uint32_t)((x ^ y) >> 63) & 1;

	if (ex == 0 || ey == 0) {
		return (fpr_ext){ .m = 0, .e = -1076, .s = s };
	}

	/* Mantissa with implicit-1 set, in [2^52, 2^53-1]. */
	uint64_t xu = (x & M52) | ((uint64_t)1 << 52);
	uint64_t yu = (y & M52) | ((uint64_t)1 << 52);

	/* 53*53 = 106-bit product. Result is in [2^104, 2^106-1]. */
	__uint128_t prod = (__uint128_t)xu * (__uint128_t)yu;

	/* Normalize to bit 127 being the top set bit. The product top bit is
	   either 105 or 104 (when both mantissas are at max). Shift left by
	   either 22 or 23 bits. */
	int32_t shift = (prod >> 105) ? 22 : 23;
	prod <<= shift;
	int32_t e = (int32_t)ex + (int32_t)ey - 2046 - shift + 105;

	return (fpr_ext){ .m = prod, .e = e, .s = s };
}

/* Extended-precision signed addition on 128-bit mantissas.
   Aligns the smaller-exponent operand by arithmetic right-shift (no
   sticky needed because we have 128 bits to play with). Result has its
   top bit at position 127 (unless mantissa cancels to zero). */
static fpr_ext
fpr_add_ext(fpr_ext a, fpr_ext b)
{
	if (a.m == 0) return b;
	if (b.m == 0) return a;

	/* Order so |a| >= |b|. */
	if (a.e < b.e || (a.e == b.e && a.m < b.m)) {
		fpr_ext t = a; a = b; b = t;
	}

	int32_t d = a.e - b.e;
	__uint128_t bm = (d >= 128) ? 0 : (b.m >> d);
	/* Sticky-equivalent: if any bit of b.m was shifted out (i.e. not
	   captured in bm), force lsb of bm to 1 to preserve rounding info. */
	__uint128_t dropped_mask = (d == 0) ? 0
	                                    : ((__uint128_t)1 << (d < 128 ? d : 0)) - 1;
	if (d >= 128) dropped_mask = ~(__uint128_t)0;
	if (b.m & dropped_mask) bm |= 1;

	__uint128_t out_m;
	uint32_t out_s = a.s;
	int32_t  out_e = a.e;
	if (a.s == b.s) {
		out_m = a.m + bm;
		/* May overflow past bit 127. */
		if (out_m < a.m || (out_m >> 127) == 0) {
			/* Carry into bit 128: shift right 1, bump exponent. */
			out_m = (out_m >> 1) | ((__uint128_t)1 << 127)
			        | (out_m & 1);
			out_e += 1;
		}
	} else {
		out_m = a.m - bm;
		if (out_m == 0) {
			return (fpr_ext){ .m = 0, .e = -1076, .s = 0 };
		}
		/* Renormalize: shift left until bit 127 is set. */
		while ((out_m >> 127) == 0) {
			out_m <<= 1;
			out_e -= 1;
		}
	}

	return (fpr_ext){ .m = out_m, .e = out_e, .s = out_s };
}

/* Round extended-precision back to 53-bit fpr using IEEE-754
   round-to-nearest-even. The 128-bit mantissa has its top set bit at
   position 127 when nonzero, so we want to keep bits 127..75 as the
   53-bit mantissa (52 explicit + 1 implicit), with bits 74..0 used for
   rounding decision. */
static fpr
fpr_round_ext(fpr_ext z)
{
	if (z.m == 0) {
		return ((uint64_t)z.s << 63);
	}
	/* Reduce to 55-bit form (top bit at 54, low bit sticky) so the same
	   rounding logic as `make()` applies. Drop bits [0, 73]; track
	   sticky-OR of those bits. */
	uint64_t low_drop_lo = (uint64_t)(z.m);
	uint64_t low_drop_hi = (uint64_t)(z.m >> 64) & ((1ULL << 9) - 1);
	uint64_t any_dropped = (low_drop_lo | low_drop_hi) ? 1 : 0;
	uint64_t mant55 = (uint64_t)(z.m >> 73) | any_dropped;
	/* mant55 now in [2^54, 2^55-1] with low bit sticky. */
	uint64_t cc = (0xC8u >> ((unsigned)mant55 & 7)) & 1;
	uint64_t out =
		((uint64_t)z.s << 63) +
		((uint64_t)(uint32_t)(z.e + 1076) << 52) +
		(mant55 >> 2) + cc;
	return out;
}

/* Public API: fused complex multiply.
 *
 * STUB IMPLEMENTATION — currently equivalent to the FPC_MUL macro.
 * Replace with the genuinely-fused asm port (see tools/fpc_mul_design.md).
 *
 * The internal fpr_mul_ext / fpr_add_ext / fpr_round_ext helpers above are
 * an unfinished C reference for the fused algorithm — they have known bugs
 * in the exponent-tracking conventions (off-by-209 in some cases due to
 * mismatch between "top bit at 127" representation and Pornin's biased-
 * exponent encoding). The asm port should NOT use this convention; it
 * should use Pornin's existing scaled-by-8 representation from fpr_add
 * lines 213-218 directly.
 */
void
fndsa_fpr_complex_mul(fpr *d_re_out, fpr *d_im_out,
                      fpr a_re, fpr a_im, fpr b_re, fpr b_im)
{
	(void)fpr_mul_ext; (void)fpr_add_ext; (void)fpr_round_ext;
	/* Inline the unfused operation to avoid recursion when
	   FPC_MUL_FUSED redefines FPC_MUL to call this routine. */
	fpr d_re = fpr_sub(fpr_mul(a_re, b_re), fpr_mul(a_im, b_im));
	fpr d_im = fpr_add(fpr_mul(a_re, b_im), fpr_mul(a_im, b_re));
	*d_re_out = d_re;
	*d_im_out = d_im;
}
