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
    // The soul's soul_id, which is what a Skald SoulAsset names in SharedSoulGuids, and the
    // game level they live on (level_for_region of npc.region; "" for the event NPCs, who
    // are nobody's level in particular). Read for the beggars' sake: each is named in his
    // Level's graph so his thanks for a garment can be asked of him (emit.hpp, thanks_class).
    std::string soul_guid;
    std::string level;
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

// Every human soul on a game level (kutnohorsko, trosecko, klaster) with a SoulAsset
// GUID and a display/generated name, so @locate can point an objective ("Find <name>")
// directly at the soul rather than dropping ground items.
struct LocateSoul {
    std::string soul_name;
    std::string full_name;
    std::string soul_guid;
    std::string level;
};

std::vector<LocateSoul> read_locate_souls(const std::string& db_path, const Filter& filter);

// Game-named (`is_named = 1` without `generated_name`) friendly human souls (main-story
// companions, allies, VIPs, and authored KCD2 named characters) whose live entity positions
// are scanned when a guard lookup opens so guards know which named NPCs are currently inside
// their settlement.
std::vector<std::string> read_story_souls(const std::string& db_path, const Filter& filter);

struct Quest;

// A friendly town, village, castle, camp, or monastery guard who offers the
// "Can you help me find someone?" dialogue option and knows everyone inside their
// Burgher (`SettlementLocation`) population center.
struct GuardSoul {
    std::string soul_name;
    std::string display_name;
    std::string gender;
    std::string default_pop_center;  // fallback Burgher location id when live pos is unread
};

std::vector<GuardSoul> read_guard_souls(const std::string& db_path,
                                        const Filter& filter,
                                        const std::vector<Quest>& quests = {});

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
    // The game level the giver lives on (level_for_region of npc.region): the Level node
    // the quest's graph is emitted under, and so the one that is awake when Henry is
    // there and the one the journal files it by. A quest's parties never straddle levels.
    std::string level;
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
    // Whether the quest is switched off in a new game (apply_quest_defaults): the giver
    // does not offer it, hails nobody over for it and carries no marker for it until the
    // player turns it on at the game's console (/enable). Off is the rule; the few stories
    // the mod ships on are named in npcdb.cpp.
    bool off_by_default = false;
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
    // The first excuse cut to Henry's presentation (bugs.txt 22 Nov 2026, In the Stocks'
    // Paul): a fop in a high-charisma outfit without armour (refuse_fop), a man playing at
    // soldier in armour (refuse_soldier), the plain refuse otherwise. The quest graph reads
    // the armour on him (CheckEquipment) and the Lua his charisma (WatchHenryLook; the
    // quest's look global), and the Talk opens on the matching line. Either written alone
    // leaves the other to the plain one.
    std::string holder_refuse_fop;
    std::string holder_refuse_soldier;
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
    // the app's to keep; the mod carries whenever told. All three or none -- but a duel's
    // (below) is "travel with him there immediately": no hour is written, and the clock is
    // left where it stands (fast_travel_at -1 with fast_travel_to set).
    std::string fast_travel_to;
    double fast_travel_at = -1.0;  // hours of the day, decimal; -1 for none written
    bool has_fast_travel() const { return !fast_travel_to.empty() && (fast_travel_at >= 0 || is_duel()); }
    // The hours the carrying is asked in (`fast_travel_hours`, "from-to"; -1 for none
    // written). The app lays the row between them; the mod reads them for the wait
    // (bugs.txt 21 Nov 2026, In the Stocks: the duel agreed before dark, Henry may wait
    // for it with the man -- the app's `fast_travel_wait` -- and the clock is wound on to
    // the window's start before the carry).
    double fast_travel_from = -1.0, fast_travel_to_hour = -1.0;
    // The holder set down for the carry (`fast_travel_holder_to`, "x,y,z", a spot measured
    // on his own road home in sight of the landing; README, "The holder set down at the
    // gate", 15 Nov 2026): at 20:30 he is wherever his evening has him, two hundred metres
    // off as often as not, and Henry lands with nobody to follow and no way to tell which
    // of the street's dark figures is his. FastTravelHolder puts him on the spot as Henry
    // lands and turns Henry's view to him, so the look hint (WatchLook) is up within the
    // watch's tick; from there he goes on with his evening by himself. Optional.
    std::string fast_travel_holder_to;
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
    // The holder's homecoming (`routine_home_at`, "HH:MM"; the scheduler script's spec,
    // build_scheduler.py, read here too): the hour his day links end and he sets off for
    // the witness's. The street night's marker (emit.hpp mark_shown_class) comes on at it;
    // -1 when none is written, and the street watch's start stands in.
    double home_at = -1.0;  // hours of the day, decimal
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
    // The truth after a tale (npc_quest_lines "tell_late", "tell_late_reply", "tell_hold",
    // "tell_hold_reply"; bugs.txt 21 Nov 2026, In the Stocks): a tale the receiver sees
    // through ends on his pushback and a menu of two instead of the handover -- Henry comes
    // clean (`tell_late`, answered with `tell_late_reply`: the truth's how, "truth", and
    // all that follows from it) or stands by his story (`tell_hold`, `tell_hold_reply`: the
    // tale's how, "fibbed"). All four written, or the tales end as before.
    std::string tell_late, tell_late_reply, tell_hold, tell_hold_reply;
    // A deliver whose interrogation settles the errand on the truth (npc_quest_lines
    // "truth_settles" = 1; bugs.txt 21 Nov 2026, In the Stocks: Father Adam, told the
    // truth, asks the next thing of Henry on the spot rather than sending him back to
    // Benesh). The truth row fires the Talk's `truth` port instead of the handover: the
    // leg's plain complete (log_completed), no "Return to <giver>", and the Lua completes
    // the errand there and leads on to the scene that follows at the receiver (leadNext),
    // as a word deliver's hand row does. The other answers still hand over and send Henry
    // back to the giver as before. Nothing without the interrogation, and never beside a
    // plea (the two are the same moment answered two ways).
    bool truth_settles = false;
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
    // Where the holder goes for good once he has given in (bugs.txt 19 Nov 2026: Blind
    // Pride's Benesh, up from his prayers and off to Ellen's camp at Bylany): a point
    // `<x>,<y>`, the scheduler script's spec (build_scheduler.py routine_gone_target: a
    // link added to his NPC hub, to a wrapper by the point, under the graph's gone tag),
    // which the graph turns on at the handover and never off. Empty for every other quest.
    std::string routine_gone_target;
    bool has_gone() const { return has_holder() && !routine_gone_target.empty(); }
    // And whom he goes to live with (20 Nov 2026: Benesh and Ellen, man and wife in
    // Kuttenberg): `wed_soul` / `wed_name` the other soul, `routine_wed_target` his day under
    // the wed tag and `routine_wed_soul` hers (`<BehaviorName>@<wrapper prefab>@<x>,<y>`, as
    // a routine_day spells one), `dress_wed` a clothing preset the Lua puts on her once the
    // wedding is reckoned (WatchWed). The graph's wed tag comes on kWedHours of the calendar
    // after the handover and takes the gone tag off (wedTimer); build_scheduler.py adds the
    // day links and a shared night link to his own sleeping place on both NPC hubs under
    // it. Empty for every other quest.
    std::string wed_soul, wed_name, routine_wed_target, routine_wed_soul, dress_wed;
    bool has_wed() const { return has_gone() && !wed_soul.empty(); }
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
    // The man talked round at his menu instead (the handover port): the objective's
    // Completed line with the marker back on the giver (emit.cpp yielded_log has the plain
    // word when this is unwritten).
    std::string log_yielded;
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
    // A seek whose giver cannot say where the animal went (`spot_text` empty; the slate's
    // place `unsaid`): the neighbours who can, each with a topic that is a clue for this
    // quest (npc_knowledge.clue_quest), in soul_name order. Each gets an optional objective
    // of their own, "Ask <name> about the <animal>", marked on them from the accept until
    // Henry has asked (the app's `@quest <id> clue` when the topic is picked at them, the
    // Lua's token of their class: emit.cpp clue_class) or the animal is found (bugs.txt 5
    // Nov 2026: "the people to talk to should be marked on the map"; 9 Nov 2026: "a
    // separate objective to talk to each of the women"). Empty for any other quest.
    struct Clue {
        std::string soul;  // soul_name
        std::string name;  // their generated name, for the objective
        std::string guid;  // their soul_id
        // A holder whose answer gives nothing (npc_knowledge.clue_blank; Sibyl, who knows
        // nothing of the cat and wants to know nothing, bugs.txt 9 Nov 2026): asking them
        // closes their objective and takes their marker off like any holder's, but does
        // not open the search.
        bool blank = false;
    };
    std::vector<Clue> clues;
    // The place as the neighbours name it (npc_quest_lines "clue_text", written only for
    // such a seek: the slate's place text the giver does not know), for the search
    // objective that opens once the first of them has been asked, "Search the hen yards
    // behind the houses on Wine Street", marked on a thing the Lua lays at the animal's
    // first point (bugs.txt 9 Nov 2026: "a marker for that area should appear on the map
    // as a place to search"). Empty for every other quest.
    std::string clue_text;
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
    // Henry to himself as he puts the paper away after reading it (npc_quest_lines
    // "chest_paper_read"; bugs.txt 18 Nov 2026: "Strange, why does Paul have the watch
    // shifts?"): the graph's UseBookTrigger on the paper's class makes a token of the
    // quest's read class in his pocket as he closes it, the watch finds the token and says
    // the line in his voice, once (WatchChest, SelfTalk). Optional.
    std::string chest_paper_read;
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
    // The threat (bugs.txt 18 Nov 2026: Elsbeth frightened instead of paid): a coerce check
    // at helper_skill, once a visit; won, the door for nothing (`letin` as the door row,
    // the journal's `log_scared`); lost, `helper_threat_no`.
    std::string helper_threat, helper_threat_yes, helper_threat_no;
    std::string helper_leave, helper_leave_reply;
    std::string log_named;
    std::string log_letin;
    std::string log_scared;
    std::string log_keyed;
    std::string objective_helper;
    // A third soul with the argument Henry needs (Blind Pride's Brother Thomas at Sedletz;
    // bugs.txt 18 Nov 2026): npc_quest_lines `counsel_soul`/`counsel_name`, their menu
    // (q_<id>_counsel.xml under a role of their own, offered while the errand is on and the
    // holder has not given in: Henry's `counsel_open`, their `counsel_open_reply`, his
    // `counsel_ask`, their `counsel_ask_yes`, which ends the talk -- the store's `counselled`,
    // letter C, the journal's `log_counselled`, the optional objective `objective_counsel`
    // with the marker on them from the accept until asked -- and `counsel_leave`,
    // `counsel_leave_reply`), and the row the counsel unlocks in the holder's menu
    // (`learned`, Henry's; `learned_yes`, the holder giving in: no check, the persuaded
    // ending), gated on the quest's counsel global, which the Lua keeps from the store.
    std::string counsel_soul, counsel_name, counsel_guid, counsel_gender;
    std::string counsel_open, counsel_open_reply;
    std::string counsel_ask, counsel_ask_yes;
    std::string counsel_leave, counsel_leave_reply;
    std::string log_counselled, objective_counsel;
    std::string holder_learned, holder_learned_yes;
    // The answer in parts (bugs.txt 19 Nov 2026: Brother Thomas's filled the screen):
    // `counsel_ask_yes` is cut at its blank lines into parts, and after each but the last
    // Henry has `counsel_more` ("Go on.") for the next or `counsel_go` ("I've got to go")
    // to leave with `counsel_go_reply`. Either way out the counsel is asked (QuestCounsel)
    // and the notes are his: a Document under counsel_notes_class, named `counsel_notes_name`,
    // whose page is `counsel_notes` (the whole answer when none is written). Hearing every
    // part is a small Scholarship reward besides (the graph's counselTalk.heard).
    std::string counsel_more, counsel_go, counsel_go_reply;
    std::string counsel_notes_name, counsel_notes;
    std::vector<std::string> counsel_parts() const;
    // The disputation (Blind Pride's Benesh, bugs.txt 19 Nov 2026): the holder is argued
    // round in three stages, each his objection and three of Henry's answers -- the sound
    // one (Scripture and the Fathers), a plausible one with nothing under it, and a plain
    // man's. The sound answer wins the stage only if Henry can give chapter and verse: he
    // has asked the counsel (the store's counselled) or his Scholarship is kDebateScholarship
    // or more (QuestDebateSound, the quest's versed global); then he says `sound_cite` and
    // the holder concedes with `sound_reply` (the last stage's concession is `learned_yes`,
    // the persuaded ending). Unversed, the holder's `sound_no` ends the talk; the other two
    // answers end it with their replies. A stage won is kept (the store's debate, the quest's
    // debate global), so the next visit opens on the next objection. npc_quest_lines
    // `debate_open` (Henry's row on the menu) and `debate_<k>_<field>` for k 1..3. With a
    // debate written the menu's plain learned row is not offered: the debate is the road to
    // learned_yes.
    struct DebateStage {
        std::string objection, sound, sound_cite, sound_reply, sound_no;
        std::string plausible, plausible_reply, common, common_reply;
        bool complete(bool last) const {
            return !objection.empty() && !sound.empty() && !sound_cite.empty() && !sound_no.empty() &&
                   (last || !sound_reply.empty()) && !plausible.empty() && !plausible_reply.empty() &&
                   !common.empty() && !common_reply.empty();
        }
    };
    static constexpr int kDebateStages = 3;
    std::string debate_open;
    DebateStage debate[kDebateStages];
    bool debate_any() const {
        if (!debate_open.empty()) return true;
        for (const auto& d : debate) {
            if (!d.objection.empty() || !d.sound.empty() || !d.sound_cite.empty() || !d.sound_reply.empty() ||
                !d.sound_no.empty() || !d.plausible.empty() || !d.plausible_reply.empty() || !d.common.empty() ||
                !d.common_reply.empty()) {
                return true;
            }
        }
        return false;
    }
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
    // A seek (kind "seek", quest-design/stories.md "Sweepings and Strays"): a creature of
    // the giver's (the item, "cat") gone missing and last seen about a place; no target.
    // The Lua spawns the mod's own animal at the first point of `path` ("x,y,z;x,y,z",
    // the waypoints before `spot_pos`) once Henry is near, plays `cue_sound` from it on
    // an interval, moves it on to the next point as he comes close, and at `spot_pos` the
    // animal settles with its litter; Henry standing over them is the find (the travel's
    // `seen` in the store, the token, and `log_found` as the objective's update). Henry's
    // rows at the giver's are the travel's tell and quiet; a seek has no lie (bugs.txt 9
    // Nov 2026), so no `lied` way is dealt. `spot_lift` (metres,
    // "0.3") raises the family above the floor the physics gives at the spot, where the
    // floor's look is higher -- a straw heap's render mesh sits above what Henry stands
    // on, and a lying cat set at the physics was inside it (bugs.txt 9 Nov 2026).
    std::string path;
    std::string log_found;
    std::string spot_lift;
    // Henry to himself (bugs.txt 9 Nov 2026): `spotted` when the animal is first before his
    // eyes ("That's Maggie, is it? Let's see where she's off to"), `discovered` as he finds
    // the litter. Said in his voice as barks of his own (WatchSeek, SelfTalk). Optional.
    std::string spotted, discovered;
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
    // An ask whose yes carries a condition the giver must keep the next morning (In the
    // Stocks' witness: Benesh will stand before the bailiff with Henry beside him; bugs.txt
    // 20 Nov 2026). The yes is the arrangement itself (bugs.txt 22 Nov 2026: Henry is not
    // sent back to tell the giver first): the holder's handover writes the fetch's Updated
    // line (log_told), opens the `arranged` objective ("Accompany Father Adam and Benesh to
    // the bailiff") on the giver in place of a return, and sets the clock; at the fall hour
    // of the next day the mod takes the one asked out of the world (WatchAsk: the fall from
    // the scaffolding). Until then the giver's Talk is the app's greet_arranged ("come back
    // tomorrow"); after it, the `fallen` ending (`log_completed_fallen`), unpaid.
    std::string objective_arranged;
    std::string log_completed_fallen;
    // An ask Henry may turn from to the man behind it (In the Stocks, bugs.txt 21 Nov 2026:
    // Father Adam asks Henry to bring Benesh round to standing witness against Paul Schmidt;
    // Henry may say he will go to Paul himself instead). Two places for it, both ending the
    // errand the way `threatened` (log_completed_threatened), which a duel scene follows
    // (after_how "threatened"): at the offer, the app's `offer_threat` row beside the
    // accepting one (answered with `offer_threat_reply`; the app sends accept and threaten
    // together, so the journal has the errand taken and ended); and on the one asked's menu
    // once his persuasion has failed (the `refused` flag the Lua raises on a lost check),
    // the `turn` row (`turn_reply`), which fires the menu's `turned` port, beside a giving
    // up (`abandon` / `abandon_reply`, the menu's `abandoned` port: the story fails, as the
    // give-up row at the giver's does). With the turn written the menu is the persuasion
    // alone -- no coin, no threat to the one asked -- so that the turn and the giving up
    // are what a failed persuasion leaves, as the design says.
    std::string offer_threat, offer_threat_reply;
    std::string holder_turn, holder_turn_reply;
    std::string holder_abandon, holder_abandon_reply;
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
    // Henry to himself as he comes to the patch (npc_quest_lines "arrived"; bugs.txt
    // 18 Nov 2026, *Forget Me Not*: "This looks like the place"): said once, in his voice
    // and as a bark of his own, within kFindArriveMetres of the spot with the errand on
    // (WatchKeepsake, SelfTalk). Optional.
    std::string arrived;
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
    // A duel (kind "duel"; bugs.txt 20 Nov 2026, In the Stocks: Paul Schmidt threatened
    // rather than begged for): the giver is the man challenged and the target too -- the
    // same soul, which no other kind allows -- and the errand is his challenge, "under the
    // light of the moon" at a yard he names (fast_travel_to), taken up by the app's
    // `fast_travel` in the evening hours (the app's fast_travel_hours) and sprung as Henry
    // lands (FastTravelGo -> DuelSpring): the target is set down facing him
    // (fast_travel_holder_to), the two in `brawl_with` are made behind Henry by the Lua
    // (XGenAIModule.SpawnEntity of their souls; random-encounter bandits, whose level is
    // the game's own scaling), and the ambush token starts the graph's brawl -- the target
    // on the player, armed, to the death (emit_brawl). The watch (WatchDuel) has the two
    // made ones down -> the cowed token (log_cowed: the objective's Updated, the brawl
    // stopped, the target begging); the target dead -> the `dead` ending at once (the
    // objective's Completed on log_completed_dead, no row said; the note item_class, a
    // Document seeded on him from the accept, is on the body); the app's `complete` ->
    // spared (log_completed): all his money to Henry, and he is gone from the game
    // (DuelGone). Henry's own death is the game's.
    std::string log_cowed;
    // The man's word as Henry lands (bugs.txt 22 Nov 2026: "a short cut scene first where
    // Paul calls Henry a trusting idiot before they all attack together"): a forced
    // dialogue of the graph's (emit_taunt_dialogue), his line and Henry's answer where one
    // is written, played off the spring token with the two of them turned to each other,
    // and only at its end are the two made at Henry's back and the brawl begun (the sprung
    // token; DuelTaunted / DuelLoose). Without a taunt the men are made and the brawl begun
    // as he lands.
    std::string duel_taunt;
    std::string duel_taunt_reply;

    bool is_duel() const { return kind == "duel"; }
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
    bool is_seek() const { return kind == "seek"; }
    // A leave or a return: an errand to a place, with a thing to put down there.
    bool is_spot() const { return is_leave() || is_return(); }
    // Whether the quest's item is a Document of the mod's own with a page the generator
    // wrote (`letter`): a deliver's letter, an ambush's directions, a buy's charm.
    bool has_document() const {
        return (is_deliver() || is_ambush() || is_buy() || is_duel()) && !item_class.empty() && !carries_thing();
    }
    // A duel sprung and cowed (above): the two tokens the Lua makes, only with the journal
    // lines they write.
    bool has_duel_spring() const { return is_duel() && !log_ambushed.empty(); }
    bool has_duel_cowed() const { return has_duel_spring() && !log_cowed.empty(); }
    // A duel with the man's word before the blows (above): the taunt's forced dialogue, the
    // role it speaks through, and the sprung token the brawl waits for.
    bool has_duel_taunt() const { return has_duel_spring() && !duel_taunt.empty(); }
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
    // An ask whose word is carried on to a third soul once the giver has been told (above);
    // or a deliver whose receiver asked how the thing was come by (has_interrogation),
    // whose giver, told the truth was told, sends Henry back to the receiver with a plea
    // (bugs.txt 19 Nov 2026, In the Stocks: Benesh begs Father Adam not to accuse Paul
    // Schmidt, and the priest refuses). The same leg -- the `onward` beat at the telling,
    // the journal's Updated line, the last objective with the marker on the one sent to,
    // their own hand row completing the quest -- with onward_soul the receiver's.
    bool carries_on() const { return (is_ask() || has_interrogation()) && !onward_soul.empty(); }
    // An ask whose telling arranges a meeting the one asked never comes to (above).
    bool has_fall() const {
        return is_ask() && !carries_on() && !objective_arranged.empty() && !log_told.empty() &&
               !log_completed_fallen.empty();
    }
    // The plea: a deliver's word carried back to the receiver (above).
    bool pleads() const { return has_interrogation() && !onward_soul.empty(); }
    // The truth settling the errand at the receiver (above): never beside a plea.
    bool settles_on_truth() const { return has_interrogation() && truth_settles && !pleads(); }
    // The truth after a tale (above): all four lines written, on an interrogation with a
    // tale to see through.
    bool has_late_truth() const {
        return has_interrogation() && !tell_late.empty() && !tell_late_reply.empty() && !tell_hold.empty() &&
               !tell_hold_reply.empty() && (!tell_vision.empty() || !tell_tale.empty());
    }
    // An ask Henry may turn from to the man behind it (above): the turn on the one asked's
    // menu, with the journal's line for the ending; and the giving up beside it.
    bool has_turn() const {
        return is_ask() && !log_completed_threatened.empty() && !holder_turn.empty() && !holder_turn_reply.empty();
    }
    bool has_holder_abandon() const { return has_turn() && !holder_abandon.empty() && !holder_abandon_reply.empty(); }
    // The same turn at the offer (the app's row); the journal's line is wanted for it too.
    bool has_offer_threat() const {
        return is_ask() && !log_completed_threatened.empty() && !offer_threat.empty() && !offer_threat_reply.empty();
    }
    // A find whose marker is a keepsake laid at the spot the things grow (above).
    bool has_keepsake() const { return is_find() && !keepsake_class.empty() && !spot_pos.empty(); }
    // A seek with clue-holders (above) and the place named for its search objective, whose
    // find the journal writes (the objective closes on it).
    bool has_search() const {
        return is_seek() && !clues.empty() && !clue_text.empty() && !spot_pos.empty() && !log_found.empty();
    }
    // Whether the Lua makes a token item for the graph to see: a pray's kneeling, a leave's
    // or a return's thing put down, a seek's creature found, an ambush sprung, a helper named by the giver, a
    // dictated letter written. Only when the journal has the line it would write.
    bool has_token() const {
        return (is_pray() && !log_prayed.empty()) || (is_spot() && !log_left.empty()) ||
               (is_seek() && !log_found.empty()) || (is_ambush() && !log_ambushed.empty()) ||
               has_helper() || has_written_log() || has_expose() || has_duel_spring();
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
    // A retrieve's holder with his first excuse cut to Henry's look (above).
    bool has_look() const {
        return is_retrieve() && has_holder() && (!holder_refuse_fop.empty() || !holder_refuse_soldier.empty());
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
    // And Henry's word on reading it.
    bool has_paper_read() const { return has_paper() && !chest_paper_read.empty(); }
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
    // The helper's threat row: its three lines written.
    bool has_helper_threat() const {
        return has_helper() && !helper_threat.empty() && !helper_threat_yes.empty() && !helper_threat_no.empty();
    }
    // The one with the argument: a holder who can be talked round (not unyielding), the
    // counsel's soul found, their ask row answered and the holder's learned row written.
    bool has_counsel() const {
        return has_holder() && !is_unyielding() && !counsel_soul.empty() && !counsel_guid.empty() &&
               !counsel_ask.empty() && !counsel_ask_yes.empty() && !holder_learned.empty() &&
               !holder_learned_yes.empty();
    }
    // The disputation: a holder who can be talked round, Henry's row and every stage
    // written (the loader throws on a stage half written), and the concession it ends on.
    bool has_debate() const {
        if (!has_holder() || is_unyielding() || debate_open.empty() || holder_learned_yes.empty()) return false;
        for (int k = 0; k < kDebateStages; ++k) {
            if (!debate[k].complete(k == kDebateStages - 1)) return false;
        }
        return true;
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
    // travel's target is looked at and never spoken to, and a duel's man (its giver again)
    // is spoken to through the app's Talk alone: no menu of ours on him.
    bool has_holder() const { return has_target() && !is_travel() && !is_duel(); }
    // Whether arriving stages a fight: a travel whose generator named somebody to start it,
    // or an ambush (whose brawlers are the whole point).
    // (A duel's brawl_with are the souls the Lua makes behind Henry, not the graph's
    // brawlers: its graph sets the target alone on him.)
    bool has_brawl() const { return (is_travel() || is_ambush() || is_duel()) && !brawl_guids.empty(); }
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
    "dead",     // beat, mind: the target died before it was done; unpaid. A duel's: the man
                // challenged killed in it, which ends the duel there (the note on his body)
    // (a leave's and a return's lied and quiet are the travel's; an ambush's lied, quiet and
    // dead are the ask's; a buy's paid, stolen and lied are the retrieve's, its lie the
    // price overstated and pocketed; a pleading deliver's threatened is Henry's third answer
    // to the plea -- he will not beg, he will go and threaten the man himself -- which the
    // duel scene follows (after_how "threatened"), and which no scene after the plea's
    // yes or no follows: those take after_how "plain", the plain complete alone)
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
    // An ask whose one asked fell before the morning's meeting (has_fall): the giver tells
    // Henry so, and nothing is paid. Untrue, as a death is: the word never reached the bailiff.
    "fallen",
};

// The endings a scene that waits on "truth" does not follow: the lies, the silence, the
// death, and every way of not telling the receiver how the thing was come by. The app's
// how_matches and the Lua's QuestHowMatches name the same list.
inline bool untrue_how(const std::string& how) {
    return how == "lied" || how == "quiet" || how == "kept" || how == "dead" || how == "fallen" ||
           how == "pinned" || how == "covered" || how == "fibbed" || how == "evaded" || how == "caught";
}

// The game's level (Libs/Tables/level.xml) a soul's npc.region stands for: "Kuttenberg
// region" is kutnohorsko, "Trosky region" trosecko, "Monastery" klaster. Anything else
// (event NPCs, the test rows) is "", nobody's level in particular.
std::string level_for_region(const std::string& region);

// Reads npc_quest joined to npc_quest_text, keeping only quests whose giver and
// target are both among personas -- a quest between souls the mod does not rename
// has nobody to speak it. A find or a pray has no target, and needs only its giver. A database from before quests existed yields none rather
// than an error. Sorted by quest_id so the output is stable.
std::vector<Quest> read_quests(const std::string& db_path, const std::vector<Persona>& personas);

// The quests a new game has on: the stories named in kQuestsOnByDefault, every scene of
// them (a quest is in the same story as the ones it follows and the ones that follow it),
// and no others. `all_on` leaves them all on, for a build that plays every quest (the
// passes). A title that matches no quest read is warned of on stderr: a renamed quest would
// otherwise turn its story off without a word.
void apply_quest_defaults(std::vector<Quest>& quests, bool all_on);
// Which of the quests (by index) belong to the same story as quest `at`: the quests linked
// to it through after_quest_id or through a chain, either way round.
std::vector<std::size_t> story_of(const std::vector<Quest>& quests, std::size_t at);

}  // namespace kcd2
