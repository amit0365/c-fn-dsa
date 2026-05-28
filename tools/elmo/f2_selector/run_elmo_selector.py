#!/usr/bin/env python3

import argparse
import ctypes
from pathlib import Path
import platform
import shutil
import subprocess
import sys

import numpy as np


HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
ARM_TOOLCHAIN = REPO / "external" / "arm-toolchain"
VARIANT_SOURCES = {
    "clear": "dispatch_clear.c",
    "masked_d1": "dispatch_masked_d1.c",
    "masked_d2": "dispatch_masked_d2.c",
    "masked_d3": "dispatch_masked_d3.c",
    "masked_d4": "dispatch_masked_d4.c",
}


def dylib_suffix():
    if platform.system() == "Darwin":
        return ".dylib"
    return ".so"


def run(cmd, cwd=None):
    print("+", " ".join(str(x) for x in cmd))
    subprocess.run(cmd, cwd=cwd, check=True)


def host_cc():
    return shutil.which("cc") or shutil.which("clang") or shutil.which("gcc")


def arm_gcc():
    matches = sorted(ARM_TOOLCHAIN.glob("*/bin/arm-none-eabi-gcc"))
    if matches:
        return str(matches[0])
    return shutil.which("arm-none-eabi-gcc")


def variant_order(variant):
    if variant == "clear":
        return 0
    return int(variant.removeprefix("masked_d"))


def compile_host(variant, build_dir, opt):
    cc = host_cc()
    if cc is None:
        raise SystemExit("no host C compiler found")
    source = HERE / VARIANT_SOURCES[variant]
    out = build_dir / f"{variant}{dylib_suffix()}"
    flags = [cc, opt, "-Wall", "-Wextra", "-std=c99", "-shared", "-fPIC",
             "-I", str(HERE), str(source), "-o", str(out)]
    if platform.system() == "Darwin":
        flags = [cc, opt, "-Wall", "-Wextra", "-std=c99", "-dynamiclib",
                 "-fPIC", "-I", str(HERE), str(source), "-o", str(out)]
    run(flags)
    return out


def compile_arm_asm(variant, build_dir, opt):
    cc = arm_gcc()
    if cc is None:
        raise SystemExit("arm-none-eabi-gcc not found")
    source = HERE / VARIANT_SOURCES[variant]
    out = build_dir / f"{variant}.s"
    run([
        cc, opt, "-Wall", "-Wextra", "-std=c99", "-S",
        "-mthumb", "-mcpu=cortex-m0", "-fno-common",
        "-fno-strict-aliasing", "-I", str(HERE), str(source), "-o", str(out),
    ])
    print(f"assembly: {out}")
    return out


def load_vectors(path):
    with np.load(path, allow_pickle=False) as data:
        return {key: data[key] for key in data.files}


def host_check(variant, lib_path, vectors, limit):
    lib = ctypes.CDLL(str(lib_path))
    n = min(limit, len(vectors["labels"]))
    failures = []

    if variant == "clear":
        fn = lib.dispatch_clear
        fn.argtypes = [
            ctypes.c_uint32,
            ctypes.POINTER(ctypes.c_uint32),
            ctypes.POINTER(ctypes.c_uint32),
        ]
    else:
        fn = getattr(lib, f"dispatch_{variant}")
        fn.argtypes = [
            ctypes.POINTER(ctypes.c_uint32),
            ctypes.POINTER(ctypes.c_uint32),
            ctypes.POINTER(ctypes.c_uint32),
            ctypes.c_uint32,
        ]
    fn.restype = ctypes.c_uint32

    for i in range(n):
        berexp = np.ascontiguousarray(vectors["berexp_results"][i],
                                      dtype=np.uint32)
        zcand = np.ascontiguousarray(vectors["z_candidates"][i],
                                     dtype=np.uint32)
        expected = int(vectors["expected"][i])
        if variant == "clear":
            got = fn(int(vectors["z0_idx"][i]),
                     berexp.ctypes.data_as(ctypes.POINTER(ctypes.c_uint32)),
                     zcand.ctypes.data_as(ctypes.POINTER(ctypes.c_uint32)))
        else:
            shares = np.ascontiguousarray(vectors["shares"][i],
                                          dtype=np.uint32)
            got = fn(shares.ctypes.data_as(ctypes.POINTER(ctypes.c_uint32)),
                     berexp.ctypes.data_as(ctypes.POINTER(ctypes.c_uint32)),
                     zcand.ctypes.data_as(ctypes.POINTER(ctypes.c_uint32)),
                     int(vectors["rng_seed"][i]))
        if int(got) != expected:
            failures.append((i, int(got), expected))
            if len(failures) >= 5:
                break

    if failures:
        for index, got, expected in failures:
            print(f"mismatch trace={index}: got={got} expected={expected}")
        raise SystemExit("host check failed")
    print(f"host check passed: {variant}, {n} vectors")


def run_elmo_binary(elmo_bin, target_bin, out_dir, ntrace):
    out_dir.mkdir(parents=True, exist_ok=True)
    cmd = [str(elmo_bin), str(target_bin)]
    if ntrace is not None:
        cmd += ["Ntrace", str(ntrace)]
    run(cmd, cwd=out_dir)
    print(f"ELMO output dir: {out_dir}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--variant", required=True, choices=sorted(VARIANT_SOURCES))
    parser.add_argument("--vectors", default=None)
    parser.add_argument("--build-dir", default=str(HERE / "build"))
    parser.add_argument("--opt", default="-O2")
    parser.add_argument("--host-check", action="store_true")
    parser.add_argument("--host-limit", type=int, default=10000)
    parser.add_argument("--emit-arm-asm", action="store_true")
    parser.add_argument("--elmo-bin", default=str(REPO / "external" / "ELMO" / "elmo"))
    parser.add_argument("--target-bin", default=None,
                        help="prebuilt Thumb binary to pass to ELMO")
    parser.add_argument("--elmo-out", default=str(HERE / "elmo_out"))
    parser.add_argument("--ntrace", type=int, default=None)
    parser.add_argument("--analyze-traces", default=None,
                        help=".npz/.npy trace matrix for analyze_selector.py")
    args = parser.parse_args()

    build_dir = Path(args.build_dir)
    build_dir.mkdir(parents=True, exist_ok=True)

    if args.host_check:
        if args.vectors is None:
            raise SystemExit("--host-check requires --vectors")
        lib_path = compile_host(args.variant, build_dir, args.opt)
        host_check(args.variant, lib_path, load_vectors(Path(args.vectors)),
                   args.host_limit)

    if args.emit_arm_asm:
        compile_arm_asm(args.variant, build_dir, args.opt)

    if args.target_bin is not None:
        run_elmo_binary(Path(args.elmo_bin), Path(args.target_bin),
                        Path(args.elmo_out), args.ntrace)

    if args.analyze_traces is not None:
        run([sys.executable, str(HERE / "analyze_selector.py"),
             "--traces", args.analyze_traces])


if __name__ == "__main__":
    main()
