#include "../src/approach.h"

// Every CHECK here compares a string_view, and doctest only forward-declares std::ostream to
// keep its own compile times down. Clang finds the insertion operator anyway; MSVC needs the
// complete type and fails inside <string_view> rather than here, which is a confusing place to
// start reading.
#include <ostream>
#include <string>

#include "../vendor/doctest/doctest.h"

using namespace moonshine;

// Shorthand for the one sentence about the player, since every band test below wants it.
std::string who(const Approach& approach) { return approach_prose(approach, ""); }

TEST_CASE("a sleeping soul is told so whatever the player looks like") {
    // A sleeping man has not seen the player, so he cannot have an opinion about their
    // clothes; the state is still told, because the model is about to be woken by it.
    CHECK(who({.asleep = true}) == "You are asleep. Someone wakes you.");
    CHECK(who({.charisma = 1.0, .blood = 0.9, .health = 5, .asleep = true}) ==
          "You are asleep. Someone who looks like a noble, is bloody, and is injured wakes you.");
    // Reported either way: by the script context, or by the animation he is playing.
    CHECK(who({.activity = "sleeping"}) == "You are asleep. Someone wakes you.");
}

TEST_CASE("an unread state is not remarked on, and is not read as zero") {
    // A mod too old to report dirt must not have every player counted clean, and more
    // importantly must not have them counted ragged for want of a charisma reading.
    CHECK(who({}) == "Someone walks up to you.");

    // Zero is a reading, and a different one: no charisma at all is a wretched figure.
    CHECK(who({.charisma = 0.0}) == "Someone who looks wretched walks up to you.");
}

TEST_CASE("the bands are the ones the game ships") {
    // rpg_charisma_low, rpg_bld_low and so on are 0.3 for every stat that has them, medium
    // 0.6, high 0.9. A player standing on a line gets the wrong thing said about them
    // otherwise.
    CHECK(who({.charisma = 0.6}) == "Someone who looks well-to-do walks up to you.");
    CHECK(who({.charisma = 0.599}) == "Someone who looks ordinary walks up to you.");
    CHECK(who({.charisma = 0.3}) == "Someone who looks ordinary walks up to you.");
    CHECK(who({.charisma = 0.299}) == "Someone who looks wretched walks up to you.");
    CHECK(who({.blood = 0.3}) == "Someone who is bloody walks up to you.");
    CHECK(who({.blood = 0.299}) == "Someone walks up to you.");
    CHECK(who({.drink = 0.7}) == "Someone who is drunk walks up to you.");
    CHECK(who({.armed = true}) == "Someone who has a weapon in hand walks up to you.");

    // Dirt is the exception, and the game's own: crime_checkPlayerConspicuousness tests
    // strictly above rpg_drt_high, so 0.9 exactly is not yet filthy.
    CHECK(who({.dirt = 0.901}) == "Someone who is filthy walks up to you.");
    CHECK(who({.dirt = 0.9}) == "Someone walks up to you.");

    // Health is not on the 0..1 scale at all: speech_greeting_lowHealthThreshold is 30.
    CHECK(who({.health = 29}) == "Someone who is injured walks up to you.");
    CHECK(who({.health = 30}) == "Someone walks up to you.");
}

TEST_CASE("the measured outfits land where the log said they did") {
    // Read off kcd.log from one save in Kuttenberg, talking to the same soul in four outfits.
    // These are the only real numbers there are, so they are the ones worth pinning: if a band
    // moves and these stop agreeing, what the model is told in-game has changed.
    CHECK(who({.charisma = 1.0, .blood = 0.0, .dirt = 0.101961, .health = 100}) ==
          "Someone who looks like a noble walks up to you.");
    CHECK(who({.charisma = 1.0, .blood = 0.0, .dirt = 0.079085, .health = 100}) ==
          "Someone who looks like a noble walks up to you.");
    CHECK(who({.charisma = 0.473429, .blood = 0.142857, .dirt = 0.0196078, .health = 100}) ==
          "Someone who looks ordinary walks up to you.");
    // Stripped to the mandatory underwear, which is as low as charisma was made to go: 0.311905
    // is just ABOVE rpg_charisma_low, so nakedness does not read as wretched. There is no naked
    // flag reachable from Lua, so this is the whole of what the game will say about it.
    CHECK(who({.charisma = 0.311905, .blood = 0.0, .dirt = 0.0196078, .health = 100}) ==
          "Someone who looks ordinary walks up to you.");

    // Blood at 0.142857 does not reach the low band, so a little blood is not remarked on.
    // That is the game's own boundary, not a preference.
    CHECK(who({.charisma = 0.473429, .blood = 0.142857}) ==
          "Someone who looks ordinary walks up to you.");
}

TEST_CASE("the prose carries every state at once") {
    // This is the whole reason the raw numbers are sent rather than a verdict: the model can
    // be told the player is bloody AND well dressed and work out what a varlet makes of that.
    const Approach lordly_mess{
        .charisma = 1.0, .blood = 0.5, .health = 20, .armed = true};
    CHECK(approach_prose(lordly_mess, "sawing wood") ==
          "You are busy sawing wood. Someone who looks like a noble, is bloody, is injured, "
          "and has a weapon in hand walks up to you.");
}

TEST_CASE("the prose says when they have met before, and reads as a sentence either way") {
    // The written greeting covers the opening line only. Every turn after it is generated, so
    // without this the model addresses somebody it has just recognised as a stranger.
    CHECK(approach_prose({.met = true}, "") ==
          "Someone you have spoken to before walks up to you.");
    CHECK(approach_prose({.charisma = 0.95, .met = true}, "") ==
          "Someone you have spoken to before, who looks like a noble, walks up to you.");
    CHECK(approach_prose({.charisma = 0.95, .blood = 0.5, .met = true}, "sawing wood") ==
          "You are busy sawing wood. Someone you have spoken to before, who looks like a noble "
          "and is bloody, walks up to you.");
    CHECK(approach_prose({.asleep = true, .met = true}, "") ==
          "You are asleep. Someone you have spoken to before wakes you.");
}

TEST_CASE("the appearance rungs are the game's own four") {
    // je_hrac_nobl.xml, "is the player noble", splits relative charisma at the same three
    // bands and names the four results. Exactly one rung always applies, so it leads the
    // list and everything else is added to it.
    CHECK(approach_prose({.charisma = 0.95}, "") ==
          "Someone who looks like a noble walks up to you.");
    CHECK(approach_prose({.charisma = 0.7}, "") ==
          "Someone who looks well-to-do walks up to you.");
    CHECK(approach_prose({.charisma = 0.45}, "") ==
          "Someone who looks ordinary walks up to you.");
    CHECK(approach_prose({.charisma = 0.1}, "") ==
          "Someone who looks wretched walks up to you.");
}

TEST_CASE("the prose reads as a sentence at each length") {
    // One phrase takes no comma, two take none either, three or more take them including
    // before the "and". Got wrong, this is the first thing the model reads about the player.
    CHECK(approach_prose({.charisma = 0.45}, "") ==
          "Someone who looks ordinary walks up to you.");
    CHECK(approach_prose({.charisma = 0.45, .blood = 0.5}, "") ==
          "Someone who looks ordinary and is bloody walks up to you.");
    CHECK(approach_prose({.charisma = 0.45, .blood = 0.5, .drink = 0.7}, "") ==
          "Someone who looks ordinary, is bloody, and is drunk walks up to you.");
}

TEST_CASE("a sleeping soul is told they are asleep, and woken rather than approached") {
    CHECK(approach_prose({.charisma = 1.0, .asleep = true}, "lying in a bed") ==
          "You are asleep. Someone who looks like a noble wakes you.");
    // The clause is not needed for it: the context alone settles it.
    CHECK(approach_prose({.asleep = true}, "") == "You are asleep. Someone wakes you.");
}

TEST_CASE("an interrupted conversation is said only when no clause says it better") {
    CHECK(approach_prose({.chatting = true}, "") ==
          "You are in the middle of talking to somebody else. Someone walks up to you.");
    // The clause is the more specific of the two, so it wins.
    CHECK(approach_prose({.chatting = true}, "gossiping over a fence") ==
          "You are busy gossiping over a fence. Someone walks up to you.");
}

TEST_CASE("a weapon in hand is described as that, and not as armour") {
    // The key is called "armoured" because that is what it reads as, but the only thing the
    // game reports is a drawn weapon. Telling the model a man is wearing armour when he is in
    // a shirt holding a knife would have it answer him as a knight.
    const std::string prose = approach_prose({.armed = true}, "");
    CHECK(prose.find("has a weapon in hand") != std::string::npos);
    CHECK(prose.find("armour") == std::string::npos);
}

TEST_CASE("the parts of the day are the game's own, and cover every hour exactly once") {
    // Boundaries from Quests/Final/Barbora/utils/time/ plus DawnTime 4.7 and DuskTime 21.3 out
    // of rpg_param.xml. Tested on the edges rather than in the middles, because an hour landing
    // in the wrong part is only ever an off-by-one at a boundary.
    CHECK(part_of_day(0.0) == "night");
    CHECK(part_of_day(4.69) == "night");
    CHECK(part_of_day(4.7) == "morning");  // dawn
    CHECK(part_of_day(9.49) == "morning");
    CHECK(part_of_day(9.5) == "late morning");  // the gap the game names nothing
    CHECK(part_of_day(11.99) == "late morning");
    CHECK(part_of_day(12.0) == "early afternoon");
    CHECK(part_of_day(14.99) == "early afternoon");
    CHECK(part_of_day(15.0) == "late afternoon");  // ours, not the game's
    CHECK(part_of_day(17.99) == "late afternoon");
    CHECK(part_of_day(18.0) == "evening");
    CHECK(part_of_day(21.29) == "evening");
    CHECK(part_of_day(21.3) == "night");  // dusk, and the 21:18-22:00 overlap resolved to night
    CHECK(part_of_day(24.0) == "night");

    // Every hour of the day lands somewhere. A gap would read as a mod too old to send the
    // clock, which is a different thing and would be silently wrong rather than visibly.
    for (int tenth = 0; tenth <= 240; ++tenth) {
        const double hour = tenth / 10.0;
        CAPTURE(hour);
        CHECK_FALSE(part_of_day(hour).empty());
    }
}

TEST_CASE("an unread or impossible clock says nothing rather than guessing midnight") {
    CHECK(part_of_day(kUnread).empty());
    CHECK(part_of_day(-3.0).empty());
    // 24 is the top of the range and reads as midnight; past it is a misread.
    CHECK(part_of_day(24.1).empty());
}

TEST_CASE("a dry sky is said out loud, and heavy rain is the game's own 0.6") {
    // Said, not passed over. Leaving it out was the first version and it measurably failed: a
    // card that asserts weather beats silence, and Bartel's says he mutters about the rain, so
    // on a dry morning he mentioned rain in 8 of 10 replies -- the same as in a downpour. Naming
    // the dry day took it to 2 of 10.
    CHECK(sky(0.0) == "dry");
    // The one case that stays silent: nobody looked.
    CHECK(sky(kUnread).empty());
    // The shipped greetings call anything above zero rain, and weather_heavy_rain starts at 0.6.
    CHECK(sky(0.01) == "drizzly");
    CHECK(sky(0.59) == "drizzly");
    CHECK(sky(0.6) == "pouring with rain");
    CHECK(sky(1.0) == "pouring with rain");
}

TEST_CASE("every sky phrase reads after a day part and on its own") {
    // Two shapes, because the mod can come back with the rain and no clock: "It is morning and
    // dry." and "It is dry." A phrase like "the sky is clear" works in the first and gives "It
    // is the sky is clear." in the second, so it cannot be used however well it scores.
    for (const double rain : {0.0, 0.2, 0.9}) {
        CAPTURE(rain);
        CHECK(approach_prose({.hour = 8.0, .rain = rain}, "")
              == "It is morning and " + std::string(sky(rain)) + ". Someone walks up to you.");
        CHECK(approach_prose({.rain = rain}, "")
              == "It is " + std::string(sky(rain)) + ". Someone walks up to you.");
    }
}

TEST_CASE("the world is set before the two people standing in it") {
    CHECK(approach_prose({.hour = 16.0, .rain = 0.2}, "") ==
          "It is late afternoon and drizzly. Someone walks up to you.");
    // Either half alone still reads as a sentence, which is what a mod too old to send the
    // clock and a dry day respectively leave behind.
    CHECK(approach_prose({.hour = 16.0}, "") ==
          "It is late afternoon. Someone walks up to you.");
    CHECK(approach_prose({.rain = 0.9}, "") ==
          "It is pouring with rain. Someone walks up to you.");
    // And with neither, nothing is said about the world at all -- the sentence is absent
    // rather than empty, which is what every persona written before this existed relies on.
    CHECK(approach_prose({}, "") == "Someone walks up to you.");
}

TEST_CASE("the world sentence joins the ones that were already there") {
    // The regression this is here for: the activity and sleep branches used to ASSIGN to the
    // prose rather than append, so adding a sentence in front of them silently deleted it.
    CHECK(approach_prose({.charisma = 1.0, .hour = 6.0, .rain = 0.7}, "sawing wood") ==
          "It is morning and pouring with rain. You are busy sawing wood. "
          "Someone who looks like a noble walks up to you.");
    CHECK(approach_prose({.asleep = true, .hour = 2.0}, "") ==
          "It is night. You are asleep. Someone wakes you.");
    CHECK(approach_prose({.chatting = true, .hour = 19.5, .rain = 0.3}, "") ==
          "It is evening and drizzly. You are in the middle of talking to somebody else. "
          "Someone walks up to you.");
    // Four sentences, with the met clause in the closing one, is the longest this gets.
    CHECK(approach_prose({.charisma = 0.2, .blood = 0.5, .hour = 10.0, .rain = 0.6,
                          .met = true}, "hoeing a row") ==
          "It is late morning and pouring with rain. You are busy hoeing a row. "
          "Someone you have spoken to before, who looks wretched and is bloody, walks up to you.");
}
