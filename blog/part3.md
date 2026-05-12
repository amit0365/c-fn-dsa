# FN-DSA RAM optimizations on constrained devices, Part 3: outsource the tree, and fit FN-DSA-1024

*From 17 KiB to 15 KiB by outsourcing the LDL tree. Plus the deployment matrix, validation methodology, and an honest look at whether FN-DSA-1024 fits on 32 KiB.*

---

## Where we left off

[Part 1](./part1.md) brought us to `51n+31` (25.5 KiB at FN-DSA-512) with zero outsourcing.

[Part 2](./part2.md) introduced **Bucket B**: outsource the lattice basis. Combined with Layer 3's outer `l10` drop, B2/B3's post-ffsamp cleanups, and Phase 5's hm-recompute, we reached `34n+31` (**17 KiB at FN-DSA-512** for both SIMD and scalar paths).

That was enough to fit FN-DSA-512 comfortably on a 32 KiB SE. It is **not** enough to fit FN-DSA-1024, which sits at 34 KiB of `tmp[]` alone.

Bucket C outsources the **LDL tree** — the full precomputed decomposition that ffsamp's recursion walks at every level. It buys us a 10× perf win on signing, plus another 4n bytes of RAM through a layout restructure that becomes possible once the tree is outside `tmp[]`.

End state, empirically measured: **`30n+31` = 15 KiB at FN-DSA-512 (scalar)** and `32n+31` = 16 KiB SIMD. At FN-DSA-1024 the same numbers in absolute terms become 30 KiB / 32 KiB — right at the 32 KiB SE budget cap.

---

## The LDL tree

ffsamp's recursion is a depth-`logn` binary tree. At every internal node it performs an LDL decomposition:

```
Given:   the level's Gram polynomials g00, g01, g11
Compute: l10 = g01 / d00       (an off-diagonal value)
         d00, d11               (the diagonal values; d00 = g00 unchanged,
                                 d11 = g11 - l10·conj(l10)·g00)
```

These three polynomials are needed at every level to drive the Gaussian sampling. In the standard recursion (Bucket A or B), `fpoly_LDL_fft` is called at every node and the result `(l10, d00, d11)` lives in `tmp[]` for the duration of that level's body.

**The Bucket C idea.** The LDL tree depends only on the private key, not on per-sign randomness. So precompute it once at provisioning, walk the entire recursion in advance, store every node's `(l10, d00, d11)` to a flash buffer in a level-major BFS order, and on every sign — read the pre-decomposed values directly instead of running `fpoly_LDL_fft`.

The tree is sized for the full recursion at every level:

| Level k | nodes at level | size per node | level size |
|---|---|---|---|
| logn | 1 | 2n fpr (l10 full + d00 half + d11 half) | 2n fpr |
| logn-1 | 2 | n fpr | 2n fpr |
| logn-2 | 4 | n/2 fpr | 2n fpr |
| ... | ... | ... | ... |
| 2 | 2^(logn-2) | 4 fpr | 2n fpr |

Each level holds `2n fpr` = `16n bytes` regardless of depth, and there are `logn-1` such levels:

```
tree_size(logn) = (logn - 1) · 16n bytes
```

| Parameter set | logn | n | tree size |
|---|---|---|---|
| FN-DSA-512 | 9 | 512 | 8 · 16·512 = 64 KiB |
| FN-DSA-1024 | 10 | 1024 | 9 · 16·1024 = 144 KiB |

That's a significant flash commitment — and it's why Bucket C is reserved for hardware where flash is plentiful relative to RAM.

The API surface:

```c
/* Precompute, once at provisioning. Caller must already have basis. */
int fndsa_compute_ldl_tree(
    unsigned logn,
    const void *basis, size_t basis_len,
    void *tree_buf, size_t tree_buf_len,    /* writes (logn-1)·16n bytes */
    void *tmp, size_t tmp_len);

/* Per-sign — caller passes basis, G, and tree pointers. */
size_t fndsa_sign_with_basis_G_and_tree_temp(
    /* ... usual args ... */
    const void *basis, const void *G, const void *tree,
    /* ... */);
```

The tree's layout is level-major BFS: all level-`logn` nodes first, then level-`logn-1`, and so on. At sign time the library indexes into it by `(level, node_index)`.

---

## C.1 — Tree-reading ffsamp (perf only)

The first thing the outsourced tree buys us is a **massive perf win**, with no immediate RAM impact.

**What it does.** At every recursion level inside `ffsamp_fft_inner`, replace:

```c
fpoly_LDL_fft(level, qc(12), qc(8), qc(14));    // decompose g00, g01, g11 in place
                                                 // → l10 at qc(8..11), d00 at qc(12..13), d11 at qc(14..15)
```

…with a direct read from the tree:

```c
const fpr *node = (const fpr *)(ss->external_tree + level_base + node_index * per_node);
const fpr *tree_l10 = node;
const fpr *tree_d00 = node + n_at_level;
const fpr *tree_d11 = node + n_at_level + (n_at_level >> 1);
```

Then the subsequent operations read from the tree pointers directly instead of from `qc(8..15)`.

**Why it's a perf win.** `fpoly_LDL_fft` is expensive — it's a polynomial division by `d00` followed by a multiply-accumulate to derive `d11`. At the outer level it's `O(n)` doubles' worth of FP work, repeated at every node of the recursion tree. The total LDL work across the entire ffsamp tree is comparable to the rest of the recursion combined.

zknox's [Falcon streaming blog post](https://zknox.eth.limo/posts/2026/04/30/falcon_streaming.html) reports the unprotected Falcon-512 sign dropping from 8.5 seconds to 840 milliseconds — a **~10× speedup** — when the LDL tree is precomputed instead of derived per sign. We see comparable results on our Speculos bench app (Bucket C's INS_RUN_BENCH for 1 sign comes in around 56 ms at FN-DSA-512, vs. several hundred ms when LDL is on-the-fly).

**RAM impact: none yet.** The tree path *shares the same qc-slot layout* as the on-the-fly path. The slots `qc(4..7)` (l10) and `qc(8..9)` (d00) are still reserved by the layout, even though the tree path never writes to them. This is by design — the recursive body keeps a single code path with `if (tree_l10 != NULL)` branches, sharing structure between basis-only and basis+tree cases.

The actual RAM savings come from a second optimization built on top of the tree.

---

## C.2 — Compact tree-only outer body (the marquee 4n savings)

This is the optimization that took FN-DSA-512 from 17 KiB to 15 KiB on scalar.

**The observation.** With both basis and tree outsourced, the outer ffsamp body never needs to write to several slots in its qc layout. Specifically:

- **`qc(4..5)` — the d00 preservation slot.** In the basis-only path, after the LDL decomposition we move `d00` here so it survives the right recursion (which clobbers the original `qc(12..13)`). In the tree path, `d00` lives in flash; we read it fresh for the left rec.
- **`qc(14..15)` — the d11 scratch slot.** In the basis-only path, we save `d11` here before the right rec's `fpoly_split_selfadj_fft` (which overwrites the source). In the tree path, we never touch this — we split `tree_d11` directly to produce the right callee's Gram inputs.

Two slots that are byproducts of layout-sharing. With tree, they're dead weight.

**The restructure.** Build a separate outer-body code path that activates only when `external_tree != NULL`, with a tighter qc layout:

```
Slot       Basis-only outer body         |  Compact tree-only outer body
----       ----------------------          ------------------------------
qc(0..3)   c1                              c1                            (unchanged)
qc(4..5)   d00 (preserved across rec)      callee_t0 (= even half of t1)
qc(6..7)   callee_t0 (= even half of t1)   callee_t1 (= odd half of t1)
qc(8..9)   callee_t1 (= odd half of t1)    callee_g01
qc(10..11) callee_g01                      callee_g00
qc(12..13) callee_g00, callee_g11          callee_g11
qc(14..15) d11 scratch                     [free / callee work area]
qc(16..27) callee work area                callee work area
```

Two things change:

1. **`qc(4..5)` is reassigned** from "d00 preservation" to "callee_t0" — the first piece of the recursive callee's input. We're slotting the callee's frame 2 qc-positions earlier than before.
2. **`qc(14..15)` becomes free**. Nothing writes there.

The downstream effect is that **the callee tmp pointer shifts from `qc(6)` to `qc(4)`**. The recursive call:

```c
ffsamp_fft_inner(ss, logn - 1, qc(4), 0);   // was: qc(6)
```

That means the callee's frame starts at byte `8n` of `tmp[]` instead of byte `12n`. The callee's footprint (empirically ~21n callee-bytes at logn = 9) used to reach byte `12n + 21n = 33n`; now it reaches `8n + 21n = 29n`.

**Outer ffsamp peak shrinks from 32n bytes (4n fpr) to 28n bytes (3.5n fpr).**

**Why `hm` moves.** With ffsamp's peak at byte 28n instead of byte 32n, the `hm` slot can move down too — from `hm_offset_n = 32` (its position in Bucket B) to `hm_offset_n = 28`. Phase 5's hm-recompute already lets `hm` overlap with ffsamp during execution (it gets clobbered and refreshed); the slot just moves to where ffsamp's new high-water mark is.

**Combined `tmp[]` minimum (scalar):** `30n+31`. The 30n decomposes as:

```
[0..28n)   ffsamp peak (outer body's writes + deep recursion's footprint)
[28n..30n) hm (recomputed post-ffsamp by Phase 5)
```

**Bytes saved.** 4n bytes from Bucket B's `34n+31` to Bucket C's `30n+31` on scalar. That's:

- **2,048 bytes (2 KiB) at FN-DSA-512.**
- **4,096 bytes (4 KiB) at FN-DSA-1024.**

For FN-DSA-1024 specifically this is the difference between 34 KiB and 30 KiB of `tmp[]` — the difference between "doesn't fit a 32 KiB SE at all" and "fits if we're careful with stack and BSS".

### The choreography in more detail

The compact tree-only outer body's actual ffsamp code is a careful dance. For readers who want to see the bookkeeping: here's the sequence at the outer call when `external_tree != NULL`:

```c
// Read pre-decomposed tree node directly from flash
const fpr *tree_l10 = (const fpr *)(ss->external_tree + ...);
const fpr *tree_d00 = tree_l10 + n;
const fpr *tree_d11 = tree_l10 + n + n/2;

// c1 = t0 + t1 · l10, in place at qc(0..3)
fpoly_muladd_fft(logn, qc(0), qc(4), tree_l10);

// Save t1 from qc(4..7) into qc(8..11), split into callee inputs
memcpy(qc(8), qc(4), sizeof(fpr) << logn);
fpoly_split_fft(logn, qc(4), qc(6), qc(8));   // qc(4..5) = even half, qc(6..7) = odd half

// Build right-rec callee's Gram inputs from tree_d11
fpoly_split_selfadj_fft(logn, qc(10), qc(8), tree_d11);   // qc(10) = g00, qc(8..9) = g01
memcpy(qc(11), qc(10), ...);                              // qc(11) = g11 (= g00 copy)

// Right recursion. Callee tmp = qc(4): callee's frame starts at byte 8n.
ffsamp_fft_inner(ss, logn - 1, qc(4), 0);

// Post-rec: tb0 = c1 - z1·l10; produces tb0 at qc(0..3) and merged z1 at qc(8..11)
fpoly_pathb_finalize_external(logn, qc(0), qc(8), qc(4), qc(6), tree_l10);

// Split tb0 into left-rec callee inputs
fpoly_split_fft(logn, qc(4), qc(6), qc(0));

// Move z1 from qc(8..11) to qc(0..3) so it survives left rec
memcpy(qc(0), qc(8), sizeof(fpr) << logn);

// Build left-rec callee's Gram inputs from tree_d00
fpoly_split_selfadj_fft(logn, qc(10), qc(8), tree_d00);
memcpy(qc(11), qc(10), ...);

// Left recursion
ffsamp_fft_inner(ss, logn - 1, qc(4), 1);

// Merge callee output → z0 at qc(8..11), then shuffle to output positions
fpoly_merge_fft(logn, qc(8), qc(4), qc(6));
memcpy(qc(4), qc(0), sizeof(fpr) << logn);   // z1 → qc(4..7)
memcpy(qc(0), qc(8), sizeof(fpr) << logn);   // z0 → qc(0..3)
```

The key tricks:

- We avoid the d00 save/restore dance because `tree_d00` is in flash, readable directly when the left rec needs it.
- We avoid the d11 scratch entirely because `fpoly_split_selfadj_fft` reads `tree_d11` directly from flash (one of the `_external` primitive variants).
- The aliasing constraint on `fpoly_pathb_finalize_external` (`c1`, `z_out`, `zlow`, `zhigh` must be pairwise disjoint) is satisfied by the new slot assignments: `c1 = qc(0..3)`, `z_out = qc(8..11)`, `zlow = qc(4..5)`, `zhigh = qc(6..7)`.

The code lives in [`sign_sampler.c:1305–1397`](sign_sampler.c#L1305). It's about 90 lines, walking through the choreography step by step with comments.

---

## Why SIMD diverges to `32n+31`

The compact tree-only outer body gets us to **`30n+31` on scalar but only `32n+31` on SIMD**. That 2n gap is structurally interesting and worth unpacking — it's the cost of SIMD's FPU-friendly post-ffsamp algorithm.

The post-ffsamp step computes `s1 = hm - (g·t0 + G·t1)` and `s2 = -(f·t0 + F·t1)`. There are two completely different implementations:

**Scalar (Cortex-M3, no FPU)**: integer-NTT domain. Converts `t0, t1` from FFT-doubles to int16 in place. Then all multiplications happen in NTT mod q = 12289. Working buffers `ut0..ut3` are each `2n bytes` (uint16). Total post-ffsamp scratch: `[0..26n)`.

**SIMD (x86 SSE2 / ARM NEON / RISC-V D, with hardware FPU)**: FP-domain (B2 from Part 2). Multiplies `t0, t1` directly against the FFT-form basis polynomials. Working buffers `w0, w1` save the original `t0, t1` (needed twice each, once for v0 and once for v1). Each is `n fpr = 8n bytes`. Total post-ffsamp scratch: `[0..32n)`.

| Metric | Scalar (int-NTT) | SIMD (FP-domain B2) |
|---|---|---|
| Coefficient size | 2 bytes (uint16) | 8 bytes (fpr) |
| Number of working buffers | 4 (ut0..ut3) | 2 (w0, w1) |
| Total post-ffsamp `tmp[]` | 26n bytes | 32n bytes |
| Slowest path on Cortex-M3 | 5–8 ms | ~50 ms (FP emulation) |
| Slowest path on aarch64 | ~3 ms (integer ALU) | ~1 ms (NEON SIMD) |

The 4× coefficient-size penalty on SIMD is the binding ceiling. `w1` ends at byte `32n`. Even if ffsamp's peak drops to byte 28n, `tmp[]` can't go below `32n+31` on SIMD because of `w1`. The library's wrapper enforces this:

```c
if (tree != NULL) {
#if FNDSA_SSE2 || FNDSA_NEON || FNDSA_RV64D
    min_tmp_len = ((size_t)32 << logn) + 31;
#else
    min_tmp_len = ((size_t)30 << logn) + 31;
#endif
}
```

**Why we don't "fix" SIMD to also hit 30n.** Three options exist and none are worth taking:

1. *Convert SIMD's post-ffsamp to integer-NTT* — that's literally undoing B2, losing the 4 FFTs of work we skip. ~5–11% slower sign for 2n of RAM that SIMD hosts have in abundance.

2. *Pack `w0, w1` into a smaller buffer* — they're used as live FFT-domain operands; no obvious encoding shaves 4× without breaking the algorithm.

3. *Stream `w0, w1` to extra scratch and recompute as needed* — adds latency, complex correctness reasoning, marginal savings.

The 30n/32n split is the natural answer: scalar wins both speed and RAM on M-class hardware where FP is emulated; SIMD wins speed on hosts with an FPU and willingly pays 2n bytes of `tmp[]` for the privilege.

---

## The full journey, side-by-side

Empirical numbers from `test_milestones.c`, with both formula and concrete KiB:

| Milestone | Outsourced state | `tmp[]` formula | FN-DSA-512 (KiB) | FN-DSA-1024 (KiB) | Δ vs prev |
|---|---|---|---|---|---|
| Baseline (reference impl) | — | `59n+31` | 29.5 | 59 | — |
| Bucket A endpoint | — | `51n+31` | 25.5 | 51 | −4 / −8 |
| Bucket B step 1 (+ basis, scalar) | basis (32n flash) | `37n+31` | 18.5 | 37 | −7 / −14 |
| Bucket B step 2 (+ B3/B2 + Phase 5) | basis (+ n for B3) | `34n+31` | 17 | 34 | −1.5 / −3 |
| Bucket C (+ tree, compact outer, **scalar**) | basis + tree ((logn-1)·16n flash) | `30n+31` | **15** | **30** | −2 / −4 |
| Bucket C (+ tree, compact outer, **SIMD**) | basis + tree | `32n+31` | 16 | 32 | −1 / −2 |

The cumulative reduction from baseline:

- **FN-DSA-512**: 29.5 KiB → 15 KiB. **49% reduction.**
- **FN-DSA-1024**: 59 KiB → 30 KiB. **49% reduction.**

The flash cost at FN-DSA-1024: 32 KiB (basis) + 1 KiB (G) + 144 KiB (tree) = **177 KiB**. That's a lot of flash. For Ledger devices it's available; for some IoT targets (e.g., Cortex-M0+ with 16 KiB flash) it's not.

---

## Validation methodology

Throughout the series we've been quoting empirically measured numbers. Here's how we measure.

### Sentinel paint-and-check

The core test technique is straightforward and surprisingly powerful:

```c
memset(tmp, 0xA5, tmp_len);            // fill with sentinel
fndsa_sign_seeded_with_basis_G_and_tree_temp(
    sk, sk_len, basis, G, tree,
    /* ... */, tmp, tmp_len);
// scan tmp[] for highest byte != 0xA5
size_t peak = 0;
for (size_t i = tmp_len; i > 0; i--) {
    if (tmp[i - 1] != 0xA5) { peak = i; break; }
}
```

`peak` is the actual high-water mark — the highest byte the sign code modified. False negatives are possible (a written value could happen to be 0xA5) but they're statistically negligible at ~1/256 per byte; bytes in our painted range are 99%+ accurate.

For finer analysis (e.g., looking for unwritten "clean bands" inside the painted region), we scan for runs of consecutive sentinel bytes ≥ N bytes long. That's how we measured that the compact tree-only outer body leaves bytes `[28n..32n)` clean inside `tmp[]` — confirming the layout savings before the wrapper allowed us to actually shrink `tmp_len`.

### Bit-exact KAT cross-check

Every optimization we ship is verified to produce **bit-identical signatures** vs. the baseline. The test harness ([`test_basis_G_tree.c`](test_basis_G_tree.c), [`test_tree_stress.c`](test_tree_stress.c)) signs the same message with the same seed via two API paths — baseline + new — and `memcmp`s the result.

Scale of the cross-check:

- KAT_512 and KAT_1024 from the NIST submission (deterministic test vectors).
- 1000 random seeds at logn 2..8 (the test_kat_stress harness from upstream).
- 100 random seeds at logn=9.
- 20 random seeds at logn=10.
- 50 random seeds × {SIMD, scalar} × {logn=9, logn=10} for the with-tree paths specifically.

KAT mismatch is fatal; the build doesn't pass. We've caught real bugs this way — e.g., a sub-tree node-index off-by-one during the tree-reading recursion that would have produced silently-incorrect signatures with valid-looking byte sizes.

### ASAN

The whole test suite runs under AddressSanitizer (`-fsanitize=address -O1 -g`). ASAN catches buffer over-reads and over-writes in the `tmp[]` arena that the wrapper's `min_tmp_len` check might miss. The sk-encoding wrappers, the recursive ffsamp body, and the `_external` primitive variants all run clean under ASAN's per-allocation guards.

### Speculos on Ledger Flex

End-to-end validation runs the bench app inside [Speculos](https://github.com/LedgerHQ/speculos) (Ledger's official emulator) targeting the Flex device profile. The bench app:

1. Provisions basis, G, and tree to NVRAM (handler `INS_PROVISION`)
2. Signs the KAT_512 message N times (handler `INS_RUN_BENCH`)
3. SHA3-256-hashes the produced signature and returns the hash to the host

For reproducibility, the host script signs three times and asserts the hash is identical across runs. The expected hash for KAT_512 with our seed is `4abaa270…6672b7` — any code change that produces a different hash means we broke determinism.

This caught one real issue during development (the Path B reorder vs. SHAKE context positioning at logn=2 buffer overlap discussed earlier in the project history); the existing code base now passes 500 consecutive reproducibility runs on Speculos without a single failure.

---

## The deployment matrix

The Lego model in Parts 1 and 2 emphasized that the library is **location-agnostic** — `basis`, `G`, `tree` are all `const void *` pointers, and the library doesn't care where the bytes physically sit. This section is the deployment guide for *where* to put them.

| Storage option | RAM cost at sign time | Setup overhead | Suitable for |
|---|---|---|---|
| **Flash (`.rodata`, build-time bake)** | 0 | one-shot at build | fixed-key apps (bench, single-key wallets, signing oracles) |
| **NVRAM (writable nonvolatile)** | 0 (read via flash pointer at sign) | one-shot at keygen via `nvm_write` | provisioned-once apps, HD-derived parent keys |
| **Host-streamed** (encrypted + MAC'd) | only the active path (~24 KiB at FN-DSA-1024) | per-sign streaming latency (~12 s on USB for FN-DSA-1024) | RAM-poor SEs, untrusted-host-friendly deployments (zknox's approach) |
| **Plain RAM** | full size during sign | recompute per session if not persistent | desktop / cloud / RAM-rich servers |

**Flash deployment** is what our bench app uses: a host-side generator (`tools/gen_ldl_tree.c`) reads the KAT private key, computes the basis + G + LDL tree, and emits C source containing the result as `const uint8_t` arrays. The arrays go into `.rodata` (flash) of the device app, and the library reads them via the `PIC()` macro at sign time. Zero NVRAM writes ever, zero per-key setup at first boot. Best for fixed-key deployments because the tree precompute happens at build time, not at runtime.

**NVRAM deployment** is what production wallets with provisioned keys would use. At first wallet unlock, the device computes basis (~5–8 ms), G (~10 ms), and the LDL tree (~30 seconds for FN-DSA-1024 on Cortex-M3) and writes them to NVRAM via `nvm_write`. Subsequent signs read from NVRAM via flash pointer just like the build-time bake. The 30-second tree precompute is a one-time cost per key — acceptable as part of unlock or recovery, less acceptable per-sign.

**Host-streamed deployment** is zknox's design: the tree lives encrypted on the host machine, and the device streams individual nodes over USB during sign. The device decrypts, verifies a MAC binding each node to its position, uses, and discards. The active SRAM footprint is just the one path through the recursion (~24 KiB), not the whole tree. The trade-off is **sign latency**: ~12.7 seconds for Falcon-1024 unprotected, per zknox's measurements. For HD wallets that can't afford 144 KiB of per-key NVRAM, this is the way.

**Plain RAM** is the trivial answer for hosted environments. Sign once on a desktop, hold basis/tree on the heap, done. Mentioned here for completeness.

The right choice depends entirely on your hardware:

- **Ledger Flex / Stax with KAT-fixed bench app**: flash bake. 0 RAM cost, 0 setup time per sign.
- **Ledger Flex / Stax with a single provisioned PQC key**: NVRAM. Pay the 30-second tree precompute once at provisioning.
- **Ledger Nano S+ with HD-derived keys + FN-DSA-1024**: host-streamed. The SE doesn't have 144 KiB of free NVRAM, so the host carries it. Pay ~12 s per sign.
- **Future SEs with more RAM**: skip Bucket C entirely. Bucket B at 17 KiB is comfortable on a 48 KiB SE.

---

## FN-DSA-1024 on a 32 KiB SE: the honest answer

The most-asked question of this work: can we fit FN-DSA-1024 on a 32 KiB Ledger SE?

Detailed accounting:

| Component | FN-DSA-1024, scalar, Bucket C, sk also outsourced |
|---|---|
| `G_fndsa_tmp` (per-sign scratch) | 30,751 B = **30.0 KiB** |
| Sign-side stack peak (recursive ffsamp + shake_context 208 B + hashed_key 64 B + nonce/seed 96 B + locals at recursion depth 10) | ~1.7 KiB |
| App-level BSS (`G_io_*` 832 B + `G_bench` 44 B + headroom probe + alignment padding) | ~1 KiB |
| **Working RAM total** | **~32.7 KiB** |

That's ~700 bytes over a hard 32 KiB ceiling. **Not fitting** as-is, but fitting *almost*.

Three realistic ways to claw back the 700 bytes:

1. **Stack hygiene.** `shake_context` is 208 bytes; placing it on the stack at every sign means it's live during the entire recursion. Reusing the sampler's existing SHAKE context for hashed-key computation (small refactor) frees ~150 bytes of stack.

2. **App BSS trim.** Ledger apps usually have a much smaller I/O footprint than our bench app shows (the bench app's BSS is padded to 4-KiB page alignment by the linker; production apps trim this). Trimming the bench app's I/O buffers to the minimum APDU size and removing the headroom probe saves ~500 bytes.

3. **Overlay `G_fndsa_tmp` onto the I/O scratch** during sign. BOLOS uses a ~256-byte I/O buffer for incoming APDU data; signing doesn't read incoming APDUs mid-recursion, so this buffer can be temporarily borrowed. Saves another ~256 bytes.

Combined, these are well within engineering reach and would put FN-DSA-1024 at ~31.5–32 KiB — fits with a tight margin.

For a 30 KiB SE budget the answer is different: FN-DSA-1024 doesn't fit, and the next move would be either zknox-style host streaming or waiting for the next generation of SE hardware with more SRAM. For a 32 KiB SE budget, FN-DSA-1024 fits with the work in this series plus modest engineering on the deployment side.

---

## Where the algorithmic floor is

The work in this series brought ffsamp's outer-body persistent set from `7n fpr` (baseline) to `3n fpr` (Bucket C). Can we go further?

**Empirically: no, not without an algorithm change.** We tried the same restructure technique on the recursive body (instead of just the outer) and measured the slots that *should* be unused in the tree path. The measurement showed those slots are written by the deeper recursion — not because the body code writes there, but because the callee's frame extends through them. With the current ffsamp algorithm, the `c1` value (8n bytes, alive across the right recursion) plus the deep callee's footprint (empirically ~22n bytes at logn = 9) saturates the `tmp[]` envelope. The 30n floor is real.

**Structural levers that would shift the floor** (none currently in scope):

- **Reformulating ffsamp** to not require `c1` alive across recursion. The standard algorithm absorbs `t1·l10` into a single buffer that survives the right rec. Alternative formulations (e.g., bottom-up sampling, Klein-Gentry variants) have different memory profiles. Adopting one would be paper-level work — KAT incompatibility is essentially guaranteed.
- **Compressing `c1`.** It lives in FFT representation (8-byte fpr per coefficient). A more compact representation (4-byte single-precision floats, or fixed-point with calibrated precision) could halve the storage. But Falcon's sampler is precision-sensitive; the literature suggests 53-bit doubles are the minimum that doesn't degrade signature quality.
- **Host streaming for `c1`** between right and left rec. Same idea as zknox's encrypted tree streaming, applied to one specific buffer. Implementation complexity high; latency cost notable; not investigated yet.

For 32 KiB SE deployments the Bucket C floor is good enough. For 16 KiB SE deployments (Nano S+, smaller IoT) the natural next step is host streaming for both tree and `c1`, which is more or less zknox's design space.

---

## Code references and reproducing

The implementation lives in [c-fn-dsa](https://github.com/...) on the `ffsamp-tree-compact-3n` branch:

- **Bucket A**: `FNDSA_LOW_RAM=1` build flag enables all of it. Code in [`sign_core.c`](sign_core.c), [`sign_fpoly.c`](sign_fpoly.c), [`sign_sampler.c`](sign_sampler.c).
- **Bucket B**: API entry points `fndsa_sign_with_basis_temp` / `fndsa_sign_with_basis_and_G_temp` in [`fndsa.h`](fndsa.h). Caller provides precomputed basis (and G, for the scalar path). Implementation in `sign_core.c`'s setup phase + `sign_sampler.c`'s outer-level body.
- **Bucket C**: API entry point `fndsa_sign_with_basis_G_and_tree_temp`. Tree precomputation in [`kgen_ldl_tree.c`](kgen_ldl_tree.c). Compact tree-only outer body in `sign_sampler.c:1305–1397`.
- **Bench app** (Ledger Flex target): [`c-fn-dsa-ledger-bench`](https://github.com/...). Builds via Docker against the BOLOS SDK; signs the KAT_512 vector and reports a SHA3 hash of the signature for reproducibility checking.
- **Validation tests**: `test_milestones.c`, `test_tree_stress.c`, `test_tree_tmp_peak.c` for sentinel measurement; `test_fndsa.c` for the full upstream KAT suite.

To reproduce the headline numbers:

```bash
$ git clone <repo> && cd c-fn-dsa
$ git checkout ffsamp-tree-compact-3n
$ clang -DFNDSA_LOW_RAM=1 test_milestones.c <obj files> -o test_milestones
$ ./test_milestones
=== LOW_RAM build, SIMD post-ffsamp path ===

[FN-DSA-512]
  fndsa_sign_seeded_temp        logn=9   high-water = 26112 B = 51.00n = 25.50 KiB
  + with_basis                  logn=9   high-water = 17408 B = 34.00n = 17.00 KiB
  + with_basis_and_G            logn=9   high-water = 17408 B = 34.00n = 17.00 KiB
  + with_basis_G_and_tree       logn=9   high-water = 16384 B = 32.00n = 16.00 KiB
```

Switch to scalar by adding `-DFNDSA_SSE2=0 -DFNDSA_AVX2=0 -DFNDSA_NEON=0 -DFNDSA_RV64D=0` to the compile line; the with-tree row drops to `15360 B = 30.00n = 15.00 KiB`.

---

## Closing

The work this series describes brings FN-DSA-512 from 30 KiB of working RAM to 15 KiB on Cortex-M3-class hardware — a 50% reduction with zero algorithmic compromise (bit-exact KAT preserved at every step). FN-DSA-1024 lands at 30 KiB, just under a 32 KiB SE budget with modest deployment-side trimming.

The Lego architecture — Bucket A foundational refactors, Bucket B basis outsourcing, Bucket C tree outsourcing, Bucket D sk outsourcing — gives integrators a real choice. Pick the bucket that matches your hardware's flash/RAM ratio. The library doesn't care where outsourced state physically lives, so the same code paths support flash-bake, NVRAM-provisioning, host-streaming, and plain RAM deployments interchangeably.

The two interesting open frontiers we leave on the table:

- **The 30n floor.** Currently algorithmic. Closing it requires reformulating ffsamp or compressing `c1`. Both are paper-level investigations, not engineering work.
- **FN-DSA-1024 on 24 KiB SEs (Nano S+).** Bucket C alone doesn't fit; host-streaming the tree (and possibly the basis) does. Implementation is non-trivial but well-modeled by zknox's published architecture.

For most production deployments the bench app's design point — Bucket C with flash-bake on Ledger Flex/Stax — is the right one. Fast sign, comfortable headroom, simple deployment.

---

*This concludes the three-part series. Part 1 covered Bucket A; Part 2 covered Bucket B; this Part 3 covered Bucket C, deployment, and the FN-DSA-1024 fit. Questions, errata, and contributions are welcome on the [repository](https://github.com/...).*
