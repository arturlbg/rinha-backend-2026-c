#!/usr/bin/env python3
"""Offline Random Forest distillation study for kdclass3.

This script is intentionally offline-only. It trains small deterministic
forests on vector-derived features and evaluates a hybrid gate:

    RF confident -> accept RF prediction
    RF uncertain -> fallback to kdclass3 exact label

The runtime candidate families must not depend on kdclass3-only analysis
columns. The kdclass3_margin family is included only to measure boundary
difficulty and should not be used as a pre-gate.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

import numpy as np
import pandas as pd


VECTOR_COLS = [f"v{i}" for i in range(14)]
RUNTIME_FAMILIES = {"vector14", "vector_poly", "domain_flags"}


def check_optional_ml() -> dict[str, bool]:
    availability: dict[str, bool] = {}
    for name in ("sklearn", "xgboost", "lightgbm"):
        try:
            __import__(name)
            availability[name] = True
        except Exception:
            availability[name] = False
    return availability


def add_feature(frame: pd.DataFrame, columns: list[np.ndarray], names: list[str], values: np.ndarray, name: str) -> None:
    columns.append(values.astype(np.float32, copy=False))
    names.append(name)


def build_features(df: pd.DataFrame, family: str) -> tuple[np.ndarray, list[str]]:
    cols: list[np.ndarray] = []
    names: list[str] = []

    vectors = df[VECTOR_COLS].to_numpy(dtype=np.float32)
    for i, col in enumerate(VECTOR_COLS):
        add_feature(df, cols, names, vectors[:, i], col)

    if family in ("vector_poly", "domain_flags", "kdclass3_margin"):
        for i in range(vectors.shape[1]):
            add_feature(df, cols, names, np.abs(vectors[:, i]), f"abs_v{i}")
        # Scale squares/interactions so thresholds stay numerically tame for C export.
        for i in range(vectors.shape[1]):
            add_feature(df, cols, names, (vectors[:, i] * vectors[:, i]) / 10000.0, f"sq_v{i}")

        interactions = [
            (0, 12, "amount_mcc"),
            (0, 9, "amount_online"),
            (0, 10, "amount_card_present"),
            (0, 11, "amount_unknown_merchant"),
            (2, 0, "amount_vs_avg_amount"),
            (2, 13, "avg_ratio_merchant_avg"),
            (3, 12, "hour_mcc"),
            (5, 8, "last_tx_count"),
            (6, 7, "last_home_distance"),
            (7, 12, "home_mcc"),
            (8, 12, "tx_count_mcc"),
            (9, 12, "online_mcc"),
            (10, 12, "card_mcc"),
            (11, 12, "unknown_mcc"),
        ]
        for a, b, name in interactions:
            add_feature(df, cols, names, (vectors[:, a] * vectors[:, b]) / 10000.0, name)

    if family in ("domain_flags", "kdclass3_margin"):
        flag_specs = [
            (0, -5000, "amount_lt_neg_5000"),
            (0, 0, "amount_ge_0"),
            (0, 5000, "amount_ge_5000"),
            (2, -5000, "ratio_lt_neg_5000"),
            (2, 5000, "ratio_ge_5000"),
            (3, 0, "hour_ge_0"),
            (5, -9999, "last_tx_sentinel"),
            (6, -9999, "last_km_sentinel"),
            (7, 5000, "home_km_ge_5000"),
            (8, 5000, "tx24_ge_5000"),
            (9, 0, "is_online"),
            (10, 0, "card_present"),
            (11, 0, "unknown_merchant"),
            (12, 0, "mcc_risk_ge_0"),
            (12, 5000, "mcc_risk_ge_5000"),
            (13, -9999, "merchant_avg_sentinel"),
        ]
        for dim, threshold, name in flag_specs:
            if "sentinel" in name:
                values = (vectors[:, dim] <= threshold).astype(np.float32)
            else:
                values = (vectors[:, dim] >= threshold).astype(np.float32)
            add_feature(df, cols, names, values, name)

    if family == "kdclass3_margin":
        # Analysis-only: these require exact kdclass3 distances, so they cannot
        # be used to avoid kdclass3 in a runtime pre-gate.
        for col in ("kd_fraud_distance3", "kd_legit_distance3", "kd_margin"):
            values = np.log1p(df[col].to_numpy(dtype=np.float64)).astype(np.float32)
            add_feature(df, cols, names, values, f"log1p_{col}")
        add_feature(df, cols, names, df["kd_predicted_class"].to_numpy(dtype=np.float32), "kd_predicted_class")

    if family not in ("vector14", "vector_poly", "domain_flags", "kdclass3_margin"):
        raise ValueError(f"unknown feature family: {family}")

    return np.column_stack(cols).astype(np.float32, copy=False), names


@dataclass
class TreeNode:
    feature: int = -1
    threshold: float = 0.0
    left: int = -1
    right: int = -1
    probability: float = 0.0
    samples: int = 0


class DecisionTree:
    def __init__(
        self,
        max_depth: int,
        min_leaf: int,
        feature_mode: str,
        max_thresholds: int,
        rng: np.random.Generator,
    ) -> None:
        self.max_depth = max_depth
        self.min_leaf = min_leaf
        self.feature_mode = feature_mode
        self.max_thresholds = max_thresholds
        self.rng = rng
        self.nodes: list[TreeNode] = []

    def _feature_subset(self, feature_count: int) -> np.ndarray:
        if self.feature_mode == "all":
            return np.arange(feature_count)
        if self.feature_mode == "sqrt":
            size = max(1, int(round(math.sqrt(feature_count))))
        elif self.feature_mode == "half":
            size = max(1, feature_count // 2)
        else:
            raise ValueError(f"unknown feature mode {self.feature_mode}")
        return np.sort(self.rng.choice(feature_count, size=size, replace=False))

    @staticmethod
    def _gini(pos: int, n: int) -> float:
        if n <= 0:
            return 0.0
        p = pos / n
        return 1.0 - p * p - (1.0 - p) * (1.0 - p)

    def fit(self, x: np.ndarray, y: np.ndarray, indices: np.ndarray) -> "DecisionTree":
        self.nodes = []
        self._build(x, y, indices.astype(np.int32, copy=False), 0)
        return self

    def _candidate_thresholds(self, values: np.ndarray) -> np.ndarray:
        unique = np.unique(values)
        if unique.size <= 1:
            return unique[:0]
        if unique.size <= self.max_thresholds:
            return (unique[:-1] + unique[1:]) * 0.5
        qs = np.linspace(0.03, 0.97, self.max_thresholds)
        thresholds = np.quantile(values, qs)
        return np.unique(thresholds.astype(np.float32))

    def _build(self, x: np.ndarray, y: np.ndarray, indices: np.ndarray, depth: int) -> int:
        node_id = len(self.nodes)
        labels = y[indices]
        pos = int(labels.sum())
        n = int(indices.size)
        probability = pos / n if n else 0.0
        self.nodes.append(TreeNode(probability=probability, samples=n))

        if depth >= self.max_depth or n < 2 * self.min_leaf or pos == 0 or pos == n:
            return node_id

        parent_gini = self._gini(pos, n)
        best_feature = -1
        best_threshold = 0.0
        best_gain = 0.0
        best_left: np.ndarray | None = None
        best_right: np.ndarray | None = None

        feature_ids = self._feature_subset(x.shape[1])
        for feature in feature_ids:
            values = x[indices, feature]
            for threshold in self._candidate_thresholds(values):
                left_mask = values <= threshold
                left_n = int(left_mask.sum())
                right_n = n - left_n
                if left_n < self.min_leaf or right_n < self.min_leaf:
                    continue
                left_labels = labels[left_mask]
                left_pos = int(left_labels.sum())
                right_pos = pos - left_pos
                child_gini = (left_n * self._gini(left_pos, left_n) + right_n * self._gini(right_pos, right_n)) / n
                gain = parent_gini - child_gini
                if gain > best_gain:
                    best_gain = gain
                    best_feature = int(feature)
                    best_threshold = float(threshold)
                    best_left = indices[left_mask]
                    best_right = indices[~left_mask]

        if best_feature < 0 or best_left is None or best_right is None:
            return node_id

        left_id = self._build(x, y, best_left, depth + 1)
        right_id = self._build(x, y, best_right, depth + 1)
        node = self.nodes[node_id]
        node.feature = best_feature
        node.threshold = best_threshold
        node.left = left_id
        node.right = right_id
        return node_id

    def predict_proba(self, x: np.ndarray) -> np.ndarray:
        out = np.empty(x.shape[0], dtype=np.float32)
        self._predict_node(0, x, np.arange(x.shape[0], dtype=np.int32), out)
        return out

    def _predict_node(self, node_id: int, x: np.ndarray, indices: np.ndarray, out: np.ndarray) -> None:
        if indices.size == 0:
            return
        node = self.nodes[node_id]
        if node.feature < 0:
            out[indices] = node.probability
            return
        values = x[indices, node.feature]
        left_mask = values <= node.threshold
        self._predict_node(node.left, x, indices[left_mask], out)
        self._predict_node(node.right, x, indices[~left_mask], out)

    @property
    def node_count(self) -> int:
        return len(self.nodes)


class Forest:
    def __init__(
        self,
        trees: int,
        max_depth: int,
        min_leaf: int,
        feature_mode: str,
        sample_fraction: float,
        max_thresholds: int,
        seed: int,
        verbose_trees: bool = False,
    ) -> None:
        self.trees = trees
        self.max_depth = max_depth
        self.min_leaf = min_leaf
        self.feature_mode = feature_mode
        self.sample_fraction = sample_fraction
        self.max_thresholds = max_thresholds
        self.seed = seed
        self.verbose_trees = verbose_trees
        self.models: list[DecisionTree] = []

    def fit(self, x: np.ndarray, y: np.ndarray, train_indices: np.ndarray) -> "Forest":
        rng = np.random.default_rng(self.seed)
        self.models = []
        sample_size = max(1, int(round(train_indices.size * self.sample_fraction)))
        for tree_id in range(self.trees):
            sample = rng.choice(train_indices, size=sample_size, replace=True)
            tree_rng = np.random.default_rng(int(rng.integers(1, 2**31 - 1)))
            tree = DecisionTree(self.max_depth, self.min_leaf, self.feature_mode, self.max_thresholds, tree_rng)
            tree.fit(x, y, sample)
            self.models.append(tree)
            if self.verbose_trees:
                print(f"  trained tree {tree_id + 1}/{self.trees} nodes={tree.node_count}", flush=True)
        return self

    def predict_proba(self, x: np.ndarray) -> np.ndarray:
        total = np.zeros(x.shape[0], dtype=np.float32)
        for tree in self.models:
            total += tree.predict_proba(x)
        return total / max(1, len(self.models))

    @property
    def node_count(self) -> int:
        return sum(tree.node_count for tree in self.models)

    def summary(self) -> dict[str, object]:
        return {
            "trees": self.trees,
            "max_depth": self.max_depth,
            "min_leaf": self.min_leaf,
            "feature_mode": self.feature_mode,
            "sample_fraction": self.sample_fraction,
            "max_thresholds": self.max_thresholds,
            "node_count": self.node_count,
        }


def median(values: Iterable[float]) -> float:
    xs = sorted(values)
    if not xs:
        return 0.0
    mid = len(xs) // 2
    if len(xs) % 2:
        return xs[mid]
    return (xs[mid - 1] + xs[mid]) / 2.0


def split_masks(df: pd.DataFrame) -> dict[str, np.ndarray]:
    parent_mod = (df["parent_id"].to_numpy(dtype=np.int64) % 10).astype(np.int32)
    source = df["source"].to_numpy(dtype=np.int32)
    official = source == 0
    return {
        "train": parent_mod < 6,
        "calibration": (parent_mod >= 6) & (parent_mod < 8),
        "validation": parent_mod >= 8,
        "official_full": official,
        "official_train": official & (parent_mod < 6),
        "official_calibration": official & (parent_mod >= 6) & (parent_mod < 8),
        "official_validation": official & (parent_mod >= 8),
    }


def error_counts(pred_fraud: np.ndarray, expected_approved: np.ndarray, mask: np.ndarray) -> tuple[int, int, int]:
    if mask.sum() == 0:
        return (0, 0, 0)
    pred_approved = ~pred_fraud.astype(bool)
    expected = expected_approved.astype(bool)
    fp = int((mask & (~pred_approved) & expected).sum())
    fn = int((mask & pred_approved & (~expected)).sum())
    errors = fp + fn
    return fp, fn, errors


def evaluate_gate(
    p_fraud: np.ndarray,
    y_fraud: np.ndarray,
    expected_approved: np.ndarray,
    masks: dict[str, np.ndarray],
    low: float,
    high: float,
) -> dict[str, float | int]:
    accept_legit = p_fraud <= low
    accept_fraud = p_fraud >= high
    accept = accept_legit | accept_fraud
    pred_fraud = np.where(accept, accept_fraud, y_fraud).astype(bool)
    rf_pred_fraud = p_fraud >= 0.5
    out: dict[str, float | int] = {"low": low, "high": high}

    for split in ("train", "calibration", "validation", "official_full", "official_validation"):
        mask = masks[split]
        n = int(mask.sum())
        fp, fn, errors = error_counts(pred_fraud, expected_approved, mask)
        rf_fp, rf_fn, rf_errors = error_counts(rf_pred_fraud, expected_approved, mask)
        accepted_mask = mask & accept
        accepted_fp, accepted_fn, accepted_errors = error_counts(accept_fraud, expected_approved, accepted_mask)
        out[f"{split}_n"] = n
        out[f"{split}_fp"] = fp
        out[f"{split}_fn"] = fn
        out[f"{split}_errors"] = errors
        out[f"{split}_fallback_rate"] = 1.0 - (float(accepted_mask.sum()) / n if n else 0.0)
        out[f"{split}_accepted_errors"] = accepted_errors
        out[f"{split}_accepted_fp"] = accepted_fp
        out[f"{split}_accepted_fn"] = accepted_fn
        out[f"{split}_rf_only_errors"] = rf_errors
        out[f"{split}_rf_only_fp"] = rf_fp
        out[f"{split}_rf_only_fn"] = rf_fn
    return out


def threshold_grid(p_cal: np.ndarray, y_cal: np.ndarray, expected_cal: np.ndarray) -> list[tuple[float, float, str]]:
    thresholds: list[tuple[float, float, str]] = []
    for band in (0.01, 0.03, 0.05, 0.08, 0.10, 0.15, 0.20, 0.25, 0.30, 0.35, 0.40, 0.45):
        thresholds.append((band, 1.0 - band, f"symmetric_{band:.2f}"))

    lows = (0.01, 0.03, 0.05, 0.08, 0.10, 0.15, 0.20, 0.25, 0.30, 0.35)
    highs = (0.65, 0.70, 0.75, 0.80, 0.85, 0.90, 0.92, 0.95, 0.97, 0.99)
    for low in lows:
        for high in highs:
            if low < 0.5 < high:
                thresholds.append((low, high, f"asym_{low:.2f}_{high:.2f}"))

    if p_cal.size:
        rf_pred = p_cal >= 0.5
        expected_fraud = ~expected_cal.astype(bool)
        wrong = rf_pred != expected_fraud
        confidence = np.maximum(p_cal, 1.0 - p_cal)
        if wrong.any():
            min_safe_conf = float(confidence[wrong].max()) + 1e-6
        else:
            min_safe_conf = 0.5
        min_safe_conf = min(max(min_safe_conf, 0.5), 0.999999)
        thresholds.append((1.0 - min_safe_conf, min_safe_conf, "cal_zero_accepted_error"))

        for q in (0.80, 0.85, 0.90, 0.925, 0.95, 0.975, 0.99):
            conf = float(np.quantile(confidence, q))
            thresholds.append((1.0 - conf, conf, f"cal_conf_q{q:.3f}"))

    seen: set[tuple[float, float]] = set()
    unique: list[tuple[float, float, str]] = []
    for low, high, name in thresholds:
        low = max(0.0, min(0.499999, low))
        high = max(0.500001, min(1.0, high))
        key = (round(low, 6), round(high, 6))
        if key not in seen:
            seen.add(key)
            unique.append((key[0], key[1], name))
    return unique


def parse_configs(mode: str) -> list[dict[str, object]]:
    if mode == "quick":
        raw = [
            (16, 8, 20, "sqrt", 0.80),
            (24, 10, 20, "sqrt", 0.80),
            (30, 12, 20, "sqrt", 0.80),
            (40, 12, 50, "half", 0.80),
        ]
    elif mode == "wide":
        raw = []
        for trees in (16, 24, 30, 40, 64):
            for depth in (8, 10, 12, 16, 20):
                for min_leaf in (5, 10, 20, 50):
                    for feature_mode in ("sqrt", "half"):
                        raw.append((trees, depth, min_leaf, feature_mode, 0.80))
    else:
        raise ValueError("--configs must be quick or wide")
    return [
        {
            "trees": trees,
            "max_depth": depth,
            "min_leaf": min_leaf,
            "feature_mode": feature_mode,
            "sample_fraction": sample_fraction,
        }
        for trees, depth, min_leaf, feature_mode, sample_fraction in raw
    ]


def summarize_sources(df: pd.DataFrame) -> dict[str, int]:
    names = {
        0: "official",
        1: "official_perturb",
        2: "edge_perturb",
        3: "reference",
        4: "reference_perturb",
    }
    counts = df["source"].value_counts().to_dict()
    return {names.get(int(k), str(k)): int(v) for k, v in counts.items()}


def run_study(args: argparse.Namespace) -> int:
    dataset = Path(args.dataset)
    df = pd.read_csv(dataset)
    df = df[df["kd_fallback_required"] == 0].copy()
    if args.max_rows and len(df) > args.max_rows:
        df = df.sample(args.max_rows, random_state=args.seed).sort_index().reset_index(drop=True)

    y_approved = df["kd_approved"].to_numpy(dtype=np.int8)
    y_fraud = (1 - y_approved).astype(bool)
    expected_column = df["expected_approved"].to_numpy(dtype=np.int16)
    official_rows = df["source"].to_numpy(dtype=np.int16) == 0
    expected_approved = np.where(official_rows & (expected_column >= 0), expected_column, y_approved).astype(bool)
    masks = split_masks(df)

    print("tooling python", f"{np.__version__=}", f"{pd.__version__=}")
    print("optional_ml", json.dumps(check_optional_ml(), sort_keys=True))
    print("dataset_rows", len(df), "sources", json.dumps(summarize_sources(df), sort_keys=True))
    print(
        "split_sizes",
        json.dumps({name: int(mask.sum()) for name, mask in masks.items()}, sort_keys=True),
    )

    families = [item.strip() for item in args.families.split(",") if item.strip()]
    configs = parse_configs(args.configs)
    rows: list[dict[str, object]] = []
    best: dict[str, object] | None = None

    for family in families:
        x, feature_names = build_features(df, family)
        print(
            f"family_start family={family} rows={x.shape[0]} features={x.shape[1]} runtime_candidate={family in RUNTIME_FAMILIES}",
            flush=True,
        )
        train_indices = np.flatnonzero(masks["train"])
        cal_indices = np.flatnonzero(masks["calibration"])
        for config_id, config in enumerate(configs):
            start = time.time()
            print(f"model_start family={family} config_id={config_id} config={json.dumps(config, sort_keys=True)}")
            model = Forest(
                trees=int(config["trees"]),
                max_depth=int(config["max_depth"]),
                min_leaf=int(config["min_leaf"]),
                feature_mode=str(config["feature_mode"]),
                sample_fraction=float(config["sample_fraction"]),
                max_thresholds=args.max_thresholds,
                seed=args.seed + config_id * 1009 + len(family) * 17,
                verbose_trees=args.verbose_trees,
            )
            model.fit(x, y_fraud.astype(np.int8), train_indices)
            p_fraud = model.predict_proba(x)
            elapsed = time.time() - start
            threshold_rows = []
            for low, high, threshold_name in threshold_grid(
                p_fraud[cal_indices],
                y_fraud[cal_indices],
                expected_approved[cal_indices],
            ):
                result = evaluate_gate(p_fraud, y_fraud, expected_approved, masks, low, high)
                row = {
                    "family": family,
                    "runtime_candidate": family in RUNTIME_FAMILIES,
                    "threshold_name": threshold_name,
                    "model_id": f"{family}_rf{config['trees']}_d{config['max_depth']}_leaf{config['min_leaf']}_{config['feature_mode']}_{config_id}",
                    "trees": int(config["trees"]),
                    "max_depth": int(config["max_depth"]),
                    "min_leaf": int(config["min_leaf"]),
                    "feature_mode": str(config["feature_mode"]),
                    "feature_count": x.shape[1],
                    "node_count": model.node_count,
                    "train_seconds": elapsed,
                    **result,
                }
                threshold_rows.append(row)
                rows.append(row)

            sorted_rows = sorted(
                threshold_rows,
                key=lambda r: (
                    int(r["validation_accepted_errors"]),
                    int(r["official_full_accepted_errors"]),
                    float(r["validation_fallback_rate"]),
                    float(r["official_full_fallback_rate"]),
                    int(r["node_count"]),
                ),
            )
            top = sorted_rows[0]
            print(
                "model_best "
                f"family={family} model={top['model_id']} threshold={top['threshold_name']} "
                f"low={top['low']:.6f} high={top['high']:.6f} "
                f"val_acc_err={top['validation_accepted_errors']} "
                f"val_fallback={top['validation_fallback_rate']:.6f} "
                f"full_acc_err={top['official_full_accepted_errors']} "
                f"full_fallback={top['official_full_fallback_rate']:.6f} "
                f"rf_only_full={top['official_full_rf_only_errors']} "
                f"nodes={top['node_count']} train_s={elapsed:.2f}",
                flush=True,
            )
            if best is None or (
                int(top["validation_accepted_errors"]),
                int(top["official_full_accepted_errors"]),
                float(top["validation_fallback_rate"]),
                float(top["official_full_fallback_rate"]),
                int(top["node_count"]),
            ) < (
                int(best["validation_accepted_errors"]),
                int(best["official_full_accepted_errors"]),
                float(best["validation_fallback_rate"]),
                float(best["official_full_fallback_rate"]),
                int(best["node_count"]),
            ):
                best = top

    if args.output:
        out = Path(args.output)
        out.parent.mkdir(parents=True, exist_ok=True)
        if rows:
            with out.open("w", newline="") as f:
                writer = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
                writer.writeheader()
                writer.writerows(rows)
            print(f"wrote_results {out}")

    if best is not None:
        print("best_candidate", json.dumps(best, sort_keys=True))
        if args.best_json:
            best_path = Path(args.best_json)
            best_path.parent.mkdir(parents=True, exist_ok=True)
            best_path.write_text(json.dumps(best, indent=2, sort_keys=True), encoding="utf-8")
            print(f"wrote_best {best_path}")

    if rows:
        promising = [
            r for r in rows
            if bool(r["runtime_candidate"])
            and int(r["validation_accepted_errors"]) == 0
            and int(r["official_full_accepted_errors"]) == 0
        ]
        promising = sorted(
            promising,
            key=lambda r: (
                float(r["official_full_fallback_rate"]),
                float(r["validation_fallback_rate"]),
                int(r["node_count"]),
            ),
        )
        print("top_runtime_candidates")
        for row in promising[:10]:
            print(
                f"  {row['model_id']} {row['threshold_name']} "
                f"low={row['low']:.6f} high={row['high']:.6f} "
                f"val_fb={row['validation_fallback_rate']:.6f} "
                f"full_fb={row['official_full_fallback_rate']:.6f} "
                f"rf_only_full={row['official_full_rf_only_errors']} "
                f"nodes={row['node_count']} features={row['feature_count']}"
            )
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="offline RF distillation study for kdclass3")
    parser.add_argument("--dataset", required=True)
    parser.add_argument("--families", default="vector14,vector_poly,domain_flags,kdclass3_margin")
    parser.add_argument("--configs", choices=("quick", "wide"), default="quick")
    parser.add_argument("--max-thresholds", type=int, default=16)
    parser.add_argument("--seed", type=int, default=1337)
    parser.add_argument("--max-rows", type=int, default=0)
    parser.add_argument("--output", default="")
    parser.add_argument("--best-json", default="")
    parser.add_argument("--verbose-trees", action="store_true")
    args = parser.parse_args()
    return run_study(args)


if __name__ == "__main__":
    raise SystemExit(main())
