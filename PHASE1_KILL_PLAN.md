---
name: Phase 1 Reduction Kill Plan
description: Reduce sign_core's phase 1 footprint below 6n FLR to unlock recursive Path B's dormant 3 KiB / 6 KiB savings at FN-DSA-512 / FN-DSA-1024.
---

# Phase 1 reduction kill plan

## Goal

Reduce `sign_core`'s phase 1 (basis_to_FFT + apply_basis + gram_fft +
compact rearrange) footprint from **6n FLR** to **≤5n FLR**, unlocking
the additional 3 KiB at logn=9 / 6 KiB at logn=10 that recursive Path B's
already-shipped infrastructure makes available.

If successful, total sign tmp[] drops from 51n+31 to ~45n+31 bytes:
- FN-DSA-512:  26,143 B → ~23,071 B (saves another 3 KiB on top of Path B)
- FN-DSA-1024: 52,255 B → ~46,111 B (saves another 6 KiB)

If unsuccessful (no clean option emerges by Day 3), kill the project
honestly and document the binding constraint.

## Why this is harder than Path B

Path B reorganized the *recursive frame* of `ffsamp_fft_inner`. Phase 1
is a *non-recursive* setup phase whose footprint is set by the gram
matrix structure: `g01 = b00·adj(b10) + b01·adj(b11)` requires all four
basis polynomials alive simultaneously. The 4n FLR basis storage is the
binding constraint. No simple reorder eliminates it.

The optimization patterns we used for ffsamp (streaming, fusion, layout
reshuffle) **may or may not** transfer. Day 1-2 of this plan is figuring
out which ones do.

## Day 0 (already done — included for context)

✅ Path B and recursive Path B infrastructure shipped. ffsamp internal peak
   confirmed at 5.25n FLR (verified by [test_path_b.c](test_path_b.c)).
   Phase 1 confirmed as the binding constraint at 6n FLR via empirical
   ASAN check: trying to shrink tmp[] to 45n+31 produced an infinite
   signing-retry loop because phase 1's basis writes at [16n, 48n) bytes
   collided with hm at slid offset 42n.

## Day 1: Survey options

**Goal:** Enumerate every realistic path to <6n FLR phase 1. Don't commit
to any one yet — list pros/cons for each.

**Tasks:**

1. **Streaming basis computation.** Compute b00..b11 incrementally; release
   one before computing the next. Constraint: gram_fft needs all four alive
   for the `g01 = b00·adj(b10) + b01·adj(b11)` step. Investigate whether
   gram can be split into multi-pass with partial-sum accumulation that
   never holds all four at once.

2. **On-demand basis recomputation.** Keep f, F as int8 (1n bytes each)
   alongside g, G; recompute b01 = -FFT(f), b11 = -FFT(F) when needed
   instead of caching. Cost: extra FFTs. Saves: 2n FLR if f and F can
   be recomputed without keeping their FFT-form alive.

3. **Half-size gram outputs in place.** g00 and g11 are self-adjoint;
   they only need n/2 FLR each. Currently `fpoly_gram_fft` writes full
   n FLR and the compact rearrange afterwards moves them to half-size.
   Investigate whether gram can write half-size directly, freeing 1n
   FLR earlier in phase 1.

4. **Move hm and G out of tmp[].** hm (2n bytes) and G (n bytes) currently
   live above the FLR working area at offsets 48n and 50n. If they live
   on the stack inside `sign_core` instead, tmp[] shrinks by 3n bytes
   per signing — but stack grows by the same. Net memory unchanged
   unless the deployment context has different stack vs heap budgets.
   For SE-class targets this is usually NOT a win.

5. **Precomputed basis via separate buffer.** Like Pornin's Rust
   `small_context` flag's inverse: the caller maintains B = [[g, -f],
   [G, -F]] in a separate buffer (computed once at key decode), passed
   into `sign_core` as an explicit parameter. tmp[] no longer needs the
   basis. Saves: full 4n FLR. Cost: API change, ~32 KiB of per-key
   precomputation storage. May or may not be acceptable depending on
   the deployment.

6. **Modular basis representation.** Use NTT (mod q) representation for
   the basis instead of FFT, reducing per-coefficient storage from 8 bytes
   (FLR) to 2 bytes (uint16). Saves up to 3n FLR if the gram computation
   can be done in NTT — but the gram involves complex multiplication of
   complex numbers represented as FFT pairs, not modular multiplication.
   This conflicts with the algorithm structure. **Likely red.**

**Output:** `phase1_options.md` with a table listing each option, estimated
savings, estimated implementation effort, and confidence level.

**Decision:**
- ≥1 option with savings ≥ 1n FLR and effort ≤ 5 days: **green**, pick best
- Only options requiring API changes / external buffers: **orange**, requires
  user decision on whether API changes are acceptable
- Every option blocked by gram's 4-poly dependency: **red**, kill

**Time:** 1 day.

## Day 2: Deep-dive the chosen option

**Goal:** Take the most promising option from Day 1 and design a concrete
implementation. Sketch the new phase 1 structure step-by-step.

**Tasks:**

1. Trace the new phase 1 layout: where does each polynomial live at each
   step? Identify aliasing concerns (similar to the `f1 == f` aliasing
   we hit in `fpoly_split_selfadj_fft` during Path B).

2. Compute the precise peak FLR. If it's 5n FLR, the savings are 1n =
   8 KiB at logn=10 / 4 KiB at logn=9.

3. Identify what new fpoly_* primitives (if any) are needed. If the
   chosen option needs a new primitive, scope its 4-architecture variants
   (SSE2, NEON, RV64D, scalar) similar to `fpoly_pathb_finalize`.

4. Verify the new layout doesn't break the FNDSA_PATH_B-gated code path.
   The new phase 1 should compose with Path B's reordering of
   apply_basis-before-gram_fft.

**Output:** `phase1_layout_walkthrough.md` documenting the new step-by-step
phase 1 structure, similar to [path_b_layout_walkthrough.md](path_b_layout_walkthrough.md).

**Decision:**
- Layout is clean, peak ≤ 5n FLR: **green**, proceed to implementation
- Layout has irreducible aliasing or peak exceeds 5n: **orange**, retry
  with a different option from Day 1
- All Day 1 options fail Day 2 design: **red**, kill

**Time:** 1 day.

## Day 3-4: Prototype

**Goal:** Implement the chosen phase 1 reduction. Gate behind a new flag
`FNDSA_PHASE1_REDUCED` (default 0) so it's separable from `FNDSA_PATH_B`
and the baseline.

**Tasks:**

1. Implement the new phase 1 in `sign_core.c` under the new flag.
2. Add any new fpoly_* primitives in `sign_fpoly.c` with all 4
   architecture variants.
3. Update tmp[] sizing: hm to its new offset, G to its new offset,
   `min_tmp` and `SIGN_WRAP_TMP_FACTOR` accordingly. New documented
   tmp budget: ~45n+31 bytes when both `FNDSA_PATH_B=1` and
   `FNDSA_PHASE1_REDUCED=1`.
4. Update `fndsa.h` to expose the further-reduced budget.

**Output:** A branch off `path-b-tmp-reduction` with the new flag.

**Decision criteria:**
- Compiles clean with all flag combinations: **green**
- Compiles only with one flag combination: **orange**, fix interaction
- Implementation reveals an irreducible structural blocker: **red**, kill

**Time:** 2 days.

## Day 5: Validation — correctness

**Goal:** Confirm the new phase 1 produces valid signatures.

**Tasks:**

1. Build with `-DFNDSA_PATH_B=1 -DFNDSA_PHASE1_REDUCED=1`.
2. Run `test_fndsa` — expect all categories pass at logn ∈ [3, 10].
3. Run with AddressSanitizer.
4. Run `test_path_b.c` (the per-level paint-and-check) and verify it
   still passes.

**Decision:**
- All tests pass + ASAN clean: **green**
- Some tests fail at small logn but pass at deployment-relevant 9, 10:
  **orange**, document and proceed (similar to Path B's logn=2 caveat)
- Tests fail at logn=9 or 10: **red**, kill (a phase 1 reduction that
  breaks deployment is worse than no reduction)

**Time:** 1 day.

## Day 6: Validation — peak measurement

**Goal:** Confirm the user-visible tmp[] reduction actually materializes.

**Tasks:**

1. Reduce `signtmp_len` in `test_fndsa.c` to the new minimum (45n+31).
2. Run tests. Expect them to pass cleanly (no infinite retry loop like
   we hit on Day 10 of the Path B kill plan).
3. Add a `test_phase1.c` analogous to `test_path_b.c` that paints the
   region beyond the new phase 1 peak and verifies it stays untouched.

**Output:** `c_phase1_validation.md` with empirical peak numbers.

**Decision:**
- tmp_len works at 45n+31, no retries: **green**
- Hangs / retries / ASAN errors: **red**, debug or kill

**Time:** 1 day.

## Day 7: Bench

**Goal:** Quantify the perf cost of the new phase 1.

**Tasks:**

1. Update `bench_path_b.c` (or write a new bench) to compare:
   - baseline (`FNDSA_PATH_B=0, FNDSA_PHASE1_REDUCED=0`)
   - Path B alone (`FNDSA_PATH_B=1, FNDSA_PHASE1_REDUCED=0`)
   - Path B + phase 1 (both =1)
2. Document the perf delta. Phase 1 reduction may cost more than Path B's
   1-2% if the chosen option involves recomputation or extra FFTs.

**Decision:**
- Combined perf cost ≤ 5%: **green**
- Combined cost 5-15%: **orange**, document for deployment decision
- Cost >15%: **orange/red**, deployment may not accept; consult user

**Time:** 1 day.

## Day 8: Compose with `FNDSA_PATH_B`

**Goal:** Verify the new flag composes cleanly with existing Path B.

**Tasks:**

1. Test all 4 flag combinations (PATH_B and PHASE1_REDUCED on/off).
2. Verify byte-identical baseline build (both flags off).
3. Verify Path B alone still produces 51n+31 budget.
4. Verify Path B + Phase 1 produces ~45n+31 budget.
5. Run `test_path_b` and (new) `test_phase1` in all relevant configs.

**Decision:**
- All 4 configs work as expected: **green**
- Conflict between the two flags: investigate and fix

**Time:** 1 day.

## Day 9: Polish

**Goal:** Get the patch into upstream-PR-ready state.

**Tasks:**

1. Audit comments: each change references its design rationale, no
   internal kill-plan doc references.
2. Update `inner.h`'s `FNDSA_PHASE1_REDUCED` docstring with the design,
   limitations, perf cost.
3. Update `fndsa.h` public docstring to document the further-reduced
   budget.
4. Update `test_fndsa.c` if any new logn-specific edge cases emerged.
5. Final ASAN run.

**Output:** Clean diff ready to commit.

**Time:** 1 day.

## Day 10: Upstream PR

**Goal:** Submit the patch.

**Tasks:**

1. Create branch off `path-b-tmp-reduction`: `path-b-phase1-reduction`.
2. Commit with Pornin-style message (see `path-b-tmp-reduction`'s commit
   for the template).
3. Push to fork.
4. Open PR description (analogous to [PATH_B_PR.md](PATH_B_PR.md)) with:
   - Summary of additional savings (3/6 KiB)
   - Why this is layered on top of Path B (not a replacement)
   - The chosen Day 1 option's rationale
   - Validation results
   - Backwards compat (default off)
5. If Path B PR has not yet been merged, document this PR as stacked
   on it.

**Time:** 1 day.

## Total: 10 working days

Same shape as the Path B kill plan, with a similar confidence-vs-effort
profile. The harder part is Day 1's option survey — Path B's algorithmic
move (t1·l10 absorption) was a clear single insight; phase 1 reduction
may not have an equivalent clean win.

## Three risks worth flagging upfront

### Risk 1: All Day 1 options may be unsatisfactory

The gram matrix's 4-poly dependency is structural. The cleanest option
(precomputed basis as separate buffer) requires an API change. The
in-tmp options (streaming basis, on-demand recomputation, half-size gram)
either don't reduce peak or trade peak for compute that we may not be
willing to spend on SE-class hardware.

**Kill criterion:** If by end of Day 2 no option has all of (savings ≥ 1n,
effort ≤ 5 days, no API change), kill the project and document.

### Risk 2: Composing two flags

Adding `FNDSA_PHASE1_REDUCED` on top of `FNDSA_PATH_B` doubles the
configuration matrix. Pornin may prefer a single combined flag (e.g.
`FNDSA_TIGHT_TMP=2` for Path B + phase 1, vs `=1` for Path B alone). Day
1-2 should propose a flag scheme that's easy for upstream to maintain.

**Mitigation:** consult Pornin's preference before committing to a flag
naming scheme. Alternatively, bundle phase 1 into Path B as a refinement
(both gated by `FNDSA_PATH_B`).

### Risk 3: Perf cost compounds

If the chosen Day 1 option costs 5% perf on top of Path B's 1-2%, the
combined overhead is 6-7%. For SE-class targets where signing latency
is rarely the bottleneck this is fine; for high-throughput servers it
may not be acceptable. Day 7's bench is the load-bearing measurement.

**Mitigation:** if perf cost is too high, consider gating phase 1 reduction
behind a separate flag from Path B so users can pick their tradeoff.

## What to bring forward from the Path B kill plan

The Path B kill plan's strongest pattern was the **feasibility probe
test** ([test_path_b.c](test_path_b.c)) that gave a concrete go/no-go
signal at every step. The phase 1 plan should write `test_phase1.c` early
(Day 1-2) so each implementation iteration has the same clean validation
criterion: "does it write outside the new claimed footprint?"

Skip the recursive cascade test concept (it was invalid for Path B
because the parent's setup writes overlap with the painted region;
phase 1 may have similar issues). Stick to direct-call tests at each
logn.

## Status of `fpoly_pathb_finalize` if this kill plan succeeds

The recursive Path B primitive is already in Pornin's tree under
`FNDSA_PATH_B`. Its function-internal 5.25n FLR ffsamp peak is the
**second** binding constraint after phase 1. Once phase 1 drops to 5n,
the next binding constraint moves down to ffsamp's 5.25n. So phase 1
reduction *also* exposes the previously-dormant savings from the
primitive.

Specifically:
- Phase 1 = 5n FLR, ffsamp = 5.25n FLR → max = 5.25n → tmp[] = 5.25n + 3n + 31 ≈ 8.25n + 31 bytes? No wait, the formula is `peak_flr * 8 bytes/flr + 3n bytes hm/G + 31 bytes alignment`. So:
- Max ffsamp/phase1 in FLR = 5.25n
- In bytes: 5.25n × 8 = 42n bytes
- Plus hm 2n + G n = 45n + 31 bytes total

So phase 1 reduction to 5n FLR brings the OVERALL peak (max of
phase 1 and ffsamp) down to 5.25n FLR, materializing the recursive Path B
savings simultaneously. **Path B + Phase 1 reduction together = 45n+31.**

If phase 1 can be reduced further to 4n FLR, ffsamp's 5.25n is the new
constraint, and tmp[] = 5.25n FLR + 3n bytes + 31 = ~45n+31 still.
Going below requires *also* reducing ffsamp further (e.g. by tightening
the recursive Path B further toward its 5.0n asymptote).

The natural next-next kill plan after this one is recursive Path B's
caller-side allocation tightening to push ffsamp from 5.25n to 5.0n.
That gives another ~0.5n savings = ~4 KiB at logn=10. Diminishing returns.

## Bottom line

If this kill plan succeeds, **deployment story for Falcon-512** becomes:
~23 KiB tmp[] = comfortable margin in any SE-class budget, including
tight ST33K1M5 variants and ST33K1M0-class chips.

**Falcon-1024** becomes ~46 KiB, still over ST33K1M5's ~40 KiB
application budget but close enough that flash-spill of a small slice
(e.g. 8 KiB) closes the gap. That's the eventual deployment plan.

If this kill plan fails (red on Day 2 or Day 3), the deployment story
remains: Path B's 51n+31 budget. Falcon-512 fits with comfortable
margin; Falcon-1024 needs flash-spill or external-buffer API for the
basis.
