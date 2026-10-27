// Who a soul is, out of the database the generator built.
//
// The game hands over an id like "kkut_additive_man_47" and nothing else. Everything
// that makes that soul a person to talk to -- the name the mod gave them, what they
// do, where they live, who they know, what is going wrong for them -- is in
// npcs.sqlite, keyed on exactly that id.
//
// Read-only, and opened read-only on purpose: the generator owns this file, the app
// only asks it questions, and a player's install should not be able to corrupt the
// thing every NPC's identity comes from.

#ifndef MOONSHINE_PERSONADB_H
#define MOONSHINE_PERSONADB_H

#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "approach.h"
#include "prompt.h"
#include "quest.h"
#include "scripted.h"

struct sqlite3;

namespace moonshine {

// What the soul was caught in the middle of, resolved from the Mannequin animation the
// mod reported. The names the engine hands over are its own spelling --
// "LumberJackSawVAR", "Bartender_TakeAndTapStein" -- so scripts/generate_kcd2_activities.py
// wrote a clause for each of the 2,016 an NPC can play into anim_activity, and this is the
// lookup side of that table.
struct Doing {
    // Reads on from "You are ": "sawing wood", "carving stone with a chisel". Empty when
    // the animation is not something a passer-by can catch -- fighting, dying, a cutscene
    // -- or when nothing was reported, and then nothing is added to the sheet.
    std::string clause;
    // One of a deliberately small set: working, eating, drinking, resting, sleeping,
    // praying, washing, talking, walking, ailing. The coarse category for anything that
    // wants to sort by it; the detail lives in the clause.
    std::string activity;

    bool caught() const { return !clause.empty(); }
};

struct Kin {
    // "brother", "acquaintance", and so on, as the relationship builder wrote it.
    std::string relationship;
    // The other party's generated name, not their soul id. Someone who was never
    // given a name is left out rather than named by id.
    std::string name;
};

// What is on file about one soul.
struct Soul {
    std::string soul;  // the id the game logged
    std::string name;  // the name the mod gave them, e.g. "Bartel"
    std::string occupation;
    // The trade as it is said aloud. npc.occupation is the game's label, and for a
    // household's hired man that label is "varlet", which nobody in 1403 called anyone;
    // npc_trade (scripts/trade_words.py) holds what he actually does -- miner, groom,
    // miller's man -- and this is that, or the occupation when the label is a trade already.
    std::string trade;
    std::string social_class;
    std::string settlement;
    std::string region;
    std::string gender;
    // voice.xml voice_id, from skald_character. 0 when the database has no such table
    // (the test fixtures) or the soul was never given one. Held for when the actors'
    // own clones may be used; the speaker uses the gender stand-in until then.
    int voice_id = 0;

    // A written background, when one has been generated for this soul. Only some
    // have: there are thousands of named souls and writing a bio costs a model call
    // each, so the rest have to be described from their facts instead.
    std::string bio;

    // The same person in forty-odd words, written to be the live prompt. The bio runs
    // to about four hundred tokens, which is some seven seconds of prefill on the model
    // this ships with, paid on the opening line of every conversation with a soul not
    // met before; the card costs a tenth of that and was distilled for the purpose.
    std::string card;

    // What they say first, before the player has said anything: one line for a stranger and
    // one for somebody they have already met, written for every named soul in the game
    // (npc_default_greeting). Saying either is free, where generating the opening line cost
    // 15.15s in the first real conversation. The player's state -- bloody, armed, at night --
    // goes into the prose the model reads, not into the choice of line: a version of this
    // kept eighteen state-keyed greetings per soul, and the multiplication cost more to
    // write than the precision was worth.
    std::string greeting_first;
    std::string greeting_again;

    std::vector<Kin> kin;

    // Something currently wrong in their life, which is what gives them a reason to
    // talk rather than just an identity to recite.
    std::string trouble;
    std::string wants;

    // The favours they are one end of, if the generator dealt them any, in the slate's order
    // (a chain's links in order). `quest` is the first of them, for the many callers and
    // tests that know a soul by one favour; `quest.exists()` is false for the many souls with
    // nothing to ask and nothing in their pocket that is not theirs. A Soul built by hand
    // with `quest` set and `quests` empty is read as having that one.
    Quest quest;
    std::vector<Quest> quests;

    // What the circle pipeline wrote for the menu's own rows (scripted.h): the answer to
    // "tell me about yourself", what they know and would tell by trust tier, and their
    // answers to "anything I can help with" when there is no favour of their own to offer.
    // Empty for a soul written before the pipeline existed, whose picks the model answers.
    std::string self_account;
    // And the answer to the same from somebody they have already told (`npc_seed_answer`,
    // intent `about`, variant 1).
    std::string about_again;
    std::vector<Knowledge> knowledge;
    std::vector<HelpReply> help_replies;
    // Henry's remarks to them with their answers, from the remarks stage, for the menu.
    std::vector<Remark> remarks;
    // The people of their circles (npc_circle_member, by soul id and given name) and the
    // circles' absentees (npc_absentee, by name): everyone their written lines might name,
    // which is who a topic can be about and whom a line is read for (scripted.h, Person).
    std::vector<Person> people;
    // Where their routine puts them, in world metres (npc_routine.pos_x, pos_y: the bed, the
    // bench, the table, the stall), one entry per distinct spot. Empty for a soul whose
    // routine was not dumped, who is then never held to be away.
    std::vector<std::pair<double, double>> spots;
};

// How far from the nearest of their routine's spots a soul has to be met before they are
// taken to be away from their places: the remarks on "this cellar" are kept back, the rest
// stay. Generous, because a routine spot is the pivot of the hub that holds the props, not
// the prop: Heinz (kkut_additive_man_10) was met at a tavern table 133 m from the pivot of
// his "Kuttenberg crowd" hub, whose benches and tables lie a median 177 m from it (4 Oct
// 2026). At this radius the home and the work pivots between them cover a town for its
// own people, and the open road outside it does not.
inline constexpr double kAwayMetres = 300.0;

// Whether the soul, met where `approach` says, is further than kAwayMetres from every spot
// their routine has for them. False when the mod sent no position or the soul has no spots:
// not knowing is not the same as being away, and keeps every remark.
bool away_from_places(const Soul& soul, const Approach& approach);

// The character sheet for a soul: what the model is told about them, and nothing
// that is true of every soul. Rules about how to speak live in the shared prefix so
// they are cached once rather than per NPC -- see prompt.h.
//
// `approach` is the raw state the mod read, which decides the closing prose and, through
// `met`, which of the two written greetings is carried. `doing` supplies the clause and the
// activity that state is missing, because resolving an animation name needs the database and
// this function does not have it.
// `say_written` false leaves Persona::greeting empty even when a line was written, which puts
// the opening line back on the model (the app's --generated-greetings; the default is to say
// the written line, which is instant where generating one cost 15.15s in the first real
// conversation). The Soul keeps its greetings either way, so a caller can still show what was
// written and say that it is not being used.
//
// `facts` is what the save says has happened on this soul's quest, with the item count off
// the approach laid over it. It decides the quest paragraph of the sheet and what, if
// anything, the session should be listening for; see plan_quest.
// `memory` is the block that says what this soul remembers of the player, already in words
// (memory_block in memory.h); empty on a first meeting, and then no block is added.
// `standing` is the rest of what the save says -- how often they have met, where every favour
// stands, a trust number -- which decides the pre-written answers (Persona::scripted).
Persona persona_for(const Soul& soul, Approach approach = {}, const Doing& doing = {},
                    bool say_written = true, QuestFacts facts = {},
                    const std::string& memory = {}, const Standing& standing = {});

// Somebody the database has never heard of. A save made before the generator ran, or
// a soul the mod renamed but the database no longer lists. Talking vaguely beats
// standing mute in front of someone.
Persona stranger();

// One quest as the console lists it: enough to say what it is and who is at each end.
struct QuestListing {
    std::string id;
    std::string kind;
    std::string title;  // npc_quest_text.name; empty when the words were never written
    std::string giver_soul, giver_name, giver_trade, giver_settlement;
    std::string target_soul, target_name, target_trade;
    std::string item_name;
    // The scene (or chain link) before this one, empty on the first; and how deep in its
    // story this one is, 0 for a first scene or a favour standing alone.
    std::string after_id;
    int depth = 0;
};

// One place a soul's routine puts them, in world metres, with the window of the day it
// holds for in minutes from midnight; start == end for a spot with no window.
struct RoutineSpot {
    int start_min = 0, end_min = 0;
    double x = 0, y = 0, z = 0;
};

class PersonaDb {
public:
    explicit PersonaDb(std::filesystem::path path);
    ~PersonaDb();

    PersonaDb(const PersonaDb&) = delete;
    PersonaDb& operator=(const PersonaDb&) = delete;

    bool ok() const { return db_ != nullptr; }
    // Why it could not be opened, for the status line.
    const std::string& trouble() const { return trouble_; }
    const std::filesystem::path& path() const { return path_; }

    std::optional<Soul> find(std::string_view soul) const;

    // What the session asks for: a sheet for whoever the game named, falling back to
    // a stranger so that a conversation always happens. `anim` is the Mannequin name
    // straight off the log line, resolved here rather than by the caller.
    Persona persona(std::string_view soul, const Approach& approach = {},
                    std::string_view anim = {}, bool say_written = true,
                    const QuestFacts& facts = {},
                    const std::string& memory = {}, const Standing& standing = {}) const;

    // What one animation name means. Empty for "none", for the empty string, for a name
    // the table has never heard of, and for one the check flagged -- a database built
    // before the table existed has to degrade to no clause, not to no conversation.
    Doing doing(std::string_view anim) const;

    // How many souls have a generated name, for the status line. A database that
    // opened but holds nothing is worth telling apart from one that is working.
    int named_souls() const;

    // A fingerprint of the quests this database deals: FNV-1a (64-bit, sixteen hex digits)
    // over the sorted quest ids joined by newlines -- byte for byte what the mod's emitter
    // writes as Moonshine.questSet (quest_set_fingerprint in mod/src/emit.cpp) and
    // the mod puts on every begin and look line as qset=. The two disagreeing means the
    // mod in the game and this app were built from different deals, and an errand accepted
    // here is one the game has never heard of: nothing in the journal, no notification
    // (notes.md, 5 Oct 2026). Empty when the database has no quest table.
    std::string quest_set() const;

    // Every quest dealt, for the console's list (console.h): the parties by name and trade,
    // the giver's settlement, and the links that make a story of several. Grouped so that a
    // story's scenes sit together -- by the first scene's giver's settlement, then that
    // giver's name, then the first scene's title, then scene order -- and in that order
    // numbered, so the same database gives the same numbers every run.
    std::vector<QuestListing> quest_list() const;

    // Where a soul's routine has them through the day, for a teleport to somebody who is
    // not loaded (emit.cpp, Teleport): one entry per distinct spot of the default routine
    // (every spot when there is no default), the window in minutes of the day, the bed and
    // the like with no window (0, 0). Empty for a soul whose routine was not dumped.
    std::vector<RoutineSpot> spots_of(std::string_view soul) const;

private:
    bool written_is_sound(std::string_view soul) const;
    // The word for a soul's work: npc_trade's row, else the label's said form, else the label.
    std::string trade_of(std::string_view soul, const std::string& occupation) const;
    void learn_places();

    std::filesystem::path path_;
    sqlite3* db_ = nullptr;
    std::string trouble_;
    // The settlements that are really settlements. npc.settlement holds a quest or
    // encounter group about as often as a town -- "oblehani Suchdole Enemy Army",
    // "bandits", "friends" -- and saying a man is "a mercenary in bohutova Vlozka Eriks
    // Company" tells the model something false about the world. social_place carries a
    // curated twenty, which is what makes the distinction cheap to draw.
    std::set<std::string, std::less<>> places_;
};

}  // namespace moonshine

#endif  // MOONSHINE_PERSONADB_H
