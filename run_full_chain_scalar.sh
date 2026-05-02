#!/bin/bash
# Build and run bench_full_chain under all 4 configurations, print a
# comparison table.
#
# Output:
#   logn=9 baseline=213487 PATH_B=215000 +PHASE1=205000 +PathA=205500
#   ... with deltas vs baseline expressed as percentages.

set -e

CC="${CC:-clang}"
COMMON_CFLAGS="-W -Wextra -Wundef -Wshadow -O2 -DFNDSA_NEON=0 -DFNDSA_SSE2=0 -DFNDSA_RV64D=0"
COMMON_OBJ_LIBS="codec.o mq.o sha3.o sysrng.o util.o kgen.o kgen_fxp.o kgen_gauss.o kgen_mp31.o kgen_ntru.o kgen_poly.o kgen_zint31.o sign.o sign_core.o sign_fpoly.o sign_fpr.o sign_sampler.o vrfy.o"

# Helper: build the library + bench under a given flag set, run it.
# Args: name, extra_cflags
run_one() {
    local name="$1"
    local flags="$2"
    echo "=== $name ===" >&2
    rm -f *.o bench_full_chain 2>/dev/null
    make CFLAGS="$COMMON_CFLAGS $flags" \
        $COMMON_OBJ_LIBS 2>&1 | grep -E "error:|warning:" || true
    $CC $COMMON_CFLAGS $flags -c bench_full_chain.c -o bench_full_chain.o
    $CC -o bench_full_chain bench_full_chain.o $COMMON_OBJ_LIBS -lm
    ./bench_full_chain
}

# Run all four configurations. Each prints lines: <logn> <ns> <iters> <tmp_len>
out_baseline=$(run_one "baseline (Pornin main)" "")
out_pathb=$(run_one "+ PATH_B" "-DFNDSA_PATH_B=1")
out_phase1=$(run_one "+ PATH_B + PHASE1_REDUCED" "-DFNDSA_PATH_B=1 -DFNDSA_PHASE1_REDUCED=1")
out_patha=$(run_one "+ PATH_B + PHASE1 + FFSAMP_5N (Path A)" "-DFNDSA_PATH_B=1 -DFNDSA_PHASE1_REDUCED=1 -DFNDSA_FFSAMP_5N_REDUCED=1")

# Parse: extract per-logn ns from each output
get_ns() {
    local logn="$1"
    local data="$2"
    echo "$data" | awk -v ln="$logn" '$1 == ln { print $2 }'
}

get_tmp() {
    local logn="$1"
    local data="$2"
    echo "$data" | awk -v ln="$logn" '$1 == ln { print $4 }'
}

echo ""
echo "=== Comparison table (host wall-clock) ==="
echo ""
printf "%-40s | %-14s | %-14s | %-12s | %-12s\n" \
    "Configuration" "logn=9 (ns)" "logn=10 (ns)" "tmp_len@9" "tmp_len@10"
printf -- '-%.0s' {1..114}; echo ""

for level in 9 10; do
    base_ns=$(get_ns "$level" "$out_baseline")
done

for cfg in "baseline:$out_baseline" "PATH_B:$out_pathb" "+PHASE1_REDUCED:$out_phase1" "+FFSAMP_5N (Path A):$out_patha"; do
    name="${cfg%%:*}"
    data="${cfg#*:}"
    ns9=$(get_ns 9 "$data")
    ns10=$(get_ns 10 "$data")
    tmp9=$(get_tmp 9 "$data")
    tmp10=$(get_tmp 10 "$data")
    printf "%-40s | %-14s | %-14s | %-12s | %-12s\n" \
        "$name" "$ns9" "$ns10" "$tmp9" "$tmp10"
done
echo ""

echo "=== Delta vs baseline (positive = slower) ==="
echo ""
base9=$(get_ns 9 "$out_baseline")
base10=$(get_ns 10 "$out_baseline")
printf "%-40s | %-14s | %-14s\n" "Configuration" "logn=9 Δ" "logn=10 Δ"
printf -- '-%.0s' {1..76}; echo ""

for cfg in "PATH_B:$out_pathb" "+PHASE1_REDUCED:$out_phase1" "+FFSAMP_5N (Path A):$out_patha"; do
    name="${cfg%%:*}"
    data="${cfg#*:}"
    ns9=$(get_ns 9 "$data")
    ns10=$(get_ns 10 "$data")
    delta9=$(awk -v a="$ns9" -v b="$base9" 'BEGIN{printf "%+.2f%%", (a/b-1)*100}')
    delta10=$(awk -v a="$ns10" -v b="$base10" 'BEGIN{printf "%+.2f%%", (a/b-1)*100}')
    printf "%-40s | %-14s | %-14s\n" "$name" "$delta9" "$delta10"
done
echo ""
