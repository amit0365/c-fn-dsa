/* cpucycles.h — frequency-invariant cycle counter, SUPERCOP / PQ-Crystals
 * style. Used by bench_full_chain.c so the benchmark output matches the
 * shape of pq-crystals/dilithium and pq-crystals/kyber's test_speed.c
 * (median + min/avg/max of NTESTS individual op timings).
 *
 * Arch dispatch:
 *   x86_64                : RDTSC (TSC ticks; on modern CPUs this is the
 *                           ARAT-invariant TSC, independent of P-state).
 *   aarch64               : MRS CNTVCT_EL0 (virtual counter; ticks at a
 *                           fixed system-counter frequency, unaffected by
 *                           DVFS — see CNTFRQ_EL0 for the rate).
 *   Cortex-M (ARMv7M+)    : DWT->CYCCNT (32-bit cycle counter; caller
 *                           must enable TRCENA + CYCCNTENA at boot).
 *   else                  : clock_gettime(CLOCK_MONOTONIC) in ns. Loses
 *                           cycle accuracy but keeps the harness portable.
 *
 * Note: returned values are platform-defined units (TSC ticks, CNTVCT
 * ticks, M-class cycles, or ns). Differences between successive readings
 * are the per-op metric; never compare absolute values across platforms. */

#ifndef CPUCYCLES_H
#define CPUCYCLES_H

#include <stdint.h>

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
static inline uint64_t cpucycles(void) {
	uint32_t lo, hi;
	__asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
	return ((uint64_t)hi << 32) | (uint64_t)lo;
}
#define CPUCYCLES_UNIT "TSC"

#elif defined(__aarch64__) || defined(_M_ARM64)
static inline uint64_t cpucycles(void) {
	uint64_t v;
	__asm__ volatile("mrs %0, cntvct_el0" : "=r"(v));
	return v;
}
#define CPUCYCLES_UNIT "CNTVCT"

#elif defined(__ARM_ARCH_7M__) || defined(__ARM_ARCH_7EM__)
/* Cortex-M3/M4: DWT cycle counter. The board's startup code must have
   set CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk and
        DWT->CTRL    |= DWT_CTRL_CYCCNTENA_Msk
   for this to return non-zero. 32-bit, wraps at ~2^32 cycles. */
#include <stdint.h>
#define DWT_CYCCNT (*(volatile uint32_t *)0xE0001004)
static inline uint64_t cpucycles(void) {
	return (uint64_t)DWT_CYCCNT;
}
#define CPUCYCLES_UNIT "DWT"

#else
/* Portable fallback. Not a cycle count — returns nanoseconds — but lets
   the harness compile on hosts without a usable cycle counter. */
#include <time.h>
static inline uint64_t cpucycles(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}
#define CPUCYCLES_UNIT "ns"
#endif

#endif /* CPUCYCLES_H */
