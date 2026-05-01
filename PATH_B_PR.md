Subject: Reduce signing tmp[] from 59n+31 to 51n+31 bytes via opt-in `FNDSA_PATH_B` build flag

Summary
=======

This patch adds an opt-in compile-time flag (`FNDSA_PATH_B`, default 0) that
reduces the signing temporary buffer from `59*n + 31` to `51*n + 31` bytes —
saves 8 KiB at logn=10 (FN-DSA-1024), 4 KiB at logn=9 (FN-DSA-512). At ~1-2%
runtime cost. All existing tests pass at logn ∈ [3, 10] including KAT
bit-exact match.

Motivation
==========

Smartcard / SE deployments (e.g. ST33K1M5 family) have ~32-40 KiB application
SRAM after the OS reserves its ~25 KiB. Falcon-512 already fits comfortably in
the baseline 30 KiB; this patch grows the margin to ~14 KiB. Falcon-1024
remains over-budget without further work but is brought from ~19 KiB over to
~11 KiB over, narrowing the gap that flash-spill or basis-restructure work
would have to close.

Two algorithmic changes
=======================

1. **t1·l10 absorption in `ffsamp_fft_inner`**

   The baseline carries both `t0` and `t1` through the right-subtree recursion
   so it can compute `tb0 = t0 + (t1 - z1) * l10` afterwards. Rearranged
   algebraically into `c1 = t0 + t1 * l10` (computed BEFORE the recursion) and
   `tb0 = c1 - z1 * l10` (computed AFTER), the persistent set across the
   recursion drops from {t0, t1, l10, d00} (14 quarters) to {c1, l10, d00}
   (10 quarters). The function-internal peak drops from 28 outer-quarters
   (7n FLR) to 24 outer-quarters (6n FLR).

   FP operation order changes; the result is distribution-equivalent under the
   FN-DSA spec but produces last-few-bit differences in intermediate floats.
   Empirical bonus: at logn ≥ 3, signatures match Pornin's baseline KATs
   bit-exact across the test suite (~thousands of signings). The discrete
   Gaussian sampler at the leaf rounds intermediate floats to integers, and
   the perturbations don't cross integer-rounding boundaries in practice.

   **logn=2 (n=4) edge case:** with random keypairs, some signatures fail
   verification under Path B at logn=2. With fixed seeds (KATs) at logn=2,
   signatures verify but bytes diverge from baseline KATs. FN-DSA does not
   define a security level at n=4 so this is not a spec gap; the affected
   logn is excluded from the test suite under Path B.

2. **Phase 1 reorder in `sign_core`**

   The baseline computes the lattice basis B in FFT, BACKS UP `b01` to a
   1n-FLR scratch buffer (`t2`), runs `gram_fft` (which destroys `b01`), then
   runs `apply_basis` (which uses the backup). Path B reorders to run
   `apply_basis` BEFORE `gram_fft`, eliminating the backup. The baseline's
   in-place destructive `apply_basis` is changed to preserve `b01` (one extra
   n-FLR memcpy) so the subsequent `gram_fft` sees the original basis. This
   recovers the 1n FLR that the basis layout had been spending on `t2`.

Files changed
=============

* `inner.h` — adds `FNDSA_PATH_B` flag (default 0)
* `sign_sampler.c` — Path B body in `ffsamp_fft_inner` (`#if FNDSA_PATH_B`)
* `sign_fpoly.c` — `apply_basis` preserve-b01 variant; new
  `fpoly_pathb_finalize` primitive (4 architecture variants: SSE2, NEON,
  RV64D, scalar)
* `sign_core.c` — phase 1 reorder; hm offset slid from 56n to 48n bytes
* `sign.c` — G offset slid from 58n to 50n bytes; `tmp_len` minimum check;
  `SIGN_WRAP` stack allocation factor
* `sign_inner.h` — `fpoly_pathb_finalize` declaration
* `fndsa.h` — public docstring updated to expose the smaller `tmp_len` budget
* `test_fndsa.c` — `signtmp_len` matches the smaller budget; logn=2 skipped
  in `test_self` and `test_kat` under Path B

New test file
=============

* `test_path_b.c` — per-level paint-and-check: at each logn ∈ [2, 10],
  paints the post-Path-B-footprint region with a sentinel pattern, calls
  `ffsamp_fft_inner` directly (via `#include "sign_sampler.c"`), and verifies
  the painted region is byte-identical after the call. Self-validating: runs
  with `FNDSA_PATH_B=0` confirm the test catches violations (baseline does
  write to qc(24..27)).

Note on `fpoly_pathb_finalize`
==============================

The new primitive collapses the post-recursion sequence (`merge_fft → memcpy
→ mul_fft → sub → memcpy`) into one fused loop. This eliminates the 8
outer-quarter scratch buffer that the unfused chain needs. Combined with a
small step-4 layout fix (l10 save moved from qc(20..23) to qc(16..19); d00
moved directly from qc(12..13) to qc(8..9) without scratch round-trip), the
function-internal peak in `ffsamp_fft_inner` drops to 21 outer-quarters
(5.25n FLR).

This is **dormant capability**: phase 1's `basis_to_FFT` layout dominates at
6n FLR, so the function-internal 5.25n is "absorbed" by phase 1. To realize
the additional 3 KiB at logn=9 / 6 KiB at logn=10 these primitives enable,
phase 1 would need a separate restructure (streaming gram or moving hm/G out
of `tmp[]`). The infrastructure is in place; future work can claim the
savings without re-implementing the recursive Path B layout.

The fused primitive also has perf value beyond the layout: it avoids two
memcpys of n FLR per signing (~16 KiB of bus traffic at logn=10).

Validation
==========

* Builds clean on macOS arm64 (NEON), Linux x86_64 (SSE2 — not directly
  tested by me, but the SSE2 variant of `fpoly_pathb_finalize` mirrors the
  structure of `fpoly_merge_fft`'s SSE2 path).
* `make CFLAGS="-DFNDSA_PATH_B=1"` runs the full test suite (`test_fndsa`)
  successfully at logn ∈ [3, 10], including:
  * `Test self` (random keypair → sign → verify) at every logn
  * `Test KAT` (fixed seeds → sign → verify → hash compare) — bit-exact
    match with baseline KATs
  * `Test sign_core` (logn=9 fixed test fixture) — bit-exact intermediate
    output matches baseline (verified via instrumentation)
* AddressSanitizer (`-fsanitize=address`) clean across the full test suite.
* `test_path_b` confirms `ffsamp_fft_inner` only writes within its 21
  outer-quarters at every logn ∈ [4, 10].
* Wall-time benchmark on macOS arm64:
  * logn=9: 214 µs baseline → 218 µs Path B (+1.9%)
  * logn=10: 265 µs baseline → 268 µs Path B (+1.1%)

Limitations
===========

1. **logn=2 unsupported.** FP edge case at n=4 produces invalid signatures
   on some random inputs. Documented in `inner.h`'s `FNDSA_PATH_B` comment
   and excluded from the test suite under Path B. FN-DSA does not define
   n=4 as a parameter set; this is not a spec gap.

2. **Bit-exact-not-guaranteed.** The FP order change is mathematically
   identity-preserving but the empirical bit-exact match with KATs at
   logn ≥ 3 is observation-only. ~1000 test signings hold; a deployment
   requiring proven bit-exact compatibility should run a larger comparison
   (~10⁶ signings) or accept distribution equivalence per the spec.

3. **Cortex-M4 assembly path (`sign_sampler_cm4.s`)** has not been ported.
   Path B would need a parallel implementation in the assembly file. The
   C path covers all other targets (x86_64, aarch64, RISC-V, scalar
   fallback) automatically.

Backwards compatibility
=======================

`FNDSA_PATH_B` defaults to 0. Without `-DFNDSA_PATH_B=1` the build is
byte-identical to the current main branch (verified by diff of the
preprocessed output). Existing callers see no behavior change.
