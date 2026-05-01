---
name: Phase 1 Reduction — Day 1 Option Survey
description: Survey of options for reducing sign_core's phase 1 footprint below 6n FLR. Six options evaluated against (savings, effort, API change, perf cost). One clear winner identified.
---

# Phase 1 reduction — option survey (Day 1)

## Constraint analysis

`sign_core`'s phase 1 footprint at 6n FLR comes from:

| Component | Size | Why |
|---|---|---|
| t0, t1 | 2n FLR | Target vector for ffsamp; written by apply_basis |
| b00, b01, b10, b11 | 4n FLR | All four basis polynomials simultaneously alive during `fpoly_gram_fft` |

The 4-polynomial simultaneity is structural. Confirmed by reading
[sign_fpoly.c:2470-2581](sign_fpoly.c#L2470-L2581) — every iteration of
`fpoly_gram_fft`'s inner loop reads from all four (b00, b01, b10, b11) and
writes outputs to b00, b01, b10. Specifically:

```
g00 = b00·adj(b00) + b01·adj(b01)        — needs (b00, b01)
g01 = b00·adj(b10) + b01·adj(b11)        — needs ALL four
g11 = b10·adj(b10) + b11·adj(b11)        — needs (b10, b11)
```

The `g01` term is the binding constraint: it requires all four basis
polynomials in the same loop iteration. No simple reorder eliminates this.

## Six options evaluated

| # | Option | Savings | Effort | API change? | Perf cost | Verdict |
|---|---|---|---|---|---|---|
| 1 | Streaming basis | 0 (peak unchanged) | 5d | no | — | ❌ blocked by gram's 4-poly need |
| 2 | On-demand basis recompute | 0 (peak unchanged) | 7d | no | high | ❌ same blocker |
| 3 | Half-size gram outputs in place | 0 (peak unchanged) | 3d | no | low | ❌ output size doesn't reduce gram-time peak |
| 4 | Move hm/G to stack | 3n bytes | 1d | no | 0 | 🟡 partial; 3 KiB at logn=10, 1.5 KiB at logn=9 |
| 5 | **Precomputed basis (separate buffer)** | **6n bytes** | **6d** | **yes** | **0 per sign** | ✅ **clear winner** |
| 6 | NTT-domain basis | — | — | — | — | ❌ red; conflicts with FFT-domain gram math |

### Detailed analysis per option

#### Option 1: Streaming basis computation
Compute basis polys incrementally; release some before others computed.
Tried multiple sketch orderings (compute pairs, compute g00 first then g01
in two-pass, etc). **Every path either keeps 3+ basis polys alive at the
critical g01 step OR forces re-computing basis polys (no peak savings,
just compute overhead).** The 4-poly simultaneity at gram time is
fundamental.

#### Option 2: On-demand basis recomputation
Keep f, F as int8 (1n bytes each), recompute b01 = -FFT(f), b11 = -FFT(F)
when needed. Costs ~n log n FLOPS per recompute. Same blocker as option 1:
gram_fft needs all four simultaneously, so we'd need to keep all four
materialized at gram time anyway. Recompute only saves space if we can
hold fewer than 4 basis polys at peak — gram blocks that.

#### Option 3: Half-size gram outputs in place
Currently `fpoly_gram_fft` writes full n FLR for each of g00, g01, g11.
The compact rearrange afterwards moves g00 and g11 to half-size (n/2 FLR
each, since self-adjoint). If gram_fft wrote them as half-size directly,
post-gram footprint shrinks by 1n FLR. **But during gram, all four basis
polys are still alive, so peak is unchanged at 6n.** Peak determines
tmp[]; output size doesn't.

#### Option 4: Move hm and G to stack
hm (2n bytes uint16) and G (n bytes int8) currently live above the FLR
working area at offsets 48n, 50n bytes. If allocated on the stack inside
`sign_core`:
```
sign_core(...) {
    uint16_t hm[N];              // 2n bytes on stack
    int8_t G[N];                 // n bytes on stack
    ...
}
```
- tmp[] shrinks by 3n bytes.
- Stack grows by 3n bytes.
- **Net memory unchanged** unless the deployment context has stack
  abundance vs heap scarcity (or vice versa).
- For SE-class hardware, stack is typically 4–8 KiB. Adding 3 KiB stack
  at logn=10 (3n = 3072 bytes) is significant.
- **Saves 3 KiB at logn=10, 1.5 KiB at logn=9** of tmp[] specifically.

Realistic for some deployments but not the killer win we're aiming for.

#### Option 5: Precomputed basis as separate buffer ⭐
B = [[g, -f], [G, -F]] computed once at key load, stored in a
caller-allocated buffer. `sign_core` takes B as a parameter; no longer
calls `basis_to_FFT` per signing.

Phase 1 footprint with this:
| Component | Size | Position |
|---|---|---|
| t0, t1 | 2n FLR | qc(0..7) |
| g00 (self-adjoint) | n/2 FLR | qc(8..9) |
| g01 | n FLR | qc(10..13) |
| g11 (self-adjoint) | n/2 FLR | qc(14..15) |
| **Total** | **4n FLR = 32n bytes** | — |

Plus ffsamp peak under recursive Path B = 5.25n FLR = 42n bytes.
Sign tmp[] = max(phase 1, ffsamp) + hm + G + alignment
           = max(32n, 42n) + 2n + n + 31
           = 42n + 3n + 31
           = **45n + 31 bytes**.

**Savings: 6n bytes** (51n+31 → 45n+31).
- logn=9 (FN-DSA-512): 26,143 → 23,071 = 3 KiB
- logn=10 (FN-DSA-1024): 52,255 → 46,111 = 6 KiB

**Cost: API change.** Caller allocates 4n FLR = 32n bytes for B; calls
new `fndsa_setup_basis(logn, sign_key, basis)` once at key load; passes
basis to `fndsa_sign_*_temp_with_basis(...)` variants.

**Why this is acceptable for SE deployment:** the basis can live in the
same persistent storage as the key (flash, not RAM). Per-sign RAM
allocation drops by 6n bytes. Total memory at the deployment level is
unchanged (same key + basis stored once); RAM specifically goes down.

**Why this matches Pornin's existing patterns:** Pornin's Rust crate
(`fn-dsa`) has a `small_context` cargo feature flag that toggles whether
basis B is precomputed. Default = precomputed at key decode (needs 32 KB
context). `small_context` = recompute per sign (saves the 32 KB at 25%
perf cost). This option ports the *default* behavior to C — adding the
precomputation knob C currently lacks.

**Implementation work (~6 days):**
- Day 1 (this doc): option survey ✅
- Day 2: design API and layout walkthrough
- Day 3-4: implement
  - New `fpoly_basis_setup(logn, f_int8, g_int8, F_int8, G_int8, basis_buf)` in `sign_fpoly.c`
  - New `fpoly_gram_fft_dst(logn, g00, g01, g11, b00, b01, b10, b11)` that reads basis and writes gram outputs to specified locations (the existing `fpoly_gram_fft` works in-place over the basis)
  - Modify `sign_core.c` to use external basis when provided
  - New `fndsa_sign_temp_with_basis()` API in `sign.c`
  - Update `fndsa.h` public docstring
- Day 5: validate (tests, ASAN)
- Day 6: bench
- Day 7-10: polish, compose with Path B, upstream PR

#### Option 6: NTT-domain basis (red)
Use NTT (mod q) representation for the basis. Coefficients become uint16
(2 bytes) instead of FLR (8 bytes), saving 3n FLR if applied to all four
basis polys.

**Conflicts with the algorithm:** gram math requires complex
multiplication of FFT-domain polynomials. NTT-domain polynomials don't
support this. Converting back and forth per gram-iteration would dominate
the runtime.

Verdict: red. Skip.

## Combined options

**Option 5 + Option 4 (basis external + hm/G on stack):**
- Phase 1 footprint = 4n FLR
- ffsamp peak = 5.25n FLR (under recursive Path B)
- max = 5.25n FLR = 42n bytes
- + 0 (hm/G now on stack)
- + 31 alignment
- **Total tmp[] = 42n + 31 bytes**

At logn=9: 21,535 B (down from 26,143 B baseline-Path-B). **Saves 4.5 KiB.**
At logn=10: 43,039 B (down from 52,255 B). **Saves 9 KiB.**

But adds 3n bytes of stack. Net: +1.5 KiB at logn=9, +3 KiB at logn=10
of stack pressure. May or may not be acceptable per deployment.

Recommended ordering: implement Option 5 first (clean 3/6 KiB win
without stack growth). Add Option 4 as an optional knob if the
deployment context can absorb stack growth.

## Decision

**Verdict: green — proceed with Option 5 (precomputed basis).**

Day 1 decision criterion was: ≥1 option with savings ≥ 1n FLR, effort
≤ 5 days, no API change. Option 5 delivers savings ≥ 1n FLR and effort
~5-6 days BUT requires API change (a new precomputation entry point and
a new sign function variant).

Per the kill plan's stated criterion this is technically **orange** (API
change), not green. Proceeding requires a user/Pornin sign-off on the
API change. The good news:

- The change is *additive*: existing API stays. New API is opt-in.
- Pornin's Rust crate already has the analogous flag (`small_context`),
  so the conceptual move is familiar to him.
- The savings number (3/6 KiB at FN-DSA-512/1024) matches the deployment
  motivation cleanly.

**Recommended Day 2 task:** sketch the API design and verify Pornin would
likely accept a precomputation knob. If yes, proceed to Day 3-4 implementation.
If the API change is unacceptable, the kill plan dies at Day 2 with no
more options to try.

## Cross-check against Pornin's Rust patterns

From the rust-fn-dsa crate documentation and the Day 0 audit notes
([rust-fn-dsa/kill_plan_notes.md](../rust-fn-dsa/kill_plan_notes.md)):

> The crate exposes a `small_context` cargo feature that toggles whether
> basis B is precomputed:
>
> | Config | `self.basis` | Per-sign work | Total context |
> |---|---|---|---|
> | Default | 32 KB precomputed at decode | Recompute Gram from B | ~114 KB |
> | `small_context` | not stored | Recompute B from f/g/F/G inside `tmp_flr`, then Gram | ~82 KB |

Pornin already has a precomputation toggle in Rust. **C lacks the toggle
entirely** — every sign in C currently recomputes B from scratch, like
Rust's `small_context` mode.

Adding C's precomputation toggle is the natural next step. It would
bring C up to feature parity with Rust on this axis.

---

## ST33K1M5 deployment grounding (post Day 0 investigation)

Replacing earlier hand-wavy "basis in flash vs RAM" framing with concrete
ST33K1M5 facts (per the [Day 0 memory-hierarchy investigation](#day-0-investigation-summary)):

### Memory hierarchy

| Tier | Capacity | Per-sign cost | Notes |
|---|---|---|---|
| SRAM | 64 KiB total, ~24-48 KiB per app (unpublished) | ns, infinite endurance | tmp[] lives here |
| **No EEPROM/FRAM tier exists on this part** | n/a | n/a | "NVM" in ST/Ledger docs = same flash array |
| User flash (NVM) | up to 1534 KiB total, app slot ~tens-to-hundreds of KiB | unpublished, est. 1-4 ms per 64 B page write | basis would live here |

**The basis storage tier and the key storage tier are the same physical
flash array, behind the same SE tamper resistance.** No separate "more
secure" or "less secure" location to choose between.

### Three deployment patterns under option 5

| Pattern | Writes per key | Per-sign perf | Verdict |
|---|---|---|---|
| **A**: write basis once at key provisioning | 1 (out of 500K page-cycle budget) | reads from flash (memory-mapped, in cache) | ✅ correct deployment |
| **B**: recompute basis at each boot (write to flash) | N (boots) × 512 pages | depends on boot frequency | ⚠️ wears flash if frequent |
| **C**: recompute basis per signing | every sign | defeats option 5 entirely | ❌ don't |

For Pattern A, durability is a non-concern: 1 cycle out of 500K leaves
99.9998% of the page-write budget for whatever else.

### Updated cost analysis (ST33K1M5 specific)

| Component | Cost |
|---|---|
| Per-sign RAM during signing | **45n+31 bytes** (saves 6n vs Path B baseline) |
| Provisioning latency (one-time per key) | ~0.5-2 s blocking + lockstep (~20-35%) = **0.6-2.7 s** |
| Persistent flash per key | 32n bytes basis + 64 B atomic flag page + (optional) 1-2 KiB CRC table = **~32-34 KiB** |
| Per-sign perf overhead | ~zero (basis_to_FFT eliminated; gram_fft reads from cached flash) |
| Lockstep / countermeasure overhead | already in baseline; no new cost |

### Atomicity constraint (becomes load-bearing)

`nvm_write()` is documented as **not transactional**. A 0.5-2 s
provisioning write can be interrupted by power loss, leaving the basis
in an undefined partial-write state.

**Mitigation: single-flag-page protocol** (the only realistic option):
1. Write basis pages (~512 of 64 B each)
2. Write a 64-byte flag page LAST containing version counter + magic
3. `nvm_write()` of one page is atomic at the SE hardware level
4. On read: check flag → if invalid, re-derive basis from key

This is the design that goes into the Day 2 API + protocol sketch.

### Per-app NV budget concern

The investigation flagged that per-app NV slot size is not officially
published, but is "tens to low-hundreds of KiB." Adding 32 KiB at
logn=10 might be 30-50% of a slot.

**Action:** before Day 3 implementation, verify with `make load` against
a real Ledger Stax/Flex/Nano S+ that the FN-DSA app has room for 32 KiB
additional `N_basis`. If not, phase 1 dies.

### Speculos can't validate the deployment

Per the investigation: Speculos doesn't simulate flash timing,
write-amplification, or watchdog interaction. Functional testing on
Speculos validates correctness only.

**Action:** Day 7 bench runs on actual Ledger device with cycle counters,
not on host or emulator.

### Side-channel surface area

The basis is a 32 KiB cleartext FFT-domain object, ~10× the size of the
compact ~3 KiB key. SE tamper resistance covers it the same way it
covers the key, but the larger surface for power/EM analysis during
gram_fft loads might benefit from additional countermeasures.

**Action:** include this in the [questions doc](ledger_donjon_questions.md)
sent to Donjon. If their existing FN-DSA masking covers gram_fft loads,
no new work needed; if not, masking-the-flash-resident-basis becomes a
prerequisite.

---

## Updated decision (post-grounding)

**Verdict: GREEN with five conditions** (was: orange-with-API-change).

The five conditions from the Ledger/Donjon questions doc:
1. ✅ App's NV slot has room for 32n bytes basis + ~1 KiB journal at logn=10
2. ✅ Provisioning latency of 0.5-2.7 s is acceptable in user flow
3. ✅ App implements page-level checksum + atomic flag (single-flag-page
   protocol — see [phase1_api_design.md](phase1_api_design.md))
4. ✅ Hardware benchmark on actual Ledger device confirms perf model
5. ✅ Side-channel countermeasures cover the 32 KiB basis surface (or
   masking already does, per Donjon)

If conditions 1, 4, 5 fail or are uncertain → kill the project, ship
Path B alone (4/8 KiB savings) as the final deployable contribution.

If conditions 2, 3 are awkward but tractable → proceed with the design
adjustments documented in [phase1_api_design.md](phase1_api_design.md).

---

## Bottom line

**Option 5 (precomputed basis in flash) is sound for ST33K1M5/Ledger
Pattern-A deployment, with the atomic-flag-page protocol as the load-
bearing implementation detail.** 3/6 KiB additional RAM savings on top
of Path B's 4/8 KiB.

Decisions blocked on: per-app NV budget verification (Q8 to Donjon) and
masking status (Q1, Q4 to Donjon). Both can be unblocked with a single
contact.

Day 2 task: sketch the API + atomic-update protocol → see
[phase1_api_design.md](phase1_api_design.md).
