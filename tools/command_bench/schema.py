#!/usr/bin/env python3
"""Unified action schema, resolution outcomes, and circle-disjoint benchmark spec.

Defines the contract shared across:
  1. Synthetic dataset generation (`generate_dataset.py`)
  2. Candidate NLU/LLM extractors (Qwen3-4B KV-suffix, FunctionGemma-270M,
     Qwen3-0.6B/1.7B, GLiNER2.5, EmbeddingGemma-300M Stage-1 gate)
  3. Salience-ranked social-graph entity linking (`linker.py`)
  4. Two-pass contextual STT evaluation (`stt_eval.py`)
  5. Production C++ `Session` integration (`@locate <soul_name>`)
"""
from __future__ import annotations

import dataclasses
import enum
import hashlib
import json
from dataclasses import dataclass, field
from typing import Any


class CommandAction(str, enum.Enum):
    """Top-level command intent recognized in freeform player speech/text."""

    NO_ACTION = "no_action"
    LOCATE_NPC = "locate_npc"
    FOLLOW_NPC = "follow_npc"
    TRAVEL_TO = "travel_to"


class TargetKind(str, enum.Enum):
    """Semantic type of the command's target slot."""

    NONE = "none"
    PERSON = "person"
    PLACE = "place"


class ResolutionStatus(str, enum.Enum):
    """Outcome after combining NLU extraction with social-graph entity linking."""

    NO_ACTION = "no_action"
    RESOLVED_SOUL = "resolved_soul"
    RESOLVED_PLACE = "resolved_place"
    UNMEETABLE_ABSENTEE = "unmeetable_absentee"
    GATED_BY_TRUST = "gated_by_trust"
    UNKNOWN_TO_NPC = "unknown_to_npc"
    AMBIGUOUS_SOUL = "ambiguous_soul"


class BenchCategory(str, enum.Enum):
    """Stratified evaluation slices for diagnosing failure modes."""

    LOCATE_HOP1_DIRECT = "locate_hop1_direct"
    LOCATE_HOP2_ADJACENT = "locate_hop2_adjacent"
    LOCATE_RELATIONAL_TRADE = "locate_relational_trade"
    LOCATE_COREF_MULTITURN = "locate_coref_multiturn"
    LOCATE_HOMONYM_SALIENCE = "locate_homonym_salience"
    CMD_FOLLOW_NPC = "cmd_follow_npc"
    CMD_TRAVEL_TO = "cmd_travel_to"
    ABSTAIN_ABSENTEE = "abstain_absentee"
    ABSTAIN_STRANGER = "abstain_stranger"
    NEG_KB_BIOGRAPHICAL_WHERE = "neg_kb_biographical_where"
    NEG_OPINION_GOSSIP = "neg_opinion_gossip"
    NEG_IDIOM_RHETORICAL = "neg_idiom_rhetorical"
    NEG_QUEST_OFFER_CHAT = "neg_quest_offer_chat"


# Canonical KCD2 settlements & landmarks valid for `travel_to` commands.
CANONICAL_PLACES: dict[str, dict[str, Any]] = {
    "place:kuttenberg": {
        "name": "Kuttenberg",
        "aliases": ["Kutna Hora", "the city", "Kuttenberg city"],
        "region": "Kuttenberg region",
    },
    "place:trosky_castle": {
        "name": "Trosky Castle",
        "aliases": ["Trosky", "the castle at Trosky"],
        "region": "Trosky region",
    },
    "place:troskowitz": {
        "name": "Troskowitz",
        "aliases": ["Troskovice"],
        "region": "Trosky region",
    },
    "place:suchdol": {
        "name": "Suchdol",
        "aliases": ["Suchdol fortress"],
        "region": "Kuttenberg region",
    },
    "place:pritoky": {
        "name": "Pritoky",
        "aliases": ["Pritoky village"],
        "region": "Kuttenberg region",
    },
    "place:bylany": {
        "name": "Bylany",
        "aliases": ["Bylany village"],
        "region": "Kuttenberg region",
    },
    "place:grunta": {
        "name": "Grunta",
        "aliases": ["Grunta village", "the Grunta ironworks"],
        "region": "Kuttenberg region",
    },
    "place:horany": {
        "name": "Horany",
        "aliases": ["Horany mines"],
        "region": "Kuttenberg region",
    },
    "place:malesov": {
        "name": "Malesov",
        "aliases": ["Malesov fortress"],
        "region": "Kuttenberg region",
    },
    "place:miskowitz": {
        "name": "Miskowitz",
        "aliases": ["Miskovice"],
        "region": "Kuttenberg region",
    },
    "place:old_kutna": {
        "name": "Old Kutna",
        "aliases": ["Stara Kutna"],
        "region": "Kuttenberg region",
    },
    "place:sedletz": {
        "name": "Sedletz",
        "aliases": ["Sedletz Monastery", "the monastery at Sedletz"],
        "region": "Kuttenberg region",
    },
    "place:sigismunds_camp": {
        "name": "Sigismund's camp",
        "aliases": ["King Sigismund's camp", "the royal army camp"],
        "region": "Kuttenberg region",
    },
    "place:tachov": {
        "name": "Tachov",
        "aliases": ["Tachov village"],
        "region": "Trosky region",
    },
    "place:zhelejov": {
        "name": "Zhelejov",
        "aliases": ["Zelejov", "Zhelejov wagoners' inn"],
        "region": "Trosky region",
    },
    "place:semin": {
        "name": "Semin",
        "aliases": ["Semin estate", "Semin mill"],
        "region": "Trosky region",
    },
    "place:nebakov": {
        "name": "Nebakov fortress",
        "aliases": ["Nebakov", "Nebakov mill"],
        "region": "Trosky region",
    },
    "place:nomad_camp": {
        "name": "Nomad camp",
        "aliases": ["the nomads' camp", "the Romani camp"],
        "region": "Trosky region",
    },
}


# Tool definitions in OpenAI / FunctionGemma JSON-Schema format for tool-calling arms.
TOOL_SCHEMAS: list[dict[str, Any]] = [
    {
        "type": "function",
        "function": {
            "name": "locate_npc",
            "description": (
                "Mark a person's current physical whereabouts on Henry's map and compass "
                "when Henry asks where to find someone right now. Do NOT call for past "
                "biographical questions (e.g. 'Where was X born?'), opinions, or gossip."
            ),
            "parameters": {
                "type": "object",
                "properties": {
                    "person": {
                        "type": "string",
                        "description": (
                            "Name, title+name, or relationship/trade of the person Henry "
                            "wants to locate (resolve pronouns like 'she'/'him' from "
                            "recent conversation turns if possible)."
                        ),
                    },
                },
                "required": ["person"],
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "follow_npc",
            "description": (
                "Instruct the NPC to physically follow, accompany, or walk with a person "
                "when Henry explicitly asks or commands it. Do NOT call for figurative uses "
                "like 'I don't follow you'."
            ),
            "parameters": {
                "type": "object",
                "properties": {
                    "person": {
                        "type": "string",
                        "description": (
                            "Name, title+name, 'me' (Henry), or relationship of the person "
                            "to follow."
                        ),
                    },
                },
                "required": ["person"],
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "travel_to",
            "description": (
                "Travel or go together to a named town, village, castle, camp, or landmark "
                "when Henry explicitly asks or commands to go/travel there."
            ),
            "parameters": {
                "type": "object",
                "properties": {
                    "destination": {
                        "type": "string",
                        "description": "Name of the destination town, castle, camp, or place.",
                    },
                },
                "required": ["destination"],
            },
        },
    },
]


# Compact GBNF grammar for llama.cpp (`llama_sampler_init_grammar`) used in Arm A / C.
# Forces deterministic, single-line structured output with explicit NONE abstention:
#   NONE
#   LOCATE: <mention>
#   FOLLOW: <mention>
#   TRAVEL: <destination>
COMPACT_COMMAND_GBNF: str = r"""
root   ::= none | locate | follow | travel
none   ::= "NONE"
locate ::= "LOCATE: " span
follow ::= "FOLLOW: " span
travel ::= "TRAVEL: " span
span   ::= [A-Za-z'] [A-Za-z0-9' -]{0,48}
""".strip()


@dataclass
class MessageTurn:
    role: str  # "assistant" (NPC) or "user" (Henry)
    content: str


@dataclass
class ExtractedCommand:
    """Normalized output of any Stage-2 NLU / SLM extractor."""

    action: CommandAction = CommandAction.NO_ACTION
    target_mention: str = ""
    target_kind: TargetKind = TargetKind.NONE
    relation_hint: str = ""
    is_pronoun: bool = False
    confidence: float = 1.0
    raw_output: str = ""

    def to_dict(self) -> dict[str, Any]:
        d = dataclasses.asdict(self)
        d["action"] = self.action.value
        d["target_kind"] = self.target_kind.value
        return d


@dataclass
class PipelineResult:
    """Full end-to-end pipeline decision including graph linking & policy checks."""

    action: CommandAction = CommandAction.NO_ACTION
    status: ResolutionStatus = ResolutionStatus.NO_ACTION
    soul_name: str = ""
    place_id: str = ""
    matched_name: str = ""
    extracted_mention: str = ""
    absentee_reason: str = ""
    dialogue_direction: str = ""
    directive: str = ""
    confidence: float = 0.0
    used_stage1_skip: bool = False
    used_stt_pass2: bool = False
    latency_ms: float = 0.0

    def to_dict(self) -> dict[str, Any]:
        d = dataclasses.asdict(self)
        d["action"] = self.action.value
        d["status"] = self.status.value
        return d


@dataclass
class BenchmarkExample:
    """One labeled evaluation or training turn grounded in `npcs.sqlite`."""

    example_id: str
    split: str  # "train" | "dev" | "test"
    category: BenchCategory
    speaker_soul: str
    speaker_name: str
    circle_id: str
    settlement: str
    # Prior conversation turns (oldest first) before `player_utterance`.
    history: list[MessageTurn] = field(default_factory=list)
    # Simulated save state for salience ranking:
    # souls Henry has met (soul_name -> met count)
    met_counts: dict[str, int] = field(default_factory=dict)
    # tokens Henry has heard (`@heard`)
    known_tokens: list[str] = field(default_factory=list)
    # souls active in Henry's current quests
    quest_souls: list[str] = field(default_factory=list)
    # Whether speaker's KbGate withholds help right now
    kb_gated: bool = False
    # What Henry says on this turn:
    player_utterance: str = ""
    # Gold annotations:
    gold_action: CommandAction = CommandAction.NO_ACTION
    gold_status: ResolutionStatus = ResolutionStatus.NO_ACTION
    gold_soul_name: str = ""
    gold_place_id: str = ""
    gold_surface_target: str = ""  # e.g. "Betty Vejmola" or "Martha" or "Trosky Castle"

    def to_json_line(self) -> str:
        d = dataclasses.asdict(self)
        d["category"] = self.category.value
        d["gold_action"] = self.gold_action.value
        d["gold_status"] = self.gold_status.value
        return json.dumps(d, ensure_ascii=False)

    @classmethod
    def from_dict(cls, d: dict[str, Any]) -> "BenchmarkExample":
        hist = [MessageTurn(**m) for m in d.get("history", [])]
        return cls(
            example_id=d["example_id"],
            split=d["split"],
            category=BenchCategory(d["category"]),
            speaker_soul=d["speaker_soul"],
            speaker_name=d["speaker_name"],
            circle_id=d["circle_id"],
            settlement=d.get("settlement", ""),
            history=hist,
            met_counts=dict(d.get("met_counts", {})),
            known_tokens=list(d.get("known_tokens", [])),
            quest_souls=list(d.get("quest_souls", [])),
            kb_gated=bool(d.get("kb_gated", False)),
            player_utterance=d["player_utterance"],
            gold_action=CommandAction(d["gold_action"]),
            gold_status=ResolutionStatus(d["gold_status"]),
            gold_soul_name=d.get("gold_soul_name", ""),
            gold_place_id=d.get("gold_place_id", ""),
            gold_surface_target=d.get("gold_surface_target", ""),
        )


def assign_circle_split(circle_id: str) -> str:
    """Deterministically partition `circle_id`s into 60% train / 15% dev / 25% test.

    Splitting by `circle_id` ensures zero character/household overlap between train and
    test splits so fine-tuned SLMs must learn generalizable command syntax and context
    rather than memorizing specific NPC names.
    """
    digest = hashlib.sha256(f"moonshine-cmd-v1:{circle_id}".encode("utf-8")).digest()
    bucket = int.from_bytes(digest[:4], "big") % 100
    if bucket < 60:
        return "train"
    if bucket < 75:
        return "dev"
    return "test"


def parse_compact_command(text: str) -> ExtractedCommand:
    """Parse `NONE | LOCATE: <span > | FOLLOW: <span> | TRAVEL: <span>` output."""
    raw = text.strip()
    first_line = raw.splitlines()[0].strip() if raw else ""
    upper = first_line.upper()
    if not first_line or upper == "NONE" or upper.startswith("NONE"):
        return ExtractedCommand(action=CommandAction.NO_ACTION, raw_output=raw)

    for prefix, action, kind in (
        ("LOCATE:", CommandAction.LOCATE_NPC, TargetKind.PERSON),
        ("FOLLOW:", CommandAction.FOLLOW_NPC, TargetKind.PERSON),
        ("TRAVEL:", CommandAction.TRAVEL_TO, TargetKind.PLACE),
    ):
        if upper.startswith(prefix):
            span = first_line[len(prefix) :].strip().strip("\"'.,!?")
            if not span or span.upper() == "NONE":
                return ExtractedCommand(action=CommandAction.NO_ACTION, raw_output=raw)
            low = span.lower()
            is_pron = low in {"he", "him", "she", "her", "them", "they", "there"}
            return ExtractedCommand(
                action=action,
                target_mention=span,
                target_kind=kind,
                is_pronoun=is_pron,
                raw_output=raw,
            )
    return ExtractedCommand(action=CommandAction.NO_ACTION, raw_output=raw)
