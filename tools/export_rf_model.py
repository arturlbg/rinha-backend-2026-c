#!/usr/bin/env python3
"""Export the Phase 13B RF gate candidate to C.

The exported model is intentionally fixed to the selected runtime-eligible
candidate from Phase 13A:

    vector_poly_rf30_d12_leaf20_sqrt_2
    low=0.08, high=0.99

It retrains the deterministic forest from the ignored training dataset, then
emits tracked C source/header files plus a small parity fixture.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np
import pandas as pd

import rf_distill_train as rf


MODEL_ID = "vector_poly_rf30_d12_leaf20_sqrt_2"
THRESHOLD_NAME = "asym_0.08_0.99"
LOW_THRESHOLD = 0.08
HIGH_THRESHOLD = 0.99
TREE_COUNT = 30
MAX_DEPTH = 12
MIN_LEAF = 20
FEATURE_MODE = "sqrt"
SAMPLE_FRACTION = 0.80
CONFIG_ID = 2
FAMILY = "vector_poly"
FEATURE_COUNT = 56
MAX_THRESHOLDS = 16
SEED = 1337


def c_float(value: float) -> str:
    return f"{float(value):.17g}"


def c_float32_literal(value: float) -> str:
    text = f"{float(value):.9g}"
    if "e" not in text and "E" not in text and "." not in text:
        text += ".0"
    return text + "f"


def model_seed() -> int:
    return SEED + CONFIG_ID * 1009 + len(FAMILY) * 17


def load_training_frame(dataset: Path) -> pd.DataFrame:
    frame = pd.read_csv(dataset)
    frame = frame[frame["kd_fallback_required"] == 0].copy()
    frame.reset_index(drop=True, inplace=True)
    return frame


def train_model(frame: pd.DataFrame) -> tuple[rf.Forest, np.ndarray, list[str], np.ndarray, dict[str, np.ndarray]]:
    x, feature_names = rf.build_features(frame, FAMILY)
    if x.shape[1] != FEATURE_COUNT:
        raise RuntimeError(f"expected {FEATURE_COUNT} features, got {x.shape[1]}")

    y_approved = frame["kd_approved"].to_numpy(dtype=np.int8)
    y_fraud = (1 - y_approved).astype(np.int8)
    masks = rf.split_masks(frame)
    train_indices = np.flatnonzero(masks["train"])
    model = rf.Forest(
        trees=TREE_COUNT,
        max_depth=MAX_DEPTH,
        min_leaf=MIN_LEAF,
        feature_mode=FEATURE_MODE,
        sample_fraction=SAMPLE_FRACTION,
        max_thresholds=MAX_THRESHOLDS,
        seed=model_seed(),
    )
    model.fit(x, y_fraud, train_indices)
    return model, x, feature_names, y_fraud.astype(bool), masks


def flatten_nodes(model: rf.Forest) -> tuple[list[int], list[dict[str, object]]]:
    roots: list[int] = []
    nodes: list[dict[str, object]] = []
    offset = 0
    for tree in model.models:
        roots.append(offset)
        for node in tree.nodes:
            if node.feature < 0:
                left = -1
                right = -1
            else:
                left = offset + node.left
                right = offset + node.right
            nodes.append(
                {
                    "feature": int(node.feature),
                    "left": int(left),
                    "right": int(right),
                    "threshold": float(node.threshold),
                    "probability": float(node.probability),
                }
            )
        offset += len(tree.nodes)
    return roots, nodes


def write_header(path: Path) -> None:
    text = f"""#ifndef RINHA_RF_GATE_MODEL_H
#define RINHA_RF_GATE_MODEL_H

#include <stdint.h>

#include "fastvector.h"

#define RF_GATE_MODEL_ID "{MODEL_ID}"
#define RF_GATE_THRESHOLD_NAME "{THRESHOLD_NAME}"
#define RF_GATE_TREE_COUNT {TREE_COUNT}u
#define RF_GATE_FEATURE_COUNT {FEATURE_COUNT}u
#define RF_GATE_LOW_THRESHOLD {LOW_THRESHOLD:.8f}
#define RF_GATE_HIGH_THRESHOLD {HIGH_THRESHOLD:.8f}
#define RF_GATE_NODE_COUNT 8490u

typedef enum {{
    RF_GATE_DECISION_FALLBACK = 0,
    RF_GATE_DECISION_LEGIT = 1,
    RF_GATE_DECISION_FRAUD = 2
}} RfGateDecision;

void rf_gate_vector_poly_features(const int16_t query[FASTVECTOR_DIMENSIONS],
                                  float features[RF_GATE_FEATURE_COUNT]);
double rf_gate_predict_fraud_probability_from_features(const float features[RF_GATE_FEATURE_COUNT]);
double rf_gate_predict_fraud_probability(const int16_t query[FASTVECTOR_DIMENSIONS]);
RfGateDecision rf_gate_decide_probability(double probability);
RfGateDecision rf_gate_decide(const int16_t query[FASTVECTOR_DIMENSIONS],
                              double *out_probability);

#endif
"""
    path.write_text(text, encoding="utf-8", newline="\n")


def write_source(path: Path, roots: list[int], nodes: list[dict[str, object]], feature_names: list[str]) -> None:
    if len(nodes) != 8490:
        raise RuntimeError(f"expected 8490 nodes, got {len(nodes)}")

    lines: list[str] = []
    lines.append('#include "rf_gate_model.h"')
    lines.append("")
    lines.append("#include <stddef.h>")
    lines.append("")
    lines.append("typedef struct {")
    lines.append("    int16_t feature;")
    lines.append("    int32_t left;")
    lines.append("    int32_t right;")
    lines.append("    double threshold;")
    lines.append("    double probability;")
    lines.append("} RfGateNode;")
    lines.append("")
    lines.append("/* Feature order:")
    for i, name in enumerate(feature_names):
        lines.append(f" * {i:02d}: {name}")
    lines.append(" */")
    lines.append("void rf_gate_vector_poly_features(const int16_t query[FASTVECTOR_DIMENSIONS],")
    lines.append("                                  float features[RF_GATE_FEATURE_COUNT]) {")
    lines.append("    for (uint32_t i = 0; i < FASTVECTOR_DIMENSIONS; i++) {")
    lines.append("        features[i] = (float)query[i];")
    lines.append("    }")
    lines.append("    for (uint32_t i = 0; i < FASTVECTOR_DIMENSIONS; i++) {")
    lines.append("        float value = features[i];")
    lines.append("        features[14u + i] = value < 0.0f ? -value : value;")
    lines.append("    }")
    lines.append("    for (uint32_t i = 0; i < FASTVECTOR_DIMENSIONS; i++) {")
    lines.append("        float value = features[i];")
    lines.append("        features[28u + i] = (value * value) / 10000.0f;")
    lines.append("    }")
    interactions = [
        (0, 12),
        (0, 9),
        (0, 10),
        (0, 11),
        (2, 0),
        (2, 13),
        (3, 12),
        (5, 8),
        (6, 7),
        (7, 12),
        (8, 12),
        (9, 12),
        (10, 12),
        (11, 12),
    ]
    for out_idx, (a, b) in enumerate(interactions, start=42):
        lines.append(
            f"    features[{out_idx}u] = (features[{a}u] * features[{b}u]) / 10000.0f;"
        )
    lines.append("}")
    lines.append("")
    lines.append(f"static const uint32_t RF_GATE_ROOTS[RF_GATE_TREE_COUNT] = {{")
    for i in range(0, len(roots), 10):
        chunk = ", ".join(f"{root}u" for root in roots[i : i + 10])
        lines.append(f"    {chunk},")
    lines.append("};")
    lines.append("")
    lines.append("static const RfGateNode RF_GATE_NODES[RF_GATE_NODE_COUNT] = {")
    for node in nodes:
        lines.append(
            "    {"
            f"{node['feature']}, {node['left']}, {node['right']}, "
            f"{c_float(float(node['threshold']))}, {c_float(float(node['probability']))}"
            "},"
        )
    lines.append("};")
    lines.append("")
    lines.append("static double predict_tree(uint32_t root, const float features[RF_GATE_FEATURE_COUNT]) {")
    lines.append("    uint32_t node_id = root;")
    lines.append("    for (;;) {")
    lines.append("        const RfGateNode *node = &RF_GATE_NODES[node_id];")
    lines.append("        if (node->feature < 0) {")
    lines.append("            return node->probability;")
    lines.append("        }")
    lines.append("        node_id = ((double)features[node->feature] <= node->threshold) ?")
    lines.append("            (uint32_t)node->left : (uint32_t)node->right;")
    lines.append("    }")
    lines.append("}")
    lines.append("")
    lines.append("double rf_gate_predict_fraud_probability_from_features(const float features[RF_GATE_FEATURE_COUNT]) {")
    lines.append("    double sum = 0.0;")
    lines.append("    for (uint32_t tree = 0; tree < RF_GATE_TREE_COUNT; tree++) {")
    lines.append("        sum += predict_tree(RF_GATE_ROOTS[tree], features);")
    lines.append("    }")
    lines.append("    return sum / (double)RF_GATE_TREE_COUNT;")
    lines.append("}")
    lines.append("")
    lines.append("double rf_gate_predict_fraud_probability(const int16_t query[FASTVECTOR_DIMENSIONS]) {")
    lines.append("    float features[RF_GATE_FEATURE_COUNT];")
    lines.append("    rf_gate_vector_poly_features(query, features);")
    lines.append("    return rf_gate_predict_fraud_probability_from_features(features);")
    lines.append("}")
    lines.append("")
    lines.append("RfGateDecision rf_gate_decide_probability(double probability) {")
    lines.append("    if (probability <= RF_GATE_LOW_THRESHOLD) {")
    lines.append("        return RF_GATE_DECISION_LEGIT;")
    lines.append("    }")
    lines.append("    if (probability >= RF_GATE_HIGH_THRESHOLD) {")
    lines.append("        return RF_GATE_DECISION_FRAUD;")
    lines.append("    }")
    lines.append("    return RF_GATE_DECISION_FALLBACK;")
    lines.append("}")
    lines.append("")
    lines.append("RfGateDecision rf_gate_decide(const int16_t query[FASTVECTOR_DIMENSIONS],")
    lines.append("                              double *out_probability) {")
    lines.append("    double probability = rf_gate_predict_fraud_probability(query);")
    lines.append("    if (out_probability != NULL) {")
    lines.append("        *out_probability = probability;")
    lines.append("    }")
    lines.append("    return rf_gate_decide_probability(probability);")
    lines.append("}")
    lines.append("")
    path.write_text("\n".join(lines), encoding="utf-8", newline="\n")


def select_fixture_rows(frame: pd.DataFrame, p_fraud: np.ndarray) -> np.ndarray:
    official = np.flatnonzero(frame["source"].to_numpy(dtype=np.int32) == 0)
    if official.size == 0:
        return np.arange(min(100, len(frame)), dtype=np.int64)

    decisions = np.where(
        p_fraud <= LOW_THRESHOLD,
        1,
        np.where(p_fraud >= HIGH_THRESHOLD, 2, 0),
    )
    selected: list[int] = []
    for decision in (1, 2, 0):
        matches = official[decisions[official] == decision]
        if matches.size:
            step = max(1, matches.size // 34)
            selected.extend(matches[::step][:34].tolist())
    if len(selected) < 100:
        step = max(1, official.size // (100 - len(selected)))
        selected.extend(official[::step].tolist())
    unique: list[int] = []
    seen: set[int] = set()
    for idx in selected:
        if idx not in seen:
            seen.add(idx)
            unique.append(idx)
        if len(unique) == 100:
            break
    return np.asarray(unique, dtype=np.int64)


def write_fixture(path: Path, frame: pd.DataFrame, x: np.ndarray, p_fraud: np.ndarray) -> None:
    indices = select_fixture_rows(frame, p_fraud)
    decisions = np.where(
        p_fraud[indices] <= LOW_THRESHOLD,
        1,
        np.where(p_fraud[indices] >= HIGH_THRESHOLD, 2, 0),
    )
    lines: list[str] = []
    lines.append("#ifndef RINHA_RF_GATE_FIXTURE_H")
    lines.append("#define RINHA_RF_GATE_FIXTURE_H")
    lines.append("")
    lines.append("#include <stdint.h>")
    lines.append("")
    lines.append("#define RF_GATE_FIXTURE_COUNT %uu" % len(indices))
    lines.append("")
    lines.append("static const int16_t RF_GATE_FIXTURE_VECTORS[RF_GATE_FIXTURE_COUNT][14] = {")
    for idx in indices:
        values = ", ".join(str(int(v)) for v in frame.loc[idx, [f"v{i}" for i in range(14)]].to_numpy())
        lines.append(f"    {{{values}}},")
    lines.append("};")
    lines.append("")
    lines.append("static const float RF_GATE_FIXTURE_FEATURES[RF_GATE_FIXTURE_COUNT][56] = {")
    for idx in indices:
        values = ", ".join(c_float32_literal(float(v)) for v in x[idx])
        lines.append(f"    {{{values}}},")
    lines.append("};")
    lines.append("")
    lines.append("static const double RF_GATE_FIXTURE_PROBABILITY[RF_GATE_FIXTURE_COUNT] = {")
    for idx in indices:
        lines.append(f"    {c_float(float(p_fraud[idx]))},")
    lines.append("};")
    lines.append("")
    lines.append("static const uint8_t RF_GATE_FIXTURE_DECISION[RF_GATE_FIXTURE_COUNT] = {")
    for decision in decisions:
        lines.append(f"    {int(decision)}u,")
    lines.append("};")
    lines.append("")
    lines.append("#endif")
    lines.append("")
    path.write_text("\n".join(lines), encoding="utf-8", newline="\n")


def main() -> int:
    parser = argparse.ArgumentParser(description="export Phase 13B RF gate model to C")
    parser.add_argument("--dataset", default="tmp/rf/rf-dataset.csv")
    parser.add_argument("--header", default="include/rf_gate_model.h")
    parser.add_argument("--source", default="src/rf_gate_model.c")
    parser.add_argument("--fixture", default="tests/rf_gate_fixture.h")
    parser.add_argument("--metadata", default="tmp/rf/rf-export-metadata.json")
    args = parser.parse_args()

    frame = load_training_frame(Path(args.dataset))
    model, x, feature_names, y_fraud, masks = train_model(frame)
    p_fraud = model.predict_proba(x)
    result = rf.evaluate_gate(
        p_fraud,
        y_fraud.astype(bool),
        np.where(
            (frame["source"].to_numpy(dtype=np.int16) == 0)
            & (frame["expected_approved"].to_numpy(dtype=np.int16) >= 0),
            frame["expected_approved"].to_numpy(dtype=np.int16),
            frame["kd_approved"].to_numpy(dtype=np.int16),
        ).astype(bool),
        masks,
        LOW_THRESHOLD,
        HIGH_THRESHOLD,
    )
    roots, nodes = flatten_nodes(model)
    Path(args.header).parent.mkdir(parents=True, exist_ok=True)
    Path(args.source).parent.mkdir(parents=True, exist_ok=True)
    Path(args.fixture).parent.mkdir(parents=True, exist_ok=True)
    write_header(Path(args.header))
    write_source(Path(args.source), roots, nodes, feature_names)
    write_fixture(Path(args.fixture), frame, x, p_fraud)

    metadata = {
        "model_id": MODEL_ID,
        "threshold_name": THRESHOLD_NAME,
        "low": LOW_THRESHOLD,
        "high": HIGH_THRESHOLD,
        "trees": TREE_COUNT,
        "max_depth": MAX_DEPTH,
        "min_leaf": MIN_LEAF,
        "feature_mode": FEATURE_MODE,
        "feature_count": FEATURE_COUNT,
        "node_count": len(nodes),
        "seed": model_seed(),
        "gate_result": result,
    }
    Path(args.metadata).parent.mkdir(parents=True, exist_ok=True)
    Path(args.metadata).write_text(json.dumps(metadata, indent=2, sort_keys=True), encoding="utf-8")
    print(json.dumps(metadata, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
