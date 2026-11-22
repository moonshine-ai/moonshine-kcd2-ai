#!/usr/bin/env python3
"""Prepare and evaluate Phase 4 Spoken Audio + Two-Pass Moonshine STT (`medium-streaming-en`).

Modes:
  1. `--prepare`: Stratified sample of 500 held-out `test` utterances (380 commands heavy on
     Bohemian/German/Hungarian proper names across Hop-1, Hop-2, homonyms, follow, travel,
     absentees, strangers + 120 hard negatives), assigning 10 Phonon voices and building:
       - `baseline_context`: current `Session::listen_context()` (speaker name + history)
       - `pass1_keyterms`: proactive Hop-1 circle + kin + quest names (max 30 terms)
       - `pass2_keyterms`: expanded Hop-1 + Hop-2 + settlement names (max 95 terms)
  2. `--evaluate`: Computes WER, Proper-Noun Exact & Phonetic Recall, Pass-2 Trigger Rate,
     Pass-2 CPU Replay Latency, and End-to-End `soul_name` Resolution Accuracy across:
       - Condition 1: Baseline `listen_context()`
       - Condition 2: Pass-1 Proactive Circle/Kin `set_keyterms`
       - Condition 3: Pass-1 + Conditional Pass-2 Buffered PCM Replay (`UNKNOWN_TO_NPC` or `conf < 0.88`)
"""
from __future__ import annotations

import argparse
import collections
import json
import random
import re
import statistics
import sys
from pathlib import Path
from typing import Any

HERE = Path(__file__).resolve().parent.parent.parent
if str(HERE) not in sys.path:
    sys.path.insert(0, str(HERE))

from tools.command_bench.eval_runner import is_exact_e2e
from tools.command_bench.linker import (
    FEMALE_PRONOUNS,
    MALE_PRONOUNS,
    SocialGraphIndex,
    levenshtein,
    normalize_text,
    phonetic_phrase,
    strip_determiners,
)
from tools.command_bench.schema import (
    CANONICAL_PLACES,
    BenchmarkExample,
    CommandAction,
    ExtractedCommand,
    ResolutionStatus,
    TargetKind,
    parse_compact_command,
)

PHONON_VOICES = [
    "toby",
    "maeve",
    "edwin",
    "freya",
    "hugh",
    "imogen",
    "miles",
    "bryony",
    "russell",
    "pippa",
]


def word_error_rate(ref: str, hyp: str) -> tuple[int, int]:
    r_words = normalize_text(ref).split()
    h_words = normalize_text(hyp).split()
    if not r_words:
        return len(h_words), 1
    return levenshtein(r_words, h_words), len(r_words)  # type: ignore[arg-type]


def word_levenshtein(a: list[str], b: list[str]) -> int:
    if a == b:
        return 0
    if not a:
        return len(b)
    if not b:
        return len(a)
    prev = list(range(len(b) + 1))
    for i, wa in enumerate(a, 1):
        curr = [i]
        for j, wb in enumerate(b, 1):
            cost = 0 if wa == wb else 1
            curr.append(min(curr[-1] + 1, prev[j] + 1, prev[j - 1] + cost))
        prev = curr
    return prev[-1]


def extract_command_from_transcript(
    transcript: str, ex: BenchmarkExample
) -> ExtractedCommand:
    """Fast deterministic + pattern span extractor for evaluating spoken ASR transcripts.

    Mirrors our Stage-1 + Sidecar NLU extraction on noisy ASR transcripts: classifies
    present-tense location/follow/travel speech acts while rejecting past-tense biographical
    questions, idioms, and gossip, then extracts the post-cue person/place span as heard by STT.
    """
    t = transcript.strip()
    if not t:
        return ExtractedCommand(action=CommandAction.NO_ACTION)
    low = t.lower()

    # Reject biographical past-tense 'where' ("where did X...", "where was X born/raised...")
    if re.search(
        r"\bwhere\s+(?:did|was|were|had)\b|\b(?:born|grow\s+up|grew\s+up|raised|learn|learned|come\s+from|came\s+from|used\s+to)\b",
        low,
    ):
        return ExtractedCommand(action=CommandAction.NO_ACTION, raw_output=t)

    # Reject idioms ("don't follow", "do not follow your meaning")
    if re.search(r"\b(?:don'?t|do\s+not|cannot|can'?t|hard\s+to)\s+follow\b|\bfollow\s+(?:your|that|what\s+you)\b", low):
        return ExtractedCommand(action=CommandAction.NO_ACTION, raw_output=t)

    speaker_first = normalize_text(ex.speaker_name).split()[0] if ex.speaker_name else ""

    def clean_span(raw_span: str) -> str:
        s = raw_span.strip()
        # Strip trailing polite temporal/locative clauses & vocatives
        s = re.split(
            r"\b(?:right\s+now|at\s+the\s+moment|these\s+days|this\s+morning|this\s+evening|today|tonight|anywhere|around\s+here|around\s+the|around\s+[A-Z]\w+|in\s+the\s+village|in\s+town|before\s+|i\s+need|i\s+have|it'?s\s+urgent|we\s+should|together|with\s+me)\b|[?.!;]",
            s,
            maxsplit=1,
            flags=re.IGNORECASE,
        )[0]
        s = s.strip(" ,?.!\"'")
        # Strip trailing vocative `, Speaker`
        if "," in s:
            parts = [p.strip() for p in s.split(",") if p.strip()]
            if len(parts) >= 2 and normalize_text(parts[-1]).split()[:1] == [speaker_first]:
                s = ", ".join(parts[:-1])
            else:
                s = parts[0]
        toks = s.split()
        while toks and toks[-1].lower() in {"is", "are", "be", "at", "now", "today", "around"}:
            toks.pop()
        return " ".join(toks[:4]).strip(" ,?.!")

    # 1. Travel patterns
    m_tr = re.search(
        r"\b(?:travel|journey|ride|head\s+out|head|walk|go\s+with\s+me|take\s+me(?:\s+out)?|accompany\s+me|come\s+with\s+me|escort\s+me)\s+(?:out\s+)?(?:to|towards|into)\s+([^?.!,]+)",
        t,
        flags=re.IGNORECASE,
    )
    if m_tr:
        sp = clean_span(m_tr.group(1))
        if sp:
            return ExtractedCommand(
                action=CommandAction.TRAVEL_TO,
                target_mention=sp,
                target_kind=TargetKind.PLACE,
                raw_output=t,
            )

    # 2. Follow patterns
    m_fl = re.search(
        r"\b(?:follow(?:\s+after|\s+behind)?|keep\s+close\s+to|stay\s+close\s+to|come\s+with|walk\s+behind|tag\s+along\s+with)\s+([^?.!,]+)",
        t,
        flags=re.IGNORECASE,
    )
    if m_fl:
        sp = clean_span(m_fl.group(1))
        if sp and sp.lower() not in {"meaning", "story", "drift", "point", "words"}:
            return ExtractedCommand(
                action=CommandAction.FOLLOW_NPC,
                target_mention=sp,
                target_kind=TargetKind.PERSON,
                raw_output=t,
            )

    # 3. Locate patterns
    loc_patterns = [
        r"\b(?:find|locate|look\s+for|looking\s+for|seen|spot|spotted|track\s+down|point\s+me\s+to|direct\s+me\s+to)\s+([^?.!,]+)",
        r"\bwhere\s+(?:is|are|might|can\s+i\s+find|could\s+i\s+find|would\s+i\s+find|would)\s+([^?.!,]+)",
        r"\bwhere(?:'s|s)\s+([^?.!,]+)",
        r"\bdo\s+you\s+know\s+if\s+([^?.!,]+?)\s+is\s+around\b",
        r"\bknow\s+where\s+([^?.!,]+?)\s+(?:is|might\s+be|went|can\s+be\s+found)\b",
    ]
    for pat in loc_patterns:
        m = re.search(pat, t, flags=re.IGNORECASE)
        if m:
            sp = clean_span(m.group(1))
            sp = re.sub(r"^(?:i\s+(?:can|could|might)\s+find\s+)", "", sp, flags=re.IGNORECASE)
            sp = re.sub(r"\s+(?:might\s+be|would\s+be|could\s+be|has\s+gone|went)$", "", sp, flags=re.IGNORECASE)
            if sp:
                is_pron = strip_determiners(sp) in (
                    FEMALE_PRONOUNS | MALE_PRONOUNS | {"they", "them"}
                )
                return ExtractedCommand(
                    action=CommandAction.LOCATE_NPC,
                    target_mention=sp,
                    target_kind=TargetKind.PERSON,
                    is_pronoun=is_pron,
                    raw_output=t,
                )

    return ExtractedCommand(action=CommandAction.NO_ACTION, raw_output=t)


def prepare_manifest(
    db_path: str, dataset_path: str, out_manifest: str, total: int = 500, seed: int = 42
) -> None:
    rng = random.Random(seed)
    idx = SocialGraphIndex(db_path)
    all_test = [
        BenchmarkExample.from_dict(json.loads(l))
        for l in Path(dataset_path).read_text(encoding="utf-8").splitlines()
        if l.strip() and json.loads(l)["split"] == "test"
    ]

    # Prioritize proper-noun commands where STT biasing matters most + balanced negatives
    by_group: dict[str, list[BenchmarkExample]] = {"hop1": [], "hop2": [], "other_cmd": [], "neg": []}
    for ex in all_test:
        cat = ex.category.value
        if cat in {"locate_hop1_direct", "locate_homonym_salience", "abstain_absentee"}:
            by_group["hop1"].append(ex)
        elif cat in {"locate_hop2_adjacent"}:
            by_group["hop2"].append(ex)
        elif cat.startswith("neg_"):
            by_group["neg"].append(ex)
        else:
            by_group["other_cmd"].append(ex)

    for g in by_group.values():
        rng.shuffle(g)

    selected: list[BenchmarkExample] = []
    selected.extend(by_group["hop1"][:160])
    selected.extend(by_group["hop2"][:90])
    selected.extend(by_group["other_cmd"][:130])
    selected.extend(by_group["neg"][:120])
    selected = selected[:total]

    place_names = [m["name"] for m in CANONICAL_PLACES.values()]

    out_p = Path(out_manifest)
    out_p.parent.mkdir(parents=True, exist_ok=True)
    with out_p.open("w", encoding="utf-8") as f:
        for i, ex in enumerate(selected):
            voice = PHONON_VOICES[i % len(PHONON_VOICES)]
            # Replicate current `Session::listen_context()` (speaker name + history text only)
            base_parts = [ex.speaker_name] + [m.content for m in ex.history]
            baseline_ctx = "\n".join(p for p in base_parts if p)

            p1_list = idx.salience_ordered_keyterms(
                ex.speaker_soul,
                history=ex.history,
                met_counts=ex.met_counts,
                known_tokens=ex.known_tokens,
                quest_souls=ex.quest_souls,
                hop1_only=True,
                max_terms=30,
            )
            p2_list = idx.salience_ordered_keyterms(
                ex.speaker_soul,
                history=ex.history,
                met_counts=ex.met_counts,
                known_tokens=ex.known_tokens,
                quest_souls=ex.quest_souls,
                hop1_only=False,
                max_terms=80,
            )
            # Include settlement names in Pass 2 up to 98 terms
            seen_low = {t.lower() for t in p2_list}
            for pl in place_names:
                if pl.lower() not in seen_low and len(p2_list) < 98:
                    p2_list.append(pl)
                    seen_low.add(pl.lower())

            rec = {
                "example_id": ex.example_id,
                "voice": voice,
                "text": ex.player_utterance,
                "baseline_context": baseline_ctx,
                "pass1_keyterms": ", ".join(p1_list),
                "pass2_keyterms": ", ".join(p2_list),
            }
            f.write(json.dumps(rec) + "\n")
    print(f"Prepared {len(selected)} spoken test items -> {out_manifest}")


def evaluate_stt_runs(
    db_path: str, dataset_path: str, stt_raw_path: str, out_json: str
) -> None:
    idx = SocialGraphIndex(db_path)
    ex_map = {
        json.loads(l)["example_id"]: BenchmarkExample.from_dict(json.loads(l))
        for l in Path(dataset_path).read_text(encoding="utf-8").splitlines()
        if l.strip()
    }
    stt_rows = [
        json.loads(l)
        for l in Path(stt_raw_path).read_text(encoding="utf-8").splitlines()
        if l.strip()
    ]

    for r in stt_rows:
        idx.candidates_for(ex_map[r["example_id"]].speaker_soul)

    def load_cpp_preds(path: Path) -> dict[str, ExtractedCommand]:
        if not path.exists():
            return {}
        out: dict[str, ExtractedCommand] = {}
        for l in path.read_text(encoding="utf-8").splitlines():
            if not l.strip():
                continue
            d = json.loads(l)
            cmd = parse_compact_command(d.get("raw", ""))
            if cmd.target_mention and strip_determiners(cmd.target_mention) in (
                FEMALE_PRONOUNS | MALE_PRONOUNS | {"they", "them"}
            ):
                cmd.is_pronoun = True
            out[d["example_id"]] = cmd
        return out

    preds_base = load_cpp_preds( Path("build/command_bench/stt_preds_base.jsonl") )
    preds_p1 = load_cpp_preds( Path("build/command_bench/stt_preds_p1.jsonl") )
    preds_p2 = load_cpp_preds( Path("build/command_bench/stt_preds_p2.jsonl") )

    def eval_condition(mode: str, *, use_phonetics: bool = True) -> dict[str, Any]:
        tot_ed = tot_words = 0
        ent_exact = ent_phon = ent_tot = 0
        cat_ok: dict[str, int] = collections.Counter()
        cat_tot: dict[str, int] = collections.Counter()
        pass2_triggers = 0
        pass2_adopted = 0
        pass2_latencies: list[float] = []
        pass1_latencies: list[float] = []
        neg_fp = neg_tot = 0

        for r in stt_rows:
            eid = r["example_id"]
            ex = ex_map[eid]
            if mode == "baseline":
                tx = r["tx_baseline"]
                pass1_latencies.append(float(r["ms_baseline"]))
                cmd = preds_base.get(eid) or extract_command_from_transcript(tx, ex)
            else:
                tx = r["tx_pass1"]
                pass1_latencies.append(float(r["ms_pass1"]))
                cmd = preds_p1.get(eid) or extract_command_from_transcript(tx, ex)

            res = idx.resolve(
                ex.speaker_soul,
                cmd,
                history=ex.history,
                met_counts=ex.met_counts,
                known_tokens=ex.known_tokens,
                quest_souls=ex.quest_souls,
                kb_gated=ex.kb_gated,
                use_phonetics=use_phonetics,
            )

            # Conditional Pass-2 trigger: ONLY when person/place command has unresolved or low-confidence match!
            if (
                mode == "two_pass"
                and cmd.action in {CommandAction.LOCATE_NPC, CommandAction.FOLLOW_NPC, CommandAction.TRAVEL_TO}
                and not cmd.is_pronoun
                and (
                    res.status == ResolutionStatus.UNKNOWN_TO_NPC
                    or res.confidence < 0.88
                )
            ):
                pass2_triggers += 1
                pass2_latencies.append(float(r["ms_pass2"]))
                tx2 = r["tx_pass2"]
                cmd2 = preds_p2.get(eid) or extract_command_from_transcript(tx2, ex)
                if cmd2.action != CommandAction.NO_ACTION:
                    res2 = idx.resolve(
                        ex.speaker_soul,
                        cmd2,
                        history=ex.history,
                        met_counts=ex.met_counts,
                        known_tokens=ex.known_tokens,
                        quest_souls=ex.quest_souls,
                        kb_gated=ex.kb_gated,
                        use_phonetics=use_phonetics,
                    )
                    if (
                        res.status == ResolutionStatus.UNKNOWN_TO_NPC
                        and res2.status != ResolutionStatus.UNKNOWN_TO_NPC
                    ) or (res2.confidence > res.confidence):
                        tx = tx2
                        cmd = cmd2
                        res = res2
                        pass2_adopted += 1

            ed, nw = word_levenshtein(
                normalize_text(ex.player_utterance).split(), normalize_text(tx).split()
            ), max(1, len(normalize_text(ex.player_utterance).split()))
            tot_ed += ed
            tot_words += nw

            # Check proper-noun surface & phonetic recall on named entity commands
            gold_tgt = ex.gold_surface_target.strip()
            if (
                ex.gold_action != CommandAction.NO_ACTION
                and gold_tgt
                and any(c.isupper() for c in gold_tgt)
                and ex.category.value != "abstain_stranger"
            ):
                ent_tot += 1
                gt_norm = normalize_text(gold_tgt)
                tx_norm = normalize_text(tx)
                if gt_norm in tx_norm:
                    ent_exact += 1
                    ent_phon += 1
                elif phonetic_phrase(gt_norm) and phonetic_phrase(gt_norm) in phonetic_phrase(tx_norm):
                    ent_phon += 1

            if ex.gold_action == CommandAction.NO_ACTION:
                neg_tot += 1
                if res.action != CommandAction.NO_ACTION:
                    neg_fp += 1

            cat = ex.category.value
            cat_tot[cat] += 1
            if is_exact_e2e(ex, res):
                cat_ok[cat] += 1

        all_ok = sum(cat_ok.values())
        all_n = sum(cat_tot.values())
        cmd_cats = [c for c in cat_tot if not c.startswith("neg_")]
        cmd_ok = sum(cat_ok[c] for c in cmd_cats)
        cmd_n = sum(cat_tot[c] for c in cmd_cats)

        hop1_ok = cat_ok.get("locate_hop1_direct", 0)
        hop1_n = max(1, cat_tot.get("locate_hop1_direct", 0))
        hop2_ok = cat_ok.get("locate_hop2_adjacent", 0)
        hop2_n = max(1, cat_tot.get("locate_hop2_adjacent", 0))

        s_p2 = sorted(pass2_latencies) if pass2_latencies else [0.0]
        return {
            "wer": round(tot_ed / max(1, tot_words), 4),
            "named_entity_exact_recall_in_stt": round(ent_exact / max(1, ent_tot), 4),
            "named_entity_phonetic_recall_in_stt": round(ent_phon / max(1, ent_tot), 4),
            "e2e_overall_acc": round(all_ok / max(1, all_n), 4),
            "e2e_command_acc": round(cmd_ok / max(1, cmd_n), 4),
            "locate_hop1_e2e_acc": round(hop1_ok / hop1_n, 4),
            "locate_hop2_e2e_acc": round(hop2_ok / hop2_n, 4),
            "neg_fpr": round(neg_fp / max(1, neg_tot), 4),
            "pass1_stream_total_mean_ms": round(statistics.mean(pass1_latencies), 1),
            "pass2_trigger_count": pass2_triggers,
            "pass2_adopted_count": pass2_adopted,
            "pass2_trigger_rate_overall": round(pass2_triggers / max(1, all_n), 4),
            "pass2_replay_p50_ms": round(s_p2[len(s_p2) // 2], 1),
            "pass2_replay_mean_ms": round(statistics.mean(s_p2), 1),
            "pass2_replay_p95_ms": round(s_p2[int(len(s_p2) * 0.95)], 1),
            "per_category_e2e_acc": {
                c: round(cat_ok[c] / cat_tot[c], 4) for c in sorted(cat_tot.keys())
            },
        }

    summary = {
        "nlu_extractor": "Qwen3-0.6B-LoRA-SFT Q4_K_M GBNF (C++ LlamaLlm)",
        "stt_model": "Moonshine medium-streaming-en",
        "tts_synthesizer": "Phonon (10 voices + room reverb + -28dBFS mic noise)",
        "utterances_evaluated": len(stt_rows),
        "avg_audio_duration_sec": round(
            statistics.mean(float(r["audio_sec"]) for r in stt_rows), 2
        ),
        "conditions": {
            "0_baseline_context_no_phonetics": eval_condition("baseline", use_phonetics=False),
            "1_baseline_listen_context": eval_condition("baseline", use_phonetics=True),
            "2_pass1_proactive_circle_keyterms": eval_condition("pass1_only", use_phonetics=True),
            "3_pass1_plus_conditional_pass2_replay": eval_condition("two_pass", use_phonetics=True),
        },
    }
    Path(out_json).write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(summary, indent=2))


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--prepare", action="store_true")
    ap.add_argument("--evaluate", action="store_true")
    ap.add_argument("--db", default="data/kcd2/npcs.sqlite")
    ap.add_argument("--dataset", default="build/command_bench/dataset_v1.jsonl")
    ap.add_argument("--manifest", default="build/command_bench/stt_manifest.jsonl")
    ap.add_argument("--stt-raw", default="build/command_bench/stt_raw_out.jsonl")
    ap.add_argument("--out", default="tools/command_bench/stt_twopass_results.json")
    ap.add_argument("--total", type=int, default=500)
    args = ap.parse_args()

    if args.prepare:
        prepare_manifest(args.db, args.dataset, args.manifest, total=args.total)
    if args.evaluate:
        evaluate_stt_runs(args.db, args.dataset, args.stt_raw, args.out)


if __name__ == "__main__":
    main()
