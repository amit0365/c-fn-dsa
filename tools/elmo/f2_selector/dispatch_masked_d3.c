#include <stdint.h>

#define F2_SELECTOR_ORDER 3
#include "mask_gadgets.h"

uint32_t
dispatch_masked_d3(const volatile uint32_t z_sh[F2_SELECTOR_NSHARES],
                   const volatile uint32_t berexp_results[4],
                   const volatile uint32_t z_candidates[4],
                   uint32_t rng_seed)
{
    return f2_sel_dispatch_masked_d3_branchless(
        z_sh, berexp_results, z_candidates, rng_seed);
}

#ifdef F2_SELECTOR_TEST_MAIN
int
main(void)
{
    volatile uint32_t z_sh[F2_SELECTOR_NSHARES] = {0, 1, 2, 3};
    volatile uint32_t berexp_results[4] = {1, 1, 1, 1};
    volatile uint32_t z_candidates[4] = {101, 211, 307, 419};
    volatile uint32_t acc = 0;
    for (uint32_t i = 0; i < 1024; i ++) {
        z_sh[1] = i & 3u;
        z_sh[2] = (i >> 2) & 3u;
        z_sh[3] = (i >> 4) & 3u;
        z_sh[0] = 3u ^ z_sh[1] ^ z_sh[2] ^ z_sh[3];
        acc ^= dispatch_masked_d3(z_sh, berexp_results, z_candidates,
                                  0x9E3779B9u ^ i);
    }
    return (int)(acc & 0xFFu);
}
#endif
