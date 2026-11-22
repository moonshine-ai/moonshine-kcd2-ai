#!/usr/bin/env python3
"""Salience-ranked social-graph entity linker and phonetic matcher over `npcs.sqlite`.

Given:
  - Speaker `soul_name`
  - Conversation history (`list[MessageTurn]`) and save-state priors (`met_counts`,
    `known_tokens`, `quest_souls`, `kb_gated`)
  - `ExtractedCommand` (`action`, `target_mention`, `is_pronoun`)

Resolves the mention against the speaker's Hop-1 (circle + direct relationships +
circle absentees) and Hop-2 (bridge circles + cross-circle relationships + active quests)
candidate graph, returning a `PipelineResult` with both the structured game directive
(`@locate <soul_name>`) and a natural bracketed direction for `Qwen3-4B`.
"""
from __future__ import annotations

import json
import re
import sqlite3
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable

from .schema import (
    CANONICAL_PLACES,
    CommandAction,
    ExtractedCommand,
    MessageTurn,
    PipelineResult,
    ResolutionStatus,
)

TITLES = {
    "father",
    "brother",
    "sister",
    "sir",
    "master",
    "mistress",
    "old",
    "young",
    "dame",
    "lord",
    "lady",
    "captain",
    "bailiff",
    "alchemist",
    "executioner",
    "wagoner",
    "butcher",
}

STOP_ARTICLES = {"the", "a", "an", "my", "your", "our", "his", "her", "their", "this", "that"}

FEMALE_PRONOUNS = {"she", "her", "hers", "herself"}
MALE_PRONOUNS = {"he", "him", "his", "himself"}

TRADE_SYNONYMS: dict[str, list[str]] = {
    "blacksmith": ["smith"],
    "weaponsmith": ["smith", "swordsmith"],
    "armourer": ["armorsmith", "armorer"],
    "innkeeper": ["tavernkeeper", "publican", "host"],
    "apothecary": ["alchemist", "herbalist"],
    "priest": ["parson", "cleric"],
    "bailiff": ["reeve"],
    "merchant": ["trader", "shopkeeper"],
    "carpenter": ["joiner"],
    "cobbler": ["shoemaker"],
}

CLOSENESS_RANK: dict[str, int] = {
    "wife": 1,
    "husband": 1,
    "father": 2,
    "mother": 2,
    "son": 2,
    "daughter": 2,
    "brother": 3,
    "sister": 3,
    "household": 4,
    "master": 5,
    "apprentice": 5,
    "employer": 5,
    "servant": 5,
    "friend": 6,
    "comrade": 7,
    "neighbour": 8,
    "acquaintance": 9,
}


def normalize_text(s: str) -> str:
    """Lowercase ASCII-folded alphanumeric + space normalization."""
    s = s.lower().strip()
    replacements = {
        "á": "a",
        "ä": "ae",
        "č": "c",
        "ď": "d",
        "é": "e",
        "ě": "e",
        "í": "i",
        "ň": "n",
        "ó": "o",
        "ö": "oe",
        "ř": "r",
        "š": "s",
        "ť": "t",
        "ú": "u",
        "ů": "u",
        "ü": "ue",
        "ý": "y",
        "ž": "z",
        "ß": "ss",
    }
    for k, v in replacements.items():
        s = s.replace(k, v)
    s = re.sub(r"['’`]", "", s)
    s = re.sub(r"[^a-z0-9]+", " ", s)
    return " ".join(s.split())


def strip_determiners(s: str) -> str:
    toks = normalize_text(s).split()
    while len(toks) > 1 and toks[0] in STOP_ARTICLES:
        toks.pop(0)
    return " ".join(toks)


def phonetic_key(word: str) -> str:
    """Compact Bohemian/German/English phonetic skeleton for medieval proper nouns.

    Maps common ASR homophones and Central European transliterations to a unified
    consonant-vowel onset code (e.g. Joerg/Jorg/Yorg -> YRK, Vejmola/Weymola -> FYML,
    Benesh/Benes -> BNS, Kunz/Kuntz -> KNTS, Schreiber/Shriver -> SRFR, Margaret/Margret -> MRKRT).
    """
    w = normalize_text(word).replace(" ", "")
    if not w:
        return ""
    # Multi-character digraph normalizations (Germanic + Slavic + English STT spellings)
    subs = [
        ("tsch", "c"),
        ("sch", "s"),
        ("cz", "c"),
        ("sz", "s"),
        ("sh", "s"),
        ("ch", "k"),
        ("ck", "k"),
        ("ph", "f"),
        ("th", "t"),
        ("gh", "k"),
        ("tz", "ts"),
        ("kn", "n"),
        ("wr", "r"),
        ("wh", "w"),
        ("ae", "e"),
        ("oe", "e"),
        ("ue", "i"),
    ]
    for a, b in subs:
        w = w.replace(a, b)
    if w.startswith("j") or w.startswith("y"):
        w = "y" + w[1:]
    if w.startswith("w"):
        w = "f" + w[1:]
    w = w.replace("v", "f").replace("w", "f")
    w = w.replace("q", "k").replace("c", "k").replace("x", "ks").replace("z", "s")
    w = w.replace("g", "k").replace("d", "t").replace("b", "p")

    # Keep initial vowel marker, drop interior vowels & 'h'/'y'
    out = [w[0]]
    for ch in w[1:]:
        if ch in "aeiouyh":
            continue
        if ch != out[-1]:
            out.append(ch)
    return "".join(out)


def phonetic_phrase(phrase: str) -> str:
    toks = [t for t in strip_determiners(phrase).split() if t not in TITLES]
    if not toks:
        toks = strip_determiners(phrase).split()
    return " ".join(phonetic_key(t) for t in toks if t)


def levenshtein(a: str, b: str) -> int:
    if a == b:
        return 0
    if not a:
        return len(b)
    if not b:
        return len(a)
    prev = list(range(len(b) + 1))
    for i, ca in enumerate(a, 1):
        curr = [i]
        for j, cb in enumerate(b, 1):
            cost = 0 if ca == cb else 1
            curr.append(min(curr[-1] + 1, prev[j] + 1, prev[j - 1] + cost))
        prev = curr
    return prev[-1]


def jaro_winkler(s1: str, s2: str) -> float:
    if s1 == s2:
        return 1.0
    len1, len2 = len(s1), len(s2)
    if len1 == 0 or len2 == 0:
        return 0.0
    match_dist = max(len1, len2) // 2 - 1
    if match_dist < 0:
        match_dist = 0
    s1_matches = [False] * len1
    s2_matches = [False] * len2
    matches = 0
    transpositions = 0
    for i in range(len1):
        start = max(0, i - match_dist)
        end = min(i + match_dist + 1, len2)
        for j in range(start, end):
            if s2_matches[j] or s1[i] != s2[j]:
                continue
            s1_matches[i] = True
            s2_matches[j] = True
            matches += 1
            break
    if matches == 0:
        return 0.0
    k = 0
    for i in range(len1):
        if not s1_matches[i]:
            continue
        while not s2_matches[k]:
            k += 1
        if s1[i] != s2[k]:
            transpositions += 1
        k += 1
    jaro = (
        matches / len1 + matches / len2 + (matches - transpositions / 2) / matches
    ) / 3.0
    prefix = 0
    for ca, cb in zip(s1[:4], s2[:4]):
        if ca == cb:
            prefix += 1
        else:
            break
    return jaro + prefix * 0.1 * (1.0 - jaro)


def _fast_similarity(
    qn: str, qp: str, an: str, ap: str, use_phonetics: bool = True
) -> float:
    if not qn or not an:
        return 0.0
    if qn == an:
        return 1.0
    jw = jaro_winkler(qn, an)
    ed_sim = 1.0 - (levenshtein(qn, an) / max(len(qn), len(an)))
    best_ortho = max(jw, ed_sim)
    if not use_phonetics:
        return best_ortho
    phon_sim = 0.0
    if qp and ap and len(qp) >= 2 and len(ap) >= 2:
        if qp == ap:
            phon_sim = 0.94
        else:
            p_ed = 1.0 - (levenshtein(qp, ap) / max(len(qp), len(ap)))
            phon_sim = p_ed * 0.90
    if phon_sim > best_ortho:
        return 0.55 * phon_sim + 0.45 * best_ortho
    return best_ortho


def string_and_phonetic_similarity(
    query: str, alias: str, use_phonetics: bool = True
) -> float:
    """Combined orthographic + phonetic similarity in [0.0, 1.0]."""
    qn = strip_determiners(query)
    an = strip_determiners(alias)
    if not qn or not an:
        return 0.0
    if qn == an:
        return 1.0
    qp = phonetic_phrase(qn) if use_phonetics else ""
    ap = phonetic_phrase(an) if use_phonetics else ""
    return _fast_similarity(qn, qp, an, ap, use_phonetics=use_phonetics)


@dataclass
class SoulNode:
    soul_name: str
    soul_id: str
    full_name: str
    given: str
    particle: str
    byname: str
    title: str
    gender: str
    trade: str
    occupation: str
    settlement: str
    region: str
    circle_id: str
    is_named: bool
    has_routine: bool = False


@dataclass
class AbsenteeNode:
    circle_id: str
    name: str
    given: str
    station: str
    why_unmeetable: str
    to_the_circle: str


@dataclass
class CandidatePerson:
    """A candidate person in the speaker's Hop-1 or Hop-2 neighborhood."""

    key: str  # soul_name, or "absentee:<name>"
    soul_name: str  # empty for absentees
    display_name: str
    given: str
    byname: str
    title: str
    gender: str
    hop: int  # 1 or 2
    source: str  # "circle" | "relationship" | "absentee" | "bridge" | "adjacent" | "quest"
    relationship: str = ""  # e.g. "son", "brother", "friend"
    trade: str = ""
    settlement: str = ""
    is_absentee: bool = False
    why_unmeetable: str = ""
    where_hint: str = ""  # short English clue from circle KB facts if available
    aliases: list[tuple[str, float]] = field(default_factory=list)  # (alias_str, weight)
    compiled_aliases: list[tuple[str, str, float]] = field(
        default_factory=list
    )  # (alias_norm, alias_phon, weight)
    given_norm: str = ""
    given_phon: str = ""
    role_norms: set[str] = field(default_factory=set)

    def __post_init__(self) -> None:
        self.given_norm = strip_determiners(self.given)
        self.given_phon = phonetic_phrase(self.given_norm) if self.given_norm else ""
        for r in (self.relationship, self.trade):
            rn = strip_determiners(r)
            if rn:
                self.role_norms.add(rn)
                for syn in TRADE_SYNONYMS.get(rn, ()):
                    self.role_norms.add(strip_determiners(syn))
        if self.aliases and not self.compiled_aliases:
            seen: dict[str, float] = {}
            for raw, w in self.aliases:
                an = strip_determiners(raw)
                if not an:
                    continue
                if w > seen.get(an, 0.0):
                    seen[an] = w
            self.compiled_aliases = [
                (an, phonetic_phrase(an), w) for an, w in seen.items()
            ]


class SocialGraphIndex:
    """In-memory index of `npcs.sqlite` social circles, relationships, absentees & quests."""

    def __init__(self, db_path: str | Path):
        self.db_path = Path(db_path)
        con = sqlite3.connect(f"file:{self.db_path}?mode=ro", uri=True)
        con.row_factory = sqlite3.Row

        self.souls: dict[str, SoulNode] = {}
        routine_souls = {
            r[0]
            for r in con.execute(
                "SELECT DISTINCT soul_name FROM npc_routine WHERE pos_x IS NOT NULL"
            )
        }

        for r in con.execute(
            """
            SELECT n.soul_name, n.soul_id, n.display_name, n.is_named, n.gender,
                   n.occupation, n.settlement, n.region,
                   g.given, g.particle, g.byname, g.title, g.full_name,
                   s.ui_name, s.full_name AS skald_full,
                   t.trade, p.circle_id
            FROM npc n
            LEFT JOIN generated_name g ON g.soul_id = n.soul_id
            LEFT JOIN skald_character s ON s.skald_character_name = n.skald_character_name
            LEFT JOIN npc_trade t ON t.soul_name = n.soul_name
            LEFT JOIN npc_person p ON p.soul_name = n.soul_name
            WHERE n.kind = 'human'
            """
        ):
            full_name = (
                r["full_name"]
                or r["ui_name"]
                or r["skald_full"]
                or r["display_name"]
                or r["soul_name"]
            )
            given = r["given"] or ""
            title = r["title"] or ""
            if not given and full_name:
                words = full_name.split()
                idx = 0
                while idx < len(words) and words[idx].lower().rstrip(".") in TITLES:
                    if not title:
                        title = words[idx]
                    idx += 1
                if idx < len(words):
                    given = words[idx]
            self.souls[r["soul_name"]] = SoulNode(
                soul_name=r["soul_name"],
                soul_id=r["soul_id"],
                full_name=full_name,
                given=given,
                particle=r["particle"] or "",
                byname=r["byname"] or "",
                title=title,
                gender=(r["gender"] or "").lower(),
                trade=r["trade"] or "",
                occupation=r["occupation"] or "",
                settlement=r["settlement"] or "",
                region=r["region"] or "",
                circle_id=r["circle_id"] or "",
                is_named=bool(r["is_named"]),
                has_routine=r["soul_name"] in routine_souls,
            )

        self.circle_members: dict[str, list[str]] = {}
        self.soul_bridges: dict[str, list[str]] = {}
        for r in con.execute(
            "SELECT circle_id, soul_name, bridges FROM npc_circle_member ORDER BY rowid"
        ):
            cid = r["circle_id"]
            sname = r["soul_name"]
            self.circle_members.setdefault(cid, []).append(sname)
            if sname in self.souls and not self.souls[sname].circle_id:
                self.souls[sname].circle_id = cid
            if r["bridges"]:
                try:
                    b = json.loads(r["bridges"])
                    if isinstance(b, list):
                        self.soul_bridges[sname] = [str(x) for x in b if x]
                except json.JSONDecodeError:
                    pass

        self.circle_absentees: dict[str, list[AbsenteeNode]] = {}
        for r in con.execute(
            "SELECT circle_id, name, station, why_unmeetable, to_the_circle FROM npc_absentee ORDER BY rowid"
        ):
            words = r["name"].split()
            idx = 0
            while idx < len(words) and words[idx].lower().rstrip(".") in TITLES:
                idx += 1
            given = words[idx] if idx < len(words) else r["name"]
            self.circle_absentees.setdefault(r["circle_id"], []).append(
                AbsenteeNode(
                    circle_id=r["circle_id"],
                    name=r["name"],
                    given=given,
                    station=r["station"] or "",
                    why_unmeetable=r["why_unmeetable"] or "",
                    to_the_circle=r["to_the_circle"] or "",
                )
            )

        self.relationships: dict[str, list[tuple[str, str, str]]] = {}
        for r in con.execute(
            "SELECT soul_name, other_soul_name, relationship, category FROM npc_relationship"
        ):
            if r["other_soul_name"] in self.souls:
                self.relationships.setdefault(r["soul_name"], []).append(
                    (r["other_soul_name"], r["relationship"] or "", r["category"] or "")
                )

        self.quest_peers: dict[str, set[str]] = {}
        for r in con.execute("SELECT quest_id, giver_soul, target_soul FROM npc_quest"):
            g, t = r["giver_soul"], r["target_soul"]
            if g and t:
                self.quest_peers.setdefault(g, set()).add(t)
                self.quest_peers.setdefault(t, set()).add(g)

        # Short circle KB facts per speaker for enriching spoken location replies
        self.speaker_facts: dict[str, list[str]] = {}
        for r in con.execute(
            "SELECT owner, fact FROM npc_kb_item WHERE scope = 'circle' AND fact != ''"
        ):
            self.speaker_facts.setdefault(r["owner"], []).append(r["fact"])

        con.close()
        self._base_cand_cache: dict[str, list[CandidatePerson]] = {}

    def _build_soul_candidate(
        self,
        node: SoulNode,
        hop: int,
        source: str,
        relationship: str = "",
        speaker_facts: Iterable[str] = (),
    ) -> CandidatePerson:
        aliases: list[tuple[str, float]] = []
        if node.full_name:
            aliases.append((node.full_name, 1.00))
        if node.title and node.given:
            aliases.append((f"{node.title} {node.given}", 0.98))
        if node.given:
            aliases.append((node.given, 0.93))
        if node.byname:
            aliases.append((node.byname, 0.92))
            if node.particle:
                aliases.append((f"{node.given} {node.particle} {node.byname}", 1.00))
        if relationship and relationship not in {"acquaintance", "comrade", "household"}:
            aliases.append((relationship, 0.92))
        trade_label = node.trade or node.occupation
        if trade_label and trade_label.lower() not in {"villager", "townsman", "townswoman"}:
            aliases.append((trade_label, 0.87))
            for syn in TRADE_SYNONYMS.get(trade_label.lower(), ()):
                aliases.append((syn, 0.85))

        where_hint = ""
        for f in speaker_facts:
            if node.full_name in f or (node.given and len(node.given) >= 3 and node.given in f):
                where_hint = f
                break

        return CandidatePerson(
            key=node.soul_name,
            soul_name=node.soul_name,
            display_name=node.full_name,
            given=node.given,
            byname=node.byname,
            title=node.title,
            gender=node.gender,
            hop=hop,
            source=source,
            relationship=relationship,
            trade=trade_label,
            settlement=node.settlement,
            is_absentee=False,
            where_hint=where_hint,
            aliases=aliases,
        )

    def candidates_for(
        self, speaker_soul: str, extra_quest_souls: Iterable[str] = ()
    ) -> list[CandidatePerson]:
        """Build ordered Hop-1 and Hop-2 candidate graph around `speaker_soul`."""
        base = self._base_cand_cache.get(speaker_soul)
        if base is None:
            speaker = self.souls.get(speaker_soul)
            if not speaker:
                return []
            facts = self.speaker_facts.get(speaker_soul, [])
            by_key: dict[str, CandidatePerson] = {}

            # 1. Hop-1: Direct relationships (kinship, work, close friends)
            rel_map: dict[str, str] = {}
            for other, rel, _cat in sorted(
                self.relationships.get(speaker_soul, []),
                key=lambda x: CLOSENESS_RANK.get(x[1], 50),
            ):
                if other == speaker_soul or other not in self.souls:
                    continue
                rel_map[other] = rel
                other_node = self.souls[other]
                same_circle = bool(speaker.circle_id and other_node.circle_id == speaker.circle_id)
                hop = 1 if (same_circle or CLOSENESS_RANK.get(rel, 50) <= 8) else 2
                by_key[other] = self._build_soul_candidate(
                    other_node,
                    hop=hop,
                    source="circle" if same_circle else "relationship",
                    relationship=rel,
                    speaker_facts=facts,
                )

            # 2. Hop-1: Co-circle members
            if speaker.circle_id:
                for member_soul in self.circle_members.get(speaker.circle_id, []):
                    if member_soul == speaker_soul or member_soul not in self.souls:
                        continue
                    if member_soul in by_key:
                        by_key[member_soul].hop = 1
                        by_key[member_soul].source = "circle"
                    else:
                        by_key[member_soul] = self._build_soul_candidate(
                            self.souls[member_soul],
                            hop=1,
                            source="circle",
                            relationship=rel_map.get(member_soul, ""),
                            speaker_facts=facts,
                        )

                # 3. Hop-1: Circle absentees (dead, moved away, locked indoors)
                for ab in self.circle_absentees.get(speaker.circle_id, []):
                    ab_key = f"absentee:{ab.name}"
                    aliases = [(ab.name, 1.00), (ab.given, 0.93)]
                    if ab.station:
                        aliases.append((ab.station, 0.87))
                    gender = ""
                    st_low = (ab.station + " " + ab.to_the_circle).lower()
                    if any(w in st_low for w in ("woman", "girl", "widow", "daughter", "wife", "maid", "she ", "her ")):
                        gender = "female"
                    elif any(w in st_low for w in ("man", "boy", "son", "husband", "father", "he ", "his ")):
                        gender = "male"
                    by_key[ab_key] = CandidatePerson(
                        key=ab_key,
                        soul_name="",
                        display_name=ab.name,
                        given=ab.given,
                        byname="",
                        title="",
                        gender=gender,
                        hop=1,
                        source="absentee",
                        trade=ab.station,
                        settlement=speaker.settlement,
                        is_absentee=True,
                        why_unmeetable=ab.why_unmeetable,
                        where_hint=ab.to_the_circle,
                        aliases=aliases,
                    )

            # 4. Hop-2: Bridge souls & their circles + cross-circle relationship neighbors
            adjacent_circles: set[str] = set()
            circle_souls = (
                self.circle_members.get(speaker.circle_id, [speaker_soul])
                if speaker.circle_id
                else [speaker_soul]
            )
            for ms in circle_souls:
                for bridge_soul in self.soul_bridges.get(ms, []):
                    if bridge_soul != speaker_soul and bridge_soul in self.souls:
                        bnode = self.souls[bridge_soul]
                        if bridge_soul not in by_key:
                            by_key[bridge_soul] = self._build_soul_candidate(
                                bnode, hop=2, source="bridge", speaker_facts=facts
                            )
                        if bnode.circle_id and bnode.circle_id != speaker.circle_id:
                            adjacent_circles.add(bnode.circle_id)
                for other, rel, _cat in self.relationships.get(ms, []):
                    if other != speaker_soul and other in self.souls:
                        onode = self.souls[other]
                        if other not in by_key:
                            by_key[other] = self._build_soul_candidate(
                                onode, hop=2, source="adjacent", speaker_facts=facts
                            )
                        if onode.circle_id and onode.circle_id != speaker.circle_id:
                            adjacent_circles.add(onode.circle_id)

            for adj_cid in adjacent_circles:
                for adj_soul in self.circle_members.get(adj_cid, []):
                    if adj_soul != speaker_soul and adj_soul in self.souls and adj_soul not in by_key:
                        by_key[adj_soul] = self._build_soul_candidate(
                            self.souls[adj_soul], hop=2, source="adjacent", speaker_facts=facts
                        )

            # 5. Hop-2: Static DB quest peers
            for qs in self.quest_peers.get(speaker_soul, set()):
                if qs != speaker_soul and qs in self.souls and qs not in by_key:
                    by_key[qs] = self._build_soul_candidate(
                        self.souls[qs], hop=2, source="quest", speaker_facts=facts
                    )

            base = list(by_key.values())
            self._base_cand_cache[speaker_soul] = base

        extra = [q for q in extra_quest_souls if q]
        if not extra:
            return base
        existing = {c.key for c in base}
        facts = self.speaker_facts.get(speaker_soul, [])
        out = list(base)
        for qs in extra:
            if qs != speaker_soul and qs in self.souls and qs not in existing:
                existing.add(qs)
                out.append(
                    self._build_soul_candidate(
                        self.souls[qs], hop=2, source="quest", speaker_facts=facts
                    )
                )
        return out

    def salience_ordered_keyterms(
        self,
        speaker_soul: str,
        history: list[MessageTurn] | None = None,
        met_counts: dict[str, int] | None = None,
        known_tokens: list[str] | None = None,
        quest_souls: list[str] | None = None,
        hop1_only: bool = False,
        max_terms: int = 35,
    ) -> list[str]:
        """Return salience-ordered proper names for Moonshine STT `set_keyterms`.

        Pass 1 uses `hop1_only=True, max_terms=30` (compact, zero WER degradation).
        Pass 2 uses `hop1_only=False, max_terms=95` (includes adjacent circles).
        """
        history = history or []
        met_counts = met_counts or {}
        known_set = set(known_tokens or [])
        quest_set = set(quest_souls or [])

        cands = self.candidates_for(speaker_soul, quest_set)
        if hop1_only:
            cands = [c for c in cands if c.hop == 1 or c.soul_name in quest_set]

        scored: list[tuple[float, CandidatePerson]] = []
        for c in cands:
            prior = self._salience_bonus(c, history, met_counts, known_set, quest_set)
            scored.append((prior, c))
        scored.sort(key=lambda x: x[0], reverse=True)

        terms: list[str] = []
        seen: set[str] = set()
        speaker = self.souls.get(speaker_soul)
        if speaker and speaker.full_name:
            terms.append(speaker.full_name)
            seen.add(speaker.full_name.lower())

        for _, c in scored:
            for term in (c.display_name, c.given, c.byname):
                t = term.strip()
                if len(t) < 3 or t.lower() in seen:
                    continue
                seen.add(t.lower())
                terms.append(t)
                if len(terms) >= max_terms:
                    return terms
        return terms

    @staticmethod
    def _history_recency(cand: CandidatePerson, history: list[MessageTurn]) -> float:
        if not history:
            return 0.0
        for rev_idx, turn in enumerate(reversed(history)):
            txt = turn.content
            for name in (cand.display_name, cand.given, cand.byname):
                if not name or len(name) < 3:
                    continue
                if re.search(rf"\b{re.escape(name)}\b", txt, flags=re.IGNORECASE):
                    return max(0.14, 0.22 - 0.02 * rev_idx)
        return 0.0

    def _salience_bonus(
        self,
        cand: CandidatePerson,
        history: list[MessageTurn],
        met_counts: dict[str, int],
        known_set: set[str],
        quest_set: set[str],
    ) -> float:
        bonus = self._history_recency(cand, history)
        if cand.soul_name and cand.soul_name in quest_set:
            bonus += 0.13
        met = met_counts.get(cand.soul_name, 0) if cand.soul_name else 0
        if met > 0:
            bonus += 0.08 + 0.015 * min(met, 3)
        elif cand.soul_name in known_set or f"name:{cand.given.lower()}" in known_set:
            bonus += 0.06

        # Social closeness to speaker
        if cand.hop == 1:
            if cand.relationship in {"wife", "husband", "son", "daughter", "father", "mother", "brother", "sister"}:
                bonus += 0.10
            elif cand.source == "circle":
                bonus += 0.08
            elif cand.source == "absentee":
                bonus += 0.07
            else:
                bonus += 0.06
        else:
            if cand.source in {"bridge", "quest"}:
                bonus += 0.035
            else:
                bonus += 0.01
        return bonus

    def _resolve_pronoun(
        self,
        mention: str,
        cands: list[CandidatePerson],
        history: list[MessageTurn],
    ) -> CandidatePerson | None:
        m_low = strip_determiners(mention)
        want_gender = ""
        if m_low in FEMALE_PRONOUNS:
            want_gender = "female"
        elif m_low in MALE_PRONOUNS:
            want_gender = "male"

        if not history:
            return None

        # Centering-Theory antecedent scoring across recent turns (last 4 messages):
        #  - Player's explicit question topic (`user` turn) carries strong discourse focus
        #  - Subject-initial mention in `assistant` reply outranks later oblique objects
        #  - Full-name match outranks bare given-name match when homonyms exist
        scores: dict[str, tuple[float, CandidatePerson]] = {}
        recent = history[-4:]
        n_recent = len(recent)
        for idx_t, turn in enumerate(recent):
            recency_weight = 1.0 + 0.35 * idx_t / max(n_recent - 1, 1)
            role_base = 2.4 if turn.role == "user" else 1.5
            for c in cands:
                if want_gender and c.gender and c.gender != want_gender:
                    continue
                best_pos: int | None = None
                full_hit = False
                for probe_idx, probe in enumerate((c.display_name, c.given)):
                    if not probe or len(probe) < 3:
                        continue
                    m = re.search(rf"\b{re.escape(probe)}\b", turn.content, flags=re.IGNORECASE)
                    if m:
                        best_pos = m.start()
                        if probe_idx == 0 and " " in probe:
                            full_hit = True
                        break
                if best_pos is not None:
                    # Earlier syntactic position in turn -> likely grammatical subject
                    pos_bonus = max(0.0, 0.7 - (best_pos / max(len(turn.content), 40)) * 0.7)
                    specificity = 0.35 if full_hit else 0.0
                    hop_bonus = 0.25 if c.hop == 1 else 0.0
                    turn_score = (role_base + pos_bonus + specificity + hop_bonus) * recency_weight
                    prev = scores.get(c.key, (0.0, c))[0]
                    scores[c.key] = (prev + turn_score, c)

        if not scores:
            return None
        return max(scores.values(), key=lambda x: x[0])[1]

    def resolve_place(self, mention: str) -> tuple[str, str, float]:
        """Match destination mention against `CANONICAL_PLACES`."""
        best_id = ""
        best_name = ""
        best_score = 0.0
        q = strip_determiners(mention)
        if not q:
            return "", "", 0.0
        for pid, meta in CANONICAL_PLACES.items():
            names = [meta["name"]] + list(meta.get("aliases", []))
            for nm in names:
                sc = string_and_phonetic_similarity(q, nm)
                if sc > best_score:
                    best_score = sc
                    best_id = pid
                    best_name = meta["name"]
        if best_score >= 0.78:
            return best_id, best_name, best_score
        return "", "", best_score

    def resolve(
        self,
        speaker_soul: str,
        extracted: ExtractedCommand,
        history: list[MessageTurn] | None = None,
        met_counts: dict[str, int] | None = None,
        known_tokens: list[str] | None = None,
        quest_souls: list[str] | None = None,
        kb_gated: bool = False,
        use_phonetics: bool = True,
        use_salience: bool = True,
        hop1_only: bool = False,
    ) -> PipelineResult:
        """Link an `ExtractedCommand` to a concrete `soul_name`, `place_id`, or abstention."""
        t0 = time.perf_counter()
        history = history or []
        met_counts = met_counts or {}
        known_set = set(known_tokens or [])
        quest_set = set(quest_souls or [])

        if extracted.action == CommandAction.NO_ACTION:
            return PipelineResult(
                action=CommandAction.NO_ACTION,
                status=ResolutionStatus.NO_ACTION,
                latency_ms=(time.perf_counter() - t0) * 1000.0,
            )

        mention = extracted.target_mention.strip()
        if not mention:
            return PipelineResult(
                action=CommandAction.NO_ACTION,
                status=ResolutionStatus.NO_ACTION,
                latency_ms=(time.perf_counter() - t0) * 1000.0,
            )

        # Travel command -> resolve against KCD2 places
        if extracted.action == CommandAction.TRAVEL_TO:
            pid, pname, sc = self.resolve_place(mention)
            if not pid:
                return PipelineResult(
                    action=CommandAction.TRAVEL_TO,
                    status=ResolutionStatus.UNKNOWN_TO_NPC,
                    extracted_mention=mention,
                    confidence=sc,
                    dialogue_direction=(
                        f"[Henry asked to travel to '{mention}', which is not a place you can take him to.]"
                    ),
                    latency_ms=(time.perf_counter() - t0) * 1000.0,
                )
            if kb_gated:
                return PipelineResult(
                    action=CommandAction.TRAVEL_TO,
                    status=ResolutionStatus.GATED_BY_TRUST,
                    place_id="",
                    matched_name=pname,
                    extracted_mention=mention,
                    confidence=sc,
                    latency_ms=(time.perf_counter() - t0) * 1000.0,
                )
            return PipelineResult(
                action=CommandAction.TRAVEL_TO,
                status=ResolutionStatus.RESOLVED_PLACE,
                place_id=pid,
                matched_name=pname,
                extracted_mention=mention,
                confidence=sc,
                directive=f"@travel {pid}",
                dialogue_direction=f"[You agree to go with Henry to {pname}.]",
                latency_ms=(time.perf_counter() - t0) * 1000.0,
            )

        # Special case for FOLLOW_NPC: "Follow me" / "Come with me"
        if extracted.action == CommandAction.FOLLOW_NPC and strip_determiners(mention) in {
            "me",
            "henry",
            "us",
        }:
            if kb_gated:
                return PipelineResult(
                    action=CommandAction.FOLLOW_NPC,
                    status=ResolutionStatus.GATED_BY_TRUST,
                    matched_name="Henry",
                    extracted_mention=mention,
                    confidence=1.0,
                    latency_ms=(time.perf_counter() - t0) * 1000.0,
                )
            return PipelineResult(
                action=CommandAction.FOLLOW_NPC,
                status=ResolutionStatus.RESOLVED_SOUL,
                soul_name="player",
                matched_name="Henry",
                extracted_mention=mention,
                confidence=1.0,
                directive="@follow player",
                dialogue_direction="[You agree to follow Henry.]",
                latency_ms=(time.perf_counter() - t0) * 1000.0,
            )

        cands = self.candidates_for(speaker_soul, quest_set)
        if hop1_only:
            cands = [c for c in cands if c.hop == 1 or c.soul_name in quest_set]

        # Self-reference guard ("Where are you?")
        speaker = self.souls.get(speaker_soul)
        if strip_determiners(mention) in {"you", "yourself"} or (
            speaker and normalize_text(mention) == normalize_text(speaker.full_name)
        ):
            return PipelineResult(
                action=CommandAction.NO_ACTION,
                status=ResolutionStatus.NO_ACTION,
                extracted_mention=mention,
                latency_ms=(time.perf_counter() - t0) * 1000.0,
            )

        # Pronoun resolution via conversation history
        if extracted.is_pronoun or strip_determiners(mention) in (FEMALE_PRONOUNS | MALE_PRONOUNS | {"they", "them"}):
            ant = self._resolve_pronoun(mention, cands, history)
            if ant is None:
                return PipelineResult(
                    action=extracted.action,
                    status=ResolutionStatus.UNKNOWN_TO_NPC,
                    extracted_mention=mention,
                    confidence=0.0,
                    dialogue_direction="[You are not sure whom Henry means; ask him who he is talking about.]",
                    latency_ms=(time.perf_counter() - t0) * 1000.0,
                )
            return self._finalize_candidate(
                extracted.action, ant, mention, 0.95, kb_gated, t0
            )

        # Score every candidate in Hop-1 & Hop-2
        scored: list[tuple[float, float, CandidatePerson]] = []
        q_clean = strip_determiners(mention)
        q_phon = phonetic_phrase(q_clean) if use_phonetics else ""
        # Strip leading title from query if candidate doesn't have one, while also scoring with title
        q_words = q_clean.split()
        q_notitle = " ".join(q_words[1:]) if len(q_words) > 1 and q_words[0] in TITLES else q_clean
        q_notitle_phon = (
            phonetic_phrase(q_notitle) if (use_phonetics and q_notitle != q_clean) else q_phon
        )
        q_nt_words = q_notitle.split()
        q_first = q_nt_words[0] if q_nt_words else ""
        q_first_phon = phonetic_key(q_first) if (use_phonetics and q_first) else ""
        q_rest = strip_determiners(" ".join(q_nt_words[1:])) if len(q_nt_words) > 1 else ""
        q_rest_phon = phonetic_phrase(q_rest) if (use_phonetics and q_rest) else ""

        for c in cands:
            # Multi-word proper-name consistency check: prevent surname-only or
            # bare-given collisions (e.g. "Gilbert Miller" != "Cecily Miller",
            # "Andreas the Ox" != bare "Andrew").
            if len(q_nt_words) >= 2 and c.given_norm:
                is_role_match = any(
                    _fast_similarity(
                        q_notitle,
                        q_notitle_phon,
                        rn,
                        phonetic_phrase(rn) if use_phonetics else "",
                        use_phonetics=use_phonetics,
                    )
                    >= 0.84
                    for rn in c.role_norms
                )
                if not is_role_match:
                    given_sim = _fast_similarity(
                        q_first,
                        q_first_phon,
                        c.given_norm,
                        c.given_phon,
                        use_phonetics=use_phonetics,
                    )
                    if given_sim < 0.76:
                        continue
                    # Candidate has only a 1-token name while query specifies a distinct byname/epithet
                    if not c.byname and " " not in strip_determiners(c.display_name):
                        epithet_matches_role = any(
                            _fast_similarity(
                                q_rest,
                                q_rest_phon,
                                rn,
                                phonetic_phrase(rn) if use_phonetics else "",
                                use_phonetics=use_phonetics,
                            )
                            >= 0.78
                            for rn in c.role_norms
                        )
                        if not epithet_matches_role:
                            continue

            best_surface = 0.0
            for alias_norm, alias_phon, weight in c.compiled_aliases:
                sim = _fast_similarity(
                    q_clean, q_phon, alias_norm, alias_phon, use_phonetics=use_phonetics
                )
                if q_notitle != q_clean:
                    sim = max(
                        sim,
                        _fast_similarity(
                            q_notitle,
                            q_notitle_phon,
                            alias_norm,
                            alias_phon,
                            use_phonetics=use_phonetics,
                        )
                        * 0.97,
                    )
                if sim >= 0.68:
                    best_surface = max(best_surface, sim * weight)
                else:
                    best_surface = max(best_surface, sim * 0.85)

            if best_surface < 0.66:
                continue
            salience = (
                self._salience_bonus(c, history, met_counts, known_set, quest_set)
                if use_salience
                else 0.0
            )
            total = best_surface + salience
            scored.append((total, best_surface, c))

        if not scored:
            return PipelineResult(
                action=extracted.action,
                status=ResolutionStatus.UNKNOWN_TO_NPC,
                extracted_mention=mention,
                confidence=0.0,
                dialogue_direction=(
                    f"[You do not know anyone called '{mention}' or where to find them; say so plainly.]"
                ),
                latency_ms=(time.perf_counter() - t0) * 1000.0,
            )

        scored.sort(key=lambda x: (x[0], x[1]), reverse=True)
        top_total, top_surface, top_cand = scored[0]

        if top_surface < 0.72:
            return PipelineResult(
                action=extracted.action,
                status=ResolutionStatus.UNKNOWN_TO_NPC,
                extracted_mention=mention,
                confidence=top_surface,
                dialogue_direction=(
                    f"[You do not know anyone called '{mention}' or where to find them; say so plainly.]"
                ),
                latency_ms=(time.perf_counter() - t0) * 1000.0,
            )

        # Check for true ambiguity (e.g. two 'John's with near-identical total score)
        if len(scored) >= 2:
            runner_total, runner_surface, runner_cand = scored[1]
            if (
                runner_cand.key != top_cand.key
                and runner_surface >= 0.85
                and (top_total - runner_total) < 0.025
            ):
                return PipelineResult(
                    action=extracted.action,
                    status=ResolutionStatus.AMBIGUOUS_SOUL,
                    matched_name=f"{top_cand.display_name} / {runner_cand.display_name}",
                    extracted_mention=mention,
                    confidence=top_surface,
                    dialogue_direction=(
                        f"[You know both {top_cand.display_name} and {runner_cand.display_name}; "
                        f"ask Henry briefly which one he means.]"
                    ),
                    latency_ms=(time.perf_counter() - t0) * 1000.0,
                )

        return self._finalize_candidate(
            extracted.action, top_cand, mention, top_surface, kb_gated, t0
        )

    def _finalize_candidate(
        self,
        action: CommandAction,
        cand: CandidatePerson,
        mention: str,
        confidence: float,
        kb_gated: bool,
        t0: float,
    ) -> PipelineResult:
        if cand.is_absentee:
            return PipelineResult(
                action=action,
                status=ResolutionStatus.UNMEETABLE_ABSENTEE,
                soul_name="",
                matched_name=cand.display_name,
                extracted_mention=mention,
                absentee_reason=cand.why_unmeetable,
                confidence=confidence,
                dialogue_direction=(
                    f"[{cand.display_name} cannot be reached: {cand.why_unmeetable} "
                    f"{cand.where_hint}]"
                ).strip(),
                latency_ms=(time.perf_counter() - t0) * 1000.0,
            )

        if kb_gated:
            return PipelineResult(
                action=action,
                status=ResolutionStatus.GATED_BY_TRUST,
                soul_name="",
                matched_name=cand.display_name,
                extracted_mention=mention,
                confidence=confidence,
                latency_ms=(time.perf_counter() - t0) * 1000.0,
            )

        verb_dir = (
            f"@locate {cand.soul_name}"
            if action == CommandAction.LOCATE_NPC
            else f"@follow {cand.soul_name}"
        )
        place_clause = f" around {cand.settlement}" if cand.settlement else ""
        fact_clause = f" ({cand.where_hint})" if cand.where_hint else ""
        if action == CommandAction.LOCATE_NPC:
            direction = (
                f"[You know {cand.display_name}{place_clause}{fact_clause}; "
                f"tell Henry briefly where to look for them — you have marked their whereabouts for him.]"
            )
        else:
            direction = f"[You agree to follow {cand.display_name}.]"

        return PipelineResult(
            action=action,
            status=ResolutionStatus.RESOLVED_SOUL,
            soul_name=cand.soul_name,
            matched_name=cand.display_name,
            extracted_mention=mention,
            confidence=confidence,
            directive=verb_dir,
            dialogue_direction=direction,
            latency_ms=(time.perf_counter() - t0) * 1000.0,
        )
