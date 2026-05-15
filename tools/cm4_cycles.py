#!/usr/bin/env python3
"""cm4_cycles.py — static cycle estimator for hand-written Cortex-M4 asm.

Reads a GAS-style .s file, identifies functions via `label:` entries (and
.type ..., %function declarations), classifies each Thumb-2 instruction
against an M4 cycle table, and prints estimated cycles per function.

Cycle model (ARM DDI 0439B Cortex-M4 TRM, sections 3.3.1-3.3.4):
  ALU / shift / multiply / bit-field / DSP    1 cycle
  LDR / LDRB / LDRH / LDRSB / LDRSH / LDRD    2 cycles (load-use stalls not modeled)
  STR / STRB / STRH / STRD                    2 cycles
  LDM / LDMIA / POP                           1 + N (N = register count)
  STM / STMIA / PUSH                          1 + N
  Taken branches B/Bcc/BL/BX/BLX              2 cycles (range 2-3; average ~2)
  IT block guard                              1 cycle

Limitations (each contributes 5-10% inaccuracy on certain shapes):
  - Load-use stalls not modeled (M4 takes 1 extra cycle when LDR is
    immediately followed by an instruction using the loaded reg).
  - Back-to-back memory ops pipelining not modeled (M4 lowers second
    LDR/STR to 1 cycle when chained; we count both at 2).
  - Branches that fall through (not taken) cost 1 cycle, not 2; we
    assume taken (over-counts ~1 cycle per branch).

Net effect on hand-tuned constant-time fpr_* asm: ±5% vs silicon. Good
enough to rank optimization targets and reason about which primitive to
attack first. NOT good enough to publish absolute cycle counts.

Usage:
  ./cm4_cycles.py path/to/file.s [more.s ...]
  ./cm4_cycles.py --json path/to/file.s     # machine-readable
"""
import json
import os
import re
import sys
from collections import defaultdict

# ---------------------------------------------------------------------------
# Cycle table

ONE_CYCLE_OPCODES = {
    # Data processing — pure ALU
    "mov", "movs", "movw", "movt", "mvn", "mvns",
    "add", "adds", "addw", "adc", "adcs",
    "sub", "subs", "subw", "sbc", "sbcs",
    "neg", "negs", "rsb", "rsbs", "cmp", "cmn", "tst", "teq",
    "and", "ands", "orr", "orrs", "eor", "eors", "bic", "bics", "orn",
    # Shifts (also 1 cycle on M4)
    "lsl", "lsls", "lsr", "lsrs", "asr", "asrs", "ror", "rors", "rrx", "rrxs",
    # Count / bit-field
    "clz", "rbit", "rev", "rev16", "revsh",
    "sxtb", "sxth", "uxtb", "uxth", "sxtab", "sxtah", "uxtab", "uxtah",
    "sbfx", "ubfx", "bfi", "bfc",
    # Multiplies — all 1 cycle on M4 (the killer feature for crypto)
    "mul", "muls", "mla", "mls",
    "umull", "smull", "umlal", "smlal", "umaal",
    "smlabb", "smlabt", "smlatb", "smlatt",
    "smulbb", "smulbt", "smultb", "smultt",
    "smulwb", "smulwt", "smlawb", "smlawt",
    "smlad", "smladx", "smlsd", "smlsdx",
    "smlald", "smlaldx", "smlsld", "smlsldx",
    "smmla", "smmlar", "smmls", "smmlsr", "smmul", "smmulr",
    "smuad", "smuadx", "smusd", "smusdx",
    # DSP SIMD-style 16-bit dual-ops — also 1 cycle
    "uadd8", "sadd8", "uadd16", "sadd16",
    "usub8", "ssub8", "usub16", "ssub16",
    "uhadd8", "uhadd16", "shadd8", "shadd16",
    "uqadd8", "uqadd16", "uqsub8", "uqsub16",
    "uhsub8", "uhsub16", "shsub8", "shsub16",
    "usax", "ssax", "uasx", "sasx", "uqasx", "uqsax", "shasx", "shsax",
    "uhasx", "uhsax", "qasx", "qsax",
    "usad8", "usada8",
    "qadd", "qsub", "qdadd", "qdsub", "qadd8", "qsub8", "qadd16", "qsub16",
    "ssat", "usat", "ssat16", "usat16", "sel",
    "pkhbt", "pkhtb",
    # FP-reg moves used as integer scratch on M4F
    "vmov", "vmsr", "vmrs",
    # System / misc
    "nop", "yield", "wfi", "wfe", "sev",
    "isb", "dsb", "dmb", "msr", "mrs",
    # Table-branch fall-through cost (the branch itself we treat below)
    # IT block guards
    "it", "itt", "ite", "ittt", "itte", "itet", "itee",
    "itttt", "ittte", "ittet", "ittee", "itett", "itete", "iteet", "iteee",
}

TWO_CYCLE_OPCODES = {
    # Single loads
    "ldr", "ldrb", "ldrh", "ldrsb", "ldrsh", "ldrd",
    # Single stores
    "str", "strb", "strh", "strd",
    # FP loads/stores when used (single-precision regs on M4F)
    "vldr", "vstr",
    # Exclusive
    "ldrex", "ldrexb", "ldrexh", "strex", "strexb", "strexh",
    # Table-branch — 2 cycles best case
    "tbb", "tbh",
}

BRANCH_OPCODES = {"b", "bl", "bx", "blx", "cbz", "cbnz"}

LDM_OPCODES = {
    "ldm", "ldmia", "ldmib", "ldmda", "ldmdb",
    "ldmfd", "ldmed", "ldmfa", "ldmea",
    "pop",
}
STM_OPCODES = {
    "stm", "stmia", "stmib", "stmda", "stmdb",
    "stmfd", "stmed", "stmfa", "stmea",
    "push",
}

# Conditional execution suffixes. Stripped before classification when the
# stem matches an opcode in ONE_CYCLE_OPCODES (or similar). Order matters
# because longer suffixes like "ls" can be a prefix of "lsl" — we strip
# only as a last resort and verify the residue is a real opcode.
COND_SUFFIXES = (
    "eq", "ne", "cs", "hs", "cc", "lo", "mi", "pl", "vs", "vc",
    "hi", "ls", "ge", "lt", "gt", "le",
)

# Strip width hints (.w / .n) and FP type hints when matching opcodes.
WIDTH_SUFFIX_RE = re.compile(
    r"\.(w|n|s|f32|f64|i32|i64|u32|u64|s32|s8|s16|s64|u8|u16|p8|p16)$"
)


def classify(opcode):
    """Return (cycles_or_None, kind). None means LDM/STM — needs reglist."""
    op = opcode.lower()
    op = WIDTH_SUFFIX_RE.sub("", op)
    if op in ONE_CYCLE_OPCODES:
        return 1, "ALU"
    if op in TWO_CYCLE_OPCODES:
        return 2, "MEM"
    if op in LDM_OPCODES:
        return None, "LDM"
    if op in STM_OPCODES:
        return None, "STM"
    if op in BRANCH_OPCODES:
        return 2, "BRANCH"
    # Try stripping a conditional suffix (e.g. `addne`, `bls`)
    for c in COND_SUFFIXES:
        if op.endswith(c) and len(op) > len(c):
            base = op[: -len(c)]
            if base in ONE_CYCLE_OPCODES:
                return 1, "ALU"
            if base in TWO_CYCLE_OPCODES:
                return 2, "MEM"
            if base in LDM_OPCODES:
                return None, "LDM"
            if base in STM_OPCODES:
                return None, "STM"
            if base in BRANCH_OPCODES or base == "b":
                return 2, "BRANCH"
    return 1, "UNKNOWN"


def count_reglist(operand_str):
    m = re.search(r"\{([^}]+)\}", operand_str)
    if not m:
        return 1
    n = 0
    for part in m.group(1).split(","):
        part = part.strip()
        rng = re.match(r"r(\d+)\s*-\s*r(\d+)", part, re.IGNORECASE)
        if rng:
            n += int(rng.group(2)) - int(rng.group(1)) + 1
        else:
            n += 1
    return max(n, 1)


def analyze(path):
    """Parse one .s file, return {func_name: [(opcode, operands, cycles, kind), ...]}."""
    funcs = {}
    current = None
    closed = set()  # functions explicitly closed by .size

    with open(path) as f:
        for raw in f:
            line = raw.rstrip()
            if "@" in line:
                line = line.split("@", 1)[0]
            stripped = line.strip()
            if not stripped:
                continue

            # Function entry: bare label at start of line
            m = re.match(r"^([A-Za-z_][\w]*):\s*$", line)
            if m:
                current = m.group(1)
                if current in closed:
                    closed.discard(current)
                funcs.setdefault(current, [])
                continue

            # Directive
            if stripped.startswith("."):
                m = re.match(r"\.size\s+([A-Za-z_][\w]*)", stripped)
                if m:
                    closed.add(m.group(1))
                    if current == m.group(1):
                        current = None
                continue

            if current is None:
                continue

            parts = stripped.split(None, 1)
            opcode = parts[0]
            operands = parts[1] if len(parts) > 1 else ""
            cycles, kind = classify(opcode)
            if cycles is None:
                cycles = 1 + count_reglist(operands)
            funcs[current].append((opcode, operands, cycles, kind))

    return funcs


# Objdump output line shape (post-macro-expansion ground truth):
#   "       0:\tea80 72e0 \teor.w\tr2, r0, r0, asr #31"
# Function header shape:
#   "00000000 <fndsa_fpr_of32>:"
OBJDUMP_FUNC_RE = re.compile(r"^[0-9a-fA-F]+\s+<([^>]+)>:\s*$")
OBJDUMP_INSN_RE = re.compile(
    r"^\s*[0-9a-fA-F]+:\s+"        # address
    r"(?:[0-9a-fA-F]{2,4}\s+)+"    # one or more hex byte groups
    r"\s*([a-zA-Z][a-zA-Z0-9.]*)"  # opcode
    r"(?:\s+(.*))?$"               # optional operands
)


def analyze_objdump(path):
    """Parse `arm-none-eabi-objdump -d` output. Macros are pre-expanded by as,
    so this gives ground-truth instruction-level counts."""
    funcs = {}
    current = None
    with open(path) as f:
        for raw in f:
            line = raw.rstrip()
            if not line.strip():
                continue
            m = OBJDUMP_FUNC_RE.match(line)
            if m:
                current = m.group(1)
                funcs.setdefault(current, [])
                continue
            if current is None:
                continue
            m = OBJDUMP_INSN_RE.match(line)
            if not m:
                continue
            opcode = m.group(1)
            operands = m.group(2) or ""
            # objdump emits `;` for trailing comments
            if ";" in operands:
                operands = operands.split(";", 1)[0].strip()
            cycles, kind = classify(opcode)
            if cycles is None:
                cycles = 1 + count_reglist(operands)
            funcs[current].append((opcode, operands, cycles, kind))
    return funcs


def main():
    args = sys.argv[1:]
    as_json = False
    objdump_mode = False
    if "--json" in args:
        as_json = True
        args = [a for a in args if a != "--json"]
    if "--objdump" in args:
        objdump_mode = True
        args = [a for a in args if a != "--objdump"]
    if not args:
        print("usage: cm4_cycles.py [--json] [--objdump] file [more ...]",
              file=sys.stderr)
        print("  Without --objdump: parse GAS .s source (misses macro expansions).",
              file=sys.stderr)
        print("  With --objdump:    parse `arm-none-eabi-objdump -d` output",
              file=sys.stderr)
        print("                     (ground truth, macros already expanded).",
              file=sys.stderr)
        return 2

    parser = analyze_objdump if objdump_mode else analyze
    all_funcs = {}
    for path in args:
        for name, ops in parser(path).items():
            if ops:
                all_funcs[name] = ops

    if as_json:
        out = {
            name: {
                "instrs": len(ops),
                "cycles": sum(o[2] for o in ops),
                "by_kind": {
                    k: sum(o[2] for o in ops if o[3].split("(")[0] == k)
                    for k in ("ALU", "MEM", "BRANCH", "LDM", "STM", "UNKNOWN")
                },
            }
            for name, ops in all_funcs.items()
        }
        print(json.dumps(out, indent=2))
        return 0

    print(f"{'function':<36} {'instrs':>8} {'cycles':>8} {'unknown':>8}")
    print(f"{'-'*36} {'-'*8} {'-'*8} {'-'*8}")
    ranked = sorted(all_funcs, key=lambda n: -sum(o[2] for o in all_funcs[n]))
    for name in ranked:
        ops = all_funcs[name]
        total = sum(o[2] for o in ops)
        unk = sum(1 for o in ops if o[3] == "UNKNOWN")
        print(f"{name:<36} {len(ops):>8} {total:>8} {unk:>8}")

    print()
    print(f"{'function':<24} {'ALU':>8} {'MEM':>8} {'BRANCH':>8} {'LDM/STM':>8} {'UNK':>8}")
    print(f"{'-'*24} {'-'*8} {'-'*8} {'-'*8} {'-'*8} {'-'*8}")
    for name in ranked[:20]:
        ops = all_funcs[name]
        kinds = defaultdict(int)
        for _, _, cycles, kind in ops:
            base = kind.split("(")[0]
            if base in ("LDM", "STM"):
                kinds["LDMSTM"] += cycles
            elif base in ("ALU", "MEM", "BRANCH", "UNKNOWN"):
                key = "UNK" if base == "UNKNOWN" else base
                kinds[key] += cycles
            else:
                kinds["ALU"] += cycles
        print(
            f"{name:<24} {kinds['ALU']:>8} {kinds['MEM']:>8} "
            f"{kinds['BRANCH']:>8} {kinds['LDMSTM']:>8} {kinds['UNK']:>8}"
        )

    return 0


if __name__ == "__main__":
    sys.exit(main())
