#pragma once

#include <limits>
#include <string>
#include <vector>

namespace kcd2 {

// One unnamed soul and the persona name generated for it.
struct Persona {
    std::string soul_name;      // the game's soul id, what <hasName> matches
    std::string full_name;      // what the nameplate should read
    std::string household_key;  // uniqueness group the name was drawn in
    std::string gender;         // "male", "female", or "not-defined"
    // One of the game's own characters, cast by exception (generate_kcd2_bios.py,
    // GAME_NAMED_EXCEPTIONS): gets the mod's menu and lines but keeps the game's name,
    // so no setUiName rule and no localization row are written for them.
    bool game_named = false;
    // npc.occupation is "beggar": the soul takes alms (food, a garment, groschen) from
    // Henry's pocket through the mod's @alms directive (behaviors.md 6.4).
    bool beggar = false;
};

// Filters are allowlists, applied together. All empty means "everything".
struct Filter {
    std::vector<std::string> settlements;  // npc.settlement
    std::vector<std::string> households;   // generated_name.household_key
    std::vector<std::string> souls;        // npc.soul_name
};

// Reads generated_name joined to npc. Throws std::runtime_error on any SQLite
// failure, including a database with no generated_name table.
std::vector<Persona> read_personas(const std::string& db_path, const Filter& filter);

// One small quest the bio generator dealt between two souls and wrote journal text
// for: the giver wants an item back from the target and pays for it. The guids are
// the souls' soul_id, which is what a Skald SoulAsset names in SharedSoulGuids; the
// texts are what Henry's journal shows, already in his voice.
struct Quest {
    std::string id;            // npc_quest.quest_id, twelve hex characters
    std::string kind;          // "retrieve"
    std::string giver_soul;    // soul_name of who asks
    std::string giver_name;    // their generated name
    std::string giver_guid;    // their soul_id
    std::string target_soul;   // soul_name of who has the thing
    std::string target_name;
    std::string target_guid;
    std::string target_gender;  // npc.gender of the holder: "male", "female" or ""; a woman
                                // is never offered the dread option (survey-followups.md §1)
    std::string giver_gender;   // npc.gender of the giver, for the voice of a hail
    std::string item_class;    // item class guid the game knows
    std::string item_name;     // "work axe"
    std::string reward_tier;   // money_change.xml row, "01 Few beers"
    int reward_groschen = 0;
    std::string reward_kind;   // the slate's kind of pay: "money", "goods", "training"...
    std::string reward_text;   // its words, "an hour's teaching in heavy weapons..."
    std::string skill;         // the dialogue check the generator priced it at, "persuade"
    std::string difficulty;    // its tier, "easy", "medium" or "hard"
    std::string name;          // journal title
    std::string description;
    std::string objective;
    std::string log_started;
    std::string log_completed;
    std::string log_giver_dead;
    // The journal's line when Henry gives the errand up to the giver's face (the app's
    // `abandon` directive, notes.md 13 Oct 2026); written by hand where the lines have
    // it, otherwise a plain one from the giver's name (abandoned_log()).
    std::string log_abandoned;
    // How this favour sits among the others (npc_slate, character-design plan §3.3). A
    // chain is a run of favours from one giver, each offered only once the one before
    // is done: chain_order counts from 1 along chain_id. A nest names the favour whose
    // giver is this one's holder: the holder hands this thing over without a check once
    // that favour is done, and not before. A dispute holder has a grievance against the
    // giver and a fifth menu line that carries it to him.
    std::string chain_id;
    int chain_order = 0;
    std::string requires_quest_id;
    bool dispute = false;
    // A link (quest-design/scenes.md): this favour is a later scene of a story and is
    // offered only once `after_quest_id` is done -- and, when `after_how` is set, only if
    // it ended one of the ways listed there ("truth|lied": the store's `how` for that
    // quest, "truth" meaning done and not lied, quiet, kept or dead). Two scenes after
    // the same quest with different hows are a branch; the one whose condition fails is
    // never offered or mentioned. The giver may be anybody, not the earlier scene's.
    std::string after_quest_id;
    std::string after_how;
    // The journal's completed log for each way the thing was won, from npc_quest_lines
    // as log_completed_<how>; empty falls back to log_completed. And the objective's
    // update when the holder dies with the thing on him.
    std::string log_completed_persuaded;
    std::string log_completed_threatened;
    std::string log_completed_paid;
    std::string log_completed_stolen;
    std::string log_completed_brokered;
    std::string log_completed_lied;
    std::string log_completed_nested;
    std::string log_target_dead;
    // The holder's side, spoken through the game's own dialogue menu while the quest
    // is open: Henry's options and what the holder says to each, from npc_quest_lines.
    // Any left empty is filled with a plain line built from the names, so a quest whose
    // holder has not been written yet is still finishable.
    std::string holder_open;          // Henry: what he came about
    std::string holder_refuse;        // holder: the first excuse
    std::string holder_persuade;      // Henry: the reasonable way (a persuade check)
    std::string holder_persuade_yes;  // holder: talked round
    std::string holder_persuade_no;   // holder: not moved
    std::string holder_threaten;      // Henry: the hard way (a dread check)
    std::string holder_threaten_yes;  // holder: frightened into it
    std::string holder_threaten_no;   // holder: not frightened
    std::string holder_pay;           // Henry: money for their trouble
    std::string holder_pay_yes;       // holder: takes it
    std::string holder_leave;         // Henry: never mind
    std::string holder_leave_reply;   // holder: sees him off
    // A dispute's fifth line and what follows it: Henry offers to carry the grievance
    // to the giver, the holder names his terms; and, back with the giver's word (or a
    // lie that he gave it), Henry says so and the holder hands over without a check.
    std::string holder_broker;        // Henry: I'll speak to him about it
    std::string holder_broker_reply;  // holder: do that, and you can have it
    std::string holder_brokered;      // Henry: he admits it
    std::string holder_brokered_yes;  // holder: hands it over
    // A nest's way in: the holder's own errand is done, so he hands over as promised.
    std::string holder_nested;        // Henry: I got your thing back, now his
    std::string holder_nested_yes;    // holder: hands it over
    // An exposure (Words Must Not Fail's second scene, notes.md 21 Oct 2026): a retrieve
    // whose holder keeps a thing against the giver (the priest keeps her prayer as proof of
    // blasphemy for the archdeacon) and a secret of his own (he sleeps in a married woman's
    // bed). The Lua watches the nights (WatchExpose): Henry within exposeMetres of the
    // holder with the holder within exposeWithMetres of `seen_with` between hour_from and
    // hour_to is the errand's mid-moment -- the store's `seen` (letter s), a token for the
    // graph (Updated, `log_caught`, marker on the holder) and the holder's menu gains the
    // `expose` row (gated on the quest's seen global): Henry says what he saw, the holder
    // gives the thing up to keep it quiet (`expose_yes`), and the errand ends `exposed`
    // (log_completed_exposed). The holder is otherwise unyielding; the thing is not seeded
    // on him when `item_of` names the earlier quest whose Document he was handed (the
    // dictated letter, whichever variant was written: the Lua reads that quest's digits).
    // The witness's household expects Henry from the accept (the giver's welcome shape), or
    // he could not be in the house at night to see anything.
    std::string holder_expose;        // Henry: I saw where you sleep
    std::string holder_expose_yes;    // holder: gives in
    std::string log_caught;
    std::string log_completed_exposed;
    // The bedside itself (notes.md 22 Oct 2026): at the catching the Lua says Henry's
    // `wake` in his voice and asks the engine for the holder's Talk, which opens -- then,
    // or when Henry presses Talk on the sleeper -- on `wake_reply` instead of the day's
    // open/refuse, with three rows under it: `expose` as above; `denounce`, Henry walking
    // out with nothing to tell the parish where its priest sleeps (the `denounced` port and
    // `log_denounced`; the holder's `denounce_reply`; the errand then ends `denounced` on
    // Henry's report to the giver, `log_completed_denounced`, the thing never coming back);
    // and `blackmail`, the thing and `blackmail_groschen` besides on a hard Speech check
    // (coerce) -- won, `blackmail_yes` and the `exposed_paid` ending (log_completed_
    // exposed_paid); lost, `blackmail_no`, the thing alone and the plain `exposed`. After a
    // denouncing his Talk is only `open_denounced`. All or none, with the lines above.
    std::string holder_wake;          // Henry: wakes him
    std::string holder_wake_reply;    // holder: what will it take
    // Henry's aside at the door (notes.md 23 Oct 2026): said in his voice, once a night, as
    // the holder goes in to the witness in the evening with Henry near enough to see the
    // door (WatchExpose, `enters`). Optional, on an exposure.
    std::string holder_enters;
    // Where he sleeps when he is with her: the x,y of `routine_bed_target` (the scheduler
    // override's `Class/name@x,y`, build_kcd2_scheduler.py), which the watch anchors the
    // door aside and the street's "at her side" on, since she is about her house and yard
    // of an evening and is no fixed point (notes.md 25 Oct 2026). NaN when the line has no
    // spot.
    double bed_x = std::numeric_limits<double>::quiet_NaN();
    double bed_y = std::numeric_limits<double>::quiet_NaN();
    bool has_bed_spot() const { return bed_x == bed_x && bed_y == bed_y; }
    // The giver's carrying (notes.md 25 Oct 2026): the app's `fast_travel` directive, owed
    // on the giver's written answer to Henry's row (the lines `fast_travel`,
    // `fast_travel_reply`, `fast_travel_later`, `fast_travel_later_reply` are the app's),
    // has the Lua take Henry to `fast_travel_to` ("x,y,z", level units; the Kourim gate
    // inside the walls) once the conversation is gone and wind the clock on to
    // `fast_travel_at` ("hh:mm" of the day; never back), as the game's own carriage does
    // (FastTravelArm). The hours Henry can ask it in (`fast_travel_hours`, "from-to") are
    // the app's to keep; the mod carries whenever told. All three or none.
    std::string fast_travel_to;
    double fast_travel_at = -1.0;  // hours of the day, decimal; -1 for none written
    bool has_fast_travel() const { return !fast_travel_to.empty() && fast_travel_at >= 0; }
    // The holder turning on Henry in the street (notes.md 23 Oct 2026, n8): on his way to
    // the witness in the evening, the game's own perception of Henry (the graph's
    // PerceptionTrigger on him, reported to the Lua through a console line) is the
    // holder's to judge: outdoors, in the walk's hours, Henry near enough, and the mod
    // marks him `followed` for the night -- the graph flips his sleep links to his own bed
    // (alert_tag) and his Talk opens on him, not Henry, with `followed` and Henry's one
    // answer `followed_reply`, the journal noting it (`log_followed`). Off again in the
    // morning, so the next night is another chance. All three or none, on an exposure.
    std::string holder_followed;      // holder: are you following me
    std::string holder_followed_reply;  // Henry: only walking
    std::string log_followed;
    // Henry to himself about the street (notes.md 25 Oct 2026), optional on a `followed`:
    // `follow_warn` when the walking holder first comes near enough in the walk's hours
    // that the next tick could be the sighting (emit.hpp kFollowWarnMetres), once a night,
    // so the player knows to hang back; `follow_over` once the holder has turned on him and
    // the words are said, so the player knows the night is lost and tomorrow is another.
    std::string holder_follow_warn;
    std::string holder_follow_over;
    std::string holder_denounce;      // Henry: nothing; the parish will hear
    std::string holder_denounce_reply;
    std::string holder_blackmail;     // Henry: the thing, and coin for my silence
    std::string holder_blackmail_yes;
    std::string holder_blackmail_no;
    std::string holder_open_denounced;
    std::string log_denounced;
    std::string log_completed_denounced;
    std::string log_completed_exposed_paid;
    int blackmail_groschen = 0;
    std::string item_of;              // the quest id whose Document is the thing
    std::string seen_with;            // soul_name of the one he is caught with
    std::string seen_with_name;
    std::string seen_with_guid;
    // A deliver (kind "deliver"): the giver wrote a letter and Henry carries it to the
    // target. The letter is the mod's own Document item (item_class, one row per letter)
    // whose page is `letter`; the receiver's two rows go through the game's own Talk like
    // the holder's; and the writer's side -- Henry says it is delivered (`report`, true or
    // not) or that he kept it (`keep`) -- is the app's, which answers with `thanks` or
    // `kept_reply`. Its endings: the plain log_completed for delivered, then kept and
    // lied; and log_opened is the objective's update when Henry breaks the seal.
    std::string letter;
    std::string receiver_hand;        // Henry: a letter for you, from X
    std::string receiver_hand_reply;  // receiver: takes it
    std::string receiver_hand_read;   // receiver: having read it there, his answer (usually empty)
    std::string receiver_hand_opened; // receiver: a word on the broken seal, said when Henry has read
                                      // it on the way (usually empty; Holy Jest's monk, 9 Oct 2026)
    // (the receiver's leave / leave_reply are read into holder_leave / holder_leave_reply:
    // the generator writes them under the same names)
    std::string giver_report;         // Henry: it is delivered
    std::string giver_keep;           // Henry: I have not carried it and will not
    std::string giver_kept_reply;     // giver: to that
    std::string log_completed_kept;
    std::string log_opened;
    // A deliver whose thing is not a letter but a thing of the game's own carried back to
    // its owner (npc_quest_lines "item_kind": "thing"; In the Stocks' second scene, the
    // parish chalice back to the priest, notes.md 9 Oct 2026): no Document is made for it,
    // nothing watches a page, and the receiver's menu is an interrogation rather than a
    // taking -- he asks how Henry came by it, and Henry's answers are the rows below. Empty
    // (a letter) for every other deliver.
    std::string item_kind;
    // The interrogation (the receiver's menu, when `tell_truth` and its reply are written):
    // after `hand` / `hand_reply` (the question), Henry tells the truth (`tell_truth`, the
    // plain completion -- how "truth"); or gives the cover story the giver asked for
    // (`tell_cover`), which the receiver answers with a second question (`tell_cover_reply`,
    // "how did you know to bring it here?") and a nested pair: the giver is named
    // (`tell_named`, how "pinned": the receiver stays suspicious of him) or the robbery
    // was common talk (`tell_heard`, how "covered": satisfied), or a tale the receiver sees
    // through at once (`tell_vision`, `tell_tale`, how "fibbed": he takes the thing, asks
    // whether Henry knows what lying to a priest is, and the talk ends; notes.md 14 Oct
    // 2026); or Henry will not say, on a Speech check (`tell_evade`, won `tell_evade_yes`
    // how "evaded", lost `tell_evade_no` how "caught") or a dread one (`tell_threat`, the
    // same two hows). Every row hands the thing over and ends the talk; the how is kept in
    // the store for the giver's answer (the app's told_reply_<how>) and the journal's
    // log_completed_<how>; the untrue hows close a scene that waits on "truth". A lost check
    // costs the town's regard (the graph's reputation rewards on complete_evaded /
    // complete_caught), a tale seen through the receiver's (complete_fibbed).
    std::string tell_truth, tell_truth_reply;
    std::string tell_cover, tell_cover_reply;
    std::string tell_named, tell_named_reply;
    std::string tell_heard, tell_heard_reply;
    std::string tell_vision, tell_vision_reply;
    std::string tell_tale, tell_tale_reply;
    std::string tell_evade, tell_evade_yes, tell_evade_no;
    std::string tell_threat, tell_threat_yes, tell_threat_no;
    std::string log_completed_pinned, log_completed_covered, log_completed_fibbed, log_completed_evaded,
        log_completed_caught;
    // A deliver whose letter Henry writes himself, at the giver's dictation (npc_quest_lines
    // dictate_*; Words Must Not Fail, notes.md 9 Oct 2026): the giver says the prayer in
    // kDictationParts parts, and after each Henry chooses how it goes down -- as said
    // (outcome 0), left out at his asking (1, where `reconsider_<k>` is written), softened
    // (2) or made larger (3). The choice is the app's (its menu after the accept); what
    // reaches the mod is the three outcomes as a command `wrote_<digits>`, kept in the store
    // and dumped back (the flag W<digits>), and the letter made then is the Document of that
    // combination: one item class per combination (letter_class), its page composed from
    // `letter_open`, the chosen `letter_<k>` / `letter_<k>_soft` / `letter_<k>_over` (nothing
    // for a part left out) and `letter_close`. The receiver reads it part by part on the
    // game's Talk -- `read_<k>`, `read_<k>_soft`, `read_<k>_over`, keyed on the dialogue
    // globals the Lua keeps from the store (quest_flag_variable p1..p3) -- and gives a
    // verdict (the `verdict` global): the prayer taken (`verdict_prayer`, the plain
    // completion), a curse on a living man refused (`verdict_cursed`, how "cursed": the
    // last part as said) or a scribe's sport seen through (`verdict_mocked`, how "mocked":
    // any part made larger). `log_written` is the objective's Updated line when the letter
    // is made (the token's trigger, as a helper's naming is).
    static constexpr int kDictationParts = 3;
    std::string dictate[kDictationParts];
    std::string reconsider_row[kDictationParts];   // empty: the part cannot be left out
    std::string letter_open, letter_close;
    std::string letter_part[kDictationParts][4];   // by outcome; [k][1] is never read
    std::string read_part[kDictationParts][4];     // the receiver's remark, by outcome; [k][1] unused
    std::string verdict_prayer, verdict_cursed, verdict_mocked;
    std::string log_written, log_completed_cursed, log_completed_mocked;
    // Which parts, written as said, the receiver refuses as a curse (npc_quest_lines
    // `curse_parts`, 1-based, space- or comma-separated); the last part when unwritten,
    // since a dictation of this shape builds to its curse. Softened or left out, no part
    // is a curse; made larger, any part is the scribe's sport instead.
    std::vector<int> curse_parts;
    // A retrieve whose giver can be lied to with the thing in Henry's coat (npc_quest_lines
    // "lie_have", Henry's row; In the Stocks, 9 Oct 2026): the hand-in is no longer on sight
    // but a row of the app's, and beside it the lie, on a Speech check. The mod's part is
    // the lied ending the retrieve then has (log_completed_lied), and that the lie pays
    // nothing and takes nothing back.
    std::string giver_lie_have;
    // A find (kind "find"): the giver wants `count` of a common thing (item_class, from the
    // shipped tables) and nobody holds them. No target, no holder menu, no letter: the
    // errand is the app's whole way through, and the hand-in takes `count` from Henry.
    // From npc_quest_lines as "count"; 1 when the generator wrote none.
    int count = 1;
    // A find's least freshness that counts, on the engine's 0..1 item health (from
    // npc_quest_lines "min_health"; 0, the default, is any). Joan's marigolds want 0.9:
    // the graph's CreateItemClassDescriptor takes it as MinimumHealth, so the "n/5" and
    // the return objective see only fresh ones, and the Lua's hand-in counts and takes
    // the same (Moonshine.CountFresh / TakeFresh), since GetCountOfClass and the by-class
    // takers would count stale ones and hand over the stalest first (health_probe.lua).
    double min_health = 0;
    // A travel (kind "travel"): the giver wants a look taken at the target, who is the
    // map's marker and knows nothing of it. No item at all. The graph's DistanceTrigger
    // fires `log_seen` (the objective's update) when Henry comes near the target and the
    // marker goes back to the giver; the Lua watches the same distance for the store's
    // `seen`. Henry's three rows at the giver's -- the truth (`tell`, the plain
    // log_completed, paid), a lie (`lie`, log_completed_lied, paid) and silence (`quiet`,
    // log_completed_quiet, unpaid) -- are the app's, answered with `thanks`, `lie_reply`
    // and `quiet_reply`.
    std::string giver_tell;           // Henry: what he saw
    std::string giver_lie;            // Henry: something else
    std::string giver_quiet;          // Henry: went, and will not say
    std::string giver_lie_reply;      // giver: to the lie, believed
    std::string giver_quiet_reply;    // giver: to the silence, unpaid
    std::string log_seen;
    std::string log_completed_quiet;
    // The face the Lua puts on the target while the errand runs (npc_quest_lines
    // "seen_mood": sad, angry, nervous, pensive, happy, drunk, injured, arrogant, or none /
    // empty), a looped fa_idle_* clip on layer 13 above the routine (behaviors.md §3), so
    // that what Henry was sent to look at is something the game shows. Off once seen.
    std::string seen_mood;
    // A travel's brawl (npc_quest_lines "brawl_with": soul_names, space- or comma-separated):
    // the ones who set on the target the moment Henry arrives, so that what he was sent to
    // look at is a fight and `log_seen` can say so. Resolved to soul_ids at read time; the
    // target himself is dropped from the list, a name the npc table has not is an error.
    // Nobody in it speaks or is marked, so they need no name and no role.
    std::vector<std::string> brawl_souls;   // soul_name, as written
    std::vector<std::string> brawl_guids;   // their soul_id
    // The giver's house open to Henry: from the moment the errand is taken up, the giver
    // and everyone who sleeps under the same roof expect him there -- the game's own
    // `crime_trespass_expected` context on their souls, the way the shipped quests open a
    // hunter's house to the player (expectedeffect, 248 uses) -- so walking into Joan's
    // rooms with her flowers is not a trespass (notes.md, 7 Oct 2026). Every giver's since
    // 8 Oct 2026 (Anna's house called Henry out with her errand taken; notes.md); a quest
    // line "welcome": "0" turns it off, and a giver who sleeps in a dormitory of more than
    // kHouseholdMost (a lodging house is not a home) gets none. The rooms themselves cannot
    // be made public: their TriggerArea binds to a quest only through the level's own Link
    // rows, so the HUD may still colour them private while nobody minds. `welcome_guids` is
    // the household, resolved at read time from the beds: every soul whose sleeping spot is
    // within kHouseholdMetres of the giver's own, on the giver's level, the giver first.
    bool welcome = true;
    std::vector<std::string> welcome_guids;
    static constexpr std::size_t kHouseholdMost = 24;
    // The giver's working day given over to something else until Henry takes the errand
    // up (npc_quest_lines "routine": a smart-object behaviour name, "crying" for Joan):
    // the graph holds the scheduler tag routine_tag(modid, id) on while the quest is still
    // None, and the level's scheduler override that scripts/build_kcd2_scheduler.py ships
    // has the giver's work link pointed at the nearest wrapper hub with that behaviour
    // under that tag, and her way to work on a home-hub link dead under it, for the work
    // hours (behaviors.md §4.6). The accept turns it off and she is at her chores again.
    // The giver also carries the game's `crime_dontCommentOnPlayer` preset while it holds,
    // so nobody weeping on a bench says "At your service, sir" or "What a good doggy".
    // Empty for every other quest.
    std::string routine;
    // How near a bed is to count as the same roof: the next room of the same house (Joan's
    // catchpoles sleep 5.3 m from her) and not the neighbours (the nearest other house in
    // Horany is 30 m off).
    static constexpr double kHouseholdMetres = 8.0;
    // A beat (kind "beat"): the giver has been wronged by the target and wants him made to
    // answer for it and make it right; the fight is the player's last resort, not the ask.
    // No item. The target's menu is the full holder dialogue -- open/refuse, persuade,
    // threaten, pay, leave -- read into the holder_* slots above, and won the retrieve's
    // way (how "persuaded", "threatened", "paid"); the graph's SoulUnconsciousTrigger on the
    // target fires `log_beaten` (the objective's update, marker back on the giver) and
    // closes the menu; the Lua watches IsUnconscious for the store's `beaten` and IsDead for
    // `target_dead`. Henry's rows at the giver's -- `report` (the man agreed, or so Henry
    // says: the plain completion, or the lie), `fought` (Henry knocked him down, how
    // "fought", paid) and `confess` (the target is dead, unpaid) -- are the app's, answered
    // with `thanks`, `fought_reply` and `dead_reply`.
    std::string giver_fought;         // Henry: he knocked the man down
    std::string giver_confess;        // Henry: he is dead
    std::string giver_fought_reply;   // giver: to the man having been knocked down
    std::string giver_dead_reply;     // giver: to the death (a mind's too)
    std::string log_beaten;
    std::string log_completed_fought;
    std::string log_completed_dead;   // a beat's or a mind's: the target died first
    // A pray (kind "pray"): kneel at one of the town's praying spots for the giver. No item
    // and no target. The Lua watches Henry's position against `spot_pos` ("x,y,z", level
    // units) and, when he has stood there long enough, makes a token item of the quest's own
    // class in his pocket for a moment; the graph's ItemDescriptorTrigger on that class fires
    // `log_prayed` (the objective's update). `spot_text` is the place as the giver names it.
    // Henry's one row at the giver's, `report`, is the app's: the plain completion when he
    // has knelt, `lied` when he has not.
    std::string spot_pos;
    std::string spot_text;
    std::string log_prayed;
    // The giver's hail (npc_quest_lines `hail`; notes.md 13 Oct 2026, Benesh): a line he
    // shouts when Henry comes near with the errand still unasked -- captioned and voiced
    // by the app in the giver's stand-in voice, outside any conversation. Empty for a
    // giver who waits to be spoken to. `dress_target` is a clothing preset guid the Lua
    // puts on the holder once a session (Paul Schmidt's fine new coat, the one Benesh
    // names); empty leaves him as the level dressed him. `hail_hours` (notes.md 25 Oct
    // 2026, Christine: "only active when she's in her daytime routine, not sleeping or
    // eating") is the giver's day, `H-H` in game hours (8-20): outside it the hail is not
    // shouted and her marker is off the map, the graph's TimeOfDayInterval and the Lua's
    // HourIn agreeing. Both -1 (the default, or a blank) is any hour, the hail as before.
    std::string hail;
    int hail_hour_from = -1;
    int hail_hour_to = -1;
    bool has_hail_hours() const { return hail_hour_from >= 0 && hail_hour_to >= 0 && hail_hour_from != hail_hour_to; }
    std::string dress_target;
    // A retrieve whose holder will not be moved (npc_quest_lines "unyielding": "1"; In the
    // Stocks, notes.md 13 Oct 2026, Paul Schmidt): the persuade and threaten rows are plain
    // refusals with no check behind them -- the sacristan takes offence whatever Henry's
    // Speech -- and there is no pay row. Each fires the menu's `rebuffed` port (the
    // objective's Updated line `log_rebuffed`, marker back on the giver) and the Lua's
    // QuestRebuffed (the store's `rebuffed`, letter n), which the app reads to have the
    // giver send Henry at the chest anyway (greet_rebuffed) and name the one who can let
    // him in. The thing is then got the quiet way, or through the helper below, and the
    // hand-in is the retrieve's stolen ending.
    bool unyielding = false;
    std::string log_rebuffed;
    // The thing in a chest rather than a pocket (npc_quest_lines "chest_pos": "x,y,z" in
    // level units, "chest_lock": 0..1, the engine's lock difficulty): the Lua spawns a
    // Stash of the mod's own at the spot when Henry first comes near (behaviors.md §6.3;
    // WatchChest), locked at that difficulty and keyed to the quest's own key class
    // (key_class), and moves the thing into it; nothing is seeded on the holder. An optional
    // fourth figure is the chest's yaw in degrees (0 faces +y, 90 faces +x), so it can stand
    // square to a room's furniture (notes.md 14 Oct 2026). Taking the thing out is the
    // errand's mid-moment: the watch sees it arrive in Henry's pocket once the chest has
    // been filled and makes a token of the quest's taken class (taken_class) for the graph's
    // trigger, which completes the fetch objective and opens the return (the store's `taken`).
    // Beside the thing, optionally, a paper ("chest_paper": the page, newlines its
    // paragraphs; "chest_paper_name": what the inventory calls it) -- In the Stocks' guard
    // rota, which says what the sacristan was up to.
    std::string chest_pos;
    double chest_lock = 0.9;
    std::string chest_paper;
    std::string chest_paper_name;
    // The one who can let Henry in (npc_quest_lines "helper_soul", "helper_name"): a soul
    // of the holder's household with a menu of their own (q_<id>_helper.xml, under a role
    // of their own), opened once the giver has named them -- the app's `named` directive
    // (the store's `named`, letter a; the Lua makes a token the graph's ItemDescriptorTrigger
    // sees, which writes `log_named` and opens the optional objective `objective_helper`,
    // tracked, marker on the helper). Rows: the door (`helper_door`, for helper_door_price
    // groschen: the holder's household carries the game's "expected" contexts from then
    // on, as a giver's does for a welcome -- the store's `letin`, letter i, `log_letin`),
    // the key (`helper_key`, a Speech check at helper_skill for helper_key_price groschen:
    // won, the key class is made in Henry's pocket -- the store's `keyed`, letter q,
    // `log_keyed`; lost, `helper_key_no`, once), and the way out.
    std::string helper_soul;
    std::string helper_name;
    std::string helper_guid;
    std::string helper_gender;
    std::string helper_skill;         // easy, medium, hard; the key row's tier
    int helper_door_price = 0;
    int helper_key_price = 0;
    std::string helper_open, helper_open_reply;
    std::string helper_door, helper_door_yes;
    std::string helper_key, helper_key_yes, helper_key_no;
    std::string helper_leave, helper_leave_reply;
    std::string log_named;
    std::string log_letin;
    std::string log_keyed;
    std::string objective_helper;
    // The holder's household (the `letin` contexts above): the holder and everyone who
    // sleeps under the same roof, by read_household's rule, with the helper beside them.
    std::vector<std::string> holder_household_guids;
    // A leave (kind "leave", quest-design/set-aside-stories.md §2): carry a thing of the
    // giver's -- made in Henry's pocket at accept, like a letter -- to a place and put it
    // down there, between `hour_from` and `hour_to` (game hours; equal or both -1 for any
    // hour). The Lua watches his position against `spot_pos` and the hour, and while he
    // stands there in the window with the thing it arms the leave key (F9) and takes a
    // drop by hand as the same; placing makes the token (log_left, the objective's update),
    // and `cue_sound` (an audio trigger name) is played from the dark a little later
    // (`heard` in the store). Henry's three rows at the giver's are the travel's (tell,
    // lie, quiet). A return (kind "return", §4) is the same errand with the ending table
    // turned round: the things are somebody else's, taken from a place that is a grave,
    // and Henry puts them back; `loot_classes` names the level's own loot that lies there,
    // and leaving with any of it, or with the goods still held, is cursed (a buff for two
    // days, lifted early if he goes back and puts them down).
    int hour_from = -1;
    int hour_to = -1;
    std::string cue_sound;
    std::string loot_classes;         // item class guids, space-separated
    std::string log_left;
    // A mind (kind "mind"): look in on the target `count` times, hours of game time apart.
    // The target's menu is his own: Henry's `check` and one reply per visit (the sequences
    // are gated on the visit count and whether the next is due, dialogue globals the Lua
    // keeps), each firing its own Out port for the journal's `log_check_N`; and `early` /
    // `early_reply` when Henry is back too soon. Henry's rows at the giver's are `report`
    // (the count done, or a lie) and `confess` (the target is dead, unpaid).
    std::string target_check;         // Henry: who sent him and why
    std::string target_check_reply[3];
    std::string target_early;         // Henry: just looking in
    std::string target_early_reply;   // target: it has not been long enough
    std::string log_check[3];
    // An ask (kind "ask", quest-design/scenes.md): the giver wants to know something only
    // the target can tell, and the target keeps it back. No item. The target's menu is the
    // beat's -- open/refuse (the tale he tells everyone), persuade, threaten, pay, leave --
    // read into the holder_* slots and won the retrieve's way; the three yes lines are the
    // truth. The handover port fires `log_told` (the objective's update, marker back on the
    // giver). No unconscious trigger: knocking the one asked down is not a way of asking.
    // Henry's rows at the giver's are the travel's -- `tell` (the truth, once told; the plain
    // completion, paid), `lie` (any time, paid), `quiet` (once told, unpaid) -- and the
    // beat's `confess` when the target is dead (log_completed_dead, unpaid), answered with
    // `thanks`, `lie_reply`, `quiet_reply` and `dead_reply`.
    std::string log_told;
    // An ask's gift (npc_quest_lines "gift_classes": item class guids, space-separated):
    // the bribe the one asked presses on Henry with the truth, his price for the word
    // stopping there (Butcher Emmeram's fox-fur hat and gloves in *Fox for Mutton*). Made
    // offered on every winning branch, whatever Henry then says to the giver; the yes lines
    // name it. Empty for an ask with no bribe in it.
    std::string gift_classes;
    // What follows the offer, when there is one: a menu of two on the winning branch. Henry
    // takes it (`gift_take`, the holder's `gift_take_reply`; the classes are made in his
    // pocket) or refuses it (`gift_refuse`), and the holder answers the refusal with a bucket
    // of guts (`gift_refuse_reply`: Henry's clothes are fouled and he is Smelly for a day).
    std::string holder_gift_take;          // Henry: takes the bribe
    std::string holder_gift_take_reply;    // holder: sees him off with it
    std::string holder_gift_refuse;        // Henry: will not be bought
    std::string holder_gift_refuse_reply;  // holder: the bucket
    // An ask whose word is carried on (npc_quest_lines "onward_soul", "onward_name"; the
    // FavourSpec's `onward`): told the truth, the giver pays and in the same breath sends
    // Henry to a third soul with it (Heinz to the master of the butchers' guild, in *Barking
    // Stew*; notes.md 5 Oct 2026: one errand in the journal, not two). The app's `onward`
    // directive at the telling pays the reward, writes `log_onward` (the objective's update,
    // marker on the one sent to) and opens the last objective (`objective_onward`); the one
    // sent to has a receiver's menu of their own (`onward_hand`, Henry saying it; their
    // `onward_hand_reply` and `onward_hand_read`), and the errand completes on that row, as a
    // word deliver's does. The lie and the silence end at the giver as any ask's. Empty for
    // an ask that ends at the giver.
    std::string onward_soul;
    std::string onward_name;
    std::string onward_guid;
    std::string onward_hand;          // Henry: who sent him and what the matter is
    std::string onward_hand_reply;    // the one sent to: what they say to that
    std::string onward_hand_read;     // the one sent to: what they make of it
    std::string log_onward;
    std::string objective_onward;
    // A reward that is a thing (npc_quest_lines "reward_class": one item class guid): made
    // in Henry's pocket at the hand-in beside whatever the words promise and the groschen,
    // by PayReward (Heinz's digestive potion in *Barking Stew*). Empty for a reward in
    // words or coin alone, which is nearly all of them.
    std::string reward_class;
    // A find's keepsake (npc_quest_lines "keepsake_class", "keepsake_name", with "spot_pos"
    // the patch the things grow in; *Widow's View*, notes.md 5 Oct 2026): a thing of the
    // mod's own that the Lua lays in the grass at the spot when the errand is taken up
    // (PlaceItem on a spot entity, behaviors.md §6.1a), and that the objective's marker
    // sits on (an ItemOnGroundAsset of its class, §10.4) in place of the giver -- the first
    // marker on a place rather than a soul. Picked up, the store notes it (`keepsake`, flag
    // letter k) and the app's giver answers the hand-in with `thanks_keepsake`; the hand-in
    // takes it with the things. Empty for every other find.
    std::string keepsake_class;
    std::string keepsake_name;
    std::string giver_thanks_keepsake;
    // An ambush (kind "ambush", quest-design/set-aside-stories.md §5): the target is spoken
    // to, and his menu has three stages, gated by a dialogue global the Lua keeps from the
    // store (0 before, 1 directed, 2 ambushed). First Henry says who sent him (`direct`) and
    // the target gives him the way (`direct_reply`) -- false directions, written down as
    // the quest's Document (item_class; its page is `letter`, which the generator writes as
    // "directions") and made in Henry's pocket on that row's exit (log_directed, the
    // objective's update). Back too soon, `early` / `early_reply`. The Lua watches Henry's
    // position against `spot_pos` within the hour window, and when he is there with the
    // directions on him makes the token: the graph's trigger (log_ambushed) sets
    // `brawl_souls` on the player. Back at the target, frightened, `truth` / `truth_reply`
    // is the real way (the handover port: log_told, marker back on the giver). Henry's rows
    // at the giver's are the ask's: tell, lie, quiet, and confess when the target is dead.
    std::string target_direct;
    std::string target_direct_reply;
    std::string target_truth;
    std::string target_truth_reply;
    std::string log_directed;
    std::string log_ambushed;
    // A buy (kind "buy", §5): a retrieve whose holder names a price in two parts -- a thing
    // of the giver's (`carry_class`, a custom item made in Henry's pocket at accept, named
    // `carry_name`) and `pay_groschen`, which the giver hands Henry at accept too -- and
    // whose menu is the door and the pay row (gated on both being on Henry) and leaving: no
    // argument and no threat, the holder is a magician and the terms are the terms. The pay
    // row's exit moves the thing to the holder, pays him and hands over the item (a
    // Document, the charm; its page is `letter`, written as "charm"). Henry's rows at the
    // giver's: tell (the plain completion, paid) and lie -- the price was more -- which is
    // paid with pay_groschen on top (log_completed_lied). Pickpocketed off the holder is the
    // retrieve's stolen.
    std::string carry_class;
    std::string carry_name;

    bool is_deliver() const { return kind == "deliver"; }
    bool is_find() const { return kind == "find"; }
    bool is_travel() const { return kind == "travel"; }
    bool is_beat() const { return kind == "beat"; }
    bool is_pray() const { return kind == "pray"; }
    bool is_mind() const { return kind == "mind"; }
    bool is_ask() const { return kind == "ask"; }
    bool is_leave() const { return kind == "leave"; }
    bool is_return() const { return kind == "return"; }
    bool is_ambush() const { return kind == "ambush"; }
    bool is_buy() const { return kind == "buy"; }
    bool is_retrieve() const { return kind == "retrieve"; }
    // A leave or a return: an errand to a place, with a thing to put down there.
    bool is_spot() const { return is_leave() || is_return(); }
    // Whether the quest's item is a Document of the mod's own with a page the generator
    // wrote (`letter`): a deliver's letter, an ambush's directions, a buy's charm.
    bool has_document() const {
        return (is_deliver() || is_ambush() || is_buy()) && !item_class.empty() && !carries_thing();
    }
    // A deliver of a thing of the game's own rather than a letter (item_kind "thing"): the
    // parish chalice carried back to the priest.
    bool carries_thing() const { return is_deliver() && !item_class.empty() && item_kind == "thing"; }
    // A deliver whose receiver asks how Henry came by it, with the rows above written.
    bool has_interrogation() const { return is_deliver() && !tell_truth.empty() && !tell_truth_reply.empty(); }
    // A deliver with no letter: word carried to the one it is for (the guild master told
    // what the butcher sells, in *Fox for Mutton*). Nothing is made at accept, the receiver's
    // row is Henry telling him, and the errand completes on that row (QuestDelivered), since
    // there is nothing to carry back to the giver but the saying so.
    bool carries_word() const { return is_deliver() && item_class.empty(); }
    // A deliver whose letter Henry writes at the giver's dictation (above): every part said,
    // every part's as-said page text, the letter's open and close, and the verdicts written.
    bool has_dictation() const {
        if (!has_document() || !is_deliver()) return false;
        for (int k = 0; k < kDictationParts; ++k) {
            if (dictate[k].empty() || letter_part[k][0].empty()) return false;
        }
        return !letter_open.empty() && !letter_close.empty() && !verdict_prayer.empty() &&
               !verdict_cursed.empty() && !verdict_mocked.empty();
    }
    // The outcomes part k of a dictated letter can have: as said, softened and made larger
    // where the page text for each is written; left out where the asking row is.
    std::vector<int> dictation_outcomes(int k) const {
        std::vector<int> out{0};
        if (!reconsider_row[k].empty()) out.push_back(1);
        if (!letter_part[k][2].empty()) out.push_back(2);
        if (!letter_part[k][3].empty()) out.push_back(3);
        return out;
    }
    // Every combination of outcomes, as the digit strings the store and the item classes
    // carry ("000", "023", ...), in order.
    std::vector<std::string> dictation_variants() const;
    // The letter's page for one combination: open, the parts as chosen, close.
    std::string dictated_letter(const std::string& wrote) const;
    // An ask whose word is carried on to a third soul once the giver has been told (above).
    bool carries_on() const { return is_ask() && !onward_soul.empty(); }
    // A find whose marker is a keepsake laid at the spot the things grow (above).
    bool has_keepsake() const { return is_find() && !keepsake_class.empty() && !spot_pos.empty(); }
    // Whether the Lua makes a token item for the graph to see: a pray's kneeling, a leave's
    // or a return's thing put down, an ambush sprung, a helper named by the giver, a
    // dictated letter written. Only when the journal has the line it would write.
    bool has_token() const {
        return (is_pray() && !log_prayed.empty()) || (is_spot() && !log_left.empty()) ||
               (is_ambush() && !log_ambushed.empty()) || has_helper() || has_written_log() || has_expose();
    }
    // A retrieve whose holder can be caught where he should not be (above): the witness,
    // the row and the journal's lines all written, and a night window to catch him in.
    bool has_expose() const {
        return is_retrieve() && has_holder() && !seen_with.empty() && !holder_expose.empty() &&
               !holder_expose_yes.empty() && !log_caught.empty() && hour_from >= 0 && hour_to >= 0 &&
               hour_from != hour_to;
    }
    // The bedside conversation on top of it (above): the waking, the two further rows and
    // their journal lines, all written.
    bool has_bedside() const {
        return has_expose() && !holder_wake.empty() && !holder_wake_reply.empty() && !holder_denounce.empty() &&
               !holder_denounce_reply.empty() && !holder_blackmail.empty() && !holder_blackmail_yes.empty() &&
               !holder_blackmail_no.empty() && !holder_open_denounced.empty() && !log_denounced.empty() &&
               !log_completed_denounced.empty() && !log_completed_exposed_paid.empty() && blackmail_groschen > 0;
    }
    // The holder turning on Henry in the street (above): his line, Henry's answer and the
    // journal's, all written, on an exposure.
    bool has_follow() const {
        return has_expose() && !holder_followed.empty() && !holder_followed_reply.empty() && !log_followed.empty();
    }
    bool any_follow() const {
        return !holder_followed.empty() || !holder_followed_reply.empty() || !log_followed.empty();
    }
    // Any of the bedside's lines written, for the all-or-none check.
    bool any_bedside() const {
        return !holder_wake.empty() || !holder_wake_reply.empty() || !holder_denounce.empty() ||
               !holder_denounce_reply.empty() || !holder_blackmail.empty() || !holder_blackmail_yes.empty() ||
               !holder_blackmail_no.empty() || !holder_open_denounced.empty() || !log_denounced.empty() ||
               !log_completed_denounced.empty() || !log_completed_exposed_paid.empty() || blackmail_groschen > 0;
    }
    // A retrieve whose thing is the Document an earlier quest put in the holder's hands
    // (item_of): nothing is seeded on him, and the Lua names the class from that quest.
    bool takes_letter_of() const { return is_retrieve() && !item_of.empty(); }
    // A dictated letter whose writing the journal notes (log_written): the token's moment.
    bool has_written_log() const { return has_dictation() && !log_written.empty(); }
    // A retrieve whose holder cannot be moved (above): the menu's rows are refusals.
    bool is_unyielding() const { return is_retrieve() && has_holder() && unyielding; }
    // A retrieve whose thing lies in a chest the Lua spawns rather than in the holder's
    // pocket (chest_pos).
    bool has_chest() const { return is_retrieve() && has_item() && !chest_pos.empty(); }
    // A paper laid in the chest beside the thing (chest_paper).
    bool has_paper() const { return has_chest() && !chest_paper.empty(); }
    // The one who can let Henry in: an unyielding retrieve with the helper's soul, the
    // door row and the journal's lines for the naming written.
    bool has_helper() const {
        return is_unyielding() && !helper_soul.empty() && !helper_door.empty() && !helper_door_yes.empty() &&
               helper_door_price > 0 && !log_named.empty() && !objective_helper.empty();
    }
    // The journal's line for the errand given up, the written one or the plain one.
    std::string abandoned_log() const {
        if (!log_abandoned.empty()) return log_abandoned;
        return "I told " + giver_name + " I would not be doing it after all.";
    }
    // The helper's key row: a chest to open, the row's three lines and a price.
    bool has_key() const {
        return has_helper() && has_chest() && !helper_key.empty() && !helper_key_yes.empty() &&
               !helper_key_no.empty() && helper_key_price > 0;
    }
    // Whether this favour waits on another's ending (a story's later scene).
    bool is_link() const { return !after_quest_id.empty(); }
    // Whether the errand has a thing at all: a find's several, a retrieve's one, a
    // letter. A travel has none, and nothing is seeded, counted or moved for it.
    bool has_item() const { return !item_class.empty(); }
    // Whether there is somebody at the other end: a holder to speak through the game's
    // menu, a receiver to hand a letter to. A find has nobody, and the quest graph,
    // the roles and the Lua all leave that side out.
    bool has_target() const { return !target_soul.empty(); }
    // Whether the target is dealt with through the game's menu: a holder to persuade, a
    // receiver to hand a letter to, a beat's man to talk round, a mind's to look in on. A
    // travel's target is looked at and never spoken to.
    bool has_holder() const { return has_target() && !is_travel(); }
    // Whether arriving stages a fight: a travel whose generator named somebody to start it,
    // or an ambush (whose brawlers are the whole point).
    bool has_brawl() const { return (is_travel() || is_ambush()) && !brawl_guids.empty(); }
    // What the pay option costs Henry, in groschen. From npc_quest_lines when the
    // generator wrote a figure, otherwise 0. 0 means the
    // errand has no pay option: the giver pays money, so buying the thing back would
    // only cut him out, and the menu leaves that choice off.
    int pay_groschen = 0;

    // The ways the thing can be won that this quest has a completed log of its own for,
    // beyond the plain one: the graph gets a completion port and the objective a state
    // per entry, and the Lua picks the port by how the handover went.
    std::vector<std::string> resolutions() const;
};

// The fields of npc_quest_lines a holder dialogue is made of, in the order above. The
// generator writes them under these names; the mod reads them under these names.
inline constexpr const char* kHolderFields[] = {
    "open", "refuse", "persuade", "persuade_yes", "persuade_no", "threaten",
    "threaten_yes", "threaten_no", "pay", "pay_yes", "leave", "leave_reply",
    "broker", "broker_reply", "brokered", "brokered_yes", "nested", "nested_yes",
};

// Every way a retrieve can end that the journal may have its own line for, in the order
// the ports and states are dealt. "stolen" is the one with no menu branch: the player
// brought the thing to the giver without the holder ever handing it over.
inline constexpr const char* kResolutions[] = {
    "persuaded", "threatened", "paid", "stolen", "brokered", "lied", "nested",
    "kept",     // deliver: Henry said he had not carried it and would not
    "quiet",    // travel: Henry went, and would not say what he saw
    "fought",   // beat: the man was knocked down rather than talked, frightened or paid round
    "dead",     // beat, mind: the target died before it was done; unpaid
    // (a leave's and a return's lied and quiet are the travel's; an ambush's lied, quiet and
    // dead are the ask's; a buy's paid, stolen and lied are the retrieve's, its lie the
    // price overstated and pocketed)
    // A deliver with an interrogation at the receiver's (tell_* above): the cover story
    // with the giver named, the cover story believed, a tale seen through, the question
    // dodged on a won check, and the check lost. The truth told is the plain completion.
    "pinned", "covered", "fibbed", "evaded", "caught",
    // A dictated letter (dictate_* above): the receiver's verdict on what Henry wrote -- a
    // curse on a living man refused, a scribe's embroidery seen through. The prayer taken
    // is the plain completion. Both are the letter honestly delivered, so neither is untrue.
    "cursed", "mocked",
    // A retrieve's holder caught where he should not be and the thing given up to keep it
    // quiet (expose above). The thing came back, so it is not untrue. The bedside's two
    // more: the thing and coin besides for Henry's silence (exposed_paid), and Henry
    // walking out with nothing to tell the parish (denounced; the thing never comes back,
    // but Henry tells the giver so, which is no lie).
    "exposed", "exposed_paid", "denounced",
};

// The endings a scene that waits on "truth" does not follow: the lies, the silence, the
// death, and every way of not telling the receiver how the thing was come by. The app's
// how_matches and the Lua's QuestHowMatches name the same list.
inline bool untrue_how(const std::string& how) {
    return how == "lied" || how == "quiet" || how == "kept" || how == "dead" ||
           how == "pinned" || how == "covered" || how == "fibbed" || how == "evaded" || how == "caught";
}

// Reads npc_quest joined to npc_quest_text, keeping only quests whose giver and
// target are both among personas -- a quest between souls the mod does not rename
// has nobody to speak it. A find or a pray has no target, and needs only its giver. A database from before quests existed yields none rather
// than an error. Sorted by quest_id so the output is stable.
std::vector<Quest> read_quests(const std::string& db_path, const std::vector<Persona>& personas);

}  // namespace kcd2
