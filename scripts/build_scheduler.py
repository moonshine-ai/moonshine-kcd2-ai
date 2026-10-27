#!/usr/bin/env python3
"""Build the level's scheduler override for the quests that change a giver's day.

A quest with a `routine` (npc_quest_lines; FavourSpec.routine in kcd2_stories.py -- Joan's
"crying" in Forget Me Not) wants its giver at a smart-object behaviour instead of her work
until Henry takes the errand up: the accept is what gives her hope, and from it she is at
her chores again. The game's own quests do this with tagged links on the
NPC that a `SchedulerLinkActivation` node switches; a mod cannot add links (behaviors.md
§4.3, §4.6), but it can ship the level's whole `tables/ai/scheduler.xml` with two of the
giver's own links edited, and a tag on each that the quest graph flips (emit.cpp
routineLink; the tag is routine_tag() here and emit.cpp's routine_tag, which must agree):

  - her NPC hub's work link is pointed at the nearest wrapper hub with the behaviour (the
    bench), its window and priority kept, with the tag normal and Urgency Slow: live only
    while the tag is on, which the graph holds while the quest is still None;
  - a spare link on her home hub (the morning one) is pointed at her work hub, for the
    work hours, at a priority above the home hub's own, with the tag *inverted*: her way
    to work once the tag is off (accepted, done or failed).

Tag off, every other NPC is untouched and she reaches her work by way of home, as many
villagers do. Tag on, the NPC hub's own best link is the bench; HaltOnInvalid on both means
the flip either way moves her at once. Proven on Joan in play (7
Oct 2026, §4.6; scripts/quest_pass.sh --road find --how routine). Other behaviours and
other givers resolve the same way, from the database's npc_routine_hub and the table.

The base table is 63 MB inside Data/Levels/<level>/level.pak on the machine that runs the
game, and the one it ships must be *that* machine's (a patch changes the table). So this is
run where the game is, with --game naming its root (scripts/build_mod.sh does that), or
against a table extracted elsewhere with --scheduler. Everything it uses is stdlib.

    scripts/build_scheduler.py --print                          # the specs, from the DB
    scripts/build_scheduler.py --scheduler /tmp/scheduler.xml --out /tmp/x.pak
    scripts/build_scheduler.py --game 'C:/Program Files (x86)/Steam/steamapps/common/KingdomComeDeliverance2' \\
        --out out/Data/Levels/kutnohorsko/moonshine_names.pak

A second kind of spec, `routine_day` (npc_quest_lines; 9 Oct 2026 -- Benesh Bell of Holy
Jest, whose bio has him on his knees while the priest is at the tavern, and whose game day
is a filler townsman's), is the giver's day for the whole game, not the quest's span: his
NPC hub's work link is pointed at a wrapper for the working hours with no tag at all, so
nothing in the graph is involved and the home hub is left alone. Its text is
`<BehaviorName>@<wrapper prefab>[@<x>,<y>]` as npc_routine's `behavior` column spells a
behaviour (`useKneelingGround@profession/parson/praying@3316,797`), the prefab picking the
wrapper among the many that carry the same BehaviorName: the one nearest home among the
hubs standing on an object of that prefab (npc_routine_object), within DAY_WRAPPER_METRES
-- or, with a point given, among that prefab's objects within POINT_METRES of the point
(Benesh's nearest praying spots are a burgher's upstairs oratory; the ten on the square
by the church are the public ones). A filler soul (the game's additive NPCs) has one link
on his NPC hub, to a home hub shared with a dozen others, and no work link of his own: for
him that one link is the one retargeted, for the home hub's working hours, so outside them
he is as the game had him.

A third kind, `routine_bed_target` (npc_quest_lines; 21 Oct 2026 -- Father Ernest of Words
Must Not Fail, whom Henry is to find in Matilda Eichner's bed), moves where the quest's
target sleeps, for good: `<bed prefab>@<x>,<y>` names a bed by its prefab and the point it
stands at (npc_routine_object, one side of a double bed within OBJECT_METRES of the point),
and the sleeper wrapper hub whose `use`/`sleeper` link uses that side is what the target's
*home hub* sleep links (BehaviorName="sleep", the mat or bed the game gave him) are pointed
at instead, as the house hub of the bed's own household points at it (Attraction 720, the
behaviour and role left to the wrapper's own links). The rest of the home hub -- his day
links, the wake-up -- is left alone, so by day he is where the game had him. The NPC hub
and the graph are untouched; proven in play by the same retarget on his NPC hub's home
link (21 Oct 2026: Ernest Lying on the bed's left side from 21:15, Matilda on the right
from 23:30, the husband displaced to another bed, no aggression).

With the street written on the same quest (a `followed` line; npcdb.hpp has_follow, notes.md
23 Oct 2026 n8: the holder catching Henry behind him on his way to her), the two sleep links
split under the graph's alert tag (emit.cpp alert_tag, the same name here) instead of both
going to her bed: the plain one to her sleeper wrapper under the tag inverted, with
HaltOnInvalid, so the night the graph turns the tag on he turns round in the street; the
low_health one kept on his own mat with the buff condition dropped and the plain one's
priority, under the tag normal, without HaltOnInvalid, so he sleeps at home that night and
the tag's dropping in the morning (the graph's rest Timer) leaves a sleeping man where he
lies. The cost is the wounded variant: hurt, he sleeps at hers like any night.

And the bed is made theirs alone (claim_bed; 26 Oct 2026, notes.md: "When I caught Ernest
this time he was in bed with the master, not the woman I'd expect"): the witness's
household bed hub gave the double bed's two sides to two classes at one priority (the
housekeeper and the weaponsmith, either side each), so a night the pair were got out of bed
again -- Henry in the house before the catching is trespass -- the master could take the
free side beside the priest. Every plain link of a foreign hub to either side's sleeper
wrapper is retargeted by class: the witness's (`seen_with`'s npc.social_class, on the spec
as witness_class) to the side the holder does not use, every other to the hub's nearest
other sleeper bed, parameters kept; BuffTag and tagged links untouched. Replayed from the
user's save: Ernest side A 20:58, Klaus in the far bed 21:08, Matilda side B 21:13.

A fourth, `routine_bed_by` (npc_quest_lines; 22 Oct 2026 -- notes.md: "Matilda should be
in bed before Father Ernest"), is the witness's bedtime, `HH:MM`, resolved from the quest's
`seen_with` soul: her home hub's evening links (the ones that run past the minute; Matilda's
is a 19:00-23:00 eat link at priority 1, which beats her all-day sleep link until it ends)
are cut to end at the minute, TimeVariation 0 and HaltOnInvalid true, so from then on the
all-day sleep link is the best she has and she goes to the bed. HaltOnInvalid is the half
that matters: without it the window's end is no end (she finishes what the link started;
cut to 21:30 without it, Matilda sat down to her mash at 21:23 and was still eating at
23:10), with it she stood up two minutes after the cut and was lying in the bed eleven
minutes later (probes, 23 Oct 2026; 21:00 shipped, since Ernest's own bedtime wanders
between 21:45 and 22:25). A link that *starts* after the minute is an error (cutting it is
not written), as is a home hub with no all-day link to fall back on. Nothing else on the
hub changes, so her morning is as the game had it.

A fifth, `routine_home_at` (npc_quest_lines, `HH:MM`; 25 Oct 2026 -- notes.md: "Ernest says
'out here in the dark' but it isn't dark yet, please delay his go to bed routine by an
hour"), is the holder's homecoming, resolved from the quest's target soul: the day links on
his home hub that end last (Ernest's two to his church hub, 08:00-20:00 with an hour's
variation and 08:30-19:50 with twenty minutes') are moved to end at the minute, variation
kept, HaltOnInvalid left as it was, so the all-day sleep link under them takes him to the
bed that much later (21:00 shipped: his walk to Radnicni was 20:20-20:50 and he was lying
by 20:53 under the base table's 20:00). A day link starting after the minute, or another
link running over it, is an error.

And the untagged work link opens at the earliest the home hub's morning link can end
(resolve, `work_start_early`; 25 Oct 2026, notes.md: "I still found her in bed on one
run"): the link is rewritten with TimeVariation 0 and had kept its own start sharp, but
Christine's morning (06:00-08:00) ends with the game's thirty minutes of jitter, and
between a morning that ended at 07:35 and a work link that opened at 08:00 the best she
had was the home hub's all-day sleep link -- a probe had her lying down at 08:00 on the
dot. Opened at 07:30 there is no gap: a late morning keeps her (HaltOnInvalid false) and
the work link, valid by then, takes her when it is over.

Except that it did not (26 Oct 2026, notes.md: "She's still in bed on my latest run"), and
the reason is the half of the scheduler the gap theory left out: a link that comes valid
while the soul is in the middle of something does not take her off it, and when that
something ends the replan belongs to the hub that owned it. A probe watched Christine eat
her mash from 06:05 to 08:20 with the work link valid from 07:30 and never looked at, then
the morning link over, lie down in her home hub's all-day bed and stay there until the
home hub's own 19:00 link got her up for the tavern -- the NPC hub never reconsidered.
What lifts a soul out of a running activity when a link opens is the link's `Urgency`
(ROUTINE_URGENCY, Fast: the innkeepers' closing-up link, a timed prio-6 routine link,
carries it; 9,927 shipped links carry one), so the routine_day work link has it. With it
she left the bowl within five game minutes of 07:30 on three probes, fetched the broom
and was sweeping at the step from 08:00 (cm7, cm7b, cm8).

Also from the same day: a routine_day's wrapper has to stand *on* an object of the named
prefab (ON_OBJECT_METRES) before one merely beside it (OBJECT_METRES) is considered, since
the leaning spot by Christine's step is 3.7 m from her broom, nearer her home, and took the
broom's spec; and a point in the spec is a filter (POINT_METRES about it), not the pick,
so where two objects of the prefab stand 13 m apart the point goes where only the one
meant is within reach (Christine's `@3218,805`, north of the house).

And the wrapper is made the soul's alone (24 Oct 2026, notes.md: Christine found in bed at
eleven in the morning with somebody else sweeping at her step). The hub her work link is
pointed at was one of the household's shared chores: a hub of another NPC's with a
OneTimeUse link to the same wrapper claims it for the day, her own link finds it taken, and
her home hub's all-day fallback is her bed. `claim_wrapper` finds every other hub with a
link to the wrapper and points that link at another hub of the same prefab within reach of
the prefab's objects that the hub already links to (its sibling chore: a broom spot a few
metres off); a hub with no sibling is left shared and the report says so.

Nothing to do (no quest has a routine on the level) writes no pak and says so, and a
spec that cannot be resolved against the table is an error, not a silent skip: the graph
would flip a tag no link carries and the giver would stop greeting Henry for nothing.
"""

from __future__ import annotations

import argparse
import json
import math
import re
import sqlite3
import struct
import sys
import zipfile
import zlib
from pathlib import Path
from xml.etree import ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_DB = ROOT / "data/kcd2/npcs.sqlite"
DEFAULT_LEVEL = "kutnohorsko"
DEFAULT_MODID = "moonshine_names"
TABLE_IN_PAK = "tables/ai/scheduler.xml"

# The retargeted link's priority: above anything a home hub carries itself (0-2) and
# well under the quest links the game writes (40+), so a real quest's `holdingWait` or a
# `fightInQuestSkirmish` still takes her off the bench.
ROUTINE_PRIORITY = 10
# What the base data puts on an area->wrapper link; the proven value.
ROUTINE_ATTRACTION = 120
# How fast a routine_day's work link is taken up once it opens (the innkeepers' closing-up
# link's value); without it a soul mid-meal at the opening is left to her home hub's bed.
ROUTINE_URGENCY = "Fast"
# What the base data puts on a house hub's link to a sleeper wrapper (routine_bed_target).
BED_ATTRACTION = 720
# How far from the giver's home a wrapper may be and still be hers.
MAX_WRAPPER_METRES = 60.0
# How far a wrapper hub may stand from the object its prefab names (routine_day), and how
# near it has to be to count as standing on it (preferred; a tavern table's hub is 1.25 m off).
OBJECT_METRES = 4.0
ON_OBJECT_METRES = 1.5
# A whole day's wrapper may be a walk away (a church), where a bench has to be by the gate.
DAY_WRAPPER_METRES = 200.0
# With a point named, the objects that count are the ones about it.
POINT_METRES = 20.0


def routine_tag(modid: str, quest_id: str) -> str:
    """emit.cpp routine_tag(): the LinkTag the quest graph flips."""
    return f"{modid}_routine_{quest_id}"


def alert_tag(modid: str, quest_id: str) -> str:
    """emit.cpp alert_tag(): the LinkTag the graph holds on for a night the holder has caught
    Henry following him (has_follow), under which his sleep links split."""
    return f"{modid}_alert_{quest_id}"


# ------------------------------------------------------------------ specs (needs the DB)


def read_specs(db: Path, level: str, modid: str) -> list[dict]:
    conn = sqlite3.connect(f"file:{db}?mode=ro", uri=True)
    rows = conn.execute(
        "SELECT l.quest_id, l.text, q.giver_soul, q.giver_name, h.hub_guid, "
        "COALESCE((SELECT gender FROM npc WHERE soul_name = q.giver_soul LIMIT 1), '') "
        "FROM npc_quest_lines l JOIN npc_quest q USING (quest_id) "
        "LEFT JOIN npc_routine_hub h ON h.soul_name = q.giver_soul AND h.level = ? "
        "WHERE l.field = 'routine' AND l.text <> '' ORDER BY l.quest_id", (level,)).fetchall()
    specs = []
    for quest_id, behaviour, soul, name, hub, gender in rows:
        if not hub:
            # The giver lives on another level, or was never dumped (build_kcd2_routine.py):
            # nothing this level's table can do, and said so rather than failed, since a
            # trosecko giver is not this pak's business.
            print(f"  {quest_id}: {name} ({soul}) has no scheduler hub on {level}; skipped", file=sys.stderr)
            continue
        specs.append({"quest_id": quest_id, "behaviour": behaviour, "soul": soul, "name": name,
                      "hub": hub, "gender": gender.lower(), "tag": routine_tag(modid, quest_id)})
    # The souls whose whole day is the story's (routine_day): behaviour@prefab, and the
    # positions of that prefab's objects on the level, for the far side to find the hub by.
    # The giver's, and -- for a retrieve whose holder is at his work all day and whose
    # house-girl is out front (In the Stocks, notes.md 13 Oct 2026) -- the target's
    # (routine_day_target) and the helper's (routine_day_helper, the soul named by the
    # quest's helper_soul line).
    rows = []
    for field, soul_sql, name_sql in (
        ("routine_day", "q.giver_soul", "q.giver_name"),
        ("routine_day_target", "q.target_soul", "q.target_name"),
        ("routine_day_helper",
         "(SELECT text FROM npc_quest_lines WHERE quest_id = l.quest_id AND field = 'helper_soul')",
         "(SELECT text FROM npc_quest_lines WHERE quest_id = l.quest_id AND field = 'helper_name')"),
    ):
        rows += [(field, *row) for row in conn.execute(
            f"SELECT l.quest_id, l.text, {soul_sql}, {name_sql}, h.hub_guid, "
            f"COALESCE((SELECT gender FROM npc WHERE soul_name = {soul_sql} LIMIT 1), '') "
            "FROM npc_quest_lines l JOIN npc_quest q USING (quest_id) "
            f"LEFT JOIN npc_routine_hub h ON h.soul_name = {soul_sql} AND h.level = ? "
            f"WHERE l.field = '{field}' AND l.text <> '' ORDER BY l.quest_id", (level,)).fetchall()]
    for field, quest_id, text, soul, name, hub, gender in rows:
        if not soul:
            raise SystemExit(f"{quest_id}: {field} names nobody (no soul for it in npc_quest / helper_soul)")
        if not hub:
            print(f"  {quest_id}: {name} ({soul}) has no scheduler hub on {level}; skipped", file=sys.stderr)
            continue
        parts = text.split("@")
        if len(parts) not in (2, 3):
            raise SystemExit(f"{quest_id}: {field} wants <BehaviorName>@<wrapper prefab>[@x,y], got {text!r}")
        behaviour, prefab = parts[0], parts[1]
        objects = [(x, y) for x, y in conn.execute(
            "SELECT pos_x, pos_y FROM npc_routine_object WHERE level = ? AND prefab = ?", (level, prefab))]
        every_object = list(objects)   # the prefab's whole set, for claim_wrapper's siblings
        if len(parts) == 3:
            # Within POINT_METRES of the point, and among those the wrapper nearest home
            # (resolve). A point is a filter, not the pick: Christine's step broom outside
            # (3220.7,793.7) is 13 m from the brooms inside, so her point stands north of
            # the house where only the step is within reach (23 Oct 2026).
            px, py = (float(v) for v in parts[2].split(","))
            objects = [(x, y) for x, y in objects if math.hypot(x - px, y - py) <= POINT_METRES]
        if not objects:
            raise SystemExit(f"{quest_id}: no `{prefab}` object on {level} where {field} looks (npc_routine_object)")
        specs.append({"quest_id": quest_id, "behaviour": behaviour, "prefab": prefab, "objects": objects,
                      "every_object": every_object,
                      "soul": soul, "name": name, "hub": hub, "gender": gender.lower(), "tag": ""})
    # The target's bed (routine_bed_target): the bed's prefab and point, and the guids of
    # that prefab's sides about the point, for the far side to find the wrapper by.
    # With the street written on the quest (a `followed` line; npcdb.hpp has_follow), the
    # two sleep links split under the alert tag instead (apply): the witness's bed with
    # the tag off, his own with it on.
    for quest_id, text, soul, name, hub, followed, witness, witness_class in conn.execute(
            "SELECT l.quest_id, l.text, q.target_soul, q.target_name, h.hub_guid, "
            "COALESCE((SELECT text FROM npc_quest_lines WHERE quest_id = l.quest_id AND field = 'followed'), ''), "
            "COALESCE((SELECT text FROM npc_quest_lines WHERE quest_id = l.quest_id AND field = 'seen_with'), ''), "
            "COALESCE((SELECT n.social_class FROM npc_quest_lines w JOIN npc n ON n.soul_name = w.text "
            "          WHERE w.quest_id = l.quest_id AND w.field = 'seen_with'), '') "
            "FROM npc_quest_lines l JOIN npc_quest q USING (quest_id) "
            "LEFT JOIN npc_routine_hub h ON h.soul_name = q.target_soul AND h.level = ? "
            "WHERE l.field = 'routine_bed_target' AND l.text <> '' ORDER BY l.quest_id", (level,)).fetchall():
        if not soul:
            raise SystemExit(f"{quest_id}: routine_bed_target on a quest with no target")
        if not hub:
            print(f"  {quest_id}: {name} ({soul}) has no scheduler hub on {level}; skipped", file=sys.stderr)
            continue
        parts = text.split("@")
        if len(parts) != 2 or "," not in parts[1]:
            raise SystemExit(f"{quest_id}: routine_bed_target wants <bed prefab>@<x>,<y>, got {text!r}")
        prefab = parts[0]
        px, py = (float(v) for v in parts[1].split(","))
        sides = [(guid, x, y) for guid, x, y in conn.execute(
            "SELECT guid, pos_x, pos_y FROM npc_routine_object WHERE level = ? AND prefab = ?", (level, prefab))
            if math.hypot(x - px, y - py) <= OBJECT_METRES]
        if not sides:
            raise SystemExit(f"{quest_id}: no `{prefab}` within {OBJECT_METRES:.0f} m of {px},{py} on {level} "
                             "(npc_routine_object)")
        sides.sort(key=lambda s: math.hypot(s[1] - px, s[2] - py))
        specs.append({"quest_id": quest_id, "bed": prefab, "sides": [s[0] for s in sides], "point": (px, py),
                      "soul": soul, "name": name, "hub": hub, "gender": "", "tag": "",
                      "alert": alert_tag(modid, quest_id) if followed.strip() else "",
                      "witness": witness, "witness_class": witness_class})
    # The witness's bedtime (routine_bed_by, `HH:MM`; 22 Oct 2026 -- Matilda, who should be
    # in the bed before Ernest comes to it): every evening link on the quest's seen_with
    # soul's home hub that runs past the minute is cut to end at it, so her home hub's
    # all-day sleep link is the best left from then on.
    for quest_id, text, soul, name, hub in conn.execute(
            "SELECT l.quest_id, l.text, w.text, COALESCE(n.text, w.text), h.hub_guid "
            "FROM npc_quest_lines l "
            "JOIN npc_quest_lines w ON w.quest_id = l.quest_id AND w.field = 'seen_with' "
            "LEFT JOIN npc_quest_lines n ON n.quest_id = l.quest_id AND n.field = 'seen_with_name' "
            "LEFT JOIN npc_routine_hub h ON h.soul_name = w.text AND h.level = ? "
            "WHERE l.field = 'routine_bed_by' AND l.text <> '' ORDER BY l.quest_id", (level,)).fetchall():
        if not hub:
            print(f"  {quest_id}: {name} ({soul}) has no scheduler hub on {level}; skipped", file=sys.stderr)
            continue
        m = re.fullmatch(r"(\d{1,2}):(\d{2})", text.strip())
        if not m or int(m.group(1)) > 23 or int(m.group(2)) > 59:
            raise SystemExit(f"{quest_id}: routine_bed_by wants HH:MM, got {text!r}")
        specs.append({"quest_id": quest_id, "bed_by": int(m.group(1)) * 60 + int(m.group(2)),
                      "soul": soul, "name": name, "hub": hub, "gender": "", "tag": ""})
    # The holder's homecoming (routine_home_at, `HH:MM`; 25 Oct 2026 -- "Ernest says 'out
    # here in the dark' but it isn't dark yet, please delay his go to bed routine by an
    # hour"): the day links on the quest's target soul's home hub that end last are moved
    # to end at the minute, their jitter kept, so he sets off for the bed that much later.
    for quest_id, text, soul, name, hub in conn.execute(
            "SELECT l.quest_id, l.text, q.target_soul, q.target_name, h.hub_guid "
            "FROM npc_quest_lines l JOIN npc_quest q USING (quest_id) "
            "LEFT JOIN npc_routine_hub h ON h.soul_name = q.target_soul AND h.level = ? "
            "WHERE l.field = 'routine_home_at' AND l.text <> '' ORDER BY l.quest_id", (level,)).fetchall():
        if not soul:
            raise SystemExit(f"{quest_id}: routine_home_at on a quest with no target")
        if not hub:
            print(f"  {quest_id}: {name} ({soul}) has no scheduler hub on {level}; skipped", file=sys.stderr)
            continue
        m = re.fullmatch(r"(\d{1,2}):(\d{2})", text.strip())
        if not m or int(m.group(1)) > 23 or int(m.group(2)) > 59:
            raise SystemExit(f"{quest_id}: routine_home_at wants HH:MM, got {text!r}")
        specs.append({"quest_id": quest_id, "home_at": int(m.group(1)) * 60 + int(m.group(2)),
                      "soul": soul, "name": name, "hub": hub, "gender": "", "tag": ""})
    return specs


# ------------------------------------------------------------------ the table (stdlib only)


def read_member(pak: Path, info: zipfile.ZipInfo) -> bytes:
    """Read one pak member. CryEngine paks store a raw deflate stream (extract_kcd2_places.py)."""
    with open(pak, "rb") as f:
        f.seek(info.header_offset)
        _sig, _ver, _flag, method, _mt, _md, _crc, comp, _unc, namelen, extralen = struct.unpack(
            "<IHHHHHIIIHH", f.read(30))
        f.read(namelen + extralen)
        data = f.read(comp)
    return zlib.decompress(data, -15) if method == 8 else data


def read_base_table(game: Path | None, scheduler: Path | None, level: str) -> bytes:
    if scheduler:
        return scheduler.read_bytes()
    pak = game / "Data/Levels" / level / "level.pak"
    if not pak.exists():
        raise SystemExit(f"no level pak at {pak}")
    z = zipfile.ZipFile(pak)
    for info in z.infolist():
        if info.filename.replace("\\", "/").lower() == TABLE_IN_PAK:
            return read_member(pak, info)
    raise SystemExit(f"{pak} has no {TABLE_IN_PAK}")


class Link:
    __slots__ = ("target", "params")

    def __init__(self, el: ET.Element):
        self.target = el.get("TargetGuid", "")
        p = el.find("Parameters")
        self.params = dict(p.attrib) if p is not None else {}

    def get(self, key: str, default: str = "") -> str:
        return self.params.get(key, default)

    def num(self, key: str, default: int = 0) -> int:
        try:
            return int(float(self.params.get(key, default)))
        except ValueError:
            return default

    @property
    def start(self) -> int:
        return self.num("TimeOfDayStart", 0)

    @property
    def end(self) -> int:
        return self.num("TimeOfDayEnd", 1440)

    @property
    def all_day(self) -> bool:
        return self.start == 0 and self.end >= 1440

    @property
    def plain(self) -> bool:
        """A routine link: no buff, role or tag condition on it."""
        return not (self.get("BuffTag") or self.get("ProvidedRole") or self.get("LinkTag")
                    or self.get("RequiredRole"))

    @property
    def homely(self) -> bool:
        """A home link: plain, or plain but for the role the soul brings to the household
        (ProvidedRole=son on a lodger's or a son's link to the house hub; Paul Schmidt,
        kkut_man_2). A role provided is not a condition on the link."""
        return not (self.get("BuffTag") or self.get("LinkTag") or self.get("RequiredRole")
                    or self.get("BehaviorName"))

    def __repr__(self) -> str:
        keys = ("Priority", "TimeOfDayStart", "TimeOfDayEnd", "BehaviorName", "ProvidedRole",
                "BuffTag", "LinkTag", "AllowedGenders", "Attraction")
        return f"-> {self.target} " + " ".join(f"{k}={self.params[k]}" for k in keys if k in self.params)


def parse_hubs(table: bytes) -> dict[str, tuple[ET.Element, list[Link]]]:
    root = ET.fromstring(table)
    hubs = {}
    for hub in root.iter("C_SmartHub"):
        hubs[hub.get("EntityGuid", "")] = (hub, [Link(l) for l in hub.iter("S_ActivityLink")])
    return hubs


def position(hub: ET.Element) -> tuple[float, float] | None:
    m = re.findall(r"[-\d.]+", hub.get("Position") or "")
    return (float(m[0]), float(m[1])) if len(m) >= 2 else None


def gender_fits(link: Link, gender: str) -> bool:
    # AllowedGenders: 0 any, 1 male, 2 female.
    g = link.get("AllowedGenders", "0")
    return g == "0" or (g == "1" and gender.startswith("m")) or (g == "2" and gender.startswith("f"))


def resolve(spec: dict, hubs: dict) -> dict:
    """The two links to edit and the wrapper to point at, or a ValueError saying why not."""
    who = f"{spec['name']} ({spec['soul']}, hub {spec['hub']})"
    if spec["hub"] not in hubs:
        raise ValueError(f"{who}: no such hub in the table")
    _, npc_links = hubs[spec["hub"]]
    homes = [l for l in npc_links if l.homely and l.all_day and l.num("Priority") == 0]
    if len(homes) != 1:
        raise ValueError(f"{who}: expected one all-day priority-0 home link on the NPC hub, found {len(homes)}")
    home = homes[0]
    works = [l for l in npc_links if l.plain and not l.all_day]
    if not works and spec["tag"]:
        raise ValueError(f"{who}: no timed work link on the NPC hub")
    works.sort(key=lambda l: (-l.num("Priority"), -(l.end - l.start)))
    if home.target not in hubs:
        raise ValueError(f"{who}: home hub {home.target} is not in the table")
    _, home_links = hubs[home.target]
    if works:
        work = works[0]
    else:
        # A filler soul: the one link is the home link, and his hours are the home hub's
        # own working ones (routine_day only; the tagged shape needs the two links).
        home_works = sorted([l for l in home_links if l.plain and not l.all_day and l.num("Priority") > 0],
                            key=lambda l: (-(l.end - l.start), l.start))
        if not home_works:
            raise ValueError(f"{who}: no timed work link on the NPC hub nor on the home hub")
        work = Link.__new__(Link)
        work.target, work.params = home.target, dict(home.params)
        work.params["TimeOfDayStart"] = str(home_works[0].start)
        work.params["TimeOfDayEnd"] = str(home_works[0].end)
    spares = [l for l in home_links if l.plain and not l.all_day and l.num("Priority") > 0]
    if not spares and spec["tag"]:
        raise ValueError(f"{who}: the home hub {home.target} has no timed link to repurpose")
    spares.sort(key=lambda l: l.start)
    spare = spares[0] if spares else None   # a routine_day leaves the home hub alone
    # Where home is, for the nearest wrapper: the first positioned hub the home hub leads to.
    anchor = None
    for l in home_links:
        if l.target in hubs:
            anchor = position(hubs[l.target][0])
            if anchor:
                break
    if anchor is None:
        raise ValueError(f"{who}: nothing the home hub leads to has a position")
    best = None
    objects = spec.get("objects") or []
    for guid, (hub, links) in hubs.items():
        pos = position(hub)
        if not pos:
            continue
        fits = [l for l in links if l.get("BehaviorName") == spec["behaviour"] and gender_fits(l, spec["gender"])]
        if not fits:
            continue
        # A routine_day names the wrapper's prefab too: the hub has to stand on one of them.
        # One standing *on* an object (ON_OBJECT_METRES) beats one merely beside it: the
        # leaning spot by Christine's step is 3.7 m from her broom and nearer home, and
        # took the broom's spec (23 Oct 2026). The proven picks stand within 1.3 m of theirs.
        off = min((math.hypot(pos[0] - x, pos[1] - y) for x, y in objects), default=0.0)
        if objects and off > OBJECT_METRES:
            continue
        d = math.hypot(pos[0] - anchor[0], pos[1] - anchor[1])
        rank = (off > ON_OBJECT_METRES, d)
        if best is None or rank < best[3]:
            best = (d, guid, pos, rank)
    limit = MAX_WRAPPER_METRES if spec["tag"] else DAY_WRAPPER_METRES
    if best is None or best[0] > limit:
        where = f"{best[0]:.0f} m off" if best else "nowhere in the level"
        raise ValueError(f"{who}: no `{spec['behaviour']}` wrapper within {limit:.0f} m of home ({where})")
    # The morning's gap (25 Oct 2026, notes.md: "I still found her in bed on one run"): the
    # work link rewritten for good runs from its start sharp (TimeVariation 0), but the home
    # hub's morning link ends with the game's jitter (Christine's 06:00-08:00, variation 30:
    # as early as 07:30), and between the one ending and the other beginning the best she
    # has is the home hub's all-day sleep link. A probe (christine_morning5) had her lying
    # down at 08:00 on the dot and up again at 08:08; on the run that found her in bed the
    # sleep held. So the work window opens at the earliest the morning can end, and there
    # is no gap for the bed to fill: a morning that runs late keeps her (HaltOnInvalid is
    # false on it) and the work link, valid by then, takes her the moment it is over.
    early = work.start
    for l in home_links:
        if not l.plain or l.all_day or l.num("Priority") <= 0:
            continue
        low = l.end - l.num("TimeVariation")
        if low < early <= l.end + l.num("TimeVariation"):
            early = low
    return {
        "npc_hub": spec["hub"], "work_target": work.target,
        "work_window": (work.start, work.end), "work_priority": work.num("Priority"),
        "work_start_early": early,
        "work_variation": work.num("TimeVariation"),
        "home_hub": home.target, "spare_target": spare.target if spare else "",
        "spare_window": (spare.start, spare.end) if spare else (0, 0),
        "wrapper": best[1], "wrapper_pos": best[2], "wrapper_metres": best[0], "anchor": anchor,
    }


def resolve_bed(spec: dict, hubs: dict) -> dict:
    """The sleeper wrapper of the named bed and the home hub whose sleep links go to it."""
    who = f"{spec['name']} ({spec['soul']}, hub {spec['hub']})"
    if spec["hub"] not in hubs:
        raise ValueError(f"{who}: no such hub in the table")
    _, npc_links = hubs[spec["hub"]]
    homes = [l for l in npc_links if l.homely and l.all_day and l.num("Priority") == 0]
    if len(homes) != 1:
        raise ValueError(f"{who}: expected one all-day priority-0 home link on the NPC hub, found {len(homes)}")
    home = homes[0]
    if home.target not in hubs:
        raise ValueError(f"{who}: home hub {home.target} is not in the table")
    _, home_links = hubs[home.target]
    sleeps = [l for l in home_links if l.get("BehaviorName") == "sleep"]
    if not sleeps:
        raise ValueError(f"{who}: the home hub {home.target} has no sleep link to point at the bed")
    if len({l.target for l in sleeps}) != 1:
        raise ValueError(f"{who}: the home hub {home.target} sleeps in more than one place; which to move?")
    # The bed's nearest side first; a side with no wrapper of its own (a bed the game
    # dresses but nobody sleeps in) falls through to the other.
    for side in spec["sides"]:
        for guid, (hub, links) in hubs.items():
            if any(l.target == side and l.get("BehaviorName") == "use" and l.get("ProvidedRole") == "sleeper"
                   for l in links):
                return {"npc_hub": spec["hub"], "home_hub": home.target, "mat": sleeps[0].target,
                        "sleep_links": len(sleeps), "wrapper": guid, "wrapper_pos": position(hub) or spec["point"],
                        "side": side}
    raise ValueError(f"{who}: no sleeper wrapper uses a side of the `{spec['bed']}` at "
                     f"{spec['point'][0]:.1f},{spec['point'][1]:.1f}")


def resolve_bed_by(spec: dict, hubs: dict) -> dict:
    """The witness's home hub and the evening links on it that run past her bedtime."""
    who = f"{spec['name']} ({spec['soul']}, hub {spec['hub']})"
    minute = spec["bed_by"]
    if spec["hub"] not in hubs:
        raise ValueError(f"{who}: no such hub in the table")
    _, npc_links = hubs[spec["hub"]]
    homes = [l for l in npc_links if l.homely and l.all_day and l.num("Priority") == 0]
    if len(homes) != 1:
        raise ValueError(f"{who}: expected one all-day priority-0 home link on the NPC hub, found {len(homes)}")
    home = homes[0]
    if home.target not in hubs:
        raise ValueError(f"{who}: home hub {home.target} is not in the table")
    _, home_links = hubs[home.target]
    if not any(l.all_day and l.num("Priority") == 0 for l in home_links):
        raise ValueError(f"{who}: the home hub {home.target} has no all-day link to fall back on at bedtime")
    late = [l for l in home_links if not l.all_day and l.num("TimeOfDayStart") < minute < l.num("TimeOfDayEnd")]
    after = [l for l in home_links if not l.all_day and minute <= l.num("TimeOfDayStart") < 1440
             and l.num("TimeOfDayEnd") > l.num("TimeOfDayStart")]
    if after:
        raise ValueError(f"{who}: the home hub {home.target} has a link starting after {minute // 60:02d}:{minute % 60:02d} "
                         f"({after[0].target}); cutting it is not written")
    if not late:
        raise ValueError(f"{who}: no link on the home hub {home.target} runs past {minute // 60:02d}:{minute % 60:02d}; "
                         "nothing to cut")
    if len({l.target for l in late}) != len(late):
        raise ValueError(f"{who}: two late links on {home.target} share a target; which to cut?")
    return {"npc_hub": spec["hub"], "home_hub": home.target,
            "late": [(l.target, l.num("TimeOfDayStart"), l.num("TimeOfDayEnd")) for l in late]}


def resolve_home_at(spec: dict, hubs: dict) -> dict:
    """The holder's home hub and the day links on it that end last (his working day): the
    ones to move to the new minute. Ernest's home hub has two to his church hub, 08:00 to
    20:00 and 08:30 to 19:50, both priority 1 over the all-day sleep link; both move."""
    who = f"{spec['name']} ({spec['soul']}, hub {spec['hub']})"
    minute = spec["home_at"]
    if spec["hub"] not in hubs:
        raise ValueError(f"{who}: no such hub in the table")
    _, npc_links = hubs[spec["hub"]]
    homes = [l for l in npc_links if l.homely and l.all_day and l.num("Priority") == 0]
    if len(homes) != 1:
        raise ValueError(f"{who}: expected one all-day priority-0 home link on the NPC hub, found {len(homes)}")
    home = homes[0]
    if home.target not in hubs:
        raise ValueError(f"{who}: home hub {home.target} is not in the table")
    _, home_links = hubs[home.target]
    if not any(l.all_day and l.num("Priority") == 0 for l in home_links):
        raise ValueError(f"{who}: the home hub {home.target} has no all-day link to come home to")
    days = [l for l in home_links if not l.all_day and l.plain and l.end > l.start]
    if not days:
        raise ValueError(f"{who}: no day link on the home hub {home.target} to move")
    last = max(l.end for l in days)
    # The last hour of his day: every day link ending within the hour of the latest.
    late = [l for l in days if l.end > last - 60]
    if any(l.start >= minute for l in late):
        raise ValueError(f"{who}: a day link on {home.target} starts after {minute // 60:02d}:{minute % 60:02d}")
    if any(not l.all_day and l.start < minute < l.end for l in home_links if l not in late):
        raise ValueError(f"{who}: another link on {home.target} runs over {minute // 60:02d}:{minute % 60:02d}; "
                         "which should he be on?")
    return {"npc_hub": spec["hub"], "home_hub": home.target,
            "late": [(l.target, l.start, l.end) for l in late]}


LINK_RE = r'(<S_ActivityLink\b[^>]*\bTargetGuid=")({target})("[^>]*>\s*<Parameters\b)([^>]*?)(\s*/>)'


def edit_link(hub_block: str, target: str, new_target: str | None, attrs: dict[str, str],
              drop: tuple[str, ...] = (), every: bool = False) -> str:
    """One link inside one hub's text: its TargetGuid and its Parameters, the rest untouched.
    Attributes are merged (and `drop`ped) and written sorted, as the exporter writes them.
    `every` edits all the hub's links to the target alike (a home hub's two sleep links,
    the plain one and the low_health one); without it the link must be the only one."""
    matches = list(re.finditer(LINK_RE.format(target=re.escape(target)), hub_block))
    if not matches or (len(matches) != 1 and not every):
        raise ValueError(f"link to {target} appears {len(matches)} times in the hub, expected once")
    out = hub_block
    for m in reversed(matches):
        params = dict(re.findall(r'([A-Za-z]+)="([^"]*)"', m.group(4)))
        params.update(attrs)
        for k in drop:
            params.pop(k, None)
        body = " ".join(f'{k}="{v}"' for k, v in sorted(params.items()))
        out = (out[:m.start()] + m.group(1) + (new_target or target) + m.group(3) + " " + body
               + m.group(5) + out[m.end():])
    return out


def split_bed_links(hub_block: str, mat: str, wrapper: str, tag: str) -> str:
    """The target's two home-hub sleep links to his mat split under the alert tag (the
    street, notes.md 23 Oct 2026 n8): the plain one pointed at the witness's sleeper wrapper
    as routine_bed_target has it, under the tag *inverted* and HaltOnInvalid, so the night
    the graph turns the tag on he turns round in the street; the low_health one kept on his
    own mat with its buff condition dropped and the plain one's priority, under the tag
    normal and *without* HaltOnInvalid, so he sleeps at home that night and the tag's
    dropping in the morning does not tip a sleeping man out of bed. Tag off, he sleeps at
    the witness's as before (and nowhere else when wounded, the price of the split)."""
    matches = list(re.finditer(LINK_RE.format(target=re.escape(mat)), hub_block))
    if len(matches) != 2:
        raise ValueError(f"the home hub has {len(matches)} sleep links to {mat}; the split wants two "
                         "(the plain one and the low_health one)")
    params = [dict(re.findall(r'([A-Za-z]+)="([^"]*)"', m.group(4))) for m in matches]
    plain = [i for i, p in enumerate(params) if not p.get("BuffTag")]
    buffed = [i for i, p in enumerate(params) if p.get("BuffTag")]
    if len(plain) != 1 or len(buffed) != 1:
        raise ValueError(f"the two sleep links to {mat} are not one plain and one under a buff "
                         f"({[p.get('BuffTag', '') for p in params]})")
    out = hub_block
    for i in sorted(range(2), key=lambda i: -matches[i].start()):
        m, p = matches[i], dict(params[i])
        if i == plain[0]:
            p.update({"Attraction": str(BED_ATTRACTION), "LinkTag": tag, "InvertLinkTagRequirement": "true",
                      "HaltOnInvalid": "true"})
            for k in ("BehaviorName", "ProvidedRole"):
                p.pop(k, None)
            target = wrapper
        else:
            p.update({"LinkTag": tag, "InvertLinkTagRequirement": "false", "HaltOnInvalid": "false",
                      "Priority": params[plain[0]].get("Priority", "0")})
            p.pop("BuffTag", None)
            target = mat
        body = " ".join(f'{k}="{v}"' for k, v in sorted(p.items()))
        out = out[:m.start()] + m.group(1) + target + m.group(3) + " " + body + m.group(5) + out[m.end():]
    return out


def claim_wrapper(text: str, hubs: dict, spec: dict, r: dict) -> tuple[str, list[str]]:
    """The routine_day's wrapper made the soul's alone (24 Oct 2026, notes.md: Christine
    found in bed at eleven in the morning, somebody else sweeping at her step). The step
    broom the spec picked is one of four one-time-use chores on the household's own hub
    (5387674092023540176, Attraction 120: two hours at each), so for those two hours the
    broom is taken, her one work link is dead, and the best she has left is her home hub's
    all-day sleep link. Every other hub with a link to the wrapper has that link pointed at a
    sibling of its own instead: another link of the same hub whose wrapper carries the same
    behaviour and stands on another object of the prefab (the three other brooms), its
    parameters kept, so the household sweeps three spots instead of four and the step is
    hers. A hub with no such sibling is left as it is and said so in the report; the soul's
    own hubs (NPC, home, work) are not foreign."""
    own = {r["npc_hub"], r["home_hub"], r["work_target"]}
    wrapper = r["wrapper"]
    notes = []
    for guid, (hub, links) in hubs.items():
        if guid in own or not any(l.target == wrapper for l in links):
            continue
        sibling = None
        for l in links:
            if l.target == wrapper or l.target not in hubs:
                continue
            cand_hub, cand_links = hubs[l.target]
            pos = position(cand_hub)
            if not pos or not any(c.get("BehaviorName") == spec["behaviour"] for c in cand_links):
                continue
            if min((math.hypot(pos[0] - x, pos[1] - y) for x, y in spec.get("every_object") or []),
                   default=OBJECT_METRES + 1) > OBJECT_METRES:
                continue
            sibling = l.target
            break
        if sibling is None:
            notes.append(f"    hub {guid} also links to the wrapper and has no other `{spec['prefab']}` of its own; "
                         "left shared (the soul will lose the spot while that hub's NPC is on it)")
            continue
        text = edit_hub(text, guid, lambda block: edit_link(block, wrapper, sibling, {}, every=True))
        notes.append(f"    hub {guid}: its link -> wrapper {wrapper} now -> {sibling}, another of its `{spec['prefab']}`; "
                     "the wrapper is hers alone")
    return text, notes


def retarget_where(hub_block: str, target: str, new_target: str, where) -> tuple[str, int]:
    """Every link of the hub to `target` whose Parameters satisfy `where` pointed at
    `new_target`, parameters kept. Returns the text and how many were moved."""
    out, moved = hub_block, 0
    for m in reversed(list(re.finditer(LINK_RE.format(target=re.escape(target)), hub_block))):
        params = dict(re.findall(r'([A-Za-z]+)="([^"]*)"', m.group(4)))
        if not where(params):
            continue
        out = out[:m.start()] + m.group(1) + new_target + m.group(3) + m.group(4) + m.group(5) + out[m.end():]
        moved += 1
    return out, moved


def claim_bed(text: str, hubs: dict, spec: dict, r: dict) -> tuple[str, list[str]]:
    """The bed theirs alone (26 Oct 2026, notes.md: "When I caught Ernest this time he was
    in bed with the master, not the woman I'd expect"). The witness's household bed hub
    (Matilda's 5559216178312029110) gives the double bed's two sides to two social classes
    at the same priority -- the housekeeper and the weaponsmith, either side each -- so who
    lies where is whoever lies down first, and a night the pair are got up again (Henry in
    the house before the catching is trespass, and the household rises to it) is a night
    the master may take the free side beside the priest and the witness a bed across the
    room; the catching, which wants her within exposeWithMetres of him, never fires. So:
    every plain link of a foreign hub to either side's sleeper wrapper is made to say where
    its class sleeps for good -- the witness's class to the side the holder does not use,
    every other class (and a classless link) to the hub's nearest other sleeper bed, its
    parameters kept, so the master beats the servants to that bed at his own priority as
    he did the night the replay watched (21:08, the bed 8.5 m off). Wounded (BuffTag) and
    tagged links are not touched. A hub with no other bed is left as it is and said so."""
    notes = []
    wrapper_a = r["wrapper"]
    wrapper_b = None
    for side in [s for s in spec["sides"] if s != r["side"]]:
        for guid, (hub, links) in hubs.items():
            if any(l.target == side and l.get("BehaviorName") == "use" and l.get("ProvidedRole") == "sleeper"
                   for l in links):
                wrapper_b = guid
                break
        if wrapper_b:
            break
    if wrapper_b is None:
        notes.append("    the bed has no second side with a sleeper wrapper; the sides are left as the game has them")
        return text, notes
    cls = spec.get("witness_class") or ""
    own = {r["npc_hub"], r["home_hub"]}

    def plain(l) -> bool:
        return not (l.get("BuffTag") or l.get("LinkTag") or l.get("RequiredRole") or l.get("BehaviorName"))

    for guid, (hub, links) in hubs.items():
        if guid in own or not any(l.target in (wrapper_a, wrapper_b) and plain(l) for l in links):
            continue
        spares = []
        for l in links:
            if l.target in (wrapper_a, wrapper_b) or l.target not in hubs:
                continue
            thub, tlinks = hubs[l.target]
            if any(c.get("BehaviorName") == "use" and c.get("ProvidedRole") == "sleeper" for c in tlinks):
                pos = position(thub)
                d = math.hypot(pos[0] - spec["point"][0], pos[1] - spec["point"][1]) if pos else 1e9
                spares.append((d, l.target))
        spare = min(spares)[1] if spares else None
        hers = lambda p: bool(cls) and p.get("SocialClass") == cls and not (p.get("BuffTag") or p.get("LinkTag")
                                                                          or p.get("RequiredRole") or p.get("BehaviorName"))
        others = lambda p: not (bool(cls) and p.get("SocialClass") == cls) and not (
            p.get("BuffTag") or p.get("LinkTag") or p.get("RequiredRole") or p.get("BehaviorName"))
        moved_hers = moved_others = 0

        def fn(block):
            nonlocal moved_hers, moved_others
            block, n = retarget_where(block, wrapper_a, wrapper_b, hers)
            moved_hers += n
            if spare:
                for w in (wrapper_a, wrapper_b):
                    block, n = retarget_where(block, w, spare, others)
                    moved_others += n
            return block
        text = edit_hub(text, guid, fn)
        line = f"    hub {guid}: "
        if cls:
            line += f"{moved_hers} `{cls}` link(s) -> side {wrapper_a} now -> side {wrapper_b}, the witness's side"
        else:
            line += "the witness has no social class in the database; her side is not settled"
        if spare:
            line += f"; {moved_others} other link(s) to either side now -> bed {spare} ({min(spares)[0]:.1f} m off)"
        elif any(others(l.params) for l in links if l.target in (wrapper_a, wrapper_b)):
            line += "; no other sleeper bed on the hub, so the other classes' links to the sides are left"
        notes.append(line)
    if not notes:
        notes.append("    no foreign hub links to either side; the bed is theirs already")
    return text, notes


def edit_hub(text: str, guid: str, fn) -> str:
    start = text.find(f'<C_SmartHub EntityGuid="{guid}"')
    if start < 0:
        raise ValueError(f"hub {guid} not found in the table text")
    end = text.find("</C_SmartHub>", start)
    if end < 0:
        raise ValueError(f"hub {guid} has no closing tag")
    return text[:start] + fn(text[start:end]) + text[end:]


def apply(table: bytes, specs: list[dict]) -> tuple[bytes, list[str]]:
    hubs = parse_hubs(table)
    text = table.decode("utf-8")
    report = []
    for spec in specs:
        if spec.get("bed"):
            # Where the target sleeps, for good: his home hub's sleep links pointed at the
            # bed's sleeper wrapper, the behaviour and role left to the wrapper's own links,
            # the day links and the wake-up untouched.
            r = resolve_bed(spec, hubs)
            if spec.get("alert"):
                # The street written on the quest: the two sleep links split under the
                # alert tag (split_bed_links), hers with it off, his own with it on.
                text = edit_hub(text, r["home_hub"], lambda block: split_bed_links(
                    block, r["mat"], r["wrapper"], spec["alert"]))
                # And the bed theirs alone (claim_bed): the witness on the other side,
                # the rest of her house in its other beds, whoever lies down first.
                text, kept = claim_bed(text, hubs, spec, r)
                report.append(
                    f"{spec['quest_id']} {spec['name']} ({spec['soul']}): sleeps in the `{spec['bed']}` at "
                    f"{spec['point'][0]:.1f},{spec['point'][1]:.1f} unless he has caught Henry following him\n"
                    f"    home hub {r['home_hub']}: plain sleep link -> {r['mat']} now -> sleeper wrapper "
                    f"{r['wrapper']} at {r['wrapper_pos'][0]:.1f},{r['wrapper_pos'][1]:.1f} (side {r['side']}), "
                    f"live while tag {spec['alert']} is off; the low_health sleep link stays -> {r['mat']}, "
                    f"its buff dropped, live while the tag is on"
                    + "".join("\n" + n for n in kept))
                continue
            text = edit_hub(text, r["home_hub"], lambda block: edit_link(
                block, r["mat"], r["wrapper"], {"Attraction": str(BED_ATTRACTION)},
                drop=("BehaviorName", "ProvidedRole"), every=True))
            text, kept = claim_bed(text, hubs, spec, r)
            report.append(
                f"{spec['quest_id']} {spec['name']} ({spec['soul']}): sleeps in the `{spec['bed']}` at "
                f"{spec['point'][0]:.1f},{spec['point'][1]:.1f} for good\n"
                f"    home hub {r['home_hub']}: {r['sleep_links']} sleep link(s) -> {r['mat']} now -> sleeper wrapper "
                f"{r['wrapper']} at {r['wrapper_pos'][0]:.1f},{r['wrapper_pos'][1]:.1f} (side {r['side']})"
                + "".join("\n" + n for n in kept))
            continue
        if spec.get("bed_by"):
            # The witness's bedtime, for good: her home hub's evening links cut to end at
            # the minute, so its all-day sleep link is the best left from then on.
            # HaltOnInvalid, or the window's end is no end: with it false (the base value)
            # she finishes what the link started, and Matilda, sat down to her mash at
            # 21:38 under a link cut to 21:30, was still eating at 23:17 (probe, 23 Oct 2026).
            r = resolve_bed_by(spec, hubs)
            minute = spec["bed_by"]
            for target, _, _ in r["late"]:
                text = edit_hub(text, r["home_hub"], lambda block, t=target: edit_link(
                    block, t, None, {"TimeOfDayEnd": str(minute), "TimeVariation": "0", "HaltOnInvalid": "true"}))
            cut = ", ".join(f"{t} ({s // 60:02d}:{s % 60:02d}-{e // 60:02d}:{e % 60:02d})" for t, s, e in r["late"])
            report.append(
                f"{spec['quest_id']} {spec['name']} ({spec['soul']}): in bed by {minute // 60:02d}:{minute % 60:02d} for good\n"
                f"    home hub {r['home_hub']}: link(s) -> {cut} now end at {minute // 60:02d}:{minute % 60:02d}")
            continue
        if spec.get("home_at"):
            # The holder's homecoming, for good: the last day links on his home hub moved
            # to end at the minute, their TimeVariation as the game had it (Ernest's wander
            # an hour either way, so the night is not the same twice), HaltOnInvalid left
            # false: the all-day sleep link under them is where he goes when they end, as
            # it was when they ended an hour earlier.
            r = resolve_home_at(spec, hubs)
            minute = spec["home_at"]
            for target in sorted({t for t, _, _ in r["late"]}):
                text = edit_hub(text, r["home_hub"], lambda block, t=target: edit_link(
                    block, t, None, {"TimeOfDayEnd": str(minute)}, every=True))
            moved = ", ".join(f"{t} ({s // 60:02d}:{s % 60:02d}-{e // 60:02d}:{e % 60:02d})" for t, s, e in r["late"])
            report.append(
                f"{spec['quest_id']} {spec['name']} ({spec['soul']}): home at {minute // 60:02d}:{minute % 60:02d} for good\n"
                f"    home hub {r['home_hub']}: day link(s) -> {moved} now end at {minute // 60:02d}:{minute % 60:02d}")
            continue
        r = resolve(spec, hubs)
        tag = spec["tag"]
        if not tag:
            # The giver's day as the story has it, for good: the work link alone, pointed at
            # the wrapper for its own hours, untagged; home and the graph untouched. The
            # window opens at the earliest the morning link can end (resolve,
            # work_start_early), so no jittered morning leaves a gap for the bed. And it
            # opens with Urgency Fast (26 Oct 2026, notes.md: "She's still in bed on my
            # latest run"): a link that comes valid while the soul is in the middle of
            # something does not take her off it by itself, and when that something ends
            # the replan is the hub's that owned it, not the NPC hub's. A probe had
            # Christine eat her mash from 06:05 to 08:20, her work link valid from 07:30 and
            # never looked at, and the home hub's morning link over, lie down in the all-day
            # bed and stay there until the home hub's own 19:00 link got her up for the
            # tavern. Urgency is how the game has a timed routine link taken up the moment
            # it opens: the innkeepers' closing-up link (prio 6, 21:00-22:30) carries Fast.
            ws, we = r["work_start_early"], r["work_window"][1]
            text = edit_hub(text, r["npc_hub"], lambda block: edit_link(
                block, r["work_target"], r["wrapper"], {
                    "Attraction": str(ROUTINE_ATTRACTION), "TimeVariation": "0",
                    "TimeOfDayStart": str(ws), "TimeOfDayEnd": str(we),
                    "Urgency": ROUTINE_URGENCY}))
            # And the wrapper hers alone (claim_wrapper): a spot shared with the game's own
            # people is a spot she loses for hours at a time, and a soul with one dead work
            # link goes to bed.
            text, claimed = claim_wrapper(text, hubs, spec, r)
            report.append(
                f"{spec['quest_id']} {spec['name']} ({spec['soul']}): `{spec['behaviour']}` at a "
                f"`{spec.get('prefab', '')}` for good\n"
                f"    NPC hub {r['npc_hub']}: work link -> {r['work_target']} (prio {r['work_priority']}, "
                f"{ws // 60:02d}:{ws % 60:02d}-{we // 60:02d}:{we % 60:02d}) now -> wrapper {r['wrapper']} "
                f"at {r['wrapper_pos'][0]:.1f},{r['wrapper_pos'][1]:.1f} ({r['wrapper_metres']:.0f} m from home)"
                + "".join("\n" + n for n in claimed))
            continue
        # The two links swap jobs. The NPC hub's work link becomes the bench link: tagged,
        # with Urgency, which is how the game's own quest links on an NPC hub take a
        # villager off her round the moment the tag goes on (`#socky_instantTavern,!Instant`
        # and 1,200 more carry one). The home hub's spare link becomes the work link under
        # the tag inverted, at a priority above the home hub's own, with the work link's
        # window and jitter, so with the tag off she reaches her work hub by way of home --
        # Geoffrey's own shape (§2.1: NPC hub -> home hub -> work hub). HaltOnInvalid on
        # both so the one she is on is dropped the moment the tag turns it off (474 shipped
        # links say it). Proven in play (7 Oct 2026, quest_pass.sh --road find --how
        # routine): tag off, her day as the game has it; the accept through her menu, and
        # she left the yard at once and was Crying on the bench 50 s later; the hand-in, and
        # she walked back to her chores. The first shape tried (the work link tagged inverted
        # in place, the bench link on the home hub) also works with the tag on from a load
        # (§4.6's probe), but was never seen with a live flip: the passes that seemed to fail
        # it had sent the accept from Lua, which never reaches the graph.
        text = edit_hub(text, r["npc_hub"], lambda block: edit_link(
            block, r["work_target"], r["wrapper"], {
                "TimeVariation": "0", "Attraction": str(ROUTINE_ATTRACTION),
                "LinkTag": tag, "InvertLinkTagRequirement": "false",
                "HaltOnInvalid": "true", "Urgency": "Slow"}))
        text = edit_hub(text, r["home_hub"], lambda block: edit_link(
            block, r["spare_target"], r["work_target"], {
                "Priority": str(ROUTINE_PRIORITY),
                "TimeOfDayStart": str(r["work_window"][0]), "TimeOfDayEnd": str(r["work_window"][1]),
                "TimeVariation": str(r["work_variation"]),
                "LinkTag": tag, "InvertLinkTagRequirement": "true", "HaltOnInvalid": "true"}))
        ws, we = r["work_window"]
        report.append(
            f"{spec['quest_id']} {spec['name']} ({spec['soul']}): `{spec['behaviour']}` under tag {tag}\n"
            f"    NPC hub {r['npc_hub']}: work link -> {r['work_target']} (prio {r['work_priority']}, "
            f"{ws // 60:02d}:{ws % 60:02d}-{we // 60:02d}:{we % 60:02d}) now -> wrapper {r['wrapper']} "
            f"at {r['wrapper_pos'][0]:.1f},{r['wrapper_pos'][1]:.1f} ({r['wrapper_metres']:.0f} m from home), "
            f"live while the tag is on\n"
            f"    home hub {r['home_hub']}: link -> {r['spare_target']} "
            f"({r['spare_window'][0] // 60:02d}:{r['spare_window'][0] % 60:02d}-"
            f"{r['spare_window'][1] // 60:02d}:{r['spare_window'][1] % 60:02d}) now -> work hub {r['work_target']}, "
            f"prio {ROUTINE_PRIORITY}, the work hours, live while the tag is off")
    out = text.encode("utf-8")
    ET.fromstring(out)   # still a table the game can parse; a malformed one hangs the level load
    return out, report


def write_pak(path: Path, table: bytes) -> None:
    """One deflated entry, DOS epoch, no extra fields: a pak written by Info-ZIP `zip`
    carries UT/ux fields and the game answers "Database system error - tables can't be
    loaded" and hangs (behaviors.md §4.6). Fixed date so an unchanged table is an
    unchanged pak and the deploy copies nothing."""
    path.parent.mkdir(parents=True, exist_ok=True)
    info = zipfile.ZipInfo(TABLE_IN_PAK, (1980, 1, 1, 0, 0, 0))
    info.compress_type = zipfile.ZIP_DEFLATED
    info.create_system = 0
    with zipfile.ZipFile(path, "w") as z:
        z.writestr(info, table, compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)


def build(specs: list[dict], game: Path | None, scheduler: Path | None, level: str, out: Path) -> None:
    table = read_base_table(game, scheduler, level)
    edited, report = apply(table, specs)
    write_pak(out, edited)
    for line in report:
        print("  " + line, file=sys.stderr)
    print(f"  {out}: {out.stat().st_size / 1e6:.1f} MB for a {len(edited) / 1e6:.1f} MB table", file=sys.stderr)


# ------------------------------------------------------------------ main


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--db", type=Path, default=DEFAULT_DB)
    ap.add_argument("--level", default=DEFAULT_LEVEL)
    ap.add_argument("--modid", default=DEFAULT_MODID)
    ap.add_argument("--specs", type=Path, help="the specs as JSON instead of the database")
    ap.add_argument("--print", action="store_true", help="print the specs and stop")
    ap.add_argument("--scheduler", type=Path, help="the base table, extracted")
    ap.add_argument("--game", help="the game root, to read the base table out of its level.pak")
    ap.add_argument("--out", type=Path, help="the pak to write (Data/Levels/<level>/<name>.pak in the mod)")
    args = ap.parse_args()

    if args.specs:
        specs = json.loads(args.specs.read_text())
    else:
        specs = read_specs(args.db, args.level, args.modid)
    if args.print:
        print(json.dumps(specs, indent=2))
        return
    if not specs:
        print(f"  no quest changes a giver's day on {args.level}; no scheduler override", file=sys.stderr)
        return
    if not args.out:
        raise SystemExit("--out is needed to build")
    if not (args.game or args.scheduler):
        raise SystemExit("--game or --scheduler is needed for the base table")
    build(specs, Path(args.game).expanduser() if args.game else None, args.scheduler, args.level, args.out)


if __name__ == "__main__":
    main()
