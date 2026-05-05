#!/bin/bash
# run_bench.sh — bench_full_chain across {NEON, scalar} × {baseline, LOW_RAM}.
#
# bench_full_chain itself uses cpucycles() over NTESTS=10000 individual sign
# operations and prints median + average + min + max in the
# pq-crystals/dilithium test_speed.c shape. This driver builds each config,
# invokes the binary once, and prints a side-by-side comparison table plus
# per-arch percent deltas computed from the medians.

set -e

CC="${CC:-clang}"
COMMON_CFLAGS_NEON="-W -Wextra -Wundef -Wshadow -O2"
COMMON_CFLAGS_SCALAR="-W -Wextra -Wundef -Wshadow -O2 -DFNDSA_NEON=0 -DFNDSA_SSE2=0 -DFNDSA_RV64D=0"
OBJ_LIBS="codec.o mq.o sha3.o sysrng.o util.o kgen.o kgen_fxp.o kgen_gauss.o kgen_mp31.o kgen_ntru.o kgen_poly.o kgen_zint31.o sign.o sign_core.o sign_fpoly.o sign_fpr.o sign_sampler.o vrfy.o"

build() {
    local cflags="$1"
    rm -f *.o bench_full_chain 2>/dev/null
    make CFLAGS="$cflags" $OBJ_LIBS > /dev/null 2>&1
    $CC $cflags -c bench_full_chain.c -o bench_full_chain.o 2>/dev/null
    $CC $cflags -c speed_print.c     -o speed_print.o     2>/dev/null
    $CC -o bench_full_chain bench_full_chain.o speed_print.o $OBJ_LIBS -lm 2>/dev/null
}

# parse_field <bench_output> <logn> <field>
#   field ∈ {median, average, min, max}
parse_field() {
    local out="$1" logn="$2" field="$3"
    echo "$out" | awk -v L="$logn" -v F="$field" '
        $0 ~ "^sign logn="L":" {
            for (i = 1; i <= NF; i++) {
                if ($i == F":") {
                    v = $(i+1)
                    gsub(/,/, "", v)
                    print v
                    exit
                }
            }
        }'
}

parse_tmp() {
    local out="$1" logn="$2"
    echo "$out" | awk -v L="$logn" '$0 ~ "^tmp  logn="L":" {print $3}'
}

parse_unit() {
    local out="$1"
    echo "$out" | awk '/^# bench_full_chain/ {
        for (i = 1; i <= NF; i++) if ($i ~ /^unit=/) { sub("unit=", "", $i); print $i; exit }
    }'
}

run_config() {
    local label="$1" cflags="$2"
    echo "=== $label ===" >&2
    build "$cflags"
    ./bench_full_chain
}

echo "==> Building + running 4 configs (NTESTS=10000 each)"
echo

NEON_B=$(run_config "NEON, baseline"    "$COMMON_CFLAGS_NEON")
NEON_L=$(run_config "NEON, LOW_RAM"     "$COMMON_CFLAGS_NEON -DFNDSA_LOW_RAM=1")
SCAL_B=$(run_config "scalar, baseline"  "$COMMON_CFLAGS_SCALAR")
SCAL_L=$(run_config "scalar, LOW_RAM"   "$COMMON_CFLAGS_SCALAR -DFNDSA_LOW_RAM=1")

UNIT=$(parse_unit "$NEON_B")

# Pull the four headline numbers per config.
nb9_med=$(parse_field "$NEON_B" 9 median);  nb9_min=$(parse_field "$NEON_B" 9 min);  nb9_max=$(parse_field "$NEON_B" 9 max)
nb10_med=$(parse_field "$NEON_B" 10 median); nb10_min=$(parse_field "$NEON_B" 10 min); nb10_max=$(parse_field "$NEON_B" 10 max)
nl9_med=$(parse_field "$NEON_L" 9 median);  nl9_min=$(parse_field "$NEON_L" 9 min);  nl9_max=$(parse_field "$NEON_L" 9 max)
nl10_med=$(parse_field "$NEON_L" 10 median); nl10_min=$(parse_field "$NEON_L" 10 min); nl10_max=$(parse_field "$NEON_L" 10 max)
sb9_med=$(parse_field "$SCAL_B" 9 median);  sb9_min=$(parse_field "$SCAL_B" 9 min);  sb9_max=$(parse_field "$SCAL_B" 9 max)
sb10_med=$(parse_field "$SCAL_B" 10 median); sb10_min=$(parse_field "$SCAL_B" 10 min); sb10_max=$(parse_field "$SCAL_B" 10 max)
sl9_med=$(parse_field "$SCAL_L" 9 median);  sl9_min=$(parse_field "$SCAL_L" 9 min);  sl9_max=$(parse_field "$SCAL_L" 9 max)
sl10_med=$(parse_field "$SCAL_L" 10 median); sl10_min=$(parse_field "$SCAL_L" 10 min); sl10_max=$(parse_field "$SCAL_L" 10 max)

t9_b=$(parse_tmp "$NEON_B" 9);  t9_l=$(parse_tmp "$NEON_L" 9)
t10_b=$(parse_tmp "$NEON_B" 10); t10_l=$(parse_tmp "$NEON_L" 10)

echo
echo "=== sign cost per config (unit=$UNIT, NTESTS=10000) ==="
printf "%-26s median=%-10s  min=%-10s  max=%-10s\n" "NEON   baseline  logn=9"  "$nb9_med"  "$nb9_min"  "$nb9_max"
printf "%-26s median=%-10s  min=%-10s  max=%-10s\n" "NEON   baseline  logn=10" "$nb10_med" "$nb10_min" "$nb10_max"
printf "%-26s median=%-10s  min=%-10s  max=%-10s\n" "NEON   LOW_RAM   logn=9"  "$nl9_med"  "$nl9_min"  "$nl9_max"
printf "%-26s median=%-10s  min=%-10s  max=%-10s\n" "NEON   LOW_RAM   logn=10" "$nl10_med" "$nl10_min" "$nl10_max"
printf "%-26s median=%-10s  min=%-10s  max=%-10s\n" "scalar baseline  logn=9"  "$sb9_med"  "$sb9_min"  "$sb9_max"
printf "%-26s median=%-10s  min=%-10s  max=%-10s\n" "scalar baseline  logn=10" "$sb10_med" "$sb10_min" "$sb10_max"
printf "%-26s median=%-10s  min=%-10s  max=%-10s\n" "scalar LOW_RAM   logn=9"  "$sl9_med"  "$sl9_min"  "$sl9_max"
printf "%-26s median=%-10s  min=%-10s  max=%-10s\n" "scalar LOW_RAM   logn=10" "$sl10_med" "$sl10_min" "$sl10_max"

# Δ from medians (LOW_RAM / baseline − 1). Negative = LOW_RAM faster.
delta() {
    awk -v a="$1" -v b="$2" 'BEGIN { printf "%+.2f%%", (a/b - 1) * 100 }'
}

echo
echo "=== Δ_median (LOW_RAM vs baseline; negative = LOW_RAM faster) ==="
printf "NEON   logn=9 : %s\n" "$(delta "$nl9_med"  "$nb9_med")"
printf "NEON   logn=10: %s\n" "$(delta "$nl10_med" "$nb10_med")"
printf "scalar logn=9 : %s\n" "$(delta "$sl9_med"  "$sb9_med")"
printf "scalar logn=10: %s\n" "$(delta "$sl10_med" "$sb10_med")"

echo
echo "=== tmp[] sizes ==="
printf "logn=9 : baseline=%s  LOW_RAM=%s  (saved %d bytes)\n" "$t9_b"  "$t9_l"  "$((t9_b  - t9_l))"
printf "logn=10: baseline=%s  LOW_RAM=%s  (saved %d bytes)\n" "$t10_b" "$t10_l" "$((t10_b - t10_l))"
