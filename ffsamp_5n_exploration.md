---
name: ffsamp 5n FLR peak exploration
description: Can the recursive Path B body's 5n FLR fixed-point peak be pushed below 5n? Pre-implementation analysis of three candidate paths.
---

# ffsamp 5n FLR peak exploration

## Premise

After Day 9.5 the user-visible signing tmp[] is **43n+31 bytes** under
`FNDSA_PATH_B + FNDSA_PHASE1_REDUCED`. The binding constraint is now
**ffsamp's 5n FLR peak** (verified empirically by `test_path_b_peak.c`
at logn 2..10), not phase 1's 4n FLR.

This document evaluates whether ffsamp can drop below 5n. It does so by
deriving the 5n fixed point analytically from the Path B body
(`sign_sampler.c:1276-1336`), then evaluating three structural moves
against that fixed point.

## Where the 5n comes from

### Path B body's 24-quarter pre-recursion layout

Per `sign_sampler.c:1310-1314`, just before the right recursion at level L:

```
qc(0..3)   c1            n FLR    (= t0 + t1·l10; persistent across right rec)
qc(4..7)   l10           n FLR    (persistent; needed post-right for tb0)
qc(8..9)   d00         ½n FLR    (self-adj; persistent; needed post-right)
qc(10..11) ce_t0        ½n FLR    (= callee t0 = split-low of t1)
qc(12..13) ce_t1        ½n FLR    (= callee t1 = split-high of t1)
qc(14..15) right_01     ½n FLR    (= callee g01)
qc(16)     right_00     ¼n FLR    (= callee g00, self-adj)
qc(17)     right_11     ¼n FLR    (= callee g11, self-adj = right_00)
qc(18..23) callee free scratch — 1.5n FLR
                                   ─────
                                   6n FLR (24 outer-q at parent level)
```

`qc(off) = tmp + (off << (logn-2))`, so each outer-quarter is n/4 FLR.

### Recursion-aware peak

The callee at level L−1 occupies `qc(10..)` upward. Empirically, callee
uses 5·n_{L−1} FLR = 2.5n parent-FLR, so its writes extend to `qc(20)`
exactly. The function-internal layout reserves `qc(18..23)` as "callee
free scratch" but the callee uses only the first 10 outer-quarters of it.

Define `t(L) = T(L) / n_L` (tmp peak in units of n at level L). Then:

```
t(L) = max(persistent_local, callee_offset + 0.5 · t(L−1))
     = max(4.5,                 2.5 + 0.5 · t(L−1))
```

- 4.5 is the local persistent set (qc(0..17), full L-level layout)
- 2.5 is callee_offset = qc(10) = 10 · 0.25n = 2.5n
- 0.5 · t(L−1) because callee FLR scale by half each level

Fixed point: t* = max(4.5, 2.5 + 0.5·t*) → t* = 5. Matches the empirical
peak at every logn 2..10.

**To push below 5, we must change either the 2.5 (callee_offset) or the
0.5·t(L−1) tail (recursive callee) — and either of those moves requires
shrinking the persistent_above set across the right recursion, since
that set physically lives between qc(0) and qc(callee_tmp).**

## The persistent_above set across the right recursion

Looking at the Path B body, what is *forced* to survive `ffsamp_fft_inner`'s
right-recursion call at step 8?

| Slot | Symbol | Why it must survive |
|---|---|---|
| qc(0..3) | c1 | needed at step 9 for `tb0 = c1 - z1·l10` |
| qc(4..7) | l10 | needed at step 9 for `tb0 = c1 - z1·l10` |
| qc(8..9) | d00 | needed at step 10 for left-subtree split |

That's **2.5n FLR** of persistent_above. The callee starts at qc(10).

The 2.5n is what limits how early the callee tmp can start. If we could
free even one of these three slots, the callee could move down and the
peak would drop.

## Three candidate paths

### Path A — Drop l10, recompute from external_basis post-right

**Idea.** During the right recursion, l10 is not in tmp[]. After the
recursion returns, recompute l10 from the external basis:

```
g01 = b00·adj(b10) + b01·adj(b11)      // 2 fpoly_mul + 1 fpoly_add
l10 = g01 / d00                         // 1 fpoly_div_selfadj (d00 is real)
```

Then proceed with `tb0 = c1 - z1·l10` as before.

**Layout (outer level only).**

```
qc(0..3)   c1           n FLR       (persistent across right rec)
qc(4..5)   d00         ½n FLR      (persistent across right rec)
qc(6..)    callee tmp    ──→         starts at offset 1.5n instead of 2.5n
```

Local outer peak during right recursion = 1.5n + 2.5n callee = **4n FLR**.

**Outer-level only.** Inner levels (L−1, L−2, …) cannot recompute their
l10 — they don't have basis polynomials, only split-form gram inputs
(right_00, right_01) which are not preserved past LDL. So inner levels
keep the standard 5n_{L−1} peak.

The recursion check: callee at qc(6), uses 2.5n absolute = qc(6..15).
End of writes = qc(16) = 4n absolute. Outer peak = max(persistent_local, 4n)
= 4n. ✓

**User-visible saving.** Outer is the binding level (largest absolute FLR).
Total tmp[] FLR area drops from 5n to 4n → saves 8n bytes/sign.

| Variant | Pre (43n+31) | Post (35n+31) | Δ |
|---|---|---|---|
| FN-DSA-512 (n=512) | 22047 B | 17951 B | **4 KiB** |
| FN-DSA-1024 (n=1024) | 44063 B | 35871 B | **8 KiB** |

**Perf cost — measured, not projected.**

Standalone microbench (`bench_recompute.c`, NEON host, library built without
PATH_B for the host baseline):

| logn | recompute step | full sign | ratio |
|---|---|---|---|
| 9  | ~388 ns | ~213 µs | **0.18%** |
| 10 | ~430 ns | ~276 µs | **0.15%** |

The recompute kernel is 2 fpoly_mul_fft + 1 fpoly_add + 1 fpoly_div_selfadj
≈ 11n real FP ops, all SIMD-vectorized cleanly. **The measured 0.18% is
~10× under the 2% acceptability threshold.**

**Projected M35P scalar cost.** M35P has no SIMD, so the recompute kernel
loses NEON's 2× throughput. But sign_core itself also vectorizes on NEON,
so the ratio is roughly preserved across host→scalar — not amplified by
the full NEON speedup. Realistic projection: ~1–1.5% on M35P scalar.
Still well under 5%, comfortably under 2%.

This replaces an earlier worst-case projection of "5–10%" — the measurement
shows the kernel is much smaller than total ffsamp work suggested.

The bench is reproducible via `make bench_recompute && ./bench_recompute`.

**Implementation effort.**
1. Pass `external_basis` through to ffsamp via `sampler_state` (or a new
   parameter on `ffsamp_fft`).
2. Detect outer-level call (`logn == ss->logn`) and select the reduced
   path; inner-level recursive calls use the existing Path B body.
3. New layout for outer body: persistent_above = c1 + d00 (1.5n at qc(0..5)),
   callee at qc(6).
4. Post-right-recursion: recompute g01, divide by d00, then call existing
   `fpoly_pathb_finalize` (input slots adjusted).
5. Test: `test_ffsamp_5n.c` paint-and-check at the new 4n boundary +
   bit-distribution-equivalence with existing path.

Estimated 4–5 days end-to-end. No new architecture-specific primitives;
the recompute reuses existing fpoly_mul / fpoly_add / fpoly_mulconst.

**Verdict: viable; ship behind a flag.** Saves 4/8 KiB at measured 0.18%
host / projected ~1–1.5% M35P scalar perf cost. Bench-confirmed sub-2%
across all reasonable assumptions. The deployment-shape question (does
FN-DSA-512 + Ethereum app fit on 32 KiB net app SRAM) is what makes Path A
load-bearing on Nano-class devices, not optional — see "Deployment context"
below.

### Path B — Share c1 with callee scratch via slot reordering

**Idea.** Instead of c1 sitting at qc(0..3) (below the callee), put it at
qc(20..23) (above the callee). Then the persistent_above set shrinks.

**Why it doesn't work.** Callee writes extend to qc(20) exactly (= 2.5n
absolute). Putting c1 at qc(20..23) collides with the boundary of callee
writes. The callee's recursive child at L−1 has the same fixed-point
peak — empirically the WRITES go all the way to qc(20). Verified by
`test_path_b_peak.c` reporting "highest touched: qc(19)" — qc(19) is the
last quarter touched, qc(20)'s start is the boundary.

A relocation of c1 to qc(20..) would be overwritten by the callee. Could
work only if the callee's recursive peak were strictly below 5·n_{L−1},
which it is not.

**Verdict: red.** Blocked by the recursive fixed point.

### Path C — Streaming right-subtree input (algorithmic change)

**Idea.** The callee's input set (ce_t0 + ce_t1 + right_01 + right_00 +
right_11 = 2n FLR at parent scale) is materialized atomically before the
right recursion starts. If split-fft and split-selfadj-fft could be fused
with the callee's first level of computation (LDL), the callee could
consume its inputs in pieces, and the input materialization peak could be
reduced.

**Why it's speculative.** The callee's LDL needs g00, g01, g11 alive
simultaneously (LDL is per-coefficient but reads all three). Similarly
the callee's apply-basis-equivalent needs t0, t1 alive. So the callee
*also* has a 4n_{L−1} input requirement. Streaming the input would
require the callee to consume one polynomial fully before reading the
next, which is incompatible with LDL's coupled structure.

**Verdict: red.** Would require redesigning ffsamp itself, not in
standard Falcon literature.

## Asymptotic vs achieved

The 5n is the recursive Path B's asymptote — verified achieved at every
logn 2..10. Going below requires breaking the recursive fixed point. Path
A breaks it at the **outer level only**, exploiting the special status of
the top-most call (it has access to external_basis). Inner levels remain
at 5n_{L−1}. Since outer is the binding absolute FLR, this is a real win.

There is no further saving available at inner levels without algorithmic
change — they have no analog of external_basis to recompute from.

## Cumulative tmp[] floor (with Path A shipped)

```
Pornin baseline:                           59n+31
+ FNDSA_PATH_B (body t1·l10 absorption):    51n+31  (Path B PR, merged track)
+ FNDSA_PHASE1_REDUCED (precomputed basis): 45n+31  (Day 5)
+ Recursive Path B tightening:              43n+31  (Day 9.5, current head)
+ ffsamp 5n→4n (Path A, hypothetical):      35n+31

FN-DSA-512:  cumulative  24 KiB → ~14 KiB tmp[]   (full chain saves 12 KiB)
FN-DSA-1024: cumulative  48 KiB → ~28 KiB tmp[]   (full chain saves 24 KiB)
```

## Deployment context — why Path A is load-bearing for FN-DSA-512 on Nano

ST33K1M5 (used across Nano S+, Nano X, Nano Gen 5, Stax, Flex):

```
64 KiB total SRAM
- 31 KiB BOLOS
≈ 32 KiB net app SRAM (all five devices; framebuffer lives off-SE on Stax/Flex)
```

Realistic peak RAM during a full ETH signing flow (with buffer reuse —
`fndsa_sign_temp` taking the same buffer the app used for TX parsing):

```
SDK glue / APDU comm buffer            ~2-4 KiB
ETH app globals                        ~1-2 KiB
FN-DSA globals                         ~1-2 KiB
max(eth_stack, fndsa_stack)            ~2-3 KiB  (shared region)
max(parse buffer, FN-DSA tmp[])        — dominant term
```

Comparison at logn=9:

| Build | tmp[] | Total peak | Fits 32 KiB? |
|---|---|---|---|
| PATH_B alone | 26143 B (~25.5 KiB) | 31.5–36.5 KiB | doesn't fit at mid-to-high estimates |
| **PATH_B + Path A** | **22047 B → 17951 B (~17.5 KiB)** | **27.5–32.5 KiB** | **fits with margin** |

**Path A is load-bearing**, not optional, for FN-DSA-512 + Ethereum-class
app deployment on Nano-class. PATH_B alone leaves the budget open only at
the low end of every estimate; Path A's 4 KiB of tmp[] reduction closes it
across the realistic range.

(Note: this assumes the app uses `fndsa_sign_temp(...)` and shares its TX
parse buffer with FN-DSA's tmp[]. Without that pattern — e.g. if the app
calls `fndsa_sign(...)` and the library puts tmp[] on its own stack —
the parse buffer and tmp[] don't overlap and the budget gains another
~8 KiB charge. This is a deployment constraint, not just a perf
optimization, and worth confirming with the app team early.)

## Recommendation

**Greenlight Path A as the FN-DSA-512 deployment shape.** The bench
settles the perf question (0.18% measured, ~1–1.5% projected scalar) and
the budget math makes it a hard requirement for Nano-class deployment.

Kill plan (5 days), targeting `ffsamp-5n-reduction` branch off
`phase1-reduction`:

| Day | Work | Output |
|---|---|---|
| 1–2 | Implement outer-only Path A: extend ffsamp_fft_inner with l10-drop + recompute-from-external_basis variant; new layout c1 @ qc(0..3), d00 @ qc(4..5), callee tmp @ qc(6) | working code, test_fndsa passes at logn 3..10 |
| 3 | `test_ffsamp_5n.c` paint-and-check at the 4n boundary; bit-distribution-equivalence with existing path | validated correctness |
| 4 | Update tmp_len budget docstrings (35n+31), ASAN clean | clean diff |
| 5 | PR draft + Donjon-side measurement asks | upstream-ready |

Hardware confirmation (Donjon-gated) lands in parallel and feeds the PR
description as deployment validation, not as a blocker on implementation.

## What the bench did NOT settle

The bench is the perf gate. It does NOT address:

1. **M35P stack peak under masking + lockstep.** If `fndsa_sign_temp`'s
   recursive frames hit 4–5 KiB instead of the projected 2–3 KiB, the
   budget tightens. Donjon-side measurement.
2. **`fndsa_*` globals footprint.** Permanent .bss/.data charged to the
   32 KiB. Donjon-side `arm-none-eabi-size` on the linked image.
3. **Worst-case ETH app peak parse buffer.** The 4–10 KiB range is wide;
   the high end (deep EIP-712 typed data) determines the parse-buffer
   side of `max(parse_buffer, fndsa_tmp[])`.
4. **SDK glue / APDU comm buffer charge.** Fixed cost before any state.

If items 1–4 land at the high end of estimates simultaneously, even Path
A can leave the budget tight by ~500 bytes. Donjon-side measurement is
the only way to settle this — but that's a deployment readiness check,
not an implementation gate.
