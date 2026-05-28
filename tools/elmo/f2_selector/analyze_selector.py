#!/usr/bin/env python3

import argparse
from pathlib import Path

import numpy as np


LABEL_PERM = np.array([
    [0, 1, 2, 3], [0, 1, 3, 2], [0, 2, 1, 3], [0, 2, 3, 1],
    [0, 3, 1, 2], [0, 3, 2, 1], [1, 0, 2, 3], [1, 0, 3, 2],
    [1, 2, 0, 3], [1, 2, 3, 0], [1, 3, 0, 2], [1, 3, 2, 0],
    [2, 0, 1, 3], [2, 0, 3, 1], [2, 1, 0, 3], [2, 1, 3, 0],
    [2, 3, 0, 1], [2, 3, 1, 0], [3, 0, 1, 2], [3, 0, 2, 1],
    [3, 1, 0, 2], [3, 1, 2, 0], [3, 2, 0, 1], [3, 2, 1, 0],
], dtype=np.uint8)


def scheduled_labels(n, mode):
    if mode == "sequential":
        return np.arange(n, dtype=np.uint8) & 3
    if mode == "block-perm":
        labels = np.empty(n, dtype=np.uint8)
        for i in range(n):
            block = i >> 2
            labels[i] = LABEL_PERM[block % 24, i & 3]
        return labels
    raise ValueError(f"unknown label mode: {mode}")


def load_npz_or_npy(path, label_mode):
    path = Path(path)
    if path.is_dir():
        files = sorted(path.glob("trace*.trc"))
        if not files:
            raise SystemExit(f"no trace*.trc files in {path}")
        traces = []
        for trace_file in files:
            traces.append(np.loadtxt(trace_file, dtype=np.float64))
        min_len = min(trace.shape[0] for trace in traces)
        traces = np.stack([trace[:min_len] for trace in traces])
        labels = scheduled_labels(traces.shape[0], label_mode)
        return traces, labels
    if path.suffix == ".npz":
        with np.load(path, allow_pickle=False) as data:
            if "traces" in data:
                traces = np.asarray(data["traces"], dtype=np.float64)
            else:
                traces = np.asarray(data[list(data.keys())[0]], dtype=np.float64)
            labels = np.asarray(data["labels"], dtype=np.uint8)
        return traces, labels
    traces = np.asarray(np.load(path), dtype=np.float64)
    return traces, None


def load_labels(path):
    if path is None:
        return None
    return np.asarray(np.load(path), dtype=np.uint8)


def balanced_indices(labels, rng):
    per_label = [np.flatnonzero(labels == label) for label in range(4)]
    n = min(len(indices) for indices in per_label)
    if n == 0:
        raise SystemExit("all four labels must be present")
    out = []
    for indices in per_label:
        picked = indices.copy()
        rng.shuffle(picked)
        out.append(picked[:n])
    out = np.concatenate(out)
    rng.shuffle(out)
    return out


def split_train_test(traces, labels, test_fraction, seed):
    rng = np.random.default_rng(seed)
    indices = balanced_indices(labels, rng)
    traces = traces[indices]
    labels = labels[indices]
    n_test = max(4, int(len(indices) * test_fraction))
    n_test -= n_test % 4
    return traces[n_test:], labels[n_test:], traces[:n_test], labels[:n_test]


def standardize(train, test):
    mean = train.mean(axis=0)
    std = train.std(axis=0)
    std[std == 0] = 1.0
    return (train - mean) / std, (test - mean) / std


def nearest_centroid_accuracy(train_x, train_y, test_x, test_y):
    centroids = np.stack([train_x[train_y == label].mean(axis=0)
                          for label in range(4)])
    dists = ((test_x[:, None, :] - centroids[None, :, :]) ** 2).sum(axis=2)
    pred = np.argmin(dists, axis=1).astype(np.uint8)
    return float((pred == test_y).mean()), pred


def confusion_mi_bits(true_labels, pred_labels):
    n = len(true_labels)
    mi = 0.0
    for y in range(4):
        py = np.count_nonzero(true_labels == y) / n
        if py == 0.0:
            continue
        for p in range(4):
            joint = np.count_nonzero((true_labels == y) & (pred_labels == p)) / n
            if joint == 0.0:
                continue
            pp = np.count_nonzero(pred_labels == p) / n
            mi += joint * np.log2(joint / (py * pp))
    return float(mi)


def feature_scores(train_x, train_y):
    overall = train_x.mean(axis=0)
    between = np.zeros(train_x.shape[1])
    within = np.zeros(train_x.shape[1])
    for label in range(4):
        group = train_x[train_y == label]
        if group.shape[0] == 0:
            continue
        delta = group.mean(axis=0) - overall
        between += group.shape[0] * delta * delta
        within += ((group - group.mean(axis=0)) ** 2).sum(axis=0)
    with np.errstate(divide="ignore", invalid="ignore"):
        scores = between / np.maximum(within, 1e-12)
    return np.where(np.isfinite(scores), scores, 0.0)


def parse_dimensions(value, max_dim):
    if value is None:
        return []
    dims = []
    for part in value.split(","):
        part = part.strip()
        if not part:
            continue
        dim = int(part)
        if dim <= 0:
            raise SystemExit("--dimension-curve values must be positive")
        dims.append(min(dim, max_dim))
    return sorted(set(dims))


def dimension_curve(train_x, train_y, test_x, test_y, dims):
    order = np.argsort(feature_scores(train_x, train_y))[::-1]
    rows = []
    for dim in dims:
        chosen = order[:dim]
        acc, pred = nearest_centroid_accuracy(
            train_x[:, chosen], train_y, test_x[:, chosen], test_y)
        rows.append((dim, acc, confusion_mi_bits(test_y, pred)))
    return rows


def accuracy_ci(acc, n, z=1.96):
    half = z * np.sqrt((acc * (1.0 - acc)) / max(n, 1))
    return max(0.0, acc - half), min(1.0, acc + half)


def mutual_information_discrete(values, labels, bins):
    edges = np.quantile(values, np.linspace(0.0, 1.0, bins + 1))
    edges = np.unique(edges)
    if len(edges) <= 2:
        return 0.0
    bucket = np.searchsorted(edges[1:-1], values, side="right")
    n = len(labels)
    mi = 0.0
    for y in range(4):
        py = np.count_nonzero(labels == y) / n
        if py == 0:
            continue
        for b in range(len(edges) - 1):
            joint = np.count_nonzero((labels == y) & (bucket == b)) / n
            if joint == 0:
                continue
            px = np.count_nonzero(bucket == b) / n
            mi += joint * np.log2(joint / (px * py))
    return float(mi)


def mi_summary(traces, labels, bins, max_samples):
    if traces.shape[1] > max_samples:
        step = int(np.ceil(traces.shape[1] / max_samples))
        traces = traces[:, ::step]
    values = [mutual_information_discrete(traces[:, i], labels, bins)
              for i in range(traces.shape[1])]
    values = np.asarray(values)
    return float(values.max()), float(values.mean())


def welch_t(a, b):
    ma = a.mean(axis=0)
    mb = b.mean(axis=0)
    va = a.var(axis=0, ddof=1)
    vb = b.var(axis=0, ddof=1)
    se = np.sqrt(va / a.shape[0] + vb / b.shape[0])
    with np.errstate(divide="ignore", invalid="ignore"):
        t = (ma - mb) / se
    return np.where(np.isfinite(t), t, 0.0)


def max_pairwise_tvla(traces, labels):
    best = (0.0, None, None, 0)
    for a in range(4):
        for b in range(a + 1, 4):
            t = welch_t(traces[labels == a], traces[labels == b])
            idx = int(np.argmax(np.abs(t)))
            value = float(abs(t[idx]))
            if value > best[0]:
                best = (value, a, b, idx)
    return best


def centered_square_tvla(traces, labels):
    centered = traces - traces.mean(axis=0)
    return max_pairwise_tvla(centered * centered, labels)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--traces", required=True,
                        help=".npz with traces+labels or .npy trace matrix")
    parser.add_argument("--labels", default=None,
                        help=".npy labels if --traces is a bare matrix")
    parser.add_argument("--label-mode", default="sequential",
                        choices=["sequential", "block-perm"])
    parser.add_argument("--test-fraction", type=float, default=0.25)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--mi-bins", type=int, default=16)
    parser.add_argument("--mi-max-samples", type=int, default=2000)
    parser.add_argument("--tvla-threshold", type=float, default=4.5)
    parser.add_argument("--second-order", action="store_true")
    parser.add_argument("--drop-prefix", type=int, default=0,
                        help="discard this many leading samples from each trace")
    parser.add_argument("--drop-suffix", type=int, default=0,
                        help="discard this many trailing samples from each trace")
    parser.add_argument("--permutation-baseline", type=int, default=0,
                        help="shuffle labels this many times and report MI bias")
    parser.add_argument("--dimension-curve", default=None,
                        help="comma-separated top-dimension counts for PI proxy")
    args = parser.parse_args()

    traces, embedded_labels = load_npz_or_npy(args.traces, args.label_mode)
    labels = embedded_labels if embedded_labels is not None else load_labels(args.labels)
    if labels is None:
        raise SystemExit("labels are required")
    if traces.shape[0] != labels.shape[0]:
        raise SystemExit("trace and label counts differ")
    if args.drop_prefix or args.drop_suffix:
        stop = traces.shape[1] - args.drop_suffix if args.drop_suffix else None
        traces = traces[:, args.drop_prefix:stop]
    if traces.shape[1] == 0:
        raise SystemExit("all trace samples were dropped")

    train_x, train_y, test_x, test_y = split_train_test(
        traces, labels, args.test_fraction, args.seed)
    train_x, test_x = standardize(train_x, test_x)
    acc, _ = nearest_centroid_accuracy(train_x, train_y, test_x, test_y)
    lo, hi = accuracy_ci(acc, len(test_y))
    mi_max, mi_mean = mi_summary(traces, labels, args.mi_bins,
                                args.mi_max_samples)
    tvla_max, tvla_a, tvla_b, tvla_idx = max_pairwise_tvla(traces, labels)

    print("=== selector leakage analysis ===")
    print(f"traces:              {traces.shape[0]} x {traces.shape[1]}")
    print(f"test traces:         {len(test_y)}")
    print(f"classifier accuracy: {100.0 * acc:.2f}% "
          f"(95% CI {100.0 * lo:.2f}..{100.0 * hi:.2f}%)")
    print(f"MI estimate:         max {mi_max:.5f} bits, "
          f"mean {mi_mean:.5f} bits")
    if args.permutation_baseline:
        rng = np.random.default_rng(args.seed)
        perm_max = []
        perm_mean = []
        for _ in range(args.permutation_baseline):
            shuffled = labels.copy()
            rng.shuffle(shuffled)
            shuffled_max, shuffled_mean = mi_summary(
                traces, shuffled, args.mi_bins, args.mi_max_samples)
            perm_max.append(shuffled_max)
            perm_mean.append(shuffled_mean)
        perm_max = np.asarray(perm_max)
        perm_mean = np.asarray(perm_mean)
        print(f"permuted-label MI:   max median {np.median(perm_max):.5f}, "
              f"p95 {np.quantile(perm_max, 0.95):.5f}, "
              f"max {perm_max.max():.5f} bits")
        print(f"permuted mean MI:    median {np.median(perm_mean):.5f}, "
              f"p95 {np.quantile(perm_mean, 0.95):.5f}, "
              f"max {perm_mean.max():.5f} bits")
    print(f"first-order TVLA:    max |t| {tvla_max:.3f} "
          f"labels {tvla_a}/{tvla_b} sample {tvla_idx}")
    print(f"TVLA threshold:      {args.tvla_threshold}")
    print("accuracy target:     ~25%; 26-27% is suspicious at 10k tests")

    if args.second_order:
        so_max, so_a, so_b, so_idx = centered_square_tvla(traces, labels)
        print(f"second-order proxy:  max |t| {so_max:.3f} "
              f"labels {so_a}/{so_b} sample {so_idx}")

    dims = parse_dimensions(args.dimension_curve, train_x.shape[1])
    if dims:
        print("dimension curve:     top_dims accuracy confusion_mi_bits")
        for dim, dim_acc, dim_mi in dimension_curve(
                train_x, train_y, test_x, test_y, dims):
            print(f"dimension curve:     {dim} {100.0 * dim_acc:.2f}% "
                  f"{dim_mi:.5f}")


if __name__ == "__main__":
    main()
