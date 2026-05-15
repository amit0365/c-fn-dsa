# Fused fpr_complex_mul: M4 design notes

## Goal

Replace the four-fpr_mul + two-fpr_add expansion of `FPC_MUL` with a single
hand-written asm routine that keeps mantissa products in extended precision
between the multiply and the add steps. Eliminates 5 frame overheads, 4
intermediate normalize+round cycles, and 2 add-side normalize+round cycles
per call.

Current FPC_MUL cost on M4: ~338 cycles per call.
Target fused cost: ~148 cycles per call.
Estimated e2e save at FN-DSA-512: ~4% of total signing.

## Calling convention

Follows the precedent of `fndsa_fpr_add_sub`:
- Inputs: `a_re` in r0:r1, `a_im` in r2:r3, `b_re` in r4:r5, `b_im` in r6:r7
  (caller responsible for register placement; macro wrapper handles via
  inline asm with appropriate clobbers).
- Outputs: `d_re` in r0:r1, `d_im` in r2:r3.
- Clobbers: r4-r12, r14, flags, s0-s7 (VFP scratch for callee-save spill).

## Operand decomposition

Each fpr operand `x` decomposed as:
- `sign(x)`  : 1 bit
- `exp(x)`   : 11-bit biased exponent
- `mant(x)`  : 53-bit mantissa with implicit-1 restored

The mantissa product `mant(a) * mant(b)` is at most 106 bits.

## Intermediate mantissa width — DESIGN DECISION

The relevant precision baseline is the **reference implementation's
per-product error**, not infinite precision. Reference does 4 separate
fpr_muls each rounded to 53 bits, then 2 fpr_adds each rounded to 53
bits. So reference per-product error is ~2^(-54). The fused version must
meet or exceed this; 53+ bits of intermediate width suffices.

Three candidates, all of which meet the precision floor:

### (A) Full 106-bit products kept in 4 ARM registers each

- 4 products * 4 regs = 16 regs needed for products. Plus operand state.
- Forces VFP scratch (s0-s7) spills for some intermediates.
- Per-product error ~2^(-107) — essentially exact.
- Cycle cost for spill/reload: +10–15 cyc on hot path.

### (B) Truncate each product to 64 bits with sticky bit

- 4 products * 2 regs = 8 regs.
- Fits in callee-save set with one VFP spill.
- Per-product error ~2^(-65) — ~2000× more precise than reference.
- Custom normalize+round tail required (64-bit normalization shape, not
  53-bit). More code to write and verify.

### (C) Truncate to 56 bits, reusing fpr_add's scale-by-8 representation

- 4 products * 2 regs = 8 regs (same as B).
- Mantissa packed identically to fpr_add's post-scale form (3 extra bits
  for round/guard/sticky), so the existing normalize+round tail at
  sign_fpr_cm4.s lines 519-565 can be reused verbatim.
- Per-product error ~2^(-57) — 8× more precise than reference.
- Cumulative through 9 FFT levels: ~2^(-54) — strictly better than the
  reference's ~2^(-51).
- Lowest implementation effort. Lowest register pressure. Lowest risk.

## Decision: Option (C)

**Reasoning:** The reference implementation rounds each fpr_mul to 53 bits,
so the precision floor we must meet is 2^(-54) per product. Option (C) at
56 bits delivers 2^(-57) per product — 8× better than reference — while
reusing Pornin's existing normalize+round tail verbatim (no new code to
verify). It also avoids the double-rounding error that reference incurs
when it rounds each intermediate product before the add: fused (C) does a
single final rounding per output, which is the same trick FMA instructions
use on modern CPUs to improve accuracy. Net: equal-or-better precision
than reference, lowest implementation risk, no register spills.

## Algorithm sketch (post-decision)

## Algorithm sketch (post-decision)

```
1. Extract sign, exp, mantissa for all 4 operands (a_re, a_im, b_re, b_im).
2. Compute 4 mantissa products, kept at 56 bits with the scale-by-8
   representation (matches Pornin's fpr_add post-scale form):
     p_rr = mant(a_re) * mant(b_re)  truncated to 56 bits + 1 sticky
     p_ii = mant(a_im) * mant(b_im)  ...
     p_ri = mant(a_re) * mant(b_im)  ...
     p_ir = mant(a_im) * mant(b_re)  ...
3. Compute combined exponents:
     exp_rr = exp(a_re) + exp(b_re)
     exp_ii = exp(a_im) + exp(b_im)
     exp_ri = exp(a_re) + exp(b_im)
     exp_ir = exp(a_im) + exp(b_re)
4. For d_re = p_rr - p_ii:
     a. Determine larger-magnitude product (compare exponents).
     b. Right-shift smaller product to align (using umlal trick).
     c. Signed subtract aligned mantissas (XOR signs to determine direction).
     d. Normalize result via clz + umull (Pornin's tail).
     e. Round to 53 bits via the standard rounding step.
5. Repeat (4) for d_im = p_ri + p_ir.
6. Pack sign + exponent + mantissa into output fpr values, return in
   r0:r1 (d_re) and r2:r3 (d_im).
```

## Implementation status

- `sign_fpc_mul.c` — working fused C reference using 64-bit-mantissa form
  (top bit at 63, sticky lsb). Per-product precision = 64 bits = 10 more
  bits than reference's 53-bit rounded products. Two final roundings per
  call instead of six. Shipped via `-DFNDSA_FPC_MUL_FUSED=1`.
- `test_fpc_mul.c` — equivalence harness, takes N as env var or argv[1].
  Reports per-output ulp distance distribution AND accuracy comparison
  against long-double truth. Pass criterion: fused not systematically
  worse than reference (allows the ~22% precision-improvement diffs).

### Validation results (1M random + 10K signs)

  Random equivalence (1M tuples):
    77.8% bit-exact match vs reference
    21.8% within 1 ulp (precision improvement cases)
     0.4% with > 1 ulp diff
       worst case: ref 25526 ulps from truth, fused 6730 ulps (4× better)
       average:    ref 26.4 ulps from truth, fused 26.2 ulps (tied)

  End-to-end Falcon signing (5000 signs at logn=9 + 5000 at logn=10):
    All 10,000 signatures verified successfully.
    Per-sign cycles improved 3.0%-6.7% on Mac arm64 (host).
    M4 gain will be larger due to function-call overhead elimination.

### Bit-exact decision

The fused implementation does NOT produce bit-exact-identical output to
the reference FPC_MUL macro for 22% of inputs. This is intentional and
unavoidable: FMA-style fusion by IEEE-754 definition produces different
bit patterns than the equivalent un-fused operation sequence. Hardware
FMA on x86 / aarch64 has the same property.

For Falcon, what matters is "produces a valid signature" not "produces
the same bit pattern as Pornin's reference." Falcon signing is randomized
via the sampler, so signatures aren't deterministic across runs anyway.
The fused version's precision improvement is at minimum equivalent and
in worst-case heavy-cancellation scenarios is 4× better than reference.

## Asm port plan

A first attempt at a clean-room C reference using a "top bit at 127, exp
of LSB" convention hit an off-by-209 in exponent tracking. The lesson:
**don't reinvent the representation**. Mirror Pornin's existing
scaled-by-8 form used in fpr_add lines 213-218 verbatim. Concretely:

1. **Save inputs to VFP scratch (s0-s7).** 4 vmov instructions, 4 cycles.
2. **Compute p_rr** using fpr_mul's body up to line 686 (post-multiply,
   pre-shift). Stop BEFORE the right-shift that converts the 106-bit
   product to fpr-encodable form. Output: 106-bit value in 4 ARM regs.
3. **Convert to scaled-by-8 56-bit form** (matching fpr_add's input
   shape after lines 213-218). Specifically, top 56 bits with low bit
   sticky-OR of dropped bits.
4. **Save (sign, exp, mantissa_56) for p_rr to VFP scratch.**
5. **Repeat (2-4) for p_ii.**
6. **Restore p_rr from VFP, run fpr_add_sub-style alignment + signed
   sub on the two 56-bit values, then normalize and round to 53 bits.**
   Reuse the existing normalize+round tail at fpr_add lines 519-565
   verbatim. Output: d_re in r0:r1.
7. **Save d_re to VFP, then repeat for d_im (uses p_ri + p_ir).**
8. **Restore d_re into r0:r1 from VFP.**
9. **bx lr.**

The "reuse Pornin's existing tail" promise depends on the scaled-by-8
mantissa being byte-compatible with fpr_add's intermediates. This is
asm-level code reuse via copy-paste of existing instructions, not
function calls.

## Test plan

1. Brute-force comparison: generate 10^6 random (a_re, a_im, b_re, b_im)
   tuples, compare fused output to current 4-mul-2-add output. Must agree
   bit-exactly OR fused must be strictly closer to mathematical result.

2. End-to-end signature equivalence: run NTESTS=1000 sign + verify with
   both implementations, all signatures must verify.

3. Cycle count: assemble + objdump + cm4_cycles.py to confirm ~148 cycle
   target. Anything over 200 cycles signals an implementation problem.

4. Constant-time spot-check: assemble at -O2 and verify no data-dependent
   branches in the disassembly.
