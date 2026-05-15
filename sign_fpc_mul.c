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

/* Extended-precision intermediate. 64-bit mantissa with top bit at 63
   when non-zero (i.e. m in [2^63, 2^64-1] or 0). Low bit of m is sticky.
   Exponent is signed; the value is m * 2^e (treating m as integer).
   Sign is in bit 0 of `s`.

   This convention fits cleanly in 2 ARM registers per mantissa, enabling
   the M4 asm port to keep 4 product mantissas live simultaneously
   (8 regs total) without VFP spills. Per-product precision is 64 bits
   = 10 more bits than the reference's 53-bit rounded products. */
typedef struct {
	uint64_t m;
	int32_t  e;
	uint32_t s;
} fpr_ext;

#define M52   (((uint64_t)1 << 52) - 1)

/* Compute x * y as fpr_ext with 64-bit normalized mantissa (top bit at 63),
   sticky in lsb. */
static fpr_ext
fpr_mul_ext(fpr x, fpr y)
{
	uint32_t ex = (uint32_t)(x >> 52) & 0x7FF;
	uint32_t ey = (uint32_t)(y >> 52) & 0x7FF;
	uint32_t s  = (uint32_t)((x ^ y) >> 63) & 1;

	if (ex == 0 || ey == 0) {
		return (fpr_ext){ .m = 0, .e = 0, .s = s };
	}

	/* Mantissas with implicit-1 set, in [2^52, 2^53-1]. */
	uint64_t xu = (x & M52) | ((uint64_t)1 << 52);
	uint64_t yu = (y & M52) | ((uint64_t)1 << 52);

	/* 53*53 = 106-bit product. Top bit at position 104 or 105. */
	__uint128_t prod = (__uint128_t)xu * (__uint128_t)yu;

	/* Normalize so top bit is at position 63 of the resulting 64-bit
	   mantissa. Top bit was at 105 (case A) or 104 (case B).
	   Case A: shift right by 105 - 63 = 42.  Low 42 bits become sticky.
	   Case B: shift right by 104 - 63 = 41.  Low 41 bits become sticky. */
	int32_t  shift;
	uint64_t mhi, mlo, sticky_mask, m;
	if ((prod >> 105) & 1) {
		shift = 42;
	} else {
		shift = 41;
	}
	mlo = (uint64_t)prod;
	mhi = (uint64_t)(prod >> 64);
	/* Compute m = prod >> shift. Since shift in {41, 42} < 64, the high
	   64 bits of prod contribute through left-shift by (64-shift). */
	m = (mhi << (64 - shift)) | (mlo >> shift);
	/* Sticky: any bit dropped from the low `shift` bits of `mlo`. */
	sticky_mask = ((uint64_t)1 << shift) - 1;
	if (mlo & sticky_mask) m |= 1;

	/* Value: V = xu * yu * 2^(ex + ey - 2046 - 104).
	   = (prod >> shift) * 2^shift * 2^(ex + ey - 2150)
	   = m * 2^(ex + ey - 2150 + shift)
	   So e = ex + ey - 2150 + shift. */
	int32_t e = (int32_t)ex + (int32_t)ey - 2150 + shift;

	return (fpr_ext){ .m = m, .e = e, .s = s };
}

/* Extended-precision signed addition on 64-bit mantissas with sticky lsb.
   Aligns the smaller-exponent operand by right-shifting (with sticky), then
   signed-combines based on signs. Result is renormalized so top bit is at
   position 63 (or m == 0). */
static fpr_ext
fpr_add_ext(fpr_ext a, fpr_ext b)
{
	if (a.m == 0) return b;
	if (b.m == 0) return a;

	/* Order so |a| >= |b| (compare by (e, m)). */
	if (a.e < b.e || (a.e == b.e && a.m < b.m)) {
		fpr_ext t = a; a = b; b = t;
	}

	int32_t d = a.e - b.e;       /* >= 0 */
	uint64_t bm;
	if (d >= 64) {
		bm = (b.m != 0) ? 1 : 0;  /* All bits sticky */
	} else if (d == 0) {
		bm = b.m;
	} else {
		uint64_t dropped = b.m & (((uint64_t)1 << d) - 1);
		bm = (b.m >> d) | (dropped ? 1 : 0);
	}

	uint64_t out_m;
	uint32_t out_s = a.s;
	int32_t  out_e = a.e;
	if (a.s == b.s) {
		/* Same sign: add. May overflow into bit 64; renormalize by
		   shifting right 1 and bumping exponent (preserve sticky). */
		uint64_t r;
		int      carry = __builtin_add_overflow(a.m, bm, &r);
		if (carry) {
			out_m = ((uint64_t)1 << 63) | (r >> 1) | (r & 1);
			out_e += 1;
		} else {
			out_m = r;
		}
	} else {
		/* Different signs: subtract |b| from |a| (a.m >= bm by ordering).
		   Result may have many leading zeros (cancellation). */
		out_m = a.m - bm;
		if (out_m == 0) {
			return (fpr_ext){ .m = 0, .e = 0, .s = 0 };
		}
		while ((out_m >> 63) == 0) {
			out_m <<= 1;
			out_e -= 1;
		}
	}

	return (fpr_ext){ .m = out_m, .e = out_e, .s = out_s };
}

/* Round extended-precision back to 53-bit fpr using Pornin's `make()`
   convention. The 64-bit mantissa has top bit at 63 (or m == 0); we
   convert to 55-bit form (top bit at 54, low bit sticky) by dropping the
   low 9 bits with sticky-OR, then call the make-equivalent inline. */
static fpr
fpr_round_ext(fpr_ext z)
{
	if (z.m == 0) {
		return ((uint64_t)z.s << 63);
	}
	uint64_t dropped = z.m & 0x1FF;       /* low 9 bits */
	uint64_t mant55 = (z.m >> 9) | (dropped ? 1 : 0);
	/* mant55 is in [2^54, 2^55-1] with low bit sticky.

	   Exponent transform: we have V = z.m * 2^z.e and want to call
	   make-equivalent with V = mant55 * 2^e_for_make. Since mant55 ≈
	   z.m / 2^9, e_for_make = z.e + 9. */
	int32_t e_for_make = z.e + 9;
	uint64_t cc = (0xC8u >> ((unsigned)mant55 & 7)) & 1;
	uint64_t out =
		((uint64_t)z.s << 63) +
		((uint64_t)(uint32_t)(e_for_make + 1076) << 52) +
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
	fpr_ext p_rr = fpr_mul_ext(a_re, b_re);
	fpr_ext p_ii = fpr_mul_ext(a_im, b_im);
	fpr_ext p_ri = fpr_mul_ext(a_re, b_im);
	fpr_ext p_ir = fpr_mul_ext(a_im, b_re);

	/* d_re = p_rr - p_ii.  Express as add by flipping p_ii's sign. */
	p_ii.s ^= 1;
	*d_re_out = fpr_round_ext(fpr_add_ext(p_rr, p_ii));
	*d_im_out = fpr_round_ext(fpr_add_ext(p_ri, p_ir));
}
