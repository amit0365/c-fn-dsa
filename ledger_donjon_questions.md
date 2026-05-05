---
name: Questions for Ledger / Donjon — FNDSA_PATH_B + Phase 1 deployment
description: Concrete questions whose answers gate Day 2+ implementation of phase 1 reduction (option 5 — precomputed basis in flash).
---

# Questions for Ledger / Donjon

Day 2 deliverable for the phase-1 reduction kill plan. The phase-1 design
(precomputed basis stored in flash via `N_`-prefixed globals) is sound on
paper but its deployability hinges on concrete answers to the questions
below. This doc is meant to be sent verbatim or near-verbatim to a
contact at Ledger / Donjon.

Context: I've shipped FNDSA_PATH_B in [pull-request-link-tbd] reducing
signing tmp[] from 59n+31 → 51n+31 bytes (saves 4 KiB at FN-DSA-512,
8 KiB at FN-DSA-1024). The follow-up phase-1 reduction would save an
additional 3/6 KiB by precomputing the lattice basis B = [[g, -f],
[G, -F]] once at key load and storing it as `N_basis` in flash, then
having `sign_core` read from `N_basis` instead of recomputing per sign.

## Masking

1. **Does the current Ledger FN-DSA build apply masking** (Boolean
   and/or arithmetic) to per-sign computation, or is the build
   currently the unmodified Pornin reference?

2. **If masked**, what order? (1st, 2nd, higher) Is the masking applied
   at the floating-point level (uncommon, hard) or at the integer-bridge
   points (Gaussian sampler input, signature post-compose)?

3. **If masked**, would the masking scheme need rework to handle a
   precomputed basis that's loaded from flash instead of recomputed
   per sign? (Our analysis suggests no — masking targets per-sign
   computation, not storage location, and gram_fft's basis-load pattern
   is the same either way — but want to confirm.)

4. **If not masked**: does Donjon plan to add FN-DSA masking before
   deployment? If so, on what timeline? (Influences whether option 5's
   PR should land before or after a masking layer.)

## Watchdog tolerance during NV writes

The phase-1 design provisions ~32 KiB of basis to flash at key load,
which is ~512 page-erase+program cycles. At 1-4 ms/page (literature
estimate for 40 nm SE eFlash) this is **0.5-2 seconds blocking** before
adding lockstep / countermeasure overhead.

5. **What is the watchdog interval on Stax / Flex / Nano S+ / Nano X /
   Nano Gen 5?** Per-device numbers welcome.

6. **Is `nvm_write()` watchdog-safe?** Specifically: does the syscall
   itself kick the watchdog internally, or must the application loop
   that calls `nvm_write()` repeatedly do its own kicking?

7. **Recommended pattern for batching ~500 page writes** within a single
   provisioning step. Is there an SDK helper for this, or do we need to
   `nvm_write()` in chunks with explicit watchdog kicks between?

## Per-app NV budget

8. **What is the per-app NV (`N_`) budget on each Ledger device?**
   Public docs don't publish this. Concrete numbers needed for: Stax,
   Flex, Nano S Plus, Nano X, Nano Gen 5.

9. **Is adding 32 KiB to the per-app NV footprint feasible** for a
   typical FN-DSA-1024 signing app? (16 KiB for FN-DSA-512.) The
   investigation found per-app slots in "tens to low hundreds of KiB,"
   so 32 KiB might be 30-50% of a slot.

10. **Does the per-app slot grow if NV usage grows**, or is it fixed at
    install time? (Affects whether existing installations can opt into
    option 5 in-place vs requiring reinstall.)

## Atomicity / journaling

`nvm_write()` is documented as not transactional ("possible data loss
if user powers off mid-write"). Our design uses a single 64-byte
"flag page" written LAST under `nvm_write()`'s page-atomicity guarantee.
On boot, missing/invalid flag → re-derive basis from key.

11. **Is single-page `nvm_write()` (64 bytes) atomic** at the SE
    hardware level? I.e., either the full 64 bytes are written, or none
    are — never a torn write?

12. **Does the SE provide any tear-resistance mechanism** beyond
    per-page atomicity? E.g., a hardware-supported journaling region
    or a "dirty bit" that survives reset?

13. **Recommended journaling pattern**: is there a Ledger-blessed
    pattern for atomic multi-page NV updates? AN5866 documents some
    primitives at the ST33 level but I'm not sure which apply through
    BOLOS.

## Speculos vs real hardware

14. **What does Speculos simulate accurately** for `nvm_write()`?
    I assume: functional correctness (writes persist across `nvm_*` calls
    within a session), MPU enforcement of `N_` regions. I assume: NOT
    timing, watchdog interaction, partial-write atomicity, or wear.
    Confirm?

15. **Recommended path for hardware benchmarking** of `nvm_write()`
    latency and gram_fft cache behavior with basis in flash. Does
    Donjon have a profiling harness, or do we instrument with GPIO
    toggles + scope?

16. **Access to a development device** for the benchmarks (Stax dev
    kit, Nano S+ dev cable, etc.) and the appropriate firmware build
    for measurement (countermeasures-disabled mode for fair
    comparisons, then re-enabled for deployment numbers)?

## Cortex-M4 assembly path

17. **Plans for `sign_sampler_cm4.s` to support FNDSA_PATH_B**?
    The current upstream PR explicitly does not touch the Cortex-M4
    assembly file (it remains on the baseline 28-quarter layout).
    Ledger's deployment is M35P, not M4 — so this may be moot for
    your purposes — but worth confirming.

18. **If the M4 path is deprecated**: is there an upcoming pure-C
    fallback for ARMv7M targets, or is the assembly path still
    load-bearing for some Ledger hardware variant?

## API design questions

19. **Preferred API shape** for the precomputed-basis variant:

    Option A: separate function family
    ```c
    void fndsa_setup_basis(unsigned logn, const uint8_t *sign_key, fpr *basis);
    size_t fndsa_sign_with_basis(...const fpr *basis...);
    ```

    Option B: opaque key handle that includes basis
    ```c
    typedef struct { /* ... */ } fndsa_key;
    void fndsa_key_init(fndsa_key *k, const uint8_t *sign_key);
    size_t fndsa_sign(fndsa_key *k, ...);
    ```

    Option B matches Pornin's Rust crate pattern more closely and is
    what we'd recommend, but it's more invasive.

20. **`N_` prefix interaction**: would the precomputed basis be declared
    as `N_basis` in the calling app, with the SDK link-script handling
    placement, or does it need to live inside the FN-DSA library's own
    NV region?

## KATs and rollback

21. **Does Ledger maintain its own KATs** for FN-DSA, or use Pornin's
    upstream test vectors? FNDSA_PATH_B preserves bit-exact KAT match
    at logn ≥ 3 in our testing (~thousands of signatures), but
    Donjon-specific masking might affect this — if Donjon has its own
    KATs, the bit-exactness check would need to run against those too.

22. **Anti-rollback for the precomputed basis**: if a user can re-load
    the same key (e.g., recovery seed import), can they replay an old
    basis? Probably not a concern since the basis is deterministic
    from the key, but worth confirming there's no way to inject a
    crafted basis instead of the legitimately-derived one.

## Side-channel surface area

23. **Does the current side-channel evaluation cover a 32 KiB
    persistent FFT-domain basis** in the SE's flash? The compact
    on-disk key is ~3 KiB; the basis is ~10× larger. SE-level tamper
    resistance protects both equally at rest, but the *power signature
    during loads* during gram_fft might benefit from countermeasures
    that current code (with basis in volatile RAM) doesn't apply.

24. **Cache-prefetch behavior** during gram_fft's sequential basis
    read: deterministic (fine for constant-time) or affected by data?
    The 2 KiB read cache mentioned in the ST33K1M5 datasheet should
    fit a fraction of the basis at a time; access pattern is sequential
    so prefetch is predictable, but worth confirming there's no
    secret-dependent branching.

25. **Does masking apply to flash reads of secret data?** Pornin's
    c-fn-dsa stance covers timing-channel resistance only; SPA/DPA on
    flash bus during basis loads is explicitly out-of-scope. With phase
    1 reduction, ~32 KiB of basis bits transit the flash bus on every
    sign. Confirm whether Donjon's masking layer applies to flash reads
    of secret data (e.g., XOR-masked fetches) or only to RAM-resident
    state. If only RAM-resident, the basis-load step is an SPA exposure
    not present in the baseline (where basis is computed from f, g, F
    in RAM each sign).

26. **Basis integrity protection — HMAC vs flash ECC?** Phase 1
    reduction stores the basis once at provisioning and reads it on
    every subsequent sign. A bit-flip in the flash basis region
    silently corrupts all subsequent signatures (the SE has no way to
    detect, because there's no on-the-fly recomputation to compare
    against). Two options:
    - HMAC-tag the basis at write; verify-on-read at sign time
      (~32 bytes overhead per basis)
    - Rely on flash ECC if the SE provides per-page integrity (M35P
      supports SECDED on some pages — confirm scope and behavior on
      uncorrectable error)
    Which does Donjon recommend for SE-resident derived secrets?

27. **Power analysis on Path A's extra fpr_div per sign.** Path A
    (proposed ffsamp 5n→4n reduction) adds n fpr_div operations on
    secret data per sign — recomputing l10 = g01/d00 from the flash
    basis post-right-recursion. fpr_div is constant-time software
    emulation (no timing leak), but each instance is a potential SPA
    leak point. Existing LDL already uses fpr_div on secrets at every
    recursion level, so Path A adds ~n more divs to a sign that already
    does several thousand. Marginal increase, but confirm masking
    covers fpr_div uniformly (no special-case handling that might miss
    the recompute call site).

28. **Net app SRAM budget on each device** after BOLOS (31 KiB) and
    SDK glue, on ST33K1M5-based platforms (Nano S+, Nano X, Nano Gen 5,
    Stax, Flex). The "32 KiB net" headline that public docs imply is
    the relevant ceiling, but the SDK glue / APDU comm buffer charge
    is undocumented and varies by firmware build. With phase 1 +
    Path A, FN-DSA-512 tmp[] + globals + stack + SDK glue should fit
    ~28-32 KiB; confirming the 32 KiB ceiling holds across devices is
    blocking for shipping FN-DSA-512 + Ethereum app on Nano-class.

---

## Correctness considerations beyond the stance

29. **Does Donjon's KAT suite exercise logn=2 (n=4)?** PATH_B has a
    known correctness regression at logn=2 (FP-order divergence
    occasionally crosses an integer-rounding boundary in the Gaussian
    sampler, producing different but verifiable signatures). FN-DSA
    does not standardize n=4 so this is a spec gap, but if Donjon's
    internal validation runs the toy parameters, the KAT mismatch
    would surface there. We skip logn=2 in our test_fndsa under
    PATH_B; confirm this is acceptable for Donjon's evaluation.

30. **fndsa_sign_temp buffer-reuse pattern.** Path A's deployment
    math closes only if the Ethereum app uses fndsa_sign_temp() and
    passes the same buffer it used for TX parsing (the buffer is
    reusable because parsing is complete before signing starts).
    Confirm:
    - SDK supports apps allocating a 22 KiB buffer in their working
      memory and passing it to crypto-library temp arguments
    - No MPU restriction or size limit prevents this
    - Ethereum app team can adopt this pattern (or already does)
    Without buffer reuse, parse buffer + tmp[] = ~30 KiB instead of
    max(parse, tmp[]) = ~22 KiB. The 8 KiB delta is the difference
    between "fits with margin" and "doesn't fit" on Nano-class.

---

## Decision matrix based on answers

| If | Then |
|---|---|
| Masking is in place AND adapts cleanly to flash basis | Proceed to Day 3 implementation |
| Masking needs a non-trivial rework | Defer phase 1 reduction until masking is settled, ship Path B alone |
| Per-app NV budget can't accommodate 32n bytes | Phase 1 dies; document and accept Path B's 4/8 KiB |
| Watchdog can't tolerate 1-2s flush | Need batched-flush API; design Day 3+ accordingly |
| Speculos doesn't simulate flash, but a hardware test rig is available | Proceed with the bench plan |
| Speculos doesn't simulate flash AND no hardware test rig | Phase 1 work proceeds on best-effort host estimates; final deployment validation deferred |
| Masking does NOT cover flash reads of secret data | HMAC-on-write + verify-on-read becomes mandatory for the basis |
| Net app SRAM < 30 KiB on any target device | Path A becomes load-bearing (not optional) for FN-DSA-512 on that device |
| Ethereum app cannot adopt fndsa_sign_temp buffer-reuse pattern | Even Path A may not close the budget on Nano-class; consider further tmp[] reduction or app-side leaning |

---

## Asks summary

- **Concrete numbers**: per-app NV budget × 5 devices, watchdog interval × 5 devices, `nvm_write()` per-page latency
- **Yes/no clarifications**: masking status, atomicity guarantees, Speculos coverage
- **Recommendations**: journaling pattern, watchdog kick pattern, API shape preference
- **Access**: hardware test rig, Donjon profiling harness if available

The most blocking items are the per-app NV budget (8) and the masking
status (1, 4). With those two answered, the rest can be designed around.
