#!/usr/bin/env python3
"""Standalone benchmark & ablation suite for `SocialGraphIndex` (`tools/command_bench/linker.py`).

Evaluates entity-resolution accuracy and latency independently of NLU extraction errors:
  1. Oracle surface mentions across all 3,397 benchmark examples (stratified by category)
  2. Hop-1-only vs. Hop-1 + Hop-2 graph ablation
  3. Salience prior ON vs. OFF ablation (crucial for homonyms like two 'Jan's)
  4. Phonetic ASR corruption stress test (Central European -> English ASR spelling shifts)
     comparing orthographic-only vs. orthographic + phonetic matching
  5. Per-call resolution latency distribution (mean, p50, p95, p99 in milliseconds)
"""
from __future__ import annotations

import argparse
import collections
import json
import random
import statistics
import sys
import time
from pathlib import Path
from typing import Any

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
)


def corrupt_medieval_name_asr(mention: str, rng: random.Random) -> str:
    """Simulate realistic English-ASR phonetic misspellings of Bohemian/German names."""
    m_low = strip_determiners(mention)
    if m_low in (FEMALE_PRONOUNS | MALE_PRONOUNS | {"they", "them", "me", "henry"}):
        return mention
    # Skip pure role/trade phrases like "your brother" or "the innkeeper"
    if not any(ch.isupper() for ch in mention):
        return mention

    out = mention
    phonetic_rules = [
        ("Vej", "Wey"),
        ("vej", "wey"),
        ("Joerg", "Yorg"),
        ("Jorg", "Yorg"),
        ("Jörg", "Yorg"),
        ("Jan", "Yahn"),
        ("Jindrich", "Yindrik"),
        ("Bohuta", "Bohuda"),
        ("Frantisek", "Frantishek"),
        ("sch", "sh"),
        ("Sch", "Sh"),
        ("tz", "ts"),
        ("ck", "k"),
        ("dt", "t"),
        ("ü", "i"),
        ("ö", "e"),
        ("ä", "e"),
        ("č", "ch"),
        ("š", "sh"),
        ("ž", "zh"),
        ("ř", "rz"),
        ("W", "V"),
        ("v", "f"),
        ("th", "t"),
        ("ph", "f"),
        ("gh", "k"),
        ("ei", "ai"),
        ("ie", "ee"),
    ]
    applied = 0
    for src, dst in phonetic_rules:
        if src in out:
            out = out.replace(src, dst, 1)
            applied += 1
            if applied >= 2:
                break

    # Fallback: drop an interior unstressed vowel or swap voiced/voiceless stop
    if applied == 0 and len(out) >= 5:
        chars = list(out)
        for idx in range(2, len(chars) - 1):
            c = chars[idx]
            if c in "aeiou":
                chars.pop(idx)
                out = "".join(chars)
                applied += 1
                break
            if c in "bdg":
                chars[idx] = {"b": "p", "d": "t", "g": "k"}[c]
                out = "".join(chars)
                applied += 1
                break
    return out


def is_exact_match(ex: BenchmarkExample, res: PipelineResult) -> bool:
    if res.action != ex.gold_action or res.status != ex.gold_status:
        return False
    if ex.gold_status == ResolutionStatus.RESOLVED_SOUL:
        return res.soul_name == ex.gold_soul_name
    if ex.gold_status == ResolutionStatus.RESOLVED_PLACE:
        return res.place_id == ex.gold_place_id
    return True


def run_condition(
    idx: SocialGraphIndex,
    examples: list[BenchmarkExample],
    *,
    corrupt_asr: bool = False,
    use_phonetics: bool = True,
    use_salience: bool = True,
    hop1_only: bool = False,
    seed: int = 123,
) -> dict[str, Any]:
    rng = random.Random(seed)
    by_cat_ok: dict[str, int] = collections.Counter()
    by_cat_tot: dict[str, int] = collections.Counter()
    latencies_ms: list[float] = []

    for ex in examples:
        mention = ex.gold_surface_target
        if corrupt_asr and ex.gold_action != CommandAction.NO_ACTION and mention:
            mention = corrupt_medieval_name_asr(mention, rng)

        is_pron = strip_determiners(mention) in (
            FEMALE_PRONOUNS | MALE_PRONOUNS | {"they", "them"}
        )
        t_kind = (
            TargetKind.NONE
            if ex.gold_action == CommandAction.NO_ACTION
            else (
                TargetKind.PLACE
                if ex.gold_action == CommandAction.TRAVEL_TO
                else TargetKind.PERSON
            )
        )
        cmd = ExtractedCommand(
            action=ex.gold_action,
            target_mention=mention if ex.gold_action != CommandAction.NO_ACTION else "",
            target_kind=t_kind,
            is_pronoun=is_pron,
        )
        t0 = time.perf_counter()
        res = idx.resolve(
            ex.speaker_soul,
            cmd,
            history=ex.history,
            met_counts=ex.met_counts,
            known_tokens=ex.known_tokens,
            quest_souls=ex.quest_souls,
            kb_gated=ex.kb_gated,
            use_phonetics=use_phonetics,
            use_salience=use_salience,
            hop1_only=hop1_only,
        )
        latencies_ms.append((time.perf_counter() - t0) * 1000.0)
        cat = ex.category.value
        by_cat_tot[cat] += 1
        if is_exact_match(ex, res):
            by_cat_ok[cat] += 1

    total_ok = sum(by_cat_ok.values())
    total_n = sum(by_cat_tot.values())
    cmd_cats = [c.value for c in BenchCategory if not c.value.startswith("neg_")]
    cmd_ok = sum(by_cat_ok[c] for c in cmd_cats)
    cmd_n = sum(by_cat_tot[c] for c in cmd_cats)

    sorted_lat = sorted(latencies_ms)
    return {
        "overall_accuracy": round(total_ok / max(total_n, 1), 4),
        "command_only_accuracy": round(cmd_ok / max(cmd_n, 1), 4),
        "per_category_accuracy": {
            c: round(by_cat_ok[c] / by_cat_tot[c], 4)
            for c in sorted(by_cat_tot.keys())
        },
        "latency_ms": {
            "mean": round(statistics.mean(sorted_lat), 4),
            "p50": round(sorted_lat[len(sorted_lat) // 2], 4),
            "p95": round(sorted_lat[int(len(sorted_lat) * 0.95)], 4),
            "p99": round(sorted_lat[int(len(sorted_lat) * 0.99)], 4),
        },
    }


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--db", default="data/kcd2/npcs.sqlite")
    ap.add_argument("--dataset", default="build/command_bench/dataset_v1.jsonl")
    ap.add_argument("--out", default="tools/command_bench/linker_bench_results.json")
    args = ap.parse_args()

    t_load = time.perf_counter()
    idx = SocialGraphIndex(args.db)
    load_ms = (time.perf_counter() - t_load) * 1000.0

    examples: list[BenchmarkExample] = []
    with Path(args.dataset).open("r", encoding="utf-8") as f:
        for line in f:
            if line.strip():
                examples.append(BenchmarkExample.from_dict(json.loads(line)))

    # Warm candidate cache across benchmark speakers so steady-state latency reflects Session cache
    for ex in examples:
        idx.candidates_for(ex.speaker_soul)

    cand_sizes_h1 = []
    cand_sizes_all = []
    seen_speakers = sorted({ex.speaker_soul for ex in examples})
    for sp in seen_speakers:
        cs = idx.candidates_for(sp)
        cand_sizes_all.append(len(cs))
        cand_sizes_h1.append(sum(1 for c in cs if c.hop == 1))

    results = {
        "num_examples": len(examples),
        "num_unique_speakers": len(seen_speakers),
        "db_load_ms": round(load_ms, 1),
        "avg_hop1_candidates": round(statistics.mean(cand_sizes_h1), 2),
        "avg_total_hop1_hop2_candidates": round(statistics.mean(cand_sizes_all), 2),
        "conditions": {
            "full_linker_clean": run_condition(
                idx, examples, corrupt_asr=False, use_phonetics=True, use_salience=True
            ),
            "ablation_hop1_only": run_condition(
                idx, examples, corrupt_asr=False, use_phonetics=True, use_salience=True, hop1_only=True
            ),
            "ablation_no_salience_prior": run_condition(
                idx, examples, corrupt_asr=False, use_phonetics=True, use_salience=False
            ),
            "asr_corrupted_with_phonetics": run_condition(
                idx, examples, corrupt_asr=True, use_phonetics=True, use_salience=True
            ),
            "asr_corrupted_ortho_only": run_condition(
                idx, examples, corrupt_asr=True, use_phonetics=False, use_salience=True
            ),
        },
    }

    Path(args.out).write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(results, indent=2))


if __name__ == "__main__":
    main()
