#!/usr/bin/env python3
"""Offline certified model-gate study for KD-primary2.

The bundled runtime currently has numpy/pandas but not sklearn/xgboost/lightgbm.
This script therefore uses small deterministic C-exportable learners:

* CART-style classification trees.
* Extra randomized shallow forests.
* A compact custom logistic GBDT that fits regression trees to log-loss gradients.

All gates are evaluated with train/calibration/validation discipline.  Thresholds
and certified regions are chosen from calibration only; validation is the first
honest score used for the Phase 10G go/no-go decision.
"""

from __future__ import annotations

import argparse
import math
import sys
from dataclasses import dataclass
from typing import Iterable

import numpy as np
import pandas as pd


RNG_SEED = 20260530
EPS = 1e-7


@dataclass
class ClassNode:
    prob: float
    n: int
    feature: int = -1
    threshold: float = 0.0
    leaf_id: int = -1
    left: "ClassNode | None" = None
    right: "ClassNode | None" = None

    @property
    def is_leaf(self) -> bool:
        return self.feature < 0


@dataclass
class RegNode:
    value: float
    n: int
    feature: int = -1
    threshold: float = 0.0
    leaf_id: int = -1
    left: "RegNode | None" = None
    right: "RegNode | None" = None

    @property
    def is_leaf(self) -> bool:
        return self.feature < 0


def sigmoid(x: np.ndarray) -> np.ndarray:
    x = np.clip(x, -32.0, 32.0)
    return 1.0 / (1.0 + np.exp(-x))


class DecisionTree:
    def __init__(
        self,
        max_depth: int,
        min_leaf: int = 64,
        thresholds_per_feature: int = 16,
        feature_subsample: float = 1.0,
        rng_seed: int = RNG_SEED,
    ) -> None:
        self.max_depth = max_depth
        self.min_leaf = min_leaf
        self.thresholds_per_feature = thresholds_per_feature
        self.feature_subsample = feature_subsample
        self.rng = np.random.default_rng(rng_seed)
        self.root: ClassNode | None = None
        self.node_count = 0
        self.leaf_count = 0

    @staticmethod
    def _gini(pos: int, total: int) -> float:
        if total <= 0:
            return 0.0
        p = pos / total
        return 1.0 - p * p - (1.0 - p) * (1.0 - p)

    def _candidate_features(self, feature_count: int) -> np.ndarray:
        if self.feature_subsample >= 0.999:
            return np.arange(feature_count, dtype=np.int32)
        count = max(1, int(round(feature_count * self.feature_subsample)))
        return self.rng.choice(feature_count, size=count, replace=False).astype(np.int32)

    def _thresholds(self, values: np.ndarray) -> np.ndarray:
        if values.size <= 2:
            return np.unique(values)
        quantiles = np.linspace(0.04, 0.96, self.thresholds_per_feature)
        return np.unique(np.quantile(values, quantiles))

    def fit(self, x: np.ndarray, y: np.ndarray, indices: np.ndarray | None = None) -> "DecisionTree":
        if indices is None:
            indices = np.arange(x.shape[0], dtype=np.int32)
        self.node_count = 0
        self.leaf_count = 0
        self.root = self._build(x, y, indices.astype(np.int32, copy=False), 0)
        return self

    def _build(self, x: np.ndarray, y: np.ndarray, idx: np.ndarray, depth: int) -> ClassNode:
        total = int(idx.size)
        pos = int(y[idx].sum())
        node = ClassNode(prob=pos / total if total else 0.0, n=total)
        self.node_count += 1
        if depth >= self.max_depth or total < self.min_leaf * 2 or pos == 0 or pos == total:
            node.leaf_id = self.leaf_count
            self.leaf_count += 1
            return node

        parent_impurity = self._gini(pos, total)
        best_gain = 0.0
        best_feature = -1
        best_threshold = 0.0
        best_left: np.ndarray | None = None
        best_right: np.ndarray | None = None

        for feature in self._candidate_features(x.shape[1]):
            values = x[idx, feature]
            for threshold in self._thresholds(values):
                left_mask = values <= threshold
                left_total = int(left_mask.sum())
                right_total = total - left_total
                if left_total < self.min_leaf or right_total < self.min_leaf:
                    continue
                left_idx = idx[left_mask]
                right_idx = idx[~left_mask]
                left_pos = int(y[left_idx].sum())
                right_pos = pos - left_pos
                impurity = (left_total * self._gini(left_pos, left_total) +
                            right_total * self._gini(right_pos, right_total)) / total
                gain = parent_impurity - impurity
                if gain > best_gain:
                    best_gain = gain
                    best_feature = int(feature)
                    best_threshold = float(threshold)
                    best_left = left_idx
                    best_right = right_idx

        if best_feature < 0 or best_left is None or best_right is None:
            node.leaf_id = self.leaf_count
            self.leaf_count += 1
            return node
        node.feature = best_feature
        node.threshold = best_threshold
        node.left = self._build(x, y, best_left, depth + 1)
        node.right = self._build(x, y, best_right, depth + 1)
        return node

    def predict_proba(self, x: np.ndarray) -> np.ndarray:
        if self.root is None:
            raise RuntimeError("tree is not fitted")
        out = np.empty(x.shape[0], dtype=np.float32)
        self._predict_node(self.root, x, np.arange(x.shape[0]), out)
        return out

    def predict_leaf_ids(self, x: np.ndarray) -> np.ndarray:
        if self.root is None:
            raise RuntimeError("tree is not fitted")
        out = np.empty(x.shape[0], dtype=np.int32)
        self._predict_leaf_node(self.root, x, np.arange(x.shape[0]), out)
        return out

    def _predict_node(self, node: ClassNode, x: np.ndarray, idx: np.ndarray, out: np.ndarray) -> None:
        if idx.size == 0:
            return
        if node.is_leaf or node.left is None or node.right is None:
            out[idx] = node.prob
            return
        values = x[idx, node.feature]
        left_mask = values <= node.threshold
        self._predict_node(node.left, x, idx[left_mask], out)
        self._predict_node(node.right, x, idx[~left_mask], out)

    def _predict_leaf_node(self, node: ClassNode, x: np.ndarray, idx: np.ndarray, out: np.ndarray) -> None:
        if idx.size == 0:
            return
        if node.is_leaf or node.left is None or node.right is None:
            out[idx] = node.leaf_id
            return
        values = x[idx, node.feature]
        left_mask = values <= node.threshold
        self._predict_leaf_node(node.left, x, idx[left_mask], out)
        self._predict_leaf_node(node.right, x, idx[~left_mask], out)


class RegressionTree:
    def __init__(
        self,
        max_depth: int,
        min_leaf: int,
        thresholds_per_feature: int,
        feature_subsample: float,
        rng_seed: int,
    ) -> None:
        self.max_depth = max_depth
        self.min_leaf = min_leaf
        self.thresholds_per_feature = thresholds_per_feature
        self.feature_subsample = feature_subsample
        self.rng = np.random.default_rng(rng_seed)
        self.root: RegNode | None = None
        self.node_count = 0
        self.leaf_count = 0

    def _candidate_features(self, feature_count: int) -> np.ndarray:
        if self.feature_subsample >= 0.999:
            return np.arange(feature_count, dtype=np.int32)
        count = max(1, int(round(feature_count * self.feature_subsample)))
        return self.rng.choice(feature_count, size=count, replace=False).astype(np.int32)

    def _thresholds(self, values: np.ndarray) -> np.ndarray:
        if values.size <= 2:
            return np.unique(values)
        quantiles = np.linspace(0.08, 0.92, self.thresholds_per_feature)
        return np.unique(np.quantile(values, quantiles))

    @staticmethod
    def _sse(values: np.ndarray) -> float:
        if values.size == 0:
            return 0.0
        s = float(values.sum())
        ss = float((values * values).sum())
        return ss - s * s / values.size

    def fit(self, x: np.ndarray, target: np.ndarray, indices: np.ndarray) -> "RegressionTree":
        self.node_count = 0
        self.leaf_count = 0
        self.root = self._build(x, target, indices.astype(np.int32, copy=False), 0)
        return self

    def _build(self, x: np.ndarray, target: np.ndarray, idx: np.ndarray, depth: int) -> RegNode:
        values_target = target[idx]
        node = RegNode(value=float(values_target.mean()) if idx.size else 0.0, n=int(idx.size))
        self.node_count += 1
        if depth >= self.max_depth or idx.size < self.min_leaf * 2:
            node.leaf_id = self.leaf_count
            self.leaf_count += 1
            return node

        parent_sse = self._sse(values_target)
        best_gain = 0.0
        best_feature = -1
        best_threshold = 0.0
        best_left: np.ndarray | None = None
        best_right: np.ndarray | None = None
        for feature in self._candidate_features(x.shape[1]):
            values = x[idx, feature]
            for threshold in self._thresholds(values):
                left_mask = values <= threshold
                left_total = int(left_mask.sum())
                right_total = idx.size - left_total
                if left_total < self.min_leaf or right_total < self.min_leaf:
                    continue
                left_idx = idx[left_mask]
                right_idx = idx[~left_mask]
                sse = self._sse(target[left_idx]) + self._sse(target[right_idx])
                gain = parent_sse - sse
                if gain > best_gain:
                    best_gain = gain
                    best_feature = int(feature)
                    best_threshold = float(threshold)
                    best_left = left_idx
                    best_right = right_idx
        if best_feature < 0 or best_left is None or best_right is None:
            node.leaf_id = self.leaf_count
            self.leaf_count += 1
            return node
        node.feature = best_feature
        node.threshold = best_threshold
        node.left = self._build(x, target, best_left, depth + 1)
        node.right = self._build(x, target, best_right, depth + 1)
        return node

    def predict(self, x: np.ndarray) -> np.ndarray:
        if self.root is None:
            raise RuntimeError("tree is not fitted")
        out = np.empty(x.shape[0], dtype=np.float32)
        self._predict_node(self.root, x, np.arange(x.shape[0]), out)
        return out

    def _predict_node(self, node: RegNode, x: np.ndarray, idx: np.ndarray, out: np.ndarray) -> None:
        if idx.size == 0:
            return
        if node.is_leaf or node.left is None or node.right is None:
            out[idx] = node.value
            return
        values = x[idx, node.feature]
        left_mask = values <= node.threshold
        self._predict_node(node.left, x, idx[left_mask], out)
        self._predict_node(node.right, x, idx[~left_mask], out)


class Forest:
    def __init__(
        self,
        n_trees: int,
        max_depth: int,
        min_leaf: int,
        thresholds_per_feature: int,
        feature_subsample: float,
        sample_fraction: float,
        rng_seed: int = RNG_SEED,
    ) -> None:
        self.n_trees = n_trees
        self.max_depth = max_depth
        self.min_leaf = min_leaf
        self.thresholds_per_feature = thresholds_per_feature
        self.feature_subsample = feature_subsample
        self.sample_fraction = sample_fraction
        self.rng = np.random.default_rng(rng_seed)
        self.trees: list[DecisionTree] = []

    @property
    def node_count(self) -> int:
        return sum(tree.node_count for tree in self.trees)

    def fit(self, x: np.ndarray, y: np.ndarray) -> "Forest":
        self.trees = []
        sample_count = max(1, int(round(x.shape[0] * self.sample_fraction)))
        for i in range(self.n_trees):
            idx = self.rng.choice(x.shape[0], size=sample_count, replace=True).astype(np.int32)
            tree = DecisionTree(
                max_depth=self.max_depth,
                min_leaf=self.min_leaf,
                thresholds_per_feature=self.thresholds_per_feature,
                feature_subsample=self.feature_subsample,
                rng_seed=RNG_SEED + 1009 * (i + 1),
            )
            tree.fit(x, y, idx)
            self.trees.append(tree)
        return self

    def predict_proba(self, x: np.ndarray) -> np.ndarray:
        out = np.zeros(x.shape[0], dtype=np.float32)
        for tree in self.trees:
            out += tree.predict_proba(x)
        out /= max(1, len(self.trees))
        return out

    def predict_agreement(self, x: np.ndarray) -> np.ndarray:
        votes = np.zeros(x.shape[0], dtype=np.float32)
        for tree in self.trees:
            votes += tree.predict_proba(x) >= 0.5
        votes /= max(1, len(self.trees))
        return np.maximum(votes, 1.0 - votes)


class LogisticGBDT:
    def __init__(
        self,
        n_trees: int,
        max_depth: int,
        learning_rate: float,
        min_leaf: int,
        thresholds_per_feature: int,
        feature_subsample: float,
        sample_fraction: float,
        rng_seed: int = RNG_SEED,
    ) -> None:
        self.n_trees = n_trees
        self.max_depth = max_depth
        self.learning_rate = learning_rate
        self.min_leaf = min_leaf
        self.thresholds_per_feature = thresholds_per_feature
        self.feature_subsample = feature_subsample
        self.sample_fraction = sample_fraction
        self.rng = np.random.default_rng(rng_seed)
        self.base_logit = 0.0
        self.trees: list[RegressionTree] = []

    @property
    def node_count(self) -> int:
        return sum(tree.node_count for tree in self.trees)

    def fit(self, x: np.ndarray, y: np.ndarray) -> "LogisticGBDT":
        pos = float(y.mean())
        pos = min(max(pos, 1e-4), 1.0 - 1e-4)
        self.base_logit = math.log(pos / (1.0 - pos))
        f = np.full(x.shape[0], self.base_logit, dtype=np.float32)
        self.trees = []
        sample_count = max(1, int(round(x.shape[0] * self.sample_fraction)))
        all_idx = np.arange(x.shape[0], dtype=np.int32)
        for i in range(self.n_trees):
            p = sigmoid(f)
            # Log-loss negative gradient.  Clipping keeps leaf updates exportable and stable.
            residual = np.clip(y.astype(np.float32) - p.astype(np.float32), -0.5, 0.5)
            idx = self.rng.choice(all_idx, size=sample_count, replace=False).astype(np.int32)
            tree = RegressionTree(
                max_depth=self.max_depth,
                min_leaf=self.min_leaf,
                thresholds_per_feature=self.thresholds_per_feature,
                feature_subsample=self.feature_subsample,
                rng_seed=RNG_SEED + 7919 * (i + 1),
            )
            tree.fit(x, residual, idx)
            update = tree.predict(x)
            f += self.learning_rate * update
            self.trees.append(tree)
        return self

    def predict_proba(self, x: np.ndarray) -> np.ndarray:
        f = np.full(x.shape[0], self.base_logit, dtype=np.float32)
        for tree in self.trees:
            f += self.learning_rate * tree.predict(x)
        return sigmoid(f).astype(np.float32)


def feature_columns(df: pd.DataFrame, family: str) -> tuple[list[str], np.ndarray]:
    vector_cols = [f"v{i}" for i in range(14)]
    raw = df[vector_cols].to_numpy(np.float32)
    parts: list[np.ndarray] = [raw]
    names = list(vector_cols)

    if family in {"vector_poly", "ivf8"}:
        parts.append(np.abs(raw))
        names.extend([f"abs_v{i}" for i in range(14)])
        parts.append((raw * raw) / 10000.0)
        names.extend([f"sq_v{i}" for i in range(14)])
        pairs = [(0, 1), (0, 2), (0, 7), (1, 2), (2, 7), (6, 7), (8, 13)]
        inter = np.stack([(raw[:, a] * raw[:, b]) / 10000.0 for a, b in pairs], axis=1)
        parts.append(inter.astype(np.float32))
        names.extend([f"v{a}_x_v{b}" for a, b in pairs])
        thresholds = [
            (0, 1000), (0, 2500), (0, 5000),
            (1, 2500), (1, 5000), (1, 7500),
            (2, 2000), (2, 5000), (2, 10000),
            (7, 1000), (7, 3000), (7, 5000),
            (8, 3000), (8, 6000), (13, 200), (13, 500),
        ]
        flags = np.stack([(raw[:, dim] >= threshold).astype(np.float32) for dim, threshold in thresholds], axis=1)
        parts.append(flags)
        names.extend([f"v{dim}_ge_{threshold}" for dim, threshold in thresholds])

    if family == "ivf8":
        ivf8_cols = [
            "ivf8_fraud_count",
            "ivf8_best",
            "ivf8_worst",
            "ivf8_spread",
            "ivf8_gap10",
            "ivf8_gap21",
            "ivf8_gap32",
            "ivf8_gap43",
            "ivf8_candidates",
            "ivf8_clusters",
            "ivf8_largest_cluster",
        ]
        ivf8_cols.extend([f"ivf8_l{i}" for i in range(5)])
        ivf8_cols.extend([f"ivf8_d{i}" for i in range(5)])
        ivf8 = df[ivf8_cols].to_numpy(np.float32)
        for col in ["ivf8_best", "ivf8_worst", "ivf8_spread", "ivf8_gap10", "ivf8_gap21",
                    "ivf8_gap32", "ivf8_gap43", "ivf8_d0", "ivf8_d1", "ivf8_d2", "ivf8_d3", "ivf8_d4"]:
            idx = ivf8_cols.index(col)
            ivf8[:, idx] = np.log1p(np.maximum(ivf8[:, idx], 0.0))
        parts.append(ivf8)
        names.extend(ivf8_cols)

    return names, np.concatenate(parts, axis=1).astype(np.float32)


def split_masks(df: pd.DataFrame) -> tuple[np.ndarray, np.ndarray, np.ndarray, np.ndarray, np.ndarray, np.ndarray]:
    if "source" in df.columns:
        official = df["source"].to_numpy(np.int32) == 0
        parent = df["parent_id"].to_numpy(np.int32)
        synthetic = ~official
        mod = np.where(parent >= 0, parent % 10, 0)
        train_synthetic = synthetic & (mod < 6)
    else:
        official = np.ones(len(df), dtype=bool)
        parent = df["row_index"].to_numpy(np.int32)
        mod = parent % 10
        train_synthetic = np.zeros(len(df), dtype=bool)
    train_official = official & (mod < 6)
    cal_official = official & (mod >= 6) & (mod < 8)
    val_official = official & (mod >= 8)
    train_rows = train_official | train_synthetic
    return official, train_official, cal_official, val_official, train_rows, parent


def error_counts(final: np.ndarray, expected: np.ndarray) -> tuple[int, int]:
    fp = int(((~final) & expected).sum())
    fn = int((final & (~expected)).sum())
    return fp, fn


def apply_gate(proba: np.ndarray,
               y_expected: np.ndarray,
               y_kd: np.ndarray,
               mask: np.ndarray,
               accept: np.ndarray) -> dict[str, float]:
    idx = np.flatnonzero(mask)
    if idx.size == 0:
        return {"n": 0, "fallback": 0, "fallback_rate": 0.0, "fp": 0, "fn": 0, "errors": 0}
    pred = proba[idx] >= 0.5
    accept_idx = accept[idx]
    final = np.where(accept_idx, pred, y_kd[idx].astype(bool))
    expected = y_expected[idx].astype(bool)
    fp, fn = error_counts(final, expected)
    fallback = int((~accept_idx).sum())
    return {
        "n": int(idx.size),
        "fallback": fallback,
        "fallback_rate": fallback / idx.size,
        "fp": fp,
        "fn": fn,
        "errors": fp + fn,
    }


def accept_margin(proba: np.ndarray, y_expected: np.ndarray, cal_mask: np.ndarray) -> tuple[np.ndarray, str]:
    pred = proba >= 0.5
    conf = np.maximum(proba, 1.0 - proba)
    wrong = cal_mask & (pred != y_expected.astype(bool))
    threshold = float(conf[wrong].max() + EPS) if wrong.any() else 0.5
    threshold = min(1.0, threshold)
    return conf > threshold, f"margin>{threshold:.7f}"


def accept_agreement(agreement: np.ndarray | None,
                     proba: np.ndarray,
                     y_expected: np.ndarray,
                     cal_mask: np.ndarray) -> tuple[np.ndarray, str] | None:
    if agreement is None:
        return None
    pred = proba >= 0.5
    wrong = cal_mask & (pred != y_expected.astype(bool))
    threshold = float(agreement[wrong].max() + EPS) if wrong.any() else 0.5
    threshold = min(1.0, threshold)
    return agreement > threshold, f"agreement>{threshold:.7f}"


def accept_bins(proba: np.ndarray,
                y_expected: np.ndarray,
                cal_mask: np.ndarray,
                bins: int,
                min_support: int) -> tuple[np.ndarray, str]:
    pred = proba >= 0.5
    conf = np.maximum(proba, 1.0 - proba)
    bin_id = np.minimum((conf * bins).astype(np.int32), bins - 1)
    safe = np.zeros((2, bins), dtype=bool)
    for label in [0, 1]:
        for b in range(bins):
            m = cal_mask & (pred == bool(label)) & (bin_id == b)
            if int(m.sum()) >= min_support and bool((pred[m] == y_expected[m].astype(bool)).all()):
                safe[label, b] = True
    accept = safe[pred.astype(np.int32), bin_id]
    return accept, f"pure_conf_bins{bins}_min{min_support}"


def accept_hybrid(proba: np.ndarray,
                  agreement: np.ndarray | None,
                  y_expected: np.ndarray,
                  cal_mask: np.ndarray,
                  bins: int,
                  min_support: int) -> tuple[np.ndarray, str]:
    margin_accept, margin_name = accept_margin(proba, y_expected, cal_mask)
    bin_accept, bin_name = accept_bins(proba, y_expected, cal_mask, bins, min_support)
    accept = margin_accept & bin_accept
    name = f"hybrid_{margin_name}_{bin_name}"
    if agreement is not None:
        agreement_accept = accept_agreement(agreement, proba, y_expected, cal_mask)
        if agreement_accept is not None:
            accept &= agreement_accept[0]
            name += f"_{agreement_accept[1]}"
    return accept, name


def chunk_worst(proba: np.ndarray,
                y_expected: np.ndarray,
                y_kd: np.ndarray,
                official: np.ndarray,
                parent: np.ndarray,
                accept: np.ndarray) -> tuple[int, float, int, int]:
    worst_errors = -1
    worst_fb = 0.0
    worst_fp = 0
    worst_fn = 0
    max_parent = int(parent[official].max()) + 1
    for fold in range(5):
        lo = (max_parent * fold) // 5
        hi = (max_parent * (fold + 1)) // 5
        mask = official & (parent >= lo) & (parent < hi)
        stats = apply_gate(proba, y_expected, y_kd, mask, accept)
        if stats["errors"] > worst_errors:
            worst_errors = int(stats["errors"])
            worst_fb = float(stats["fallback_rate"])
            worst_fp = int(stats["fp"])
            worst_fn = int(stats["fn"])
    return worst_errors, worst_fb, worst_fp, worst_fn


def evaluate_certifications(model_name: str,
                            family: str,
                            node_count: int,
                            proba: np.ndarray,
                            y_expected: np.ndarray,
                            y_kd: np.ndarray,
                            masks: tuple[np.ndarray, np.ndarray, np.ndarray, np.ndarray],
                            parent: np.ndarray,
                            agreement: np.ndarray | None = None) -> list[dict[str, float]]:
    official, train_official, cal_official, val_official = masks
    candidates: list[tuple[np.ndarray, str]] = []
    candidates.append(accept_margin(proba, y_expected, cal_official))
    agreement_accept = accept_agreement(agreement, proba, y_expected, cal_official)
    if agreement_accept is not None:
        candidates.append(agreement_accept)
    for bins, min_support in [(50, 1), (100, 1), (100, 3), (200, 1)]:
        candidates.append(accept_bins(proba, y_expected, cal_official, bins, min_support))
    candidates.append(accept_hybrid(proba, agreement, y_expected, cal_official, 100, 1))

    out: list[dict[str, float]] = []
    for accept, cert_name in candidates:
        train = apply_gate(proba, y_expected, y_kd, train_official, accept)
        cal = apply_gate(proba, y_expected, y_kd, cal_official, accept)
        val = apply_gate(proba, y_expected, y_kd, val_official, accept)
        full = apply_gate(proba, y_expected, y_kd, official, accept)
        worst_errors, worst_fb, worst_fp, worst_fn = chunk_worst(proba, y_expected, y_kd, official, parent, accept)
        row = {
            "family": family,
            "model": model_name,
            "cert": cert_name,
            "nodes": node_count,
            "train_errors": train["errors"],
            "cal_errors": cal["errors"],
            "val_errors": val["errors"],
            "full_errors": full["errors"],
            "val_fallback": val["fallback_rate"],
            "full_fallback": full["fallback_rate"],
            "chunk_worst_errors": worst_errors,
            "chunk_worst_fallback": worst_fb,
        }
        out.append(row)
        print(
            "cert_result "
            f"family={family} model={model_name} cert={cert_name} nodes={node_count} "
            f"train_fp={train['fp']} train_fn={train['fn']} train_fb={train['fallback_rate']:.6f} "
            f"cal_fp={cal['fp']} cal_fn={cal['fn']} cal_fb={cal['fallback_rate']:.6f} "
            f"val_fp={val['fp']} val_fn={val['fn']} val_fb={val['fallback_rate']:.6f} "
            f"full_fp={full['fp']} full_fn={full['fn']} full_fb={full['fallback_rate']:.6f} "
            f"chunk_worst_errors={worst_errors} chunk_worst_fp={worst_fp} chunk_worst_fn={worst_fn} "
            f"chunk_worst_fb={worst_fb:.6f}"
        )
    return out


def run_study(df: pd.DataFrame,
              families: Iterable[str],
              model_set: str,
              sample_train: int) -> list[dict[str, float]]:
    official, train_official, cal_official, val_official, train_rows, parent = split_masks(df)
    y_kd = df["kd_approved"].to_numpy(np.int8)
    y_expected = np.where(official, df["expected_approved"].to_numpy(np.int8), y_kd).astype(np.int8)

    if sample_train > 0:
        rng = np.random.default_rng(RNG_SEED)
        train_idx = np.flatnonzero(train_rows)
        official_train_idx = np.flatnonzero(train_official)
        synthetic_train_idx = np.setdiff1d(train_idx, official_train_idx, assume_unique=False)
        if synthetic_train_idx.size > sample_train:
            keep_syn = rng.choice(synthetic_train_idx, size=sample_train, replace=False)
            train_rows = np.zeros(len(df), dtype=bool)
            train_rows[official_train_idx] = True
            train_rows[keep_syn] = True
            print(f"sampled_training_synthetic kept={sample_train} train_rows={int(train_rows.sum())}")

    print(
        "dataset_summary "
        f"rows={len(df)} official={int(official.sum())} train_official={int(train_official.sum())} "
        f"calibration_official={int(cal_official.sum())} validation_official={int(val_official.sum())} "
        f"train_rows={int(train_rows.sum())} synthetic={int((~official).sum())} "
        f"kd_expected_mismatch_official={int((y_kd[official] != y_expected[official]).sum())}"
    )

    all_results: list[dict[str, float]] = []
    for family in families:
        names, x_all = feature_columns(df, family)
        x_train = x_all[train_rows]
        y_train = y_kd[train_rows]
        masks = (official, train_official, cal_official, val_official)
        print(f"feature_family name={family} features={len(names)} train_rows={x_train.shape[0]}")

        if model_set in {"all", "baseline"}:
            for depth in [6, 8, 10]:
                tree = DecisionTree(max_depth=depth, min_leaf=64, thresholds_per_feature=16, rng_seed=RNG_SEED + depth)
                tree.fit(x_train, y_train)
                proba = tree.predict_proba(x_all)
                all_results.extend(evaluate_certifications(f"tree_d{depth}", family, tree.node_count, proba,
                                                           y_expected, y_kd, masks, parent))
            for n_trees, depth in [(12, 8), (16, 10), (24, 8)]:
                forest = Forest(n_trees=n_trees,
                                max_depth=depth,
                                min_leaf=64,
                                thresholds_per_feature=12,
                                feature_subsample=0.55,
                                sample_fraction=0.80,
                                rng_seed=RNG_SEED + n_trees * 100 + depth)
                forest.fit(x_train, y_train)
                proba = forest.predict_proba(x_all)
                agreement = forest.predict_agreement(x_all)
                all_results.extend(evaluate_certifications(f"forest{n_trees}_d{depth}", family, forest.node_count,
                                                           proba, y_expected, y_kd, masks, parent, agreement))

        if model_set in {"all", "gbdt"}:
            gbdt_grid = [
                (16, 3, 0.10, 96),
                (32, 3, 0.08, 96),
                (64, 3, 0.05, 128),
                (32, 4, 0.07, 128),
                (64, 4, 0.05, 160),
            ]
            for n_trees, depth, lr, min_leaf in gbdt_grid:
                gbdt = LogisticGBDT(n_trees=n_trees,
                                    max_depth=depth,
                                    learning_rate=lr,
                                    min_leaf=min_leaf,
                                    thresholds_per_feature=8,
                                    feature_subsample=0.50,
                                    sample_fraction=0.75,
                                    rng_seed=RNG_SEED + n_trees * 17 + depth)
                gbdt.fit(x_train, y_train)
                proba = gbdt.predict_proba(x_all)
                all_results.extend(evaluate_certifications(f"gbdt{n_trees}_d{depth}_lr{lr}", family, gbdt.node_count,
                                                           proba, y_expected, y_kd, masks, parent))
    all_results.sort(key=lambda r: (r["val_errors"], r["val_fallback"], r["chunk_worst_errors"], r["full_errors"]))
    if all_results:
        best = all_results[0]
        print(
            "best_candidate "
            f"family={best['family']} model={best['model']} cert={best['cert']} nodes={best['nodes']} "
            f"val_errors={best['val_errors']} val_fallback={best['val_fallback']:.6f} "
            f"full_errors={best['full_errors']} full_fallback={best['full_fallback']:.6f} "
            f"chunk_worst_errors={best['chunk_worst_errors']} chunk_worst_fallback={best['chunk_worst_fallback']:.6f}"
        )
    return all_results


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dataset", required=True)
    parser.add_argument("--families", default="vector_poly,ivf8",
                        help="comma-separated: vector,vector_poly,ivf8")
    parser.add_argument("--models", default="all", choices=["all", "baseline", "gbdt"])
    parser.add_argument("--sample-train", type=int, default=0,
                        help="optional deterministic cap for synthetic training rows")
    args = parser.parse_args()

    df = pd.read_csv(args.dataset)
    families = [part.strip() for part in args.families.split(",") if part.strip()]
    run_study(df, families, args.models, args.sample_train)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
