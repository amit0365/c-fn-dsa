Subject: Reduce signing tmp[] from 51n+31 to 43n+31 bytes via opt-in `FNDSA_PHASE1_REDUCED` + precomputed-basis API

Summary
=======

This patch adds an opt-in compile-time flag (`FNDSA_PHASE1_REDUCED`,
default 0; requires `FNDSA_PATH_B`) that introduces a precomputed-basis
API: the caller computes the lattice basis B = [[g, -f], [G, -F]] in FFT
representation once via `fndsa_compute_basis()`, stores it in a caller-
allocated buffer (typically smartcard flash), and passes it to a new
`fndsa_sign_*_with_basis_temp()` API instead of recomputing per sign.

Effect on the signing temporary buffer:

  - Default                              : 59n+31 bytes
  - With FNDSA_PATH_B                    : 51n+31 bytes  (8n saved)
  - With FNDSA_PATH_B + PHASE1_REDUCED   : 43n+31 bytes  (16n saved
                                                          via the new
                                                          with-basis API)

Saves an additional 4 KiB at FN-DSA-512 / 8 KiB at FN-DSA-1024 on top of
the FNDSA_PATH_B reduction. Bit-exact KAT match against the existing
recompute path at logn ∈ {9, 10}. 4-flag-combination build matrix is
clean (baseline / PATH_B / PATH_B+PHASE1 / PATH_B+PHASE1+FFSAMP_5N).

For signing throughput, PHASE1_REDUCED is **net positive** because the
basis_to_FFT computation (4× FFT + set_small + neg) is hoisted out of
the per-sign hot path and amortized to provisioning time. Empirical
measurement (10-run host NEON median, logn=9): PHASE1_REDUCED is ~2%
faster per sign than baseline.

This is the second in a stack of three opt-in flags:
- `FNDSA_PATH_B`: 4/8 KiB savings, ~1-2% perf cost (already shipped)
- `FNDSA_PHASE1_REDUCED` (this PR): 4/8 KiB savings, **~0% perf cost**
                                     (slightly faster, in fact)
- `FNDSA_FFSAMP_5N_REDUCED`: 3/6 KiB savings, ~0% perf

Motivation
==========

Smartcard / SE deployments (Ledger ST33K1M5 family on Stax, Flex,
Nano S+) have ~32 KiB net application SRAM after BOLOS reserves ~31 KiB
of the 64 KiB total. Combined with the Ethereum app's runtime working
set, the FN-DSA temporary buffer must fit well below 32 KiB.

Without PHASE1_REDUCED, the per-sign FN-DSA temporary at FN-DSA-1024
under FNDSA_PATH_B alone is 52,255 bytes (≈51 KiB) — substantially
over budget. PHASE1_REDUCED brings this to 44,063 bytes (≈43 KiB),
narrowing but not closing the gap. (Closure to ≤32 KiB requires the
companion `FNDSA_FFSAMP_5N_REDUCED` flag, filed separately.)

For ST33K1M5 specifically, the precomputed basis (4n FLR = 32n bytes
at FN-DSA-1024, 16n bytes at FN-DSA-512) lives in caller-supplied
flash. The deployment runbook (separate document) covers atomic-flag-
page write protocols for tear-resistance against power loss mid-write.

The basis_to_FFT amortization
=============================

Baseline (no flags) signing recomputes the full lattice basis in FFT
representation on every sign:

    fpoly_set_small(b00, g);  fpoly_set_small(b01, f);
    fpoly_set_small(b10, G);  fpoly_set_small(b11, F);
    fpoly_FFT(b00);  fpoly_FFT(b01);  fpoly_FFT(b10);  fpoly_FFT(b11);
    fpoly_neg(b01);  fpoly_neg(b11);

This is ~5% of FN-DSA-512 sign time on M-class CPUs (Pornin's M4 paper,
table 2). It can be hoisted to provisioning time without changing the
signature output, since the basis depends only on the (f, g, F)
signing key — not the message or nonce.

PHASE1_REDUCED adds two new primitives that operate on a read-only
external basis buffer:

1. **fpoly_gram_fft_dst** — non-destructive variant of `fpoly_gram_fft`
   that reads basis from external memory and writes gram outputs
   (g00, g01, g11) to specified destinations in compact form. The
   compact form (g00 ½n + g01 n + g11 ½n = 2n FLR) replaces the
   full-size in-place version (4n FLR including dead b11) used in
   the baseline recompute path.

2. **fpoly_apply_basis_external** — thin wrapper around the existing
   `fpoly_apply_basis` (with FNDSA_PATH_B's preserve-b01 modification),
   reading b01 and b11 from the external basis instead of an in-tmp
   buffer.

In sign_core, when an `external_basis` is provided, phase 1 skips
`basis_to_FFT` + `fpoly_gram_fft` + the compact rearrange entirely:

    if (external_basis != NULL) {
        fpoly_apply_basis_external(t0, t1, external_basis, hm);
        fpoly_gram_fft_dst(g00, g01, g11, external_basis);
        goto phase1_done;
    }

The phase 1 footprint drops from 6n FLR (under PATH_B) to 4n FLR
(2n compact gram + 2n target vector). Under FNDSA_PATH_B's recursive
ffsamp peak of 5n FLR, the overall sign tmp[] becomes:

    max(phase 1 = 4n, ffsamp = 5n) + hm + G + alignment = 5n + 3n + 31
                                                         = 43n+31 bytes

Public API additions
====================

Three new public functions are added:

1. **fndsa_compute_basis** — compute the basis from a signing key and
   store it in a caller-allocated buffer. Called once at provisioning.

       int fndsa_compute_basis(
           const void *sign_key, size_t sign_key_len,
           void *basis_buf, size_t basis_buf_len);

   Returns 1 on success, 0 on error (invalid key, buffer too small).

2. **fndsa_sign_with_basis_temp** — sign with a precomputed basis,
   sysrng for randomness:

       size_t fndsa_sign_with_basis_temp(
           const void *sign_key, size_t sign_key_len,
           const void *basis,
           const void *ctx, size_t ctx_len,
           const char *id, const void *hv, size_t hv_len,
           void *sig, size_t max_sig_len,
           void *tmp, size_t tmp_len);

   Returns the signature length on success, 0 on error.

3. **fndsa_sign_seeded_with_basis_temp** — same with explicit seed for
   deterministic signing (typically used for KAT validation).

Plus a sizing macro:

    #define FNDSA_BASIS_SIZE(logn)   ((size_t)32 << (logn))

The basis is 4n FLR = 32n bytes for logn ≥ 2. Caller is responsible
for the allocation; typical placement is non-volatile (flash) on
embedded targets, plain RAM otherwise.

Files changed
=============

Production code:
- `inner.h`: `FNDSA_PHASE1_REDUCED` flag definition + design comment.
  Errors at compile time if used without `FNDSA_PATH_B`.
- `sign_inner.h`: declarations for `fpoly_gram_fft_dst` and
  `fpoly_apply_basis_external` (gated on PHASE1_REDUCED).
- `sign_core.c`: optional `external_basis` parameter. When non-NULL,
  phase 1 takes the new fast path (skips basis_to_FFT + gram_fft +
  rearrange); when NULL, falls back to the existing PATH_B / baseline
  path. Updated hm_offset_n: 40n bytes when external_basis non-NULL
  under PATH_B + PHASE1.
- `sign.c`: G_offset_n = 42n under PHASE1_REDUCED + external_basis.
  New `fndsa_compute_basis` and `fndsa_sign_*_with_basis_temp` public
  functions (gated on PHASE1_REDUCED). Wrapper `sign_with_basis_wrapper`
  validates inputs (sign_key header byte 0x50|logn; tmp_len ≥ 43n+31;
  max_sig_len ≥ FNDSA_SIGNATURE_SIZE).
- `sign_fpoly.c`: implementations of `fpoly_gram_fft_dst` and
  `fpoly_apply_basis_external` in 4 architecture variants each.

Tests:
- `test_phase1.c`: paint-and-check at the new 43n+31 byte boundary at
  logn ∈ {9, 10}. Bytes [43n, 51n+31) confirmed untouched.
- `test_phase1_api.c`: end-to-end test of fndsa_compute_basis +
  fndsa_sign_with_basis_temp; bit-exact KAT match against the existing
  recompute path; undersized-tmp_len rejection check.
- `test_phase1_primitives.c`: standalone unit test of
  `fpoly_gram_fft_dst` confirming bit-identical output to
  `fpoly_gram_fft` on the same basis.
- `test_phase1_signing.c`: direct sign_core invocation with
  external_basis, validates the full signing pipeline at the primitive
  level.

Validation
==========

All shipped at the time of submission:
- ✓ test_fndsa passes at logn ∈ [3, 10] under all 4 flag combinations
  (each of {PATH_B, PHASE1_REDUCED} on/off).
- ✓ test_phase1_api: bit-exact KAT match at logn ∈ {9, 10}.
- ✓ test_phase1: 43n+31 boundary respected at logn ∈ {9, 10}.
- ✓ test_phase1_primitives: fpoly_gram_fft_dst bit-identical to
  fpoly_gram_fft.
- ✓ ASAN-clean.
- ✓ Undersized tmp_len rejection (tmp_len = 43n+30 returns 0).

Performance (median of 10 runs, host NEON, ns per sign):

    Configuration                 | logn=9    | Δ        | logn=10   | Δ
    ------------------------------|-----------|----------|-----------|--------
    baseline                      | 132,448   | —        | 237,106   | —
    + PATH_B                      | 131,539   | -0.69%   | 237,357   | +0.11%
    + PATH_B + PHASE1_REDUCED     | 129,875   | -1.94%   | 235,190   | -0.81%

Performance (median of 5 runs, scalar emulation, ns per sign):

    Configuration                 | logn=9     | Δ        | logn=10    | Δ
    ------------------------------|------------|----------|------------|--------
    baseline                      | 1,333,345  | —        | 2,859,738  | —
    + PATH_B                      | 1,376,428  | +3.23%   | 2,959,530  | +3.49%
    + PATH_B + PHASE1_REDUCED     | 1,180,969  | -11.43%  | 2,520,890  | -11.85%

PHASE1_REDUCED is empirically **faster than baseline** because the
basis_to_FFT cost (4 FFTs + 4 set_small + 2 neg) is hoisted to
provisioning. The savings are particularly pronounced under scalar
emulation (≈11% faster) where SIMD does not accelerate the FFT.

For SE deployment, the per-sign cost includes flash reads of the
precomputed basis (32 KiB at logn=10). On STM32F407 at 168 MHz,
out-of-cache flash adds 5 cycles per access; M35P with masking +
lockstep may differ. Hardware bench (Donjon) is the load-bearing
measurement for the deployed SE configuration.

Atomic-flag-page write protocol
================================

For SE deployment where the basis lives in flash, mid-write power
loss can leave the basis partially written. The recommended protocol
(documented separately in the deployment runbook):

1. Write the basis bytes to a staging region.
2. Verify checksum / HMAC of the staged bytes.
3. Write a single-page commit flag to a known address; the SE
   guarantees single-page write atomicity.
4. Sign-time check: validate the commit flag; if invalid, re-run
   provisioning.

Caveats: on multi-page basis (32 KiB at logn=10 needs ~512 64-byte
pages), the staging region itself can be partially written — the
HMAC step catches that case. Page-level atomicity is the primitive
the SE provides; we build atomicity-of-the-whole-basis on top.

Backwards compatibility
=======================

Default off — no behavior change unless the caller explicitly enables
`FNDSA_PHASE1_REDUCED` at compile time AND uses the with-basis API.
Adds new public functions but does NOT change any existing API
signatures or semantics.

The standard (without-basis) API path continues to work unchanged
under PHASE1_REDUCED — when external_basis is NULL, sign_core falls
back to the baseline / PATH_B recompute path. This means a binary
built with `FNDSA_PHASE1_REDUCED=1` accepts both APIs at runtime;
the caller chooses per-call.

Stacked on prior PR
===================

This PR is stacked on the FNDSA_PATH_B PR (commit 3d0e8ca on the
`path-b-tmp-reduction` branch). PHASE1_REDUCED requires PATH_B's
preserve-b01 apply_basis variant — `fpoly_apply_basis_external` is
a thin wrapper around it. Without PATH_B, the basis would be destroyed
during apply_basis, breaking the precomputed-basis contract.

If the PATH_B PR has not yet been merged, this PR is documented as
stacked on it. The two together constitute the smallest deliverable
unit; FNDSA_PHASE1_REDUCED alone has no use without PATH_B's
preserve-b01 modification.

Caveats and limitations
=======================

1. **logn=2 (n=4) caveat inherited from PATH_B**: at the smallest
   parameter, FP-order changes can cross integer-rounding boundaries.
   FN-DSA does not standardize n=4. Test suite skips it under PATH_B;
   PHASE1_REDUCED inherits the skip without further changes.

2. **Per-app NV budget on Ledger** is 32 KiB at FN-DSA-1024.
   Confirmed feasible on ST33K1M5 (1.5 MB flash, 2% utilization).
   Smaller-NV platforms may need to defer to FN-DSA-512 (16 KiB
   basis) or skip PHASE1_REDUCED entirely.

3. **Tear-resistance is caller's responsibility.** The library
   provides `fndsa_compute_basis` (one-shot synchronous computation
   to a caller-allocated buffer); the atomic-flag-page write protocol
   for tear-safe flash storage is application-level. Recommended
   shape: HMAC-tag the basis at provisioning, verify-on-read at sign.

4. **Requires the with-basis API to realize savings.** A binary built
   with PHASE1_REDUCED but using only the standard API gets no tmp[]
   reduction (still 51n+31 under PATH_B). The reduction is conditional
   on the caller's API choice.

5. **NULL basis acceptance is now an explicit error** — the with-basis
   wrapper rejects NULL basis to prevent buffer overflow when sign_core
   would fall back to the larger no-basis layout. Documented in fndsa.h.

Stacked-PR review aid
=====================

For reviewers viewing just this PR's incremental changes:

    git diff path-b-tmp-reduction..phase1-reduction

The diff is moderate (~700 lines): 2 new fpoly primitives, optional
external_basis parameter on sign_core, new public API in sign.c, plus
the four test files.
