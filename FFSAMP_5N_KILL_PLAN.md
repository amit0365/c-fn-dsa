---
name: ffsamp 5n→4n Reduction Kill Plan
description: Drop l10 from ffsamp's outer-level persistent set and recompute from external_basis post-right-recursion. Saves 4 KiB at FN-DSA-512 / 8 KiB at FN-DSA-1024.
---

# ffsamp 5n→4n reduction kill plan

## Goal

Reduce the **outer-level** ffsamp peak from 5n FLR to 4n FLR by dropping
l10 from the persistent set across the right recursion and recomputing
it from `external_basis` post-recursion. Inner recursion levels are
unaffected (they have no access to a basis to recompute from).

If successful, total sign tmp[] drops from 43n+31 to **35n+31 bytes**:
- FN-DSA-512:  22047 B → 17951 B (saves 4 KiB on top of phase 1 reduction)
- FN-DSA-1024: 44063 B → 35871 B (saves 8 KiB)

This is the move that makes FN-DSA-512 + Ethereum app **fit on Nano-class
ST33K1M5 with margin** (32 KiB net app SRAM after BOLOS reservation).

## Why this is the deployment-critical move, not an optimization

Per the budget math in `ffsamp_5n_exploration.md`:

| Build | tmp[] | Total peak | Fits 32 KiB Nano? |
|---|---|---|---|
| Current head (PATH_B + phase 1) | 22047 B | 31.5–36.5 KiB | borderline; doesn't fit at mid-to-high estimates |
| **+ Path A (35n+31)** | **17951 B** | **27.5–32.5 KiB** | **fits at low/mid; borderline at high end** |

Without Path A, FN-DSA-512 deployment on Nano-class is borderline.
Path A's 4 KiB is what closes the budget across the realistic estimate
range. **This is the load-bearing move for shipping.**

## Why this is structurally different from prior moves

PATH_B body and phase 1 reduction both pushed against *intra-ffsamp*
constraints. Path A pushes against the **recursive fixed point**:

```
t(L) = max(4.5, 2.5 + 0.5 · t(L−1))   →   t* = 5
```

The 2.5 is `persistent_above = c1 + l10 + d00` (qc(0..9)) — what must
survive across the right recursion. We can't break the fixed point
recursively (inner levels have no `external_basis` to recompute from),
so we break it **only at the outer level**, where `external_basis` is
in scope.

Inner levels stay at 5n_{L−1}; outer level drops to 4n. Since outer
is the binding absolute peak, total tmp[] drops 1n FLR.

## Day 0 (already done — included for context)

✅ **Recursive PB tightening (5b85fdf)** confirmed empirically that
   T(L) = 5n at every level via `test_path_b_peak.c`. Day 9.5 took
   tmp[] from 45n+31 to 43n+31 by tightening the layout reservation.

✅ **ffsamp_5n_exploration.md (a0a9560)** derived the fixed point from
   the actual code, evaluated 3 candidate paths (l10-recompute,
   slot-reordering, streaming), and confirmed Path A is the only viable
   move.

✅ **Recompute kernel microbench (38b8ec0)** measured 0.18% host overhead
   at logn=9 — well under the 2% acceptability threshold. M4 cycle
   counts back-calculated from Pornin's table 2 give ~2% scalar; still
   under 5%.

✅ **Security stance compliance (ef74c3b)** confirmed Path A preserves
   all four of Pornin's stance rules. New attack surfaces (flash SPA,
   bit-flip, basis div traffic) flagged as deployment-runbook items,
   not implementation gates.

✅ **Donjon questions** (items 25-30) drafted to gate deployment
   validation in parallel with implementation.

## Day 1: Wire external_basis through to ffsamp + restructure outer body

**Goal:** Make `external_basis` reachable from `ffsamp_fft_inner` at the
outer-level call, and replace the outer-level persistent layout with the
new c1+d00 (1.5n) + callee@qc(6) shape.

**Tasks:**

1. Add `const fpr *external_basis` field to `sampler_state` struct
   (`sign_inner.h`). Set in `sign_core` before calling `ffsamp_fft`,
   only when FNDSA_PHASE1_REDUCED + caller passed external_basis.

2. In `ffsamp_fft_inner`, gate a new outer-only branch behind
   `#if FNDSA_PHASE1_REDUCED` + `if (logn == ss->logn && ss->external_basis != NULL)`:
   - New layout: c1 @ qc(0..3) (n), d00 @ qc(4..5) (½n), callee @ qc(6)
   - Step 1: `fpoly_LDL_fft(qc(12), qc(8), qc(14))` — compute l10 in qc(8..11)
   - Step 2-3: c1 = t0 + t1·l10 (same as PATH_B body)
   - Step 4 NEW: move d00 from qc(12..13) → qc(4..5); callee tmp = qc(6)
     (drop l10 — overwrite qc(8..11) and qc(12..13))
   - Step 5-7: split t1, split d11 → callee positions starting at qc(6)
   - Step 8: right recursion at qc(6) (writes through qc(15) at most)
   - Step 9 NEW: recompute g01 = b00·adj(b10) + b01·adj(b11) from
     `ss->external_basis` into qc(8..11); then l10 = g01 / d00 in place
   - Step 10: `fpoly_pathb_finalize` with l10 at qc(8..11), z at qc(6..9)
   - Step 11+: split d00, split tb0, left recursion, final merge
     (mostly unchanged from PATH_B body, just with relocated slots)

3. Inner-level recursive calls take the existing PATH_B body path
   (`logn != ss->logn` OR `external_basis == NULL`). No structural change
   inside.

**Output:** Code compiles with all 4 flag combinations; test_fndsa runs
to completion (may not pass yet — Day 2 fixes correctness).

**Decision:**
- Compiles + ASAN clean: green
- Compiles only with one flag combination: orange, fix interaction
- Logical impossibility surfaces during implementation: red, kill

**Time:** 1 day.

## Day 2: Correctness — get test_fndsa passing at logn 3..10

**Goal:** Get bit-distribution-equivalence with the existing path. We
don't need bit-exact (the new FP order may diverge in intermediate bits)
but signatures must verify and the empirical distribution must match.

**Tasks:**

1. Run `test_fndsa` under `-DFNDSA_PATH_B=1 -DFNDSA_PHASE1_REDUCED=1`
   with the new outer-level path active. Debug any verification failures.

2. Common failure modes to check:
   - `external_basis` pointer not propagated to inner recursive calls
     (should be — inner levels read it via `ss` only at the outer level)
   - g01 recompute formula sign error (g10 = adj(g01); we want g01)
   - l10 division order — `fpoly_div_selfadj` writes in place vs new buffer
   - `fpoly_pathb_finalize` reads l10 from qc(8..11) — confirm offset
     matches the recompute destination
   - Step 4's overwrite of qc(8..11) and qc(12..13) is safe only if
     the recompute writes to those same locations afterwards (yes, by
     design)

3. ASAN run after correctness: confirm no out-of-bounds reads from
   `external_basis` or writes past the new 4n FLR boundary.

**Output:** test_fndsa passes at logn 3..10 under all 4 flag combinations.

**Decision:**
- All tests pass + ASAN clean: green
- Tests fail at small logn but pass at 9, 10: orange, document and proceed
- Tests fail at logn 9 or 10: red, debug or kill

**Time:** 1 day.

## Day 3: Paint-and-check + bit-distribution-equivalence test

**Goal:** Per-test artifact analogous to `test_path_b.c` and
`test_path_b_peak.c` — direct evidence that the 4n FLR boundary holds.

**Tasks:**

1. Add `test_ffsamp_5n.c`: paint qc(20..27) with sentinel, run a sign,
   verify sentinel intact post-sign at logn 9 and 10. (At outer level
   the new peak is 4n = 16 outer-q, so qc(16..27) should be untouched
   by the outer layout. Inner recursion still uses qc(10..) up to its
   2.5n callee peak, so the precise unmodified region is qc(20..27)
   given callee tmp at qc(6).)

2. Add a bit-distribution-equivalence check: sign 1000 random messages
   under the existing path and the new Path A path; confirm:
   - All signatures verify
   - The distribution of z0/z1 byte values matches statistically (this
     is rather than bit-exact since FP order changes)

3. Optional: bit-exact comparison at the same KAT seeds — likely fails
   because Path A's FP order differs from the existing path. Document
   if so.

**Output:** `test_ffsamp_5n.c` passes; statistical equivalence
documented in commit message.

**Decision:**
- Painted region intact + verifies pass: green
- Painted region modified: red, layout violation, fix
- Verification fails: red, correctness regression, fix

**Time:** 1 day.

## Day 4: Update tmp_len budget + composition + ASAN

**Goal:** Update the public API to advertise the 35n+31 minimum, confirm
all flag combinations still work, final ASAN sweep.

**Tasks:**

1. `fndsa.h`: update tmp_len docstring table to add a new column or row
   for "PATH_B + PHASE1_REDUCED + Path A" with 17951 / 35871 numbers.
   New formula: 35n+31 bytes when all three flags enabled.

2. `sign.c`: tighten `min_tmp` check in `sign_with_basis_wrapper` to
   35n+31 when Path A is active (gated on a new `FNDSA_FFSAMP_5N_REDUCED`
   build flag, default 0).

3. Test all 8 flag combinations:
   - baseline (no flags)
   - PATH_B only
   - PATH_B + PHASE1_REDUCED
   - PATH_B + PHASE1_REDUCED + FFSAMP_5N_REDUCED (the new one)
   - PHASE1_REDUCED alone (should error: requires PATH_B)
   - FFSAMP_5N_REDUCED alone or with only PATH_B (should error:
     requires PHASE1_REDUCED for external_basis)

4. Final ASAN run on all 8 combinations; bench_recompute still passes
   under the new build to confirm no regression.

**Output:** Clean diff, all flag combinations work or error correctly.

**Decision:**
- All 8 configs work as expected: green
- Conflict between flags: investigate and fix

**Time:** 1 day.

## Day 5: Polish + PR + Donjon coordination

**Goal:** Get the patch into upstream-PR-ready state and send Donjon
questions in parallel.

**Tasks:**

1. Audit comments: each change references its design rationale, no
   internal kill-plan-doc references.

2. Update `inner.h`'s docstrings: add `FNDSA_FFSAMP_5N_REDUCED` flag
   with full design rationale (the t(L) = max(4.5, 2.5 + 0.5·t(L-1))
   fixed point + outer-level break via external_basis recompute).

3. Update `fndsa.h` public docstring with the further-reduced budget
   and the 3-flag composability matrix.

4. Send `ledger_donjon_questions.md` (items 25-30) to Donjon contact —
   gate deployment validation, not the PR itself.

5. PR text: layered on top of phase1-reduction PR (which is itself
   layered on path-b-tmp-reduction). Three-PR stack, each with clean
   incremental savings:
   - PATH_B: saves 4/8 KiB
   - PHASE1_REDUCED: saves additional 3/6 KiB + 1/2 KiB (Day 9.5)
   - FFSAMP_5N_REDUCED: saves additional 4/8 KiB

**Output:** PR ready to file once previous PRs in the stack are
mergeable.

**Time:** 1 day.

## Total: 5 working days

Tighter than the phase 1 kill plan (10 days) because:
- The exploration is done (no Day 1-2 option survey needed)
- The bench is done (no perf gate to clear)
- The security analysis is done (no stance compliance investigation)
- We're applying the same pattern as phase 1 reduction (extend
  ffsamp_fft_inner with a flag-gated outer-level branch + new primitive
  call) so the engineering shape is familiar

## Three risks worth flagging upfront

### Risk 1: Bit-exact KAT mismatch may fail at logn=2

The recompute changes FP order at the outer level. Per the PATH_B
logn=2 caveat, FP-order changes can cross integer-rounding boundaries
at small n. Possible that Path A introduces a NEW logn-specific
divergence at logn=3 or 4 that wasn't present in PATH_B alone.

**Kill criterion:** if logn=9 or 10 fails, kill. logn ≤ 4 failures get
documented and skipped.

### Risk 2: Inner-level fixed point may have hidden dependence on l10 layout

The PATH_B body internally relies on l10 at qc(4..7) post-step-6. If
the outer-level path drops l10 from that slot during the right recursion,
the inner recursive call chain at logn-1 still expects its own l10 at
its qc(4..7). This is fine because the inner call's tmp pointer is
qc(6) at the outer level (which is callee tmp at L-1's qc(0)), so the
inner level's qc(4..7) is at outer qc(8..11) — which IS the slot we
overwrite for d00 storage in step 4.

**This needs careful aliasing analysis on Day 1.** If the inner call
reads from outer qc(8..11) (its own l10 slot) and we've overwritten
that with d00 staging, the inner call will see corrupt data.

The phase1-reduction layout (current head) avoids this by leaving
qc(8..11) untouched after step 4. Path A overwrites it for the d00
relocation. Need to confirm by tracing the inner call's reads.

**Kill criterion:** if the aliasing makes the layout impossible without
extra scratch space, the 4n FLR target may not be reachable; need to
fall back to 4.5n which doesn't save enough to be worth shipping.

### Risk 3: M4 cycle math underestimates Path A's perf cost

Bench shows 0.18% host (NEON SIMD), back-calculation from Pornin's
M4 table 2 gives ~2% (scalar div-heavy). Donjon hardware bench may
show higher if M35P-specific assembly or masking adds overhead.

**Kill criterion:** if Donjon hardware bench shows >5% per-sign
overhead, reconsider whether the 4 KiB saving is worth it. (Probably
still yes given the deployment math, but worth re-validating.)

**Mitigation:** the bench is non-blocking for shipping the C code under
a flag (off by default for non-Ledger users, on for Ledger build).
Donjon's number determines whether Ledger flips the flag, not whether
upstream merges the patch.

## What to bring forward from prior kill plans

The phase 1 kill plan's strongest pattern was the **feasibility probe
test** (`test_path_b.c` + `test_path_b_peak.c`) that gave a concrete
go/no-go signal at every step. Day 3 of this plan writes
`test_ffsamp_5n.c` for the same reason: paint the supposedly-untouched
region, run a sign, verify the paint survives.

The Day 9.5 recursive-PB tightening pattern (verify empirical T(L)
before tightening the reservation) doesn't apply here — Path A's outer
peak is determined by layout, not empirical recursion. It's a fixed
4n by construction.

## What this kill plan does NOT do

- **It does not reduce inner-level ffsamp peaks.** Inner recursion stays
  at 5n_{L−1}. Going below would require an algorithmic change to
  ffsamp itself (e.g., a streaming variant), which is not in standard
  Falcon literature.

- **It does not address FN-DSA-1024 deployment.** Even with Path A,
  FN-DSA-1024 tmp[] = 35871 bytes ≈ 35 KiB, still over the 32 KiB net
  app SRAM budget. FN-DSA-1024 needs further optimization (or a
  different deployment shape: external scratch buffer, off-chip
  signing, etc.).

- **It does not change Falcon spec compliance or change Pornin's
  security stance.** Per `ffsamp_5n_exploration.md`'s Security stance
  compliance section.

- **It does not require Donjon access to ship.** The C code merges
  under Pornin's existing stance; deployment validation happens in
  parallel.
