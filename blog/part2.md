# FN-DSA RAM optimizations on constrained devices, Part 2: outsource the basis

*From 25.5 KiB to 17 KiB — by computing the lattice basis once and reading it from outside `tmp[]` thereafter.*

---

## Where we left off

[Part 1](./part1.md) walked through Bucket A: foundational refactors to the reference Falcon code (recursive body restructure, fused FFT primitives, layout reorder) that brought `tmp[]` from `59n+31` (29.5 KiB at FN-DSA-512) to `51n+31` (25.5 KiB). All zero-flash, zero-perf-cost — pure code reorganization.

That covered the "free" wins. To go further we have to **commit to outsourcing state**: precompute something once at provisioning, store it somewhere outside `tmp[]`, and pass it to the library on every sign.

Bucket B outsources one thing: the **lattice basis B**.

By the end of this post, `tmp[]` is down to **`34n+31` = 17 KiB at FN-DSA-512** and **34 KiB at FN-DSA-1024**, with **`37n+31` = 18.5 KiB** ("the 19 KiB stop") as a meaningful intermediate milestone for builds that want one fewer dependency.

---

## What "outsourcing the basis" actually means

In the standard signing API, every sign call:

1. Decodes f, g, F (the secret-key polynomials) from the encoded sign_key
2. Reconstructs G via NTT division
3. Calls `basis_to_FFT(f, g, F, G)` to produce the 4n-fpr lattice basis `B = [[g, -f], [G, -F]]` in FFT representation
4. Calls `gram_fft(B)` to produce the Gram matrix
5. Runs ffsamp + post-ffsamp
6. Emits the signature

Steps 3 and 4 produce `B` and `G` *inside* `tmp[]`. The basis lives there briefly, gets consumed by Gram and apply_basis, and is gone by the time ffsamp starts. But while it's alive, it eats 4n fpr = `32n bytes` of `tmp[]` — which, given the cascade of overlapping lifetimes in the setup phase, dominates the working budget.

**Bucket B's idea:** the basis only depends on the private key. The private key doesn't change between signs. So compute the basis once, store it somewhere outside `tmp[]`, and pass a pointer per sign.

The new API surface:

```c
/* One-time precompute, at provisioning time. */
int fndsa_compute_basis(
    unsigned logn,
    const void *sign_key, size_t sign_key_len,
    void *basis_buf, size_t basis_buf_len);   /* writes 32n bytes */

/* Per-sign — caller passes basis pointer. */
size_t fndsa_sign_with_basis_temp(
    const void *sign_key, size_t sign_key_len,
    const void *basis,                        /* read-only, 32n bytes */
    const void *ctx, size_t ctx_len,
    const char *id, const void *hv, size_t hv_len,
    void *sig, size_t max_sig_len,
    void *tmp, size_t tmp_len);
```

Two things to internalize about this API.

**First, the library is location-agnostic.** `basis` is just a `const void *`. It can point to flash (`.rodata`), NVRAM (writable nonvolatile memory on the SE), a buffer streamed from a host PC over USB, or a chunk of plain RAM that the application heap allocated. The library reads from the pointer the same way regardless. The deployment trade-off lives entirely on the caller's side.

**Second, the basis must be 8-byte aligned** (the library's API explicitly checks this and refuses misaligned pointers). It contains IEEE 754 doubles, and modern compilers vectorize reads with alignment assumptions. Flash and NVRAM regions on Ledger devices are naturally aligned; the caller usually doesn't have to do anything beyond declaring the region with `__attribute__((aligned(8)))`.

The first sign call after a fresh provision runs `fndsa_compute_basis(sign_key, basis_buf)` once — about 5–8 ms on a Cortex-M3 — and writes the 32n-byte basis to the caller's chosen storage. Every subsequent sign reads from there.

---

## The headline: where Bucket B's bytes come from

Before walking through the optimizations one by one, here's the empirically measured progression. All numbers from running the paint-and-check measurement test (which sentinel-paints `tmp[]`, signs, and reports the highest byte that got modified) on the current `ffsamp-tree-compact-3n` branch:

| Configuration | `tmp[]` formula | FN-DSA-512 | FN-DSA-1024 | Notes |
|---|---|---|---|---|
| Bucket A endpoint | `51n+31` | 25.5 KiB | 51 KiB | from Part 1 |
| Bucket B step 1 — basis API alone | `~43n+31` | 21.5 KiB | 43 KiB | Layer 2 only |
| Bucket B step 2 — + outer l10 drop | `37n+31` | **18.5 KiB** | 37 KiB | Layer 3 (the "19 KiB stop") |
| Bucket B step 3 — + B2 (SIMD) | `34n+31` | **17 KiB** | 34 KiB | SIMD-only |
| Bucket B step 3 — + B3 + Phase 5 (scalar) | `34n+31` | **17 KiB** | 34 KiB | scalar parallel |

That's a journey of **8.5 KiB at FN-DSA-512** (17 KiB total saved versus the original 29.5 KiB baseline), at a flash cost of 32n bytes = **16 KiB at FN-DSA-512 / 32 KiB at FN-DSA-1024** (plus n bytes for B3 on scalar, see below).

Trade-off shape: every 1 byte of basis in flash buys roughly 0.5 bytes of RAM, because the basis is a 32n-byte commit that frees up ~8.5n bytes (the Gram + setup overhead disappears, plus the downstream optimizations it unlocks). That's not a great ratio in isolation — but on hardware where flash is plentiful and RAM is the bottleneck, it's the right direction.

---

## Layer 2 — the basis API itself

**What it does.** With the basis passed in from outside, the setup phase of `sign_core` is rewritten:

```
BEFORE (no outsourcing):
  basis_to_FFT(f, g, F, G)        →  basis at tmp[+(N)..]    (writes 32n)
  gram_fft(basis)                  →  Gram in place, destroys b01
  apply_basis(t0, t1, t2, b11)     →  target [t0, t1] via t2 backup of b01
  rearrange Gram → compact layout  →  several memcpys

AFTER (basis outsourced):
  fpoly_apply_basis_external(t0, t1, external_basis, hm)
    →  target [t0, t1] from outsourced basis, no destruction
  fpoly_gram_fft_dst(g00, g01, g11, external_basis)
    →  Gram written directly to compact layout
```

The setup now consists of two primitive calls. No `basis` lives in `tmp[]`; the basis stays in whatever outsourced storage the caller chose. No `t2` backup is needed because the new `apply_basis_external` doesn't destroy anything (it reads from a const buffer). No Gram rearrange is needed because `fpoly_gram_fft_dst` writes directly to the compact slot layout that ffsamp's outer body expects.

**Bytes saved.** The basis itself is the biggest win: 4n fpr = 32n bytes that used to live in `tmp[]` are now outside. Plus the elimination of the t2 backup (1n fpr) and the various Gram setup buffers. Combined: roughly **8n bytes of `tmp[]`** ≈ **4 KiB at FN-DSA-512**.

But there's a subtlety. Layer 2 alone — just outsourcing the basis without taking any of the downstream Bucket B optimizations (Layer 3, B2/B3, Phase 5) — doesn't get us all the way to `34n+31`. The basis exit from `tmp[]` lets us shrink the setup phase, but the outer ffsamp call's working set is still wide because it carries `l10` across the right recursion. That's Layer 3's job. So:

| Sub-opt taken | Empirical `tmp[]` |
|---|---|
| Just Layer 2 | `~43n+31` (this isn't an officially supported config; it's a stepping stone) |
| Layer 2 + Layer 3 | `37n+31` (= **18.5 KiB at FN-DSA-512**) |
| Layer 2 + Layer 3 + B2 (SIMD) | `34n+31` |
| Layer 2 + Layer 3 + B3 + Phase 5 (scalar) | `34n+31` |

The `37n+31` row — Layer 2 + Layer 3 only — is the milestone we've been calling **"the 19 KiB stop"**. Rounded up from 18.5, it sits at a natural design point: a production wallet build that wants to commit to flash-resident basis but isn't ready to add Phase 5's extra `hash_to_point` call or the SIMD/scalar split logic of B2/B3.

---

## Layer 3 — outer-level l10 drop

This is where Layer 2 stops being just "the basis API" and starts being a real RAM optimization.

**Background.** `ffsamp_fft_inner` is recursive. At each level it computes:

```
l10 = g01 / d00         (the LDL off-diagonal)
c1 = t0 + t1 · l10
right_rec → z1
tb0 = c1 - z1 · l10     ← l10 is needed AGAIN here
... (left rec, merge)
```

At the **outer** level, `l10` has to survive across the right recursion because the post-rec finalize needs it. In Bucket A this means `l10` (n fpr = 8n bytes) sits in the outer body's persistent set the entire time the right rec is running. The outer-body layout reserves `qc(4..7)` for it.

**The Layer 3 trick:** at the outer level (and only the outer level), drop `l10` from the persistent set across the right rec. After the rec returns, **recompute `l10` from the outsourced basis**:

```
g01 = b00 · adj(b10) + b01 · adj(b11)    ← derived from outsourced basis
l10 = g01 / d00                          ← LDL division, in place
... use l10 ...
```

This costs one polynomial multiplication (`fpoly_g01_fft_external`, ~11n FP ops) plus one LDL division. On a Cortex-M3 that's a couple of milliseconds. In exchange we save 1n fpr of `tmp[]` at the outer level — `qc(4..7)` becomes free space — which is the difference between `~43n+31` and `37n+31`.

**Why only at the outer level?** The recursive callees would have to do the same recomputation, but they don't have direct access to the outsourced basis at their reduced-degree level. They'd have to derive their level's `g01` from a chain of split operations, which costs more memory than just keeping `l10` in scratch. So we apply Layer 3 *only* at the outer call (`logn == ss->logn`), and inner recursion levels keep their `l10` in `tmp[]` as before.

**Bytes saved.** 1n fpr = **8n bytes = 4 KiB at FN-DSA-512 / 8 KiB at FN-DSA-1024**.

**Cost.** ~11n FP ops + 1 LDL division per sign. On hosted hardware with an FPU this is ~3 µs at n=1024 — negligible. On Cortex-M3 (no FPU; FP is software-emulated) it's closer to 1–2 ms, still a small fraction of the sign's total time.

**Why it requires the basis to be outsourced.** The recomputation reads `b00, b01, b10, b11` directly via flash pointers. Without outsourced basis, we'd have nothing to recompute from — the only way to get `l10` back would be to redo the entire setup phase, which obviously defeats the purpose.

---

## Phase 5 — hm-recompute (the necessary follow-up to Layer 3)

Layer 3 has a subtle side effect that has to be addressed.

**The problem.** Before Layer 3, `hm` (the hash-to-point output, n × uint16 = 2n bytes) lived at byte offset `48n` of `tmp[]`. There was plenty of room because ffsamp's outer body topped out around `qc(15)` = byte `30n` and never reached `48n`.

After Layer 3, the outer body's footprint is tighter: it uses fewer slots. But to actually shrink `tmp[]`, we'd want to move `hm` down to byte `32n` and free up `tmp[34n..50n)`. The problem: the **deep recursion's working set** extends from the outer callee's start position upward by ~21n callee-bytes. With the new tighter outer layout, that footprint reaches byte ~33n — which **overlaps `hm`'s new slot at 32n**.

The deep recursion clobbers part of `hm` during ffsamp.

**The fix (Phase 5).** Don't fight the overlap. Let the deep recursion clobber `hm`. Then, after ffsamp returns and before the post-ffsamp step reads `hm`, run `hash_to_point` again to recompute the same 2n bytes.

```c
ffsamp_fft(...)                  // may clobber hm
...
hash_to_point(..., hm)           // refresh hm before s1/s2 computation
...read hm into s1...
```

The hash function is deterministic on the same inputs, so the refreshed `hm` matches what was originally there.

**Bytes saved.** Phase 5 lets `tmp[]` shrink by 2n bytes — the slack that used to surround `hm` to keep it safe from the deep recursion.

**Cost.** One extra `hash_to_point` call per sign. `hash_to_point` is essentially a SHAKE-256 absorption + extraction over a few hundred bytes. On Cortex-M3 that's ~1–2 ms; on hosted hardware, microseconds.

**Why it depends on Layer 3.** Without Layer 3 the deep recursion never threatens `hm`, so there's no need to recompute. Phase 5 is a follow-up that *enables* the layout compaction Layer 3 makes feasible.

---

## B2 — basis-direct post-ffsamp (SIMD path only)

Up to this point in Bucket B we've been focused on `tmp[]` during ffsamp itself. There's an entirely separate cleanup to do at the **post-ffsamp** stage — the lattice point computation that produces `s1` and `s2`.

**Background.** After ffsamp produces the sampled vector `[t0, t1]`, the signature requires computing:

```
v0 = t0 · g + t1 · G
v1 = -t0 · f - t1 · F
s1 = hm - v0
s2 = -v1
```

The reference implementation reconstructs `g, f, G, F` in NTT representation from the int8 secret-key polynomials by:

1. Decoding f, g, F from the encoded sign_key (writing int8 arrays into `tmp[]`)
2. Reconstructing G via NTT division (the same step that's done once during basis precompute)
3. Running 4 forward FFTs (`g, f, G, F` → FFT form)
4. Multiplying in FFT form and inverse-transforming back

That's *a lot* of work to reconstruct values that, with outsourced basis, are already sitting in flash in FFT form: the basis components are `b00 = FFT(g)`, `b01 = FFT(-f)`, `b10 = FFT(G)`, `b11 = FFT(-F)`.

**The B2 trick.** When the basis is outsourced, do the lattice point computation directly against the FFT-form basis:

```
v0 = t0 · b00 + t1 · b10   (= t0 · g + t1 · G, since b00 = FFT(g) and b10 = FFT(G))
v1 = t0 · b01 + t1 · b11   (= t0 · (-f) + t1 · (-F) = -(t0 · f + t1 · F))
```

This skips:
- the int8 redecode of f and g (2n bytes of work)
- the NTT reconstruction of G (n bytes of work, plus the NTT primitives)
- 4 forward FFTs (significant CPU)

It also eliminates `G[]` from `tmp[]` entirely. In the reference / Bucket A path, `G[]` lives at byte offset `36n` of `tmp[]` because the post-ffsamp NTT-domain path reads it. With B2 we don't read `G[]` during sign at all (we're using `b10` directly).

**Bytes saved.** n bytes = the `G[]` slot. Modest in absolute terms, but it's exactly the difference between `35n+31` and `34n+31`.

**Perf cost.** Actually faster — we *skip* work. On a host with FPU, measured 5–11% faster sign vs. Layer-2 + Layer-3 alone, depending on parameter set and SIMD width.

**Why "SIMD only".** The post-ffsamp computation involves multiplying polynomials in FFT form. FFT form means 8-byte IEEE doubles. On hardware with an FPU + SIMD, multiplying doubles directly is fast. On scalar hardware without an FPU, *software-emulated* doubles are much slower than the integer-NTT path the reference implementation uses. So scalar builds keep the integer-NTT post-ffsamp; B2 is only enabled on builds where the FP path is actually fast.

In code, B2 is gated by `FNDSA_SSE2 || FNDSA_NEON || FNDSA_RV64D`.

---

## B3 — G outsourced (scalar parallel of B2)

Scalar builds can't use B2 — the FP-direct path is too slow without an FPU. So they need a different way to get `G[]` out of `tmp[]`.

**The B3 trick.** When precomputing the basis at provisioning, also precompute `G` (an n-byte int8 polynomial) and store it separately in the caller's outsourced storage. The new API:

```c
int fndsa_compute_basis_and_G(
    unsigned logn,
    const void *sign_key, size_t sign_key_len,
    void *basis_buf, size_t basis_len,    /* 32n bytes */
    void *G_buf, size_t G_len);           /* n bytes */

size_t fndsa_sign_with_basis_and_G_temp(
    /* ... usual args ... */
    const void *basis,
    const void *G,                        /* read-only, n bytes int8 */
    /* ... */);
```

At sign time, the scalar post-ffsamp NTT-domain path reads G via the caller-supplied pointer instead of from `tmp[]`. The G derivation step in `sign_step1` is also skipped (we'd just be regenerating bytes that are already in flash).

**Bytes saved.** n bytes of `tmp[]` (the `G[]` slot at offset 36n).

**Flash cost.** n bytes = 512 bytes at FN-DSA-512, 1024 bytes at FN-DSA-1024. Trivial.

**Perf cost.** Saves ~5–10 ms of NTT work per sign (the `G = h*F mod q` reconstruction that scalar previously had to do every sign).

**Combined with Phase 5.** B3 alone gets us to `36n+31`. To reach the same `34n+31` as the SIMD-with-B2 path, scalar also needs Phase 5's hm-recompute (which works in scalar too — the algorithm doesn't care). The combination `Layer 2 + Layer 3 + B3 + Phase 5` is the canonical scalar Bucket B configuration.

---

## Putting Bucket B together: 17 KiB at FN-DSA-512

The empirical measurement from `test_milestones.c` on the current branch:

```
SIMD build:
  + with_basis              logn=9   high-water = 17,408 B   = 34.00n   = 17.00 KiB
  + with_basis_and_G        logn=9   high-water = 17,408 B   = 34.00n   = 17.00 KiB

Scalar build:
  + with_basis              logn=9   high-water = 18,944 B   = 37.00n   = 18.50 KiB
  + with_basis_and_G        logn=9   high-water = 17,408 B   = 34.00n   = 17.00 KiB
```

A few things to read out of this.

**The scalar `with_basis` row at 37n+31** is the "19 KiB stop": Layer 2 + Layer 3 + Phase 5, without B3 (G still derived per-sign into `tmp[+36n..]`). 18.5 KiB. The natural design point for a scalar Cortex-M build that wants flash-resident basis but doesn't want to also outsource G.

**The SIMD `with_basis` row at 34n+31** already shows B2 active — on SIMD, even without explicit G outsourcing, the FP-direct post-ffsamp doesn't read `G[]` so it can be omitted from `tmp[]`. SIMD reaches 17 KiB just from outsourcing the basis.

**The scalar `with_basis_and_G` row at 34n+31** is B3 + Phase 5 doing the work — it takes the explicit G outsourcing API to match SIMD's 17 KiB.

**The convergence at 34n+31 / 17 KiB** is the Bucket B endpoint for both SIMD and scalar. It's the point where further compression requires Bucket C's tree outsourcing.

### Why ffsamp's outer body has stopped getting tighter

In Part 1 we walked through how Bucket A's recursive body restructure (A.2) brought the recursive ffsamp body's working set from 7n fpr down to 5n fpr at the outer level. Layer 3 then drops `l10` from the outer's persistent set, taking the outer body to 4n fpr.

At this point ffsamp's outer footprint can't shrink any further without committing to more outsourced state. The reason: the recursive callee at logn-1, sitting inside the outer's frame, still uses the on-the-fly LDL decomposition. At every level it calls `fpoly_LDL_fft` to derive `l10, d00, d11` from the incoming Gram. Those derived values have to live somewhere in `tmp[]`. That's the working set we can't compress without outsourcing.

Bucket C's idea is to outsource the entire LDL decomposition — precompute `(l10, d00, d11)` for every node of the recursion tree once at provisioning, store the tree, and skip `fpoly_LDL_fft` entirely at sign time. That's the next post.

---

## On the 32 KiB SE budget

With Bucket B's full stack applied:

- **FN-DSA-512** uses 17 KiB of `tmp[]` + ~1.5 KiB stack + ~1 KiB app BSS ≈ **20 KiB total**. Plenty of headroom on a 32 KiB SE.
- **FN-DSA-1024** uses 34 KiB of `tmp[]` alone — **still over the 32 KiB budget**, even before adding stack. Bucket B is not enough for FN-DSA-1024.

For FN-DSA-1024 we need Bucket C. (And as we'll see in Part 3, even Bucket C lands at the edge — fitting FN-DSA-1024 on a 32 KiB cap is a question of how many additional KiB you can shave off stack and app-level BSS.)

---

## Where the perf goes

This series leads with RAM savings, but Bucket B is also a **perf win** in absolute terms:

- The setup phase shrinks dramatically. Computing the basis at provisioning happens once; every sign skips the ~5–8 ms of `basis_to_FFT + gram_fft` work.
- B2 (SIMD) skips the 4 forward FFTs in post-ffsamp.
- B3 (scalar) skips the `G = h*F mod q` derivation per sign.
- Layer 3 *adds* a small recomputation (~11n FP ops at the outer level), so it's a wash on hosts and a slight loss on Cortex-M3.
- Phase 5 adds one extra `hash_to_point` per sign (~1–2 ms on M3, microseconds on hosts).

Net on Cortex-M3 (no FPU; software FP), measured: roughly perf-neutral. The setup savings and the B3 G-precompute savings roughly cancel out Layer 3's FP overhead and Phase 5's hash call.

Net on aarch64 with NEON: perf-neutral to ~5–11% faster, depending on parameter set. The B2 path's elimination of 4 forward FFTs is the dominant win.

We treat Bucket B as a "RAM optimization that happens not to cost perf". For most production deployments that's exactly the right framing — you adopt it for the RAM win and get to ignore the perf side.

---

## What's coming in Part 3

Part 3 — the final one — tackles **Bucket C: outsource the LDL tree**. The tree is a much bigger commitment: `(logn-1) · 16n` bytes, which is **64 KiB at FN-DSA-512** and **144 KiB at FN-DSA-1024**. In exchange:

- **Tree-reading ffsamp (B0)** — the recursion no longer does `fpoly_LDL_fft` at each level; it reads pre-decomposed `(l10, d00, d11)` directly from the tree. Perf only (no RAM win), but the perf win is large: roughly 10× faster sign per zknox's published numbers (which match our own Speculos measurements).

- **Compact tree-only outer body** — the marquee RAM optimization. When the outer body knows it doesn't have to materialize `l10, d00, d11` (they come from the tree), we can redesign its qc-slot layout to be tighter. The callee tmp pointer shifts from `qc(6)` to `qc(4)`, the d00 storage slot vanishes, the d11 scratch slot vanishes. ffsamp's outer peak drops from 4n fpr to 3n fpr. `tmp[]` reaches **30n+31 = 15 KiB at FN-DSA-512** on scalar (16 KiB on SIMD — we'll dig into why the SIMD ceiling is 2n higher).

- **The deployment matrix** — where the outsourced state physically lives. Flash (build-time `.rodata` bake), NVRAM (provisioning-time writes), host-streamed (zknox's encrypted-stream architecture), or even RAM. Each is a different trade-off; the library code is the same.

- **FN-DSA-1024 fit** — the honest answer on whether 30 KiB of `tmp[]` plus stack plus BSS fits a 32 KiB SE. Spoiler: it's tight, fixable with modest engineering on the deployment side, and worth a separate look at where the algorithmic floor actually is.

---

**Code references.** The Bucket B optimizations are in [`sign_core.c`](sign_core.c) (setup-phase rewrite, hm offset logic, B2 post-ffsamp branch), [`sign_sampler.c`](sign_sampler.c) (outer-level l10 drop), [`sign.c`](sign.c) (sign_step1 G-derivation gating, min_tmp_len wrapper logic), and [`sign_fpoly.c`](sign_fpoly.c) (the `_external` primitive variants: `fpoly_apply_basis_external`, `fpoly_g01_fft_external`, `fpoly_pathb_finalize_external`). API surface is in [`fndsa.h`](fndsa.h).

The measurement test that produced this post's numbers is [`test_milestones.c`](test_milestones.c) — same harness used in Part 1, exercising all four sign API entry points.

---

*Part 3 (final): Bucket C, the deployment matrix, and FN-DSA-1024 on 32 KiB.*
