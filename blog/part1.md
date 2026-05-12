# FN-DSA RAM optimizations on constrained devices, Part 1: the foundation

*How we got FN-DSA-512 from 29.5 KiB to 25.5 KiB of working RAM — with zero flash budget, zero perf cost.*

---

## Why this matters

NIST finalized FN-DSA (Falcon) as one of the post-quantum signature standards. Like all PQ primitives, it's hungry for RAM compared to ECDSA: a reference implementation on a 32-bit CPU eats roughly **29.5 KiB of working memory per signature at FN-DSA-512** and **59 KiB at FN-DSA-1024**.

Most secure-element (SE) chips don't have that. Ledger's Flex and Stax wallets ship with a ST33K1M5 SE, which gives an application on the order of 30 KiB of SRAM after the operating system reserve. A signing routine that wants 29.5 KiB has zero headroom; one that wants 59 KiB does not run at all.

This series walks through the RAM optimizations we built into [c-fn-dsa](https://github.com/...) to drive that number down. The work is organized as a **layered staircase** — each layer is independently understandable, and you can choose how high to climb based on what your hardware can afford.

By the end of three posts:

- **Part 1 (this one)**: the foundational refactors that buy us **4 KiB at FN-DSA-512** for zero flash cost and zero perf cost — pure code reorganization. End state: `51n+31` bytes = **25.5 KiB**.
- **Part 2**: outsource the *basis* to caller-provided storage. End state: **17 KiB** (with B2 SIMD or B3 scalar + Phase 5).
- **Part 3**: outsource the *LDL tree* as well. End state: **15 KiB scalar, 16 KiB SIMD**. Plus the deployment matrix (flash vs NVRAM vs host-streaming) and an honest look at where FN-DSA-1024 lands.

Throughout, we use **two parallel units** for memory sizes:

- A formula in `n bytes` where `n = 2^logn` is the polynomial degree (n = 512 at FN-DSA-512, n = 1024 at FN-DSA-1024). Memory grows linearly in n.
- A concrete KiB number at FN-DSA-512 (and where space permits, FN-DSA-1024).

Both numbers are **empirically measured** with paint-and-check sentinel tests against the actual code, not extracted from documentation. The measurement methodology gets its own section in Part 3.

---

## Anatomy of an FN-DSA signature

Before we can cut RAM, we need to know where it goes. FN-DSA signing decomposes into three phases:

```
┌───────────────────────────────────────────────────────────────────┐
│ Phase 1 — Setup                                                   │
│   • Decode int8 polynomials (f, g, F) from the encoded sign_key   │
│   • Reconstruct G = h*F mod q (NTT)                               │
│   • Build the lattice basis B = [[g, -f], [G, -F]] in FFT form    │
│   • Compute the Gram matrix G = B·adj(B):  g00, g01, g11          │
│   • Compute the target vector [t0, t1] from the message hash hm   │
├───────────────────────────────────────────────────────────────────┤
│ Phase 2 — ffsamp recursion (Fast Fourier sampling)                │
│   • LDL-decompose the outer Gram: l10, d00, d11                   │
│   • Recurse on a depth-logn tree, splitting at each level         │
│   • Sample integer offsets at the leaves (Gaussian sampler)       │
│   • Merge back up to produce a lattice point [z0, z1]             │
├───────────────────────────────────────────────────────────────────┤
│ Phase 3 — Post-ffsamp                                             │
│   • Compute v0 = t0·g + t1·G, v1 = -t0·f - t1·F                   │
│   • Compute s1 = hm − v0, s2 = -v1                                │
│   • Check the squared norm; reject and loop if too large          │
│   • Encode (nonce, s2) into the output signature                  │
└───────────────────────────────────────────────────────────────────┘
```

Two of these phases dominate the RAM budget: Phase 1 (setup) holds the entire basis and Gram matrix simultaneously, and Phase 2 (ffsamp) carries a working set across a recursion of depth `logn`. Phase 3 reuses the same buffer.

In the reference implementation, `tmp[]` is one big scratch buffer of `59n+31` bytes (the `+31` is alignment slop for 32-byte alignment). At FN-DSA-512 that's **30,237 bytes ≈ 29.5 KiB**. At FN-DSA-1024 it's **60,447 bytes ≈ 59 KiB**.

### The baseline layout

Conceptually, the baseline `tmp[]` looks like this during ffsamp's outer call (the busiest moment):

```
tmp[]:  [    7n fpr     ][ G ][   misc / hm   ]
        ↑               ↑    ↑                ↑
        byte 0          56n  57n              59n
```

The "7n fpr" region (56n bytes; one fpr is an 8-byte IEEE 754 double) is ffsamp's working budget. The recursive ffsamp algorithm Pornin describes allocates **28 quarter-slots** (`qc(0)..qc(27)`), each `n/4` fpr = `2n` bytes, for a total of `28 × 2n = 56n bytes = 7n fpr` at the outer call. Beyond that: `G[]` (n bytes int8), the hash-to-point output `hm` (2n bytes uint16), and a few odds and ends.

The 28 quarter-slots reserve room for everything ffsamp's recursive body might touch at the outer level:

```
qc index:  0    4    8    12  14  16  18    22  ...   28
           t0   t1   l10  d00 d11 16  18  callee free space
                          (sa)(sa)
```

`(sa)` = self-adjoint, occupying only half-size. The slots `qc(16..27)` are reserved for the recursive callee's working space (the callee at logn-1 sits within the caller's qc(14..27) region).

**This 7n fpr is where most of the optimization work happens**, and the journey of this blog series is essentially: how do we get from 7n fpr at the outer call down to ~3n fpr (Bucket C), and how do we reuse the trailing buffers (G, hm, post-ffsamp NTT temporaries) more aggressively along the way.

---

## The outsourcing staircase

We organize optimizations as a four-step staircase, where each step **outsources** more state to the caller (and therefore expects more state to be precomputed):

```
Bucket A     no outsourcing                           — foundational refactors
Bucket B     basis outsourced                         — +32n bytes outside tmp[]
Bucket C     basis + LDL tree outsourced              — +(logn-1)·16n bytes outside
Bucket D     basis + tree + sk outsourced             — +sk_len bytes outside (fixed-key)
```

A few things to internalize about this staircase:

1. **The buckets compose cumulatively**, not independently. Bucket B's optimizations work *because* Bucket A's recursive-body restructure already made the inner body smaller. Bucket C's compact outer body works *because* Bucket B's outer-level l10 drop already made the outer body smaller. You can't pick Bucket C without paying for Bucket B's outsourcing too.

2. **Within each bucket, the individual sub-optimizations are independent Lego blocks.** In Bucket B there are five: Layer 2 (the API), Layer 3 (outer l10 drop), B2 (basis-direct post-ffsamp, SIMD-only), B3 (G outsourced, scalar-only), Phase 5 (hm-recompute). You can take a SIMD build with Layer 2 + Layer 3 + B2 + Phase 5; a scalar build with Layer 2 + Layer 3 + B3 + Phase 5; or anything in between.

3. **"Outsourced" is location-agnostic.** The library takes a `const void *` pointer to outsourced state. The caller decides where the bytes physically sit — flash, NVRAM, host-streamed, or even RAM if they have plenty. We'll return to this deployment matrix in Part 3.

The headline numbers (all empirically measured on the current `ffsamp-tree-compact-3n` branch with sentinel paint-and-check tests):

| Milestone | `tmp[]` formula | FN-DSA-512 | FN-DSA-1024 |
|---|---|---|---|
| Baseline (reference) | `59n+31` | **29.5 KiB** | **59 KiB** |
| Bucket A endpoint | `51n+31` | **25.5 KiB** | **51 KiB** |
| Bucket B endpoint (scalar) | `34n+31` | **17 KiB** | **34 KiB** |
| Bucket C endpoint (scalar) | `30n+31` | **15 KiB** | **30 KiB** |

That's a 14.5 KiB / 29 KiB reduction at FN-DSA-512 / FN-DSA-1024 respectively — roughly half the baseline.

---

## Bucket A: foundational refactors

Bucket A enables the entire staircase. Turn on the build flag `FNDSA_LOW_RAM=1` and you get all of Bucket A's optimizations automatically — they're not opt-in within Bucket A. The same compile flag is the prerequisite for Buckets B/C/D's API entry points to exist.

Bucket A contains **five independent sub-optimizations**. Four of them are foundational (also used by Buckets B and C); one of them is replaced by Bucket B and becomes dead code there.

| Sub-opt | What it does | Saves | Used by Bucket B/C? |
|---|---|---|---|
| A.1 Path B reorder | apply_basis runs before gram_fft; b01 preserved | 8n | No (replaced by `_external` setup) |
| A.2 Recursive ffsamp restructure | absorb t1·l10 into c1 before right rec | 1n fpr / level | Yes |
| A.3 `fpoly_muladd_fft` | fused per-coeff FMA, no scratch | (folded into A.2) | Yes |
| A.4 `fpoly_pathb_finalize` | fused tb0 + z1 merge, no scratch | (folded into A.2) | Yes |
| A.5 `fpoly_gram_fft_dst` | direct-write Gram to compact layout | (enables compact in B) | Yes |

We'll walk through each.

### A.1 — Path B layout reorder

**What it does.** The reference setup phase computes:

1. `basis_to_FFT(f, g, F, G)` — produces `b00, b01, b10, b11` in FFT form
2. `gram_fft(b00, b01, b10, b11)` — produces `g00, g01, g11` *in place over the basis*, destroying `b01`
3. `apply_basis([t0, t1] := [hm, 0] · B^{-1})` — needs `b01` and `b11` to apply

Step 3 needs `b01`, which step 2 just destroyed. The reference solution: before step 2, take a 1n-fpr backup of `b01` into a scratch slot `t2`, then step 3 reads `t2` instead of `b01`.

Path B reorders the operations and modifies `apply_basis`:

1. `basis_to_FFT(f, g, F, G)` — produces `b00, b01, b10, b11`
2. `apply_basis_preserving_b01(b01, b11, [t0, t1] := [hm, 0] · B^{-1})` — reads `b01`, leaves it intact
3. `gram_fft(b00, b01, b10, b11)` — now safe to consume `b01`

Now we don't need the `t2` backup at all. The compact rearrange that follows uses `b11` (dead after `gram_fft`) as the small scratch buffer for the half-size memcpys, which is the only remaining transient.

**Savings.** 1n fpr = **8n bytes** = **4 KiB at FN-DSA-512 / 8 KiB at FN-DSA-1024**.

**Cost.** Zero. The reorder produces bit-identical Gram outputs (FP add/mul ordering changes are within the IEEE-754 deterministic semantics our tests verify).

**Why Bucket B doesn't depend on it.** When the basis is outsourced (Bucket B), we never run `basis_to_FFT` or `gram_fft` in the destructive form. We use `fpoly_apply_basis_external` (reads basis from outside `tmp[]`, doesn't destroy anything) and `fpoly_gram_fft_dst` (writes Gram outputs to a separate compact location). The reorder problem doesn't exist there.

This is the only Bucket A sub-opt that gets superseded. The other four are load-bearing for the entire staircase.

### A.2 — Recursive ffsamp body restructure

**What it does.** This is the biggest win and the most subtle. Inside `ffsamp_fft_inner`'s recursive body, the baseline carries both `t0` and `t1` (the target vector at this level) through the right-subtree recursion. After the right rec returns, the body computes:

```
tb0 = t0 + (t1 - z1) · l10
```

where `z1` is the just-returned right-subtree result.

The restructure absorbs `t1·l10` into `t0` *before* the right rec:

```
c1 = t0 + t1·l10       (computed before the rec, in place over t0's slot)
                       (so we don't have to carry t1 through)

right rec  →  z1

tb0 = c1 - z1·l10      (computed after the rec)
```

The bookkeeping is non-obvious — you have to maintain enough information to invert the absorption later — but the algebra works out and the net effect is: only `c1` (one full polynomial = n fpr) needs to persist across the right recursion, not both `t0` and `t1` (two polynomials = 2n fpr).

This **saves 1n fpr per recursion level** in the persistent set. At the outer level with logn = 9 the recursion is 8 levels deep, but the savings don't simply multiply by depth — each level only "saves 1n fpr" relative to that level's `n`, and `n` halves at each step. The effective saving at the outer call is closer to a flat 1n fpr at the outer level, because each callee's frame fits within its allocated quarter-slot region.

**Savings.** The post-restructure ffsamp recursive body uses **24 of the 28 baseline quarter-slots = 6n fpr** instead of `7n fpr`. After we add A.3, A.4, A.5 (below) and let the outer-level body be similarly tight, total `tmp[]` drops by an additional 4n bytes beyond Path B — bringing the cumulative Bucket A endpoint to `51n+31` = **25.5 KiB at FN-DSA-512**.

**Cost.** Zero. The intermediate values are different (you're now staging `c1` in place where `t0` used to be) but the final samples `[z0, z1]` are bit-identical. We verify this with 1000-seed KAT stress at logn 2..8 and dozens at logn 9, 10.

**Why Buckets B and C depend on it.** Both Bucket B's outer-level l10 drop and Bucket C's compact tree-only outer body assume the recursive body underneath them is already restructured. If you took A.2 out, the deep-recursion footprint widens by ~1n fpr per level — and our reported numbers for Bucket B (17 KiB) and Bucket C (15 KiB) silently grow.

### A.3, A.4, A.5 — Fused primitives

A.2's restructure leaves us in a position where a few hot operations want to be fused into single passes to avoid intermediate scratch. The three new primitives:

#### A.3 — `fpoly_muladd_fft(logn, c, a, b)`

Computes `c[i] = c[i] + a[i] · b[i]` per coefficient in CPU registers, no scratch buffer required. (Roughly: a fused multiply-add over the FFT representation of two polynomials.)

This replaces the baseline pattern:

```c
memcpy(tmp_scratch, a, n * sizeof(fpr));        // copy a
fpoly_mul_fft(logn, tmp_scratch, b);            // tmp_scratch *= b
fpoly_add(logn, c, tmp_scratch);                // c += tmp_scratch
```

…with one primitive that does the same work without `tmp_scratch`. The `c1 = t0 + t1·l10` computation that A.2 needs runs this primitive at every level.

#### A.4 — `fpoly_pathb_finalize(logn, c1, t1_slot, zlow, zhigh)`

The post-right-rec finalize step computes `tb0 = c1 - z1·l10` and produces the full merged `z1` (from its split halves `zlow`, `zhigh`). Baseline did this in a 5-step chain:

```c
merge_fft(z1, zlow, zhigh)          // z1 = merge(zlow, zhigh)
memcpy(tmp_scratch, z1)              // backup z1
fpoly_mul_fft(tmp_scratch, l10)     // tmp_scratch = z1 · l10
fpoly_sub(c1, tmp_scratch)           // c1 -= z1 · l10
// tb0 now in c1
```

…with a 1n fpr `tmp_scratch` buffer. `fpoly_pathb_finalize` rolls all this into one per-coefficient pass with no scratch.

#### A.5 — `fpoly_gram_fft_dst(logn, g00_dst, g01_dst, g11_dst, basis)`

Writes the three Gram polynomial components (`g00`, `g01`, `g11`) directly into a caller-specified compact layout — typically `qc(8..11)` for `g01`, `qc(12..13)` for `g00`, `qc(14..15)` for `g11` — instead of the baseline pattern of writing them somewhere then doing a `memcpy` rearrange. The compact layout is what ffsamp's outer body expects.

This primitive becomes the load-bearing setup primitive in Bucket B too — when basis is outsourced, the setup call is `fpoly_gram_fft_dst(logn, g00_dst, g01_dst, g11_dst, external_basis)`, reading from outsourced flash/NVRAM/whatever, writing to the compact `tmp[]` layout in one pass.

**Combined savings (A.3 + A.4 + A.5).** These don't each save a discrete number of bytes — they enable A.2's restructure to fit in the smaller persistent set. Without them, A.2's savings on paper get clawed back by the temporary scratch the un-fused operations need.

**Cost.** Zero. Each primitive is a direct algebraic rewrite of an existing sequence; the FP rounding order changes but the bit-exact output is verified.

---

## Bucket A endpoint: 25.5 KiB at FN-DSA-512

Empirically measured (`test_milestones.c` with `FNDSA_LOW_RAM=1` and no outsourcing API):

```
fndsa_sign_seeded_temp     logn=9    high-water = 26,112 B   = 51.00n   = 6.375n fpr   = 25.50 KiB
fndsa_sign_seeded_temp     logn=10   high-water = 52,224 B   = 51.00n                  = 51.00 KiB
```

The outer ffsamp call now uses **6.375n fpr** of `tmp[]` at FN-DSA-512 (down from 7.375n in the baseline). The remaining ~`6.375n` decomposes as:

- ffsamp's persistent set (c1 + d00 + callee inputs + callee free space): ~5n fpr
- decoded f, g, F at byte offsets 4n..7n: ~0.375n fpr equivalent
- G in tmp at byte 36n+: ~0.125n fpr equivalent
- hm at byte 48n: ~0.25n fpr equivalent
- post-ffsamp NTT buffers (ut0..ut3) overlapping ffsamp: covered

We've reclaimed 1n fpr (8n bytes / 4 KiB at FN-DSA-512) for **zero perf cost and zero flash cost**. That's the entire Bucket A win.

**On a 32 KiB SE budget** — FN-DSA-512 now fits comfortably with ~6 KiB headroom for stack and app-level BSS. FN-DSA-1024 is still way out at 51 KiB, well beyond budget.

To go lower we need to start outsourcing state — which is Bucket B's job, the topic of Part 2.

---

## What's coming in Part 2

Part 2 introduces the **outsourced-basis API**. The caller precomputes the 4n-fpr lattice basis B once (`fndsa_compute_basis()`), stores it somewhere outside `tmp[]` (flash, NVRAM, host-streamed buffer — the library doesn't care), and passes a pointer per sign (`fndsa_sign_with_basis_temp()`).

That single move unlocks five sub-optimizations:

- **Layer 2** — the basis API itself. The 2n-fpr setup overhead (basis construction + Gram) vanishes from `tmp[]`.
- **Layer 3** — outer-level `l10` drop. ffsamp's outer body no longer carries `l10` across the right recursion; it recomputes from the outsourced basis after.
- **B2** — basis-direct post-ffsamp (SIMD only). Skip the int8 redecode of `f`, `g` and the 4 forward-FFTs in the post-ffsamp lattice point computation. Multiply `[t0, t1]` directly against the FFT-form basis. Also eliminates `G[]` from `tmp[]` on SIMD builds.
- **B3** — G outsourced (scalar parallel of B2). Scalar can't use B2 (no FPU, FP path is slow), so it gets the equivalent win by outsourcing G too. Caller precomputes G alongside basis, stored separately.
- **Phase 5** — hm-recompute. After Layer 3, the deep recursion's footprint expands into hm's slot; rather than carve out space for hm, we recompute `hash_to_point` once post-ffsamp.

End state: **`34n+31` = 17 KiB at FN-DSA-512** (same number for SIMD with B2 and scalar with B3), with a clean intermediate milestone of **`37n+31` = 18.5 KiB** ("the 19 KiB stop") if you take Layer 3 without the B2/B3 sub-optimization.

In Part 3 we tackle Bucket C — outsource the LDL tree, refactor ffsamp's outer body to exploit the tree, and arrive at **15 KiB scalar / 16 KiB SIMD**. Plus the deployment matrix and the FN-DSA-1024 fit question.

---

**Code references.** All measurements in this post are reproduced from the [c-fn-dsa repository's `ffsamp-tree-compact-3n` branch](https://github.com/...). The Bucket A optimizations live in [`sign_core.c`](sign_core.c), [`sign_fpoly.c`](sign_fpoly.c), and [`sign_sampler.c`](sign_sampler.c); the API surface (only the standard `fndsa_sign_temp` is needed for Bucket A) is in [`fndsa.h`](fndsa.h). The measurement test we cite throughout is [`test_milestones.c`](test_milestones.c). The original Falcon paper is by [Pornin et al.](https://falcon-sign.info/) and we encourage curious readers to start there for the algorithm itself.

---

*This is Part 1 of a three-part series. Part 2 covers Bucket B (basis outsourcing). Part 3 covers Bucket C (tree outsourcing), deployment options (flash / NVRAM / host-streaming), and an honest look at FN-DSA-1024 on 32-KiB-class hardware.*
