---
name: Phase 1 API + Atomic-Update Protocol Design (Day 2)
description: API shape for the precomputed-basis variant of FN-DSA signing, plus the atomic-flag-page protocol for tear-resistant basis storage on ST33K1M5 NVM.
---

# Phase 1 API + atomic-update protocol design (Day 2)

Companion to [phase1_options.md](phase1_options.md). Concrete design for
option 5: precomputed basis in flash, accessed by `sign_core` via a new
caller-allocated buffer parameter.

## API shape — Option B (key handle)

Of the two API variants in [ledger_donjon_questions.md](ledger_donjon_questions.md)
Q19, **Option B (opaque key handle)** is the recommended shape. It
matches Pornin's Rust crate pattern, keeps the surface tidy, and makes
the basis lifecycle explicit to the caller.

### Public API (additions to fndsa.h)

```c
/* Precomputed signing key + basis. Total in-flash size: ~3 KiB key +
   32 KiB basis at logn=10 (16 KiB at logn=9). The buffer must be
   provided by the caller and live in the SE's secure NVM region (on
   Ledger: declare as N_-prefixed global). */
typedef struct {
    uint8_t  enc_key[FNDSA_SIGN_KEY_SIZE_MAX];
    size_t   key_len;
    unsigned logn;
    /* Basis B = [[g, -f], [G, -F]] in FFT representation,
       4n FLR = 32n bytes. Layout matches sign_core.c's
       basis_to_FFT output convention. */
    fpr      basis[4 << FNDSA_LOGN_MAX];
    /* Atomic-update flag: see "Atomic-flag-page protocol" below.
       Must be the LAST 64-byte page of the entire structure. */
    struct {
        uint32_t magic;     /* 0xFD5A_BA51 = "FNDSA-BASIS" sentinel */
        uint32_t version;   /* monotonically incremented on each
                               legitimate basis write */
        uint8_t  reserved[56];
    } atomic_flag;
} fndsa_key_with_basis;

/* Compute basis from compact signing key and store it in `key_buf`,
   atomically. Atomic in the sense that a power loss mid-write leaves
   the structure in a state that's detectable on next use (atomic_flag
   either matches expected version or doesn't). On Ledger, the caller
   typically declares `key_buf` as `N_my_app_key` and this function
   issues nvm_write() syscalls internally.

   Returns:
     1 on success
     0 on error (invalid key, NV write failure)

   Per-sign: this function does NOT need to be called. Once a key is
   set up via this function, sign with fndsa_sign_*_with_basis below. */
int fndsa_setup_basis(
    fndsa_key_with_basis *key_buf,
    const void *sign_key, size_t sign_key_len);

/* Verify that `key_buf` contains a valid (non-torn) basis. Returns 1
   if the atomic_flag's magic matches and version is non-zero; 0
   otherwise. Caller should call fndsa_setup_basis() if this returns 0
   (e.g. after first install or after a power loss during a previous
   setup_basis call). */
int fndsa_basis_is_valid(const fndsa_key_with_basis *key_buf);

/* Sign with a precomputed basis. tmp[] must be at least 45n+31 bytes
   (down from 51n+31 with FNDSA_PATH_B alone, and 59n+31 baseline). */
size_t fndsa_sign_with_basis_temp(
    const fndsa_key_with_basis *key,
    const void *ctx, size_t ctx_len,
    const char *id, const void *hv, size_t hv_len,
    void *sig, size_t max_sig_len,
    void *tmp, size_t tmp_len);

size_t fndsa_sign_seeded_with_basis_temp(
    const fndsa_key_with_basis *key,
    const void *ctx, size_t ctx_len,
    const char *id, const void *hv, size_t hv_len,
    const void *seed, size_t seed_len,
    void *sig, size_t max_sig_len,
    void *tmp, size_t tmp_len);
```

### Backwards compatibility

The existing `fndsa_sign_*` functions are unchanged. The new
`*_with_basis_*` variants are purely additive. Apps not using the new
API see no behavior change.

The new entry points are gated by `FNDSA_PATH_B && FNDSA_PHASE1_REDUCED`
(or whatever flag scheme Pornin prefers — see [ledger_donjon_questions.md](ledger_donjon_questions.md) Q1+ for the open question).

## Atomic-flag-page protocol

### Storage layout

The `fndsa_key_with_basis` struct is laid out so the atomic_flag
occupies its own 64-byte page (the BOLOS NV page granularity). The
key is at the start; basis fills the middle; flag is the LAST 64
bytes.

```
Offset 0:                    enc_key (variable, padded to 64-byte boundary)
Offset key_padded:           basis (32n bytes at logn=10, 512 pages)
Offset (key_padded + 32n):   atomic_flag (64 bytes, exactly 1 page)
Total size:                  key_padded + 32n + 64 bytes
```

At logn=10: ~32-34 KiB per key (depending on key encoding length).

### Provisioning sequence (fndsa_setup_basis)

```
Step 1: Compute basis B from sign_key in a temporary RAM buffer
        (uses the existing basis_to_FFT, ~5 µs on host, scaled on M35P)

Step 2: nvm_write(key_buf->enc_key, sign_key, sign_key_len)
        // ~1 page, ~1-4 ms

Step 3: nvm_write(key_buf->basis, &B, 4n * sizeof(fpr))
        // ~512 pages, ~0.5-2 s blocking
        // Kick watchdog every K pages (K determined by Q5/Q7 answer)

Step 4: Construct flag = { magic=0xFD5A_BA51, version=N+1, reserved=0 }
        in a stack-local 64-byte buffer

Step 5: nvm_write(&key_buf->atomic_flag, &flag, 64)
        // ATOMIC at the SE hardware level: this single page write
        // either succeeds entirely or doesn't write at all
        // (per Q11 confirmation from Ledger)

Step 6: return 1 (success)

Failure mode: power loss between step 1 and step 5 leaves
        atomic_flag in pre-update state (magic=0 or stale version).
        On next call to fndsa_basis_is_valid, returns 0, caller
        re-runs fndsa_setup_basis.
```

### Per-sign read path (fndsa_sign_*_with_basis_*)

```
Step 1: Verify atomic_flag.magic == 0xFD5A_BA51
        // single 4-byte read, in cache, ~ns

Step 2: Verify atomic_flag.version != 0
        // single 4-byte read

Step 3: If either check fails, return 0 (caller must re-provision)

Step 4: Proceed with signing using key_buf->basis as the basis source
        (see sign_core.c modifications below)
```

### Why single flag is sufficient

The investigation noted that BOLOS provides single-page atomic writes
but no multi-page transaction. The single-flag-page protocol exploits
the page-level atomicity:

- 512 basis pages may be in any partial-write state after a power loss
- The single flag page is either fully present (last write committed)
  or fully absent (write never committed)
- The flag is the LAST page written, so a present flag implies all
  basis pages were also written
- A torn write of basis (some pages new, some old) cannot leave a
  "valid"-looking flag, because the flag write happens AFTER all
  basis writes

This is the same pattern used in journaling filesystems (write data,
then commit pointer) but at hardware page granularity.

### Recovery on power loss

If `fndsa_basis_is_valid()` returns 0:

```c
if (!fndsa_basis_is_valid(&N_my_key)) {
    /* Either first install, or last setup_basis was interrupted.
       Re-derive from the compact key (which was written and committed
       BEFORE the basis pages — its presence is a precondition). */
    fndsa_setup_basis(&N_my_key, sign_key_bytes, sign_key_len);
}
```

The compact key (~3 KiB) is provisioned through the normal Ledger
key-load flow, which has its own atomicity guarantees outside the
scope of this design. The basis re-derivation just runs setup_basis
again from the already-present key.

## Modifications to sign_core.c

The new `_with_basis` path takes a basis pointer instead of running
basis_to_FFT per sign:

```c
size_t
sign_core_with_basis(unsigned logn,
    const fpr *external_basis,    /* NEW: 4n FLR, FFT-domain */
    const uint8_t *hashed_vk, ...)
{
    /* ...rounds setup as before... */

    /* SKIP basis_to_FFT — basis comes from external_basis */
    /* Phase 1 working area is now smaller: just t0, t1 + gram outputs */

    fpr *t0 = (fpr *)tmp;
    fpr *t1 = t0 + n;

    /* Layout under FNDSA_PATH_B + FNDSA_PHASE1_REDUCED:
        qc(0..3)   t0 (target, written by apply_basis)
        qc(4..7)   t1 (target)
        qc(8..9)   g00 (self-adjoint, half-size)
        qc(10..13) g01 (full size n FLR)
        qc(14..15) g11 (self-adjoint, half-size)
        qc(16..21) ffsamp scratch (recursive Path B uses 21 outer-q
                                    starting at qc(0); fits exactly)
       Total used: 22 outer-q = 5.5n FLR. */

    /* Apply basis (uses external_basis's b01, b11; modified to read
       from external pointer rather than tmp). hm at offset 42n bytes
       (was 48n bytes under FNDSA_PATH_B alone). */
    fpoly_apply_basis_external(logn, t0, t1, external_basis, hm);

    /* Compute gram from external basis, write to compact slots
       directly (new fpoly_gram_fft_dst variant). */
    fpr *g00 = t1 + n;            /* qc(8..9) */
    fpr *g01 = g00 + (n >> 1);    /* qc(10..13) */
    fpr *g11 = g01 + n;           /* qc(14..15) */
    fpoly_gram_fft_dst(logn, g00, g01, g11,
        external_basis,           /* b00 */
        external_basis + n,       /* b01 */
        external_basis + 2*n,     /* b10 */
        external_basis + 3*n);    /* b11 */

    /* ffsamp_fft uses the same Path B body. */
    ffsamp_fft(&ss, tmp);

    /* ... rest of post-ffsamp signature compose as in current code ... */
}
```

### Two new fpoly_* primitives

1. **`fpoly_apply_basis_external`** — reads b01, b11 from external buffer
   instead of tmp[]. Signature change from `apply_basis(t0, t1, b01, b11, hm)`
   to `apply_basis_external(t0, t1, basis, hm)` with internal pointer
   arithmetic. Probably implementable as a thin wrapper that calls the
   existing apply_basis with computed pointers.

2. **`fpoly_gram_fft_dst`** — writes gram outputs to specified destination
   pointers instead of in-place over the basis. Reads basis from external
   (read-only) buffer. Same arithmetic as `fpoly_gram_fft` but with 7
   pointer parameters instead of 4. ~150 lines per architecture variant
   (4 variants).

## Per-app `N_basis` declaration pattern

On Ledger, the caller declares the persistent state via `N_`-prefixed
global:

```c
/* In the Ledger app's main C file: */
#include "fndsa.h"

const fndsa_key_with_basis N_my_signing_key;

/* On app init, after key is loaded: */
if (!fndsa_basis_is_valid(&N_my_signing_key)) {
    fndsa_setup_basis(
        (fndsa_key_with_basis *)PIC(&N_my_signing_key),  /* PIC = position-independent */
        sign_key_bytes, sign_key_len);
}

/* On signing request: */
size_t sig_len = fndsa_sign_with_basis_temp(
    &N_my_signing_key, ctx, ctx_len, id, hv, hv_len,
    sig, sizeof sig, tmp, sizeof tmp);
```

The `nvm_write()` calls inside `fndsa_setup_basis` automatically write
to the slot the SDK link script allocated for `N_my_signing_key`.

## Open questions before commit

1. **Flag scheme**: single FNDSA_PATH_B that bundles phase 1, or separate
   FNDSA_PHASE1_REDUCED on top of FNDSA_PATH_B? Q1 to Pornin.

2. **Function naming**: `fndsa_sign_with_basis_temp` vs
   `fndsa_sign_temp_with_basis` vs match-existing-pattern. Cosmetic.

3. **`PIC()` macro on Ledger**: required for accessing N_-prefixed
   globals on some Ledger SDK versions, not on others. Library should
   not embed `PIC()` directly; caller wraps as needed.

4. **`ffsamp_fft` reuse**: should work unchanged with the new
   `_with_basis` path since ffsamp doesn't touch the basis directly.
   Verify in Day 3 implementation.

## Cross-architecture considerations

The new fpoly primitives need 4 variants (SSE2/NEON/RV64D/scalar) per
Pornin's existing pattern. Same approach as `fpoly_pathb_finalize` from
the Path B work — ~200 lines of code in sign_fpoly.c.

## Validation plan (Day 5-6)

Same pattern as Path B:
- `test_fndsa` with new flag: all categories at logn ∈ [3, 10]
- ASAN
- New `test_phase1.c` paint-and-check: paint above the new ffsamp peak
  (qc(22..23) under recursive Path B + phase 1) and verify untouched
- Cross-check: bit-exact KAT match with current FNDSA_PATH_B (the
  algebra is preserved; only the storage location of basis changes)

## Estimated implementation effort

- Day 3: API + sign.c plumbing + new fpoly_apply_basis_external (1-2 days)
- Day 4: fpoly_gram_fft_dst (4 architecture variants) + sign_core_with_basis (1-2 days)
- Day 5: tmp_len reductions + fndsa.h docstring + atomic-flag protocol (1 day)
- Day 6: tests + ASAN (1 day)
- Day 7: hardware bench (gated on Ledger device access)

Total: ~5-7 days of code, plus 1-2 days hardware bench, plus polish/PR.