#!/usr/bin/env python3

import argparse
import csv
from pathlib import Path
import random

import numpy as np


Z_CANDIDATES = np.array([101, 211, 307, 419], dtype=np.uint32)


def variant_order(variant):
    if variant == "clear":
        return 0
    if variant.startswith("masked_d"):
        return int(variant.removeprefix("masked_d"))
    raise ValueError(f"unknown variant: {variant}")


def accept_vector(index, mode, rng):
    if mode == "fixed":
        return np.array([1, 1, 1, 1], dtype=np.uint32)
    if mode == "random-balanced":
        pattern = index & 0xF
        return np.array([(pattern >> lane) & 1 for lane in range(4)],
                        dtype=np.uint32)
    if mode == "random":
        return np.array([rng.randrange(2) for _ in range(4)],
                        dtype=np.uint32)
    raise ValueError(f"unknown accept mode: {mode}")


def make_vectors(variant, ntraces, accept_mode, seed):
    order = variant_order(variant)
    rng = random.Random(seed)

    labels = np.array([i & 3 for i in range(ntraces)], dtype=np.uint8)
    rng.shuffle(labels)

    shares = np.zeros((ntraces, order + 1), dtype=np.uint8)
    z0_idx = np.zeros(ntraces, dtype=np.uint8)
    berexp = np.zeros((ntraces, 4), dtype=np.uint32)
    z_candidates = np.tile(Z_CANDIDATES, (ntraces, 1))
    rng_seed = np.zeros(ntraces, dtype=np.uint32)
    expected = np.zeros(ntraces, dtype=np.uint32)

    for trace_index, label in enumerate(labels):
        label = int(label)
        z0_idx[trace_index] = label
        if order > 0:
            acc = label
            for share_index in range(1, order + 1):
                share = rng.randrange(4)
                shares[trace_index, share_index] = share
                acc ^= share
            shares[trace_index, 0] = acc
        else:
            shares[trace_index, 0] = label

        berexp[trace_index] = accept_vector(trace_index, accept_mode, rng)
        rng_seed[trace_index] = rng.randrange(1, 2**32)
        if berexp[trace_index, label] != 0:
            expected[trace_index] = z_candidates[trace_index, label]

    return {
        "variant": np.array(variant),
        "accept_mode": np.array(accept_mode),
        "labels": labels,
        "z0_idx": z0_idx,
        "shares": shares,
        "berexp_results": berexp,
        "z_candidates": z_candidates,
        "rng_seed": rng_seed,
        "expected": expected,
    }


def write_csv(path, vectors):
    with path.open("w", newline="") as f:
        writer = csv.writer(f)
        nshares = vectors["shares"].shape[1]
        header = (
            ["trace", "label", "z0_idx"]
            + [f"z_sh_{i}" for i in range(nshares)]
            + [f"berexp_{i}" for i in range(4)]
            + [f"z_candidate_{i}" for i in range(4)]
            + ["rng_seed", "expected"]
        )
        writer.writerow(header)
        for i, label in enumerate(vectors["labels"]):
            writer.writerow(
                [i, int(label), int(vectors["z0_idx"][i])]
                + [int(x) for x in vectors["shares"][i]]
                + [int(x) for x in vectors["berexp_results"][i]]
                + [int(x) for x in vectors["z_candidates"][i]]
                + [int(vectors["rng_seed"][i]), int(vectors["expected"][i])]
            )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--variant", required=True,
                        choices=["clear", "masked_d1", "masked_d2",
                                 "masked_d3", "masked_d4"])
    parser.add_argument("--ntraces", type=int, default=10000)
    parser.add_argument("--accept-mode", default="fixed",
                        choices=["fixed", "random", "random-balanced"])
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--out", required=True)
    parser.add_argument("--csv", default=None)
    args = parser.parse_args()

    vectors = make_vectors(args.variant, args.ntraces, args.accept_mode,
                           args.seed)
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    np.savez_compressed(out, **vectors)
    if args.csv is not None:
        write_csv(Path(args.csv), vectors)

    counts = np.bincount(vectors["labels"], minlength=4)
    print(f"wrote {out}")
    print(f"variant={args.variant} accept_mode={args.accept_mode}")
    print("label counts:", " ".join(str(int(x)) for x in counts))


if __name__ == "__main__":
    main()
