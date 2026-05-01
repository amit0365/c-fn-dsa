---
name: Path B layout walkthrough — t0 + t1·l10 absorption + tight repack
description: Step-by-step buffer-state transition for the Path B refactor of ffsamp_fft_inner in c-fn-dsa. Goal: 7n → 6n peak via algorithmic absorption (no flash-spill).
---

# Path B layout walkthrough

Day 2 deliverable per [KILL_PLAN.md](KILL_PLAN.md). Companion to the planned `path_b_sketch.diff`.

The aim is to transform [sign_sampler.c:1252-1326](sign_sampler.c#L1252-L1326) so that:
- The first recursion's persistent set drops from 14q to 10q (c1 + l10 + d00 instead of t0 + t1 + l10 + d00).
- The total tmp[] allocation drops from 28q (7n FLR) to 24q (6n FLR).
- The recursion is invoked with `tmp = qc(10)` instead of `tmp = qc(14)`, so callee scratch fits the tighter envelope.

Each "quarter" is `n/4` FLR at the outer logn. q=quarter, qc(N)=offset N quarters into tmp.

---

## Notation

```
[ ----t0---- ][ ----t1---- ][ ----g01--- ][-g00-][-g11-][--free space-------]
0  1  2  3    4  5  6  7    8  9 10 11   12 13  14 15  16 17 18 19 20 21 22 23 24 25 26 27
```

- A "wide block" `----xxx----` is 4 quarters = n FLR (full-degree FFT polynomial)
- A narrow block `-xxx-` is 2 quarters = n/2 FLR (self-adjoint half-storage)
- `_` = dead, `?` = scratch in use

After every step I'll show:
- *Live* buffers with their slot
- *Dead* slots (allocated but the data is no longer needed)
- *Scratch* (transient use within the step)

---

## Step 0 — Function entry (unchanged from baseline)

```
qc      0..3   4..7   8..11  12..13  14..15  16..27
state   t0     t1     g01    g00     g11     free
size    n      n      n      n/2     n/2     3n
```

Live: t0, t1, g01, g00, g11. Total live: 16q = 4n. Allocated: 28q.

This is what the caller hands us. Same as [sign_sampler.c:1240-1245](sign_sampler.c#L1240-L1245).

---

## Step 1 — LDL decomposition (unchanged from baseline)

```c
fpoly_LDL_fft(logn, qc(12), qc(8), qc(14));   // same as sign_sampler.c:1252
// In-place: g01 → l10 at qc(8..11); g11 → d11 at qc(14..15); g00 unchanged → d00 at qc(12..13)
```

```
qc      0..3   4..7   8..11  12..13  14..15  16..27
state   t0     t1     l10    d00     d11     free
```

Live: 16q. Identical footprint to baseline; we haven't diverged yet.

---

## Step 2 — Compute t1·l10 → scratch at qc(16..19)

```c
memcpy(qc(16), qc(4), sizeof(fpr) << logn);       // qc(16..19) := t1
fpoly_mul_fft(logn, qc(16), qc(8));               // qc(16..19) := t1 · l10
```

```
qc      0..3   4..7   8..11  12..13  14..15  16..19      20..27
state   t0     t1     l10    d00     d11     t1·l10      free
size    n      n      n      n/2     n/2     n           2n
```

Live: 16q + 4q = 20q. **Peak so far: 20q = 5n.** (Baseline peak at this point: ~16q + ε. We're 4q higher transiently.)

This is a transient spike — the t1·l10 product is consumed in the next step.

---

## Step 3 — Absorb into t0: c1 := t0 + t1·l10

```c
fpoly_add(logn, qc(0), qc(16));   // qc(0..3) := t0 + t1·l10 = c1
// qc(16..19) is now dead
```

```
qc      0..3   4..7   8..11  12..13  14..15  16..19      20..27
state   c1     t1     l10    d00     d11     dead        free
```

Live: 16q. The transient spike is gone; we've turned t0 into c1 in place.

**Algebraic note:** the original recurrence is `tb0 = t0 + (t1 - z1)·l10`. We're computing the `t0 + t1·l10` half of it now (before recursion), and will subtract `z1·l10` after the recursion. The IEEE-754 reordering is real and is what causes KAT divergence (see [c_reorder_f_day1.md](c_reorder_f_day1.md) Day 1.5 addendum).

---

## Step 4 — Split t1 (split is non-destructive in source)

The original code at [sign_sampler.c:1281](sign_sampler.c#L1281) writes the split to qc(14..15) and qc(16..17). In Path B we want the split outputs at qc(10..11) and qc(12..13) (the callee's t0/t1 positions when `tmp = qc(10)`). But qc(10..11) is currently the second half of l10, and qc(12..13) is d00 — both still live.

We have to move l10 and d00 first. But l10 is at qc(8..11) and t1 is at qc(4..7) — moving l10 to qc(4..7) requires destroying t1, which we still need to split.

**Order:** split first (uses t1 at qc(4..7)), then move l10. This means the split has to write to free scratch, and l10 has to be moved out of qc(8..11) before the split's outputs land there.

Re-ordered Step 4:
1. **Move l10 out** to scratch at qc(20..23): `memcpy(qc(20), qc(8), n)`. Now qc(8..11) is free.
2. **Move d00 out** to scratch at qc(24..25): `memcpy(qc(24), qc(12), n/2)`. Now qc(12..13) is free.
3. **Split t1 with new output positions:** `fpoly_split_fft(logn, qc(10), qc(12), qc(4))`. Writes f0 to qc(10..11), f1 to qc(12..13). Reads t1 from qc(4..7) (unchanged).

After Step 4:

```
qc      0..3   4..7   8..9   10..11   12..13   14..15   16..19   20..23   24..25   26..27
state   c1     t1*    free   ce_t0    ce_t1    d11      free     l10      d00      free
```

(`t1*` = stale, `ce_t0` = callee_t0 = split-low of t1, `ce_t1` = callee_t1 = split-high of t1)

Live: c1(4) + ce_t0(2) + ce_t1(2) + d11(2) + l10(4) + d00(2) = 16q. t1 at qc(4..7) is now logically dead (split is done).

---

## Step 5 — Split d11 with new output positions

The callee expects `g01, g00, g11` at its `qc(8..11), qc(12..13), qc(14..15)`. With `tmp = qc(10)`, those map to outer `qc(14..15), qc(16), qc(17)`.

```c
fpoly_split_selfadj_fft(logn, qc(16), qc(14), qc(14));   // *** note: input/output overlap on qc(14)
memcpy(qc(17), qc(16), sizeof(fpr) << (logn - 2));        // right_11 := right_00
```

**Aliasing concern:** d11 is at qc(14..15) (input), right_01 output goes to qc(14..15) (overwrites in place), right_00 output goes to qc(16). Looking at [sign_fpoly.c:2289-2308](sign_fpoly.c#L2289-L2308), `fpoly_split_selfadj_fft` reads each `ab_re` from `f` then writes `f0[i]` and `f1[i]` to disjoint addresses. The loop is `for i in 0..qn` and reads `f[2i..2i+1]`. The output `f1[i]` at qc(14..15) shares storage with the input. Need to verify the loop reads each input position before writing. **Open question for Day 2 implementation; may need an intermediate scratch.**

After Step 5 (assuming aliasing works or an intermediate is used):

```
qc      0..3   4..7   8..9   10..11   12..13   14..15   16     17     18..19   20..23   24..25   26..27
state   c1     dead   free   ce_t0    ce_t1    rt_01    rt_00  rt_11  free     l10      d00      free
```

Live: c1(4) + ce_t0(2) + ce_t1(2) + rt_01(2) + rt_00(1) + rt_11(1) + l10(4) + d00(2) = 18q.

---

## Step 6 — Pack l10 and d00 into final positions

```c
memcpy(qc(4), qc(20), sizeof(fpr) << logn);       // l10: scratch → qc(4..7)
memcpy(qc(8), qc(24), sizeof(fpr) << (logn - 1)); // d00: scratch → qc(8..9)
```

After Step 6:

```
qc      0..3   4..7   8..9   10..11   12..13   14..15   16     17     18..27
state   c1     l10    d00    ce_t0    ce_t1    rt_01    rt_00  rt_11  free
```

Live: 18q. *All dead/scratch slots compacted away.* The persistent set (c1, l10, d00) at qc(0..9) = 10q. The callee's input layout at qc(10..17) = 8q, plus callee scratch at qc(18..23) = 6q.

**Allocated tmp[] requirement so far:** qc(0..23) = 24q = **6n FLR**. qc(24..27) is unused at this point. ✓

---

## Step 7 — First recursive call

```c
ffsamp_fft_inner(ss, logn - 1, qc(10));  // was qc(14)
```

Callee uses qc(10..23) = 14 outer-quarters = 7·(n/2) inner FLR. Inside callee:
- callee's qc(0..3)  = outer qc(10..11) = ce_t0 ✓
- callee's qc(4..7)  = outer qc(12..13) = ce_t1 ✓
- callee's qc(8..11) = outer qc(14..15) = rt_01 (callee's g01) ✓
- callee's qc(12..13)= outer qc(16)     = rt_00 (callee's g00) ✓
- callee's qc(14..15)= outer qc(17)     = rt_11 (callee's g11) ✓
- callee's qc(16..27)= outer qc(18..23) = free scratch (12 callee-q = 1.5n outer FLR) ✓

**Live during recursion:**
- Persistent: c1(4q) + l10(4q) + d00(2q) = 10q
- Callee window: 14q (all in use as scratch)
- **Total live: 24q = 6n FLR.** ← PEAK

Compared to baseline peak of 28q = 7n: **saves 4q = 1n FLR.** ✓

---

## Step 8 — Post-recursion: compute tb0 := c1 - z1·l10

After recursion returns, qc(10..11) holds z1_split_low and qc(12..13) holds z1_split_high (the callee's t0, t1 outputs). We need:
1. Merge z1 from split form
2. Multiply by l10
3. Subtract from c1, write to tb0

If we layer Reorder F here (the streaming fusion from [reorder_f_c_sketch.diff](reorder_f_c_sketch.diff), adapted to the new layout), the merge + mul + sub can be fused in a single loop:

```c
/* Reorder F-style fused: tb0 = c1 - z1·l10, where z1 is merged on the fly
   from (qc(10), qc(12)). Output written to qc(0..3) replacing c1, and z1
   written into qc(4..7) replacing l10 (which is now dead). */
{
    size_t hn = (size_t)1 << (logn - 1);
    size_t qn = hn >> 1;
    fpr *out_tb0 = qc(0);
    fpr *out_z1  = qc(4);  /* overwrites l10 */
    const fpr *l10 = qc(4); /* read before write — same buffer, sequential per-element use */
    const fpr *zlow  = qc(10);
    const fpr *zhigh = qc(12);
    /* per-iteration: same merge math + mul + sub fusion as Reorder F */
    /* CRITICAL: read l10[2i..] BEFORE writing out_z1[2i..] within iteration */
    /* ... (see reorder_f_c_sketch.diff for the per-element body, adapted) */
}
```

After Step 8:

```
qc      0..3   4..7   8..9   10..27
state   tb0    z1     d00    free/dead
```

Live: tb0(4) + z1(4) + d00(2) = 10q. (Previous live during recursion was 24q; now we're back down.)

---

## Step 9 — Second recursion (symmetric repack)

The second recursion needs: callee_t0, callee_t1 (split of tb0), and (left_01, left_00, left_11) from splitting d00.

If we apply the same packing, callee tmp = qc(?). z1 must be preserved (it's the function's output). Where does z1 live?

z1 is currently at qc(4..7). For the second recursion to use the tight window, z1 needs to be moved. Since we have qc(10..27) free (18q), there's plenty of room. Move z1 to qc(20..23), say:

```c
memcpy(qc(20), qc(4), sizeof(fpr) << logn);   // z1 → qc(20..23) (preserve)
```

Now apply analogous Path B repack for the second recursion: split d00 to produce left_01/00/11; split tb0 to produce callee_t0/t1; recurse with `tmp = ?`.

The second recursion's persistent set is much smaller (just z1 = 4q), so the layout pressure is lower than the first recursion. We could use a similar pattern but it's not strictly required for the peak — the first recursion is the binding peak. As long as the second recursion's footprint stays ≤ 24q, we're fine.

**Simplification:** for the second recursion, we don't NEED to use the tight pack — original layout works as long as the total stays under our 24q ceiling. The second recursion's original peak is 18q live + ε ≈ 22q ≤ 24q ✓.

Actually re-checking: with z1 moved to qc(20..23), the original second-recursion layout (tb0 at qc(0..3), free at qc(4..13), d00 at qc(8..9)) doesn't quite fit. Need to think this through carefully.

**Open question for Day 2:** is the second-recursion layout an issue, or does it accommodate the 24q ceiling automatically? Quick sketch: if we keep the original placements but with z1 living at qc(20..23) instead of qc(4..7):

- qc(0..3): tb0 → consumed by split → dead
- qc(4..7): split outputs (callee_t0, callee_t1) at qc(4..5), qc(6..7) (with `tmp = qc(4)` for the second recursion)... but qc(8..9) is d00 — collision again.

Same issue as the first recursion. Solution: move d00 out of qc(8..9) before splitting tb0. d00 is consumed by split_selfadj at this point so we can write its output to wherever — say keep it at qc(8..11) and have callee scratch start later.

Detailed second-recursion layout still needs working out; flagging as Day 2 follow-up.

---

## Memcpy budget (per signature)

Counting moves added by Path B:

| Step | Operation | Size | Cost |
|---|---|---|---|
| 2 | t1 → qc(16..19) for mul | n FLR | 8 KiB at logn=10 |
| 4.1 | l10 → scratch qc(20..23) | n FLR | 8 KiB |
| 4.2 | d00 → scratch qc(24..25) | n/2 FLR | 4 KiB |
| 6.1 | l10: scratch → qc(4..7) | n FLR | 8 KiB |
| 6.2 | d00: scratch → qc(8..9) | n/2 FLR | 4 KiB |
| 9 | z1 → qc(20..23) for 2nd recursion | n FLR | 8 KiB |
| **Total** | | **5n FLR** | **40 KiB** at logn=10 |

That's ~5n FLR of memcpys per signing, not 3n as estimated earlier. Per-signing perf overhead at logn=10: 40 KiB of bus traffic plus the inherent algorithm cost. On Cortex-M35P, RAM throughput is ~50–100 MB/s; 40 KiB = ~0.5 ms additional latency. Algorithm latency for FN-DSA-1024 signing is order ~10 ms, so this is ~5% perf overhead. **Probably acceptable but should be measured.**

If perf overhead is too high, several memcpys can be elided by being clever about which scratch slot l10 lives in (e.g. compute t1·l10 directly at qc(20..23), then never move l10 back — use it from qc(20..23) during the recursion). This is a Day 2.5 optimization if needed.

---

## Open questions to resolve in Day 2 implementation

1. ~~**Aliasing in step 5**~~ → **resolved (see addendum below):** `fpoly_split_selfadj_fft` does NOT tolerate `f1 == f`. Workaround adds 1 memcpy.
2. ~~**Second recursion layout**~~ → **resolved (see addendum below):** layout works cleanly with no additional memcpys; z1 stays at qc(4..7), no relocation needed.
3. **Is the absorption legal at all logn?** The leaf at logn=1 already has its own fused recurrence; Path B is for logn ≥ 2 (the recursive case). Should compose with the existing leaf, but verify.
4. **Distribution equivalence** — chi-squared test plan deferred to Day 3-4.

---

## Addendum 1: aliasing analysis for `fpoly_split_selfadj_fft`

**Verdict: f1 cannot alias f.** Step 5 as originally drafted (`fpoly_split_selfadj_fft(logn, qc(16), qc(14), qc(14))`) would corrupt input. Need a 2q scratch buffer.

Reading the SSE2 body at [sign_fpoly.c:2289-2308](sign_fpoly.c#L2289-L2308):

```c
for (size_t i = 0; i < qn; i ++) {
    __m128d ab_re = _mm_loadu_pd(ff + (i << 1));    // reads f[2i], f[2i+1]
    /* ... compute u, w ... */
    _mm_store_sd(ff0 + i, u);                        // writes f0[i]
    _mm_store_sd(ff0 + i + qn, _mm_setzero_pd());    // writes f0[i+qn]
    _mm_store_sd(ff1 + i, w);                        // writes f1[i]
    _mm_store_sd(ff1 + i + qn, _mm_shuffle_pd(w, w, 1)); // writes f1[i+qn]
}
```

If `f1 == f`, iteration `i` writes f1[i+qn] = f[i+qn]. Iteration `j > i` reads f[2j, 2j+1]. The collision: 2j = i+qn → i = 2j - qn. So for j ≥ qn/2, iteration j reads input f[2j] that was *overwritten* in iteration i = 2j - qn. **Corruption confirmed.**

Same analysis holds for NEON, RV64D, and scalar — all four variants have the same access pattern (writes to `f1[i+qn]` for i in 0..qn-1, reads from `f[0..hn-1]` for input, and qn = hn/2 so the high-half writes hit indices that later reads cover).

**Workaround for step 5:** save d11 to free scratch before splitting.

```c
/* Step 5 (revised): split d11 with output at callee positions, avoiding aliasing */
memcpy(qc(18), qc(14), sizeof(fpr) << (logn - 1));     // d11 → qc(18..19) scratch (2q)
fpoly_split_selfadj_fft(logn, qc(16), qc(14), qc(18)); // f0=qc(16), f1=qc(14..15), f=qc(18..19) — disjoint ✓
memcpy(qc(17), qc(16), sizeof(fpr) << (logn - 2));     // right_11 := right_00 (1q)
```

Highest tmp[] index touched: qc(19) — well under 24q ceiling. No peak impact.

Updated memcpy budget for first recursion:
- t1 → qc(16..19) for mul scratch: 4q
- l10 → scratch qc(20..23): 4q
- d00 → scratch (now needs different position to avoid clashing with l10): 2q
- **d11 → scratch qc(18..19) (NEW)**: 2q
- l10 → final qc(4..7): 4q
- d00 → final qc(8..9): 2q
- **Subtotal first recursion: 18q = 4.5n FLR** (was 16q before aliasing fix)

---

## Addendum 2: second recursion walkthrough

State at entry to second recursion (after Reorder F-style fused tb0 computation):

```
qc      0..3   4..7   8..9   10..23
state   tb0    z1     d00    dead
```

Live: 10q. Plenty of room.

**Step 9 — split d00 with output at callee positions:**
```c
fpoly_split_selfadj_fft(logn, qc(16), qc(14), qc(8));
memcpy(qc(17), qc(16), sizeof(fpr) << (logn - 2));
```
Aliasing: f=qc(8..9), f0=qc(16), f1=qc(14..15). All disjoint ✓ (no fix needed).

After step 9:
```
qc      0..3   4..7   8..9     10..13   14..15   16     17     18..23
state   tb0    z1     dead*    free     left_01  l_00   l_11   free
```
(*qc(8..9) is dead because d00 was the only thing living there, and split_selfadj reads d00 then we don't need it anymore)

**Step 10 — split tb0 with output at callee positions:**
```c
fpoly_split_fft(logn, qc(10), qc(12), qc(0));
```
Aliasing: f=qc(0..3), f0=qc(10..11), f1=qc(12..13). All disjoint ✓.

After step 10:
```
qc      0..3   4..7   8..9   10..11   12..13   14..15   16     17     18..23
state   tb0*   z1     dead   ce_t0    ce_t1    left_01  l_00   l_11   free
```
(*qc(0..3) is dead since split is non-destructive but tb0 not needed anymore)

**Step 11 — second recursion:**
```c
ffsamp_fft_inner(ss, logn - 1, qc(10));
```
Callee uses qc(10..23) = 14 outer-q. Live during recursion: z1(4q) + callee(14q) = **18q**. Allocated: 24q. Peak unchanged from baseline ceiling.

After recursion:
```
qc      0..3   4..7   8..9   10..11   12..13   14..23
state   dead   z1     dead   ce_t0    ce_t1    callee scratch (dead)
```
(callee_t0/t1 now hold z0_split_low and z0_split_high)

**Step 12 — final merge:**
```c
fpoly_merge_fft(logn, qc(0), qc(10), qc(12));
```
Aliasing: f=qc(0..3), f0=qc(10..11), f1=qc(12..13). All disjoint ✓.

Final state:
```
qc      0..3   4..7   8..9   10..23
state   z0     z1     dead   dead
```

Function exit: z0 at qc(0..3), z1 at qc(4..7). Matches the function's API contract — caller at [sign_core.c:213](sign_core.c#L213) reads `t0` and `t1` from `tmp[0..n]`, `tmp[n..2n]`. ✓

**No additional memcpys needed for the second recursion.** z1 stays at qc(4..7) the whole time. The second recursion is actually cleaner than the first because by then l10 is dead and there's no t1 to preserve.

---

## Revised memcpy budget (per signature)

| Step | Operation | Size | Notes |
|---|---|---|---|
| 2 | t1 → qc(16..19) for mul | 4q | n FLR |
| 4 | l10 → scratch qc(20..23) | 4q | n FLR |
| 4 | d00 → scratch (qc(?, 2q)) | 2q | n/2 FLR |
| 5 | **d11 → scratch qc(18..19)** | 2q | n/2 FLR — added by aliasing fix |
| 5 | right_11 := right_00 | 1q | already in baseline (line 1265) |
| 7 | l10 → final qc(4..7) | 4q | n FLR |
| 7 | d00 → final qc(8..9) | 2q | n/2 FLR |
| 9 | left_11 := left_00 | 1q | already in baseline (line 1310) |
| **First recursion subtotal** | | **18q** | **4.5n FLR added by Path B** (excluding baseline 2q) |
| 10 | (split is non-destructive, no memcpy) | 0 | — |
| **Second recursion subtotal** | | **0q** | — |

**Total memcpys added by Path B per signature: 4.5n FLR** (vs my earlier estimate of 5n; aliasing fix added 0.5n but second recursion saved more by being clean).

At logn=10: 36 KiB of bus traffic per sign. At ~50 MB/s on M35P, ~0.7 ms latency. ~5–7% perf overhead on a ~10 ms sign. Roughly as estimated; small enough to be acceptable, large enough to want to claw back if possible (the in-place mul optimization saves ~0.5n more, taking total to ~4n FLR).

---

## Net peak math (final)

| Configuration | Allocated tmp[] | Live peak (during recursion) |
|---|---|---|
| Baseline | 28q = 7n FLR | 28q = 7n FLR |
| Path B | **24q = 6n FLR** | **24q = 6n FLR** |
| Saving | 4q = 1n FLR | 4q = 1n FLR |

At logn=9 (Falcon-512): **4 KiB exact**.
At logn=10 (Falcon-1024): **8 KiB exact**.

Composes with flash-spill: spilling the 2.5n persistent set (c1 + l10 + d00 = 10q) during the right recursion brings live peak to 6n − 2.5n = 3.5n FLR during the spill window.

---

## Bottom line

Path B is structurally sound for the first recursion: 28q → 24q peak, saves 1n FLR = 8 KiB at logn=10. Trade: ~5% perf hit, KAT regeneration. Second-recursion details still need a careful walkthrough but should fit. Distribution equivalence is the load-bearing assumption — if it doesn't hold, Path B is dead and we fall back to Path A + spill.

Next: write `path_b_sketch.diff` based on this walkthrough.
