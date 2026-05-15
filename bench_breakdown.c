/* bench_breakdown.c — region-level cycle breakdown of fndsa_sign.
 *
 * Companion to bench_full_chain.c: same warm-up + measurement shape, but
 * accumulates per-region cycles (fpoly_FFT, fpoly_iFFT, ffsamp_fft) across
 * NTESTS iterations and prints a percentage table against the total sign
 * cost. The audit pass uses this to rank optimization recommendations
 * by the share of cycles they actually move.
 *
 * Build (scalar fallback — the M3-shaped baseline):
 *   make CFLAGS="-W -Wextra -Wundef -Wshadow -O2 \
 *                -DFNDSA_NEON=0 -DFNDSA_SSE2=0 -DFNDSA_RV64D=0 \
 *                -DFNDSA_BENCH_REGIONS=1" \
 *        $OBJ_LIBS bench_regions.o
 *   $CC $CFLAGS -c bench_breakdown.c -o bench_breakdown.o
 *   $CC $CFLAGS -c speed_print.c     -o speed_print.o
 *   $CC -o bench_breakdown bench_breakdown.o bench_regions.o speed_print.o \
 *       $OBJ_LIBS -lm
 *
 * Run: ./bench_breakdown
 *
 * The instrumentation in sign_fpoly.c and sign_sampler.c is a no-op
 * unless -DFNDSA_BENCH_REGIONS=1 is set. Without the flag this file
 * still compiles but reports zeros for every region — verify the flag
 * reached every translation unit. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>

#include "fndsa.h"
#include "cpucycles.h"
#include "bench_regions.h"

#ifndef FNDSA_BENCH_REGIONS
#error "bench_breakdown.c requires -DFNDSA_BENCH_REGIONS=1"
#endif

#ifndef NTESTS
#define NTESTS 1000
#endif

static int
bench_at_logn(unsigned logn)
{
	size_t sk_len      = FNDSA_SIGN_KEY_SIZE(logn);
	size_t vk_len      = FNDSA_VRFY_KEY_SIZE(logn);
	size_t sig_len_max = FNDSA_SIGNATURE_SIZE(logn);
	size_t tmp_len     = ((size_t)59 << logn) + 31;

	uint8_t *sk  = malloc(sk_len);
	uint8_t *vk  = malloc(vk_len);
	uint8_t *sig = malloc(sig_len_max);
	uint8_t *tmp = malloc(tmp_len);
	if (!sk || !vk || !sig || !tmp) {
		fprintf(stderr, "logn=%u: alloc failed\n", logn);
		return 1;
	}

	uint8_t kseed[8] = {0xCA, 0xFE, 0xBA, 0xBE, (uint8_t)logn, 0, 0, 0};
	fndsa_keygen_seeded(logn, kseed, sizeof kseed, sk, vk);

	uint8_t mseed[8] = {0xDE, 0xAD, 0xBE, 0xEF, 0, 0, 0, 0};

	/* Warm-up — five untimed iterations so caches / branch predictor
	   / DVFS settle before measurement. */
	for (int i = 0; i < 5; i++) {
		mseed[4] = (uint8_t)i;
		(void)fndsa_sign_seeded_temp(
			sk, sk_len, NULL, 0, FNDSA_HASH_ID_RAW, "msg", 3,
			mseed, sizeof mseed, sig, sig_len_max, tmp, tmp_len);
	}

	/* Reset region accumulators just before the measurement loop so
	   warm-up cycles do not pollute the percentages. */
	fndsa_bench_region_reset();

	uint64_t total_start = cpucycles();
	for (int i = 0; i < NTESTS; i++) {
		mseed[4] = (uint8_t)i;
		mseed[5] = (uint8_t)(i >> 8);
		size_t l = fndsa_sign_seeded_temp(
			sk, sk_len, NULL, 0, FNDSA_HASH_ID_RAW, "msg", 3,
			mseed, sizeof mseed, sig, sig_len_max, tmp, tmp_len);
		if (l == 0) {
			fprintf(stderr, "logn=%u iter=%d: sign FAILED\n", logn, i);
			return 1;
		}
	}
	uint64_t total_cycles = cpucycles() - total_start;

	printf("\n=== Region breakdown logn=%u (NTESTS=%d, unit=%s) ===\n",
		logn, NTESTS, CPUCYCLES_UNIT);
	printf("total sign cycles (sum over %d ops): %" PRIu64 "\n",
		NTESTS, total_cycles);
	printf("per-sign average:                     %" PRIu64 "\n\n",
		total_cycles / (uint64_t)NTESTS);

	/* Region layout (non-overlap structure verified by code inspection):
	     FFT, iFFT, FFSAMP are top-level (non-overlapping with each other).
	     SAMPLER is nested inside FFSAMP (every sampler_next call happens
	     during ffsamp_fft_inner recursion). To avoid double-counting,
	     Other = total - (FFT + iFFT + FFSAMP). SAMPLER is reported with
	     its share of total AND its share of FFSAMP separately. */
	uint64_t cyc_fft     = fndsa_bench_region_cycles[BENCH_REGION_FFT];
	uint64_t cyc_ifft    = fndsa_bench_region_cycles[BENCH_REGION_IFFT];
	uint64_t cyc_ffsamp  = fndsa_bench_region_cycles[BENCH_REGION_FFSAMP];
	uint64_t cyc_sampler = fndsa_bench_region_cycles[BENCH_REGION_SAMPLER];
	uint64_t top_level = cyc_fft + cyc_ifft + cyc_ffsamp;

	printf("%-14s %16s %8s %10s %12s %16s\n",
		"region", "cycles", "% total", "% ffsamp", "calls", "cyc/call");
	printf("%-14s %16s %8s %10s %12s %16s\n",
		"------", "------", "-------", "--------", "-----", "--------");
	for (int i = 0; i < BENCH_REGION_COUNT; i++) {
		uint64_t cyc   = fndsa_bench_region_cycles[i];
		uint64_t calls = fndsa_bench_region_calls[i];
		double pct = total_cycles
			? 100.0 * (double)cyc / (double)total_cycles
			: 0.0;
		uint64_t per_call = calls ? cyc / calls : 0;
		/* % ffsamp is meaningful only for SAMPLER (nested) and FFSAMP
		   itself (== 100%). Print '-' otherwise so the column doesn't
		   suggest spurious nesting. */
		if (i == BENCH_REGION_SAMPLER || i == BENCH_REGION_FFSAMP) {
			double pct_ffsamp = cyc_ffsamp
				? 100.0 * (double)cyc / (double)cyc_ffsamp
				: 0.0;
			printf("%-14s %16" PRIu64 " %7.2f%% %9.2f%% %12" PRIu64
				" %16" PRIu64 "\n",
				fndsa_bench_region_name[i], cyc, pct, pct_ffsamp,
				calls, per_call);
		} else {
			printf("%-14s %16" PRIu64 " %7.2f%% %10s %12" PRIu64
				" %16" PRIu64 "\n",
				fndsa_bench_region_name[i], cyc, pct, "-", calls,
				per_call);
		}
	}

	/* Per-primitive call counts. cm4_breakdown.py multiplies these by the
	   per-routine cycle figures from cm4_cycles.py to project M4 cost.
	   Lines tagged "PRIM:" are the parser contract — keep stable. */
	printf("\nPrimitive calls per sign (averaged over %d ops):\n", NTESTS);
	printf("%-14s %16s %16s\n", "primitive", "total", "per-sign");
	printf("%-14s %16s %16s\n", "---------", "-----", "--------");
	for (int i = 0; i < BENCH_PRIM_COUNT; i++) {
		uint64_t calls = fndsa_bench_prim_calls[i];
		uint64_t per   = calls / (uint64_t)NTESTS;
		printf("%-14s %16" PRIu64 " %16" PRIu64 "\n",
			fndsa_bench_prim_name[i], calls, per);
		printf("PRIM:%u:%s:%" PRIu64 ":%" PRIu64 "\n",
			(unsigned)logn, fndsa_bench_prim_name[i], calls, per);
	}

	/* |Δexp| histogram for fpr_add_sub. Used to decide whether the
	   bounded-exponent specialization (fpr_add_sub_close) is viable.
	   Buckets: ≤3, ≤7, ≤15, ≤31, ≤63, >63. */
	uint64_t dexp_total = 0;
	for (int i = 0; i < BENCH_DEXP_BUCKETS; i++) {
		dexp_total += fndsa_bench_dexp_hist[i];
	}
	if (dexp_total > 0) {
		printf("\n|Δexp| distribution for fpr_add_sub (cumulative):\n");
		printf("%-10s %16s %10s %12s\n",
			"|Δexp|", "calls", "%", "cumul %");
		printf("%-10s %16s %10s %12s\n",
			"------", "-----", "---", "-------");
		const char *labels[BENCH_DEXP_BUCKETS] = {
			"≤ 3", "≤ 7", "≤ 15", "≤ 31", "≤ 63", "> 63"
		};
		uint64_t cumul = 0;
		for (int i = 0; i < BENCH_DEXP_BUCKETS; i++) {
			uint64_t c = fndsa_bench_dexp_hist[i];
			cumul += c;
			double pct = 100.0 * (double)c / (double)dexp_total;
			double cpct = 100.0 * (double)cumul / (double)dexp_total;
			printf("%-10s %16" PRIu64 " %9.2f%% %11.2f%%\n",
				labels[i], c, pct, cpct);
			printf("DEXP:%u:%d:%" PRIu64 "\n",
				(unsigned)logn, fndsa_bench_dexp_lo[i], c);
		}
	}

	/* "Other" bucket: total minus top-level regions (FFT + iFFT + FFSAMP).
	   SAMPLER is excluded from the sum because it lives inside FFSAMP. */
	if (top_level > total_cycles) {
		printf("\nWARNING: top-level regions sum to %" PRIu64
			" > total %" PRIu64 " — accounting bug or timer wrap "
			"(check cpucycles unit; DWT is 32-bit and wraps).\n",
			top_level, total_cycles);
	} else {
		uint64_t other = total_cycles - top_level;
		double other_pct = total_cycles
			? 100.0 * (double)other / (double)total_cycles
			: 0.0;
		printf("%-14s %16" PRIu64 " %7.2f%% %10s %12s %16s\n",
			"Other", other, other_pct, "-", "-", "-");
		if (other_pct > 30.0) {
			printf("NOTE: Other is %.1f%% of total — add brackets to "
				"unattributed code paths (hash_to_point, encoding, "
				"setup) before optimizing the named regions.\n",
				other_pct);
		}
	}

	free(tmp); free(sig); free(vk); free(sk);
	return 0;
}

int main(void)
{
	printf("# bench_breakdown  config=scalar+FNDSA_BENCH_REGIONS  "
		"NTESTS=%d  unit=%s\n", NTESTS, CPUCYCLES_UNIT);
	int failures = 0;
	if (bench_at_logn(9)  != 0) failures++;
	if (bench_at_logn(10) != 0) failures++;
	return failures;
}
