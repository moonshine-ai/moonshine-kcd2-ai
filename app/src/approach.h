#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

// What the mod saw at the moment the player walked up, and what the app makes of it: a
// sentence of prose for the model. The written greeting does not depend on any of it -- a
// soul has one line for a stranger and one for somebody met before, and only `met` picks
// between them -- so the state's whole use is the prose.
//
// The bands used to live in the emitted Lua, with copies in personadb.h and
// public/lib/look.js that had to be edited in lockstep -- and twice were not, once because
// "filthy" was tested two bands off the game's own value. So the mod now reports the raw
// numbers it read and nothing more, and every judgement about them is made here.
namespace moonshine {

// A reading the mod could not take: the stat is not exposed on this save, or the mod
// predates reporting it. Negative because every stat the game hands over is either 0..1 or a
// health value, so no real reading can collide with it.
inline constexpr double kUnread = -1.0;

// Raw state, no judgement. The player fields are what the game's own greeting logic reads
// off the player's soul; the soul fields describe the character being spoken to.
struct Approach {
    double charisma = kUnread;  // rch, relative, 0..1 -- how well dressed they look
    double blood = kUnread;     // bld, relative, 0..1
    double dirt = kUnread;      // drt, relative, 0..1
    double drink = kUnread;     // dru, relative, 0..1
    double health = kUnread;    // GetState("health"), NOT on the 0..1 scale
    bool armed = false;         // human:IsWeaponDrawn()

    bool asleep = false;    // the speech_sleeping script context
    bool chatting = false;  // the speech_npcToNpcDialogActive script context

    // The world outside, which belongs to neither party. Both come off the same BRIDGE begin
    // line, and both go into the prose.
    double hour = kUnread;  // Calendar.GetWorldHourOfDay(), 0..24 decimal -- 18.5 is half six
    double rain = kUnread;  // EnvironmentModule.GetRainIntensity(), 0 dry to 1 heavy

    // The Speech stat of each party (`spc=` the player's, `nspc=` the soul's, soul:GetStatLevel
    // "spc", 1..30), for the one written row the app weighs itself: Henry prying the letter's
    // matter out of its writer (scripted.h, pry_row), settled the way the game's own
    // DialogUtils.PersuadeByPlayer settles a persuasion -- his stat against theirs, by a factor.
    // -1 when the mod did not say; an unread check is a failed one, and the log says why.
    int speech = -1;
    int npc_speech = -1;

    // Whether this soul has been talked to before. The one field the mod cannot supply --
    // the game keeps no record of who the player has spoken to that Lua can read, and the
    // mod has no memory between launches -- so the app fills it from its own book of
    // acquaintances. Every soul has a second greeting written for this case, because failing
    // to recognise somebody met an hour ago is the kind of lapse a player notices at once.
    bool met = false;

    // How many of the item this soul's quest is about the player is carrying, counted by the
    // mod off the player's inventory as the conversation opens. The one fact about a quest
    // only the game can supply, and the one that decides whether the giver pays up. -1 when
    // the mod did not say -- no quest for this soul, or a mod too old to count.
    int have = -1;
    // The same count for every quest this soul is party to, by quest id, for a soul on more
    // than one (`holding=<id>:<n>,...` on the begin line). Empty from a mod that sends only
    // `have=`, which then stands for the soul's first quest.
    std::vector<std::pair<std::string, int>> holding;
    // Whether the player can pray at a shrine at all (`pray=0|1` on a pray's giver: the
    // game's Basic Theology perk). -1 when the mod did not say -- no pray here, or a mod too
    // old to ask -- and an unread answer withholds nothing.
    int can_pray = -1;
    // What Henry could give a beggar (`alms=<letters>` on a beggar's begin line, read by
    // the mod off his pocket): m for a purse of three groschen, f for food, c for a spare
    // garment. Empty on everyone else, and on a beggar he has nothing for; the menu lays
    // a row for each letter (menu.h, kAlmsRow).
    std::string alms;

    // The anim_activity category the soul's animation resolved to: "working", "eating",
    // "talking" and so on, or empty for an animation that says nothing.
    std::string activity;

    // Where the soul is standing, in the game's world metres (`at=<x>,<y>` on the begin
    // line), both unread when the mod did not say. The app sets it against the places the
    // soul's routine takes them, to keep a remark on "this cellar" for the cellar
    // (personadb.h, away_from_places).
    double x = kUnread;
    double y = kUnread;

    // Which deal the game's mod was built from (`qset=` on the begin and look lines): the
    // fingerprint the emitter writes as Moonshine.questSet, for main.cpp to set against
    // PersonaDb::quest_set(). Empty from a mod too old to send it.
    std::string quest_set;
};

// Which part of the day an hour falls in, in the game's own words. Empty when unread.
//
// The boundaries are the game's: Quests/Final/Barbora/utils/time/ splits the day into
// morning, afternoon, evening and night, and rpg_param.xml ships DawnTime 4.7 and DuskTime
// 21.3 for the two that move. Two gaps in that scheme are ours and are marked where they
// are decided.
std::string_view part_of_day(double hour);

// What the sky is doing, as a phrase that reads on from "It is " both after a day part and on
// its own. Empty ONLY when the reading was not taken.
//
// A dry day is said out loud, and the first version of this did not say it -- on the reasoning
// that announcing the absence of news wastes tokens. That was wrong, and measurably: a card
// that asserts weather beats silence every time. Bartel's says "You mutter about the rain and
// the leaky roof", and at seven in the morning on a dry day he mentioned rain in 8 of 10
// replies, which is exactly what he does in a downpour. Naming the dry day takes that to 2 of
// 10. A positive description also beats a negation -- "it is not raining" left it at 3 of 10,
// because a negated word is still the word.
std::string_view sky(double rain);

// The state turned into prose for the model, which gets all of it:
//
//   It is late afternoon and drizzly. You are busy sawing wood. Someone who looks like a
//   noble, is bloody, is injured, and has a weapon in hand walks up to you.
//
// The world comes first because it is the widest thing true of the scene and the model reads
// it as staging before it reads the two people in it. `clause` is the anim_activity phrase
// for what they were caught doing, and reads on from "You are busy ". Empty when the
// animation was not worth reporting, and then that sentence is left out.
std::string approach_prose(const Approach& approach, std::string_view clause);

}  // namespace moonshine
