#ifndef F2_SELECTOR_MASK_GADGETS_H
#define F2_SELECTOR_MASK_GADGETS_H

#include <stdint.h>

#ifndef F2_SELECTOR_ORDER
#error "define F2_SELECTOR_ORDER before including mask_gadgets.h"
#endif

#define F2_SELECTOR_NSHARES (F2_SELECTOR_ORDER + 1)

static inline uint32_t
f2_sel_rng_next(uint32_t *rng)
{
    uint32_t x = *rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *rng = x;
    return x;
}

static inline uint32_t
f2_sel_expand_bit(uint32_t bit)
{
    return 0u - (bit & 1u);
}

static inline uint32_t
f2_sel_match_bit0(uint32_t share, uint32_t lane)
{
    return ((share & 1u) ^ (((lane & 1u) ^ 1u) & 1u)) & 1u;
}

static inline uint32_t
f2_sel_match_bit1(uint32_t share, uint32_t lane)
{
    return ((((share >> 1) & 1u) ^ ((((lane >> 1) & 1u) ^ 1u) & 1u)) & 1u);
}

static inline uint32_t
f2_sel_bit0(uint32_t share)
{
    return share & 1u;
}

static inline uint32_t
f2_sel_bit1(uint32_t share)
{
    return (share >> 1) & 1u;
}

#define F2_SEL_PAIR(oi, oj, ai, aj, bi, bj, rng) do { \
    uint32_t f2_sel_r = f2_sel_rng_next((rng)) & 1u; \
    uint32_t f2_sel_cross = (((ai) & (bj)) ^ ((aj) & (bi))) & 1u; \
    (oi) ^= f2_sel_r; \
    (oj) ^= f2_sel_r ^ f2_sel_cross; \
} while (0)

static inline void
f2_sel_and_d1(uint32_t *o0, uint32_t *o1,
              uint32_t a0, uint32_t a1,
              uint32_t b0, uint32_t b1,
              uint32_t *rng)
{
    uint32_t r0 = (a0 & b0) & 1u;
    uint32_t r1 = (a1 & b1) & 1u;
    F2_SEL_PAIR(r0, r1, a0, a1, b0, b1, rng);
    *o0 = r0 & 1u;
    *o1 = r1 & 1u;
}

static inline void
f2_sel_and_d2(uint32_t *o0, uint32_t *o1, uint32_t *o2,
              uint32_t a0, uint32_t a1, uint32_t a2,
              uint32_t b0, uint32_t b1, uint32_t b2,
              uint32_t *rng)
{
    uint32_t r0 = (a0 & b0) & 1u;
    uint32_t r1 = (a1 & b1) & 1u;
    uint32_t r2 = (a2 & b2) & 1u;
    F2_SEL_PAIR(r0, r1, a0, a1, b0, b1, rng);
    F2_SEL_PAIR(r0, r2, a0, a2, b0, b2, rng);
    F2_SEL_PAIR(r1, r2, a1, a2, b1, b2, rng);
    *o0 = r0 & 1u;
    *o1 = r1 & 1u;
    *o2 = r2 & 1u;
}

static inline void
f2_sel_and_d3(uint32_t *o0, uint32_t *o1, uint32_t *o2, uint32_t *o3,
              uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3,
              uint32_t b0, uint32_t b1, uint32_t b2, uint32_t b3,
              uint32_t *rng)
{
    uint32_t r0 = (a0 & b0) & 1u;
    uint32_t r1 = (a1 & b1) & 1u;
    uint32_t r2 = (a2 & b2) & 1u;
    uint32_t r3 = (a3 & b3) & 1u;
    F2_SEL_PAIR(r0, r1, a0, a1, b0, b1, rng);
    F2_SEL_PAIR(r0, r2, a0, a2, b0, b2, rng);
    F2_SEL_PAIR(r0, r3, a0, a3, b0, b3, rng);
    F2_SEL_PAIR(r1, r2, a1, a2, b1, b2, rng);
    F2_SEL_PAIR(r1, r3, a1, a3, b1, b3, rng);
    F2_SEL_PAIR(r2, r3, a2, a3, b2, b3, rng);
    *o0 = r0 & 1u;
    *o1 = r1 & 1u;
    *o2 = r2 & 1u;
    *o3 = r3 & 1u;
}

static inline void
f2_sel_and_d4(uint32_t *o0, uint32_t *o1, uint32_t *o2,
              uint32_t *o3, uint32_t *o4,
              uint32_t a0, uint32_t a1, uint32_t a2,
              uint32_t a3, uint32_t a4,
              uint32_t b0, uint32_t b1, uint32_t b2,
              uint32_t b3, uint32_t b4,
              uint32_t *rng)
{
    uint32_t r0 = (a0 & b0) & 1u;
    uint32_t r1 = (a1 & b1) & 1u;
    uint32_t r2 = (a2 & b2) & 1u;
    uint32_t r3 = (a3 & b3) & 1u;
    uint32_t r4 = (a4 & b4) & 1u;
    F2_SEL_PAIR(r0, r1, a0, a1, b0, b1, rng);
    F2_SEL_PAIR(r0, r2, a0, a2, b0, b2, rng);
    F2_SEL_PAIR(r0, r3, a0, a3, b0, b3, rng);
    F2_SEL_PAIR(r0, r4, a0, a4, b0, b4, rng);
    F2_SEL_PAIR(r1, r2, a1, a2, b1, b2, rng);
    F2_SEL_PAIR(r1, r3, a1, a3, b1, b3, rng);
    F2_SEL_PAIR(r1, r4, a1, a4, b1, b4, rng);
    F2_SEL_PAIR(r2, r3, a2, a3, b2, b3, rng);
    F2_SEL_PAIR(r2, r4, a2, a4, b2, b4, rng);
    F2_SEL_PAIR(r3, r4, a3, a4, b3, b4, rng);
    *o0 = r0 & 1u;
    *o1 = r1 & 1u;
    *o2 = r2 & 1u;
    *o3 = r3 & 1u;
    *o4 = r4 & 1u;
}

#define F2_SEL_LOAD_MATCH_D1(lane) \
    uint32_t a0 = f2_sel_match_bit0(z0, (lane)); \
    uint32_t a1 = f2_sel_bit0(z1); \
    uint32_t b0 = f2_sel_match_bit1(z0, (lane)); \
    uint32_t b1 = f2_sel_bit1(z1)

#define F2_SEL_LOAD_MATCH_D2(lane) \
    uint32_t a0 = f2_sel_match_bit0(z0, (lane)); \
    uint32_t a1 = f2_sel_bit0(z1); \
    uint32_t a2 = f2_sel_bit0(z2); \
    uint32_t b0 = f2_sel_match_bit1(z0, (lane)); \
    uint32_t b1 = f2_sel_bit1(z1); \
    uint32_t b2 = f2_sel_bit1(z2)

#define F2_SEL_LOAD_MATCH_D3(lane) \
    uint32_t a0 = f2_sel_match_bit0(z0, (lane)); \
    uint32_t a1 = f2_sel_bit0(z1); \
    uint32_t a2 = f2_sel_bit0(z2); \
    uint32_t a3 = f2_sel_bit0(z3); \
    uint32_t b0 = f2_sel_match_bit1(z0, (lane)); \
    uint32_t b1 = f2_sel_bit1(z1); \
    uint32_t b2 = f2_sel_bit1(z2); \
    uint32_t b3 = f2_sel_bit1(z3)

#define F2_SEL_LOAD_MATCH_D4(lane) \
    uint32_t a0 = f2_sel_match_bit0(z0, (lane)); \
    uint32_t a1 = f2_sel_bit0(z1); \
    uint32_t a2 = f2_sel_bit0(z2); \
    uint32_t a3 = f2_sel_bit0(z3); \
    uint32_t a4 = f2_sel_bit0(z4); \
    uint32_t b0 = f2_sel_match_bit1(z0, (lane)); \
    uint32_t b1 = f2_sel_bit1(z1); \
    uint32_t b2 = f2_sel_bit1(z2); \
    uint32_t b3 = f2_sel_bit1(z3); \
    uint32_t b4 = f2_sel_bit1(z4)

static inline void
f2_sel_eq2_const_d1_branchless(
    const volatile uint32_t z_sh[F2_SELECTOR_NSHARES],
    uint32_t lane, uint32_t eq_sh[2], uint32_t *rng)
{
    uint32_t z0 = z_sh[0], z1 = z_sh[1];
    F2_SEL_LOAD_MATCH_D1(lane);
    f2_sel_and_d1(&eq_sh[0], &eq_sh[1], a0, a1, b0, b1, rng);
}

static inline void
f2_sel_eq2_const_d2_branchless(
    const volatile uint32_t z_sh[F2_SELECTOR_NSHARES],
    uint32_t lane, uint32_t eq_sh[3], uint32_t *rng)
{
    uint32_t z0 = z_sh[0], z1 = z_sh[1], z2 = z_sh[2];
    F2_SEL_LOAD_MATCH_D2(lane);
    f2_sel_and_d2(&eq_sh[0], &eq_sh[1], &eq_sh[2],
                  a0, a1, a2, b0, b1, b2, rng);
}

static inline void
f2_sel_eq2_const_d3_branchless(
    const volatile uint32_t z_sh[F2_SELECTOR_NSHARES],
    uint32_t lane, uint32_t eq_sh[4], uint32_t *rng)
{
    uint32_t z0 = z_sh[0], z1 = z_sh[1], z2 = z_sh[2], z3 = z_sh[3];
    F2_SEL_LOAD_MATCH_D3(lane);
    f2_sel_and_d3(&eq_sh[0], &eq_sh[1], &eq_sh[2], &eq_sh[3],
                  a0, a1, a2, a3, b0, b1, b2, b3, rng);
}

static inline void
f2_sel_eq2_const_d4_branchless(
    const volatile uint32_t z_sh[F2_SELECTOR_NSHARES],
    uint32_t lane, uint32_t eq_sh[5], uint32_t *rng)
{
    uint32_t z0 = z_sh[0], z1 = z_sh[1], z2 = z_sh[2], z3 = z_sh[3];
    uint32_t z4 = z_sh[4];
    F2_SEL_LOAD_MATCH_D4(lane);
    f2_sel_and_d4(&eq_sh[0], &eq_sh[1], &eq_sh[2], &eq_sh[3], &eq_sh[4],
                  a0, a1, a2, a3, a4, b0, b1, b2, b3, b4, rng);
}

static inline void
f2_sel_selected_shares_d1_branchless(
    const volatile uint32_t z_sh[F2_SELECTOR_NSHARES],
    const volatile uint32_t berexp_results[4],
    const volatile uint32_t z_candidates[4],
    uint32_t accept_sh[2], uint32_t value_sh[2], uint32_t rng_seed)
{
    uint32_t eq[2];
    uint32_t rng = rng_seed | 1u;
    accept_sh[0] = 0;
    accept_sh[1] = 0;
    value_sh[0] = 0;
    value_sh[1] = 0;
#define F2_SEL_ACCUM_D1(lane) do { \
    uint32_t accept = berexp_results[(lane)] & 1u; \
    uint32_t value = z_candidates[(lane)]; \
    f2_sel_eq2_const_d1_branchless(z_sh, (lane), eq, &rng); \
    accept_sh[0] ^= eq[0] & accept; \
    accept_sh[1] ^= eq[1] & accept; \
    value_sh[0] ^= f2_sel_expand_bit(eq[0]) & value; \
    value_sh[1] ^= f2_sel_expand_bit(eq[1]) & value; \
} while (0)
    F2_SEL_ACCUM_D1(0);
    F2_SEL_ACCUM_D1(1);
    F2_SEL_ACCUM_D1(2);
    F2_SEL_ACCUM_D1(3);
#undef F2_SEL_ACCUM_D1
}

static inline void
f2_sel_selected_shares_d2_branchless(
    const volatile uint32_t z_sh[F2_SELECTOR_NSHARES],
    const volatile uint32_t berexp_results[4],
    const volatile uint32_t z_candidates[4],
    uint32_t accept_sh[3], uint32_t value_sh[3], uint32_t rng_seed)
{
    uint32_t eq[3];
    uint32_t rng = rng_seed | 1u;
    accept_sh[0] = 0;
    accept_sh[1] = 0;
    accept_sh[2] = 0;
    value_sh[0] = 0;
    value_sh[1] = 0;
    value_sh[2] = 0;
#define F2_SEL_ACCUM_D2(lane) do { \
    uint32_t accept = berexp_results[(lane)] & 1u; \
    uint32_t value = z_candidates[(lane)]; \
    f2_sel_eq2_const_d2_branchless(z_sh, (lane), eq, &rng); \
    accept_sh[0] ^= eq[0] & accept; \
    accept_sh[1] ^= eq[1] & accept; \
    accept_sh[2] ^= eq[2] & accept; \
    value_sh[0] ^= f2_sel_expand_bit(eq[0]) & value; \
    value_sh[1] ^= f2_sel_expand_bit(eq[1]) & value; \
    value_sh[2] ^= f2_sel_expand_bit(eq[2]) & value; \
} while (0)
    F2_SEL_ACCUM_D2(0);
    F2_SEL_ACCUM_D2(1);
    F2_SEL_ACCUM_D2(2);
    F2_SEL_ACCUM_D2(3);
#undef F2_SEL_ACCUM_D2
}

static inline void
f2_sel_selected_shares_d3_branchless(
    const volatile uint32_t z_sh[F2_SELECTOR_NSHARES],
    const volatile uint32_t berexp_results[4],
    const volatile uint32_t z_candidates[4],
    uint32_t accept_sh[4], uint32_t value_sh[4], uint32_t rng_seed)
{
    uint32_t eq[4];
    uint32_t rng = rng_seed | 1u;
    accept_sh[0] = 0;
    accept_sh[1] = 0;
    accept_sh[2] = 0;
    accept_sh[3] = 0;
    value_sh[0] = 0;
    value_sh[1] = 0;
    value_sh[2] = 0;
    value_sh[3] = 0;
#define F2_SEL_ACCUM_D3(lane) do { \
    uint32_t accept = berexp_results[(lane)] & 1u; \
    uint32_t value = z_candidates[(lane)]; \
    f2_sel_eq2_const_d3_branchless(z_sh, (lane), eq, &rng); \
    accept_sh[0] ^= eq[0] & accept; \
    accept_sh[1] ^= eq[1] & accept; \
    accept_sh[2] ^= eq[2] & accept; \
    accept_sh[3] ^= eq[3] & accept; \
    value_sh[0] ^= f2_sel_expand_bit(eq[0]) & value; \
    value_sh[1] ^= f2_sel_expand_bit(eq[1]) & value; \
    value_sh[2] ^= f2_sel_expand_bit(eq[2]) & value; \
    value_sh[3] ^= f2_sel_expand_bit(eq[3]) & value; \
} while (0)
    F2_SEL_ACCUM_D3(0);
    F2_SEL_ACCUM_D3(1);
    F2_SEL_ACCUM_D3(2);
    F2_SEL_ACCUM_D3(3);
#undef F2_SEL_ACCUM_D3
}

static inline void
f2_sel_selected_shares_d4_branchless(
    const volatile uint32_t z_sh[F2_SELECTOR_NSHARES],
    const volatile uint32_t berexp_results[4],
    const volatile uint32_t z_candidates[4],
    uint32_t accept_sh[5], uint32_t value_sh[5], uint32_t rng_seed)
{
    uint32_t eq[5];
    uint32_t rng = rng_seed | 1u;
    accept_sh[0] = 0;
    accept_sh[1] = 0;
    accept_sh[2] = 0;
    accept_sh[3] = 0;
    accept_sh[4] = 0;
    value_sh[0] = 0;
    value_sh[1] = 0;
    value_sh[2] = 0;
    value_sh[3] = 0;
    value_sh[4] = 0;
#define F2_SEL_ACCUM_D4(lane) do { \
    uint32_t accept = berexp_results[(lane)] & 1u; \
    uint32_t value = z_candidates[(lane)]; \
    f2_sel_eq2_const_d4_branchless(z_sh, (lane), eq, &rng); \
    accept_sh[0] ^= eq[0] & accept; \
    accept_sh[1] ^= eq[1] & accept; \
    accept_sh[2] ^= eq[2] & accept; \
    accept_sh[3] ^= eq[3] & accept; \
    accept_sh[4] ^= eq[4] & accept; \
    value_sh[0] ^= f2_sel_expand_bit(eq[0]) & value; \
    value_sh[1] ^= f2_sel_expand_bit(eq[1]) & value; \
    value_sh[2] ^= f2_sel_expand_bit(eq[2]) & value; \
    value_sh[3] ^= f2_sel_expand_bit(eq[3]) & value; \
    value_sh[4] ^= f2_sel_expand_bit(eq[4]) & value; \
} while (0)
    F2_SEL_ACCUM_D4(0);
    F2_SEL_ACCUM_D4(1);
    F2_SEL_ACCUM_D4(2);
    F2_SEL_ACCUM_D4(3);
#undef F2_SEL_ACCUM_D4
}

static inline uint32_t
f2_sel_dispatch_masked_d1_branchless(
    const volatile uint32_t z_sh[F2_SELECTOR_NSHARES],
    const volatile uint32_t berexp_results[4],
    const volatile uint32_t z_candidates[4],
    uint32_t rng_seed)
{
    uint32_t accept_sh[2], value_sh[2];
    f2_sel_selected_shares_d1_branchless(
        z_sh, berexp_results, z_candidates, accept_sh, value_sh, rng_seed);
    return (value_sh[0] ^ value_sh[1])
        & f2_sel_expand_bit(accept_sh[0] ^ accept_sh[1]);
}

static inline uint32_t
f2_sel_dispatch_masked_d2_branchless(
    const volatile uint32_t z_sh[F2_SELECTOR_NSHARES],
    const volatile uint32_t berexp_results[4],
    const volatile uint32_t z_candidates[4],
    uint32_t rng_seed)
{
    uint32_t accept_sh[3], value_sh[3];
    f2_sel_selected_shares_d2_branchless(
        z_sh, berexp_results, z_candidates, accept_sh, value_sh, rng_seed);
    return (value_sh[0] ^ value_sh[1] ^ value_sh[2])
        & f2_sel_expand_bit(accept_sh[0] ^ accept_sh[1] ^ accept_sh[2]);
}

static inline uint32_t
f2_sel_dispatch_masked_d3_branchless(
    const volatile uint32_t z_sh[F2_SELECTOR_NSHARES],
    const volatile uint32_t berexp_results[4],
    const volatile uint32_t z_candidates[4],
    uint32_t rng_seed)
{
    uint32_t accept_sh[4], value_sh[4];
    f2_sel_selected_shares_d3_branchless(
        z_sh, berexp_results, z_candidates, accept_sh, value_sh, rng_seed);
    return (value_sh[0] ^ value_sh[1] ^ value_sh[2] ^ value_sh[3])
        & f2_sel_expand_bit(accept_sh[0] ^ accept_sh[1]
                            ^ accept_sh[2] ^ accept_sh[3]);
}

static inline uint32_t
f2_sel_dispatch_masked_d4_branchless(
    const volatile uint32_t z_sh[F2_SELECTOR_NSHARES],
    const volatile uint32_t berexp_results[4],
    const volatile uint32_t z_candidates[4],
    uint32_t rng_seed)
{
    uint32_t accept_sh[5], value_sh[5];
    f2_sel_selected_shares_d4_branchless(
        z_sh, berexp_results, z_candidates, accept_sh, value_sh, rng_seed);
    return (value_sh[0] ^ value_sh[1] ^ value_sh[2] ^ value_sh[3]
            ^ value_sh[4])
        & f2_sel_expand_bit(accept_sh[0] ^ accept_sh[1] ^ accept_sh[2]
                            ^ accept_sh[3] ^ accept_sh[4]);
}

#endif
