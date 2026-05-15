#!/bin/bash
# run_breakdown.sh — build and run bench_breakdown in the scalar config
# (FNDSA_NEON=0 FNDSA_SSE2=0 FNDSA_RV64D=0), with FNDSA_BENCH_REGIONS=1
# threaded through every translation unit so the instrumentation in
# sign_fpoly.c / sign_sampler.c is live.
#
# Output: total sign cycles, per-sign cycles, and (once you implement
# the print block in bench_breakdown.c) the per-region percentage table.
#
# Usage:
#   ./run_breakdown.sh         # default NTESTS=1000
#   NTESTS=5000 ./run_breakdown.sh
#
# Compare against ./run_bench.sh's scalar baseline — total sign cycles
# should match within ~1-2% (the bracket macros add a few hundred cycles
# per call; with ~10 FFT calls + ~2 iFFT + 1 ffsamp per sign, overhead
# is well under 0.1% of total).

set -e

CC="${CC:-clang}"
NTESTS="${NTESTS:-1000}"
CFLAGS="-W -Wextra -Wundef -Wshadow -O2 \
        -DFNDSA_NEON=0 -DFNDSA_SSE2=0 -DFNDSA_RV64D=0 \
        -DFNDSA_BENCH_REGIONS=1 -DNTESTS=$NTESTS"
OBJ_LIBS="codec.o mq.o sha3.o sysrng.o util.o \
          kgen.o kgen_fxp.o kgen_gauss.o kgen_mp31.o kgen_ntru.o kgen_poly.o kgen_zint31.o \
          sign.o sign_core.o sign_fpoly.o sign_fpr.o sign_sampler.o vrfy.o"

rm -f *.o bench_breakdown 2>/dev/null
make CFLAGS="$CFLAGS" $OBJ_LIBS > /dev/null
$CC $CFLAGS -c bench_regions.c   -o bench_regions.o
$CC $CFLAGS -c bench_breakdown.c -o bench_breakdown.o
$CC $CFLAGS -c speed_print.c     -o speed_print.o
$CC -o bench_breakdown bench_breakdown.o bench_regions.o speed_print.o $OBJ_LIBS -lm

./bench_breakdown
