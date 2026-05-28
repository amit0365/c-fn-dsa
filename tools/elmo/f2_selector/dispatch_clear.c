#include <stdint.h>

uint32_t
dispatch_clear(uint32_t z0_idx,
               const volatile uint32_t berexp_results[4],
               const volatile uint32_t z_candidates[4])
{
    z0_idx &= 3u;
    if ((berexp_results[z0_idx] & 1u) != 0) {
        return z_candidates[z0_idx];
    }
    return 0;
}

#ifdef F2_SELECTOR_TEST_MAIN
int
main(void)
{
    volatile uint32_t berexp_results[4] = {1, 1, 1, 1};
    volatile uint32_t z_candidates[4] = {101, 211, 307, 419};
    volatile uint32_t acc = 0;
    for (uint32_t i = 0; i < 1024; i ++) {
        acc ^= dispatch_clear(i & 3u, berexp_results, z_candidates);
    }
    return (int)(acc & 0xFFu);
}
#endif
