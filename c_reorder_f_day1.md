---
name: Day 1 — Reorder F C sketch + rationale (with Day 1.5 erratum)
description: C-side kill plan Day 1 finding. Reorder F is bit-exact and implementable but does NOT reduce peak in C — the only true 1n algorithmic peak win is t0 + t1·l10 absorption (Day 1.5 addendum), which costs bit-exactness with current KATs.
---

# Day 1 — Reorder F C sketch (c-fn-dsa, KILL_PLAN.md)

Date: 2026-05-01. Repo at HEAD `33026d4`. Source-only audit, no compilation, no benchmarks. Updated 2026-05-01 with Day 1.5 erratum and addendum (see below).

Day 0 was completed as the prior conversation's evidence sweep. Day 1's deliverables per the kill plan: a code diff in [reorder_f_c_sketch.diff](reorder_f_c_sketch.diff) and this rationale.

---

## Day 1 verdict: ORANGE — investigate

**Reorder F is implementable in C and bit-exact-equivalent on paper. But it does not reduce peak RAM in C: the 7n function-wide peak is set by the first-recursion frame, which Reorder F runs *after*. A subsequent erratum review (see "What it would take" section) confirmed that combining Reorder F with a left-subtree reshuffle also does not reduce peak — neither operation touches the binding constraint.**

The plan's Day 1 decision criteria were:
- *"Compiles cleanly in your head, savings track 1n: green"*
- *"Implementation has subtle issues (alignment, FFT-domain interactions): orange — investigate"*

The implementation itself is clean (the leaf precedent generalizes line-for-line). The "savings track 1n" half is what fails. Hence orange, not green.

The only realistic algorithmic 1n peak reduction in C identified so far is **t0 + t1·l10 absorption with tight repack** — see the Day 1.5 addendum below. It saves 1n on peak (28q → 24q = 6n) but breaks bit-exactness with Pornin's existing KATs because the FP operation order changes (the result is distribution-equivalent and meets the FN-DSA spec, just not byte-identical).

This is structurally analogous to the Rust audit's caveat about the 9n vs 7n accounting difference ([rust-fn-dsa/kill_plan_notes.md:94-102](../rust-fn-dsa/kill_plan_notes.md#L94-L102)) — Reorder F savings in C are an accounting nuance that the email pitch needs to handle carefully.

---

## What Reorder F looks like in C

Target sequence: [sign_sampler.c:1281-1299](sign_sampler.c#L1281-L1299).

```c
fpoly_split_fft (logn, qc(14), qc(16), qc(4));        // split t1 into split-form (qc(14..15), qc(16..17))
ffsamp_fft_inner(ss, logn - 1, qc(14));               // recurse on right subtree, output overwrites qc(14..17)
fpoly_merge_fft (logn, qc(18), qc(14), qc(16));       // *** materializes z1 at qc(18..21) — 4 quarters = n FLR ***

memcpy   (qc(14), qc(4),  sizeof(fpr) << logn);       // qc(14..17) := t1
fpoly_sub(logn,   qc(14), qc(18));                    // qc(14..17) := t1 - z1
memcpy   (qc(4),  qc(18), sizeof(fpr) << logn);       // qc(4..7)   := z1   (move z1 into t1)
fpoly_mul_fft(logn, qc(14), qc(8));                   // qc(14..17) := (t1-z1) * l10
fpoly_add    (logn, qc(0),  qc(14));                  // qc(0..3)   := t0 + (t1-z1) * l10  (= tb0)
```

Reorder F replaces the merge + 5 sequential ops with a single fused loop, mirroring the leaf-level recurrence at [sign_sampler.c:1004-1018](sign_sampler.c#L1004-L1018). The merge twiddle math (s_re, s_im from `GM[2*(i+hn)..]`) is taken straight from [sign_fpoly.c:2444-2449](sign_fpoly.c#L2444-L2449) and inlined.

Per outer-loop iteration on `i ∈ [0, qn)` where `hn = n/2`, `qn = n/4`:

```
Read:   z1_low_re, z1_low_im   ← qc(14)[i], qc(14)[i+qn]      (callee t0 output)
        z1_high_re, z1_high_im ← qc(16)[i], qc(16)[i+qn]      (callee t1 output)
        s_re, s_im             ← GM[(i+hn)<<1], GM[(i+hn)<<1+1]
        t0_re*4, t0_im*4       ← qc(0)[2i..2i+1], qc(0)[2i+hn..2i+1+hn]
        t1_re*4, t1_im*4       ← qc(4)[2i..2i+1], qc(4)[2i+hn..2i+1+hn]
        l10_re*4, l10_im*4     ← qc(8)[2i..2i+1], qc(8)[2i+hn..2i+1+hn]

Compute (registers, no spill):
        c     = z1_high * s            (complex mul)
        z1[2i]   = z1_low + c          (merge formula)
        z1[2i+1] = z1_low - c
        d     = t1 - z1                (complex sub)
        p     = d * l10                (complex mul, FFT-domain)
        tb0   = t0 + p                 (complex add)

Write:  qc(0)[...] ← tb0   (overwrites t0)
        qc(4)[...] ← z1    (overwrites t1)
```

No write to `qc(18..21)` ever happens. This eliminates the merge-moment working-set blip from 22q to 18q (a 4q = 1n FLR transient saving).

---

## Why "1n" doesn't reach the peak

I traced the live footprint at every step of `ffsamp_fft_inner` at the outer level. Quarter sizes throughout assume outer `logn` (each quarter = n/4 FLR).

| Step | qc(0..3) | qc(4..7) | qc(8..11) | qc(12..13) | qc(14..27) live | Total |
|---|---|---|---|---|---|---|
| Entry | t0 (4) | t1 (4) | g01 (4) | g00 (2) | g11 at 14..15 (2) | **16q = 4n** |
| After LDL [1252](sign_sampler.c#L1252) | t0 | t1 | l10 | d00 | d11 at 14..15 | 16q = 4n |
| After split_selfadj [1264](sign_sampler.c#L1264) | t0 | t1 | l10 | d00 | right_01/00/11 at 18..21 | 18q = 4.5n |
| After split_fft [1281](sign_sampler.c#L1281) | t0 | t1 | l10 | d00 | t1_split at 14..17 + right at 18..21 | 22q = 5.5n |
| **During recursion [1282](sign_sampler.c#L1282)** | t0 | t1 | l10 | d00 | **callee scratch (14)** | **28q = 7n  ← PEAK** |
| After merge [1283](sign_sampler.c#L1283) | t0 | t1 | l10 | d00 | z1 at 18..21 | 18q = 4.5n |
| After lines 1295-1299 | tb0 | z1 | (dead) | d00 | (dead) | 10q = 2.5n |
| After split_selfadj [1309](sign_sampler.c#L1309) | tb0 | z1 | (dead) | (dead) | left_01/00/11 at 18..21 | 12q = 3n |
| **During recursion [1325](sign_sampler.c#L1325)** | (dead) | z1 | (dead) | (dead) | callee scratch (14) | 18q = 4.5n |

The peak is **the first recursion**. It is 14 quarters of passive set (t0, t1, l10, d00) plus 14 quarters of recursion scratch — 28 quarters total = 7n. The recursion-scratch size is locked: `ffsamp_fft_inner` at `logn-1` needs `7·(n/2) = 3.5n` FLR = 14 outer-quarters, no less.

Reorder F operates **after** the first recursion returns. It cannot change what happens during it.

The 1n savings Reorder F delivers is at the merge moment (22q → 18q transient), not at the function peak (28q, unchanged).

---

## What it would take to make Reorder F save 1n on peak

**Erratum (added during Day 1.5 review):** an earlier version of this section claimed that combining Reorder F with a left-subtree reshuffle (qc(18..21) → qc(8..11)) would yield 1n peak savings. That claim is wrong. A careful trace shows the reshuffle changes only the *second* recursion's footprint, not the function peak.

The reasoning: the function peak is at the *first* recursion ([sign_sampler.c:1282](sign_sampler.c#L1282)), where all four persistent buffers (t0, t1, l10, d00 = 14q) are live and the callee scratch at qc(14..27) consumes the remaining 14q. Reorder F runs *after* the first recursion. Reshuffling where left-subtree storage lives runs *after* the first recursion. Neither touches the 14q+14q=28q binding constraint.

Tracing the reshuffle concretely. Modify [sign_sampler.c:1309](sign_sampler.c#L1309) to write left-subtree to qc(8..11) instead of qc(18..21):

```c
fpoly_split_selfadj_fft(logn, qc(10), qc(8),  qc(12));   // was qc(20), qc(18)
memcpy(qc(11), qc(10), sizeof(fpr) << (logn - 2));       // was qc(21), qc(20)
```

Then the second recursion at [1325](sign_sampler.c#L1325) needs `tmp = qc(4)` (so its `qc(8..11), qc(12..13), qc(14..15)` map to outer `qc(8..9), qc(10), qc(11)` — the new left-subtree positions). But `qc(4..7)` already holds z1 (the function's output), so z1 has to be moved to qc(18..21) and back. Cost: 2 memcpys × n FLR per call.

Live-set comparison at second recursion:

| | Before reshuffle | After reshuffle |
|---|---|---|
| Persistent | z1 at qc(4..7) (4q) | z1 at qc(18..21) (4q) |
| Callee scratch | qc(14..27) (14q) | qc(4..17) (14q) |
| **Live** | **18q** | **18q** |
| Allocated | 28q | 28q |

Same live count, same allocation. The reshuffle moves where things sit; it doesn't shrink anything.

**To actually reduce the peak, you have to attack the first recursion's 14q persistent set.** The reshuffle and Reorder F operate on the wrong side of the function for that. The realistic angle is the t0 + t1·l10 absorption (drops persistent from 14q to 10q, total peak 28q → 24q = 6n, saves 1n), at the cost of FP-order changes that break bit-exact KAT compatibility — see *Day 1.5 addendum* below.

---

## What Reorder F *does* deliver in C

1. **Cleanliness.** Generalizes a fusion pattern Pornin already ships at logn=1 ([sign_sampler.c:1004-1018](sign_sampler.c#L1004-L1018), [:1074-1088](sign_sampler.c#L1074-L1088)) to the recursive case. Reduces line count and eliminates a redundant materialization.
2. **Composes with flash-spill more cleanly.** The spill design needs a clean "passive interval" boundary; Reorder F removes one of the muddier intermediate states (the qc(18..21) staging that exists only for the merge → consume sequence).
3. **Cache pressure.** The fused loop touches z1 elements once instead of writing them to qc(18..21) and reading them back across two passes. On Cortex-M35P with small data caches, this matters (though it's not a memory-budget claim).
4. **Bit-exact equivalence is provable from the leaf precedent.** The fusion is the same arithmetic applied at a different recursion level; the IEEE-754 rounding is identical because each output bit comes from the same multiply-add tree.

These are real wins. None of them justify "Reorder F saves 1n on C peak" in the email pitch.

---

## Implications for the email pitch

The email pitch as written claims:

> Current C: 60 KiB / C + Reorder F: ~52 KiB / C + Reorder F + spill: ~36 KiB

Per Day 1's findings, this should be:

> Current C: 60 KiB / C + Reorder F: **~60 KiB (peak unchanged, working-set transient saved)** / C + Reorder F + spill: ~32–36 KiB

The flash-spill of the 3.5n passive set is what does the heavy lifting in C. Reorder F is enabling/cleanup, not a peak-reducer. **The pitch's deployment-fit number is determined by spill alone.**

Alternatively, if the pitch wants to claim a 1n algorithmic saving, the only honest route is the t0 + t1·l10 absorption (Day 1.5 addendum below) — and that requires accepting KAT regeneration because the FP operation order changes.

---

## Recommended Day 2 path

Per the kill plan's orange decision rule: *"Implementation has subtle issues … investigate"*. Two paths:

**Path A — Implement Reorder F as-is, accept the orange finding:**
1. Apply [reorder_f_c_sketch.diff](reorder_f_c_sketch.diff) (scalar path only — Cortex-M35P relevant)
2. Run test vectors → expect bit-exact pass (Day 3-4)
3. Measure peak → expect *unchanged* at 7n (Day 5)
4. Update kill plan to drop the "C + Reorder F: ~52 KiB" intermediate row; report final number as `C + spill = 60 KiB - 3.5n × spilled = ~32 KiB` at logn=10
5. Continue to Day 6+ (spill validation) — this is the load-bearing work in C, not Reorder F

**Path B — Pursue t0 + t1·l10 absorption for a real 1n peak win:** (replaces the earlier "Path B — bundle with layout reshuffle" which was based on the corrected erratum above)
1. Day 1.5: design the absorption + tight repack (sketch in addendum below)
2. Day 2: implement; expect ~3n FLR of memcpys per call from the repack
3. Day 3-4: KATs **will** diverge — investigate distribution equivalence (sample many signatures, compare statistical properties), regenerate KATs from the new FP order, document
4. Day 5: peak measurement — expect 24q = 6n
5. Days 6-8 (spill) compose on top: 6n − 3.5n = 2.5n peak achievable

Path A is what the kill plan's risk profile favors (smaller scope, faster to validation, bit-exact). Path B trades bit-exactness for a real 1n algorithmic peak reduction independent of flash-spill — only worth pursuing if the pitch positioning *requires* an algorithmic-only RAM win or if "spill at last resort" is a hard constraint. Recommend Path A by default.

---

## Day 1.5 addendum: algorithmic angles for peak reduction (no spill)

The kill plan's framing prefers spill as a last resort. After the Day 1 erratum showed Reorder F + reshuffle can't reduce peak, this addendum surveys the algorithmic options.

### The four candidates surveyed

| Angle | Δ peak | Bit-exact? | Cost | Verdict |
|---|---|---|---|---|
| Reorder F alone | 0 | ✅ | low | cleanup, not a reducer |
| Reorder F + reshuffle (qc(18..21)→qc(8..11)) | 0 | ✅ | + 2 memcpys | does not reduce peak — see erratum above |
| Pass d11 to recursion (callee splits internally) | 0 | ✅ | callee API change | helps a transient moment, not peak |
| **t0 + t1·l10 absorption + tight repack** | **−1n** | ❌ | ~3n memcpys/call, KAT regen | only true algorithmic 1n peak win in C |

### t0 + t1·l10 absorption — sketch

Original recurrence: `tb0 = t0 + (t1 - z1)·l10`. Algebraically equivalent: `c1 := t0 + t1·l10`, then `tb0 := c1 - z1·l10`.

The two are not bit-exact under IEEE-754 because the operation order changes (`(t1 - z1)·l10` then `+ t0` vs `(t0 + t1·l10) - z1·l10`). The result is *distribution-equivalent* — signatures verify under the FN-DSA spec — but won't match Pornin's existing KATs byte-for-byte.

If `c1` is computed in place at qc(0..3) before the right recursion and t1 is then dead, the persistent set across the right recursion drops:

```
Original persistent (across right recursion):  t0 + t1 + l10 + d00 = 4 + 4 + 4 + 2 = 14q
New persistent (across right recursion):       c1 +     l10 + d00 = 4     + 4 + 2 = 10q
```

But the saving only translates to peak if you *also* repack the layout so qc(4..7) (now dead) is folded into the callee scratch range. Required moves:

1. Compute `t1·l10 → scratch (qc(16..19))`, then `qc(0..3) := t0 + scratch` → `c1` ready
2. Move `l10` from qc(8..11) to scratch, then back into qc(4..7) after t1 is split
3. Move `d00` from qc(12..13) to qc(8..9)
4. Split d11 with output landing at the new callee positions (right_01 at qc(14..15), right_00 at qc(16), right_11 at qc(17))
5. Split t1 with output landing at qc(10..11), qc(12..13)
6. Recurse with `tmp = qc(10)` — callee uses qc(10..23) = 14q

Final peak layout:

```
qc(0..3)   c1       (persistent, 4q)
qc(4..7)   l10      (persistent, 4q)
qc(8..9)   d00      (persistent, 2q)
qc(10..23) callee   (recursion scratch, 14q including right-subtree input)
qc(24..27) free
```

Live during first recursion: 24q = **6n FLR**. Total tmp[] could shrink from 28q to 24q = 1n savings = **8 KiB at logn=10, 4 KiB at logn=9**.

### Costs

- **~3n FLR of memcpys per signature** for the layout repack (steps 2, 3 plus scratch round-trips). At logn=10 that's ~24 KiB of bus traffic per sign. Probably under 5% perf hit but should be measured.
- **KAT regeneration.** Distribution equivalence is what the FN-DSA spec requires; bit-exactness is what Pornin's repo's existing test vectors require. Path B accepts the divergence and ships new KATs. The Rust audit notes called out the same kind of distinction in the "9n vs 7n accounting difference" caveat.
- **Symmetric work for the second recursion.** The same absorption may or may not help on the second-recursion side; needs its own trace.

### Compositional with flash-spill

If Path B works (peak = 6n), spilling the 2.5n persistent set during the right recursion brings live peak to 6n − 2.5n = 3.5n during the spill window. At logn=10: 28 KiB → 24 KiB → 14 KiB transient peak. Whether that's worth the engineering depends on the deployment story.

### Recommendation

If the deployment story permits new KATs (it should — distribution equivalence is the spec-level property), Path B is worth a Day 1.5 prototype. The win is real and stands without flash-spill. If the deployment story requires byte-exact compat with Pornin's reference, Path B is closed and spill is the only path to <50 KiB.

---

## Cross-check against Rust Day 1

The Rust audit at [rust-fn-dsa/kill_plan_notes.md:50-92](../rust-fn-dsa/kill_plan_notes.md#L50-L92) found ffsamp peak at 9n in Rust. The 2n difference vs C's 7n comes from:

| Buffer | Rust | C |
|---|---|---|
| g00 | n FLR (full-size in FFT) | n/2 FLR (self-adjoint, half-size at qc(12..13)) |
| g11 | n FLR | n/2 FLR (half-size at qc(14..15)) |
| | | |
| Net | 2n more than C | 1n less, persistent |

So C's 7n is *already* a tighter baseline because Pornin exploits self-adjointness. The Rust streaming target was 9n → 5n (4n savings). The C equivalent is 7n → 7n - 3.5n = 3.5n if the spill works, plus or minus the Reorder F nuance. **C ends up tighter than Rust either way** — which is consistent with Pornin's reference being designed for the deployment context the kill plan is aiming at.
