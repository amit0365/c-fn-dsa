/* bench_regions.c — global accumulators for region-level cycle accounting.
 * See bench_regions.h. Only compiled into the audit binary; everything
 * else links normally without referencing these symbols. */

#ifdef FNDSA_BENCH_REGIONS

#include "bench_regions.h"

uint64_t fndsa_bench_region_cycles[BENCH_REGION_COUNT];
uint64_t fndsa_bench_region_calls[BENCH_REGION_COUNT];

const char *fndsa_bench_region_name[BENCH_REGION_COUNT] = {
	"fpoly_FFT",
	"fpoly_iFFT",
	"ffsamp_fft",
	"sampler_next",
};

uint64_t fndsa_bench_prim_calls[BENCH_PRIM_COUNT];

const char *fndsa_bench_prim_name[BENCH_PRIM_COUNT] = {
	"fpr_add",
	"fpr_add_sub",
	"fpr_mul",
	"fpr_div",
	"fpr_sqrt",
	"fpr_scaled",
};

uint64_t fndsa_bench_dexp_hist[BENCH_DEXP_BUCKETS];
const int fndsa_bench_dexp_lo[BENCH_DEXP_BUCKETS] = {0, 4, 8, 16, 32, 64};

#endif
