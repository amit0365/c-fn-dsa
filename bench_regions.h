/* bench_regions.h — region-level cycle accounting for the audit pass.
 *
 * Gated by FNDSA_BENCH_REGIONS. When undefined (the normal build), every
 * macro expands to ((void)0) — zero runtime cost, zero binary delta.
 *
 * Usage inside a target function:
 *
 *     void fpoly_FFT(unsigned logn, fpr *f) {
 *         BENCH_REGION_ENTER(BENCH_REGION_FFT);
 *         ... existing body ...
 *         BENCH_REGION_LEAVE(BENCH_REGION_FFT);
 *     }
 *
 * Notes:
 *   - Counters are NOT thread-safe (single-threaded benchmark only).
 *   - ENTER/LEAVE must be in the same scope; do not split across branches.
 *   - Nested regions accumulate correctly: an enter/leave pair always adds
 *     (leave - enter) cycles to the named region, including time spent in
 *     any inner regions.
 *   - The cycle unit is whatever cpucycles.h emits for the host (TSC ticks
 *     on x86, CNTVCT on aarch64, DWT cycles on Cortex-M, ns on portable
 *     fallback). Cross-platform absolute comparisons are meaningless; the
 *     same-platform percentages are what the audit cares about. */

#ifndef FNDSA_BENCH_REGIONS_H
#define FNDSA_BENCH_REGIONS_H

#ifdef FNDSA_BENCH_REGIONS

#include <stdint.h>
#include "cpucycles.h"

enum {
	BENCH_REGION_FFT = 0,         /* every fpoly_FFT call (cumulative) */
	BENCH_REGION_IFFT,            /* every fpoly_iFFT call (cumulative) */
	BENCH_REGION_FFSAMP,          /* the outer ffsamp_fft() wrapper */
	BENCH_REGION_SAMPLER,         /* every sampler_next() call (nested inside FFSAMP) */
	BENCH_REGION_COUNT
};

/* Primitive call counters. Used by cm4_breakdown.py to project M4 cycles:
   per-primitive M4 cycles = count(P) * cycles_cm4(P), summed.
   ADD_SUB is counted separately from ADD because the M4 has a fused 86-cycle
   add_sub routine; the scalar build expands FPR_ADD_SUB() to add+sub (= 2 adds
   in the ADD counter), so the projection script must subtract 2*add_sub from
   the raw add count to recover standalone-add cost. */
enum {
	BENCH_PRIM_ADD = 0,
	BENCH_PRIM_ADD_SUB,
	BENCH_PRIM_MUL,
	BENCH_PRIM_DIV,
	BENCH_PRIM_SQRT,
	BENCH_PRIM_SCALED,
	BENCH_PRIM_COUNT
};

extern const char *fndsa_bench_region_name[BENCH_REGION_COUNT];
extern uint64_t fndsa_bench_region_cycles[BENCH_REGION_COUNT];
extern uint64_t fndsa_bench_region_calls[BENCH_REGION_COUNT];

extern const char *fndsa_bench_prim_name[BENCH_PRIM_COUNT];
extern uint64_t fndsa_bench_prim_calls[BENCH_PRIM_COUNT];

/* Histogram of |Δexp| for fpr_add_sub calls, to decide whether a
   bounded-exponent specialization is worth writing. Bucket i counts
   calls where |Δexp| is in [bucket_lo[i], bucket_lo[i+1]). */
#define BENCH_DEXP_BUCKETS 6
extern uint64_t fndsa_bench_dexp_hist[BENCH_DEXP_BUCKETS];
extern const int fndsa_bench_dexp_lo[BENCH_DEXP_BUCKETS];

/* Compute |exp(x) - exp(y)| from raw fpr bit patterns; cheap bit ops only. */
static inline void
fndsa_bench_dexp_record(uint64_t x, uint64_t y)
{
	int ex = (int)((x >> 52) & 0x7FF);
	int ey = (int)((y >> 52) & 0x7FF);
	int d  = ex > ey ? ex - ey : ey - ex;
	int b  = 0;
	if      (d <=  3) b = 0;
	else if (d <=  7) b = 1;
	else if (d <= 15) b = 2;
	else if (d <= 31) b = 3;
	else if (d <= 63) b = 4;
	else              b = 5;
	fndsa_bench_dexp_hist[b]++;
}

#define BENCH_REGION_ENTER(R)   uint64_t _bench_t_##R = cpucycles()
#define BENCH_REGION_LEAVE(R)   do { \
		fndsa_bench_region_cycles[R] += cpucycles() - _bench_t_##R; \
		fndsa_bench_region_calls[R]  += 1; \
	} while (0)

#define BENCH_PRIM_INC(P)       (fndsa_bench_prim_calls[P]++)
#define BENCH_DEXP_RECORD(X, Y) fndsa_bench_dexp_record((X), (Y))

static inline void
fndsa_bench_region_reset(void)
{
	for (int i = 0; i < BENCH_REGION_COUNT; i++) {
		fndsa_bench_region_cycles[i] = 0;
		fndsa_bench_region_calls[i]  = 0;
	}
	for (int i = 0; i < BENCH_PRIM_COUNT; i++) {
		fndsa_bench_prim_calls[i] = 0;
	}
	for (int i = 0; i < BENCH_DEXP_BUCKETS; i++) {
		fndsa_bench_dexp_hist[i] = 0;
	}
}

#else  /* !FNDSA_BENCH_REGIONS */

#define BENCH_REGION_ENTER(R)   ((void)0)
#define BENCH_REGION_LEAVE(R)   ((void)0)
#define BENCH_PRIM_INC(P)       ((void)0)
#define BENCH_DEXP_RECORD(X, Y) ((void)0)

#endif

#endif /* FNDSA_BENCH_REGIONS_H */
