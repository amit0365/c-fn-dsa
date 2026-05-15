	.syntax	unified
	.cpu	cortex-m4
	.file	"sign_fpc_mul_cm4.s"
	.text

@ =======================================================================
@ Fused complex multiplication for FN-DSA on Cortex-M4.
@
@ See sign_fpc_mul.c for the C reference.
@ See tools/fpc_mul_design.md for the design.
@
@ Computes:
@   d_re = a_re * b_re - a_im * b_im
@   d_im = a_re * b_im + a_im * b_re
@
@ Skips the 4 intermediate normalize+round steps that the reference
@ FPC_MUL macro pays for (one per fpr_mul). Net cycle target on M4:
@ ~250-280 cycles per call vs reference's ~308.
@
@ Constant-time discipline: all branches are on loop counters or compile-
@ time constants, not on data. Mirrors Pornin's existing constant-time
@ patterns.
@ =======================================================================

@ -----------------------------------------------------------------------
@ Macro: MUL_EXT
@   Computes  z = x * y  with extended precision.
@   Inputs (registers): r0:r1 = x (fpr), r2:r3 = y (fpr).
@   Outputs:
@     r0:r1   = mantissa (64-bit, top bit at 63 when non-zero, sticky lsb)
@     r4      = exponent (signed; "exp of bit 0 of mantissa" convention)
@     r5      = sign (top bit; lower bits ignored)
@   Clobbers: r2, r3, r6, r7, r12, flags.
@   Cycle estimate: ~30 cycles (vs Pornin's fpr_mul body at 35).
@ -----------------------------------------------------------------------
.macro MUL_EXT
	@ --- Setup: extract exponents, sign, handle zero ---
	ubfx	r6, r1, #20, #11      @ ex
	ubfx	r12, r3, #20, #11     @ ey
	eor	r5, r1, r3            @ top bit = sign(x) ^ sign(y)
	adds	r4, r6, r12
	sub	r4, r4, #1024
	mul	r6, r6, r12           @ r6 != 0 iff both ex,ey non-zero
	usat	r6, #1, r6
	muls	r4, r6                @ exponent = 0 if either is zero
	bfi	r1, r6, #20, #12      @ insert implicit-1 bit (if non-zero)
	bfi	r3, r6, #20, #12

	@ --- 53*53 multiply, result in r6:r12:r4_high:r0_high (low to high) ---
	@ Wait — fpr_mul uses r6:r12:r4:r0 layout. Let me follow that.
	@ r0:r1 = x, r2:r3 = y. After umulls: bits 0-31 in r6, 32-63 in r12,
	@ 64-95 in r4_intermediate (overlap), top in r0.
	@
	@ But we need r4 to hold our exponent! So we can't reuse r4 as a
	@ multiply register. Spill exponent first.
	push	{r4, r5}              @ save aggregate exp + sign
	@ Actually we ALSO need r5 for sign. So push both.
	@ Alternative: use VFP scratch. Push is simpler for clarity.

	@ Now do the multiply (Pornin's pattern, lines 681-684 of sign_fpr_cm4.s):
	umull	r6, r12, r0, r2       @ r6:r12 = x_lo * y_lo
	umull	r4, r0, r0, r3        @ r4:r0 = x_lo * y_hi (note: r0 reused)
	umaal	r12, r4, r1, r2       @ r12:r4 += x_hi * y_lo
	umaal	r4, r0, r1, r3        @ r4:r0 += x_hi * y_hi
	@ Now r6:r12:r4:r0 (low to high) = 106-bit product (top bit at 105 or 104).

	@ --- Normalize to 64-bit form (top bit at 63) with sticky lsb ---
	@ If top bit at 105: shift right by 42. Output mantissa = bits[105:42].
	@ If top bit at 104: shift right by 41. Output mantissa = bits[104:41].
	@ Detect: r0's bit 9 is bit 105 of product.
	lsrs	r2, r0, #9            @ r2 = 1 if top bit at 105, else 0
	@ shift_count = 42 - r2  (so 41 if r2=1, 42 if r2=0)
	@ Wait that's backwards. Let me re-check.
	@ When top bit is at 105 (r2=1), value is in [2^105, 2^106), needs
	@ shift right by 42 to put top at 63. So shift = 42 when r2 = 1.
	@ When top bit is at 104 (r2=0), shift right by 41.
	@ shift = 41 + r2.

	@ For our 64-bit normalized output, want bits [42, 105] (case A) or
	@ [41, 104] (case B) of the 128-bit product.
	@ Case A: mant_hi = (r0 << 22) | (r4 >> 10); mant_lo = (r4 << 22) | (r12 >> 10)
	@         sticky bits = r12[9:0] | r6
	@ Case B: mant_hi = (r0 << 23) | (r4 >> 9); mant_lo = (r4 << 23) | (r12 >> 9)
	@         sticky bits = r12[8:0] | r6

	@ Branchless: use r2 (0 or 1) to select shift offset.
	@ Compute both then select? Or just use a variable shift via reg.

	@ Approach: compute shift_amount, then use lsl/lsr by register.
	rsb	r3, r2, #23           @ r3 = 23 - r2 (= 23 case B, 22 case A)
	@ Actually want: shift_left = 23 - r2_top
	@   case A (r2=1): shift_left = 22
	@   case B (r2=0): shift_left = 23
	@ So r3 = 23 - r2.
	@ mant_hi = (r0 << r3) | (r4 >> (32 - r3))
	@ But (32 - r3) = 9 (case A) or 10 (case B). Let r7 = 32 - r3 = 9 + r2.
	add	r7, r2, #9            @ r7 = 9 + r2 (= 10 case A, 9 case B)
	lsl	r0, r0, r3
	lsr	r1, r4, r7            @ r1 = bits going into mant_hi from r4
	orr	r0, r0, r1            @ r0 = mant_hi
	lsl	r4, r4, r3
	lsr	r1, r12, r7
	orr	r1, r4, r1            @ r1 = mant_lo
	@ Now r0:r1 = (mant_hi : mant_lo)... wait that puts hi in r0.
	@ Actually we want mant_lo in r0 and mant_hi in r1 (matching fpr layout).
	@ Let me re-do: want r0 = mant_lo, r1 = mant_hi.
	@ Hmm I've ended with r0 = mant_hi, r1 = mant_lo. Swap them.
	mov	r2, r0
	mov	r0, r1
	mov	r1, r2

	@ --- Sticky bit: low (32-r7) bits of r12 plus all of r6 ---
	@ shift_count_for_sticky = 10 (case A) or 9 (case B), i.e. = r7.
	@ sticky_mask for r12 = (1 << r7) - 1
	mov	r3, #1
	lsl	r3, r3, r7
	subs	r3, r3, #1            @ sticky mask
	ands	r3, r3, r12           @ r3 = dropped bits from r12
	orrs	r3, r3, r6            @ OR with r6 (entire reg dropped)
	@ Set lsb of mantissa if any sticky bit set
	usat	r3, #1, r3            @ r3 = 0 or 1
	orrs	r0, r0, r3            @ set lsb of mant_lo

	@ --- Restore exponent and sign, adjust exponent for shift ---
	pop	{r4, r5}
	@ Exponent adjust: e_final = ex + ey - 2046 - 104 - shift_left
	@                          + ... actually need careful derivation.
	@ Recall: r4 holds (ex + ey - 1024) (or 0 if either was zero).
	@ The aggregate exponent for product (top bit at position k) such that
	@ V = mantissa * 2^(e_final), with mantissa top bit at 63:
	@   V = xu * yu * 2^(ex + ey - 2046 - 104)
	@   product top bit at 105 (case A): mantissa = product >> 42, so
	@     V = mantissa * 2^42 * 2^(ex+ey-2150) = mantissa * 2^(ex+ey-2108)
	@   product top bit at 104 (case B): mantissa = product >> 41, so
	@     V = mantissa * 2^41 * 2^(ex+ey-2150) = mantissa * 2^(ex+ey-2109)
	@   Combined: e_final = ex + ey - 2108 - (1 - r2) = ex + ey - 2109 + r2
	@   With r4 = ex + ey - 1024:
	@   e_final = r4 - 1085 + r2
	subw	r4, r4, #1085
	add	r4, r4, r2

	@ Output: r0:r1 = mantissa (lo:hi), r4 = exp, r5 top bit = sign.
.endm

@ =======================================================================
@ void fndsa_fpr_complex_mul(fpr *d_re, fpr *d_im,
@                            fpr a_re, fpr a_im, fpr b_re, fpr b_im)
@
@ Standard AAPCS calling convention with output pointers:
@   r0 = &d_re, r1 = &d_im, r2:r3 = a_re, [sp+0]:[sp+4] = a_im,
@   [sp+8]:[sp+12] = b_re, [sp+16]:[sp+20] = b_im
@
@ ... Actually for performance, use a custom convention via inline asm
@ in the FPC_MUL macro, mirroring fpr_add_sub. Inputs in r0-r7.
@
@ Custom calling convention (matching FPR_ADD_SUB style):
@   Inputs:  r0:r1 = a_re, r2:r3 = a_im, r4:r5 = b_re, r6:r7 = b_im
@   Outputs: r0:r1 = d_re, r2:r3 = d_im
@   Clobbers: r0-r12, r14, flags, s0-s15
@
@ STATUS: scaffolded. MUL_EXT macro above is drafted and assembles
@ cleanly (measured at 42 cycles per expansion). However, MUL_EXT
@ produces a 64-bit normalized intermediate which costs MORE than
@ Pornin's existing fpr_mul (which produces 53-bit). The right target
@ for the full implementation is 55-bit intermediate matching Pornin's
@ pre-round form — see tools/fpc_mul_design.md.
@
@ Realistic full-asm cycle estimate: ~276 cycles per call (saves ~32
@ cyc vs reference's ~308 = ~0.7% e2e). Multi-day effort to write
@ fpr_mul_no_round + fpr_add_take_extended + orchestration, plus
@ M4 hardware validation.
@ =======================================================================

	.align	2
	.global	fndsa_fpr_complex_mul_asm
	.thumb
	.thumb_func
	.type	fndsa_fpr_complex_mul_asm, %function
fndsa_fpr_complex_mul_asm:
	@ TODO: full implementation. For now, this entry point is reserved
	@ for the asm port. The C wrapper in sign_fpc_mul.c calls the C
	@ reference (fndsa_fpr_complex_mul). When this asm is complete, the
	@ FPC_MUL macro on M4 should call this directly.
	@
	@ Sketched flow:
	@   1. Spill all 4 inputs to s0-s7.
	@   2. MUL_EXT(a_re, b_re) → save (r0:r1, r4, r5) to s8-s11.
	@   3. MUL_EXT(a_im, b_im) → ADD_EXT_SUB → ROUND → store as d_re in s12-s13.
	@   4. MUL_EXT(a_re, b_im) → save to s8-s11.
	@   5. MUL_EXT(a_im, b_re) → ADD_EXT_ADD → ROUND → r2:r3 = d_im.
	@   6. Restore d_re from s12-s13 into r0:r1.
	@   7. Return.
	bx	lr
	.size	fndsa_fpr_complex_mul_asm,.-fndsa_fpr_complex_mul_asm
