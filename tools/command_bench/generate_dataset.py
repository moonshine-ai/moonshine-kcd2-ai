#!/usr/bin/env python3
"""Cloud-parallel synthetic benchmark generator grounded in `data/kcd2/npcs.sqlite`.

Samples ~220 real social circles across Kuttenberg and Trosky, constructs deterministic
social-graph scenarios (Hop-1, Hop-2, relational/trade, multi-turn pronoun coreference,
homonym salience disambiguation, follow/travel commands, circle absentees, strangers,
and 4 families of adversarial hard negatives), and calls Claude (`claude-haiku-5-5`)
in parallel batches to author varied, natural Henry-of-Skalitz player utterances.

Outputs:
  - `build/command_bench/dataset_v1.jsonl` (~3,200+ circle-disjoint train/dev/test rows)
  - `tools/command_bench/dataset_stats.json` (counts by split, action, status, category)
"""
from __future__ import annotations

import argparse
import collections
import concurrent.futures
import json
import os
import random
import re
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any

HERE = Path(__file__).resolve().parent.parent.parent
if str(HERE) not in sys.path:
    sys.path.insert(0, str(HERE))

from tools.command_bench.linker import CandidatePerson, SocialGraphIndex, normalize_text
from tools.command_bench.schema import (
    CANONICAL_PLACES,
    BenchCategory,
    BenchmarkExample,
    CommandAction,
    MessageTurn,
    ResolutionStatus,
    assign_circle_split,
)

PRONOUN_WORDS = {"he", "him", "she", "her", "they", "them"}


def build_blueprints(idx: SocialGraphIndex, seed: int = 42, target_total: int = 3300) -> list[dict[str, Any]]:
    """Construct deterministic ground-truth scenario blueprints from `npcs.sqlite`."""
    rng = random.Random(seed)
    all_circles = sorted(idx.circle_members.keys())
    rng.shuffle(all_circles)

    # All named human souls by settlement for sampling true strangers
    all_souls = [
        s for s in idx.souls.values()
        if s.given and len(s.given) >= 4 and s.full_name and not s.full_name[0].islower()
    ]
    place_items = list(CANONICAL_PLACES.items())

    blueprints: list[dict[str, Any]] = []
    bp_idx = 0

    def add_bp(**kw: Any) -> None:
        nonlocal bp_idx
        bp_idx += 1
        kw["bp_id"] = f"ex_{bp_idx:05d}"
        blueprints.append(kw)

    for cid in all_circles:
        members = [m for m in idx.circle_members[cid] if m in idx.souls]
        if not members:
            continue
        split = assign_circle_split(cid)
        # Pick up to 2 speakers per circle to reach ~3,300 diverse turns across 439 circles
        speakers = members[:2] if len(members) >= 2 else members[:1]
        rng.shuffle(speakers)

        for speaker_soul in speakers:
            speaker = idx.souls[speaker_soul]
            cands = idx.candidates_for(speaker_soul)
            if not cands:
                continue

            hop1_live = [c for c in cands if c.hop == 1 and not c.is_absentee and c.soul_name]
            hop2_live = [c for c in cands if c.hop == 2 and not c.is_absentee and c.soul_name]
            absentees = [c for c in cands if c.is_absentee]
            cand_keys = {c.soul_name for c in cands if c.soul_name}
            cand_givens = {c.given.lower() for c in cands if c.given}

            # 1. Hop-1 direct locate
            if hop1_live:
                t = rng.choice(hop1_live)
                use_given_only = (
                    sum(1 for c in cands if c.given.lower() == t.given.lower()) == 1
                    and bool(t.given)
                    and rng.random() < 0.55
                )
                ref_name = (
                    (f"{t.title} {t.given}".strip() if t.title else t.given)
                    if use_given_only
                    else t.display_name
                )
                add_bp(
                    split=split,
                    category=BenchCategory.LOCATE_HOP1_DIRECT,
                    speaker=speaker,
                    circle_id=cid,
                    gold_action=CommandAction.LOCATE_NPC,
                    gold_status=ResolutionStatus.RESOLVED_SOUL,
                    gold_soul_name=t.soul_name,
                    gold_surface_target=ref_name,
                    instruction=(
                        f"Henry asks {speaker.full_name} where to find '{ref_name}' right now "
                        f"(use the exact name '{ref_name}'; vary phrasing: direct question, "
                        f"'have you seen...', 'do you know where...', 'point me to...', etc.)."
                    ),
                )

            # 2. Hop-2 adjacent/bridge locate
            if hop2_live and rng.random() < 0.80:
                # Prefer unique full_name in hop2
                t = rng.choice(hop2_live)
                ref_name = t.display_name
                if sum(1 for c in cands if c.display_name.lower() == ref_name.lower()) == 1:
                    add_bp(
                        split=split,
                        category=BenchCategory.LOCATE_HOP2_ADJACENT,
                        speaker=speaker,
                        circle_id=cid,
                        gold_action=CommandAction.LOCATE_NPC,
                        gold_status=ResolutionStatus.RESOLVED_SOUL,
                        gold_soul_name=t.soul_name,
                        gold_surface_target=ref_name,
                        known_tokens=[t.soul_name],
                        instruction=(
                            f"Henry asks {speaker.full_name} if they know where '{ref_name}' "
                            f"can be found around {t.settlement or speaker.settlement or 'town'} right now "
                            f"(must include the name '{ref_name}')."
                        ),
                    )

            # 3. Relational or trade reference (no proper name!)
            rel_counts = collections.Counter(
                c.relationship for c in hop1_live if c.relationship in {
                    "wife", "husband", "son", "daughter", "brother", "sister", "father", "mother", "employer", "master", "apprentice"
                }
            )
            unique_rels = [
                c for c in hop1_live
                if c.relationship in rel_counts and rel_counts[c.relationship] == 1
            ]
            trade_counts = collections.Counter(
                c.trade.lower() for c in hop1_live
                if c.trade and c.trade.lower() not in {"villager", "townsman", "townswoman", "soldier", "bandit"}
            )
            unique_trades = [
                c for c in hop1_live
                if c.trade and trade_counts[c.trade.lower()] == 1
            ]
            if (unique_rels or unique_trades) and rng.random() < 0.70:
                if unique_rels and (not unique_trades or rng.random() < 0.65):
                    t = rng.choice(unique_rels)
                    phrase = f"your {t.relationship}"
                else:
                    t = rng.choice(unique_trades)
                    phrase = f"the {t.trade.lower()}"
                add_bp(
                    split=split,
                    category=BenchCategory.LOCATE_RELATIONAL_TRADE,
                    speaker=speaker,
                    circle_id=cid,
                    gold_action=CommandAction.LOCATE_NPC,
                    gold_status=ResolutionStatus.RESOLVED_SOUL,
                    gold_soul_name=t.soul_name,
                    gold_surface_target=phrase,
                    forbid_words=[t.given.lower(), t.byname.lower()] if t.given else [],
                    instruction=(
                        f"Henry asks {speaker.full_name} where to find '{phrase}' right now. "
                        f"CRITICAL: refer to them ONLY as '{phrase}' — do NOT use their name '{t.display_name}'."
                    ),
                )

            # 4. Multi-turn pronoun coreference ("Where is she/he?")
            gendered_hop1 = [c for c in hop1_live if c.gender in {"male", "female"}]
            if gendered_hop1 and rng.random() < 0.70:
                t = rng.choice(gendered_hop1)
                pron = "she" if t.gender == "female" else "he"
                obj_pron = "her" if t.gender == "female" else "him"
                npc_line = (
                    t.where_hint
                    if t.where_hint and t.given in t.where_hint
                    else f"{t.display_name} has been busy all morning with work around {speaker.settlement or 'here'}."
                )
                hist = [
                    MessageTurn("user", f"How have things been with {t.display_name}?"),
                    MessageTurn("assistant", npc_line),
                ]
                add_bp(
                    split=split,
                    category=BenchCategory.LOCATE_COREF_MULTITURN,
                    speaker=speaker,
                    circle_id=cid,
                    history=hist,
                    gold_action=CommandAction.LOCATE_NPC,
                    gold_status=ResolutionStatus.RESOLVED_SOUL,
                    gold_soul_name=t.soul_name,
                    gold_surface_target=obj_pron,
                    require_pronoun=True,
                    forbid_words=[t.given.lower(), t.byname.lower()] if t.given else [],
                    instruction=(
                        f"In the prior turn, {speaker.full_name} just spoke about {t.display_name} ({t.gender}). "
                        f"Write Henry's follow-up asking where {pron} is / where to find {obj_pron} right now "
                        f"using ONLY a pronoun ('{pron}' or '{obj_pron}'), WITHOUT repeating '{t.display_name}'."
                    ),
                )

            # 5. Homonym disambiguation via salience (history / quest / met_counts)
            by_given: dict[str, list[CandidatePerson]] = {}
            for c in hop1_live + hop2_live:
                if c.given and len(c.given) >= 3:
                    by_given.setdefault(c.given, []).append(c)
            homonym_groups = [g for g in by_given.values() if len(g) >= 2]
            if homonym_groups and rng.random() < 0.85:
                grp = rng.choice(homonym_groups)
                # Pick a target and inject strong salience (history mention or quest + met)
                t = rng.choice(grp)
                shared_given = t.given
                mode = rng.choice(["history", "quest_met"])
                hist = []
                met_counts: dict[str, int] = {}
                quest_souls: list[str] = []
                if mode == "history":
                    hist = [
                        MessageTurn(
                            "assistant",
                            f"If you ask me, {t.display_name} owes half the street a favour.",
                        )
                    ]
                else:
                    # Boost `t` with both active quest AND past meetings
                    quest_souls = [t.soul_name]
                    met_counts = {t.soul_name: 3}
                # Verify our deterministic salience rules agree `t` is the unique winner
                add_bp(
                    split=split,
                    category=BenchCategory.LOCATE_HOMONYM_SALIENCE,
                    speaker=speaker,
                    circle_id=cid,
                    history=hist,
                    met_counts=met_counts,
                    quest_souls=quest_souls,
                    gold_action=CommandAction.LOCATE_NPC,
                    gold_status=ResolutionStatus.RESOLVED_SOUL,
                    gold_soul_name=t.soul_name,
                    gold_surface_target=shared_given,
                    forbid_words=[t.byname.lower()] if t.byname else [],
                    instruction=(
                        f"Henry asks {speaker.full_name} where '{shared_given}' is right now, "
                        f"using ONLY the first name '{shared_given}' (do NOT include surname/byname '{t.byname}')."
                    ),
                )

            # 6. Follow NPC command (extensibility)
            if hop1_live and rng.random() < 0.55:
                if rng.random() < 0.25:
                    add_bp(
                        split=split,
                        category=BenchCategory.CMD_FOLLOW_NPC,
                        speaker=speaker,
                        circle_id=cid,
                        gold_action=CommandAction.FOLLOW_NPC,
                        gold_status=ResolutionStatus.RESOLVED_SOUL,
                        gold_soul_name="player",
                        gold_surface_target="me",
                        instruction=(
                            f"Henry commands or asks {speaker.full_name} to follow him / come along with him "
                            f"(e.g. 'Follow me', 'Come with me now', 'Walk behind me and keep quiet')."
                        ),
                    )
                else:
                    t = rng.choice(hop1_live)
                    add_bp(
                        split=split,
                        category=BenchCategory.CMD_FOLLOW_NPC,
                        speaker=speaker,
                        circle_id=cid,
                        gold_action=CommandAction.FOLLOW_NPC,
                        gold_status=ResolutionStatus.RESOLVED_SOUL,
                        gold_soul_name=t.soul_name,
                        gold_surface_target=t.display_name,
                        instruction=(
                            f"Henry asks or commands {speaker.full_name} to follow, accompany, or go after "
                            f"'{t.display_name}' (must include '{t.display_name}')."
                        ),
                    )

            # 7. Travel To Place command (extensibility)
            if rng.random() < 0.50:
                pid, pmeta = rng.choice(place_items)
                pname = rng.choice([pmeta["name"]] + pmeta["aliases"][:1])
                add_bp(
                    split=split,
                    category=BenchCategory.CMD_TRAVEL_TO,
                    speaker=speaker,
                    circle_id=cid,
                    gold_action=CommandAction.TRAVEL_TO,
                    gold_status=ResolutionStatus.RESOLVED_PLACE,
                    gold_place_id=pid,
                    gold_surface_target=pname,
                    instruction=(
                        f"Henry asks or commands {speaker.full_name} to travel with him / take him / head out "
                        f"to '{pname}' (must name '{pname}')."
                    ),
                )

            # 8. Circle Absentee (unmeetable -> abstain from marker!)
            if absentees and rng.random() < 0.65:
                ab = rng.choice(absentees)
                add_bp(
                    split=split,
                    category=BenchCategory.ABSTAIN_ABSENTEE,
                    speaker=speaker,
                    circle_id=cid,
                    gold_action=CommandAction.LOCATE_NPC,
                    gold_status=ResolutionStatus.UNMEETABLE_ABSENTEE,
                    gold_soul_name="",
                    gold_surface_target=ab.display_name,
                    instruction=(
                        f"Henry asks {speaker.full_name} where he can find '{ab.display_name}' right now "
                        f"(must name '{ab.display_name}')."
                    ),
                )

            # 9. Complete Stranger outside Hop-1/Hop-2 (-> unknown_to_npc)
            if rng.random() < 0.45:
                for _ in range(20):
                    st = rng.choice(all_souls)
                    if (
                        st.soul_name not in cand_keys
                        and st.soul_name != speaker_soul
                        and st.given.lower() not in cand_givens
                        and st.byname
                    ):
                        add_bp(
                            split=split,
                            category=BenchCategory.ABSTAIN_STRANGER,
                            speaker=speaker,
                            circle_id=cid,
                            gold_action=CommandAction.LOCATE_NPC,
                            gold_status=ResolutionStatus.UNKNOWN_TO_NPC,
                            gold_soul_name="",
                            gold_surface_target=st.full_name,
                            instruction=(
                                f"Henry asks {speaker.full_name} where to find '{st.full_name}' "
                                f"(someone this NPC has never heard of; must include '{st.full_name}')."
                            ),
                        )
                        break

            # 10. Hard Negative A: Biographical / Past / Lore "Where" question (KnowledgeDirector, NOT locate_npc!)
            if hop1_live and rng.random() < 0.65:
                t = rng.choice(hop1_live)
                add_bp(
                    split=split,
                    category=BenchCategory.NEG_KB_BIOGRAPHICAL_WHERE,
                    speaker=speaker,
                    circle_id=cid,
                    gold_action=CommandAction.NO_ACTION,
                    gold_status=ResolutionStatus.NO_ACTION,
                    gold_surface_target=t.display_name,
                    instruction=(
                        f"Henry asks a question starting with or containing 'where' that mentions "
                        f"'{t.display_name}' or {speaker.full_name}, BUT asks about past history, birthplace, "
                        f"where they learned their trade, or where they bought/lost an item — NOT where "
                        f"{t.display_name} currently is in the world!"
                    ),
                )

            # 11. Hard Negative B: Gossip / Opinion / Character question about a circle NPC
            if hop1_live and rng.random() < 0.65:
                t = rng.choice(hop1_live)
                add_bp(
                    split=split,
                    category=BenchCategory.NEG_OPINION_GOSSIP,
                    speaker=speaker,
                    circle_id=cid,
                    gold_action=CommandAction.NO_ACTION,
                    gold_status=ResolutionStatus.NO_ACTION,
                    gold_surface_target=t.display_name,
                    instruction=(
                        f"Henry asks {speaker.full_name} for gossip, opinion, or history about '{t.display_name}' "
                        f"(e.g. whether they can be trusted, why they quarrelled, how they earn a living) — "
                        f"do NOT ask where {t.display_name} is located."
                    ),
                )

            # 12. Hard Negative C: Idiomatic / Rhetorical 'where' / 'follow' / 'travel' / 'go'
            if rng.random() < 0.50:
                add_bp(
                    split=split,
                    category=BenchCategory.NEG_IDIOM_RHETORICAL,
                    speaker=speaker,
                    circle_id=cid,
                    gold_action=CommandAction.NO_ACTION,
                    gold_status=ResolutionStatus.NO_ACTION,
                    instruction=(
                        f"Henry says a conversational line to {speaker.full_name} that uses a word like "
                        f"'follow', 'where', 'find', 'travel', or 'point' figuratively or rhetorically "
                        f"(e.g. 'I don't follow your meaning', 'Where do you get off speaking like that?', "
                        f"'Have you ever travelled beyond Kuttenberg?', 'I find that hard to believe') — "
                        f"NOT asking to locate anyone or go anywhere."
                    ),
                )

            # 13. Hard Negative D: Quest help-offer or ordinary medieval chat
            if rng.random() < 0.55:
                add_bp(
                    split=split,
                    category=BenchCategory.NEG_QUEST_OFFER_CHAT,
                    speaker=speaker,
                    circle_id=cid,
                    gold_action=CommandAction.NO_ACTION,
                    gold_status=ResolutionStatus.NO_ACTION,
                    instruction=(
                        f"Henry greets {speaker.full_name}, asks if they have any work/trouble he can help with, "
                        f"or makes small talk about weather, coin, war, or trade in {speaker.settlement or 'Bohemia'}."
                    ),
                )

        if len(blueprints) >= target_total:
            break

    return blueprints[:target_total]


def call_anthropic_batch(
    batch: list[dict[str, Any]], api_key: str, model: str = "claude-haiku-5-5"
) -> dict[str, str]:
    """Generate natural Henry utterances for a batch of blueprints via Anthropic API."""
    items_txt = []
    for bp in batch:
        hist_str = ""
        if bp.get("history"):
            turns = " | ".join(f"{m.role}: {m.content}" for m in bp["history"])
            hist_str = f" [Prior context: {turns}]"
        items_txt.append(f'- id="{bp["bp_id"]}":{hist_str} {bp["instruction"]}')

    prompt = (
        "You are generating realistic spoken/typed player lines for Henry of Skalitz in "
        "Kingdom Come: Deliverance II (1403 Bohemia). Keep lines concise (4 to 22 words), "
        "natural for spoken conversation, varied in phrasing across items, with no stage "
        "directions or asterisks.\n\n"
        "Return ONLY a valid JSON object mapping each `id` to Henry's single utterance string.\n\n"
        + "\n".join(items_txt)
    )

    payload = json.dumps(
        {
            "model": model,
            "max_tokens": 2048,
            "messages": [{"role": "user", "content": prompt}],
        }
    ).encode("utf-8")

    for attempt in range(3):
        try:
            req = urllib.request.Request(
                "https://api.anthropic.com/v1/messages",
                data=payload,
                headers={
                    "x-api-key": api_key,
                    "anthropic-version": "2023-06-01",
                    "content-type": "application/json",
                },
                method="POST",
            )
            with urllib.request.urlopen(req, timeout=45) as resp:
                body = json.loads(resp.read().decode("utf-8"))
            text = "".join(
                blk.get("text", "") for blk in body.get("content", []) if blk.get("type") == "text"
            ).strip()
            # Strip markdown code fences if present
            if text.startswith("```"):
                text = re.sub(r"^```(?:json)?\s*", "", text)
                text = re.sub(r"\s*```$", "", text)
            start = text.find("{")
            end = text.rfind("}")
            if start >= 0 and end > start:
                parsed = json.loads(text[start : end + 1])
                if isinstance(parsed, dict):
                    return {str(k): str(v).strip() for k, v in parsed.items()}
        except (urllib.error.URLError, json.JSONDecodeError, TimeoutError) as exc:
            if attempt == 2:
                print(f"Batch warning after retries: {exc}", file=sys.stderr)
            time.sleep(1.2 * (attempt + 1))
    return {}


def validate_utterance(bp: dict[str, Any], utt: str) -> bool:
    """Verify generated utterance respects blueprint constraints (no leaked names, etc.)."""
    if not utt or len(utt) < 3 or len(utt) > 220 or "*" in utt:
        return False
    low_words = set(normalize_text(utt).split())
    for bad in bp.get("forbid_words", []):
        if bad and len(bad) >= 3 and normalize_text(bad) in low_words:
            return False
    if bp.get("require_pronoun") and not (low_words & PRONOUN_WORDS):
        return False
    cat: BenchCategory = bp["category"]
    if cat in {
        BenchCategory.NEG_OPINION_GOSSIP,
        BenchCategory.NEG_IDIOM_RHETORICAL,
        BenchCategory.NEG_QUEST_OFFER_CHAT,
    }:
        # Reject accidental location requests in non-biographical negatives
        if re.search(r"\bwhere\s+(?:is|are|can\s+i\s+find|might\s+i\s+find)\b", utt.lower()):
            return False
    return True


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--db", default="data/kcd2/npcs.sqlite")
    ap.add_argument("--out", default="build/command_bench/dataset_v1.jsonl")
    ap.add_argument("--stats", default="tools/command_bench/dataset_stats.json")
    ap.add_argument("--total", type=int, default=3300)
    ap.add_argument("--workers", type=int, default=24)
    ap.add_argument("--batch-size", type=int, default=15)
    ap.add_argument("--model", default="claude-haiku-5-5")
    ap.add_argument("--seed", type=int, default=42)
    args = ap.parse_args()

    api_key = os.environ.get("ANTHROPIC_API_KEY", "")
    if not api_key:
        raise SystemExit("ANTHROPIC_API_KEY is required for cloud dataset generation")

    t0 = time.perf_counter()
    idx = SocialGraphIndex(args.db)
    blueprints = build_blueprints(idx, seed=args.seed, target_total=args.total)
    print(f"Constructed {len(blueprints)} grounded blueprints across circle-disjoint splits.")

    batches = [
        blueprints[i : i + args.batch_size]
        for i in range(0, len(blueprints), args.batch_size)
    ]

    utterances: dict[str, str] = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.workers) as pool:
        futs = {
            pool.submit(call_anthropic_batch, b, api_key, args.model): idx_b
            for idx_b, b in enumerate(batches)
        }
        done_count = 0
        for fut in concurrent.futures.as_completed(futs):
            res = fut.result()
            utterances.update(res)
            done_count += 1
            if done_count % 25 == 0 or done_count == len(batches):
                print(
                    f"  [{done_count}/{len(batches)} batches] {len(utterances)} raw utterances ({time.perf_counter() - t0:.1f}s)"
                )

    examples: list[BenchmarkExample] = []
    dropped = 0
    oracle_mismatch = 0

    for bp in blueprints:
        utt = utterances.get(bp["bp_id"], "").strip().strip('"')
        if not validate_utterance(bp, utt):
            dropped += 1
            continue

        speaker = bp["speaker"]
        ex = BenchmarkExample(
            example_id=bp["bp_id"],
            split=bp["split"],
            category=bp["category"],
            speaker_soul=speaker.soul_name,
            speaker_name=speaker.full_name,
            circle_id=bp["circle_id"],
            settlement=speaker.settlement,
            history=bp.get("history", []),
            met_counts=bp.get("met_counts", {}),
            known_tokens=bp.get("known_tokens", []),
            quest_souls=bp.get("quest_souls", []),
            kb_gated=False,
            player_utterance=utt,
            gold_action=bp["gold_action"],
            gold_status=bp["gold_status"],
            gold_soul_name=bp.get("gold_soul_name", ""),
            gold_place_id=bp.get("gold_place_id", ""),
            gold_surface_target=bp.get("gold_surface_target", ""),
        )

        # Verify homonym blueprints resolve cleanly to `gold_soul_name` under oracle mention
        if ex.category == BenchCategory.LOCATE_HOMONYM_SALIENCE:
            from tools.command_bench.schema import ExtractedCommand, TargetKind

            oracle_res = idx.resolve(
                ex.speaker_soul,
                ExtractedCommand(
                    action=CommandAction.LOCATE_NPC,
                    target_mention=ex.gold_surface_target,
                    target_kind=TargetKind.PERSON,
                ),
                history=ex.history,
                met_counts=ex.met_counts,
                known_tokens=ex.known_tokens,
                quest_souls=ex.quest_souls,
            )
            if oracle_res.soul_name != ex.gold_soul_name:
                oracle_mismatch += 1
                continue

        examples.append(ex)

    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with out_path.open("w", encoding="utf-8") as f:
        for ex in examples:
            f.write(ex.to_json_line() + "\n")

    # Compute summary stats and verify zero circle overlap across splits
    splits_circles: dict[str, set[str]] = collections.defaultdict(set)
    by_split = collections.Counter()
    by_cat = collections.Counter()
    by_action = collections.Counter()
    by_status = collections.Counter()
    for ex in examples:
        splits_circles[ex.split].add(ex.circle_id)
        by_split[ex.split] += 1
        by_cat[ex.category.value] += 1
        by_action[ex.gold_action.value] += 1
        by_status[ex.gold_status.value] += 1

    assert not (splits_circles["train"] & splits_circles["test"]), "Train/test circle leak!"
    assert not (splits_circles["dev"] & splits_circles["test"]), "Dev/test circle leak!"

    stats = {
        "total_examples": len(examples),
        "dropped_validation": dropped,
        "dropped_homonym_oracle": oracle_mismatch,
        "elapsed_seconds": round(time.perf_counter() - t0, 2),
        "generator_model": args.model,
        "unique_circles_per_split": {k: len(v) for k, v in sorted(splits_circles.items())},
        "counts_by_split": dict(sorted(by_split.items())),
        "counts_by_action": dict(sorted(by_action.items())),
        "counts_by_status": dict(sorted(by_status.items())),
        "counts_by_category": dict(sorted(by_cat.items())),
    }
    stats_path = Path(args.stats)
    stats_path.parent.mkdir(parents=True, exist_ok=True)
    stats_path.write_text(json.dumps(stats, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(stats, indent=2))


if __name__ == "__main__":
    main()
