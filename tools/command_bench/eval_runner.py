#!/usr/bin/env python3
"""Unified end-to-end evaluation runner for the KCD2 NPC Command Bake-Off.

Evaluates every candidate extractor arm (both standalone and gated behind the
Stage-1 `EmbeddingGemma-300M` 768-d linear gate) end-to-end through `SocialGraphIndex`
against `data/kcd2/npcs.sqlite` on the held-out circle-disjoint `test` split (746 examples).

Metrics reported per arm:
  - Action Precision, Recall, F1, 4-way Action Accuracy
  - Overall Negative False Positive Rate (target < 1.0%)
  - Biographical 'Where' Trap False Positive Rate (`neg_kb_biographical_where`)
  - End-to-End Exact Resolution Accuracy (overall, command-only, and per-category across all 13 slices)
  - Abstention accuracy on `npc_absentee` and out-of-circle strangers
  - CPU wall-clock latency (p50/mean/p95 ms on non-command vs. command turns), RAM delta, and Disk MB
"""
from __future__ import annotations

import argparse
import collections
import json
import re
import statistics
import sys
from pathlib import Path
from typing import Any

import numpy as np

HERE = Path(__file__).resolve().parent.parent.parent
if str(HERE) not in sys.path:
    sys.path.insert(0, str(HERE))

from tools.command_bench.linker import (
    FEMALE_PRONOUNS,
    MALE_PRONOUNS,
    SocialGraphIndex,
    strip_determiners,
)
from tools.command_bench.schema import (
    BenchCategory,
    BenchmarkExample,
    CommandAction,
    ExtractedCommand,
    PipelineResult,
    ResolutionStatus,
    TargetKind,
    parse_compact_command,
)


def parse_fg_raw(raw: str) -> ExtractedCommand:
    m = re.search(
        r"call:(locate_npc|follow_npc|travel_to)\{.*?target_mention:(?:<escape>|\"|')?(.*?)(?:<escape>|\"|')?\}",
        raw,
        flags=re.DOTALL,
    )
    if m:
        act = CommandAction(m.group(1))
        span = m.group(2).strip()
        is_pron = strip_determiners(span) in (FEMALE_PRONOUNS | MALE_PRONOUNS | {"they", "them"})
        tk = TargetKind.PLACE if act == CommandAction.TRAVEL_TO else TargetKind.PERSON
        return ExtractedCommand(action=act, target_mention=span, target_kind=tk, is_pronoun=is_pron, raw_output=raw)
    return ExtractedCommand(action=CommandAction.NO_ACTION, raw_output=raw)


def is_exact_e2e(ex: BenchmarkExample, res: PipelineResult) -> bool:
    if res.action != ex.gold_action or res.status != ex.gold_status:
        return False
    if ex.gold_status == ResolutionStatus.RESOLVED_SOUL:
        return res.soul_name == ex.gold_soul_name
    if ex.gold_status == ResolutionStatus.RESOLVED_PLACE:
        return res.place_id == ex.gold_place_id
    return True


def evaluate_predictions(
    idx: SocialGraphIndex,
    test_examples: list[BenchmarkExample],
    pred_map: dict[str, ExtractedCommand],
    *,
    stage1_gate_mask: dict[str, bool] | None = None,
    cpu_timing_map: dict[str, float] | None = None,
    disk_mb: float = 0.0,
    ram_mb: float = 0.0,
    default_cmd_ms: float | None = None,
) -> dict[str, Any]:
    tp = fp = fn = tn = 0
    exact_action = 0
    bio_fp = bio_tot = 0
    cat_ok: dict[str, int] = collections.Counter()
    cat_tot: dict[str, int] = collections.Counter()
    cmd_latencies: list[float] = []
    noncmd_latencies: list[float] = []

    for ex in test_examples:
        eid = ex.example_id
        gated_out = False
        if stage1_gate_mask is not None and not stage1_gate_mask.get(eid, True):
            gated_out = True
            cmd = ExtractedCommand(action=CommandAction.NO_ACTION)
        else:
            cmd = pred_map.get(eid, ExtractedCommand(action=CommandAction.NO_ACTION))

        # Ensure pronoun flag is populated consistently
        if cmd.action != CommandAction.NO_ACTION and cmd.target_mention:
            if strip_determiners(cmd.target_mention) in (
                FEMALE_PRONOUNS | MALE_PRONOUNS | {"they", "them"}
            ):
                cmd.is_pronoun = True

        res = idx.resolve(
            ex.speaker_soul,
            cmd,
            history=ex.history,
            met_counts=ex.met_counts,
            known_tokens=ex.known_tokens,
            quest_souls=ex.quest_souls,
            kb_gated=ex.kb_gated,
        )

        # Timing accounting (Stage-1 dot product = 0.001 ms; linker = res.latency_ms)
        extractor_ms = 0.0 if gated_out else (
            cpu_timing_map[eid]
            if (cpu_timing_map and eid in cpu_timing_map)
            else (default_cmd_ms or 0.0)
        )
        turn_ms = 0.001 + extractor_ms + (res.latency_ms if cmd.action != CommandAction.NO_ACTION else 0.0)
        if ex.gold_action == CommandAction.NO_ACTION:
            noncmd_latencies.append(turn_ms)
        else:
            cmd_latencies.append(turn_ms)

        # Action classification metrics (before entity abstention, using `cmd.action`, except self-ref)
        pred_act = res.action if res.action == CommandAction.NO_ACTION else cmd.action
        if pred_act == ex.gold_action:
            exact_action += 1

        if ex.gold_action == CommandAction.NO_ACTION:
            if pred_act == CommandAction.NO_ACTION:
                tn += 1
            else:
                fp += 1
            if ex.category == BenchCategory.NEG_KB_BIOGRAPHICAL_WHERE:
                bio_tot += 1
                if pred_act != CommandAction.NO_ACTION:
                    bio_fp += 1
        else:
            if pred_act == ex.gold_action:
                tp += 1
            elif pred_act == CommandAction.NO_ACTION:
                fn += 1
            else:
                fp += 1

        cat = ex.category.value
        cat_tot[cat] += 1
        if is_exact_e2e(ex, res):
            cat_ok[cat] += 1

    prec = tp / max(tp + fp, 1)
    rec = tp / max(tp + fn, 1)
    f1 = 2 * prec * rec / max(prec + rec, 1e-9)
    neg_fpr = fp / max(tn + fp, 1)

    all_ok = sum(cat_ok.values())
    all_n = sum(cat_tot.values())
    cmd_cats = [c.value for c in BenchCategory if not c.value.startswith("neg_")]
    cmd_ok = sum(cat_ok[c] for c in cmd_cats)
    cmd_n = sum(cat_tot[c] for c in cmd_cats)

    abs_ok = cat_ok["abstain_absentee"] + cat_ok["abstain_stranger"]
    abs_n = cat_tot["abstain_absentee"] + cat_tot["abstain_stranger"]

    s_cmd = sorted(cmd_latencies) if cmd_latencies else [0.0]
    s_non = sorted(noncmd_latencies) if noncmd_latencies else [0.0]

    return {
        "action_4way_acc": round(exact_action / max(all_n, 1), 4),
        "action_precision": round(prec, 4),
        "action_recall": round(rec, 4),
        "action_f1": round(f1, 4),
        "neg_fpr": round(neg_fpr, 4),
        "biographical_where_fpr": round(bio_fp / max(bio_tot, 1), 4),
        "e2e_overall_acc": round(all_ok / max(all_n, 1), 4),
        "e2e_command_acc": round(cmd_ok / max(cmd_n, 1), 4),
        "abstention_acc": round(abs_ok / max(abs_n, 1), 4),
        "per_category_e2e_acc": {
            c: round(cat_ok[c] / cat_tot[c], 4) for c in sorted(cat_tot.keys())
        },
        "resources": {
            "added_disk_mb": round(disk_mb, 1),
            "added_ram_mb": round(ram_mb, 1),
            "noncmd_overhead_p50_ms": round(s_non[len(s_non) // 2], 3),
            "noncmd_overhead_mean_ms": round(statistics.mean(s_non), 3),
            "cmd_latency_p50_ms": round(s_cmd[len(s_cmd) // 2], 2),
            "cmd_latency_mean_ms": round(statistics.mean(s_cmd), 2),
            "cmd_latency_p95_ms": round(s_cmd[int(len(s_cmd) * 0.95)], 2),
        },
    }


def load_stage1_mask(
    dataset_path: Path, embeddings_path: Path, weights_path: Path
) -> dict[str, bool]:
    rows = [
        json.loads(l)
        for l in dataset_path.read_text(encoding="utf-8").splitlines()
        if l.strip()
    ]
    vecs = np.fromfile(embeddings_path, dtype=np.float32).reshape(len(rows), 768)
    w_obj = json.loads(weights_path.read_text(encoding="utf-8"))
    W = np.array(w_obj["weights"], dtype=np.float32)
    b = np.array(w_obj["bias"], dtype=np.float32)
    thresh = float(w_obj["recommended_cmd_threshold"])

    logits = vecs @ W.T + b
    logits = logits - np.max(logits, axis=1, keepdims=True)
    probs = np.exp(logits) / np.sum(np.exp(logits), axis=1, keepdims=True)
    p_cmd = 1.0 - probs[:, 0]

    return {rows[i]["example_id"]: bool(p_cmd[i] >= thresh) for i in range(len(rows))}


def load_cloud_preds(arm_entry: dict[str, Any]) -> dict[str, ExtractedCommand]:
    out: dict[str, ExtractedCommand] = {}
    for p in arm_entry.get("test_predictions", []):
        act_str = p.get("action", "no_action")
        try:
            act = CommandAction(act_str)
        except ValueError:
            act = CommandAction.NO_ACTION
        span = (p.get("target_mention") or "").strip()
        tk = (
            TargetKind.NONE
            if act == CommandAction.NO_ACTION
            else (TargetKind.PLACE if act == CommandAction.TRAVEL_TO else TargetKind.PERSON)
        )
        out[p["example_id"]] = ExtractedCommand(
            action=act, target_mention=span, target_kind=tk, raw_output=p.get("raw", "")
        )
    return out


def load_cpp_jsonl_preds(
    path: Path, fmt: str = "compact"
) -> tuple[dict[str, ExtractedCommand], dict[str, float], float]:
    preds: dict[str, ExtractedCommand] = {}
    timings: dict[str, float] = {}
    rss_deltas: list[float] = []
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line.strip():
            continue
        r = json.loads(line)
        eid = r["example_id"]
        raw = r.get("raw", "")
        preds[eid] = parse_fg_raw(raw) if fmt == "fg" else parse_compact_command(raw)
        timings[eid] = float(r.get("wall_ms", 0.0))
        if "rss_delta_mb" in r:
            rss_deltas.append(float(r["rss_delta_mb"]))
    avg_rss = statistics.mean(rss_deltas) if rss_deltas else 0.0
    return preds, timings, avg_rss


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--db", default="data/kcd2/npcs.sqlite")
    ap.add_argument("--dataset", default="build/command_bench/dataset_v1.jsonl")
    ap.add_argument("--embeddings", default="build/command_bench/embeddings_768.f32")
    ap.add_argument("--gate-weights", default="tools/command_bench/stage1_gate_weights.json")
    ap.add_argument("--arm-a", default="build/command_bench/arm_a_test_preds.jsonl")
    ap.add_argument("--arm-b-cloud", default="build/command_bench/arm_B_results.json")
    ap.add_argument("--arm-cd-cloud", default="build/command_bench/arm_CD_results.json")
    ap.add_argument("--sidecar-timings", default="build/command_bench/sidecar_cpu_summary.json")
    ap.add_argument("--out", default="tools/command_bench/bakeoff_results.json")
    args = ap.parse_args()

    idx = SocialGraphIndex(args.db)
    all_examples = [
        BenchmarkExample.from_dict(json.loads(l))
        for l in Path(args.dataset).read_text(encoding="utf-8").splitlines()
        if l.strip()
    ]
    test_examples = [ex for ex in all_examples if ex.split == "test"]
    for ex in test_examples:
        idx.candidates_for(ex.speaker_soul)

    gate_mask = load_stage1_mask(
        Path(args.dataset), Path(args.embeddings), Path(args.gate_weights)
    )

    sidecar_meta: dict[str, dict[str, float]] = {}
    if Path(args.sidecar_timings).exists():
        sidecar_meta = json.loads(Path(args.sidecar_timings).read_text(encoding="utf-8"))

    report: dict[str, Any] = {"test_examples": len(test_examples), "arms": {}}

    # Arm A (Qwen3-4B KV-Suffix + GBNF)
    if Path(args.arm_a).exists():
        p_a, t_a, _ = load_cpp_jsonl_preds(Path(args.arm_a), fmt="compact")
        report["arms"]["Arm_A_Qwen3_4B_KVSuffix_GBNF_Ungated"] = evaluate_predictions(
            idx, test_examples, p_a, cpu_timing_map=t_a, disk_mb=0.0, ram_mb=0.0
        )
        report["arms"]["Arm_A_Qwen3_4B_KVSuffix_GBNF_Gated"] = evaluate_predictions(
            idx,
            test_examples,
            p_a,
            stage1_gate_mask=gate_mask,
            cpu_timing_map=t_a,
            disk_mb=0.0,
            ram_mb=0.0,
        )

    # Arm B (FunctionGemma-270M Zero-Shot & LoRA-SFT)
    if Path(args.arm_b_cloud).exists():
        b_data = json.loads(Path(args.arm_b_cloud).read_text(encoding="utf-8"))
        fg_meta = sidecar_meta.get(
            "functiongemma_270m_q4_k_m",
            {"disk_mb": 253.1, "ram_mb": 275.0, "cmd_p50_ms": 55.0},
        )
        for key, label in [
            ("functiongemma_270m_zeroshot", "Arm_B_FunctionGemma_270M_ZeroShot"),
            ("functiongemma_270m_lora_sft", "Arm_B_FunctionGemma_270M_LoRA_SFT"),
        ]:
            if key in b_data:
                preds = load_cloud_preds(b_data[key])
                report["arms"][label] = evaluate_predictions(
                    idx,
                    test_examples,
                    preds,
                    disk_mb=fg_meta["disk_mb"],
                    ram_mb=fg_meta["ram_mb"],
                    default_cmd_ms=fg_meta["cmd_p50_ms"],
                )
                report["arms"][f"{label}_Gated"] = evaluate_predictions(
                    idx,
                    test_examples,
                    preds,
                    stage1_gate_mask=gate_mask,
                    disk_mb=fg_meta["disk_mb"],
                    ram_mb=fg_meta["ram_mb"],
                    default_cmd_ms=fg_meta["cmd_p50_ms"],
                )

    # Arm D local GLiNER2.5-small standalone + Stage-1 routed
    arm_d_local = Path("build/command_bench/arm_D_local_results.json")
    if arm_d_local.exists():
        d_local = json.loads(arm_d_local.read_text(encoding="utf-8"))
        d_ms = float(d_local["cpu_ms_p50"])
        for key, label in [
            ("gliner25_standalone", "Arm_D_GLiNER25_Small_74M_Standalone"),
            ("gliner25_routed", "Arm_D_Stage1Router_Plus_GLiNER25_Small_74M"),
        ]:
            preds = load_cloud_preds(d_local[key])
            report["arms"][label] = evaluate_predictions(
                idx,
                test_examples,
                preds,
                stage1_gate_mask=(gate_mask if "routed" in key else None),
                disk_mb=82.0,
                ram_mb=145.0,
                default_cmd_ms=d_ms,
            )

    # Arms C (Hammer2.1-0.5B, Qwen3-1.7B, Qwen3-0.6B Zero-Shot & LoRA-SFT)
    q17_ms = 195.0
    q17_rss = 1150.0
    if Path("build/command_bench/qwen3_17b_zs_q4_cpu_preds.jsonl").exists():
        _, q17_tims, q17_rss = load_cpp_jsonl_preds(
            Path("build/command_bench/qwen3_17b_zs_q4_cpu_preds.jsonl"), fmt="compact"
        )
        s_q17 = sorted(q17_tims.values())
        q17_ms = s_q17[len(s_q17) // 2]

    if Path(args.arm_cd_cloud).exists():
        cd_data = json.loads(Path(args.arm_cd_cloud).read_text(encoding="utf-8"))
        meta_defaults = {
            "hammer2_1_0_5b_zeroshot": (
                "Arm_C_Hammer2_1_0_5B_ZeroShot",
                sidecar_meta.get(
                    "hammer_0_5b_q4_k_m", {"disk_mb": 352.0, "ram_mb": 380.0, "cmd_p50_ms": 75.0}
                ),
            ),
            "qwen3_1_7b_zeroshot": (
                "Arm_C_Qwen3_1_7B_ZeroShot",
                {"disk_mb": 1051.0, "ram_mb": q17_rss, "cmd_p50_ms": q17_ms},
            ),
            "qwen3_0_6b_zeroshot": (
                "Arm_C_Qwen3_0_6B_ZeroShot",
                {"disk_mb": 378.0, "ram_mb": 642.0, "cmd_p50_ms": 115.0},
            ),
            "qwen3_0_6b_lora_sft": (
                "Arm_C_Qwen3_0_6B_LoRA_SFT_BF16",
                {"disk_mb": 378.0, "ram_mb": 642.0, "cmd_p50_ms": 115.0},
            ),
        }
        for key, (label, m) in meta_defaults.items():
            if key in cd_data:
                preds = load_cloud_preds(cd_data[key])
                report["arms"][label] = evaluate_predictions(
                    idx,
                    test_examples,
                    preds,
                    disk_mb=m["disk_mb"],
                    ram_mb=m["ram_mb"],
                    default_cmd_ms=m["cmd_p50_ms"],
                )
                report["arms"][f"{label}_Gated"] = evaluate_predictions(
                    idx,
                    test_examples,
                    preds,
                    stage1_gate_mask=gate_mask,
                    disk_mb=m["disk_mb"],
                    ram_mb=m["ram_mb"],
                    default_cmd_ms=m["cmd_p50_ms"],
                )

    # Quantized GGUF Sidecar C++ LlamaLlm CPU runs (all 746 test examples)
    for key, fname, fmt, disk_mb in [
        ("Arm_B_FunctionGemma_270M_LoRA_SFT_Q8_0_CPU", "build/command_bench/fg270m_sft_q8_cpu_preds.jsonl", "fg", 278.0),
        ("Arm_B_FunctionGemma_270M_LoRA_SFT_Q4_K_M_CPU", "build/command_bench/fg270m_sft_q4_cpu_preds.jsonl", "fg", 241.0),
        ("Arm_C_Qwen3_0_6B_LoRA_SFT_Q4_K_M_GBNF_CPU", "build/command_bench/qwen3_06b_sft_q4_cpu_preds.jsonl", "compact", 378.0),
        ("Arm_C_Qwen3_0_6B_LoRA_SFT_Q8_0_GBNF_CPU", "build/command_bench/qwen3_06b_sft_q8_cpu_preds.jsonl", "compact", 610.0),
    ]:
        p = Path(fname)
        if p.exists():
            preds, tims, rss = load_cpp_jsonl_preds(p, fmt=fmt)
            report["arms"][f"{key}_Ungated"] = evaluate_predictions(
                idx,
                test_examples,
                preds,
                cpu_timing_map=tims,
                disk_mb=disk_mb,
                ram_mb=rss,
            )
            report["arms"][f"{key}_Gated"] = evaluate_predictions(
                idx,
                test_examples,
                preds,
                stage1_gate_mask=gate_mask,
                cpu_timing_map=tims,
                disk_mb=disk_mb,
                ram_mb=rss,
            )

    Path(args.out).write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
