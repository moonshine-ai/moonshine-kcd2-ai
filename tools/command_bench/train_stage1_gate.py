#!/usr/bin/env python3
"""Train and benchmark the Stage-1 `EmbeddingGemma-300M` 768-d Fast Intent Gate.

Reuses the L2-normalized 768-d query embedding already computed on every turn for
`KnowledgeDirector::advise()` (`0 ms` extra embedding cost; `< 0.02 ms` dot product).

Trains on the circle-disjoint `train` split (2,132 examples), selects operating thresholds
on `dev` (519 examples) to achieve >= 99.0% command recall, and evaluates on `test` (746 examples):
  1. Binary Command Gate (`command` vs `no_action`): skip rate on non-commands vs. command recall
  2. 4-Way Linear Softmax Classifier (`no_action`, `locate_npc`, `follow_npc`, `travel_to`)
  3. 2-Layer MLP (`768 -> 64 -> 4`) comparison
Exports compact C++-ready float weights (`< 25 KB`) to `tools/command_bench/stage1_gate_weights.json`.
"""
from __future__ import annotations

import json
import time
from pathlib import Path

import numpy as np
from sklearn.linear_model import LogisticRegression
from sklearn.neural_network import MLPClassifier

CLASSES = ["no_action", "locate_npc", "follow_npc", "travel_to"]
CLASS_TO_ID = {c: i for i, c in enumerate(CLASSES)}


def main() -> None:
    ds_path = Path("build/command_bench/dataset_v1.jsonl")
    emb_path = Path("build/command_bench/embeddings_768.f32")

    rows = [
        json.loads(line)
        for line in ds_path.read_text(encoding="utf-8").splitlines()
        if line.strip()
    ]
    vecs = np.fromfile(emb_path, dtype=np.float32).reshape(len(rows), 768)

    idx_by_split: dict[str, list[int]] = {"train": [], "dev": [], "test": []}
    y_all = np.array([CLASS_TO_ID[r["gold_action"]] for r in rows], dtype=np.int64)
    for i, r in enumerate(rows):
        idx_by_split[r["split"]].append(i)

    X_tr, y_tr = vecs[idx_by_split["train"]], y_all[idx_by_split["train"]]
    X_dv, y_dv = vecs[idx_by_split["dev"]], y_all[idx_by_split["dev"]]
    X_te, y_te = vecs[idx_by_split["test"]], y_all[idx_by_split["test"]]

    # 1. Multinomial Logistic Regression (single matrix multiply W @ x + b in C++: 768x4 = 3,072 floats = 12 KB!)
    clf_lin = LogisticRegression(C=8.0, max_iter=1000, random_state=42)
    clf_lin.fit(X_tr, y_tr)

    # 2. Small 2-layer MLP (768 -> 64 -> 4)
    clf_mlp = MLPClassifier(
        hidden_layer_sizes=(64,),
        alpha=1e-3,
        max_iter=400,
        early_stopping=True,
        random_state=42,
    )
    clf_mlp.fit(X_tr, y_tr)

    # Measure inference speed of raw numpy matrix multiply (upper bound on C++ dot product)
    W = clf_lin.coef_.astype(np.float32)  # (4, 768)
    b = clf_lin.intercept_.astype(np.float32)  # (4,)
    t0 = time.perf_counter()
    for _ in range(100):
        _ = X_te @ W.T + b
    per_query_us = ((time.perf_counter() - t0) / (100 * len(X_te))) * 1e6

    # Calibrate Stage-1 binary gate threshold on `dev` for P(command) = 1 - P(no_action)
    probs_dv = clf_lin.predict_proba(X_dv)
    p_cmd_dv = 1.0 - probs_dv[:, 0]
    is_cmd_dv = y_dv != 0

    # Sweep thresholds for 99.5%, 99.0%, and 98.0% dev recall
    probs_te = clf_lin.predict_proba(X_te)
    p_cmd_te = 1.0 - probs_te[:, 0]
    is_cmd_te = y_te != 0

    operating_points = []
    for target_rec in (0.995, 0.990, 0.980, 0.950):
        cmd_scores = np.sort(p_cmd_dv[is_cmd_dv])
        cut_idx = max(0, int(np.floor((1.0 - target_rec) * len(cmd_scores))) - 1)
        thresh = float(cmd_scores[cut_idx])
        dv_rec = float(np.mean(p_cmd_dv[is_cmd_dv] >= thresh))
        dv_skip = float(np.mean(p_cmd_dv[~is_cmd_dv] < thresh))
        te_rec = float(np.mean(p_cmd_te[is_cmd_te] >= thresh))
        te_skip = float(np.mean(p_cmd_te[~is_cmd_te] < thresh))
        # Realistic turn mix: ~85% normal chat/KB turns, ~15% command turns
        overall_skip_85_15 = 0.85 * te_skip + 0.15 * (1.0 - te_rec)
        operating_points.append(
            {
                "target_dev_recall": target_rec,
                "threshold": round(thresh, 4),
                "dev_cmd_recall": round(dv_rec, 4),
                "dev_noncmd_skip_rate": round(dv_skip, 4),
                "test_cmd_recall": round(te_rec, 4),
                "test_noncmd_skip_rate": round(te_skip, 4),
                "estimated_turn_skip_rate_85_15_mix": round(overall_skip_85_15, 4),
            }
        )

    lin_pred_te = np.argmax(probs_te, axis=1)
    mlp_probs_te = clf_mlp.predict_proba(X_te)
    mlp_pred_te = np.argmax(mlp_probs_te, axis=1)

    def eval_4way(preds: np.ndarray) -> dict[str, float]:
        acc = float(np.mean(preds == y_te))
        neg_mask = y_te == 0
        fpr = float(np.mean(preds[neg_mask] != 0))
        cmd_mask = y_te != 0
        tp = int(np.sum((preds == y_te) & cmd_mask))
        fp = int(np.sum((preds != 0) & ((y_te == 0) | (preds != y_te))))
        fn = int(np.sum((preds == 0) & cmd_mask))
        prec = tp / max(tp + fp, 1)
        rec = tp / max(tp + fn, 1)
        f1 = 2 * prec * rec / max(prec + rec, 1e-9)
        # Check breakdown on biographical 'where' traps specifically!
        test_rows = [rows[i] for i in idx_by_split["test"]]
        bio_idx = [
            j
            for j, r in enumerate(test_rows)
            if r["category"] == "neg_kb_biographical_where"
        ]
        bio_fpr = float(np.mean(preds[bio_idx] != 0)) if bio_idx else 0.0
        return {
            "test_4way_accuracy": round(acc, 4),
            "test_cmd_precision": round(prec, 4),
            "test_cmd_recall": round(rec, 4),
            "test_cmd_f1": round(f1, 4),
            "test_neg_fpr": round(fpr, 4),
            "test_biographical_where_fpr": round(bio_fpr, 4),
        }

    results = {
        "embedder": "embeddinggemma-300m-q8_0.gguf (shared with KnowledgeDirector, 0 ms extra embed cost)",
        "classifier_inference_us": round(per_query_us, 3),
        "linear_weights_bytes": int(W.nbytes + b.nbytes),
        "linear_4way": eval_4way(lin_pred_te),
        "mlp_64_4way": eval_4way(mlp_pred_te),
        "gate_operating_points": operating_points,
    }

    Path("tools/command_bench/stage1_gate_results.json").write_text(
        json.dumps(results, indent=2) + "\n", encoding="utf-8"
    )
    # Export linear weights for direct C++ header / JSON loading
    default_op = operating_points[1]  # 99.0% target recall operating point
    Path("tools/command_bench/stage1_gate_weights.json").write_text(
        json.dumps(
            {
                "classes": CLASSES,
                "dim": 768,
                "recommended_cmd_threshold": default_op["threshold"],
                "weights": [[round(float(x), 6) for x in row] for row in W],
                "bias": [round(float(x), 6) for x in b],
            }
        )
        + "\n",
        encoding="utf-8",
    )
    print(json.dumps(results, indent=2))


if __name__ == "__main__":
    main()
