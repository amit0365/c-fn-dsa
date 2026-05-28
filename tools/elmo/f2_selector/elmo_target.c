#include <stdint.h>

#include "mask_gadgets.h"

#ifndef F2_SELECTOR_VARIANT
#define F2_SELECTOR_VARIANT 0
#endif

#define MODE_LOAD_ONLY 0
#define MODE_EQ_ONLY 1
#define MODE_SELECT_ONLY 2
#define MODE_OPEN_ONLY 3
#define MODE_FULL_DISPATCH 4
#define MODE_BIT_LINEAR_ONLY 5
#define MODE_ISW_AND_ONLY 6
#define MODE_EQ_NO_SINK 7
#define MODE_EQ_ONE_SHARE_SINK 8
#define MODE_ISW_AND_ONE_SHARE_SINK 9
#define MODE_ISW_AND_D2_ASM 10
#define MODE_EQ_D2_ASM 11

#ifndef F2_SELECTOR_TRACE_MODE
#define F2_SELECTOR_TRACE_MODE MODE_FULL_DISPATCH
#endif

#ifndef F2_SELECTOR_SINK_SHARE
#define F2_SELECTOR_SINK_SHARE 0
#endif

#if F2_SELECTOR_VARIANT == 0
uint32_t dispatch_clear(uint32_t z0_idx,
                        const volatile uint32_t berexp_results[4],
                        const volatile uint32_t z_candidates[4]);
#elif F2_SELECTOR_VARIANT == 1
uint32_t dispatch_masked_d1(const volatile uint32_t z_sh[F2_SELECTOR_NSHARES],
                            const volatile uint32_t berexp_results[4],
                            const volatile uint32_t z_candidates[4],
                            uint32_t rng_seed);
#elif F2_SELECTOR_VARIANT == 2
uint32_t dispatch_masked_d2(const volatile uint32_t z_sh[F2_SELECTOR_NSHARES],
                            const volatile uint32_t berexp_results[4],
                            const volatile uint32_t z_candidates[4],
                            uint32_t rng_seed);
#elif F2_SELECTOR_VARIANT == 3
uint32_t dispatch_masked_d3(const volatile uint32_t z_sh[F2_SELECTOR_NSHARES],
                            const volatile uint32_t berexp_results[4],
                            const volatile uint32_t z_candidates[4],
                            uint32_t rng_seed);
#elif F2_SELECTOR_VARIANT == 4
uint32_t dispatch_masked_d4(const volatile uint32_t z_sh[F2_SELECTOR_NSHARES],
                            const volatile uint32_t berexp_results[4],
                            const volatile uint32_t z_candidates[4],
                            uint32_t rng_seed);
#else
#error "unsupported F2_SELECTOR_VARIANT"
#endif

#ifndef F2_SELECTOR_NSHARES
#define F2_SELECTOR_NSHARES 1
#endif

#if F2_SELECTOR_SINK_SHARE < 0
#error "F2_SELECTOR_SINK_SHARE must be non-negative"
#endif

#if F2_SELECTOR_SINK_SHARE >= F2_SELECTOR_NSHARES
#error "F2_SELECTOR_SINK_SHARE is outside the available share range"
#endif

static volatile uint32_t sink;
static volatile uint32_t share_sink[F2_SELECTOR_NSHARES];
static uint32_t bench_rng_state = 0xC001D00Du;

static const uint32_t label_perm[24][4] = {
    {0, 1, 2, 3}, {0, 1, 3, 2}, {0, 2, 1, 3}, {0, 2, 3, 1},
    {0, 3, 1, 2}, {0, 3, 2, 1}, {1, 0, 2, 3}, {1, 0, 3, 2},
    {1, 2, 0, 3}, {1, 2, 3, 0}, {1, 3, 0, 2}, {1, 3, 2, 0},
    {2, 0, 1, 3}, {2, 0, 3, 1}, {2, 1, 0, 3}, {2, 1, 3, 0},
    {2, 3, 0, 1}, {2, 3, 1, 0}, {3, 0, 1, 2}, {3, 0, 2, 1},
    {3, 1, 0, 2}, {3, 1, 2, 0}, {3, 2, 0, 1}, {3, 2, 1, 0}
};

static void
elmo_starttrigger(void)
{
    volatile uint32_t *trigger = (volatile uint32_t *)0xE0000004u;
    *trigger = 1u;
}

static void
elmo_endtrigger(void)
{
    volatile uint32_t *trigger = (volatile uint32_t *)0xE0000004u;
    *trigger = 0u;
}

static void
elmo_load_n(uint32_t *ntraces)
{
    volatile uint32_t *load_n = (volatile uint32_t *)0xE1000010u;
    *ntraces = *load_n;
}

static void
elmo_printbyte(const unsigned char *out)
{
    volatile uint32_t *print_byte = (volatile uint32_t *)0xE0000000u;
    *print_byte = *out;
}

static void
elmo_endprogram(void)
{
    volatile uint32_t *halt = (volatile uint32_t *)0xF0000000u;
    *halt = 0u;
}

static uint32_t
rand_u32(void)
{
    uint32_t x = bench_rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    bench_rng_state = x;
    return x;
}

static void
make_shares(volatile uint32_t z_sh[F2_SELECTOR_NSHARES],
            uint32_t label)
{
#if F2_SELECTOR_VARIANT == 0
    z_sh[0] = label & 3u;
#else
    uint32_t acc = label & 3u;
    for (uint32_t i = 1; i < F2_SELECTOR_NSHARES; i ++) {
        uint32_t share = rand_u32() & 3u;
        z_sh[i] = share;
        acc ^= share;
    }
    z_sh[0] = acc & 3u;
#endif
}

#if F2_SELECTOR_VARIANT != 0
#if F2_SELECTOR_VARIANT == 2
uint32_t trace_isw_and_d2_asm_window(const uint32_t a_sh[4][F2_SELECTOR_NSHARES],
                                     const uint32_t b_sh[4][F2_SELECTOR_NSHARES],
                                     uint32_t rng_seed);
uint32_t trace_eq2_const_d2_asm_window(
    const volatile uint32_t z_sh[F2_SELECTOR_NSHARES],
    uint32_t rng_seed);
#endif

static void
reset_share_sinks(void)
{
    share_sink[0] = 0;
#if F2_SELECTOR_NSHARES > 1
    share_sink[1] = 0;
#endif
#if F2_SELECTOR_NSHARES > 2
    share_sink[2] = 0;
#endif
#if F2_SELECTOR_NSHARES > 3
    share_sink[3] = 0;
#endif
#if F2_SELECTOR_NSHARES > 4
    share_sink[4] = 0;
#endif
}

static void
consume_share_registers(const uint32_t shares[F2_SELECTOR_NSHARES])
{
#if F2_SELECTOR_VARIANT == 1
    __asm__ volatile("" : : "r"(shares[0]), "r"(shares[1]) : "memory");
#elif F2_SELECTOR_VARIANT == 2
    __asm__ volatile("" : : "r"(shares[0]), "r"(shares[1]),
                     "r"(shares[2]) : "memory");
#elif F2_SELECTOR_VARIANT == 3
    __asm__ volatile("" : : "r"(shares[0]), "r"(shares[1]),
                     "r"(shares[2]), "r"(shares[3]) : "memory");
#else
    __asm__ volatile("" : : "r"(shares[0]), "r"(shares[1]),
                     "r"(shares[2]), "r"(shares[3]), "r"(shares[4])
                     : "memory");
#endif
}

static void
sink_one_share(const uint32_t shares[F2_SELECTOR_NSHARES])
{
#if F2_SELECTOR_SINK_SHARE == 0
    share_sink[0] ^= shares[0];
#elif F2_SELECTOR_SINK_SHARE == 1
    share_sink[1] ^= shares[1];
#elif F2_SELECTOR_SINK_SHARE == 2
    share_sink[2] ^= shares[2];
#elif F2_SELECTOR_SINK_SHARE == 3
    share_sink[3] ^= shares[3];
#else
    share_sink[4] ^= shares[4];
#endif
}

static uint32_t
eq2_clear_const(uint32_t value, uint32_t lane)
{
    uint32_t diff = (value ^ lane) & 3u;
    return (((diff & 1u) ^ 1u) & ((((diff >> 1) & 1u) ^ 1u))) & 1u;
}

static void
make_synthetic_onehot(uint32_t eq_sh[4][F2_SELECTOR_NSHARES],
                      uint32_t rng_seed)
{
    uint32_t synthetic_lane = rng_seed & 3u;
    uint32_t rng = rng_seed ^ 0xA5A5A5A5u;

    for (uint32_t lane = 0; lane < 4; lane ++) {
        uint32_t acc = eq2_clear_const(synthetic_lane, lane);
        for (uint32_t share = 1; share < F2_SELECTOR_NSHARES; share ++) {
            uint32_t bit = rand_u32() ^ f2_sel_rng_next(&rng);
            eq_sh[lane][share] = bit & 1u;
            acc ^= bit;
        }
        eq_sh[lane][0] = acc & 1u;
    }
}

static void
precompute_selected_shares(
    const volatile uint32_t z_sh[F2_SELECTOR_NSHARES],
    const volatile uint32_t berexp_results[4],
    const volatile uint32_t z_candidates[4],
    uint32_t accept_sh[F2_SELECTOR_NSHARES],
    uint32_t value_sh[F2_SELECTOR_NSHARES],
    uint32_t rng_seed)
{
#if F2_SELECTOR_VARIANT == 1
    f2_sel_selected_shares_d1_branchless(
        z_sh, berexp_results, z_candidates, accept_sh, value_sh, rng_seed);
#elif F2_SELECTOR_VARIANT == 2
    f2_sel_selected_shares_d2_branchless(
        z_sh, berexp_results, z_candidates, accept_sh, value_sh, rng_seed);
#elif F2_SELECTOR_VARIANT == 3
    f2_sel_selected_shares_d3_branchless(
        z_sh, berexp_results, z_candidates, accept_sh, value_sh, rng_seed);
#else
    f2_sel_selected_shares_d4_branchless(
        z_sh, berexp_results, z_candidates, accept_sh, value_sh, rng_seed);
#endif
}

static void
precompute_match_shares(const volatile uint32_t z_sh[F2_SELECTOR_NSHARES],
                        uint32_t a_sh[4][F2_SELECTOR_NSHARES],
                        uint32_t b_sh[4][F2_SELECTOR_NSHARES])
{
    for (uint32_t lane = 0; lane < 4; lane ++) {
        a_sh[lane][0] = f2_sel_match_bit0(z_sh[0], lane);
        b_sh[lane][0] = f2_sel_match_bit1(z_sh[0], lane);
        for (uint32_t share = 1; share < F2_SELECTOR_NSHARES; share ++) {
            a_sh[lane][share] = f2_sel_bit0(z_sh[share]);
            b_sh[lane][share] = f2_sel_bit1(z_sh[share]);
        }
    }
}

__attribute__((noinline))
static uint32_t
trace_load_only(const volatile uint32_t z_sh[F2_SELECTOR_NSHARES])
{
    uint32_t z0;
#if F2_SELECTOR_NSHARES > 1
    uint32_t z1;
#endif
#if F2_SELECTOR_NSHARES > 2
    uint32_t z2;
#endif
#if F2_SELECTOR_NSHARES > 3
    uint32_t z3;
#endif
#if F2_SELECTOR_NSHARES > 4
    uint32_t z4;
#endif

    reset_share_sinks();
    elmo_starttrigger();
    z0 = z_sh[0];
    share_sink[0] ^= (z0 & 1u) ^ ((z0 >> 1) & 1u);
#if F2_SELECTOR_NSHARES > 1
    z1 = z_sh[1];
    share_sink[1] ^= (z1 & 1u) ^ ((z1 >> 1) & 1u);
#endif
#if F2_SELECTOR_NSHARES > 2
    z2 = z_sh[2];
    share_sink[2] ^= (z2 & 1u) ^ ((z2 >> 1) & 1u);
#endif
#if F2_SELECTOR_NSHARES > 3
    z3 = z_sh[3];
    share_sink[3] ^= (z3 & 1u) ^ ((z3 >> 1) & 1u);
#endif
#if F2_SELECTOR_NSHARES > 4
    z4 = z_sh[4];
    share_sink[4] ^= (z4 & 1u) ^ ((z4 >> 1) & 1u);
#endif
    elmo_endtrigger();
    return 0;
}

__attribute__((noinline))
static uint32_t
trace_bit_linear_only(const volatile uint32_t z_sh[F2_SELECTOR_NSHARES])
{
    reset_share_sinks();
    elmo_starttrigger();
#if F2_SELECTOR_VARIANT == 1
#define F2_SEL_LINEAR_SINK(lane) do { \
    uint32_t z0 = z_sh[0], z1 = z_sh[1]; \
    F2_SEL_LOAD_MATCH_D1(lane); \
    share_sink[0] ^= a0 ^ b0; \
    share_sink[1] ^= a1 ^ b1; \
} while (0)
#elif F2_SELECTOR_VARIANT == 2
#define F2_SEL_LINEAR_SINK(lane) do { \
    uint32_t z0 = z_sh[0], z1 = z_sh[1], z2 = z_sh[2]; \
    F2_SEL_LOAD_MATCH_D2(lane); \
    share_sink[0] ^= a0 ^ b0; \
    share_sink[1] ^= a1 ^ b1; \
    share_sink[2] ^= a2 ^ b2; \
} while (0)
#elif F2_SELECTOR_VARIANT == 3
#define F2_SEL_LINEAR_SINK(lane) do { \
    uint32_t z0 = z_sh[0], z1 = z_sh[1], z2 = z_sh[2], z3 = z_sh[3]; \
    F2_SEL_LOAD_MATCH_D3(lane); \
    share_sink[0] ^= a0 ^ b0; \
    share_sink[1] ^= a1 ^ b1; \
    share_sink[2] ^= a2 ^ b2; \
    share_sink[3] ^= a3 ^ b3; \
} while (0)
#else
#define F2_SEL_LINEAR_SINK(lane) do { \
    uint32_t z0 = z_sh[0], z1 = z_sh[1], z2 = z_sh[2], z3 = z_sh[3]; \
    uint32_t z4 = z_sh[4]; \
    F2_SEL_LOAD_MATCH_D4(lane); \
    share_sink[0] ^= a0 ^ b0; \
    share_sink[1] ^= a1 ^ b1; \
    share_sink[2] ^= a2 ^ b2; \
    share_sink[3] ^= a3 ^ b3; \
    share_sink[4] ^= a4 ^ b4; \
} while (0)
#endif
    F2_SEL_LINEAR_SINK(0);
    F2_SEL_LINEAR_SINK(1);
    F2_SEL_LINEAR_SINK(2);
    F2_SEL_LINEAR_SINK(3);
#undef F2_SEL_LINEAR_SINK
    elmo_endtrigger();
    return 0;
}

__attribute__((noinline))
static uint32_t
trace_isw_and_only(const uint32_t a_sh[4][F2_SELECTOR_NSHARES],
                   const uint32_t b_sh[4][F2_SELECTOR_NSHARES],
                   uint32_t rng_seed)
{
    uint32_t rng = rng_seed | 1u;

    reset_share_sinks();
    elmo_starttrigger();
#if F2_SELECTOR_VARIANT == 1
#define F2_SEL_AND_SINK(lane) do { \
    uint32_t e0, e1; \
    f2_sel_and_d1(&e0, &e1, \
                  a_sh[(lane)][0], a_sh[(lane)][1], \
                  b_sh[(lane)][0], b_sh[(lane)][1], &rng); \
    share_sink[0] ^= e0; \
    share_sink[1] ^= e1; \
} while (0)
#elif F2_SELECTOR_VARIANT == 2
#define F2_SEL_AND_SINK(lane) do { \
    uint32_t e0, e1, e2; \
    f2_sel_and_d2(&e0, &e1, &e2, \
                  a_sh[(lane)][0], a_sh[(lane)][1], a_sh[(lane)][2], \
                  b_sh[(lane)][0], b_sh[(lane)][1], b_sh[(lane)][2], &rng); \
    share_sink[0] ^= e0; \
    share_sink[1] ^= e1; \
    share_sink[2] ^= e2; \
} while (0)
#elif F2_SELECTOR_VARIANT == 3
#define F2_SEL_AND_SINK(lane) do { \
    uint32_t e0, e1, e2, e3; \
    f2_sel_and_d3(&e0, &e1, &e2, &e3, \
                  a_sh[(lane)][0], a_sh[(lane)][1], \
                  a_sh[(lane)][2], a_sh[(lane)][3], \
                  b_sh[(lane)][0], b_sh[(lane)][1], \
                  b_sh[(lane)][2], b_sh[(lane)][3], &rng); \
    share_sink[0] ^= e0; \
    share_sink[1] ^= e1; \
    share_sink[2] ^= e2; \
    share_sink[3] ^= e3; \
} while (0)
#else
#define F2_SEL_AND_SINK(lane) do { \
    uint32_t e0, e1, e2, e3, e4; \
    f2_sel_and_d4(&e0, &e1, &e2, &e3, &e4, \
                  a_sh[(lane)][0], a_sh[(lane)][1], \
                  a_sh[(lane)][2], a_sh[(lane)][3], a_sh[(lane)][4], \
                  b_sh[(lane)][0], b_sh[(lane)][1], \
                  b_sh[(lane)][2], b_sh[(lane)][3], b_sh[(lane)][4], &rng); \
    share_sink[0] ^= e0; \
    share_sink[1] ^= e1; \
    share_sink[2] ^= e2; \
    share_sink[3] ^= e3; \
    share_sink[4] ^= e4; \
} while (0)
#endif
    F2_SEL_AND_SINK(0);
    F2_SEL_AND_SINK(1);
    F2_SEL_AND_SINK(2);
    F2_SEL_AND_SINK(3);
#undef F2_SEL_AND_SINK
    elmo_endtrigger();
    return 0;
}

__attribute__((noinline))
static uint32_t
trace_isw_and_one_share_sink(const uint32_t a_sh[4][F2_SELECTOR_NSHARES],
                             const uint32_t b_sh[4][F2_SELECTOR_NSHARES],
                             uint32_t rng_seed)
{
    uint32_t rng = rng_seed | 1u;

    reset_share_sinks();
    elmo_starttrigger();
#if F2_SELECTOR_VARIANT == 1
#define F2_SEL_AND_ONE_SINK(lane) do { \
    uint32_t eq[F2_SELECTOR_NSHARES]; \
    f2_sel_and_d1(&eq[0], &eq[1], \
                  a_sh[(lane)][0], a_sh[(lane)][1], \
                  b_sh[(lane)][0], b_sh[(lane)][1], &rng); \
    sink_one_share(eq); \
} while (0)
#elif F2_SELECTOR_VARIANT == 2
#define F2_SEL_AND_ONE_SINK(lane) do { \
    uint32_t eq[F2_SELECTOR_NSHARES]; \
    f2_sel_and_d2(&eq[0], &eq[1], &eq[2], \
                  a_sh[(lane)][0], a_sh[(lane)][1], a_sh[(lane)][2], \
                  b_sh[(lane)][0], b_sh[(lane)][1], b_sh[(lane)][2], &rng); \
    sink_one_share(eq); \
} while (0)
#elif F2_SELECTOR_VARIANT == 3
#define F2_SEL_AND_ONE_SINK(lane) do { \
    uint32_t eq[F2_SELECTOR_NSHARES]; \
    f2_sel_and_d3(&eq[0], &eq[1], &eq[2], &eq[3], \
                  a_sh[(lane)][0], a_sh[(lane)][1], \
                  a_sh[(lane)][2], a_sh[(lane)][3], \
                  b_sh[(lane)][0], b_sh[(lane)][1], \
                  b_sh[(lane)][2], b_sh[(lane)][3], &rng); \
    sink_one_share(eq); \
} while (0)
#else
#define F2_SEL_AND_ONE_SINK(lane) do { \
    uint32_t eq[F2_SELECTOR_NSHARES]; \
    f2_sel_and_d4(&eq[0], &eq[1], &eq[2], &eq[3], &eq[4], \
                  a_sh[(lane)][0], a_sh[(lane)][1], \
                  a_sh[(lane)][2], a_sh[(lane)][3], a_sh[(lane)][4], \
                  b_sh[(lane)][0], b_sh[(lane)][1], \
                  b_sh[(lane)][2], b_sh[(lane)][3], b_sh[(lane)][4], &rng); \
    sink_one_share(eq); \
} while (0)
#endif
    F2_SEL_AND_ONE_SINK(0);
    F2_SEL_AND_ONE_SINK(1);
    F2_SEL_AND_ONE_SINK(2);
    F2_SEL_AND_ONE_SINK(3);
#undef F2_SEL_AND_ONE_SINK
    elmo_endtrigger();
    return 0;
}

__attribute__((noinline))
static uint32_t
trace_eq_only(const volatile uint32_t z_sh[F2_SELECTOR_NSHARES],
              uint32_t rng_seed)
{
    uint32_t eq[F2_SELECTOR_NSHARES];
    uint32_t rng = rng_seed | 1u;

    reset_share_sinks();
    elmo_starttrigger();
#if F2_SELECTOR_VARIANT == 1
#define F2_SEL_EQ_CALL(lane) \
    f2_sel_eq2_const_d1_branchless(z_sh, (lane), eq, &rng)
#define F2_SEL_EQ_CONSUME() do { \
    share_sink[0] ^= eq[0]; \
    share_sink[1] ^= eq[1]; \
} while (0)
#elif F2_SELECTOR_VARIANT == 2
#define F2_SEL_EQ_CALL(lane) \
    f2_sel_eq2_const_d2_branchless(z_sh, (lane), eq, &rng)
#define F2_SEL_EQ_CONSUME() do { \
    share_sink[0] ^= eq[0]; \
    share_sink[1] ^= eq[1]; \
    share_sink[2] ^= eq[2]; \
} while (0)
#elif F2_SELECTOR_VARIANT == 3
#define F2_SEL_EQ_CALL(lane) \
    f2_sel_eq2_const_d3_branchless(z_sh, (lane), eq, &rng)
#define F2_SEL_EQ_CONSUME() do { \
    share_sink[0] ^= eq[0]; \
    share_sink[1] ^= eq[1]; \
    share_sink[2] ^= eq[2]; \
    share_sink[3] ^= eq[3]; \
} while (0)
#else
#define F2_SEL_EQ_CALL(lane) \
    f2_sel_eq2_const_d4_branchless(z_sh, (lane), eq, &rng)
#define F2_SEL_EQ_CONSUME() do { \
    share_sink[0] ^= eq[0]; \
    share_sink[1] ^= eq[1]; \
    share_sink[2] ^= eq[2]; \
    share_sink[3] ^= eq[3]; \
    share_sink[4] ^= eq[4]; \
} while (0)
#endif
#define F2_SEL_EQ_SINK(lane) do { \
    F2_SEL_EQ_CALL(lane); \
    F2_SEL_EQ_CONSUME(); \
} while (0)
    F2_SEL_EQ_SINK(0);
    F2_SEL_EQ_SINK(1);
    F2_SEL_EQ_SINK(2);
    F2_SEL_EQ_SINK(3);
#undef F2_SEL_EQ_SINK
#undef F2_SEL_EQ_CALL
#undef F2_SEL_EQ_CONSUME
    elmo_endtrigger();
    return 0;
}

__attribute__((noinline))
static uint32_t
trace_eq_no_sink(const volatile uint32_t z_sh[F2_SELECTOR_NSHARES],
                 uint32_t rng_seed)
{
    uint32_t eq[F2_SELECTOR_NSHARES];
    uint32_t rng = rng_seed | 1u;

    elmo_starttrigger();
#if F2_SELECTOR_VARIANT == 1
#define F2_SEL_EQ_NO_SINK_CALL(lane) \
    f2_sel_eq2_const_d1_branchless(z_sh, (lane), eq, &rng)
#elif F2_SELECTOR_VARIANT == 2
#define F2_SEL_EQ_NO_SINK_CALL(lane) \
    f2_sel_eq2_const_d2_branchless(z_sh, (lane), eq, &rng)
#elif F2_SELECTOR_VARIANT == 3
#define F2_SEL_EQ_NO_SINK_CALL(lane) \
    f2_sel_eq2_const_d3_branchless(z_sh, (lane), eq, &rng)
#else
#define F2_SEL_EQ_NO_SINK_CALL(lane) \
    f2_sel_eq2_const_d4_branchless(z_sh, (lane), eq, &rng)
#endif
#define F2_SEL_EQ_NO_SINK(lane) do { \
    F2_SEL_EQ_NO_SINK_CALL(lane); \
    consume_share_registers(eq); \
} while (0)
    F2_SEL_EQ_NO_SINK(0);
    F2_SEL_EQ_NO_SINK(1);
    F2_SEL_EQ_NO_SINK(2);
    F2_SEL_EQ_NO_SINK(3);
#undef F2_SEL_EQ_NO_SINK
#undef F2_SEL_EQ_NO_SINK_CALL
    elmo_endtrigger();
    return 0;
}

__attribute__((noinline))
static uint32_t
trace_eq_one_share_sink(const volatile uint32_t z_sh[F2_SELECTOR_NSHARES],
                        uint32_t rng_seed)
{
    uint32_t eq[F2_SELECTOR_NSHARES];
    uint32_t rng = rng_seed | 1u;

    reset_share_sinks();
    elmo_starttrigger();
#if F2_SELECTOR_VARIANT == 1
#define F2_SEL_EQ_ONE_SINK_CALL(lane) \
    f2_sel_eq2_const_d1_branchless(z_sh, (lane), eq, &rng)
#elif F2_SELECTOR_VARIANT == 2
#define F2_SEL_EQ_ONE_SINK_CALL(lane) \
    f2_sel_eq2_const_d2_branchless(z_sh, (lane), eq, &rng)
#elif F2_SELECTOR_VARIANT == 3
#define F2_SEL_EQ_ONE_SINK_CALL(lane) \
    f2_sel_eq2_const_d3_branchless(z_sh, (lane), eq, &rng)
#else
#define F2_SEL_EQ_ONE_SINK_CALL(lane) \
    f2_sel_eq2_const_d4_branchless(z_sh, (lane), eq, &rng)
#endif
#define F2_SEL_EQ_ONE_SINK(lane) do { \
    F2_SEL_EQ_ONE_SINK_CALL(lane); \
    sink_one_share(eq); \
} while (0)
    F2_SEL_EQ_ONE_SINK(0);
    F2_SEL_EQ_ONE_SINK(1);
    F2_SEL_EQ_ONE_SINK(2);
    F2_SEL_EQ_ONE_SINK(3);
#undef F2_SEL_EQ_ONE_SINK
#undef F2_SEL_EQ_ONE_SINK_CALL
    elmo_endtrigger();
    return 0;
}

__attribute__((noinline))
static uint32_t
trace_select_only(const uint32_t eq_sh[4][F2_SELECTOR_NSHARES],
                  const volatile uint32_t berexp_results[4],
                  const volatile uint32_t z_candidates[4])
{
    uint32_t sa0 = 0, sz0 = 0;
#if F2_SELECTOR_NSHARES > 1
    uint32_t sa1 = 0, sz1 = 0;
#endif
#if F2_SELECTOR_NSHARES > 2
    uint32_t sa2 = 0, sz2 = 0;
#endif
#if F2_SELECTOR_NSHARES > 3
    uint32_t sa3 = 0, sz3 = 0;
#endif
#if F2_SELECTOR_NSHARES > 4
    uint32_t sa4 = 0, sz4 = 0;
#endif

    reset_share_sinks();
    elmo_starttrigger();
#if F2_SELECTOR_VARIANT == 1
#define F2_SEL_SELECT_ACCUM(lane) do { \
    uint32_t accept = berexp_results[(lane)] & 1u; \
    uint32_t value = z_candidates[(lane)]; \
    sa0 ^= eq_sh[(lane)][0] & accept; \
    sa1 ^= eq_sh[(lane)][1] & accept; \
    sz0 ^= f2_sel_expand_bit(eq_sh[(lane)][0]) & value; \
    sz1 ^= f2_sel_expand_bit(eq_sh[(lane)][1]) & value; \
} while (0)
#elif F2_SELECTOR_VARIANT == 2
#define F2_SEL_SELECT_ACCUM(lane) do { \
    uint32_t accept = berexp_results[(lane)] & 1u; \
    uint32_t value = z_candidates[(lane)]; \
    sa0 ^= eq_sh[(lane)][0] & accept; \
    sa1 ^= eq_sh[(lane)][1] & accept; \
    sa2 ^= eq_sh[(lane)][2] & accept; \
    sz0 ^= f2_sel_expand_bit(eq_sh[(lane)][0]) & value; \
    sz1 ^= f2_sel_expand_bit(eq_sh[(lane)][1]) & value; \
    sz2 ^= f2_sel_expand_bit(eq_sh[(lane)][2]) & value; \
} while (0)
#elif F2_SELECTOR_VARIANT == 3
#define F2_SEL_SELECT_ACCUM(lane) do { \
    uint32_t accept = berexp_results[(lane)] & 1u; \
    uint32_t value = z_candidates[(lane)]; \
    sa0 ^= eq_sh[(lane)][0] & accept; \
    sa1 ^= eq_sh[(lane)][1] & accept; \
    sa2 ^= eq_sh[(lane)][2] & accept; \
    sa3 ^= eq_sh[(lane)][3] & accept; \
    sz0 ^= f2_sel_expand_bit(eq_sh[(lane)][0]) & value; \
    sz1 ^= f2_sel_expand_bit(eq_sh[(lane)][1]) & value; \
    sz2 ^= f2_sel_expand_bit(eq_sh[(lane)][2]) & value; \
    sz3 ^= f2_sel_expand_bit(eq_sh[(lane)][3]) & value; \
} while (0)
#else
#define F2_SEL_SELECT_ACCUM(lane) do { \
    uint32_t accept = berexp_results[(lane)] & 1u; \
    uint32_t value = z_candidates[(lane)]; \
    sa0 ^= eq_sh[(lane)][0] & accept; \
    sa1 ^= eq_sh[(lane)][1] & accept; \
    sa2 ^= eq_sh[(lane)][2] & accept; \
    sa3 ^= eq_sh[(lane)][3] & accept; \
    sa4 ^= eq_sh[(lane)][4] & accept; \
    sz0 ^= f2_sel_expand_bit(eq_sh[(lane)][0]) & value; \
    sz1 ^= f2_sel_expand_bit(eq_sh[(lane)][1]) & value; \
    sz2 ^= f2_sel_expand_bit(eq_sh[(lane)][2]) & value; \
    sz3 ^= f2_sel_expand_bit(eq_sh[(lane)][3]) & value; \
    sz4 ^= f2_sel_expand_bit(eq_sh[(lane)][4]) & value; \
} while (0)
#endif
#define F2_SEL_SELECT_SINK(lane) do { \
    F2_SEL_SELECT_ACCUM(lane); \
} while (0)
    F2_SEL_SELECT_SINK(0);
    F2_SEL_SELECT_SINK(1);
    F2_SEL_SELECT_SINK(2);
    F2_SEL_SELECT_SINK(3);
#undef F2_SEL_SELECT_SINK
#undef F2_SEL_SELECT_ACCUM
    share_sink[0] ^= sa0 ^ sz0;
#if F2_SELECTOR_NSHARES > 1
    share_sink[1] ^= sa1 ^ sz1;
#endif
#if F2_SELECTOR_NSHARES > 2
    share_sink[2] ^= sa2 ^ sz2;
#endif
#if F2_SELECTOR_NSHARES > 3
    share_sink[3] ^= sa3 ^ sz3;
#endif
#if F2_SELECTOR_NSHARES > 4
    share_sink[4] ^= sa4 ^ sz4;
#endif
    elmo_endtrigger();
    return 0;
}

__attribute__((noinline))
static uint32_t
trace_open_only(const uint32_t accept_sh[F2_SELECTOR_NSHARES],
                const uint32_t value_sh[F2_SELECTOR_NSHARES])
{
    uint32_t accept;
    uint32_t value;

    elmo_starttrigger();
#if F2_SELECTOR_VARIANT == 1
    accept = accept_sh[0] ^ accept_sh[1];
    value = value_sh[0] ^ value_sh[1];
#elif F2_SELECTOR_VARIANT == 2
    accept = accept_sh[0] ^ accept_sh[1] ^ accept_sh[2];
    value = value_sh[0] ^ value_sh[1] ^ value_sh[2];
#elif F2_SELECTOR_VARIANT == 3
    accept = accept_sh[0] ^ accept_sh[1] ^ accept_sh[2] ^ accept_sh[3];
    value = value_sh[0] ^ value_sh[1] ^ value_sh[2] ^ value_sh[3];
#else
    accept = accept_sh[0] ^ accept_sh[1] ^ accept_sh[2] ^ accept_sh[3]
        ^ accept_sh[4];
    value = value_sh[0] ^ value_sh[1] ^ value_sh[2] ^ value_sh[3]
        ^ value_sh[4];
#endif
    value &= f2_sel_expand_bit(accept);
    elmo_endtrigger();
    return value;
}
#endif

#if F2_SELECTOR_VARIANT == 0
__attribute__((noinline))
static uint32_t
trace_dispatch_clear(uint32_t label,
                     const volatile uint32_t berexp_results[4],
                     const volatile uint32_t z_candidates[4])
{
    uint32_t result;

    elmo_starttrigger();
    result = dispatch_clear(label, berexp_results, z_candidates);
    elmo_endtrigger();
    return result;
}
#endif

#if F2_SELECTOR_VARIANT != 0
__attribute__((noinline))
static uint32_t
trace_dispatch_masked(const volatile uint32_t z_sh[F2_SELECTOR_NSHARES],
                      const volatile uint32_t berexp_results[4],
                      const volatile uint32_t z_candidates[4],
                      uint32_t rng_seed)
{
    uint32_t result;

#if F2_SELECTOR_TRACE_MODE == MODE_LOAD_ONLY
    result = trace_load_only(z_sh);
#elif F2_SELECTOR_TRACE_MODE == MODE_EQ_ONLY
    result = trace_eq_only(z_sh, rng_seed);
#elif F2_SELECTOR_TRACE_MODE == MODE_SELECT_ONLY
    uint32_t eq_sh[4][F2_SELECTOR_NSHARES];
    make_synthetic_onehot(eq_sh, rng_seed);
    result = trace_select_only(eq_sh, berexp_results, z_candidates);
#elif F2_SELECTOR_TRACE_MODE == MODE_OPEN_ONLY
    uint32_t accept_sh[F2_SELECTOR_NSHARES];
    uint32_t value_sh[F2_SELECTOR_NSHARES];
    precompute_selected_shares(
        z_sh, berexp_results, z_candidates, accept_sh, value_sh, rng_seed);
    result = trace_open_only(accept_sh, value_sh);
#elif F2_SELECTOR_TRACE_MODE == MODE_BIT_LINEAR_ONLY
    result = trace_bit_linear_only(z_sh);
#elif F2_SELECTOR_TRACE_MODE == MODE_ISW_AND_ONLY
    uint32_t a_sh[4][F2_SELECTOR_NSHARES];
    uint32_t b_sh[4][F2_SELECTOR_NSHARES];
    precompute_match_shares(z_sh, a_sh, b_sh);
    result = trace_isw_and_only(a_sh, b_sh, rng_seed);
#elif F2_SELECTOR_TRACE_MODE == MODE_EQ_NO_SINK
    result = trace_eq_no_sink(z_sh, rng_seed);
#elif F2_SELECTOR_TRACE_MODE == MODE_EQ_ONE_SHARE_SINK
    result = trace_eq_one_share_sink(z_sh, rng_seed);
#elif F2_SELECTOR_TRACE_MODE == MODE_ISW_AND_ONE_SHARE_SINK
    uint32_t a_sh[4][F2_SELECTOR_NSHARES];
    uint32_t b_sh[4][F2_SELECTOR_NSHARES];
    precompute_match_shares(z_sh, a_sh, b_sh);
    result = trace_isw_and_one_share_sink(a_sh, b_sh, rng_seed);
#elif F2_SELECTOR_TRACE_MODE == MODE_ISW_AND_D2_ASM
    uint32_t a_sh[4][F2_SELECTOR_NSHARES];
    uint32_t b_sh[4][F2_SELECTOR_NSHARES];
    precompute_match_shares(z_sh, a_sh, b_sh);
#if F2_SELECTOR_VARIANT == 2
    result = trace_isw_and_d2_asm_window(a_sh, b_sh, rng_seed);
#else
    result = 0;
#endif
#elif F2_SELECTOR_TRACE_MODE == MODE_EQ_D2_ASM
#if F2_SELECTOR_VARIANT == 2
    result = trace_eq2_const_d2_asm_window(z_sh, rng_seed);
#else
    result = 0;
#endif
#else
    elmo_starttrigger();
#if F2_SELECTOR_VARIANT == 1
    result = dispatch_masked_d1(z_sh, berexp_results, z_candidates, rng_seed);
#elif F2_SELECTOR_VARIANT == 2
    result = dispatch_masked_d2(z_sh, berexp_results, z_candidates, rng_seed);
#elif F2_SELECTOR_VARIANT == 3
    result = dispatch_masked_d3(z_sh, berexp_results, z_candidates, rng_seed);
#else
    result = dispatch_masked_d4(z_sh, berexp_results, z_candidates, rng_seed);
#endif
    elmo_endtrigger();
#endif
    return result;
}
#endif

int
main(void)
{
    uint32_t ntraces;
    volatile uint32_t berexp_results[4] = {1, 1, 1, 1};
    volatile uint32_t z_candidates[4] = {
        0x5A5A5A5Au, 0x5A5A5A5Au, 0x5A5A5A5Au, 0x5A5A5A5Au
    };
    volatile uint32_t z_sh[F2_SELECTOR_NSHARES];

    elmo_load_n(&ntraces);
    for (uint32_t i = 0; i < ntraces; i ++) {
        uint32_t block = i >> 2;
        uint32_t label = label_perm[block % 24u][i & 3u];
        uint32_t result;
        uint32_t rng_seed;
        make_shares(z_sh, label);
        rng_seed = rand_u32();

#if F2_SELECTOR_VARIANT == 0
        result = trace_dispatch_clear(label, berexp_results, z_candidates);
#else
        result = trace_dispatch_masked(z_sh, berexp_results, z_candidates,
                                       rng_seed);
#endif
        sink ^= result;
    }

    elmo_printbyte((const unsigned char *)&sink);
    elmo_endprogram();
    return 0;
}
