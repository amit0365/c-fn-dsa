Subject: Reduce signing tmp[] from 43n+31 to 37n+31 bytes via opt-in `FNDSA_FFSAMP_5N_REDUCED` build flag (Path A)

Summary
=======

This patch adds an opt-in compile-time flag (`FNDSA_FFSAMP_5N_REDUCED`,
default 0; requires `FNDSA_PHASE1_REDUCED` and `FNDSA_PATH_B`) that further
reduces the signing temporary buffer from `43*n + 31` to `37*n + 31` bytes —
saves 6 KiB at logn=10 (FN-DSA-1024), 3 KiB at logn=9 (FN-DSA-512). The
function-internal ffsamp peak drops from 5n FLR to 4n FLR at the outer
recursion level via a new "Path A" body that drops `l10` from its persistent
set across the right recursion and recomputes it from the precomputed
`external_basis` post-recursion.

Empirically signs **slightly faster than baseline**, not slower:

  Host NEON (10-run median, logn=9):
    baseline                        132.4 µs       —
    + FNDSA_PATH_B                  131.5 µs    -0.69%
    + FNDSA_PHASE1_REDUCED          129.9 µs    -1.94%
    + FNDSA_FFSAMP_5N_REDUCED       130.0 µs    -1.86%

  Scalar (5-run median, logn=9):
    baseline                       1333.3 µs       —
    + FNDSA_PATH_B                 1376.4 µs    +3.23%
    + FNDSA_PHASE1_REDUCED         1181.0 µs   -11.43%
    + FNDSA_FFSAMP_5N_REDUCED      1221.4 µs    -8.39%

The PHASE1_REDUCED basis-amortization more than offsets Path A's recompute
overhead. All existing tests pass at logn ∈ [3, 10]; bit-exact KAT match
against the existing path at logn ∈ {9, 10}.

This is the third in a stack of three opt-in flags:
- `FNDSA_PATH_B`: 4/8 KiB savings, 1-2% perf cost (already shipped)
- `FNDSA_PHASE1_REDUCED`: 3/6 KiB savings, ~0% perf (already shipped)
- `FNDSA_FFSAMP_5N_REDUCED`: 3/6 KiB savings, **net ~0% perf or slight gain**
                              (this PR)

Cumulative tmp[] savings vs Pornin's main:
  FN-DSA-512:  30,239 → 18,975 bytes (saves 11 KiB, 37% smaller)
  FN-DSA-1024: 60,447 → 37,919 bytes (saves 22 KiB, 37% smaller)

Motivation
==========

Smartcard / SE deployments (Ledger ST33K1M5 family on Stax, Flex, Nano S+)
have ~32 KiB net application SRAM after BOLOS reserves ~31 KiB of the 64 KiB
total. Combined with the Ethereum app's runtime working set (parse buffers,
BOLOS framework overhead, stack), this leaves only ~14-20 KiB available for
the FN-DSA temporary buffer. Path A's reduction to 37n+31 bytes (= 18,975 at
FN-DSA-512) brings the buffer comfortably within budget on all targeted
devices. Without Path A, FN-DSA-512 + Ethereum app deployment is borderline
or doesn't fit at all on display-heavy Ledger devices.

Algorithmic change
==================

The core move is to drop `l10` from the persistent set across the right
recursion in `ffsamp_fft_inner`'s outer-level body, then recompute it
from `external_basis` post-recursion. This requires:

1. **A fused fpoly_muladd_fft primitive** for in-place `c1 += t1·l10`
   computation without scratch:

     fpoly_muladd_fft(logn, c, a, b):
         per FFT-domain complex coefficient k:
             c_re[k] += a_re[k]*b_re[k] - a_im[k]*b_im[k]
             c_im[k] += a_re[k]*b_im[k] + a_im[k]*b_re[k]

   This is fused multiply-add (FMA, analogous to Intel `_mm_fmadd_pd` /
   ARM `vfmaq_f64`), NOT a cryptographic Message Authentication Code.
   Implemented in 4 architecture variants: SSE2, NEON, RV64D, scalar.
   Eliminates the n-FLR scratch slot the un-fused chain (memcpy + mul +
   add) would otherwise need.

2. **A new fpoly_g01_fft_external primitive** that recomputes `g01`
   (the off-diagonal Hermitian gram entry) from external_basis directly:

     g01 = b00·adj(b10) + b01·adj(b11)

   Per-coefficient, no scratch beyond dst. Same SSE2/NEON/scalar variants.

3. **Outer-level body restructure in `ffsamp_fft_inner`** (16-quarter
   layout):

     qc(0..3)    c1               (n FLR, persistent across right rec)
     qc(4..5)    d00              (½n FLR, persistent across right rec)
     qc(6..7)    callee t0        (½n; split-low of t1)
     qc(8..9)    callee t1        (½n; split-high of t1)
     qc(10..11)  right_01         (½n; callee g01)
     qc(12)      right_00         (¼n; callee g00 self-adj)
     qc(13)      right_11         (¼n; callee g11 self-adj = right_00)
     qc(14..15)  free / scratch

   Persistent_above shrinks from 2.5n (PATH_B's c1+l10+d00) to 1.5n (just
   c1+d00). Callee tmp moves from qc(10) down to qc(6), giving outer-level
   peak 1.5n + 2.5n_callee = 4n FLR.

4. **Post-right-recursion recompute step**:
     - `fpoly_g01_fft_external` recomputes g01 into qc(10..13)
     - `fpoly_LDL_fft(d00, qc(10), qc(14))` derives l10 (in place)
     - `fpoly_pathb_finalize` consumes l10 + z_split → tb0 + z1

   The recompute reads basis from external_basis (passed via
   `sampler_state.external_basis`, set by sign_core when caller invoked
   the with-basis API).

Files changed
=============

Production code:
- `inner.h`: `FNDSA_FFSAMP_5N_REDUCED` flag definition + design comment.
  Errors at compile time if used without `FNDSA_PHASE1_REDUCED`.
- `sign_inner.h`: `external_basis` field added to `sampler_state`
  (gated on FFSAMP_5N). Declarations for the two new primitives.
- `sign_core.c`: wires `external_basis` from sign_with_basis_temp into
  `sampler_state`; updates hm_offset_n to 34n bytes under FFSAMP_5N.
- `sign.c`: G_offset_n = 36n under FFSAMP_5N; min tmp_len check tightened
  to 37n+31; explicit NULL-basis rejection (overflow vulnerability fix —
  applied to both with-FFSAMP_5N and prior PHASE1_REDUCED-only path).
- `sign_fpoly.c`: implementations of `fpoly_muladd_fft` and
  `fpoly_g01_fft_external` in 4 architecture variants each (~110 +
  ~100 = ~210 lines).
- `sign_sampler.c`: new outer-level Path A body in `ffsamp_fft_inner`,
  gated on `logn == ss->logn AND ss->external_basis != NULL`. ~110 lines.
  Inner recursive calls take the standard PATH_B body unchanged.

Tests:
- `test_ffsamp_5n.c`: paint-and-check at the new 37n+31 byte boundary.
  Confirms bytes [37n, 51n+31) are untouched at logn ∈ {9, 10}.
- `test_ffsamp_5n_peak.c`: direct measurement of `ffsamp_fft_inner`'s
  internal peak under Path A (analogous to test_path_b_peak.c).
  Confirms T(L) = 16 outer-q = 4.0n FLR at logn ∈ {9, 10}.
- `test_phase1_api.c`: extended with NULL-basis rejection check.

Benchmarks:
- `bench_full_chain.c` + `run_full_chain_bench.sh`: full per-sign
  measurement across all 4 flag combinations.
- `run_full_chain_scalar.sh`: same with SIMD disabled (M-class proxy).

Public API
==========

The `FNDSA_FFSAMP_5N_REDUCED` flag tightens `fndsa_*_with_basis_temp`'s
minimum tmp_len from 43n+31 to 37n+31:

    Default                              : 59n+31 bytes
    With FNDSA_PATH_B                    : 51n+31 bytes
    With PATH_B + PHASE1_REDUCED         : 43n+31 bytes
    With PATH_B + PHASE1 + FFSAMP_5N     : 37n+31 bytes

The flag is dependent: requires `FNDSA_PHASE1_REDUCED` (which itself
requires `FNDSA_PATH_B`). Compile-time errors if used without the
required prerequisites.

The 37n+31 minimum is sized for both SIMD and scalar build configurations:
- SIMD: FP-stays post-ffsamp scratch ends at byte 34n; hm at 34n, G at 36n.
- Scalar: integer post-ffsamp ends at byte 26n; same offsets, 2n bytes/n
  unused-but-allocated. Acceptable for a unified API minimum.

Validation
==========

All shipped at the time of submission:
- ✓ test_fndsa passes at logn ∈ [3, 10] under all 8 flag combinations
  (each of {PATH_B, PHASE1_REDUCED, FFSAMP_5N} on/off).
- ✓ test_phase1_api: bit-exact KAT match at logn ∈ {9, 10} with FFSAMP_5N.
- ✓ test_ffsamp_5n: 37n+31 boundary respected at logn ∈ {9, 10}.
- ✓ test_ffsamp_5n_peak: T(L) = 16 outer-q (4n FLR) confirmed at
  logn ∈ {9, 10}.
- ✓ ASAN-clean.
- ✓ NULL-basis rejection regression test.

Performance (median of 10 runs, host NEON, ns per sign):

    Configuration                          | logn=9     | Δ        | logn=10    | Δ
    ---------------------------------------|------------|----------|------------|----------
    baseline                               | 132,448    | —        | 237,106    | —
    PATH_B                                 | 131,539    | -0.69%   | 237,357    | +0.11%
    PATH_B + PHASE1_REDUCED                | 129,875    | -1.94%   | 235,190    | -0.81%
    PATH_B + PHASE1 + FFSAMP_5N            | 129,988    | -1.86%   | 234,444    | -1.12%

Performance (median of 5 runs, scalar emulation, ns per sign):

    Configuration                          | logn=9     | Δ        | logn=10    | Δ
    ---------------------------------------|------------|----------|------------|----------
    baseline                               | 1,333,345  | —        | 2,859,738  | —
    PATH_B                                 | 1,376,428  | +3.23%   | 2,959,530  | +3.49%
    PATH_B + PHASE1_REDUCED                | 1,180,969  | -11.43%  | 2,520,890  | -11.85%
    PATH_B + PHASE1 + FFSAMP_5N            | 1,221,435  | -8.39%   | 2,602,476  | -8.99%

The full chain (PATH_B + PHASE1_REDUCED + FFSAMP_5N) is empirically
faster than baseline on both host NEON (~2%) and scalar emulation (~9%),
because PHASE1_REDUCED's elimination of `basis_to_FFT` per sign more than
offsets PATH_B body's small overhead and Path A's recompute step.

For reference, fpr_div is 458 cycles on M4 (Pornin's table 2), and the
recompute step adds n fpr_div ops per sign. At logn=9, this is ~234k
cycles = ~1% of the 22M-cycle baseline sign. The bench data is consistent
with that estimate.

Backwards compatibility
=======================

Default off — no behavior change unless the caller explicitly enables
`FNDSA_FFSAMP_5N_REDUCED` at compile time. Adds a new public-API flag but
does NOT change any existing API signatures or semantics.

The new outer-level body is gated on TWO conditions: `logn == ss->logn`
(only the top-level call, not inner recursions) AND `ss->external_basis
!= NULL` (only when the caller went through the with-basis API). All other
paths use the standard PATH_B body unchanged.

Stacked on prior PRs
====================

This PR is the third in a stack:
- PR 1: `FNDSA_PATH_B` (already shipped at commit 3d0e8ca)
- PR 2: `FNDSA_PHASE1_REDUCED` (in review)
- PR 3 (this): `FNDSA_FFSAMP_5N_REDUCED`

If PRs 1-2 are not yet merged, this PR is documented as stacked on them.
The three flags' design is intentionally additive: each opt-in flag
unlocks the next layer of optimization.

Caveats and limitations
=======================

1. **logn=2 (n=4) caveat inherited from PATH_B**: at the smallest
   parameter, FP-order changes can cross integer-rounding boundaries
   in the Gaussian sampler. FN-DSA does not standardize n=4, so the
   test suite skips it. No action needed under FFSAMP_5N — it just
   inherits PATH_B's existing behavior.

2. **NULL basis rejection is now mandatory** in the with-basis API.
   Pre-FFSAMP_5N, passing NULL basis was already unsafe (51n+31 layout
   needed but only 43n+31 enforced); FFSAMP_5N tightens the gap further.
   The fix applies to both build configurations.

3. **Scalar projection for SE deployment** assumes the same component
   ratios as Pornin's M4 numbers. Hardware bench under SE-class
   countermeasures (masking, lockstep) is needed to confirm M35P
   numbers translate from this scalar emulation. Donjon coordination
   tracks this.

4. **Path A is OUTER-LEVEL only.** Inner recursion levels keep the
   standard PATH_B body (5n_{L-1} peak). The 4n FLR claim is for the
   outer call only; the next-level callee at L-1 still uses 2.5n_L
   parent FLR per the recursive fixed point. Inner reduction would
   require its own external_basis access at every recursion level —
   not feasible without significant restructuring.

Stacked-PR review aid
=====================

For reviewers who want to see just this PR's changes without PR 1 and
PR 2 noise, the diff is best viewed as:

    git diff phase1-reduction..ffsamp-5n-reduction

The rebased diff is small (~600 lines) and well-localized: 2 new fpoly
primitives, 1 new sampler_state field, 1 new outer-level body branch
in ffsamp_fft_inner, 4 small offset adjustments, 4 small wrapper edits.
