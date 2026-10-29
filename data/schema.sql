-- The schema of npcs.sqlite, and of npcs-vectors.sqlite beside it, as the app and the mod
-- generator read them.
--
-- This file is the one source for the shipped database. tools/assemble_db.py runs it and
-- then loads a data drop into it (drop/db/<table>.tsv or .jsonl, plus the vector matrix);
-- app/tests/test_schema.cpp runs it and asks the app's own loaders to read a row of every
-- table, so a column renamed here and not in the code (or the other way about) fails a
-- unit test rather than a conversation. It is two files' worth: everything up to the
-- "==== npcs-vectors.sqlite" line is npcs.sqlite, and what follows it is the vectors' own
-- file, so that the text (which changes with every drop, and is a few hundred megabytes)
-- and the vectors (which change only when the knowledge does) ship and download apart.
--
-- Only the tables somebody ships reads are here. The generation pipeline keeps many more
-- (drafts, reviews, batch jobs, circles, opinions, the social graph's edges) in its working
-- database, and those never leave it. Readers, for the record:
--
--   app   moonshine_app, src/personadb.cpp and src/knowledge.cpp
--   mod   kcd2modgen, mod/src/npcdb.cpp
--   sched scripts/build_scheduler.py, the level's scheduler override
--
-- Column order matters: the drop's TSV and JSONL files name their columns, but the
-- vector matrix does not, and the assembled file's bytes are compared across machines.

-- ---- the souls ------------------------------------------------------------------- app mod sched

CREATE TABLE npc (
    soul_id TEXT PRIMARY KEY,
    soul_name TEXT NOT NULL,
    source_file TEXT,
    display_name TEXT,
    kind TEXT,
    is_named INTEGER NOT NULL,
    is_unique INTEGER NOT NULL,
    skald_character_name TEXT,
    social_class_id INTEGER,
    social_class TEXT,
    occupation TEXT,
    soul_archetype_id INTEGER,
    archetype TEXT,
    gender TEXT,
    race TEXT,
    faction_name TEXT,
    region TEXT,
    settlement TEXT,
    faction_category TEXT,
    faction_detail TEXT,
    combat_level REAL,
    voice_group_name TEXT,
    soul_class TEXT,
    vip_class TEXT,
    inventory_preset TEXT,
    brain_id TEXT,
    teacher_skills TEXT
);
CREATE INDEX idx_npc_kind_named ON npc(kind, is_named);
CREATE INDEX idx_npc_name ON npc(display_name);
CREATE INDEX idx_npc_region ON npc(region);
CREATE INDEX idx_npc_settlement ON npc(settlement);
CREATE INDEX idx_npc_skald ON npc(skald_character_name);
CREATE INDEX idx_npc_social ON npc(social_class);
CREATE INDEX idx_npc_soul_name ON npc(soul_name);

-- app
CREATE TABLE skald_character (
    skald_character_name TEXT PRIMARY KEY,
    ui_name TEXT,
    full_name TEXT,
    gender TEXT,
    age INTEGER,
    age_label TEXT,
    body_type INTEGER,
    voice_id INTEGER,
    voice_categories TEXT,
    profession TEXT,
    profession_design TEXT,
    master_role_name TEXT,
    mortality_id INTEGER,
    unique_assets TEXT,
    is_generic INTEGER NOT NULL
);

-- app mod
CREATE TABLE generated_name (
    soul_id TEXT PRIMARY KEY,
    given TEXT NOT NULL,
    particle TEXT,
    byname TEXT,
    title TEXT,
    full_name TEXT NOT NULL,
    style TEXT NOT NULL,
    pool TEXT NOT NULL,
    household_key TEXT NOT NULL,
    reason TEXT,
    FOREIGN KEY (soul_id) REFERENCES npc(soul_id)
);
CREATE INDEX idx_generated_name_full ON generated_name(full_name);
CREATE INDEX idx_generated_name_household ON generated_name(household_key);

-- app
CREATE TABLE npc_trade (
    soul_name TEXT PRIMARY KEY,
    trade TEXT NOT NULL,
    why TEXT NOT NULL
);

-- app: what a Mannequin fragment name means in words
CREATE TABLE anim_activity (
    fragment TEXT PRIMARY KEY,
    clause TEXT NOT NULL,
    activity TEXT NOT NULL,
    model TEXT NOT NULL,
    prompt_sha TEXT NOT NULL,
    made_at TEXT NOT NULL,
    issues TEXT
);

-- ---- who they are ---------------------------------------------------------------------- app

CREATE TABLE npc_bio (
    soul_name TEXT PRIMARY KEY,
    soul_id TEXT NOT NULL,
    name TEXT NOT NULL,
    bio TEXT NOT NULL,
    dossier TEXT NOT NULL,
    words INTEGER NOT NULL,
    issues TEXT,
    model TEXT NOT NULL,
    prompt_sha TEXT NOT NULL,
    dossier_sha TEXT NOT NULL,
    written_at TEXT NOT NULL
);

CREATE TABLE npc_card (
    soul_name TEXT PRIMARY KEY,
    card TEXT NOT NULL,
    words INTEGER NOT NULL
);

CREATE TABLE npc_hook (
    hook_id TEXT NOT NULL,
    kind TEXT NOT NULL,
    premise TEXT NOT NULL,
    soul_name TEXT NOT NULL,
    other_soul_name TEXT,
    problem TEXT NOT NULL,
    desire TEXT NOT NULL,
    secret INTEGER NOT NULL,
    PRIMARY KEY (hook_id, soul_name)
);
CREATE INDEX idx_hook_soul ON npc_hook(soul_name);

CREATE TABLE npc_person (
    soul_name TEXT PRIMARY KEY,
    circle_id TEXT NOT NULL,
    long_bio TEXT NOT NULL,
    self_account TEXT NOT NULL,
    card TEXT NOT NULL,
    words INTEGER NOT NULL,
    issues TEXT,
    circle_sha TEXT NOT NULL,
    prompt_sha TEXT NOT NULL,
    model TEXT NOT NULL,
    written_at TEXT NOT NULL,
    remark_issues TEXT
);

CREATE TABLE npc_circle_member (
    circle_id TEXT NOT NULL,
    soul_name TEXT NOT NULL,
    role TEXT NOT NULL,
    refers_to_quest_id TEXT,
    bridges TEXT,
    PRIMARY KEY (circle_id, soul_name)
);
CREATE INDEX idx_circle_member_soul ON npc_circle_member(soul_name);

CREATE TABLE npc_absentee (
    circle_id TEXT NOT NULL,
    name TEXT NOT NULL,
    station TEXT NOT NULL,
    why_unmeetable TEXT NOT NULL,
    to_the_circle TEXT NOT NULL,
    PRIMARY KEY (circle_id, name)
);

CREATE TABLE npc_relationship (
    soul_id TEXT NOT NULL,
    soul_name TEXT NOT NULL,
    other_soul_id TEXT NOT NULL,
    other_soul_name TEXT NOT NULL,
    relationship TEXT NOT NULL,
    category TEXT NOT NULL,
    certainty TEXT NOT NULL,
    evidence TEXT,
    PRIMARY KEY (soul_name, other_soul_name)
);
CREATE INDEX idx_relationship_category ON npc_relationship(category);
CREATE INDEX idx_relationship_soul ON npc_relationship(soul_id);
CREATE INDEX idx_relationship_type ON npc_relationship(relationship);

CREATE TABLE social_place (
    place_id TEXT PRIMARY KEY,
    kind TEXT NOT NULL,
    name TEXT,
    level TEXT,
    settlement TEXT,
    region TEXT,
    faction_name TEXT,
    hub_entity TEXT,
    pos TEXT,
    comment TEXT,
    house_trade TEXT,
    size INTEGER NOT NULL,
    confidence TEXT NOT NULL
);

CREATE TABLE social_membership (
    soul_id TEXT NOT NULL,
    soul_name TEXT NOT NULL,
    place_id TEXT NOT NULL,
    role TEXT NOT NULL,
    confidence TEXT NOT NULL,
    PRIMARY KEY (soul_id, place_id)
);
CREATE INDEX idx_social_membership_place ON social_membership(place_id);
CREATE INDEX idx_social_membership_soul ON social_membership(soul_name);

-- ---- what they say --------------------------------------------------------------------- app

CREATE TABLE npc_default_greeting (
    soul_name TEXT NOT NULL,
    met INTEGER NOT NULL,
    words TEXT NOT NULL,
    issues TEXT NOT NULL DEFAULT '',
    model TEXT NOT NULL DEFAULT '',
    PRIMARY KEY (soul_name, met)
);

CREATE TABLE npc_knowledge (
    soul_name TEXT NOT NULL,
    topic_key TEXT NOT NULL,
    label TEXT NOT NULL,
    trust TEXT NOT NULL,
    text TEXT NOT NULL,
    after_quest TEXT,
    after_how TEXT,
    PRIMARY KEY (soul_name, topic_key)
);

CREATE TABLE npc_seed_answer (
    soul_name TEXT NOT NULL,
    intent TEXT NOT NULL,
    variant INTEGER NOT NULL,
    text TEXT NOT NULL,
    refers_to_quest_id TEXT,
    PRIMARY KEY (soul_name, intent, variant)
);

CREATE TABLE npc_remark (
    soul_name TEXT NOT NULL,
    remark_key TEXT NOT NULL,
    row TEXT NOT NULL,
    text TEXT NOT NULL,
    place TEXT NOT NULL DEFAULT '',
    PRIMARY KEY (soul_name, remark_key)
);

-- ---- the knowledge base ---------------------------------------------------------------- app

CREATE TABLE npc_kb_owner (
    soul_name TEXT NOT NULL,
    owner TEXT NOT NULL,
    PRIMARY KEY (soul_name, owner)
);

CREATE TABLE npc_kb_item (
    owner TEXT NOT NULL,
    item_id TEXT NOT NULL,
    scope TEXT NOT NULL,
    topic TEXT NOT NULL,
    kind TEXT NOT NULL,
    label TEXT NOT NULL,
    trust TEXT NOT NULL,
    price_kind TEXT,
    price_what TEXT,
    menu INTEGER NOT NULL DEFAULT 1,
    when_tag TEXT,
    fact TEXT NOT NULL,
    answer TEXT NOT NULL,
    withheld TEXT,
    source TEXT NOT NULL,
    flag TEXT,
    answer_sha TEXT NOT NULL,
    inputs_sha TEXT NOT NULL,
    model TEXT NOT NULL,
    written_at TEXT NOT NULL,
    PRIMARY KEY (owner, item_id)
);
CREATE INDEX idx_kb_item_sha ON npc_kb_item(answer_sha);

CREATE TABLE npc_kb_question (
    owner TEXT NOT NULL,
    item_id TEXT NOT NULL,
    n INTEGER NOT NULL,
    text TEXT NOT NULL,
    sha TEXT NOT NULL,
    PRIMARY KEY (owner, item_id, n)
);
CREATE INDEX idx_kb_question_sha ON npc_kb_question(sha);
-- The vectors these shas name are in the second file, at the end of this schema.

-- ---- the quests ------------------------------------------------------------------ app mod sched

CREATE TABLE npc_quest (
    quest_id TEXT PRIMARY KEY,
    hook_id TEXT NOT NULL,
    kind TEXT NOT NULL,
    giver_soul TEXT NOT NULL,
    giver_name TEXT NOT NULL,
    giver_trade TEXT NOT NULL,
    target_soul TEXT NOT NULL,
    target_name TEXT NOT NULL,
    target_trade TEXT NOT NULL,
    item_class TEXT NOT NULL,
    item_name TEXT NOT NULL,
    reward_tier TEXT NOT NULL,
    reward_groschen INTEGER NOT NULL,
    skill TEXT NOT NULL,
    difficulty TEXT NOT NULL
);
CREATE INDEX idx_quest_giver ON npc_quest(giver_soul);
CREATE INDEX idx_quest_target ON npc_quest(target_soul);

CREATE TABLE npc_quest_text (
    quest_id TEXT PRIMARY KEY,
    name TEXT NOT NULL,
    description TEXT NOT NULL,
    objective TEXT NOT NULL,
    log_started TEXT NOT NULL,
    log_completed TEXT NOT NULL,
    log_giver_dead TEXT NOT NULL,
    offer TEXT NOT NULL,
    thanks TEXT NOT NULL
);

CREATE TABLE npc_quest_lines (
    quest_id TEXT NOT NULL,
    field TEXT NOT NULL,
    text TEXT NOT NULL,
    PRIMARY KEY (quest_id, field)
);

CREATE TABLE npc_slate (
    quest_id TEXT PRIMARY KEY,
    circle_id TEXT NOT NULL,
    kind TEXT NOT NULL,
    giver_soul TEXT NOT NULL,
    target_soul TEXT NOT NULL,
    item TEXT NOT NULL,
    item_class TEXT,
    reward_groschen INTEGER NOT NULL,
    existing INTEGER NOT NULL,
    chain_id TEXT,
    chain_order INTEGER,
    requires_quest_id TEXT,
    dispute INTEGER NOT NULL,
    twist TEXT,
    clue INTEGER NOT NULL,
    reward_kind TEXT,
    reward_text TEXT,
    item_count INTEGER NOT NULL DEFAULT 1,
    place TEXT,
    story_id TEXT,
    story_role TEXT,
    after_quest_id TEXT,
    after_how TEXT
);
CREATE INDEX idx_slate_circle ON npc_slate(circle_id);

-- ---- their days ------------------------------------------------------------------ app mod sched

CREATE TABLE npc_routine (
    soul_name TEXT NOT NULL,
    level TEXT NOT NULL,
    slot INTEGER NOT NULL,
    start_min INTEGER NOT NULL,
    end_min INTEGER NOT NULL,
    minutes INTEGER NOT NULL,
    variation_min INTEGER NOT NULL,
    priority INTEGER NOT NULL,
    priorities TEXT NOT NULL,
    is_default INTEGER NOT NULL,
    tag TEXT,
    conditions TEXT,
    roles TEXT,
    behavior TEXT NOT NULL,
    behaviors TEXT NOT NULL,
    activity_family TEXT NOT NULL,
    hub_guid TEXT NOT NULL,
    hub_extras TEXT,
    place_id TEXT,
    place_name TEXT,
    pos_x REAL,
    pos_y REAL,
    pos_z REAL,
    path TEXT NOT NULL,
    PRIMARY KEY (soul_name, level, slot)
);
CREATE INDEX idx_npc_routine_family ON npc_routine(activity_family);
CREATE INDEX idx_npc_routine_place ON npc_routine(place_id);

-- sched
CREATE TABLE npc_routine_hub (
    soul_name TEXT NOT NULL,
    level TEXT NOT NULL,
    hub_guid TEXT NOT NULL,
    class TEXT,
    dump_x REAL,
    dump_y REAL,
    dump_z REAL,
    dumped_at TEXT NOT NULL,
    PRIMARY KEY (soul_name, level)
);

-- sched
CREATE TABLE npc_routine_object (
    level TEXT NOT NULL,
    guid TEXT NOT NULL,
    class TEXT,
    prefab TEXT,
    name TEXT,
    pos_x REAL,
    pos_y REAL,
    pos_z REAL,
    PRIMARY KEY (level, guid)
);

-- ==== npcs-vectors.sqlite ================================================================== app
--
-- The knowledge base's vectors, keyed by the sha of their text: the sha the rows of
-- npc_kb_item (answer_sha) and npc_kb_question (sha) carry as hex, here as its bytes
-- (the drop keys them by the first 16 hex digits of the sha256, so eight bytes; a whole
-- sha256 would be 32, and the app reads either).
-- EmbeddingGemma's 768 floats stored as signed bytes: vec is dim int8s, each vec[i] * scale
-- the component, with scale the vector's largest magnitude over 127. The app rebuilds the
-- floats on reading and normalises again. A quarter of the float32 original, and ranks as
-- it does: measured over 8 souls x 19 lines, the same best item on every one and every
-- score within 0.002, so the thresholds in app/src/knowledge.h keep their float32
-- calibration. (Keeping only the first 256 components, which the model is trained to allow,
-- Matryoshka-style, would be a twelfth, but scores ran higher and closer together and the
-- best item changed on one line in ten; the app cuts a wider query to the rows' dim, so
-- that layout reads too if it is ever wanted.) WITHOUT ROWID: the key is the row's address,
-- with no second tree beside it. tools/assemble_db.py writes it from the drop's float32
-- matrix (db/npc_kb_vector.f32, little-endian, with db/npc_kb_vector.index.tsv saying
-- which row is whose).
CREATE TABLE npc_kb_vector (
    sha BLOB PRIMARY KEY,
    dim INTEGER NOT NULL,
    scale REAL NOT NULL,
    vec BLOB NOT NULL
) WITHOUT ROWID;
