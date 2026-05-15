#!/usr/bin/env python3
"""cm4_breakdown.py — project per-primitive M4 cycle attribution.

Combines two inputs:
  1. Per-routine M4 cycle counts from cm4_cycles.py (objdump mode for ground
     truth — macros pre-expanded by GAS).
  2. Per-primitive call counts from bench_breakdown's PRIM: lines (scalar
     build with FNDSA_BENCH_REGIONS=1).

Output: a per-primitive table showing call count × per-call M4 cycles =
projected M4 cost per sign, ranked by share. Plus a recommendation of
which primitive to attack first.

Calling convention adjustment: the scalar build expands FPR_ADD_SUB(a,b,x,y)
to fpr_add + fpr_sub (= 2 leaf adds in the fpr_add counter), but the M4 has
one fused 86-cycle fndsa_fpr_add_sub routine. We subtract 2*add_sub from the
raw add count to recover standalone adds before pricing.

Usage:
  ./cm4_breakdown.py                 # auto: assemble + run bench
  ./cm4_breakdown.py --bench-out F   # use a saved bench output file
  ./cm4_breakdown.py --logn 10       # report on a specific logn
"""
import argparse
import json
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CM4_CYCLES = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                          "cm4_cycles.py")

# Map bench primitive names to the public M4 routine symbols whose cycle
# counts cm4_cycles.py reports. fpr_scaled is the scalar of32-style builder
# used at the FFT/iFFT entry/exit.
PRIM_TO_ROUTINE = {
    "fpr_add":     "fndsa_fpr_add",
    "fpr_add_sub": "fndsa_fpr_add_sub",
    "fpr_mul":     "fndsa_fpr_mul",
    "fpr_div":     "fndsa_fpr_div",
    "fpr_sqrt":    "fndsa_fpr_sqrt",
    "fpr_scaled":  "fndsa_fpr_scaled",
}

PRIM_LINE_RE = re.compile(
    r"^PRIM:(\d+):([A-Za-z_][\w]*):(\d+):(\d+)\s*$"
)

# Region table line shape:
#   "fpoly_FFT             247849772   17.83%          -         5000  ..."
REGION_LINE_RE = re.compile(
    r"^(fpoly_FFT|fpoly_iFFT|ffsamp_fft|sampler_next|Other)\s+"
    r"(\d+)\s+(\d+\.\d+)%"
)

# "=== Region breakdown logn=9 ..." marks the start of a logn block in
# bench output; PRIM lines and region lines below it belong to that logn.
LOGN_HDR_RE = re.compile(r"=== Region breakdown logn=(\d+)")

# Plausibility of an aggressive optimization pass per primitive. These
# encode "30% gain feels reachable on a hand-tuned add routine; only 10%
# on the already-unrolled iterative div/sqrt." Tune as new data arrives —
# the leverage ranking is sensitive to these.
PLAUSIBLE_GAIN = {
    "fpr_add":     0.25,
    "fpr_add_sub": 0.20,
    "fpr_mul":     0.20,
    "fpr_div":     0.10,
    "fpr_sqrt":    0.10,
    "fpr_scaled":  0.10,
}


def assemble_and_dump(asm_paths, outdir):
    """Assemble GAS .s files for cortex-m4+fpv4-sp-d16, then objdump each."""
    dump_paths = []
    for src in asm_paths:
        obj = os.path.join(outdir, os.path.basename(src) + ".o")
        dump = os.path.join(outdir, os.path.basename(src) + ".dump")
        subprocess.run(
            ["arm-none-eabi-as",
             "-mcpu=cortex-m4", "-mthumb",
             "-mfpu=fpv4-sp-d16", "-mfloat-abi=softfp",
             src, "-o", obj],
            check=True,
        )
        with open(dump, "w") as f:
            subprocess.run(
                ["arm-none-eabi-objdump", "-d", obj],
                check=True, stdout=f,
            )
        dump_paths.append(dump)
    return dump_paths


def get_m4_cycle_table(asm_paths):
    """Return {routine_name: cycles} via cm4_cycles.py --objdump --json."""
    with tempfile.TemporaryDirectory() as tmpdir:
        dumps = assemble_and_dump(asm_paths, tmpdir)
        out = subprocess.check_output(
            [sys.executable, CM4_CYCLES, "--objdump", "--json", *dumps],
            text=True,
        )
    j = json.loads(out)
    return {name: info["cycles"] for name, info in j.items()}


def run_bench_breakdown(bench_path):
    """Run ./bench_breakdown and return its stdout. The binary must already
    be built with FNDSA_BENCH_REGIONS=1 (use ./run_breakdown.sh)."""
    out = subprocess.check_output([bench_path], text=True, cwd=ROOT)
    return out


def parse_prim_lines(text):
    """Return {logn: {prim_name: total_calls}}."""
    out = {}
    for line in text.splitlines():
        m = PRIM_LINE_RE.match(line)
        if not m:
            continue
        logn = int(m.group(1))
        prim = m.group(2)
        total = int(m.group(3))
        out.setdefault(logn, {})[prim] = total
    return out


def parse_region_shares(text):
    """Walk the bench output and extract per-logn region % shares.

    Returns {logn: {region_name: pct_of_total}}. The fpr-dominated fraction
    is FFT + iFFT + (FFSAMP - SAMPLER), used to scale fpr-only M4 cycles
    into an e2e estimate.
    """
    out = {}
    current = None
    for line in text.splitlines():
        m = LOGN_HDR_RE.search(line)
        if m:
            current = int(m.group(1))
            out.setdefault(current, {})
            continue
        if current is None:
            continue
        m = REGION_LINE_RE.match(line)
        if m:
            out[current][m.group(1)] = float(m.group(3))
    return out


def fpr_share_of_e2e(region_pct):
    """Fraction of total cycles spent in fpr-dominated regions.
    Defined as FFT + iFFT + (FFSAMP - SAMPLER); sampler_next is excluded
    because its body is SHAKE + ber_exp + table lookups, not fpr_*.
    Returned as a ratio in [0, 1]. Returns None if data is missing."""
    needed = ("fpoly_FFT", "fpoly_iFFT", "ffsamp_fft", "sampler_next")
    if not all(k in region_pct for k in needed):
        return None
    fft     = region_pct["fpoly_FFT"]
    ifft    = region_pct["fpoly_iFFT"]
    ffsamp  = region_pct["ffsamp_fft"]
    sampler = region_pct["sampler_next"]
    return (fft + ifft + (ffsamp - sampler)) / 100.0


def project_one(counts_per_sign, cycles_per_call):
    """Compose the M4 cost model.

    counts_per_sign: dict prim_name → calls per sign (already divided by NTESTS)
    cycles_per_call: dict prim_name → cycles per call on M4

    Returns a list of (prim_name, calls_used, cycles_each, total_cycles)
    rows, with the standalone-add adjustment applied:
       standalone_add = raw_add - 2*add_sub
    so add_sub events are priced once at the fused-routine cost (86 cycles)
    instead of twice at the leaf add cost (2*69 = 138 cycles).
    """
    raw_add = counts_per_sign.get("fpr_add", 0)
    add_sub = counts_per_sign.get("fpr_add_sub", 0)
    standalone_add = max(raw_add - 2 * add_sub, 0)

    rows = []
    # Order matches reading order; rank later for display.
    for prim in ("fpr_add", "fpr_add_sub", "fpr_mul",
                 "fpr_div", "fpr_sqrt", "fpr_scaled"):
        cyc = cycles_per_call.get(prim, 0)
        if prim == "fpr_add":
            calls = standalone_add
        else:
            calls = counts_per_sign.get(prim, 0)
        rows.append((prim, calls, cyc, calls * cyc))
    return rows


def recommend_target(rows, fpr_total, e2e_total):
    """Pick the highest-leverage primitive using share × plausible-gain.

    Rows: list of (prim_name, calls_per_sign, cycles_per_call, total_cycles).
    fpr_total: sum across rows (fpr-only M4 cycles per sign).
    e2e_total: estimated end-to-end M4 cycles per sign (None if unknown).

    Returns a multi-line string ranking each primitive by expected-cycles-
    saved if its plausible-gain were realized. The headline (first line)
    names the winner."""
    scored = []
    for prim, _calls, _cyc, tot in rows:
        gain = PLAUSIBLE_GAIN.get(prim, 0.10)
        saved = tot * gain
        scored.append((prim, tot, gain, saved))
    scored.sort(key=lambda r: -r[3])

    winner_prim, winner_tot, winner_gain, winner_saved = scored[0]
    fpr_pct = 100.0 * winner_tot / fpr_total if fpr_total else 0.0
    moved_fpr_pct = 100.0 * winner_saved / fpr_total if fpr_total else 0.0
    e2e_part = ""
    if e2e_total:
        moved_e2e_pct = 100.0 * winner_saved / e2e_total
        e2e_part = f", ~{moved_e2e_pct:.1f}% of e2e"

    head = (f"{winner_prim}: owns {fpr_pct:.0f}% of fpr cost; "
            f"a {int(winner_gain*100)}% optimization there saves "
            f"~{winner_saved/1e3:.0f}K cycles/sign "
            f"(~{moved_fpr_pct:.1f}% of fpr{e2e_part})")

    lines = [head, "", "Full leverage ranking (share × plausible gain):"]
    for prim, tot, gain, saved in scored:
        share = 100.0 * tot / fpr_total if fpr_total else 0.0
        moved = 100.0 * saved / fpr_total if fpr_total else 0.0
        lines.append(
            f"  {prim:<14} share={share:5.1f}%  "
            f"plausible_gain={int(gain*100):>2}%  "
            f"=> saves {saved/1e3:>6.0f}K cyc ({moved:4.1f}% of fpr)"
        )
    return "\n".join(lines)


def print_report(logn, counts_per_sign, cycles_per_call, fpr_share):
    rows = project_one(counts_per_sign, cycles_per_call)
    fpr_total = sum(r[3] for r in rows)
    rows_sorted = sorted(rows, key=lambda r: -r[3])

    e2e_total = int(fpr_total / fpr_share) if fpr_share else None

    print(f"\n=== M4-projected fpr_* breakdown (logn={logn}) ===")
    if e2e_total:
        print(f"  fpr-region share of measured cycles : {fpr_share*100:.1f}%")
        print(f"  implied e2e M4 cycles per sign      : ~{e2e_total/1e6:.2f} M")
        print(f"  (Pornin's published M4 sign for FN-DSA-512: ~17–25 M)")
    print(f"\n{'primitive':<14} {'calls/sign':>12} {'cyc/call':>9} "
          f"{'M4 cyc/sign':>14} {'% fpr':>8} {'% e2e':>8}")
    print(f"{'-'*14} {'-'*12} {'-'*9} {'-'*14} {'-'*8} {'-'*8}")
    for prim, calls, cyc, tot in rows_sorted:
        pct_fpr = 100.0 * tot / fpr_total if fpr_total else 0.0
        pct_e2e = 100.0 * tot / e2e_total if e2e_total else 0.0
        e2e_cell = f"{pct_e2e:>7.2f}%" if e2e_total else "    -   "
        print(f"{prim:<14} {calls:>12} {cyc:>9} {tot:>14} "
              f"{pct_fpr:>7.2f}% {e2e_cell}")
    print(f"{'-'*14} {'-'*12} {'-'*9} {'-'*14} {'-'*8} {'-'*8}")
    fpr_e2e_pct = 100.0 * fpr_total / e2e_total if e2e_total else 0.0
    e2e_cell = f"{fpr_e2e_pct:>7.2f}%" if e2e_total else "    -   "
    print(f"{'TOTAL fpr':<14} {'':<12} {'':<9} {fpr_total:>14} "
          f"{'100.00%':>8} {e2e_cell}")

    print()
    print(recommend_target(rows_sorted, fpr_total, e2e_total))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bench-out", help="Use bench output from this file"
                    " instead of running ./bench_breakdown.")
    ap.add_argument("--logn", type=int, default=None,
                    help="Only report this logn (default: all).")
    args = ap.parse_args()

    asm_paths = [
        os.path.join(ROOT, "sign_fpr_cm4.s"),
        os.path.join(ROOT, "sign_sampler_cm4.s"),
    ]
    cycles_table = get_m4_cycle_table(asm_paths)
    cycles_per_call = {
        prim: cycles_table[routine]
        for prim, routine in PRIM_TO_ROUTINE.items()
        if routine in cycles_table
    }

    if args.bench_out:
        with open(args.bench_out) as f:
            bench_text = f.read()
    else:
        bench_text = run_bench_breakdown(os.path.join(ROOT, "bench_breakdown"))

    by_logn = parse_prim_lines(bench_text)
    region_by_logn = parse_region_shares(bench_text)
    if not by_logn:
        print("ERROR: no PRIM: lines found in bench output. Did you rebuild "
              "with the new instrumentation? Run ./run_breakdown.sh first.",
              file=sys.stderr)
        return 2

    print("=== M4 per-call cycle table (from objdump of cm4 asm) ===")
    for prim in ("fpr_add", "fpr_add_sub", "fpr_mul",
                 "fpr_div", "fpr_sqrt", "fpr_scaled"):
        cyc = cycles_per_call.get(prim, "??")
        print(f"  {prim:<14} {cyc} cycles")

    targets = sorted(by_logn) if args.logn is None else [args.logn]
    for logn in targets:
        if logn not in by_logn:
            print(f"WARNING: no PRIM: data for logn={logn}", file=sys.stderr)
            continue
        # The PRIM line emits per-sign and per-total; the parser took the
        # total. Convert to per-sign by reading the per-sign field in raw
        # output, but easier to just divide by NTESTS = total/per_sign ratio
        # — for now we assume the bench's per-sign is what we want, which
        # equals total // NTESTS. Re-emit per-sign by parsing the 4th group.
        # Simpler: re-parse with a per-sign view.
        per_sign = {}
        for line in bench_text.splitlines():
            m = PRIM_LINE_RE.match(line)
            if not m or int(m.group(1)) != logn:
                continue
            per_sign[m.group(2)] = int(m.group(4))
        fpr_share = fpr_share_of_e2e(region_by_logn.get(logn, {}))
        print_report(logn, per_sign, cycles_per_call, fpr_share)
    return 0


if __name__ == "__main__":
    sys.exit(main())
