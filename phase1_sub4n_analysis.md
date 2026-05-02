---
name: Phase 1 sub-4n FLR exploration
description: Can phase 1 (with FNDSA_PHASE1_REDUCED + external_basis) drop below the 4n FLR floor? Pre-implementation analysis.
---

# Phase 1 below 4n FLR — exploration

## Premise

After Day 9.5, the user-visible signing tmp[] is **43n+31 bytes** under
`FNDSA_PATH_B + FNDSA_PHASE1_REDUCED`, with the FLR working area at **5n
peak**. The two binding peaks:

- **Phase 1 peak (handoff to ffsamp):** 4n FLR — compact gram (2n) + (t0, t1) (2n).
- **ffsamp peak (Path B body recursive):** 5n FLR — empirically verified by `test_path_b_peak.c`.

Total tmp[] FLR = max(4n, 5n) = 5n. Phase 1 is no longer the binding constraint;
ffsamp is. Pushing phase 1 below 4n therefore yields **zero KiB of user-visible
savings** unless ffsamp is *also* pushed below its current 5n peak.

This document evaluates whether sub-4n phase 1 is even theoretically reachable,
independent of whether the savings would materialize today.

## The 4n decomposition (current state)

Per `sign_core.c:198-211` the FNDSA_PHASE1_REDUCED layout writes:

```
qc(0..3)   t0  (n FLR)              — apply_basis output
qc(4..7)   t1  (n FLR)              — apply_basis output
qc(8..11)  g01 (n FLR)              — gram_fft_dst output, full-size off-diag
qc(12..13) g00 (n/2 FLR self-adj)   — gram_fft_dst output
qc(14..15) g11 (n/2 FLR self-adj)   — gram_fft_dst output
                                    ─────
                                    4n FLR total
```

Each component is at its information-theoretic minimum:

| Component | Size | Why minimum |
|---|---|---|
| g00 | n/2 FLR | Self-adjoint Hermitian polynomial → real coefficients only |
| g11 | n/2 FLR | Self-adjoint Hermitian polynomial → real coefficients only |
| g01 | n FLR | Off-diagonal of Hermitian gram, genuinely complex; g10 = adj(g01) is omitted |
| t0  | n FLR | apply_basis output, full-size complex polynomial |
| t1  | n FLR | apply_basis output, full-size complex polynomial |

The user's framing is correct: **the gram outputs (2n FLR compact) cannot be
shrunk** without changing the gram math itself, and the compact form already
exploits all available Hermitian symmetry.

So any sub-4n move must come from the **(t0, t1) pair** — the apply_basis
outputs.

## Where does the 4n constraint come from?

Both (t0, t1) need to be alive at the start of `ffsamp_fft_inner` because
Path B's body (`sign_sampler.c:1276-1284`) computes:

```c
fpoly_LDL_fft(logn, qc(12), qc(8), qc(14));      // g00,g01,g11 → d00,l10,d11 in place
memcpy(qc(16), qc(4), …); fpoly_mul_fft(qc(16), qc(8));   // qc(16) = t1*l10
fpoly_add(qc(0), qc(16));                         // qc(0) = c1 = t0 + t1*l10
```

After this, t0 is dead but **t1 is still needed for the right-subtree recursion
input** (split at step 5). So the persistent live set during c1 computation is:

```
d00 (½n) + l10 (n) + d11 (½n) + t0 (n) + t1 (n) = 4n FLR
```

This is the binding moment. Every other point in phase 1 + Path B setup is at
or below 4n.

## Three candidate paths below 4n

### Option A — Compute t0 lazily, fused into c1

**Idea.** Have phase 1 emit only `(g00, g01, g11, t1)` = 3n FLR. Compute t0
*just-in-time* inside ffsamp's body, fused with the c1 step:

```
c1[i] = (b01[i] · FFT_hm[i] · (1/q)) + t1[i] · l10[i]
```

This is coefficient-wise in the FFT domain, since apply_basis is itself a
sequence of pointwise complex multiplications.

**The catch.** The fused loop still needs an n-FLR slot to hold `FFT(hm)` —
the conversion-and-FFT of hm — *before* it can be multiplied with b01 to
produce t0 chunks. Where does FFT(hm) live?

- In a tmp[] slot → **back to 4n FLR** (FFT_hm replaces t0 in the same slot, no
  net win).
- Above tmp[] in hm's region → hm is uint16_t × 2n = 4n bytes = ½n FLR
  equivalent; FFT_hm needs n FLR = 8n bytes. Cost: above-tmp grows by ~6n
  bytes. tmp[] shrinks by 1n FLR = 8n bytes. **Net per-sign saving: 2n bytes.**

At FN-DSA-1024 (n=1024) that is 2 KiB; at FN-DSA-512 it is 1 KiB. Real, but
small.

**Engineering cost.** A new fused `fpoly_apply_basis_t0_fused_into_c1`
primitive in 4 architecture variants (SSE2, NEON, RV64D, scalar), plus careful
aliasing analysis (the c1 slot is read-modify-written while b01 is read from
external_basis). Comparable in scope to `fpoly_pathb_finalize`.

**Verdict.** Theoretically reachable; **not worth shipping** at the current
overall ffsamp-bound 5n peak — the saved n FLR isn't visible to the user.

### Option B — Drop t1 into a smaller representation

**Idea.** Could t1 be stored in a half-size form before ffsamp consumes it?

**No.** ffsamp's right-subtree recursion uses t1 as a generic full-degree FFT
polynomial. It is not self-adjoint (depends on a random hashed point hm and a
non-symmetric basis row). There is no algebraic structure to exploit.

**Verdict.** Red.

### Option C — Compress the gram's off-diagonal

**Idea.** Find a representation of g01 below n FLR.

**No.** g01 = b00·adj(b10) + b01·adj(b11) is a full complex polynomial whose
coefficients carry the entire off-diagonal information of the Hermitian gram.
Hermitian symmetry only buys symmetry between g01 and g10 (we already drop g10).

**Verdict.** Red. The user's premise is exactly right — this is the floor.

## Decision

**4n FLR is a hard structural floor for phase 1's handoff to standard ffsamp.**

The only sub-4n option is Option A (lazy t0 fused into c1). Its theoretical
saving is 1n FLR at phase 1, materialized as ~2n bytes of tmp[] reduction by
moving FFT(hm) above the FLR area. At FN-DSA-1024 that is 1 KiB per signing —
real, but:

- **Currently dormant.** Total tmp[] = 5n FLR + above-tmp (set by ffsamp peak).
  Until ffsamp drops below 5n, phase 1 reductions don't move user-visible
  tmp[].
- **Engineering cost > saving.** A 4-arch fused primitive plus aliasing audit
  for a 1 KiB saving is asymmetric vs the 4 KiB / 8 KiB savings of prior moves.
- **Future trigger.** If a future ffsamp restructure pushes its peak below 4n
  (e.g. eliminates one of the persistent {c1, l10, d00} slots), this analysis
  becomes actionable: phase 1 reduction to 3n unlocks an additional 1 KiB.

## What would unblock further savings

The natural binding constraint to attack next is **ffsamp's 5n peak**, not
phase 1's 4n. The 5n is composed of:

- c1 (n) — Path B's fused t0+t1·l10
- l10 (n) — needed post-recursion for tb0
- d00 (½n) — left-subtree input
- right-subtree input (split: callee t0 ½n + callee t1 ½n + right_01 ½n + right_00 ¼n + right_11 ¼n) ≈ 2n
                                                                       ─────
                                                                       5n total

Reducing ffsamp would mean either:

1. Eliminating l10's persistence by recomputing it from d00 and the gram
   (cost: extra LDL-like work post-right-recursion).
2. Sharing storage between c1 and one of the right-subtree split outputs
   (currently isolated; non-trivial aliasing).
3. A streaming variant of the right-subtree that doesn't materialize all of
   {ce_t0, ce_t1, right_01, right_00, right_11} simultaneously.

Each of these is a separate kill plan in its own right. None has the clean
single-insight character of Path B's t1·l10 absorption; they are deeper
structural moves.

## Recommendation

**Close this branch as documented research. Do not implement.**

- Sub-4n phase 1 is theoretically reachable via Option A but yields no
  user-visible savings until ffsamp drops below 5n.
- The 4n floor is otherwise structural, exactly as the user framed.
- A future kill plan targeting ffsamp's 5n peak would re-open this analysis.
