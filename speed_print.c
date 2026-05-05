/* speed_print.c — adapted from pq-crystals/dilithium ref/test/speed_print.c
 * (CC0-1.0 / public domain). Same arithmetic, same output column layout:
 *
 *   <label> median: <cycles>, average: <cycles>, min: <cycles>, max: <cycles>
 *
 * Input is an NTESTS-element array of cpucycles() snapshots taken
 * immediately before each op. The function computes NTESTS-1 pairwise
 * differences (each = one op's cycle cost), sorts them in place, and
 * reports median (middle element), arithmetic mean, min, and max. */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include "speed_print.h"
#include "cpucycles.h"

static int
cmp_uint64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a;
	uint64_t y = *(const uint64_t *)b;
	if (x < y) return -1;
	if (x > y) return  1;
	return 0;
}

void
print_results(const char *label, uint64_t *t, size_t tlen)
{
	if (tlen < 2) {
		printf("%s: insufficient samples (%zu)\n", label, tlen);
		return;
	}

	/* Convert NTESTS timestamps into NTESTS-1 per-op differences in place. */
	size_t n = tlen - 1;
	for (size_t i = 0; i < n; i++) {
		t[i] = t[i + 1] - t[i];
	}

	/* Sort for median + min + max. Mean is computed from the sorted
	   array (sum is order-independent in exact arithmetic; for u64
	   addition there's no rounding). */
	qsort(t, n, sizeof(uint64_t), cmp_uint64);

	uint64_t median = t[n / 2];
	uint64_t min    = t[0];
	uint64_t max    = t[n - 1];

	uint64_t sum = 0;
	for (size_t i = 0; i < n; i++) {
		sum += t[i];
	}
	uint64_t mean = sum / n;

	printf("%s median: %llu, average: %llu, min: %llu, max: %llu (%s, n=%zu)\n",
		label,
		(unsigned long long)median,
		(unsigned long long)mean,
		(unsigned long long)min,
		(unsigned long long)max,
		CPUCYCLES_UNIT, n);
}
