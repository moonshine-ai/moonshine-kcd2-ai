#include "approach.h"

#include <vector>

namespace moonshine {
namespace {

// The bands every relative stat in the game shares, from Libs/Tables/ai/ScriptParams.xml:
// rpg_<stat>_low, _medium and _high are 0.3, 0.6 and 0.9 for all thirteen stats that have
// them. Named rather than inlined so a reader can see they are the game's numbers, not ours.
constexpr double kLow = 0.3;
constexpr double kMedium = 0.6;
constexpr double kHigh = 0.9;

// speech_greeting_lowHealthThreshold, from the same file. Health is not on the 0..1 scale,
// and copying the game's comparison rather than its scale means we never have to know what
// the maximum is.
constexpr double kHurt = 30.0;

// Drunkenness is the one state with no shipped band. The game asks IsDrunk, a buff, and its
// GetDerivitedDrunkennessInInterval module takes interval_min and interval_max as plain
// float ports rather than reading rpg_dru_* -- which do not exist. So this is our number,
// and it is the medium band on the reasoning that mistaking a sober man for a drunk one is
// the worse error. UNMEASURED: dru is logged on every approach, so a session with a few
// drinks will say whether it is right.
constexpr double kDrunk = kMedium;

// The two hours the game itself moves the day around, from Libs/Tables/rpg_param.xml:
// DawnTime 4.7 and DuskTime 21.3, about 04:42 and 21:18. GetDawnTime and GetDuskTime exist
// only as quest-graph functions, so these numbers are copied rather than asked for.
constexpr double kDawn = 4.7;
constexpr double kDusk = 21.3;

// The fixed edges, from Quests/Final/Barbora/utils/time/: morning ends at 09:30, afternoon
// runs 12:00 to 18:00, evening starts at 18:00.
constexpr double kMorningEnds = 9.5;
constexpr double kAfternoonBegins = 12.0;
constexpr double kEveningBegins = 18.0;

// OURS, and the one boundary with no counterpart in the game: it names no part between 12:00
// and 18:00, and six hours of undifferentiated "afternoon" is too coarse for a line of
// dialogue -- a man at half twelve and a man at half five are in different days. Split at
// three, which is where "this afternoon" stops meaning the part after lunch. Nothing in the
// game reacts to these words, so unlike the dirt bands there is no behaviour to match.
constexpr double kAfternoonLate = 15.0;

// The heavy-rain edge is the game's own weather_heavy_rain condition, which starts at 0.6.
// Anything above zero counts as rain, the way the shipped greetings test it.
constexpr double kHeavyRain = 0.6;

bool read(double value) { return value >= 0.0; }

// Reads on from "Someone who ". Exactly one appearance rung always applies, so it leads and
// the rest are added to it.
//
// The four rungs are the game's own: Quests/.../je_hrac_nobl.xml ("is the player noble")
// splits relative charisma into AtLeastHigh, MediumToHigh, LowToMedium and AtMostLow and
// names the results looks-divine, looks-nice, does-not-look-nice and looks-awful.
std::string_view dressed_like(double charisma) {
    if (charisma >= kHigh) return "looks like a noble";
    if (charisma >= kMedium) return "looks well-to-do";
    if (charisma >= kLow) return "looks ordinary";
    return "looks wretched";
}

// "a", "a and b", "a, b, and c" -- the comma before "and" because the list runs to four.
std::string listed(const std::vector<std::string_view>& parts) {
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) {
            if (parts.size() > 2) out += ",";
            out += i + 1 == parts.size() ? " and " : " ";
        }
        out += parts[i];
    }
    return out;
}

}  // namespace

std::string_view part_of_day(double hour) {
    // Out of range rather than merely unread: an hour is 0..24 by definition, so anything
    // else is a misread and saying nothing beats saying midnight.
    if (!read(hour) || hour > 24.0) return {};

    // Night is tested first because it is the one part that wraps midnight, and testing a
    // wrapped window first is what saves every other test from having to know about it.
    //
    // It also settles an overlap the game leaves open: its evening runs to 22:00 while its
    // night starts at dusk, 21:18, so the 42 minutes between belong to both. They go to
    // night here, on the grounds that dusk is a thing a person can see and 22:00 is not.
    if (hour < kDawn || hour >= kDusk) return "night";
    if (hour < kMorningEnds) return "morning";
    // 09:30 to 12:00 is in none of the game's four parts. Late morning is that gap, and the
    // name is ours.
    if (hour < kAfternoonBegins) return "late morning";
    if (hour < kAfternoonLate) return "early afternoon";
    if (hour < kEveningBegins) return "late afternoon";
    return "evening";
}

std::string_view sky(double rain) {
    if (!read(rain)) return {};
    // Every one of these has to read after "It is late afternoon and" AND after a bare "It is",
    // because the mod can come back with the rain and no clock.
    if (rain <= 0.0) return "dry";
    return rain >= kHeavyRain ? "pouring with rain" : "drizzly";
}

std::string approach_prose(const Approach& a, std::string_view clause) {
    const bool asleep = a.asleep || a.activity == "sleeping";

    // Sentences are joined with a single space, and one with nothing to say is left out rather
    // than leaving a gap behind. Worth a helper rather than three `prose +=` in a row: every
    // sentence here is optional, so each one used to carry its own test for whether anything
    // came before it.
    std::string prose;
    auto say = [&prose](std::string_view sentence) {
        if (sentence.empty()) return;
        if (!prose.empty()) prose += " ";
        prose += sentence;
    };

    // The world first, being the widest thing true of the scene, and staging the model reads
    // before it reads the two people standing in it. Either half can be missing -- a mod too
    // old to report the clock, or a dry day, which says nothing on purpose -- so the sentence
    // is assembled from whichever halves there are.
    const std::string_view when = part_of_day(a.hour);
    const std::string_view weather = sky(a.rain);
    if (!when.empty() || !weather.empty()) {
        std::string world = "It is ";
        world += when;
        if (!when.empty() && !weather.empty()) world += " and ";
        world += weather;
        world += ".";
        say(world);
    }

    if (asleep) {
        say("You are asleep.");
    } else if (!clause.empty()) {
        say("You are busy " + std::string(clause) + ".");
    } else if (a.chatting) {
        say("You are in the middle of talking to somebody else.");
    }

    // Unlike the greeting, the prose gets every state at once: the model can weigh a bloody
    // noble against a bloody beggar, where a written line has to be one or the other.
    std::vector<std::string_view> parts;
    if (read(a.charisma)) parts.push_back(dressed_like(a.charisma));
    if (read(a.blood) && a.blood >= kLow) parts.push_back("is bloody");
    if (read(a.health) && a.health < kHurt) parts.push_back("is injured");
    if (read(a.dirt) && a.dirt > kHigh) parts.push_back("is filthy");
    if (read(a.drink) && a.drink >= kDrunk) parts.push_back("is drunk");
    // What the game reports is a drawn weapon, not armour, and saying "wearing armour" to the
    // model would have it answer a man in a shirt with a knife out as though he were a
    // knight.
    if (a.armed) parts.push_back("has a weapon in hand");

    std::string arrival = "Someone";
    // The greeting is only the first line. Without this the model would spend every turn after
    // it talking to a stranger it has just addressed by name, which is the same lapse the
    // reunion greetings exist to avoid -- one line later.
    if (a.met) arrival += " you have spoken to before";
    if (!parts.empty()) {
        arrival += a.met ? ", who " : " who ";
        arrival += listed(parts);
        if (a.met) arrival += ",";
    }
    arrival += asleep ? " wakes you." : " walks up to you.";
    say(arrival);
    return prose;
}

}  // namespace moonshine
