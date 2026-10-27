#include "doctest.h"

// For doctest's printing of a string_view on a failed CHECK: MSVC's <string_view> declares
// the operator<< and leaves the stream type to whoever includes <ostream>, which nothing else
// here does.
#include <ostream>

#include "memory.h"
#include "scripted.h"

using namespace moonshine;

namespace {

Quest bartels_axe(QuestRole role = QuestRole::Giver) {
    Quest quest;
    quest.id = "c00183e1da7e";
    quest.kind = "retrieve";
    quest.role = role;
    quest.giver_soul = "kkut_additive_man_47";
    quest.giver_name = "Bartel";
    quest.target_soul = "kkut_additive_man_45";
    quest.target_name = "Julian";
    quest.item_class = "1fc42528-2bef-4dde-bf8a-04febeef41c8";
    quest.item_name = "work axe";
    quest.reward_groschen = 10;
    quest.offer = "Julian has my axe. Ten groschen if you bring it back.";
    return quest;
}

const std::vector<Knowledge> kKnowledge = {
    {"daily_work", "Waiting for work", "open", "I wait by the pillory like the rest."},
    {"julian_thief", "Julian the thief", "friendly", "Julian is a thief. He has had my axe since Candlemas."},
    {"agatha_grave", "Agatha's grave", "confided", "Agatha died of a winter fever."},
    {"his_trade", "his trade", "open", "Digging, mostly."},
};

const std::vector<HelpReply> kHelp = {
    {"Go find Alan around the timber yards. Ludwig took his axe.", "754fc3fa4b94"},
    {"Unless you're buying the next round, I've nothing for you to do.", ""},
};

}  // namespace

TEST_CASE("trust opens with meetings and favours done, and a stranger gets the open topics only") {
    Standing stranger;
    stranger.met = 1;
    CHECK(trust_tier(stranger, {}) == Trust::Open);
    Standing second;
    second.met = 2;
    CHECK(trust_tier(second, {}) == Trust::Friendly);
    QuestFacts done;
    done.done = true;
    CHECK(trust_tier(stranger, done) == Trust::Friendly);
    Standing third;
    third.met = 3;
    CHECK(trust_tier(third, done) == Trust::Confided);
    CHECK(trust_tier(second, done) == Trust::Friendly);
    Standing trusted;
    trusted.met = 1;
    trusted.trust = kConfidedTrust;
    CHECK(trust_tier(trusted, {}) == Trust::Confided);

    CHECK(trust_allows(Trust::Open, "open"));
    CHECK_FALSE(trust_allows(Trust::Open, "friendly"));
    CHECK(trust_allows(Trust::Friendly, "friendly"));
    CHECK_FALSE(trust_allows(Trust::Friendly, "confided"));
    CHECK(trust_allows(Trust::Confided, "confided"));
    // A tag nobody knows opens to nobody, rather than to everybody.
    CHECK_FALSE(trust_allows(Trust::Confided, "secret"));
}

TEST_CASE("the giver's help is the offer while it is offerable, with the accepting row beside it") {
    Standing first;
    first.met = 1;
    const Scripted plan = plan_scripted("I'm a varlet.", kKnowledge, kHelp, bartels_axe(), {}, first);
    CHECK(plan.offer == "Julian has my axe. Ten groschen if you bring it back.");
    CHECK(plan.accept_row == "I'll get your work axe back from Julian.");
    // Not the written refusals: those are for a man with nothing to ask.
    CHECK(plan.help.empty());
    CHECK(plan.about == "I'm a varlet.");
    CHECK_FALSE(plan.about_told);
    // Open topics only, and the word that something was held back.
    REQUIRE(plan.topics.size() == 2);
    CHECK(plan.topics[0].key == "daily_work");
    CHECK(plan.topics[1].key == "his_trade");
    CHECK(plan.withheld);
}

TEST_CASE("a letter's accepting row is to carry it, not to fetch it back") {
    Quest letter = bartels_axe();
    letter.id = "1cc118427a63";
    letter.kind = "deliver";
    letter.giver_name = "Sebastian Keller";
    letter.target_name = "Alice Lane";
    letter.item_name = "letter";
    letter.offer = "I need a letter taken to Alice Lane.";
    Standing first;
    first.met = 1;
    const Scripted plan = plan_scripted("", kKnowledge, kHelp, letter, {}, first);
    CHECK(plan.offer == "I need a letter taken to Alice Lane.");
    CHECK(plan.accept_row == "I'll take your letter to Alice Lane.");
}

TEST_CASE("a find's accepting row names the count, and is offered with some already in hand") {
    Quest find = bartels_axe();
    find.id = "c9c431657008";
    find.kind = "find";
    find.giver_name = "Mangy Margery";
    find.target_soul.clear();
    find.target_name.clear();
    find.item_name = "chamomile";
    find.count = 4;
    find.offer = "I need four chamomile to steep. Get them for me?";
    Standing first;
    first.met = 1;
    QuestFacts two;
    two.have = 2;
    const Scripted plan = plan_scripted("", kKnowledge, kHelp, find, two, first);
    CHECK(plan.offer == find.offer);
    // No number in the mouth: the count is the journal's.
    CHECK(plan.accept_row == "I'll get you the chamomile you want.");
}

TEST_CASE("a travel's accepting row names the one looked at, and its seen flag comes off the dump") {
    Quest travel = bartels_axe();
    travel.id = "77b6f6794885";
    travel.kind = "travel";
    travel.giver_name = "Old Vit";
    travel.target_soul = "kkut_man_141";
    travel.target_name = "Havel";
    travel.item_class.clear();
    travel.item_name.clear();
    travel.offer = "Go and see if Havel is still at the wall. I can't be seen there.";
    Standing first;
    first.met = 1;
    QuestFacts carrying;
    carrying.have = 2;
    const Scripted plan = plan_scripted("", kKnowledge, kHelp, travel, carrying, first);
    CHECK(plan.offer == travel.offer);
    CHECK(plan.accept_row == "I'll go and look in on Havel for you.");

    Standing standing;
    standing.quests["77b6f6794885"] = "accepted";
    standing.flags["77b6f6794885"] = "s";
    QuestFacts seen = standing.facts_of("77b6f6794885");
    CHECK(seen.accepted);
    CHECK(seen.seen);
    CHECK_FALSE(seen.broker);
    standing.flags["77b6f6794885"] = "";
    CHECK_FALSE(standing.facts_of("77b6f6794885").seen);

    // A beat's, a pray's and a mind's letters: k down, x dead, y knelt, one c per visit.
    standing.flags["77b6f6794885"] = "kxycc";
    QuestFacts flags = standing.facts_of("77b6f6794885");
    CHECK(flags.beaten);
    CHECK(flags.dead);
    CHECK(flags.prayed);
    CHECK(flags.checks == 2);
    CHECK_FALSE(flags.seen);

    // A leave's and a return's: l down, h the cry heard, o the grave robbed, u the curse on.
    standing.flags["77b6f6794885"] = "lhou";
    QuestFacts spot = standing.facts_of("77b6f6794885");
    CHECK(spot.left);
    CHECK(spot.heard);
    CHECK(spot.looted);
    CHECK(spot.cursed);
    CHECK_FALSE(spot.prayed);

    // An ambush's: g the false way written down, m the men at the place met.
    standing.flags["77b6f6794885"] = "gm";
    QuestFacts ambush = standing.facts_of("77b6f6794885");
    CHECK(ambush.directed);
    CHECK(ambush.ambushed);
    CHECK_FALSE(ambush.left);
}

TEST_CASE("the thingless kinds' accepting rows say what Henry is taking on") {
    Quest beat = bartels_axe();
    beat.kind = "beat";
    beat.target_name = "Anthony";
    beat.item_class.clear();
    beat.item_name.clear();
    beat.offer = "I want him to say sorry to my face.";
    Standing first;
    first.met = 1;
    CHECK(plan_scripted("", kKnowledge, kHelp, beat, {}, first).accept_row ==
          "I'll go and have a word with Anthony for you, and see he makes it right.");
    Quest pray = beat;
    pray.kind = "pray";
    pray.target_soul.clear();
    pray.target_name.clear();
    pray.spot_text = "the cross by the gate";
    CHECK(plan_scripted("", kKnowledge, kHelp, pray, {}, first).accept_row ==
          "I'll go and say a prayer for you at the cross by the gate.");
    Quest mind = beat;
    mind.kind = "mind";
    mind.target_name = "Reginald";
    CHECK(plan_scripted("", kKnowledge, kHelp, mind, {}, first).accept_row ==
          "I'll look in on Reginald for you, and keep looking in.");
}

TEST_CASE("once agreed, or with the thing in hand, the giver's help goes back to the model") {
    Standing first;
    first.met = 1;
    QuestFacts accepted;
    accepted.accepted = true;
    Scripted plan = plan_scripted("", kKnowledge, kHelp, bartels_axe(), accepted, first);
    CHECK(plan.offer.empty());
    CHECK(plan.accept_row.empty());
    CHECK(plan.help.empty());
    // But the offer stands as the reminder, for the row that asks what it was again.
    CHECK(plan.remind == bartels_axe().offer);
    QuestFacts carrying;
    carrying.have = 1;
    plan = plan_scripted("", kKnowledge, kHelp, bartels_axe(), carrying, first);
    CHECK(plan.offer.empty());
    CHECK(plan.help.empty());
    CHECK(plan.remind.empty());
    // Not before the errand is taken up, and not once it is done or locked.
    CHECK(plan_scripted("", kKnowledge, kHelp, bartels_axe(), {}, first).remind.empty());
    QuestFacts done = accepted;
    done.done = true;
    CHECK(plan_scripted("", kKnowledge, kHelp, bartels_axe(), done, first).remind.empty());
    // The second telling of the offer rides with the accepting row, to be said on it.
    Quest twice = bartels_axe();
    twice.offer_2 = "Julian has it, up at the mill. Bring it back and I'll see you right.";
    CHECK(plan_scripted("", kKnowledge, kHelp, twice, {}, first).accept_reply == twice.offer_2);
    CHECK(plan_scripted("", kKnowledge, kHelp, twice, {}, first).more_row.empty());
    // Written, the answer to his agreeing (the way there) goes before the second telling,
    // and his question under the offer rides with its answer; neither once the errand is taken.
    Quest agreed = twice;
    agreed.agreed = "Good. Julian's at the mill, up the stream a few minutes' walk.";
    agreed.more = "Why has Julian got your axe?";
    agreed.more_reply = "Lent it him for a day. That was before Lent.";
    plan = plan_scripted("", kKnowledge, kHelp, agreed, {}, first);
    CHECK(plan.accept_reply == agreed.agreed);
    CHECK(plan.more_row == agreed.more);
    CHECK(plan.more_reply == agreed.more_reply);
    plan = plan_scripted("", kKnowledge, kHelp, agreed, accepted, first);
    CHECK(plan.accept_reply.empty());
    CHECK(plan.more_row.empty());
    // A quest with no written offer leaves help to the model too, rather than say a refusal;
    // the accepting row is the app's own all the same, so the errand can be taken on a pick.
    Quest unwritten = bartels_axe();
    unwritten.offer.clear();
    plan = plan_scripted("", kKnowledge, kHelp, unwritten, {}, first);
    CHECK(plan.offer.empty());
    CHECK(plan.help.empty());
    CHECK(plan.accept_row == "I'll get your work axe back from Julian.");
}

TEST_CASE("what a giver knows is gated on a favour of theirs done, and nobody else is gated") {
    Standing first;
    first.met = 1;
    // Nobody's giver: open.
    CHECK(plan_scripted("", kKnowledge, kHelp, {}, {}, first).kb_gate == KbGate::Open);
    CHECK(kb_gate_word(KbGate::Open) == "open");
    // The giver with the favour there to be asked puts the question off and asks it.
    CHECK(plan_scripted("", kKnowledge, kHelp, bartels_axe(), {}, first).kb_gate == KbGate::Asking);
    // Taken up, the player is reminded.
    QuestFacts agreed;
    agreed.accepted = true;
    CHECK(plan_scripted("", kKnowledge, kHelp, bartels_axe(), agreed, first).kb_gate == KbGate::Owed);
    // Done, whatever the standing, they answer.
    QuestFacts done;
    done.done = true;
    CHECK(plan_scripted("", kKnowledge, kHelp, bartels_axe(), done, first).kb_gate == KbGate::Open);
    // A chain's later link, not yet theirs to ask: shut, with nothing to ask.
    QuestFacts locked;
    locked.locked = true;
    CHECK(plan_scripted("", kKnowledge, kHelp, bartels_axe(), locked, first).kb_gate == KbGate::Shut);
    // A favour they never have to ask is no favour.
    QuestFacts voided;
    voided.voided = true;
    CHECK(plan_scripted("", kKnowledge, kHelp, bartels_axe(), voided, first).kb_gate == KbGate::Open);
    // The holder of the axe is not gated by Bartel's favour.
    CHECK(plan_scripted("", kKnowledge, kHelp, bartels_axe(QuestRole::Target), {}, first).kb_gate ==
          KbGate::Open);
    // But a holder who gives another favour is gated by that one, and one of theirs done
    // opens the gate whichever it was.
    Quest own = bartels_axe();
    own.id = "754fc3fa4b94";
    CHECK(plan_scripted("", kKnowledge, kHelp, bartels_axe(QuestRole::Target), {}, first, {own}, {{}}).kb_gate ==
          KbGate::Asking);
    CHECK(plan_scripted("", kKnowledge, kHelp, bartels_axe(), agreed, first, {own}, {done}).kb_gate ==
          KbGate::Open);
}

TEST_CASE("with the favour done the written help replies are used, a different one each meeting") {
    QuestFacts done;
    done.done = true;
    Standing first;
    first.met = 1;
    Scripted plan = plan_scripted("", kKnowledge, kHelp, bartels_axe(), done, first);
    CHECK(plan.offer.empty());
    CHECK(plan.help == kHelp[0].text);
    Standing second;
    second.met = 2;
    plan = plan_scripted("", kKnowledge, kHelp, bartels_axe(), done, second);
    CHECK(plan.help == kHelp[1].text);
    // And round again.
    Standing third;
    third.met = 3;
    plan = plan_scripted("", kKnowledge, kHelp, bartels_axe(), done, third);
    CHECK(plan.help == kHelp[0].text);
}

TEST_CASE("a lead is only given while what it leads to is still there to be taken up") {
    Standing first;
    first.met = 1;
    // A soul with no favour of their own: help is a lead to Alan's.
    Scripted plan = plan_scripted("", {}, kHelp, {}, {}, first);
    CHECK(plan.help == kHelp[0].text);
    // Alan's errand accepted: the lead is stale, the next reply stands in.
    first.quests["754fc3fa4b94"] = "accepted";
    plan = plan_scripted("", {}, kHelp, {}, {}, first);
    CHECK(plan.help == kHelp[1].text);
    // With nothing but leads and all of them stale, nothing is said and the model answers.
    plan = plan_scripted("", {}, {kHelp[0]}, {}, {}, first);
    CHECK(plan.help.empty());
}

TEST_CASE("the offer of help is due on the menu for an errand of their own or a lead to one, and on nobody else") {
    Standing first;
    first.met = 1;
    // A giver with the favour there to be taken, written or not.
    CHECK(plan_scripted("", kKnowledge, kHelp, bartels_axe(), {}, first).help_due);
    Quest unwritten = bartels_axe();
    unwritten.offer.clear();
    CHECK(plan_scripted("", kKnowledge, {}, unwritten, {}, first).help_due);
    // A soul with no favour whose help is a lead to Alan's, while Alan's is to be had.
    CHECK(plan_scripted("", {}, kHelp, {}, {}, first).help_due);
    // Alan's taken, the refusal stands in, and the row is not due.
    Standing alans_taken = first;
    alans_taken.quests["754fc3fa4b94"] = "accepted";
    const Scripted refused = plan_scripted("", {}, kHelp, {}, {}, alans_taken);
    CHECK(refused.help == kHelp[1].text);
    CHECK_FALSE(refused.help_due);
    // Nothing written at all; the giver's errand taken up (the reminder is the row then),
    // or done.
    CHECK_FALSE(plan_scripted("", {}, {}, {}, {}, first).help_due);
    QuestFacts accepted;
    accepted.accepted = true;
    CHECK_FALSE(plan_scripted("", kKnowledge, kHelp, bartels_axe(), accepted, first).help_due);
    QuestFacts done;
    done.done = true;
    const Scripted after = plan_scripted("", kKnowledge, {kHelp[1]}, bartels_axe(), done, first);
    CHECK(after.help == kHelp[1].text);
    CHECK_FALSE(after.help_due);
}

TEST_CASE("the self-account is for a first meeting; afterwards the line for being asked again, or the model told it was said") {
    Standing second;
    second.met = 2;
    const Scripted plan = plan_scripted("I'm a varlet.", {}, {}, {}, {}, second);
    CHECK(plan.about.empty());
    CHECK(plan.about_told);
    // With the line for a second asking written, that is what `about` says on a later meeting;
    // on a first meeting the self-account stands, whatever else is written.
    const Scripted again = plan_scripted("I'm a varlet.", {}, {}, {}, {}, second, {}, {}, {},
                                         "I told you. A varlet.");
    CHECK(again.about == "I told you. A varlet.");
    CHECK(again.about_told);
    Standing first;
    first.met = 1;
    CHECK(plan_scripted("I'm a varlet.", {}, {}, {}, {}, first, {}, {}, {}, "I told you. A varlet.").about ==
          "I'm a varlet.");
    // Nothing written, nothing to have told.
    const Scripted none = plan_scripted("", {}, {}, {}, {}, second);
    CHECK(none.about.empty());
    CHECK_FALSE(none.about_told);
    // A save that said nothing counts as a first meeting.
    const Scripted unknown = plan_scripted("I'm a varlet.", {}, {}, {}, {}, {});
    CHECK(unknown.about == "I'm a varlet.");
}

TEST_CASE("topics about the other party or the thing are dropped once the thing has moved") {
    Standing third;
    third.met = 3;
    QuestFacts done;
    done.done = true;
    // Confided, so every tier is open; only the stale one goes.
    const Scripted plan = plan_scripted("", kKnowledge, kHelp, bartels_axe(), done, third);
    REQUIRE(plan.topics.size() == 3);
    CHECK(plan.topics[0].key == "daily_work");
    CHECK(plan.topics[1].key == "agatha_grave");
    CHECK(plan.topics[2].key == "his_trade");
    // Before it moves, what the giver says of the holder is still true (and three meetings
    // with the favour not done is friendly, so the confided one is the one kept back).
    const Scripted before = plan_scripted("", kKnowledge, kHelp, bartels_axe(), {}, third);
    REQUIRE(before.topics.size() == 3);
    CHECK(before.topics[1].key == "julian_thief");
    CHECK(before.withheld);
    // From the holder's side the giver is the other party: "Bartel's axe" would go too.
    const std::vector<Knowledge> holders = {
        {"bartel_axe", "Bartel's axe", "open", "Aye, I've got his work axe."},
        {"havel_peace", "Havel's nature", "open", "Havel's a good sort."},
    };
    QuestFacts handed;
    handed.handed = true;
    const Scripted holder = plan_scripted("", holders, {}, bartels_axe(QuestRole::Target), handed, third);
    REQUIRE(holder.topics.size() == 1);
    CHECK(holder.topics[0].key == "havel_peace");

    // A remark whose answer speaks of the other party or the thing goes the same way, read
    // off the words since it has no key or label to say what it is about; the rest stand,
    // and one with no answer written is never a row.
    const std::vector<Remark> remarks = {
        {"missing", "You look like you're missing something.", "My iron. Julian's got my axe."},
        {"cold", "It must be hard standing here.", "It's what a man does to eat."},
        {"blank", "Well?", ""},
    };
    const Scripted after = plan_scripted("", kKnowledge, kHelp, bartels_axe(), done, third, {}, {}, remarks);
    REQUIRE(after.topics.size() == 3);
    REQUIRE(after.remarks.size() == 1);
    CHECK(after.remarks[0].key == "cold");
    const Scripted still = plan_scripted("", kKnowledge, kHelp, bartels_axe(), {}, third, {}, {}, remarks);
    REQUIRE(still.remarks.size() == 2);
    CHECK(still.remarks[0].key == "missing");
    CHECK_FALSE(still.empty());
}

TEST_CASE("the ask round's words face whoever is speaking, and names keep their capitals") {
    const std::vector<Topic> topics = {
        {"daily_work", "Waiting for work", ""},
        {"his_trade", "his trade", ""},
        {"drahomira", "Drahomira", ""},
    };
    // The soul gives leave to ask and recites nothing: the rows under the reply say what
    // there is (notes.md 8 Oct 2026, "sitting on the bench" said of oneself). The opener
    // varies by soul, and no name or label of a topic is in it.
    CHECK(ask_reply(topics, 0) == "Ask, then.");
    CHECK(ask_reply(topics, 1) == "What is it you want to know?");
    CHECK(ask_reply(topics, 2) == "Ask, then, though I know little but my own story.");
    for (unsigned opener = 0; opener < 8; ++opener) {
        const std::string reply = ask_reply(topics, opener);
        CHECK(reply.find("trade") == std::string::npos);
        CHECK(reply.find("Drahomira") == std::string::npos);
        CHECK(reply.find("work") == std::string::npos);
    }
    CHECK(ask_row(topics[0]) == "Tell me about waiting for work.");
    CHECK(ask_row(topics[1]) == "Tell me about your trade.");
    CHECK(ask_row(topics[2]) == "Tell me about Drahomira.");
    CHECK(ask_row({"the_beggars", "The other beggars", ""}) == "Tell me about the other beggars.");
    CHECK(ask_row({"mikesh", "Mikesh the foreman", ""}) == "Tell me about Mikesh the foreman.");
    CHECK(ask_row({"widow", "her husband Paul", ""}) == "Tell me about your husband Paul.");
    // One topic or three, the leave to ask is the same words.
    CHECK(ask_reply({topics[2]}, 0) == "Ask, then.");
    CHECK(ask_reply({{"clerk", "the clerk's trade", ""}}, 0) == "Ask, then.");
}

TEST_CASE("a topic is about the person its label names, by the circle's people or by name alone") {
    // Joan's circle: neighbours by soul id, the dead and the distant by name.
    const std::vector<Person> people = {
        {"khor_woman_2", "Alice"}, {"khor_man_29", "Havel"}, {name_token("Humphrey"), "Humphrey"},
        {name_token("Father Ernest"), given_name("Father Ernest")},
    };
    CHECK(name_token("Father Ernest") == "name:ernest");
    CHECK(given_name("Old Hedvika") == "Hedvika");
    CHECK(given_name("Alice's") == "Alice");
    CHECK(topic_person("Alice's panic", people) == "khor_woman_2");
    CHECK(topic_person("Havel's condition", people) == "khor_man_29");
    CHECK(topic_person("Humphrey", people) == "name:humphrey");
    CHECK(topic_person("Young Ernest", people) == "name:ernest");
    // About nobody: the work, the place, a phrase.
    CHECK(topic_person("churning butter", people).empty());
    CHECK(topic_person("the wild meadow", people).empty());
    CHECK(topic_person("his trade", people).empty());
    // A name the circle does not list is still a name, gated on its being said.
    CHECK(topic_person("Baldwin's wet ledger", people) == "name:baldwin");
    CHECK(topic_person("Drahomira", people) == "name:drahomira");

    // Whom a line names, as whole words only.
    CHECK(people_named("Alice weeps over the levy; Havel coughs.", people) ==
          std::vector<std::string>{"khor_woman_2", "khor_man_29"});
    CHECK(people_named("I loved my Humphrey.", people) == std::vector<std::string>{"name:humphrey"});
    CHECK(people_named("Alices are two a penny.", people).empty());
    CHECK(people_named("Nothing to tell.", people).empty());

    // plan_scripted marks every topic and lists the unlisted names among the people to
    // read for, so a soul's line that names them opens the row.
    const std::vector<Knowledge> knowledge = {
        {"alices_fears", "Alice's panic", "open", "Alice is making herself sick with worry."},
        {"dairy_work", "churning butter", "open", "I run the master's dairy."},
        {"baldwin_roof", "Baldwin's wet ledger", "open", "Baldwin left it in the rain."},
    };
    Standing standing;
    standing.met = 1;
    const Scripted planned = plan_scripted("", knowledge, {}, Quest{}, {}, standing, {}, {}, {}, {}, people);
    REQUIRE(planned.topics.size() == 3);
    CHECK(planned.topics[0].about == "khor_woman_2");
    CHECK(planned.topics[1].about.empty());
    CHECK(planned.topics[2].about == "name:baldwin");
    REQUIRE(planned.people.size() == people.size() + 1);
    CHECK(planned.people.back().token == "name:baldwin");
    CHECK(planned.people.back().name == "Baldwin");
}

TEST_CASE("an ask intent carries its key and gives it back, and nothing else reads as one") {
    CHECK(ask_intent("daily_work") == "ask:daily_work");
    CHECK(ask_key("ask:daily_work") == "daily_work");
    CHECK(ask_key("ask").empty());
    CHECK(ask_key("ask:").empty());
    CHECK(ask_key("about").empty());
    // A remark's row the same way, under its own prefix, so the two cannot be confused.
    CHECK(say_intent("cold_work") == "say:cold_work");
    CHECK(say_key("say:cold_work") == "cold_work");
    CHECK(say_key("say:").empty());
    CHECK(say_key("ask:cold_work").empty());
    CHECK(ask_key("say:cold_work").empty());
}

TEST_CASE("the standing is read off the record, every quest's stage included") {
    const std::optional<SoulRecord> record = read_state_line(
        "BRIDGE state npc=kkut_additive_man_45 met=2 first=1000 last=90000 rev=0 entries=0"
        " quest=c00183e1da7e accepted=true handed=true done=false"
        " quests=c00183e1da7e:h,754fc3fa4b94:a,6202819aa209:d stat:trust=0.75");
    REQUIRE(record.has_value());
    REQUIRE(record->quests.size() == 3);
    CHECK(record->quests[1].first == "754fc3fa4b94");
    CHECK(record->quests[1].second == "accepted");
    CHECK(record->quests[2].second == "done");
    const Standing standing = standing_of(*record);
    CHECK(standing.met == 2);
    CHECK(standing.quests.at("c00183e1da7e") == "handed");
    CHECK(standing.quests.at("6202819aa209") == "done");
    CHECK_FALSE(standing.offerable("754fc3fa4b94"));
    CHECK(standing.offerable("ffffffffffff"));
    CHECK(standing.trust == doctest::Approx(0.75));

    // A mod from before the field: the soul's own quest is still known from the old fields.
    const std::optional<SoulRecord> older = read_state_line(
        "BRIDGE state npc=kkut_additive_man_47 met=1 first=nil last=nil rev=0 entries=0"
        " quest=c00183e1da7e accepted=true handed=false done=false");
    REQUIRE(older.has_value());
    CHECK(older->quests.empty());
    const Standing was = standing_of(*older);
    CHECK(was.quests.at("c00183e1da7e") == "accepted");
    CHECK(was.trust < 0);
    // A stage letter nobody knows is left out rather than guessed at.
    const std::optional<SoulRecord> odd = read_state_line(
        "BRIDGE state npc=x met=1 first=nil last=nil rev=0 entries=0 quests=a:z,b:d");
    REQUIRE(odd.has_value());
    REQUIRE(odd->quests.size() == 1);
    CHECK(odd->quests[0].first == "b");
}

TEST_CASE("the dispute's flags ride after the stage letter, and the facts of each quest come off the standing") {
    const std::optional<SoulRecord> record = read_state_line(
        "BRIDGE state npc=kkut_additive_man_47 met=2 first=1000 last=90000 rev=0 entries=0"
        " quest=c00183e1da7e accepted=true handed=false done=false"
        " quests=c00183e1da7e:ab,754fc3fa4b94:hbp,96759bc0b28f:d");
    REQUIRE(record.has_value());
    REQUIRE(record->quests.size() == 3);
    CHECK(record->quests[0].second == "accepted");
    CHECK(record->quests[1].second == "handed");
    REQUIRE(record->flags.size() == 2);
    CHECK(record->flags[0] == std::pair<std::string, std::string>{"c00183e1da7e", "b"});
    CHECK(record->flags[1] == std::pair<std::string, std::string>{"754fc3fa4b94", "bp"});

    Standing standing = standing_of(*record);
    CHECK(standing.flags.at("c00183e1da7e") == "b");
    QuestFacts axe = standing.facts_of("c00183e1da7e");
    CHECK(axe.accepted);
    CHECK_FALSE(axe.handed);
    CHECK(axe.broker);
    CHECK_FALSE(axe.apologised);
    CHECK(axe.have == -1);
    QuestFacts other = standing.facts_of("754fc3fa4b94", 2);
    CHECK(other.handed);
    CHECK(other.accepted);
    CHECK(other.broker);
    CHECK(other.apologised);
    CHECK(other.have == 2);
    QuestFacts done = standing.facts_of("96759bc0b28f");
    CHECK(done.done);
    CHECK(done.accepted);
    CHECK_FALSE(done.broker);
    CHECK(standing.is_done("96759bc0b28f"));
    CHECK_FALSE(standing.is_done("c00183e1da7e"));
    // The count by id wins over the fallback, which is the begin line's have= for the first.
    standing.holding["c00183e1da7e"] = 1;
    CHECK(standing.facts_of("c00183e1da7e", 0).have == 1);
    CHECK(standing.facts_of("ffffffffffff").accepted == false);
}

TEST_CASE("how a favour ended rides after a slash on the quests field, and reads as the facts' how") {
    const std::optional<SoulRecord> record = read_state_line(
        "BRIDGE state npc=kkut_additive_man_47 met=2 first=1000 last=90000 rev=0 entries=0"
        " quests=c00183e1da7e:dk/fought,754fc3fa4b94:h/persuaded,96759bc0b28f:d,1a5919f0aec1:a/,x:dx");
    REQUIRE(record.has_value());
    REQUIRE(record->quests.size() == 5);
    REQUIRE(record->flags.size() == 2);
    CHECK(record->flags[0] == std::pair<std::string, std::string>{"c00183e1da7e", "k"});
    CHECK(record->flags[1] == std::pair<std::string, std::string>{"x", "x"});
    REQUIRE(record->hows.size() == 2);
    CHECK(record->hows[0] == std::pair<std::string, std::string>{"c00183e1da7e", "fought"});
    CHECK(record->hows[1] == std::pair<std::string, std::string>{"754fc3fa4b94", "persuaded"});

    const Standing standing = standing_of(*record);
    QuestFacts beaten = standing.facts_of("c00183e1da7e");
    CHECK(beaten.done);
    CHECK(beaten.beaten);
    CHECK(beaten.how == "fought");
    QuestFacts told = standing.facts_of("754fc3fa4b94");
    CHECK(told.handed);
    CHECK_FALSE(told.done);
    CHECK(told.how == "persuaded");
    CHECK(standing.facts_of("96759bc0b28f").how.empty());
    CHECK(standing.facts_of("1a5919f0aec1").how.empty());
    CHECK(standing.facts_of("x").dead);
}

TEST_CASE("a state line ending CR LF, as the game writes it on Windows, reads the last how whole") {
    // getline takes the LF and leaves the CR on the last field; the how is compared whole, so
    // "lied\r" read as not-lied and a scene gated on the truth was offered after a lie on
    // the Windows laptop (8 Oct 2026, a quest pass: --road run --how lied).
    const std::optional<SoulRecord> record = read_state_line(
        "BRIDGE state npc=kkut_additive_man_40 met=3 first=8078669 last=8079048 rev=0 entries=11"
        " quest=3fd6ec9c8213 accepted=false handed=false done=false quests=5f8274531a7f:d/lied\r");
    REQUIRE(record.has_value());
    REQUIRE(record->hows.size() == 1);
    CHECK(record->hows[0] == std::pair<std::string, std::string>{"5f8274531a7f", "lied"});
    CHECK(standing_of(*record).facts_of("5f8274531a7f").how == "lied");
    CHECK_FALSE(how_matches(standing_of(*record).facts_of("5f8274531a7f").how, "truth"));
    // And with no how, the flags are not a CR either.
    const std::optional<SoulRecord> flagged = read_state_line(
        "BRIDGE state npc=kkut_additive_man_40 met=1 first=nil last=nil rev=0 entries=0 quests=5f8274531a7f:agm\r");
    REQUIRE(flagged.has_value());
    REQUIRE(flagged->flags.size() == 1);
    CHECK(flagged->flags[0].second == "gm");
}
