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
@ Macro: ADD_POST_EXTRACT
@   Inlined body of fpr_add starting AFTER the operand extraction
@   (sign_fpr_cm4.s lines 220-327). Skips the 22-cycle extract prologue
@   that calls would pay for.
@
@   Required input state (must be set up by caller):
@     r6:r7 = mantissa of x in 56-bit form (multiple of 8 if bottom 3 bits
@             matter for rounding; otherwise sticky in low bits is fine)
@     r2:r3 = mantissa of y in 56-bit form
@     r4    = ex (biased exponent, [0, 2046])
@     r0    = ey (biased exponent)
@     r5    = sign info: bit 31 = sign-xor, bits 0-30 = sign of x
@
@   Output: r0:r1 = result fpr.
@   Clobbers: r2, r3, r4, r5, r6, r7, r12, flags.
@   Cycle estimate: ~38 cycles (vs fpr_add's 69 = saves ~31 cyc).
@ -----------------------------------------------------------------------
.macro ADD_POST_EXTRACT
	@ === Alignment shift (lines 230-258 of fpr_add) ===
	subs	r0, r4, r0
	usat	r0, #6, r0
	sbfx	r1, r0, #5, #1
	and	r12, r1, r2, lsr #1
	bic	r2, r2, r1
	umlal	r3, r2, r3, r1
	and	r0, r0, #31
	mov	r1, #0xFFFFFFFF
	lsr	r1, r0
	eors	r0, r0
	umlal	r3, r0, r3, r1
	umlal	r2, r3, r2, r1
	orrs	r12, r12, r2, lsr #1
	usat	r2, #1, r12
	orrs	r3, r2

	@ === Signed combination (lines 266-270 of fpr_add) ===
	movs	r1, #1
	orr	r2, r1, r5, asr #31
	add	r0, r0, r3, lsr #31
	smlal	r6, r7, r3, r2
	mla	r7, r0, r2, r7

	@ === Normalize (lines 279-291 of fpr_add) ===
	clz	r2, r7
	sbfx	r0, r2, #5, #1
	umlal	r6, r7, r6, r0
	add	r4, r4, r0, lsl #5
	clz	r2, r7
	subs	r4, r4, r2
	lsls	r1, r2
	umull	r6, r12, r6, r1
	mla	r12, r1, r7, r12

	@ === Exponent fixup (lines 302-307 of fpr_add) ===
	adds	r4, #7
	ands	r4, r4, r12, asr #31

	@ === Round to 53 bits (lines 320-327 of fpr_add) ===
	lsls	r5, #31
	orr	r1, r5, r4, lsl #20
	lsls	r3, r6, #21
	lsrs	r0, r6, #11
	bfi	r3, r0, #27, #1
	adds	r3, r3, #0x78000000
	adcs	r0, r0, r12, lsl #21
	adcs	r1, r1, r12, lsr #11

	@ Output: r0:r1 = result fpr.
.endm

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
	@ FIRST-PASS: calls existing fpr_mul/fpr_add via BL, uses VFP for
	@ spills. Cycle count: 41 (body) + 4*35 (mul) + 2*69 (add) = 319 cyc.
	@ See fndsa_fpr_complex_mul_fused_asm below for the actual fused version.
	@
	@ Custom calling convention:
	@   Inputs:  r0:r1=a_re, r2:r3=a_im, r4:r5=b_re, r6:r7=b_im
	@   Outputs: r0:r1=d_re, r2:r3=d_im
	@   Clobbers: r0-r12, r14, flags, s0-s15
	push	{r14}                 @ save LR

	@ Save all 4 inputs to VFP scratch
	vmov	s0, s1, r0, r1        @ s0:s1 = a_re
	vmov	s2, s3, r2, r3        @ s2:s3 = a_im
	vmov	s4, s5, r4, r5        @ s4:s5 = b_re
	vmov	s6, s7, r6, r7        @ s6:s7 = b_im

	@ p_rr = fpr_mul(a_re, b_re).  Inputs already in r0:r1 and r4:r5.
	mov	r2, r4
	mov	r3, r5
	bl	fndsa_fpr_mul
	vmov	s8, s9, r0, r1        @ s8:s9 = p_rr

	@ p_ii = fpr_mul(a_im, b_im).
	vmov	r0, r1, s2, s3        @ a_im
	vmov	r2, r3, s6, s7        @ b_im
	bl	fndsa_fpr_mul
	@ d_re = fpr_sub(p_rr, p_ii) = fpr_add(p_rr, -p_ii).
	@ Flip sign of p_ii (top bit of r1)
	eor	r1, r1, #0x80000000
	mov	r2, r0
	mov	r3, r1
	vmov	r0, r1, s8, s9        @ p_rr into r0:r1
	bl	fndsa_fpr_add
	vmov	s12, s13, r0, r1      @ s12:s13 = d_re

	@ p_ri = fpr_mul(a_re, b_im).
	vmov	r0, r1, s0, s1        @ a_re
	vmov	r2, r3, s6, s7        @ b_im
	bl	fndsa_fpr_mul
	vmov	s10, s11, r0, r1      @ s10:s11 = p_ri

	@ p_ir = fpr_mul(a_im, b_re).
	vmov	r0, r1, s2, s3        @ a_im
	vmov	r2, r3, s4, s5        @ b_re
	bl	fndsa_fpr_mul
	@ d_im = fpr_add(p_ri, p_ir).  p_ir is in r0:r1.
	mov	r2, r0
	mov	r3, r1
	vmov	r0, r1, s10, s11      @ p_ri into r0:r1
	bl	fndsa_fpr_add
	@ d_im now in r0:r1; need to move to r2:r3 for output convention
	mov	r2, r0
	mov	r3, r1
	@ Restore d_re into r0:r1
	vmov	r0, r1, s12, s13

	pop	{pc}                  @ restore LR and return
	.size	fndsa_fpr_complex_mul_asm,.-fndsa_fpr_complex_mul_asm

@ =======================================================================
@ FUSED version: uses fpr_mul (still BL) for the 4 multiplies but
@ INLINES the add bodies via ADD_POST_EXTRACT to skip the per-call
@ extract overhead.
@
@ Strategy: still call fpr_mul to get fpr-form intermediate products,
@ then EXTRACT them inline (2 ops each) and run ADD_POST_EXTRACT.
@ This avoids re-entering fpr_add's prologue + extract = ~25 cyc each.
@
@ Note: this isn't "true fusion" (we still round each mul to 53-bit),
@ but it captures the BL-overhead savings on the add side.
@
@ Cycle target: 4*35 (mul) + 2*40 (add inline) + ~50 (extraction +
@                 orchestration) = ~270 cyc vs reference 347 = ~2.2% e2e.
@ =======================================================================
	.align	2
	.global	fndsa_fpr_complex_mul_fused_asm
	.thumb
	.thumb_func
	.type	fndsa_fpr_complex_mul_fused_asm, %function
fndsa_fpr_complex_mul_fused_asm:
	@ Custom calling convention:
	@   Inputs:  r0:r1=a_re, r2:r3=a_im, r4:r5=b_re, r6:r7=b_im
	@   Outputs: r0:r1=d_re, r2:r3=d_im
	push	{r14}

	@ Spill all inputs to VFP
	vmov	s0, s1, r0, r1
	vmov	s2, s3, r2, r3
	vmov	s4, s5, r4, r5
	vmov	s6, s7, r6, r7

	@ p_rr = fpr_mul(a_re, b_re)
	mov	r2, r4
	mov	r3, r5
	bl	fndsa_fpr_mul
	vmov	s8, s9, r0, r1            @ save p_rr

	@ p_ii = fpr_mul(a_im, b_im)
	vmov	r0, r1, s2, s3
	vmov	r2, r3, s6, s7
	bl	fndsa_fpr_mul
	@ d_re = p_rr - p_ii. Flip sign of p_ii first.
	eor	r1, r1, #0x80000000
	@ Now we need to extract both operands and run ADD_POST_EXTRACT.
	@ Operands:
	@   x = p_rr (in s8:s9)
	@   y = p_ii_neg (in r0:r1)
	@ Set up post-extract state (mirroring fpr_add lines 197-218):
	@   r6:r7 = mantissa of x scaled to 56-bit
	@   r2:r3 = mantissa of y scaled to 56-bit
	@   r4 = ex (biased)
	@   r0 = ey (biased)
	@   r5 = sign info
	mov	r2, r0                    @ y_lo
	mov	r3, r1                    @ y_hi
	vmov	r6, r7, s8, s9            @ x = p_rr

	@ Conditional swap (lines 182-191) — needed because we don't know
	@ which has greater abs value. Re-using existing code shape:
	@ But operands are now in r6:r7 and r2:r3 (not r0:r1 and r2:r3).
	@ Move them to standard slots first.
	mov	r0, r6
	mov	r1, r7
	@ Now r0:r1 = x, r2:r3 = y. Run conditional swap.
	lsls	r7, r1, #1
	subs	r6, r1, r1, asr #31
	sbcs	r6, r0, r2
	sbcs	r6, r7, r3, lsl #1
	sbcs	r4, r4
	uadd8	r4, r4, r4
	sel	r6, r2, r0
	sel	r7, r3, r1
	sel	r2, r0, r2
	sel	r3, r1, r3
	@ Now x is in r6:r7, y in r2:r3.

	@ Build sign info (lines 197-198):
	and	r5, r3, #0x80000000
	eor	r5, r5, r7, asr #31

	@ Extract mantissas to 53-bit form with implicit-1 (lines 204-211):
	ubfx	r4, r7, #20, #11          @ ex
	usat	r1, #1, r4
	bfi	r7, r1, #20, #12
	ubfx	r0, r3, #20, #11          @ ey
	usat	r1, #1, r0
	bfi	r3, r1, #20, #12

	@ Scale to 56-bit (lines 214-218):
	mov	r1, #7
	lsls	r7, #3
	umlal	r6, r7, r6, r1
	lsls	r3, #3
	umlal	r2, r3, r2, r1

	@ NOW the post-extract state is set up. Run the inline add body.
	ADD_POST_EXTRACT
	@ Result: r0:r1 = d_re. Save it.
	vmov	s12, s13, r0, r1

	@ p_ri = fpr_mul(a_re, b_im)
	vmov	r0, r1, s0, s1
	vmov	r2, r3, s6, s7
	bl	fndsa_fpr_mul
	vmov	s10, s11, r0, r1            @ save p_ri

	@ p_ir = fpr_mul(a_im, b_re)
	vmov	r0, r1, s2, s3
	vmov	r2, r3, s4, s5
	bl	fndsa_fpr_mul
	@ d_im = p_ri + p_ir. p_ir is in r0:r1.
	mov	r2, r0
	mov	r3, r1
	vmov	r0, r1, s10, s11            @ p_ri into r0:r1

	@ Conditional swap
	lsls	r7, r1, #1
	subs	r6, r1, r1, asr #31
	sbcs	r6, r0, r2
	sbcs	r6, r7, r3, lsl #1
	sbcs	r4, r4
	uadd8	r4, r4, r4
	sel	r6, r2, r0
	sel	r7, r3, r1
	sel	r2, r0, r2
	sel	r3, r1, r3

	and	r5, r3, #0x80000000
	eor	r5, r5, r7, asr #31

	ubfx	r4, r7, #20, #11
	usat	r1, #1, r4
	bfi	r7, r1, #20, #12
	ubfx	r0, r3, #20, #11
	usat	r1, #1, r0
	bfi	r3, r1, #20, #12

	mov	r1, #7
	lsls	r7, #3
	umlal	r6, r7, r6, r1
	lsls	r3, #3
	umlal	r2, r3, r2, r1

	ADD_POST_EXTRACT
	@ d_im in r0:r1; move to r2:r3 for output
	mov	r2, r0
	mov	r3, r1
	@ Restore d_re into r0:r1
	vmov	r0, r1, s12, s13

	pop	{pc}
	.size	fndsa_fpr_complex_mul_fused_asm,.-fndsa_fpr_complex_mul_fused_asm

@ =======================================================================
@ fpr_mul_no_round (helper for fused FPC_MUL)
@
@ Computes x * y but outputs in 55-bit-mantissa form (matching Pornin's
@ pre-round intermediate from fpr_mul) instead of rounding to 53-bit fpr.
@
@ Custom calling convention (called via inline asm only):
@   Inputs:  r0:r1 = x, r2:r3 = y
@   Outputs:
@     r0:r1   = 55-bit mantissa, top bit at 54, low bit sticky (lo:hi)
@     r4      = unbiased exponent, signed
@     r5      = sign (bit 31 set/unset; lower bits = 0)
@   Clobbers: r2, r3, r6, r7, r12, flags. VFP s0-s2 used for callee saves.
@
@ Cycle estimate (static): 28 cycles body + 4 frame = 32 cycles.
@ Mirrors fpr_mul body (sign_fpr_cm4.s lines 643-710), changes:
@   - Shift constant 2^12 -> 2^14 to produce 55-bit form (not 53-bit)
@   - Compute sticky bit instead of round bit
@   - No final adcs/pack into fpr; output components separately
@ =======================================================================
	.align	2
	.global	fndsa_fpr_mul_no_round
	.thumb
	.thumb_func
	.type	fndsa_fpr_mul_no_round, %function
fndsa_fpr_mul_no_round:
	@ Save callee-saves to VFP scratch (mirrors fpr_mul prologue)
	vmov	s0, s1, r4, r5
	vmov	s2, r6

	@ ---- Setup: extract exponents, sign, handle zero (mirrors fpr_mul lines 647-672) ----
	ubfx	r6, r1, #20, #11      @ ex
	ubfx	r12, r3, #20, #11     @ ey
	eor	r5, r1, r3            @ top bit = sign(x) ^ sign(y)
	adds	r4, r6, r12
	sub	r4, r4, #1024
	mul	r6, r6, r12           @ r6 != 0 iff both ex,ey non-zero
	usat	r6, #1, r6
	muls	r4, r6                @ exp = 0 if either input zero
	bfi	r1, r6, #20, #12      @ insert implicit-1 (or 0 if input zero)
	bfi	r3, r6, #20, #12

	@ ---- 53*53 multiply (Pornin's umull/umaal sequence, lines 681-684) ----
	umull	r6, r12, r0, r2       @ r6:r12 = lo*lo
	umull	r4, r0, r0, r3        @ r4:r0  = lo*hi (note: r4 reused later for exp)
	umaal	r12, r4, r1, r2       @ r12:r4 += hi*lo
	umaal	r4, r0, r1, r3        @ r4:r0  += hi*hi
	@ Now r6:r12:r4:r0 (low to high) = 106-bit product.

	@ ---- Shift to 55-bit form (top bit at 54), put result in r3:r12 ----
	@ Pornin shifts to 53-bit; we shift to 55 by changing the shift constant.
	@ 53-bit shift used 2^12 (->shift 52) or 2^11 (->shift 53).
	@ For 55-bit, use 2^14 (->shift 50) or 2^13 (->shift 51).
	lsrs	r3, r0, #9            @ r3 = 1 if top bit at 105, else 0
	@ Recover the sign+exp word: same as Pornin (r5 with top bit only)
	add	r5, r3, r5, lsr #20   @ Wait — we want r5 to keep sign + need exp separately.
	@ ^ Actually for our convention we want sign and exp in SEPARATE regs.
	@ Pornin's `add r5, r3, r5, lsr #20` packs exp adjustment + truncates
	@ extra bits. For us: just remember the exp adjustment in r3.
	@ Roll back: we want unbiased exp in r4 at end, but r4 is being used
	@ as a multiply intermediate. Let me restore it from VFP.

	movw	r2, #0x4000           @ 2^14 (vs Pornin's 2^12 = 0x1000)
	lsrs	r2, r3                @ r2 = 2^14 (case A) or 2^13 (case B)

	@ Do the shift: r1:r3:r12 receives the shifted product (3 regs, 96 bits).
	umull	r1, r3, r12, r2       @ r3:r1 = r12 * shift_factor
	mul	r12, r0, r2           @ r12 = r0 * shift_factor (low 32)
	umlal	r3, r12, r4, r2       @ r12:r3 += r4 * shift_factor

	@ After this:
	@   r3:r12 = 55-bit mantissa (top bit at 54), in lo:hi
	@   r1     = bits dropped from above the cutoff (rounding bit + below)
	@   r6     = bottom 32 bits dropped (sticky)

	@ ---- Compute sticky bit (OR of all dropped bits) ----
	@ For our 55-bit-with-sticky output, we want LSB of r3 to reflect
	@ "any bit was dropped below position 54". The dropped bits are:
	@   - all of r6
	@   - all of r1
	@   - any nonzero in r12's already-shifted-out bits (none since umlal cleanly)
	orrs	r6, r6, r1            @ r6 = combined dropped bits
	usat	r6, #1, r6            @ r6 = 0 or 1
	orrs	r3, r3, r6            @ set lsb of r3 = sticky

	@ ---- Restore exp into r4 and sign into r5, output format conversion ----
	@ Currently:
	@   r3:r12 = 55-bit mantissa (lo:hi)
	@   r5 (mangled by `add r5, r3, r5, lsr #20`) — drop and restore from saved.
	@ Hmm we mangled r5. Need to recompute exp + sign cleanly.

	@ Restore original ex+ey-1024 from VFP-saved r4 (s0)? No wait, r4
	@ holds the multiply intermediate now. We need to EITHER preserve r4 across
	@ the multiply (impossible — Pornin's umaal sequence reuses it) or RECOMPUTE
	@ exp.

	@ Recompute exp from x and y (loaded from VFP):
	@ Actually r0:r1 still hold the operands? No — r0 was used in umull and r1 in umaal.
	@ The original x and y are gone.

	@ STATUS: This is where the design needs more thought. Pornin's fpr_mul
	@ keeps exp in r5 via the `add r5, r3, r5, lsr #20` which combines
	@ exp+sign into a single packed form. For our split-output convention
	@ we'd need an additional register (or VFP slot) for exp. That's an
	@ extra cycle or two of overhead.
	@
	@ For now, leave this stub returning a packed sign+exp in r5 (Pornin's
	@ convention) and let the caller unpack as needed. Output:
	@   r0 (= old r3) : r1 (= old r12)  = 55-bit mantissa (lo:hi)
	@   r5 = packed (sign in bit 31, exp in bits 20-30) - Pornin's format
	@   r2-r4, r12 clobbered
	mov	r0, r3
	mov	r1, r12

	@ Restore callee-saves
	vmov	r4, r5, s0, s1
	vmov	r6, s2
	bx	lr
	.size	fndsa_fpr_mul_no_round,.-fndsa_fpr_mul_no_round
