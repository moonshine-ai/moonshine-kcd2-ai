# /// script
# requires-python = ">=3.11"
# dependencies = [
#   "torch>=2.4.0",
#   "gliner2[local]",
#   "numpy>=1.26.0",
# ]
# ///
"""Evaluate Arm D (`fastino/gliner2.5-small-v1`, 74M) standalone and paired with
the Stage-1 `EmbeddingGemma-300M` 4-way linear intent router on local CPU.
"""
from __future__ import annotations

import json
import re
import statistics
import time
from pathlib import Path

import numpy as np
import torch
from gliner2 import AutoExtractor


def main() -> None:
    ds_path = Path("build/command_bench/dataset_v1.jsonl")
    emb_path = Path("build/command_bench/embeddings_768.f32")
    w_path = Path("tools/command_bench/stage1_gate_weights.json")

    all_rows = [
        json.loads(l)
        for l in ds_path.read_text(encoding="utf-8").splitlines()
        if l.strip()
    ]
    vecs = np.fromfile(emb_path, dtype=np.float32).reshape(len(all_rows), 768)
    w_obj = json.loads(w_path.read_text(encoding="utf-8"))
    classes = w_obj["classes"]
    W = np.array(w_obj["weights"], dtype=np.float32)
    b = np.array(w_obj["bias"], dtype=np.float32)
    logits = vecs @ W.T + b
    router_preds = [classes[int(i)] for i in np.argmax(logits, axis=1)]

    test_pairs = [
        (r, router_preds[idx])
        for idx, r in enumerate(all_rows)
        if r["split"] == "test"
    ]

    torch.set_num_threads(8)
    t_load = time.perf_counter()
    ext = AutoExtractor.from_pretrained("fastino/gliner2.5-small-v1", map_location="cpu")
    load_sec = time.perf_counter() - t_load

    schema = (
        ext.create_schema()
        .entities(
            {
                "person_target": "Person name, relative ('your brother'), or trade ('the blacksmith', 'the peasant') being looked for or followed",
                "place_target": "Town, village, castle, mines, or landmark destination to travel to",
            }
        )
        .classification(
            "intent",
            [
                "ask_where_person_is_right_now",
                "command_to_follow_person",
                "command_to_travel_to_place",
                "normal_conversation_or_past_biographical_question",
            ],
        )
    )
    label_map = {
        "ask_where_person_is_right_now": "locate_npc",
        "command_to_follow_person": "follow_npc",
        "command_to_travel_to_place": "travel_to",
        "normal_conversation_or_past_biographical_question": "no_action",
    }

    # Warmup 3 calls
    for r, _ in test_pairs[:3]:
        _ = ext.extract(r["player_utterance"], schema, include_confidence=True)

    standalone_preds = []
    routed_preds = []
    cpu_ms_list = []

    for r, routed_act in test_pairs:
        utt = r["player_utterance"]
        speaker_low = (r.get("speaker_name") or "").strip().lower()
        speaker_first = speaker_low.split()[0] if speaker_low else ""

        t0 = time.perf_counter()
        res = ext.extract(utt, schema, include_confidence=True)
        cpu_ms = (time.perf_counter() - t0) * 1000.0
        cpu_ms_list.append(cpu_ms)

        raw_cls = res.get("intent", "")
        if isinstance(raw_cls, dict):
            raw_cls = raw_cls.get("label") or raw_cls.get("value") or ""
        gliner_act = label_map.get(str(raw_cls), "no_action")

        ents = res.get("entities", {})
        p_spans = [
            (e["text"] if isinstance(e, dict) else str(e)).strip()
            for e in ents.get("person_target", [])
        ]
        loc_spans = [
            (e["text"] if isinstance(e, dict) else str(e)).strip()
            for e in ents.get("place_target", [])
        ]

        # Filter vocative addressee (the NPC speaker Henry is addressing!)
        filtered_p = [
            s
            for s in p_spans
            if s.lower() != speaker_low and s.lower() != speaker_first
        ]

        def pick_mention(act: str) -> str:
            if act == "no_action":
                return ""
            if act == "travel_to":
                if loc_spans:
                    return loc_spans[0]
                if filtered_p:
                    return filtered_p[0]
                return ""
            # locate_npc or follow_npc
            if filtered_p:
                return filtered_p[0]
            m_pron = re.search(
                r"\b(him|her|he|she|they|them|me)\b", utt, flags=re.IGNORECASE
            )
            if m_pron:
                return m_pron.group(1)
            if p_spans:
                return p_spans[0]
            return ""

        m_stand = pick_mention(gliner_act)
        standalone_preds.append(
            {
                "example_id": r["example_id"],
                "action": gliner_act if m_stand else "no_action",
                "target_mention": m_stand,
                "raw": json.dumps(res)[:160],
            }
        )

        m_rout = pick_mention(routed_act)
        routed_preds.append(
            {
                "example_id": r["example_id"],
                "action": routed_act if m_rout else "no_action",
                "target_mention": m_rout,
                "raw": json.dumps(res)[:160],
            }
        )

    cpu_sorted = sorted(cpu_ms_list)
    out = {
        "load_sec": round(load_sec, 2),
        "cpu_ms_mean": round(statistics.mean(cpu_sorted), 2),
        "cpu_ms_p50": round(cpu_sorted[len(cpu_sorted) // 2], 2),
        "cpu_ms_p95": round(cpu_sorted[int(len(cpu_sorted) * 0.95)], 2),
        "gliner25_standalone": {"test_predictions": standalone_preds},
        "gliner25_routed": {"test_predictions": routed_preds},
    }
    Path("build/command_bench/arm_D_local_results.json").write_text(
        json.dumps(out, indent=2), encoding="utf-8"
    )
    print(
        json.dumps(
            {k: v for k, v in out.items() if not isinstance(v, dict)}, indent=2
        )
    )


if __name__ == "__main__":
    main()
