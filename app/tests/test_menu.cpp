#include "doctest.h"

#include "fake_game.h"
#include "gamelog.h"
#include "memory.h"
#include "menu.h"
#include "quest.h"
#include "session.h"
#include "stub_llm.h"

using namespace moonshine;

namespace {

const std::string kBartel = "kkut_additive_man_47";
const std::string kWorld = "Bohemia, 1403.";

Quest bartels_axe() {
    Quest quest;
    quest.id = "c00183e1da7e";
    quest.kind = "retrieve";
    quest.role = QuestRole::Giver;
    quest.giver_soul = kBartel;
    quest.giver_name = "Bartel";
    quest.target_soul = "kkut_additive_man_45";
    quest.target_name = "Julian";
    quest.item_class = "1fc42528-2bef-4dde-bf8a-04febeef41c8";
    quest.item_name = "work axe";
    quest.reward_groschen = 50;
    quest.skill = "persuade";
    quest.difficulty = "medium";
    return quest;
}

// A conversation with the menu on, the game faked and the model scripted. Every row is the
// app's own, so the model is called for the prefill of the sheet, for the replies nothing
// is written for, and for the judge -- never for rows.
struct Fixture {
    FakeGame game;
    StubLlm llm;
    TurnWriter writer{game.paths()};
    Options options;
    Prefiller prefiller{llm, options};
    Session session{llm, writer, prefiller, options};
    std::vector<std::string> statuses;
    std::vector<MenuOption> offered;
    std::vector<std::pair<std::string, std::string>> recorded;
    // What the voice was asked to make ahead, text and actor; and how often it was told
    // the look had moved on.
    std::vector<std::pair<std::string, std::string>> prepared;
    int dropped = 0;

    explicit Fixture(bool with_quest = false, std::string greeting = "What do you want?") {
        session.set_world(kWorld);
        session.set_menu(true);
        session.set_stream_interval(std::chrono::hours(1));
        // The look-ahead at once, not after the look has lasted; the dwell has a case of
        // its own.
        session.set_look_dwell(std::chrono::milliseconds(0));
        session.set_persona_source(
            [with_quest, greeting](std::string_view, const Approach& approach, std::string_view,
                                   const SoulRecord&) -> std::optional<Persona> {
                Persona persona{"Bartel", "You are Bartel, a varlet in Kuttenberg.", greeting};
                if (with_quest) {
                    // A favour with no text written for it: the offer is the model's, and
                    // the accepting row the app's, as persona_for would have it.
                    persona.quest_of = bartels_axe();
                    persona.quest = plan_quest(persona.quest_of, {}, approach);
                    Standing standing;
                    standing.met = 1;
                    persona.scripted = plan_scripted("", {}, {}, persona.quest_of, {}, standing);
                }
                return persona;
            });
        Listener listener;
        listener.on_status = [this](std::string_view text) { statuses.emplace_back(text); };
        listener.on_option = [this](const MenuOption& option) { offered.push_back(option); };
        listener.on_quest = [this](std::string_view id, std::string_view command) {
            recorded.emplace_back(std::string(id), std::string(command));
        };
        listener.on_prepare = [this](std::string_view text, std::string_view actor) {
            prepared.emplace_back(std::string(text), std::string(actor));
        };
        listener.on_drop_prepared = [this] { ++dropped; };
        session.set_listener(std::move(listener));
    }

    // The conversation opens on the written greeting, which asks nothing of the model: the
    // rows are the app's. The prefill of the sheet is the poller's, between looks at the
    // log (Session::speculate), and it takes a reply of its own off the stub; done here as
    // the poller would, so nothing is left on the stub's queue for a later call to take by
    // mistake.
    void open(LogTailer& tailer) {
        llm.replies = {""};
        game.begin(kBartel);
        drain(tailer);
        REQUIRE(llm.replies.size() == 1);
        REQUIRE(session.has_speculation());
        session.speculate([] { return false; });
        REQUIRE(llm.replies.empty());
    }

    void drain(LogTailer& tailer) {
        for (const Event& event : tailer.poll()) session.handle(event);
    }

    // The mod's pump: plays every turn on disk and reports each.
    void play_all(LogTailer& tailer) {
        while (game.play()) drain(tailer);
    }

    bool said(std::string_view text) const {
        for (const std::string& line : statuses) {
            if (line.find(text) != std::string::npos) return true;
        }
        return false;
    }

    int times_said(std::string_view text) const {
        int n = 0;
        for (const std::string& line : statuses) {
            if (line.find(text) != std::string::npos) ++n;
        }
        return n;
    }
};

}  // namespace

TEST_CASE("the opening round is the design's three, and a row's intent is one of a closed list") {
    const std::vector<MenuOption> seeds = first_round_rows();
    REQUIRE(seeds.size() == 3);
    CHECK(seeds[0].intent == "help");
    CHECK(seeds[0].text == kFirstRoundSeeds[0]);
    CHECK(seeds[1].intent == "ask");
    CHECK(seeds[2].row == 3);
    CHECK(seeds[2].intent == "about");

    for (std::string_view intent : kIntents) CHECK(known_intent(intent));
    for (std::string_view intent : kDirectorIntents) CHECK(known_intent(intent));
    // A topic or remark row carries its key in the intent, and a key with nothing after it
    // is not one.
    CHECK(known_intent("ask:daily_work"));
    CHECK_FALSE(known_intent("ask:"));
    CHECK_FALSE(known_intent("asking"));
    CHECK(known_intent("say:cold_work"));
    CHECK_FALSE(known_intent("say:"));
    CHECK_FALSE(known_intent("threaten"));
}

TEST_CASE("the rows travel as counted directives, all of them on every file") {
    const std::vector<MenuOption> rows = {{1, "help", "Anything I can do?"},
                                          {2, "ask", "Where: the smith?"}};
    CHECK(opt_directive(rows[0]) == "@opt 1 help 18:Anything I can do?");
    CHECK(opt_turn(rows) == "@opt 1 help 18:Anything I can do? @opt 2 ask 17:Where: the smith?");
}

TEST_CASE("a pick is read off the mod's line, words and all") {
    const std::optional<Pick> pick =
        read_pick("<12:04:41> [moonshine_names] BRIDGE picked 2 ask -- Where's the smith?");
    REQUIRE(pick);
    CHECK(pick->row == 2);
    CHECK(pick->intent == "ask");
    CHECK(pick->text == "Where's the smith?");
    // The mod's own bottom row, and an empty row picked before the list was whole.
    CHECK(read_pick("BRIDGE picked 5 leave -- (End dialog)")->intent == "leave");
    CHECK(read_pick("BRIDGE picked 3 none -- ")->text.empty());
    CHECK_FALSE(read_pick("BRIDGE picked"));
    CHECK_FALSE(read_pick("BRIDGE said picked 2"));

    const Event event = classify("BRIDGE picked 2 ask -- Where's the smith?");
    CHECK(event.marker == Marker::Picked);
    CHECK(event.turn == 2);
    CHECK(event.intent == "ask");
    CHECK(event.said == "Where's the smith?");
}

TEST_CASE("the opening round is the seeds at once, and a list nothing is written for stays short") {
    Fixture f;
    LogTailer tailer(f.game.log());
    f.game.launch();
    f.open(tailer);

    // Turn 1 is the greeting; turn 2 carries the seeds, the moment the greeting is
    // written. This soul has no errand to give and no lead to one, so the offer of help is
    // not among them (since 4 Oct 2026: Scripted::help_due); nothing is written for the
    // soul, so there is no third row, and the model is not asked for one: the prefill is
    // its only call.
    REQUIRE(f.offered.size() == 2);
    CHECK(f.offered[0].row == 1);
    CHECK(f.offered[0].intent == "ask");
    CHECK(f.offered[0].text == kFirstRoundSeeds[1]);
    CHECK(f.offered[1].row == 2);
    CHECK(f.offered[1].intent == "about");
    // The list goes whole on one file, counted, so the mod draws it as it stands.
    CHECK(f.game.whole(2) == "@opt 1 ask 18:I have a question. "
                             "@opt 2 about 23:Tell me about yourself. @rows 2\n");
    CHECK(f.session.rows().size() == 2);
    CHECK(f.llm.calls.size() == 1);
    CHECK(f.said("offered 2 rows, all written"));
    CHECK_FALSE(f.game.has_turn(3));

    f.play_all(tailer);
    REQUIRE(f.game.menu().size() == 2);
    CHECK(f.game.menu()[1] == "Tell me about yourself.");

    // The pick comes back as the words, and is answered by the model, since nothing is
    // written for it. The list after is empty -- the seeds are spent or unwritten and there
    // is nothing else -- so the count of none follows the reply, and the mod draws the way
    // out alone rather than waiting on rows that are not coming.
    f.llm.replies = {"Wood. I saw it."};
    f.game.picked(2, "about", "Tell me about yourself.");
    f.drain(tailer);
    CHECK(f.said("picked row 2 (about)"));
    f.play_all(tailer);
    CHECK(f.session.history().size() == 3);
    CHECK(f.session.history()[1].content == "Tell me about yourself.");
    CHECK(f.session.history()[2].content == "Wood. I saw it.");
    CHECK(f.llm.calls.size() == 2);
    CHECK(f.said("no rows to offer, so the list is the way out alone"));
    CHECK(f.offered.size() == 2);
    CHECK(f.session.rows().empty());
    CHECK(f.game.directives().back() == "@rows 0");
    CHECK(f.game.menu().empty());

    // Typed words the same: answered, and followed by nothing.
    f.llm.replies = {"Wood, I said."};
    f.game.typed("Sorry?");
    f.drain(tailer);
    f.play_all(tailer);
    CHECK(f.llm.calls.size() == 3);
    CHECK(f.times_said("no rows to offer") == 2);
    CHECK(f.offered.size() == 2);
}

TEST_CASE("with the menu off, no rows are asked for") {
    Fixture f;
    f.session.set_menu(false);
    LogTailer tailer(f.game.log());
    f.game.launch();
    f.game.begin(kBartel);
    f.drain(tailer);
    // Nothing is asked of the model by the begin itself; the sheet's prefill is left for
    // the poller to do between looks at the log.
    CHECK(f.llm.calls.empty());
    CHECK(f.session.has_speculation());
    CHECK(f.offered.empty());
}

TEST_CASE("a giver's offer in the model's words is followed by the app's accepting row, which takes the errand unjudged") {
    Fixture f(true);
    LogTailer tailer(f.game.log());
    f.game.launch();
    f.open(tailer);
    f.play_all(tailer);
    REQUIRE(f.game.menu().size() == 3);

    // Henry asks; nothing is written for the favour, so Bartel's offer is the model's. The
    // opening round's own help row is the app's, written before any offer: not judged. The
    // list after it is the accepting row, composed by the app from the favour's fields, and
    // nothing else, since nothing else is written.
    f.llm.replies = {"Julian has my axe. Fifty groschen if you get it back."};
    f.game.picked(1, "help", std::string(kFirstRoundSeeds[0]));
    f.drain(tailer);
    f.play_all(tailer);
    CHECK(f.recorded.empty());
    CHECK(f.said("picked as help, so the judge was not asked"));
    CHECK(f.llm.calls.size() == 2);
    REQUIRE(f.game.menu().size() == 1);
    CHECK(f.game.menu()[0] == "I'll get your work axe back from Julian.");
    CHECK(f.offered.back().intent == "accept");
    CHECK(f.said("offered 1 rows, all written"));

    // Picked, the row is the yes without asking, the model speaks the thanks, and the
    // direction went in with the words as it would have on a yes from the judge.
    f.llm.replies = {"Bless you, lad."};
    f.game.picked(1, "accept", "I'll get your work axe back from Julian.");
    f.drain(tailer);
    f.play_all(tailer);
    REQUIRE(f.recorded.size() == 1);
    CHECK(f.recorded[0].second == kQuestAccept);
    CHECK(f.said("picked as accept, so the judge was not asked"));
    bool carried = false;
    for (const std::string& directive : f.game.directives()) {
        if (directive.find("@quest") == 0 && directive.find("accept") != std::string::npos) {
            carried = true;
        }
    }
    CHECK(carried);
    CHECK(f.session.history()[3].content.find("I'll get your work axe back from Julian.") == 0);
    CHECK(f.session.history()[3].content.size() >
          std::string("I'll get your work axe back from Julian.").size());
    // The prefill, the offer, the thanks. A judge would have made a fourth.
    CHECK(f.llm.calls.size() == 3);
    // And taken up, the conversation closes on the thanks, as the game's own quests close
    // on the accepting line: the turn is flagged @end and no list follows it.
    CHECK(f.offered.size() == 4);
    CHECK(f.said("the errand is taken, so the conversation closes and no rows are offered"));
    bool ended = false;
    for (const std::string& directive : f.game.directives()) {
        if (directive == "@end") ended = true;
    }
    CHECK(ended);
    CHECK_FALSE(f.game.has_turn(f.session.played() + 1));
}

TEST_CASE("a pick the app did not write is not put to the judge either, except agreement or help after the offer") {
    // The rows are all the app's now, but the pick comes back from the mod as an intent and
    // words, and the director's reading of it does not depend on who wrote the row.
    Fixture f(true);
    LogTailer tailer(f.game.log());
    f.game.launch();
    f.open(tailer);
    f.play_all(tailer);

    f.llm.replies = {"Fifty groschen, for my axe."};
    f.game.picked(4, "press", "And what is it you want of me?");
    f.drain(tailer);
    f.play_all(tailer);
    CHECK(f.recorded.empty());
    CHECK(f.said("picked as press, so the judge was not asked"));
    CHECK(f.llm.calls.size() == 2);

    // "agree" is agreement to whatever was last said, which may be the errand: judged.
    f.llm.replies = {"YES", "Good lad."};
    f.game.picked(2, "agree", "Aye.");
    f.drain(tailer);
    f.play_all(tailer);
    REQUIRE(f.recorded.size() == 1);
    CHECK(f.recorded[0].second == kQuestAccept);
    CHECK(f.llm.calls.size() == 4);

    // Typed words after a pick are judged as typed words: the intent does not linger.
    // (The question is spent now, so nothing is asked; but the intent is gone too.)
    f.llm.replies = {"Off you go."};
    f.game.typed("Right then.");
    f.drain(tailer);
    CHECK(f.llm.calls.size() == 5);

    // And a help row after the offer, which a model once labelled a yes: judged, and the
    // judge says so.
    Fixture g(true);
    LogTailer tail(g.game.log());
    g.game.launch();
    g.open(tail);
    g.play_all(tail);
    g.llm.replies = {"Julian has my axe. Fifty groschen if you get it back."};
    g.game.picked(1, "help", std::string(kFirstRoundSeeds[0]));
    g.drain(tail);
    g.play_all(tail);
    CHECK(g.recorded.empty());
    g.llm.replies = {"YES", "Bless you, lad."};
    g.game.picked(1, "help", "I'll see what I can do about the axe.");
    g.drain(tail);
    g.play_all(tail);
    CHECK(g.said("picked as help, which may be a yes; asking the judge"));
    REQUIRE(g.recorded.size() == 1);
    CHECK(g.recorded[0].second == kQuestAccept);
    bool directed = false;
    for (const Message& m : g.session.history()) {
        if (m.content.find("They have agreed") != std::string::npos) directed = true;
    }
    CHECK(directed);
}

TEST_CASE("the mod's own bottom row is not a pick, and a pick at nobody is not either") {
    Fixture f;
    LogTailer tailer(f.game.log());
    f.game.launch();
    f.open(tailer);
    f.game.say("<12:04:41> [moonshine_names] BRIDGE picked 5 leave -- (End dialog)");
    f.game.left();
    f.drain(tailer);
    CHECK_FALSE(f.said("picked row 5"));
    CHECK(f.session.rows().empty());
    f.game.picked(1, "help", std::string(kFirstRoundSeeds[0]));
    f.drain(tailer);
    CHECK(f.said("nobody is listening"));
}

namespace {

// Bartel as the circle pipeline wrote him: the offer in his own words, the accepting row
// beside it, an account of himself and the line for being asked again, and four things he
// knows.
Persona written_bartel(const Standing& standing, QuestFacts facts = {}, bool with_again = true,
                       std::string thanks = {}) {
    Persona persona{"Bartel", "You are Bartel, a varlet in Kuttenberg.", "What do you want?"};
    Quest quest = bartels_axe();
    quest.offer = "Julian has my axe. Fifty groschen if you bring it back.";
    quest.thanks = std::move(thanks);
    persona.quest_of = quest;
    persona.quest = plan_quest(quest, facts, {});
    const std::vector<Knowledge> knowledge = {
        {"daily_work", "Waiting for work", "open", "I wait by the pillory like the rest."},
        {"his_trade", "his trade", "open", "Digging, mostly. Carrying when there is nothing to dig."},
        {"the_pillory", "The pillory", "open", "They put a baker in it last month."},
        {"agatha_grave", "Agatha's grave", "confided", "Agatha died of a winter fever."},
    };
    const std::vector<HelpReply> help = {{"No. I have my tools and my peace.", ""}};
    persona.scripted = plan_scripted("Bartel. I dig where I'm told.", knowledge, help, quest,
                                     facts, standing, {}, {}, {},
                                     with_again ? "You asked me that before. I dig. Still." : "");
    return persona;
}

struct WrittenFixture : Fixture {
    explicit WrittenFixture(Standing standing, QuestFacts facts = {}, bool with_again = true,
                            std::string thanks = {})
        : Fixture(false) {
        session.set_persona_source(
            [standing, facts, with_again, thanks](std::string_view, const Approach&, std::string_view,
                                                  const SoulRecord&) -> std::optional<Persona> {
                return written_bartel(standing, facts, with_again, thanks);
            });
    }
};

Standing first_meeting() {
    Standing standing;
    standing.met = 1;
    return standing;
}

}  // namespace

TEST_CASE("the help row is answered with the written offer, and the accepting row is laid ready") {
    WrittenFixture f(first_meeting());
    LogTailer tailer(f.game.log());
    f.game.launch();
    // The opening round is whole: the three seeds, and for the fourth the first of what he
    // knows, as a row of its own.
    f.open(tailer);
    f.play_all(tailer);
    REQUIRE(f.game.menu().size() == 4);
    CHECK(f.game.menu()[3] == "Tell me about waiting for work.");
    CHECK(f.said("offered 4 rows, all written"));
    CHECK(f.said("with written answers for the offer, about and 3 topics"));
    const std::size_t calls = f.llm.calls.size();

    // The pick is answered from the database, and so is the list after it: the accepting
    // row first, laid by the answer, then the account of himself, then two more of what he
    // knows, turned on from the one the opening round showed. The model is not called.
    f.game.picked(1, "help", std::string(kFirstRoundSeeds[0]));
    f.drain(tailer);
    CHECK(f.said("picked as help, answered with the offer as written"));
    CHECK(f.llm.calls.size() == calls);
    f.play_all(tailer);
    REQUIRE(f.session.history().size() == 3);
    CHECK(f.session.history()[2].content == "Julian has my axe. Fifty groschen if you bring it back.");
    REQUIRE(f.game.menu().size() == 4);
    CHECK(f.game.menu()[0] == "I'll get your work axe back from Julian.");
    CHECK(f.game.menu()[1] == kFirstRoundSeeds[2]);
    CHECK(f.game.menu()[2] == "Tell me about your trade.");
    CHECK(f.game.menu()[3] == "Tell me about the pillory.");
    CHECK(f.offered[4].intent == "accept");

    // Asked again in the same conversation, the offer is not recited: the model, which has
    // it in the history, finds other words. (A help row after the offer is judged first, as
    // ever, since a model once labelled a yes as help; here the words are not one.) The list
    // after it is written again, and the accepting row stays at the head of it until the
    // errand is taken: the judge and the reply are the only calls.
    f.llm.replies = {"NO", "I said: Julian, my axe, fifty groschen."};
    f.game.picked(2, "help", "Anything else I can do?");
    f.drain(tailer);
    f.play_all(tailer);
    CHECK(f.session.history()[4].content == "I said: Julian, my axe, fifty groschen.");
    CHECK(f.llm.calls.size() == calls + 2);
    CHECK(f.llm.replies.empty());
    REQUIRE(f.game.menu().size() == 4);
    CHECK(f.game.menu()[0] == "I'll get your work axe back from Julian.");
    CHECK(f.game.menu()[1] == kFirstRoundSeeds[2]);

    // Picked, the row is the yes, as any accept row is, and the model speaks the thanks.
    // With the errand taken the conversation closes on them: no list follows.
    f.llm.replies = {"Bless you, lad."};
    f.game.picked(1, "accept", "I'll get your work axe back from Julian.");
    f.drain(tailer);
    f.play_all(tailer);
    REQUIRE(f.recorded.size() == 1);
    CHECK(f.recorded[0].second == kQuestAccept);
    CHECK(f.said("picked as accept, so the judge was not asked"));
    CHECK(f.said("the errand is taken, so the conversation closes and no rows are offered"));
    CHECK(f.game.directives().back() == "@end");
    CHECK_FALSE(f.game.has_turn(f.session.played() + 1));
}

namespace {

// Bartel with the carrying written (notes.md 25 Oct 2026, Christine and the Kourim gate):
// the two rows for after the yes, between the day's hours.
std::optional<Persona> carrying_bartel(double hour, QuestFacts facts = {}) {
    Persona persona = written_bartel(first_meeting(), facts);
    Quest& quest = persona.quest_of;
    quest.fast_travel_row = "Take me to the gate, then, and I'll wait for him.";
    quest.fast_travel_reply = "Come. I know a short way round the walls.";
    quest.fast_travel_later_row = "I've got things to do first.";
    quest.fast_travel_later_reply = "Then come back when you are free.";
    Approach approach;
    approach.hour = hour;
    persona.quest = plan_quest(quest, facts, approach);
    return persona;
}

}  // namespace

TEST_CASE("the giver's carrying is laid after the yes in place of the close, and picked or declined closes the conversation") {
    for (const bool go : {true, false}) {
        Fixture f(false);
        f.session.set_persona_source([](std::string_view, const Approach&, std::string_view, const SoulRecord&) {
            return carrying_bartel(12.0);
        });
        LogTailer tailer(f.game.log());
        f.game.launch();
        f.open(tailer);
        f.play_all(tailer);
        // Before the yes the rows wait: the opening list has neither.
        for (const std::string& row : f.game.menu()) {
            CHECK(row != "Take me to the gate, then, and I'll wait for him.");
            CHECK(row != "I've got things to do first.");
        }
        f.game.picked(1, "help", std::string(kFirstRoundSeeds[0]));
        f.drain(tailer);
        f.play_all(tailer);
        REQUIRE(f.game.menu().size() == 4);
        CHECK(f.game.menu()[0] == "I'll get your work axe back from Julian.");
        // The yes: owed, and the conversation stays open for the two rows, which are the list.
        f.llm.replies = {"Bless you, lad."};
        f.game.picked(1, "accept", "I'll get your work axe back from Julian.");
        f.drain(tailer);
        f.play_all(tailer);
        REQUIRE(f.recorded.size() == 1);
        CHECK(f.recorded[0].second == kQuestAccept);
        CHECK(f.said("the yes leaves the conversation open for the rows laid after it"));
        CHECK_FALSE(f.said("the errand is taken, so the conversation closes and no rows are offered"));
        for (const std::string& directive : f.game.directives()) CHECK(directive != "@end");
        REQUIRE(f.game.menu().size() >= 2);
        CHECK(f.game.menu()[0] == "Take me to the gate, then, and I'll wait for him.");
        CHECK(f.game.menu()[1] == "I've got things to do first.");
        const std::size_t calls = f.llm.calls.size();
        if (go) {
            // Carried: her written answer, the command owed, the close on it.
            f.game.picked(1, "fast_travel", "Take me to the gate, then, and I'll wait for him.");
            f.drain(tailer);
            f.play_all(tailer);
            CHECK(f.said("picked as fast_travel, answered with the writer's answer to fast_travel as written"));
            REQUIRE(f.recorded.size() == 2);
            CHECK(f.recorded[1].second == kQuestFastTravel);
            bool owed = false;
            for (const std::string& directive : f.game.directives()) {
                if (directive == "@quest c00183e1da7e fast_travel") owed = true;
            }
            CHECK(owed);
            CHECK(f.session.history().back().content == "Come. I know a short way round the walls.");
        } else {
            // Declined: her written answer, nothing owed, the close all the same.
            f.game.picked(2, "fast_travel_later", "I've got things to do first.");
            f.drain(tailer);
            f.play_all(tailer);
            CHECK(f.recorded.size() == 1);
            CHECK(f.session.history().back().content == "Then come back when you are free.");
        }
        CHECK(f.llm.calls.size() == calls);
        CHECK(f.game.directives().back() == "@end");
        CHECK_FALSE(f.game.has_turn(f.session.played() + 1));
    }
}

TEST_CASE("back at the giver empty-handed the carrying is on the list by day and not by night") {
    QuestFacts accepted;
    accepted.accepted = true;
    for (const double hour : {12.0, 21.0}) {
        Fixture f(false);
        f.session.set_persona_source([hour, accepted](std::string_view, const Approach&, std::string_view, const SoulRecord&) {
            return carrying_bartel(hour, accepted);
        });
        LogTailer tailer(f.game.log());
        f.game.launch();
        f.open(tailer);
        f.play_all(tailer);
        bool laid = false, declined = false;
        for (const std::string& row : f.game.menu()) {
            if (row == "Take me to the gate, then, and I'll wait for him.") laid = true;
            if (row == "I've got things to do first.") declined = true;
        }
        CHECK(laid == (hour < 19.0));
        CHECK_FALSE(declined);
        if (!laid) continue;
        // Picked, it closes the conversation with the command owed, and the give-up row
        // was not spent with it: an errand still open.
        int row = 0;
        for (const MenuOption& option : f.offered) {
            if (option.intent == "fast_travel") row = option.row;
        }
        REQUIRE(row > 0);
        f.game.picked(row, "fast_travel", "Take me to the gate, then, and I'll wait for him.");
        f.drain(tailer);
        f.play_all(tailer);
        REQUIRE(f.recorded.size() == 1);
        CHECK(f.recorded[0].second == kQuestFastTravel);
        CHECK(f.game.directives().back() == "@end");
    }
}

TEST_CASE("the accepting row picked straight after the offer takes the errand") {
    WrittenFixture f(first_meeting());
    LogTailer tailer(f.game.log());
    f.game.launch();
    f.open(tailer);
    f.play_all(tailer);
    f.game.picked(1, "help", std::string(kFirstRoundSeeds[0]));
    f.drain(tailer);
    f.play_all(tailer);
    f.llm.replies = {"Bless you, lad."};
    f.game.picked(1, "accept", "I'll get your work axe back from Julian.");
    f.drain(tailer);
    f.play_all(tailer);
    REQUIRE(f.recorded.size() == 1);
    CHECK(f.recorded[0].second == kQuestAccept);
    CHECK(f.said("picked as accept, so the judge was not asked"));
    // The yes and the close ride on the thanks together, in that order, so the mod has the
    // beat queued before it leaves.
    const std::vector<std::string>& directives = f.game.directives();
    REQUIRE(directives.size() >= 2);
    CHECK(directives[directives.size() - 2].find("@quest") == 0);
    CHECK(directives[directives.size() - 2].find(" accept") != std::string::npos);
    CHECK(directives.back() == "@end");

    // The close is this conversation's alone: back at him with the errand taken, the next
    // reply is an ordinary turn with a list after it.
    f.game.left("ended by the app");
    f.drain(tailer);
    f.llm.replies = {""};
    f.game.begin(kBartel);
    f.drain(tailer);
    f.play_all(tailer);
    CHECK_FALSE(f.game.directives().back() == "@end");
    CHECK(f.game.directives().back().find("@rows ") == 0);
    CHECK_FALSE(f.game.menu().empty());
}

TEST_CASE("the chest taken up again, on the written row, closes the conversation like the first yes") {
    // notes.md 14 Oct 2026 (In the Stocks): back at Benesh with Paul's refusal, Henry's yes
    // to the chest names the house-girl and was followed by a list; the giver's answer is
    // the last word, as it is on the first accept.
    Fixture f;
    f.session.set_persona_source(
        [](std::string_view, const Approach&, std::string_view, const SoulRecord&) -> std::optional<Persona> {
            Persona persona{"Bartel", "You are Bartel, a varlet in Kuttenberg.", "What do you want?"};
            Quest quest = bartels_axe();
            quest.unyielding = true;
            quest.helper_name = "Elsbeth";
            quest.greet_rebuffed = "He would not hear of it? Then take it from his chest. Elsbeth keeps the house.";
            quest.greet_owed = "Any luck with Julian?";
            quest.named_row = "Then his chest it is. Where do I find this Elsbeth?";
            quest.named_reply = "Sweeping the step at Welvara's, any afternoon. Mind the master.";
            QuestFacts rebuffed;
            rebuffed.accepted = true;
            rebuffed.rebuffed = true;
            persona.quest_of = quest;
            persona.quest = plan_quest(quest, rebuffed, {});
            persona.scripted = plan_scripted("Bartel. I dig where I'm told.", {}, {}, quest, rebuffed, {}, {}, {}, {}, "");
            return persona;
        });
    LogTailer tailer(f.game.log());
    f.game.launch();
    f.open(tailer);
    f.play_all(tailer);
    REQUIRE_FALSE(f.game.menu().empty());
    CHECK(f.game.menu()[0] == "Then his chest it is. Where do I find this Elsbeth?");
    f.game.picked(1, "accept_again", f.game.menu()[0]);
    f.drain(tailer);
    f.play_all(tailer);
    REQUIRE(f.recorded.size() == 1);
    CHECK(f.recorded[0].second == kQuestNamed);
    CHECK(f.said("the errand is taken, so the conversation closes and no rows are offered"));
    const std::vector<std::string>& directives = f.game.directives();
    REQUIRE(directives.size() >= 2);
    CHECK(directives[directives.size() - 2].find(" named") != std::string::npos);
    CHECK(directives.back() == "@end");
    CHECK_FALSE(f.game.has_turn(f.session.played() + 1));
}

TEST_CASE("a voiced app marks its lines, and says when Henry's and the closing words have been heard") {
    // The mod guesses a line's reading time from its length, and the voice runs past that
    // on a long line: the camera cut Henry off to show the reply, and cut the last words
    // of a conversation off to leave. An app that speaks says so on every line (@voiced),
    // and when the audio of Henry's line or of the closing line has played to the end
    // sends a hook saying which, which is what the mod waits for.
    WrittenFixture f(first_meeting());
    f.session.set_voiced(true);
    LogTailer tailer(f.game.log());
    f.game.launch();
    f.open(tailer);
    f.play_all(tailer);
    // The greeting carries it, and the rows after it do not: a list is not spoken, and the
    // mod takes it off the hold by its first token.
    const std::vector<std::string>& directives = f.game.directives();
    CHECK(std::count(directives.begin(), directives.end(), "@voiced") == 1);
    CHECK((directives.back().rfind("@opt ", 0) == 0 || directives.back().rfind("@rows ", 0) == 0));

    // Henry's line, heard out, is the first hook; some other line finishing is nothing;
    // the same words again are nothing, since the camera has already moved on.
    f.game.picked(1, "help", std::string(kFirstRoundSeeds[0]));
    f.drain(tailer);
    f.play_all(tailer);
    f.session.spoken("Anything else I can do?");
    CHECK(f.game.hook(1).empty());
    f.session.spoken(std::string(kFirstRoundSeeds[0]));
    CHECK(f.game.hook(1) == "Moonshine.Spoken(\"henry\")\n");
    f.session.spoken(std::string(kFirstRoundSeeds[0]));
    CHECK(f.game.hook(2).empty());

    // The closing turn: @end beside @voiced, and the closing words heard to the end -- with
    // whitespace as the voice marker carries it -- are the next hook, once.
    f.llm.replies = {"Bless you, lad."};
    f.game.picked(1, "accept", "I'll get your work axe back from Julian.");
    f.drain(tailer);
    f.play_all(tailer);
    REQUIRE(directives.size() >= 2);
    CHECK(directives[directives.size() - 2] == "@end");
    CHECK(directives.back() == "@voiced");
    f.session.spoken("Bless  you,\nlad.");
    CHECK(f.game.hook(2) == "Moonshine.Spoken(\"npc\")\n");
    f.session.spoken("Bless you, lad.");
    CHECK(f.game.hook(3).empty());
    // Henry's accepting line, heard out after the reply was written, still gets its hook.
    f.session.spoken("I'll get your work axe back from Julian.");
    CHECK(f.game.hook(3) == "Moonshine.Spoken(\"henry\")\n");

    // An app with no voice marks nothing and sends nothing, as before.
    f.game.left("ended by the app");
    f.drain(tailer);
    f.session.set_voiced(false);
    f.llm.replies = {""};
    f.game.begin(kBartel);
    f.drain(tailer);
    f.play_all(tailer);
    f.game.picked(1, "help", std::string(kFirstRoundSeeds[0]));
    f.drain(tailer);
    f.play_all(tailer);
    f.session.spoken(std::string(kFirstRoundSeeds[0]));
    CHECK(f.game.hook(4).empty());
    f.llm.replies = {"Bless you, lad."};
    f.game.picked(1, "accept", "I'll get your work axe back from Julian.");
    f.drain(tailer);
    f.play_all(tailer);
    CHECK(f.game.directives().back() == "@end");
    CHECK(std::find(directives.begin() + static_cast<long>(directives.size()) - 3,
                    directives.end(), "@voiced") == directives.end());
    f.session.spoken("Bless you, lad.");
    CHECK(f.game.hook(4).empty());
}

TEST_CASE("with the second telling written, the accepting row is answered without the model") {
    // The pipeline writes the giver's second telling (offer_2) for the moment Henry agrees;
    // said on the accepting row, the yes owed beside it, and nothing streams in after.
    Fixture f(false);
    f.session.set_persona_source(
        [](std::string_view, const Approach&, std::string_view, const SoulRecord&)
            -> std::optional<Persona> {
            Persona persona = written_bartel(first_meeting());
            persona.quest_of.offer_2 = "Julian, up at the mill. Mind he does not see you coming.";
            persona.scripted = plan_scripted("Bartel. I dig where I'm told.", {}, {}, persona.quest_of,
                                             {}, first_meeting());
            return persona;
        });
    LogTailer tailer(f.game.log());
    f.game.launch();
    f.open(tailer);
    f.play_all(tailer);
    f.game.picked(1, "help", std::string(kFirstRoundSeeds[0]));
    f.drain(tailer);
    f.play_all(tailer);
    REQUIRE(f.game.menu()[0] == "I'll get your work axe back from Julian.");
    const std::size_t calls = f.llm.calls.size();
    f.game.picked(1, "accept", "I'll get your work axe back from Julian.");
    f.drain(tailer);
    CHECK(f.said("picked as accept, answered with the second telling of the offer as written"));
    CHECK(f.llm.calls.size() == calls);
    REQUIRE(f.recorded.size() == 1);
    CHECK(f.recorded[0].second == kQuestAccept);
    REQUIRE(f.session.history().size() == 5);
    CHECK(f.session.history()[4].content == "Julian, up at the mill. Mind he does not see you coming.");
    // The history carries the direction with the yes, as the judge's road would.
    CHECK(f.session.history()[3].content.find("I'll get your work axe back from Julian.") == 0);
    CHECK(f.session.history()[3].content.size() > std::string("I'll get your work axe back from Julian.").size());
    f.play_all(tailer);
    // The second telling closes the conversation, as the thanks would.
    CHECK(f.game.directives().back() == "@end");
    CHECK_FALSE(f.game.has_turn(f.session.played() + 1));
}

TEST_CASE("Henry's question under the offer is answered as written and leaves the offer open") {
    // The pipeline writes `more` (Henry's one question about the errand) and `more_reply`,
    // and `agreed` for the moment he says yes (notes.md 5 Oct 2026). The question is laid
    // under the accepting row; picked, it is answered without the model and the accepting
    // row is there again; picked, that is answered with `agreed`, not the second telling.
    Fixture f(false);
    f.session.set_persona_source(
        [](std::string_view, const Approach&, std::string_view, const SoulRecord&)
            -> std::optional<Persona> {
            Persona persona = written_bartel(first_meeting());
            persona.quest_of.offer_2 = "Julian, up at the mill. Mind he does not see you coming.";
            persona.quest_of.agreed = "Good lad. Julian's at the mill, a few minutes' walk north along the stream.";
            persona.quest_of.more = "Why did Julian keep your axe?";
            persona.quest_of.more_reply = "He says I owe him for a day's work I never got.";
            persona.scripted = plan_scripted("Bartel. I dig where I'm told.", {}, {}, persona.quest_of,
                                             {}, first_meeting());
            return persona;
        });
    LogTailer tailer(f.game.log());
    f.game.launch();
    f.open(tailer);
    f.play_all(tailer);
    f.game.picked(1, "help", std::string(kFirstRoundSeeds[0]));
    f.drain(tailer);
    f.play_all(tailer);
    REQUIRE(f.game.menu().size() >= 2);
    CHECK(f.game.menu()[0] == "I'll get your work axe back from Julian.");
    CHECK(f.game.menu()[1] == "Why did Julian keep your axe?");
    CHECK(f.offered[f.offered.size() - f.game.menu().size() + 1].intent == "more");

    const std::size_t calls = f.llm.calls.size();
    f.game.picked(2, "more", "Why did Julian keep your axe?");
    f.drain(tailer);
    CHECK(f.said("picked as more, answered with the written answer to his question about the errand as written"));
    CHECK(f.llm.calls.size() == calls);
    CHECK(f.recorded.empty());
    CHECK(f.session.history().back().content == "He says I owe him for a day's work I never got.");
    f.play_all(tailer);
    // Answered, the question is gone and the accepting row leads the list again.
    REQUIRE_FALSE(f.game.menu().empty());
    CHECK(f.game.menu()[0] == "I'll get your work axe back from Julian.");
    for (const std::string& row : f.game.menu()) CHECK(row != "Why did Julian keep your axe?");

    f.game.picked(1, "accept", "I'll get your work axe back from Julian.");
    f.drain(tailer);
    CHECK(f.said("picked as accept, answered with the written answer to his agreeing as written"));
    CHECK(f.llm.calls.size() == calls);
    REQUIRE(f.recorded.size() == 1);
    CHECK(f.recorded[0].second == kQuestAccept);
    CHECK(f.session.history().back().content ==
          "Good lad. Julian's at the mill, a few minutes' walk north along the stream.");
}

TEST_CASE("a second question under the offer is laid with the first, and each is answered once") {
    // In the Stocks (notes.md 9 Oct 2026): Benesh asks Henry to say nothing of him or of
    // Paul, and Henry has two things to ask about that -- "Why?" and "You want me to lie to
    // a priest?" -- `more` and `more_2`. Both are laid under the accepting row; each is
    // answered as written, and the other stays on the list until it is asked too.
    Fixture f(false);
    f.session.set_persona_source(
        [](std::string_view, const Approach&, std::string_view, const SoulRecord&)
            -> std::optional<Persona> {
            Persona persona = written_bartel(first_meeting());
            persona.quest_of.agreed = "Good lad. Julian's at the mill.";
            persona.quest_of.more = "Why did Julian keep your axe?";
            persona.quest_of.more_reply = "He says I owe him for a day's work I never got.";
            persona.quest_of.more_2 = "You want me to lie to him?";
            persona.quest_of.more_2_reply = "Not lie. Just don't say who sent you.";
            persona.scripted = plan_scripted("Bartel. I dig where I'm told.", {}, {}, persona.quest_of,
                                             {}, first_meeting());
            return persona;
        });
    LogTailer tailer(f.game.log());
    f.game.launch();
    f.open(tailer);
    f.play_all(tailer);
    f.game.picked(1, "help", std::string(kFirstRoundSeeds[0]));
    f.drain(tailer);
    f.play_all(tailer);
    REQUIRE(f.game.menu().size() >= 3);
    CHECK(f.game.menu()[0] == "I'll get your work axe back from Julian.");
    CHECK(f.game.menu()[1] == "Why did Julian keep your axe?");
    CHECK(f.game.menu()[2] == "You want me to lie to him?");
    CHECK(f.offered[f.offered.size() - f.game.menu().size() + 2].intent == "more_2");

    const std::size_t calls = f.llm.calls.size();
    f.game.picked(3, "more_2", "You want me to lie to him?");
    f.drain(tailer);
    CHECK(f.llm.calls.size() == calls);
    CHECK(f.recorded.empty());
    CHECK(f.session.history().back().content == "Not lie. Just don't say who sent you.");
    f.play_all(tailer);
    // Answered, that question is gone; the accepting row leads and the first question stays.
    REQUIRE(f.game.menu().size() >= 2);
    CHECK(f.game.menu()[0] == "I'll get your work axe back from Julian.");
    CHECK(f.game.menu()[1] == "Why did Julian keep your axe?");
    for (const std::string& row : f.game.menu()) CHECK(row != "You want me to lie to him?");

    f.game.picked(2, "more", "Why did Julian keep your axe?");
    f.drain(tailer);
    CHECK(f.llm.calls.size() == calls);
    CHECK(f.session.history().back().content == "He says I owe him for a day's work I never got.");
    f.play_all(tailer);
    REQUIRE_FALSE(f.game.menu().empty());
    CHECK(f.game.menu()[0] == "I'll get your work axe back from Julian.");
    for (const std::string& row : f.game.menu()) {
        CHECK(row != "Why did Julian keep your axe?");
        CHECK(row != "You want me to lie to him?");
    }
}

TEST_CASE("a quest's own opener stands in for the offer of help, and a two-beat offer is two turns") {
    // Forget Me Not (notes.md 7 Oct 2026): "Is there anything I can help you with?" is the
    // wrong question to a widow who is plainly grieving, so the quest writes Henry's row
    // (`opener`); and her answer is two beats -- why she wants the flowers, then what she
    // asks -- written as two turns, the first flagged @beat so the mod keeps the turn,
    // rather than one paragraph.
    const std::string opener = "Why are you so unhappy?";
    const std::string first = "Julian took my axe the week my wife fell ill, and I have not been able to work since.";
    const std::string second = "Bring it back and there's fifty groschen in it for you.";
    Fixture f(false);
    f.session.set_persona_source(
        [&](std::string_view, const Approach&, std::string_view, const SoulRecord&)
            -> std::optional<Persona> {
            Persona persona = written_bartel(first_meeting());
            persona.quest_of.opener = opener;
            persona.quest_of.offer = first + "\n\n" + second;
            persona.scripted = plan_scripted("Bartel. I dig where I'm told.", {}, {}, persona.quest_of,
                                             {}, first_meeting());
            return persona;
        });
    LogTailer tailer(f.game.log());
    f.game.launch();
    f.open(tailer);
    f.play_all(tailer);
    REQUIRE_FALSE(f.game.menu().empty());
    CHECK(f.game.menu()[0] == opener);
    for (const std::string& row : f.game.menu()) CHECK(row != kFirstRoundSeeds[0]);

    const std::size_t calls = f.llm.calls.size();
    const int at = f.session.played();
    f.game.picked(1, "help", opener);
    f.drain(tailer);
    // Answered from the written offer, no model and no judge: the opener is the help row.
    CHECK(f.llm.calls.size() == calls);
    CHECK(f.session.history().back().content == first + "\n\n" + second);
    // Two turns: the first beat flagged, carrying what the save is told, and the second
    // bare, with the rows after it.
    const std::string beat_one = f.game.whole(at + 1);
    const std::string beat_two = f.game.whole(at + 2);
    CHECK(beat_one.rfind(std::string(kBeatDirective) + " ", 0) == 0);
    CHECK(beat_one.find(first) != std::string::npos);
    CHECK(beat_one.find(second) == std::string::npos);
    CHECK(beat_two.rfind(second, 0) == 0);
    f.play_all(tailer);
    REQUIRE_FALSE(f.game.menu().empty());
    CHECK(f.game.menu()[0] == "I'll get your work axe back from Julian.");
    std::size_t beats = 0;
    for (const std::string& directive : f.game.directives()) {
        if (directive == kBeatDirective) ++beats;
    }
    CHECK(beats == 1);
}

TEST_CASE("back at the giver with the errand taken up, the offer of help gives way to a reminder") {
    // Bartel's axe agreed and not yet brought back: "What can I help you with?" is the wrong
    // question now, so the row asks for the errand again and is answered with the offer, once.
    QuestFacts accepted;
    accepted.accepted = true;
    WrittenFixture f(first_meeting(), accepted);
    LogTailer tailer(f.game.log());
    f.game.launch();
    f.open(tailer);
    f.play_all(tailer);
    REQUIRE(f.game.menu().size() == 4);
    CHECK(f.game.menu()[0] == kRemindRow);
    CHECK(f.offered[0].intent == "remind");
    for (const MenuOption& row : f.offered) CHECK(row.intent != "help");
    CHECK(f.said("offered 4 rows, all written"));
    const std::size_t calls = f.llm.calls.size();
    f.game.picked(1, "remind", std::string(kRemindRow));
    f.drain(tailer);
    CHECK(f.said("picked as remind, answered with the offer again, as a reminder as written"));
    CHECK(f.llm.calls.size() == calls);
    CHECK(f.session.history().back().content == "Julian has my axe. Fifty groschen if you bring it back.");
    CHECK(f.recorded.empty());
    // Reminded, the row is gone from the list after, and the offer of help does not come back.
    f.play_all(tailer);
    for (const MenuOption& row : f.offered) {
        if (row.row > 4) CHECK((row.intent != "remind" && row.intent != "help"));
    }
    for (const std::string& row : f.game.menu()) {
        CHECK(row != kRemindRow);
        CHECK(row != kFirstRoundSeeds[0]);
    }
}

TEST_CASE("the give-up row is last on the giver's list while the errand is open, and picked, the model answers and the store is told") {
    // notes.md (13 Oct 2026): "there should always be an 'I give up' option in the first
    // set of a conversation, when you have an active quest with the person you're talking
    // to." Nothing is written for it on Bartel, so the model answers, told what has just
    // become true; the abandon goes out ahead of the reply, and the list after has none of
    // the errand's rows left.
    QuestFacts accepted;
    accepted.accepted = true;
    WrittenFixture f(first_meeting(), accepted);
    LogTailer tailer(f.game.log());
    f.game.launch();
    f.open(tailer);
    f.play_all(tailer);
    REQUIRE(f.game.menu().size() == 4);
    CHECK(f.game.menu()[0] == kRemindRow);
    CHECK(f.game.menu()[3] == kGiveUpRow);
    CHECK(f.offered[3].intent == "give_up");
    const std::size_t calls = f.llm.calls.size();
    f.llm.replies = {"Then that's that. I'll find somebody with a spine."};
    f.game.picked(4, "give_up", std::string(kGiveUpRow));
    f.drain(tailer);
    CHECK(f.said("picked as give_up, answered by the model with the command owed"));
    CHECK(f.llm.calls.size() == calls + 1);
    CHECK(f.llm.calls.back().messages.back().content.find("giving up the errand") != std::string::npos);
    REQUIRE(f.recorded.size() == 1);
    CHECK(f.recorded[0].second == kQuestAbandon);
    REQUIRE(f.game.play().has_value());
    bool abandoned = false;
    for (const std::string& directive : f.game.directives()) {
        if (directive.find("@quest c00183e1da7e abandon") == 0) abandoned = true;
    }
    CHECK(abandoned);
    CHECK(f.session.history().back().content == "Then that's that. I'll find somebody with a spine.");
    f.play_all(tailer);
    for (const MenuOption& row : f.offered) {
        if (row.row > 4) CHECK((row.intent != "give_up" && row.intent != "remind" && row.intent != "help"));
    }
    for (const std::string& row : f.game.menu()) {
        CHECK(row != kGiveUpRow);
        CHECK(row != kRemindRow);
    }
}

TEST_CASE("the errand paid off on the opening line is not offered to be reminded of after") {
    // notes.md (7 Oct 2026): Joan had her marigolds and the menu under her thanks still
    // said "Remind me what it is you need of me." The reminder was planned on the facts
    // before the settle; it goes with the complete the opening line carries.
    QuestFacts brought;
    brought.accepted = true;
    brought.have = 1;
    WrittenFixture f(first_meeting(), brought, true, "My axe. Good lad; here is your fifty.");
    LogTailer tailer(f.game.log());
    f.game.launch();
    f.game.begin(kBartel);
    f.drain(tailer);
    f.play_all(tailer);
    CHECK(f.said("the quest settles on this turn, on the line written for it"));
    REQUIRE(f.recorded.size() == 1);
    CHECK(f.recorded[0].second == kQuestComplete);
    // And since (notes.md 23 Oct 2026) the thanks close the talk when nothing leads on:
    // no list at all under them, the turn flagged @end, as an accepting line's is.
    CHECK(f.said("the errand is taken, so the conversation closes and no rows are offered"));
    CHECK(f.game.menu().empty());
    CHECK(f.game.directives().back() == "@end");
    for (const MenuOption& row : f.offered) CHECK((row.intent != "remind" && row.intent != "help"));
}

TEST_CASE("at a beggar, the row that gives is laid for what Henry carries, and each gift owes the mod @alms") {
    // notes.md (6 Oct 2026): beggars always accept food, clothing and groschen. The begin
    // line says what he could spare (alms=mf: a purse and food, no spare garment); the row
    // is on the opening list, answered with a row per kind, and each kind is given once.
    WrittenFixture f(first_meeting());
    LogTailer tailer(f.game.log());
    f.game.launch();
    f.llm.replies = {""};
    f.game.begin_with(kBartel, " alms=mf");
    f.drain(tailer);
    f.play_all(tailer);
    REQUIRE(f.game.menu().size() == 4);
    CHECK(f.game.menu()[3] == kAlmsRow);
    CHECK(f.offered[3].intent == "alms");
    const std::size_t calls = f.llm.calls.size();

    f.game.picked(4, "alms", std::string(kAlmsRow));
    f.drain(tailer);
    CHECK(f.said("picked as alms, answered with the beggar's thanks, with 2 things to give as written"));
    CHECK(f.llm.calls.size() == calls);
    f.play_all(tailer);
    REQUIRE(f.game.menu().size() >= 2);
    CHECK(f.game.menu()[0] == "Here's three groschen. Get yourself a hot meal.");
    CHECK(f.game.menu()[1] == "Here, something to eat.");
    for (const std::string& row : f.game.menu()) CHECK(row.find("keep the cold off") == std::string::npos);
    const std::size_t given = f.offered.size() - f.game.menu().size();
    CHECK(f.offered[given].intent == "alms:money");
    CHECK(f.offered[given + 1].intent == "alms:food");

    // The groschen: the thanks as written, and @alms money ahead of them for the mod.
    f.game.picked(1, "alms:money", "Here's three groschen. Get yourself a hot meal.");
    f.drain(tailer);
    CHECK(f.said("picked as alms:money, answered with the thanks for money as written"));
    CHECK(f.llm.calls.size() == calls);
    f.play_all(tailer);
    bool owed = false;
    for (const std::string& directive : f.game.directives()) owed = owed || directive == "@alms money";
    CHECK(owed);
    CHECK(f.session.history().back().content == "Three groschen! God reward you, sir. That's bread for days.");
    // Given, the money row does not come back; the giving row does, for the food.
    for (const std::string& row : f.game.menu()) CHECK(row != "Here's three groschen. Get yourself a hot meal.");
    bool again = false;
    for (const std::string& row : f.game.menu()) again = again || row == kAlmsRow;
    CHECK(again);
    int money_rows = 0;
    for (const MenuOption& row : f.offered) money_rows += row.intent == "alms:money";
    CHECK(money_rows == 1);
}

TEST_CASE("about is the self-account on a first meeting, and the line for being asked again after") {
    WrittenFixture f(first_meeting());
    LogTailer tailer(f.game.log());
    f.game.launch();
    f.open(tailer);
    f.play_all(tailer);
    const std::size_t calls = f.llm.calls.size();
    f.game.picked(3, "about", std::string(kFirstRoundSeeds[2]));
    f.drain(tailer);
    CHECK(f.said("picked as about, answered with the self-account as written"));
    CHECK(f.session.history()[2].content == "Bartel. I dig where I'm told.");
    f.play_all(tailer);
    // The list after is written too: the offer of help comes back, since it was not taken,
    // and what he knows fills the rest; `about`, spent, is not offered again.
    CHECK(f.llm.calls.size() == calls);
    REQUIRE(f.game.menu().size() == 4);
    CHECK(f.game.menu()[0] == kFirstRoundSeeds[0]);
    CHECK(f.game.menu()[1] == "Tell me about your trade.");
    for (const std::string& row : f.game.menu()) CHECK(row != kFirstRoundSeeds[2]);
    // Asked twice in one conversation -- the mod handing back a row the app did not offer --
    // the second time is the model's, told not to recite.
    f.llm.replies = {"I said. I dig."};
    f.game.picked(3, "about", "Tell me about yourself.");
    f.drain(tailer);
    CHECK(f.session.history()[3].content.find("told them about yourself before") != std::string::npos);
    CHECK(f.session.history()[4].content == "I said. I dig.");

    // Second meeting: the account was given last time, so the pick is answered with the
    // line written for being asked again, and the model is not called.
    Standing again;
    again.met = 2;
    WrittenFixture g(again);
    LogTailer tail(g.game.log());
    g.game.launch();
    g.open(tail);
    g.play_all(tail);
    CHECK(g.said("with written answers for the offer, about and 3 topics"));
    const std::size_t g_calls = g.llm.calls.size();
    g.game.picked(3, "about", std::string(kFirstRoundSeeds[2]));
    g.drain(tail);
    CHECK(g.said("picked as about, answered with the self-account as written"));
    CHECK(g.session.history()[2].content == "You asked me that before. I dig. Still.");
    CHECK(g.llm.calls.size() == g_calls);

    // A soul with no such line: the model answers, told the account was given.
    WrittenFixture h(again, {}, false);
    LogTailer tail_h(h.game.log());
    h.game.launch();
    h.open(tail_h);
    h.play_all(tail_h);
    h.llm.replies = {"I told you. I dig."};
    h.game.picked(3, "about", std::string(kFirstRoundSeeds[2]));
    h.drain(tail_h);
    CHECK_FALSE(h.said("as written"));
    const std::string& asked = h.session.history()[1].content;
    CHECK(asked.find(kFirstRoundSeeds[2]) == 0);
    CHECK(asked.find("told them about yourself before") != std::string::npos);
    CHECK(h.session.history()[2].content == "I told you. I dig.");
}

TEST_CASE("a question is met with three things they know, each a row, and the row is answered") {
    WrittenFixture f(first_meeting());
    LogTailer tailer(f.game.log());
    f.game.launch();
    f.open(tailer);
    f.play_all(tailer);

    // A stranger is offered the open topics, three at a time; the confided one is kept back.
    // The fourth row is the offer of help, still unspent, and nothing is asked of the model.
    const std::size_t calls = f.llm.calls.size();
    f.game.picked(2, "ask", std::string(kFirstRoundSeeds[1]));
    f.drain(tailer);
    CHECK(f.said("picked as ask, answered with 3 topics to choose from as written"));
    f.play_all(tailer);
    // The reply is the leave to ask and recites none of the rows (notes.md 8 Oct 2026).
    const std::string& reply = f.session.history()[2].content;
    CHECK(reply.find("Ask") != std::string::npos);
    CHECK(reply.find("pillory") == std::string::npos);
    REQUIRE(f.game.menu().size() == 4);
    CHECK(f.game.menu()[0] == "Tell me about waiting for work.");
    CHECK(f.game.menu()[1] == "Tell me about your trade.");
    CHECK(f.game.menu()[2] == "Tell me about the pillory.");
    CHECK(f.game.menu()[3] == kFirstRoundSeeds[0]);
    CHECK(f.offered[5].intent == "ask:his_trade");
    CHECK(f.llm.calls.size() == calls);
    // The topic row carries its key in the intent, which the mod hands back on the pick.
    bool carried = false;
    for (const std::string& directive : f.game.directives()) {
        if (directive.find("@opt 2 ask:his_trade ") == 0) carried = true;
    }
    CHECK(carried);

    // Picked, the topic is answered in the written words, with no call at all.
    f.game.picked(2, "ask:his_trade", "Tell me about your trade.");
    f.drain(tailer);
    CHECK(f.said("picked as ask:his_trade, answered with what they know of his trade as written"));
    CHECK(f.llm.calls.size() == calls);
    CHECK(f.session.history()[4].content == "Digging, mostly. Carrying when there is nothing to dig.");
    f.play_all(tailer);
    // And it is not on the list after, which is the two seeds and the two topics left.
    REQUIRE(f.game.menu().size() == 4);
    for (const std::string& row : f.game.menu()) CHECK(row != "Tell me about your trade.");

    // Asked again, the topic already told is not among the ones offered; the two that are
    // come first, laid by the answer, with the seeds after them.
    f.game.picked(2, "ask", "Where does the road to Sasau go?");
    f.drain(tailer);
    f.play_all(tailer);
    REQUIRE(f.game.menu().size() == 4);
    CHECK(f.game.menu()[0] == "Tell me about waiting for work.");
    CHECK(f.game.menu()[1] == "Tell me about the pillory.");
    CHECK(f.game.menu()[2] == kFirstRoundSeeds[0]);
    CHECK(f.game.menu()[3] == kFirstRoundSeeds[2]);
    CHECK(f.llm.calls.size() == calls);

    // A topic key nobody wrote for is not answered from the database, and the model speaks.
    f.llm.replies = {"Eh?"};
    f.game.picked(1, "ask:nothing_here", "Tell me about nothing.");
    f.drain(tailer);
    CHECK(f.session.history().back().content == "Eh?");
    CHECK(f.llm.replies.empty());
}

TEST_CASE("with the favour done, help is not offered, a typed offer gets the written reply, and the errand is not offered again") {
    Standing third;
    third.met = 3;
    third.quests["c00183e1da7e"] = "done";
    QuestFacts done;
    done.done = true;
    WrittenFixture f(third, done);
    LogTailer tailer(f.game.log());
    f.game.launch();
    // The save has Agatha named to Henry on an earlier visit, so the confided topic about
    // her is his to ask after (knows_of); without that it would stay off the list.
    f.game.dump(kBartel, 3, {}, "", 0, {"name:agatha"});
    f.open(tailer);
    f.play_all(tailer);
    // His errand is done and his written answer to help is a refusal, not a lead, so the
    // offer of help is not on the menu (Scripted::help_due): the question, the account of
    // himself, and two of what he knows.
    REQUIRE(f.game.menu().size() == 4);
    CHECK(f.game.menu()[0] == kFirstRoundSeeds[1]);
    CHECK(f.game.menu()[1] == kFirstRoundSeeds[2]);
    for (const MenuOption& row : f.offered) CHECK(row.intent != "help");
    const std::size_t calls = f.llm.calls.size();
    // Offered all the same (a pick by intent stands for the typed offer, which the fixture
    // has no knowledge store to recognise), the answer is the written refusal, no model.
    f.game.picked(1, "help", std::string(kFirstRoundSeeds[0]));
    f.drain(tailer);
    CHECK(f.said("picked as help, answered with the help reply as written"));
    CHECK(f.session.history()[2].content == "No. I have my tools and my peace.");
    f.play_all(tailer);
    // The account of himself for being asked again, then three of what he knows, and no
    // accepting row for a favour done.
    REQUIRE(f.game.menu().size() == 4);
    CHECK(f.game.menu()[0] == kFirstRoundSeeds[2]);
    CHECK(f.game.menu()[1] == "Tell me about the pillory.");
    CHECK(f.recorded.empty());
    CHECK(f.llm.calls.size() == calls);
    // And a man met three times with the favour done tells the confided thing.
    f.game.picked(2, "ask", "Where does the road to Sasau go?");
    f.drain(tailer);
    f.play_all(tailer);
    // Julian's topic is gone with the axe come home; the rest are told three at a time.
    CHECK(f.session.history()[4].content.find("pillory") == std::string::npos);
    f.game.picked(1, "ask:daily_work", "Tell me about waiting for work.");
    f.drain(tailer);
    f.play_all(tailer);
    f.game.picked(1, "ask", "Where does the road to Sasau go?");
    f.drain(tailer);
    f.play_all(tailer);
    // The three left and the account of himself are the list; nothing is asked of the model
    // for it, on this round or the ones before.
    REQUIRE(f.game.menu().size() == 4);
    CHECK(f.game.menu()[2] == "Tell me about Agatha's grave.");
    CHECK(f.game.menu()[3] == kFirstRoundSeeds[2]);
    CHECK(f.llm.calls.size() == calls);
}

TEST_CASE("a question is not offered to somebody the soul trusts with nothing they know") {
    // Everything Bartel knows is confided, and the player is a stranger: `ask` would have
    // nothing written behind it, so the opening round is two seeds and the written rest.
    Fixture f(false);
    f.session.set_persona_source(
        [](std::string_view, const Approach&, std::string_view, const SoulRecord&)
            -> std::optional<Persona> {
            Persona persona{"Bartel", "You are Bartel, a varlet in Kuttenberg.", "What?"};
            const std::vector<Knowledge> knowledge = {
                {"agatha_grave", "Agatha's grave", "confided", "Agatha died of a winter fever."},
                {"the_debt", "his debt", "trusted", "I owe Mikesh for the winter."},
            };
            const std::vector<Remark> remarks = {
                {"cold_work", "Cold work, this.", "Cold? You should try it in January."},
            };
            Standing standing;
            standing.met = 1;
            persona.scripted = plan_scripted("I dig.", knowledge, {}, {}, {}, standing, {}, {}, remarks);
            REQUIRE(persona.scripted.withheld);
            REQUIRE(persona.scripted.topics.empty());
            return persona;
        });
    LogTailer tailer(f.game.log());
    f.game.launch();
    f.open(tailer);
    f.play_all(tailer);
    // No errand and no lead, so no offer of help either: the account of himself and the
    // one remark.
    REQUIRE(f.game.menu().size() == 2);
    CHECK(f.game.menu()[0] == kFirstRoundSeeds[2]);
    CHECK(f.game.menu()[1] == "Cold work, this.");
    for (const MenuOption& row : f.offered) CHECK((row.intent != "ask" && row.intent != "help"));
    CHECK(f.said("offered 2 rows, all written"));
    CHECK(f.llm.calls.size() == 1);
}

namespace {

// Joan, whose topics are about her neighbour Alice (a soul of the circle), her late husband
// Humphrey (an absentee, by name) and her dairy (nobody). Her account of herself names
// Humphrey; nothing she says unasked names Alice.
std::optional<Persona> joan_persona() {
    Persona persona{"Joan", "You are Joan, a widow in Horany.", "What is it?"};
    const std::vector<Person> people = {{"khor_woman_2", "Alice"}, {name_token("Humphrey"), "Humphrey"}};
    const std::vector<Knowledge> knowledge = {
        {"dairy_work", "churning butter", "open", "I run the master's dairy."},
        {"alices_fears", "Alice's panic", "open", "Alice is making herself sick with worry."},
        {"husband_humphrey", "Humphrey", "open", "I loved my Humphrey deeply."},
    };
    Standing standing;
    standing.met = 1;
    persona.scripted = plan_scripted("Joan. I keep the dairy, since my Humphrey died.", knowledge, {}, {}, {},
                                     standing, {}, {}, {}, {}, people);
    return persona;
}

}  // namespace

TEST_CASE("a question about a person waits until Henry has met them or heard them named") {
    Fixture f(false);
    f.session.set_persona_source(
        [](std::string_view, const Approach&, std::string_view, const SoulRecord&) { return joan_persona(); });
    LogTailer tailer(f.game.log());
    f.game.launch();
    f.open(tailer);
    f.play_all(tailer);
    // The opening round: the question, the account of herself, and of what she knows only
    // the dairy -- Alice and Humphrey are names Henry has not heard.
    REQUIRE(f.game.menu().size() == 3);
    CHECK(f.game.menu()[0] == kFirstRoundSeeds[1]);
    CHECK(f.game.menu()[1] == kFirstRoundSeeds[2]);
    CHECK(f.game.menu()[2] == "Tell me about churning butter.");
    for (const std::string& directive : f.game.directives()) CHECK(directive.rfind("@heard", 0) != 0);

    // Her account of herself names Humphrey: the save is told on that very turn, and the
    // row about him is on the list after it. Alice is still nobody to Henry.
    f.game.picked(2, "about", std::string(kFirstRoundSeeds[2]));
    f.drain(tailer);
    f.play_all(tailer);
    CHECK(std::find(f.game.directives().begin(), f.game.directives().end(), "@heard name:humphrey") !=
          f.game.directives().end());
    std::vector<std::string> menu = f.game.menu();
    CHECK(std::find(menu.begin(), menu.end(), "Tell me about Humphrey.") != menu.end());
    CHECK(std::find(menu.begin(), menu.end(), "Tell me about Alice's panic.") == menu.end());
    // And the ask round offers what is known of, no more.
    f.game.picked(1, "ask", std::string(kFirstRoundSeeds[1]));
    f.drain(tailer);
    f.play_all(tailer);
    menu = f.game.menu();
    CHECK(std::find(menu.begin(), menu.end(), "Tell me about Humphrey.") != menu.end());
    CHECK(std::find(menu.begin(), menu.end(), "Tell me about Alice's panic.") == menu.end());
    CHECK(f.llm.calls.size() == 1);
}

TEST_CASE("a person the save has Henry meeting or hearing of is open from the first list") {
    Fixture f(false);
    f.session.set_persona_source(
        [](std::string_view, const Approach&, std::string_view, const SoulRecord&) { return joan_persona(); });
    LogTailer tailer(f.game.log());
    f.game.launch();
    // The dump before the begin: Alice met (by her soul id), Humphrey heard of before.
    f.game.dump(kBartel, 1, {}, "", 0, {"khor_woman_2", "name:humphrey"});
    f.open(tailer);
    f.play_all(tailer);
    REQUIRE(f.game.menu().size() == 4);
    CHECK(f.game.menu()[2] == "Tell me about churning butter.");
    CHECK(f.game.menu()[3] == "Tell me about Alice's panic.");
    // Nothing owed the save for what it already has.
    for (const std::string& directive : f.game.directives()) CHECK(directive.rfind("@heard", 0) != 0);
}

TEST_CASE("the question itself is withheld while everything written is about strangers") {
    Fixture f(false);
    f.session.set_persona_source(
        [](std::string_view, const Approach&, std::string_view, const SoulRecord&) -> std::optional<Persona> {
            Persona persona{"Joan", "You are Joan, a widow in Horany.", "What is it?"};
            const std::vector<Person> people = {{"khor_woman_2", "Alice"}};
            const std::vector<Knowledge> knowledge = {
                {"alices_fears", "Alice's panic", "open", "Alice is making herself sick with worry."},
            };
            Standing standing;
            standing.met = 1;
            persona.scripted = plan_scripted("", knowledge, {}, {}, {}, standing, {}, {}, {}, {}, people);
            return persona;
        });
    LogTailer tailer(f.game.log());
    f.game.launch();
    f.open(tailer);
    f.play_all(tailer);
    for (const MenuOption& row : f.offered) CHECK(row.intent != "ask");
}

TEST_CASE("the holder's condition is a row on the giver's menu, and his written answer owes his word") {
    // Bartel's axe as a dispute, the player back from Julian carrying his condition.
    Fixture f(false);
    f.session.set_persona_source(
        [](std::string_view, const Approach& approach, std::string_view,
           const SoulRecord&) -> std::optional<Persona> {
            Persona persona{"Bartel", "You are Bartel, a varlet in Kuttenberg.", "Well?"};
            Quest quest = bartels_axe();
            quest.dispute = true;
            quest.apologise = "Julian says you must admit he paid your slate.";
            quest.apologise_yes = "Christ! Fine. He paid. Tell him I said it.";
            quest.apologise_no = "I'll admit no such thing.";
            QuestFacts facts;
            facts.accepted = true;
            facts.broker = true;
            persona.quest_of = quest;
            persona.quest = plan_quest(quest, facts, approach);
            return persona;
        });
    LogTailer tailer(f.game.log());
    f.game.launch();
    // The opening round: the seeds and the condition, all the app's. No offer of help:
    // the errand is taken, and nothing scripted here says to ask (Scripted::help_due).
    f.open(tailer);
    f.play_all(tailer);
    // The condition first: the errand's rows head every list (notes.md 9 Oct 2026); the
    // give-up row last, on every list while the errand is open (notes.md 13 Oct 2026).
    REQUIRE(f.game.menu().size() == 4);
    CHECK(f.game.menu()[0] == "Julian says you must admit he paid your slate.");
    CHECK(f.offered[0].intent == "apologise");
    CHECK(f.game.menu()[3] == kGiveUpRow);
    CHECK(f.offered[3].intent == "give_up");
    CHECK(f.said("offered 4 rows, all written"));

    // Picked, the answer is the written one, the command rides ahead of it in the turn file,
    // and the row is not offered again: with nothing else written, the list after is the
    // give-up row alone.
    const std::size_t calls = f.llm.calls.size();
    f.game.picked(3, "apologise", "Julian says you must admit he paid your slate.");
    f.drain(tailer);
    CHECK(f.said("picked as apologise, answered with the giver giving his word as written"));
    CHECK(f.llm.calls.size() == calls);
    REQUIRE(f.recorded.size() == 1);
    CHECK(f.recorded[0].second == kQuestApologise);
    REQUIRE(f.game.play().has_value());
    bool carried = false;
    for (const std::string& directive : f.game.directives()) {
        if (directive.find("@quest c00183e1da7e apologise") == 0) carried = true;
    }
    CHECK(carried);
    CHECK(f.session.history().back().content == "Christ! Fine. He paid. Tell him I said it.");
    f.play_all(tailer);
    REQUIRE(f.offered.size() == 5);
    CHECK(f.offered[4].intent == "give_up");
    CHECK(f.said("offered 1 rows, all written"));
}

TEST_CASE("a letter's writer has Henry's two reports on his menu, and the one picked ends the errand") {
    // Veit's letter, agreed and still in Henry's pocket: the writer's menu carries "it is
    // delivered" (a lie here, paid all the same) and "I lost it".
    Fixture f(false);
    f.session.set_persona_source(
        [](std::string_view, const Approach& approach, std::string_view,
           const SoulRecord&) -> std::optional<Persona> {
            Persona persona{"Veit Berger", "You are Veit Berger, a minter.", "Well?"};
            Quest quest;
            quest.id = "8ab11c4ab065";
            quest.kind = "deliver";
            quest.role = QuestRole::Giver;
            quest.giver_soul = "prepadeniVlasskehoDvora_worker_2";
            quest.giver_name = "Veit Berger";
            quest.target_soul = "prepadeniVlasskehoDvora_silverCarrier_4";
            quest.target_name = "Ulrich the Mouth";
            quest.item_class = "859d46b3-d739-5d45-a80e-64a232ab9ed3";
            quest.item_name = "letter";
            quest.reward_groschen = 5;
            quest.thanks = "You actually gave it to him? Here is the five groschen.";
            quest.report = "I delivered your letter to Ulrich the Mouth.";
            quest.keep = "I'm sorry, but I lost your letter.";
            quest.kept_reply = "Lost it? Saints. Just leave me be.";
            QuestFacts facts;
            facts.accepted = true;
            facts.have = approach.have;
            persona.quest_of = quest;
            persona.quest = plan_quest(quest, facts, approach);
            return persona;
        });
    LogTailer tailer(f.game.log());
    f.game.launch();
    f.llm.replies = {""};
    // A store with no accept time withholds nothing; the Speech stats are what the lie is
    // weighed on (Henry's 20 over Veit's 10 carries it at the default factor).
    f.game.say("<12:04:31> BRIDGE begin npc=prepadeniVlasskehoDvora_worker_2 doing=none spc=20 nspc=10 have=1");
    f.drain(tailer);
    // The sheet's prefill, as the poller does it between looks at the log.
    f.session.speculate([] { return false; });
    f.play_all(tailer);
    // Four rows: the two reports first, which are what he came back for (notes.md 9 Oct
    // 2026: the errand's rows head the list), then the one seed there is room for (no offer
    // of help on a man whose errand is taken), and the give-up row last (notes.md 13 Oct
    // 2026). The report before the trip says it is a lie.
    REQUIRE(f.game.menu().size() == 4);
    CHECK(f.game.menu()[0] == "(Lie) I delivered your letter to Ulrich the Mouth.");
    CHECK(f.offered[0].intent == "report");
    CHECK(f.game.menu()[1] == "I'm sorry, but I lost your letter.");
    CHECK(f.offered[1].intent == "keep");
    CHECK(f.offered[2].intent == "ask");
    CHECK(f.offered[3].intent == "give_up");
    CHECK(f.said("offered 4 rows, all written"));

    // Picked, the thanks are the written ones and the lie rides ahead of them; and both
    // rows are gone from the next list, since the errand is over either way.
    f.game.picked(1, "report", "(Lie) I delivered your letter to Ulrich the Mouth.");
    f.drain(tailer);
    CHECK(f.said("picked as report, answered with the writer's answer to report, the speech check won as written"));
    REQUIRE(f.recorded.size() == 1);
    CHECK(f.recorded[0].second == kQuestLie);
    REQUIRE(f.game.play().has_value());
    bool lied = false;
    for (const std::string& directive : f.game.directives()) {
        if (directive.find("@quest 8ab11c4ab065 lie") == 0) lied = true;
    }
    CHECK(lied);
    CHECK(f.session.history().back().content ==
          "You actually gave it to him? Here is the five groschen.");
    f.play_all(tailer);
    for (std::size_t i = 4; i < f.offered.size(); ++i) {
        CHECK(f.offered[i].text != "(Lie) I delivered your letter to Ulrich the Mouth.");
        CHECK(f.offered[i].text != "I'm sorry, but I lost your letter.");
    }
    CHECK(f.llm.calls.size() == 1);
}

TEST_CASE("a letter's writer catches the lie when Henry's Speech is short, and the errand stands") {
    // The same letter, the same lie, and a Henry whose Speech does not carry it: the
    // writer's catch, no groschen, no store command, and the row spent while the errand
    // and "I lost it" stay (notes.md 9 Oct 2026).
    Fixture f(false);
    f.session.set_persona_source(
        [](std::string_view, const Approach& approach, std::string_view,
           const SoulRecord&) -> std::optional<Persona> {
            Persona persona{"Veit Berger", "You are Veit Berger, a minter.", "Well?"};
            Quest quest;
            quest.id = "8ab11c4ab065";
            quest.kind = "deliver";
            quest.role = QuestRole::Giver;
            quest.giver_soul = "prepadeniVlasskehoDvora_worker_2";
            quest.giver_name = "Veit Berger";
            quest.target_soul = "prepadeniVlasskehoDvora_silverCarrier_4";
            quest.target_name = "Ulrich the Mouth";
            quest.item_class = "859d46b3-d739-5d45-a80e-64a232ab9ed3";
            quest.item_name = "letter";
            quest.reward_groschen = 5;
            quest.thanks = "You actually gave it to him? Here is the five groschen.";
            quest.report = "I delivered your letter to Ulrich the Mouth.";
            quest.keep = "I'm sorry, but I lost your letter.";
            quest.kept_reply = "Lost it? Saints. Just leave me be.";
            quest.lie_caught = "Delivered? You have not left the square. Get on with it.";
            QuestFacts facts;
            facts.accepted = true;
            facts.have = approach.have;
            facts.hours_since_accept = 3.0;
            persona.quest_of = quest;
            persona.quest = plan_quest(quest, facts, approach);
            return persona;
        });
    LogTailer tailer(f.game.log());
    f.game.launch();
    f.llm.replies = {""};
    f.game.say("<12:04:31> BRIDGE begin npc=prepadeniVlasskehoDvora_worker_2 doing=none spc=10 nspc=10 have=1");
    f.drain(tailer);
    f.session.speculate([] { return false; });
    f.play_all(tailer);
    REQUIRE(f.game.menu().size() == 4);
    CHECK(f.game.menu()[0] == "(Lie) I delivered your letter to Ulrich the Mouth.");
    f.game.picked(1, "report", "(Lie) I delivered your letter to Ulrich the Mouth.");
    f.drain(tailer);
    CHECK(f.said("picked as report, answered with the writer catching the report (speech 10 against 10) as written"));
    CHECK(f.recorded.empty());
    REQUIRE(f.game.play().has_value());
    bool marked = false;
    for (const std::string& directive : f.game.directives()) {
        if (directive == "@check speech fail") marked = true;
        CHECK(directive.find("@quest 8ab11c4ab065 lie") != 0);
    }
    CHECK(marked);
    CHECK(f.session.history().back().content == "Delivered? You have not left the square. Get on with it.");
    // The lie is spent; "I lost it" is still there to pick.
    f.play_all(tailer);
    bool keep = false;
    for (std::size_t i = 4; i < f.offered.size(); ++i) {
        CHECK(f.offered[i].text != "(Lie) I delivered your letter to Ulrich the Mouth.");
        if (f.offered[i].text == "I'm sorry, but I lost your letter.") keep = true;
    }
    CHECK(keep);
    CHECK(f.llm.calls.size() == 1);
}

TEST_CASE("a soul with nothing written for the pick is answered by the model, as before") {
    Fixture f(true);
    LogTailer tailer(f.game.log());
    f.game.launch();
    f.open(tailer);
    f.play_all(tailer);
    CHECK_FALSE(f.said("with written answers"));
    f.llm.replies = {"Nothing you can help with."};
    f.game.picked(2, "ask", std::string(kFirstRoundSeeds[1]));
    f.drain(tailer);
    CHECK_FALSE(f.said("as written"));
    CHECK(f.session.history()[2].content == "Nothing you can help with.");
    CHECK(f.llm.replies.empty());
}

TEST_CASE("the remarks written for a soul fill the list between the topics, and are answered as written") {
    // Wenzel as the pipeline would write him with remarks: no favour, nothing to say to
    // help, two things he knows and three remarks Henry might make.
    Fixture f(false);
    f.session.set_persona_source(
        [](std::string_view, const Approach&, std::string_view, const SoulRecord&)
            -> std::optional<Persona> {
            Persona persona{"Wenzel", "You are Wenzel, a carpenter.", "Hm?"};
            const std::vector<Knowledge> knowledge = {
                {"the_saw", "his saw", "open", "Frantishek has it. Has had it since Lent."},
                {"the_mill", "the mill", "open", "The mill's wheel wants new paddles."},
            };
            const std::vector<Remark> remarks = {
                {"cold_work", "Cold work, this.", "Cold? You should try it in January."},
                {"good_timber", "That's good timber you've got there.", "Oak. From the Bishop's wood, and paid for."},
                {"long_day", "You look like you've had a long day.", "Every day's long. This one's just louder."},
            };
            Standing standing;
            standing.met = 1;
            persona.scripted = plan_scripted("", knowledge, {}, {}, {}, standing, {}, {}, remarks);
            return persona;
        });
    LogTailer tailer(f.game.log());
    f.game.launch();
    // Nothing is written for about on this soul, so the question and the account still
    // open the round, as the design has it; no offer of help, since he has no errand and
    // no lead; then the first topic and the first remark, in turn.
    f.open(tailer);
    f.play_all(tailer);
    REQUIRE(f.game.menu().size() == 4);
    CHECK(f.game.menu()[0] == kFirstRoundSeeds[1]);
    CHECK(f.game.menu()[2] == "Tell me about your saw.");
    CHECK(f.game.menu()[3] == "Cold work, this.");
    CHECK(f.said("offered 4 rows, all written"));

    // A typed line goes to the model as ever; the list after it is written, and turns on
    // from where the opening round left off: topic, remark, remark, and round to the first
    // topic. The seeds are not on it, since there is nothing written to answer them with.
    const std::size_t calls = f.llm.calls.size();
    f.llm.replies = {"What of it?"};
    f.game.typed("Busy?");
    f.drain(tailer);
    f.play_all(tailer);
    CHECK(f.llm.calls.size() == calls + 1);
    REQUIRE(f.game.menu().size() == 4);
    CHECK(f.game.menu()[0] == "Tell me about the mill.");
    CHECK(f.game.menu()[1] == "That's good timber you've got there.");
    CHECK(f.game.menu()[2] == "You look like you've had a long day.");
    CHECK(f.game.menu()[3] == "Tell me about your saw.");
    CHECK(f.offered[4].intent == "ask:the_mill");

    // Picked, the remark is answered in the written words, with no call; and it is spent.
    f.game.picked(1, "say:cold_work", "Cold work, this.");
    f.drain(tailer);
    CHECK(f.said("picked as say:cold_work, answered with the written answer to that remark as written"));
    CHECK(f.llm.calls.size() == calls + 1);
    CHECK(f.session.history().back().content == "Cold? You should try it in January.");
    CHECK(f.session.history()[f.session.history().size() - 2].content == "Cold work, this.");
    f.play_all(tailer);
    REQUIRE(f.game.menu().size() == 4);
    for (const std::string& row : f.game.menu()) CHECK(row != "Cold work, this.");

    // As the written rows run out the list gets shorter, and nothing is asked of the model
    // to fill it; with everything spent there is no list, and the player types.
    for (const std::string& intent : {"say:good_timber", "say:long_day", "ask:the_saw"}) {
        f.game.picked(1, intent, "whatever the row said");
        f.drain(tailer);
        f.play_all(tailer);
    }
    CHECK(f.llm.calls.size() == calls + 1);
    REQUIRE(f.game.menu().size() == 1);
    CHECK(f.game.menu()[0] == "Tell me about the mill.");
    f.game.picked(1, "ask:the_mill", "Tell me about the mill.");
    f.drain(tailer);
    f.play_all(tailer);
    CHECK(f.llm.calls.size() == calls + 1);
    CHECK(f.said("no rows to offer, so the list is the way out alone"));
    CHECK(f.session.rows().empty());
}

// ---------------------------------------------------------------- guessing ahead

TEST_CASE("while the player reads the list, the replies to its rows are worked out, and a pick uses one") {
    Fixture f;
    f.session.set_speculation(true);
    std::vector<Timing> timings;
    Listener listener;
    listener.on_status = [&f](std::string_view text) { f.statuses.emplace_back(text); };
    listener.on_option = [&f](const MenuOption& option) { f.offered.push_back(option); };
    listener.on_timing = [&timings](const Timing& timing) { timings.push_back(timing); };
    f.session.set_listener(std::move(listener));
    LogTailer tailer(f.game.log());
    f.game.launch();
    // Nothing is written for Bartel here, so every row on the opening round -- the two
    // seeds (no errand, so no offer of help) -- is one the model would have to answer.
    f.open(tailer);
    f.play_all(tailer);
    REQUIRE(f.game.menu().size() == 2);
    CHECK(f.session.has_speculation());
    const std::size_t calls = f.llm.calls.size();

    // One guess a call, row order, each the very call a pick would make: the row's words
    // as the player's turn, at the reply's own settings.
    f.llm.replies = {"Ask, then."};
    CHECK(f.session.speculate([] { return false; }));
    CHECK(f.llm.calls.size() == calls + 1);
    CHECK(f.llm.last().messages.back().content == kFirstRoundSeeds[1]);
    CHECK(f.llm.last().messages.back().role == "user");
    CHECK(f.llm.last().options.temperature == f.options.temperature);
    CHECK(f.said("guessed the reply to \"I have a question.\" ahead"));
    // Nothing reached the game: the guess is held, not said.
    CHECK_FALSE(f.game.has_turn(f.session.played() + 1));
    CHECK(f.session.history().size() == 1);

    // The second guess is the last, so the call that makes it says there is no more to do.
    f.llm.replies = {"I saw wood. That's the whole of it."};
    CHECK_FALSE(f.session.speculate([] { return false; }));
    CHECK_FALSE(f.session.has_speculation());
    CHECK(f.llm.calls.size() == calls + 2);
    CHECK(f.llm.last().messages.back().content == kFirstRoundSeeds[2]);

    // The pick is answered from the guess, whole and at once: no call is made, and the
    // timing carries the flag that says the wait was paid before.
    f.game.picked(2, "about", std::string(kFirstRoundSeeds[2]));
    f.drain(tailer);
    CHECK(f.said("was guessed ahead, and is said at once"));
    CHECK(f.llm.calls.size() == calls + 2);
    REQUIRE(f.session.history().size() == 3);
    CHECK(f.session.history()[1].content == kFirstRoundSeeds[2]);
    CHECK(f.session.history()[2].content == "I saw wood. That's the whole of it.");
    REQUIRE(!timings.empty());
    CHECK(timings.back().guessed);
    f.play_all(tailer);
    // One used; the one the player did not pick was wasted, and is counted. Nothing is
    // written for a list after, so there is nothing to guess.
    CHECK(f.session.guesses_used() == 1);
    CHECK(f.session.guesses_wasted() == 1);
    CHECK_FALSE(f.session.has_speculation());
}

TEST_CASE("a guess is stopped within a token by whatever wants the model, and taken up again after") {
    Fixture f;
    f.session.set_speculation(true);
    LogTailer tailer(f.game.log());
    f.game.launch();
    f.open(tailer);
    f.play_all(tailer);
    const std::size_t calls = f.llm.calls.size();

    // Interrupted at the first piece: the call was made, nothing was kept, and the guess
    // is still to do.
    f.llm.replies = {"Not from you."};
    CHECK(f.session.speculate([] { return true; }));
    CHECK(f.llm.calls.size() == calls + 1);
    CHECK(f.session.has_speculation());

    // A typed line while the guesses stand: answered by the model as ever, since no guess
    // was for those words, and the guesses for the old list go with the old list.
    f.llm.replies = {"Whole of it.", "Fine.", "What?"};
    CHECK(f.session.speculate([] { return false; }));
    CHECK_FALSE(f.session.speculate([] { return false; }));
    CHECK_FALSE(f.session.has_speculation());
    f.game.typed("Busy?");
    f.drain(tailer);
    CHECK(f.session.history()[2].content == "What?");
    CHECK_FALSE(f.said("was guessed ahead"));
    CHECK(f.session.guesses_used() == 0);
    CHECK(f.session.guesses_wasted() == 2);
    f.play_all(tailer);
    // No list follows a typed line on a soul with nothing written, so nothing is left to
    // guess.
    CHECK(f.session.rows().empty());
    CHECK_FALSE(f.session.has_speculation());
    CHECK_FALSE(f.session.speculate([] { return false; }));
}

TEST_CASE("rows the judge would have to rule on are not guessed, and written rows need no guess") {
    // Bartel as written: after the offer, the accepting row is the one row on the list
    // without a written answer, and its guess carries the direction the judge would add.
    WrittenFixture g(first_meeting());
    g.session.set_speculation(true);
    LogTailer tail(g.game.log());
    g.game.launch();
    g.open(tail);
    g.play_all(tail);
    CHECK_FALSE(g.session.has_speculation());
    g.game.picked(1, "help", std::string(kFirstRoundSeeds[0]));
    g.drain(tail);
    g.play_all(tail);
    REQUIRE(g.game.menu().size() == 4);
    CHECK(g.game.menu()[0] == "I'll get your work axe back from Julian.");
    CHECK(g.session.has_speculation());
    g.llm.replies = {"Bless you."};
    CHECK_FALSE(g.session.speculate([] { return false; }));
    const std::string& asked = g.llm.last().messages.back().content;
    CHECK(asked.find("I'll get your work axe back from Julian.") == 0);
    CHECK(asked.size() > std::string("I'll get your work axe back from Julian.").size());
    // Picked, the guess is the thanks, and the errand is taken as it always is.
    g.game.picked(1, "accept", "I'll get your work axe back from Julian.");
    g.drain(tail);
    CHECK(g.said("was guessed ahead"));
    CHECK(g.session.history().back().content == "Bless you.");
    REQUIRE(g.recorded.size() == 1);
    CHECK(g.recorded[0].second == kQuestAccept);

    // A giver whose offer is the model's: the opening round's own help row is not put to
    // the judge and is guessed; the help row again after the offer would be, and is not.
    Fixture f(true);
    f.session.set_speculation(true);
    LogTailer tailer(f.game.log());
    f.game.launch();
    f.open(tailer);
    f.play_all(tailer);
    REQUIRE(f.game.menu().size() == 3);
    f.llm.replies = {"One.", "Two.", "Three."};
    CHECK(f.session.speculate([] { return false; }));
    CHECK(f.session.speculate([] { return false; }));
    CHECK_FALSE(f.session.speculate([] { return false; }));
    CHECK(f.llm.calls[f.llm.calls.size() - 3].messages.back().content == kFirstRoundSeeds[0]);
}

// ------------------------------------------------------------- the opening line, on a look

namespace {

// A persona source with a say in what is written: `greeting` is the line, `exact` whether it
// was written for the very visit, and the sheet says whether they have met, so a look on a
// first meeting and a press on a second are two different sheets, as they are in the game.
void looks_at(Fixture& f, std::string greeting, bool exact) {
    f.session.set_persona_source(
        [greeting = std::move(greeting), exact](std::string_view, const Approach&, std::string_view,
                                                const SoulRecord& record) -> std::optional<Persona> {
            Persona persona{"Bartel", "You are Bartel, a varlet in Kuttenberg.", greeting};
            persona.greeting_exact = exact;
            if (record.met > 1) persona.sheet += " You have spoken with Henry before.";
            return persona;
        });
}

}  // namespace

TEST_CASE("a soul looked at with nothing written for the moment has their opening line guessed, and the press says it at once") {
    Fixture f;
    f.session.set_speculation(true);
    looks_at(f, "", false);
    std::vector<Timing> timings;
    Listener listener;
    listener.on_status = [&f](std::string_view text) { f.statuses.emplace_back(text); };
    listener.on_option = [&f](const MenuOption& option) { f.offered.push_back(option); };
    listener.on_timing = [&timings](const Timing& timing) { timings.push_back(timing); };
    f.session.set_listener(std::move(listener));
    LogTailer tailer(f.game.log());
    f.game.launch();

    // The mod dumps the record as a begin would -- met counting the conversation that has
    // not started -- and then the look. Nothing is talking, and there is a guess to make.
    f.game.dump(kBartel, 1, {});
    f.game.look(kBartel);
    f.drain(tailer);
    CHECK_FALSE(f.session.talking());
    CHECK(f.said("looked at Bartel (kkut_additive_man_47), a first meeting; nothing is written for them"));
    CHECK(f.session.has_speculation());
    const std::size_t calls = f.llm.calls.size();

    // One call, the very one a begin would make: the sheet, no history, the approach.
    f.llm.replies = {"Hm? What is it?"};
    CHECK_FALSE(f.session.speculate([] { return false; }));
    CHECK(f.llm.calls.size() == calls + 1);
    CHECK(f.llm.last().messages.size() == 2);
    CHECK(f.llm.last().messages.back().content == kApproached);
    CHECK(f.said("guessed Bartel's opening line ahead, "));
    CHECK_FALSE(f.session.has_speculation());
    // Nothing reached the game.
    CHECK_FALSE(f.game.has_turn(1));

    // The press: the same dump, the same sheet, and the line goes out with no call at all.
    // The prefill the written path pays after the greeting is not paid either, since the
    // guess left the sheet in the cache.
    f.game.dump(kBartel, 1, {});
    f.game.begin(kBartel);
    f.drain(tailer);
    CHECK(f.session.talking());
    CHECK(f.said("their opening line was guessed while the player looked at them, and is said at once"));
    CHECK(f.llm.calls.size() == calls + 1);
    REQUIRE(f.session.history().size() == 1);
    CHECK(f.session.history()[0].content == "Hm? What is it?");
    CHECK(f.game.has_turn(1));
    REQUIRE(!timings.empty());
    CHECK(timings.back().guessed);
    CHECK(f.session.guesses_used() == 1);
    CHECK(f.session.guesses_wasted() == 0);
    f.play_all(tailer);
    CHECK(f.game.menu().size() == 2);
}

TEST_CASE("a look writes the opening list ahead, so the menu is in the inbox before the press") {
    Fixture f;
    f.session.set_speculation(true);
    looks_at(f, "What do you want?", true);
    LogTailer tailer(f.game.log());
    f.game.launch();

    f.game.dump(kBartel, 1, {});
    f.game.look(kBartel);
    f.drain(tailer);
    CHECK_FALSE(f.session.talking());
    CHECK_FALSE(f.game.has_turn(1));
    CHECK(f.game.has_turn(2));
    CHECK(f.game.whole(2).find("@opt ") != std::string::npos);
    CHECK(f.game.whole(2).find("@rows ") != std::string::npos);
    CHECK(f.said("opening rows written ahead for Bartel"));
}

TEST_CASE("a guessed opening line is set aside when the press is not the moment it was guessed for") {
    Fixture f;
    f.session.set_speculation(true);
    looks_at(f, "", false);
    LogTailer tailer(f.game.log());
    f.game.launch();

    f.game.dump(kBartel, 1, {});
    f.game.look(kBartel);
    f.drain(tailer);
    f.llm.replies = {"Hm? What is it?"};
    f.session.speculate([] { return false; });
    const std::size_t calls = f.llm.calls.size();

    // Looked at as a stranger, pressed as somebody met before: another sheet, so the guess
    // would be the wrong man's words. It is set aside, and the line is generated as it
    // always was; the kept line is not wasted until nobody comes back for it.
    f.llm.replies = {"You again."};
    f.game.dump(kBartel, 2, {});
    f.game.begin(kBartel);
    f.drain(tailer);
    CHECK(f.said("the opening line guessed for Bartel no longer fits, and is kept for a later look"));
    CHECK(f.llm.calls.size() == calls + 1);
    REQUIRE(f.session.history().size() == 1);
    CHECK(f.session.history()[0].content == "You again.");
    CHECK(f.session.guesses_used() == 0);
    CHECK(f.session.guesses_wasted() == 0);
}

TEST_CASE("a finished guess is kept when the look moves on, and comes back without a call when the look does") {
    Fixture f;
    f.session.set_speculation(true);
    looks_at(f, "", false);
    LogTailer tailer(f.game.log());
    f.game.launch();
    const std::string kOther = "kkut_additive_man_45";

    // Bartel's line, guessed and finished.
    f.game.dump(kBartel, 1, {});
    f.game.look(kBartel);
    f.drain(tailer);
    f.llm.replies = {"Hm? What is it?"};
    CHECK_FALSE(f.session.speculate([] { return false; }));
    CHECK(f.said("guessed Bartel's opening line ahead, "));
    const std::size_t calls = f.llm.calls.size();

    // The look goes to his neighbour: Bartel's line is kept, the neighbour's is guessed.
    f.game.dump(kOther, 1, {});
    f.game.look(kOther);
    f.drain(tailer);
    f.llm.replies = {"Aye?"};
    CHECK_FALSE(f.session.speculate([] { return false; }));
    CHECK(f.llm.calls.size() == calls + 1);

    // And back, and forth: neither line is made again.
    f.game.dump(kBartel, 1, {});
    f.game.look(kBartel);
    f.drain(tailer);
    CHECK(f.said("looked at Bartel (kkut_additive_man_47), a first meeting; their opening line was guessed on an earlier look, and is kept"));
    CHECK_FALSE(f.session.has_speculation());
    f.game.dump(kOther, 1, {});
    f.game.look(kOther);
    f.drain(tailer);
    CHECK_FALSE(f.session.has_speculation());
    f.game.dump(kBartel, 1, {});
    f.game.look(kBartel);
    f.drain(tailer);
    CHECK(f.llm.calls.size() == calls + 1);
    CHECK(f.session.guesses_wasted() == 0);

    // The press says the kept line, with no call.
    f.game.dump(kBartel, 1, {});
    f.game.begin(kBartel);
    f.drain(tailer);
    CHECK(f.said("their opening line was guessed while the player looked at them, and is said at once"));
    CHECK(f.llm.calls.size() == calls + 1);
    REQUIRE(f.session.history().size() == 1);
    CHECK(f.session.history()[0].content == "Hm? What is it?");
    CHECK(f.session.guesses_used() == 1);
}

TEST_CASE("the written greeting goes to the voice to make ahead, at the look and again as it is written") {
    Fixture f;
    f.session.set_speculation(true);
    // Two beats, in the actor the soul speaks in.
    f.session.set_persona_source(
        [](std::string_view, const Approach&, std::string_view, const SoulRecord&) -> std::optional<Persona> {
            Persona persona{"Bartel", "You are Bartel, a varlet in Kuttenberg.",
                            "Hm.\n\nWhat is it\nyou want?"};
            persona.actor = "pete-male";
            return persona;
        });
    LogTailer tailer(f.game.log());
    f.game.launch();

    // At the look, once it has lasted (speculate, the dwell being nothing here): both
    // beats, in order, as they will be shown, before any press and whether or not the line
    // is also being guessed (it is: the written line knows nothing of the state they are
    // met in). The voice goes first; the guess is still there to make after it.
    f.game.dump(kBartel, 1, {});
    f.game.look(kBartel);
    f.drain(tailer);
    CHECK(f.prepared.empty());
    REQUIRE(f.session.has_speculation());
    CHECK(f.session.speculate([] { return false; }));
    REQUIRE(f.prepared.size() == 2);
    CHECK(f.prepared[0].first == "Hm.");
    CHECK(f.prepared[0].second == "pete-male");
    CHECK(f.prepared[1].first == "What is it\nyou want?");
    CHECK(f.session.has_speculation());
    // Looked at again: nothing new to make.
    f.game.dump(kBartel, 1, {});
    f.game.look(kBartel);
    f.drain(tailer);
    CHECK(f.prepared.size() == 2);
    f.prepared.clear();

    // The press, with the guess still unmade: the written line is said and asked for again
    // as it is written, which the voice knows it has.
    f.llm.replies = {""};
    f.game.begin(kBartel);
    f.drain(tailer);
    REQUIRE(f.prepared.size() == 2);
    CHECK(f.prepared[0].first == "Hm.");
    CHECK(f.prepared[1].second == "pete-male");
    REQUIRE(f.session.history().size() == 1);
    CHECK(f.session.history()[0].content == "Hm.\n\nWhat is it\nyou want?");
}

TEST_CASE("a soul with nothing written has nothing made ahead") {
    Fixture f;
    looks_at(f, "", false);
    LogTailer tailer(f.game.log());
    f.game.launch();
    f.game.dump(kBartel, 1, {});
    f.game.look(kBartel);
    f.drain(tailer);
    CHECK_FALSE(f.session.has_speculation());
    CHECK_FALSE(f.session.speculate([] { return false; }));
    CHECK(f.prepared.empty());
}

TEST_CASE("a look starts nothing until it has lasted, and a look at somebody else drops what the one before asked for") {
    // 14 Oct 2026: walking through a crowd, six souls looked at in twelve seconds each
    // started the voice on a greeting beside the game, and the GPU driver reset.
    Fixture f;
    f.session.set_speculation(true);
    looks_at(f, "Hm.\n\nWhat is it\nyou want?", false);
    LogTailer tailer(f.game.log());
    f.game.launch();

    // A dwell the test will not outlast: the look is planned, the opening list is written
    // (that is only a file), and nothing is made or guessed -- speculate() says it has
    // nothing to do, and the poller comes back later.
    f.session.set_look_dwell(std::chrono::minutes(10));
    f.game.dump(kBartel, 1, {});
    f.game.look(kBartel);
    f.drain(tailer);
    CHECK(f.said("opening rows written ahead for Bartel"));
    CHECK(f.session.has_speculation());
    CHECK_FALSE(f.session.speculate([] { return false; }));
    CHECK(f.prepared.empty());
    CHECK(f.llm.calls.empty());
    CHECK(f.dropped == 0);

    // The look moves on before the dwell is out: the first soul's look-ahead is replaced,
    // and the voice is told, in case it had started on it.
    f.game.dump("kkut_additive_man_45", 1, {});
    f.game.look("kkut_additive_man_45");
    f.drain(tailer);
    CHECK(f.dropped == 1);
    CHECK(f.prepared.empty());
    // The same soul looked at again does not start the dwell over, nor drop anything.
    f.game.dump("kkut_additive_man_45", 1, {});
    f.game.look("kkut_additive_man_45");
    f.drain(tailer);
    CHECK(f.dropped == 1);

    // The look has lasted (the dwell made nothing, for the test): the voice is started on
    // the beats first, then the model on the line.
    f.session.set_look_dwell(std::chrono::milliseconds(0));
    f.game.dump(kBartel, 1, {});
    f.game.look(kBartel);
    f.drain(tailer);
    CHECK(f.dropped == 2);
    CHECK(f.session.speculate([] { return false; }));
    REQUIRE(f.prepared.size() == 2);
    CHECK(f.prepared[0].first == "Hm.");
    CHECK(f.said("has lasted, so their greeting is being made ahead"));
    CHECK(f.llm.calls.empty());
    f.llm.replies = {"Hm? What is it?"};
    CHECK_FALSE(f.session.speculate([] { return false; }));
    CHECK(f.said("guessed Bartel's opening line ahead"));

    // A press on somebody other than the one looked at drops that one's greeting too.
    f.prepared.clear();
    f.llm.replies = {"Aye?", ""};
    f.game.dump("kkut_additive_man_45", 1, {});
    f.game.begin("kkut_additive_man_45");
    f.drain(tailer);
    CHECK(f.dropped == 3);
    CHECK(f.session.talking());
}

TEST_CASE("a look at somebody else takes the guess over, and a guess gives way within a token") {
    Fixture f;
    f.session.set_speculation(true);
    looks_at(f, "", false);
    LogTailer tailer(f.game.log());
    f.game.launch();

    f.game.dump(kBartel, 1, {});
    f.game.look(kBartel);
    f.drain(tailer);
    // Interrupted on the first piece: the log has a line, say. Nothing is kept of the reply,
    // and the guess stands to be made again.
    f.llm.replies = {"Hm? What is it?"};
    int pieces = 0;
    CHECK(f.session.speculate([&pieces] { return ++pieces > 0; }));
    CHECK(pieces == 1);
    CHECK(f.session.has_speculation());
    CHECK_FALSE(f.said("guessed Bartel's opening line ahead"));

    // The line that interrupted it was a look at another soul, whose line is the one wanted
    // now. The same soul again in the same state would have changed nothing.
    f.game.dump("kkut_additive_man_45", 1, {});
    f.game.look("kkut_additive_man_45");
    f.drain(tailer);
    CHECK(f.said("looked at Bartel (kkut_additive_man_45)"));
    f.llm.replies = {"Aye?"};
    CHECK_FALSE(f.session.speculate([] { return false; }));
    CHECK(f.said("guessed Bartel's opening line ahead, "));
    const std::size_t calls = f.llm.calls.size();
    f.game.dump("kkut_additive_man_45", 1, {});
    f.game.look("kkut_additive_man_45");
    f.drain(tailer);
    CHECK_FALSE(f.session.has_speculation());
    CHECK(f.llm.calls.size() == calls);

    // Pressed while a fresh guess is still being written: not waited for, generated now.
    f.game.dump(kBartel, 1, {});
    f.game.look(kBartel);
    f.drain(tailer);
    // The other man's finished line is kept for a later look, not wasted.
    CHECK(f.session.guesses_wasted() == 0);
    f.llm.replies = {"Hm?"};
    CHECK(f.session.speculate([] { return true; }));
    f.llm.replies = {"Hm? What is it?"};
    f.game.dump(kBartel, 1, {});
    f.game.begin(kBartel);
    f.drain(tailer);
    CHECK(f.said("was still being guessed when the player pressed"));
    CHECK(f.llm.calls.size() == calls + 2);
    REQUIRE(f.session.history().size() == 1);
    CHECK(f.session.history()[0].content == "Hm? What is it?");
}

TEST_CASE("a soul with a line written for the moment is not guessed; the look reads their sheet in instead") {
    Fixture f;
    f.session.set_speculation(true);
    looks_at(f, "What do you want?", true);
    LogTailer tailer(f.game.log());
    f.game.launch();

    f.game.dump(kBartel, 1, {});
    f.game.look(kBartel);
    f.drain(tailer);
    CHECK(f.said("a line is written for this, so only their sheet is read in ahead"));
    CHECK(f.session.has_speculation());
    const std::size_t calls = f.llm.calls.size();
    // The voice first, on the written line, with the sheet still to read.
    CHECK(f.session.speculate([] { return false; }));
    REQUIRE(f.prepared.size() == 1);
    CHECK(f.prepared[0].first == "What do you want?");
    CHECK(f.llm.calls.size() == calls);
    // Then the prefill and nothing more: one token asked for, against the sheet.
    f.llm.replies = {""};
    CHECK_FALSE(f.session.speculate([] { return false; }));
    CHECK(f.llm.calls.size() == calls + 1);
    CHECK(f.llm.last().options.reply_tokens == 1);
    CHECK(f.said("Bartel's sheet is read in"));

    // On the press the written line is said, and the sheet is not read in again: no call
    // is made at all.
    f.game.dump(kBartel, 1, {});
    f.game.begin(kBartel);
    f.drain(tailer);
    CHECK(f.llm.calls.size() == calls + 1);
    REQUIRE(f.session.history().size() == 1);
    CHECK(f.session.history()[0].content == "What do you want?");
    CHECK(f.session.guesses_used() == 0);
    CHECK(f.session.guesses_wasted() == 0);

    // The soul's general line knows nothing of the state they are met in, so it is not exact,
    // and the model is asked for a better one; the written line is what would have been said.
    f.game.left();
    f.drain(tailer);
    looks_at(f, "What do you want?", false);
    f.game.dump(kBartel, 2, {});
    f.game.look(kBartel);
    f.drain(tailer);
    CHECK(f.said("the written line knows nothing of the state they are met in, so their opening line is guessed ahead"));
}

TEST_CASE("with guessing off, a look still writes the opening list, but does not guess a line") {
    Fixture f;
    looks_at(f, "", false);
    LogTailer tailer(f.game.log());
    f.game.launch();
    f.game.dump(kBartel, 1, {});
    f.game.look(kBartel);
    f.drain(tailer);
    CHECK_FALSE(f.session.has_speculation());
    CHECK_FALSE(f.session.speculate([] { return false; }));
    CHECK(f.llm.calls.empty());
    CHECK(f.game.has_turn(2));
    CHECK(f.said("opening rows written ahead for Bartel"));
}

TEST_CASE("the truth told, the reminder goes and the giver's next scene is there to ask in the same conversation") {
    // Sweepings and Strays (notes.md 8 Oct 2026): Anna's A Notched Ear follows it once the
    // truth is told, but the plan is fixed when a conversation opens, so the hand-in left
    // "Remind me what it is you need of me." on the list and the second scene waited for
    // Henry to happen back -- "just talk and return". Now the ending replans the
    // conversation: the reminder goes with the errand, the next scene's offer is said in
    // the same breath as the thanks (a second "Is there anything I can help with?" was a
    // hand-over "easy to miss"), and its accepting row goes first, the yes owed on its own
    // id.
    Quest street;
    street.id = "0e3817706672";
    street.kind = "ask";
    street.role = QuestRole::Giver;
    street.giver_soul = kBartel;
    street.giver_name = "Anna";
    street.target_soul = "kkut_woman_49";
    street.target_name = "Matilda";
    street.item_name = "word";
    street.reward_groschen = 14;
    street.offer = "Matilda feeds every stray and must know something. Go find out what she's hiding.";
    street.thanks = "The bailiff's house? Sakra, I knew there was devilry afoot! Here are a few groschen.";
    // Written for leading on: said in `thanks`'s place on the row, as on the greeting.
    street.thanks_onward = "The bailiff's house? Sakra, I knew it! Here are a few groschen -- and wait.";
    street.tell = "Matilda saw Kunigunde shut the cat inside the bailiff's house.";
    street.lie = "Matilda hasn't seen your cat at all.";
    street.quiet = "I know what happened to the cats, but I'm keeping it to myself.";
    Quest notched = street;
    notched.id = "37214dc9cf7b";
    notched.name = "A Notched Ear";
    notched.target_soul = "kkut_woman_226";
    notched.target_name = "Kunigunde";
    notched.reward_groschen = 0;
    notched.after_quest_id = street.id;
    notched.after_how = "truth";
    notched.offer = "Kunigunde claims she has an old pet. Get the truth, and I'll give you a hot supper.";
    notched.offer_2 = "The bailiff's house is in the marketplace, right by the tavern.";
    notched.tell = "It's a stray with a notched ear that came in the spring.";
    notched.lie = "She says she's had the cat for years.";
    notched.quiet = "I found out about the cat, but I'm keeping it to myself.";

    Fixture f(false);
    f.session.set_persona_source(
        [&](std::string_view, const Approach& approach, std::string_view, const SoulRecord& record)
            -> std::optional<Persona> {
            // As persona_for reads the save: the first scene handed and not done until the
            // record says done, and the second locked until then.
            const Standing standing = standing_of(record);
            Persona persona{"Anna", "You are Anna, a housekeeper in Kuttenberg.", "Did Matilda confess?"};
            QuestFacts first = standing.facts_of(street.id);
            if (!first.done) first.accepted = first.handed = true;
            QuestFacts second = standing.facts_of(notched.id);
            second.locked = !first.done;
            second.voided = first.done && !how_matches(first.how, notched.after_how);
            const bool next = first.done && !second.locked && !second.voided;
            persona.quest_of = next ? notched : street;
            persona.quest = plan_quest(persona.quest_of, next ? second : first, approach);
            persona.scripted = plan_scripted("", {}, {}, persona.quest_of, next ? second : first, standing,
                                             {street, notched}, {first, second});
            return persona;
        });
    LogTailer tailer(f.game.log());
    f.game.launch();
    f.open(tailer);
    f.play_all(tailer);
    // The hand-in's list: the three endings first, then the reminder in the offer of
    // help's place.
    REQUIRE(f.game.menu().size() == 4);
    CHECK(f.game.menu()[0] == street.tell);
    CHECK(f.game.menu()[3] == kRemindRow);

    const std::size_t calls = f.llm.calls.size();
    f.game.picked(2, "tell", street.tell);
    f.drain(tailer);
    CHECK(f.said("picked as tell, answered with the writer's answer to tell as written"));
    CHECK(f.said("A Notched Ear (37214dc9cf7b) to ask next: its offer is said with the thanks"));
    REQUIRE(f.recorded.size() == 1);
    CHECK(f.recorded[0] == std::make_pair(street.id, std::string(kQuestComplete)));
    // One reply in two beats: the thanks, then the next scene's offer; the complete owed
    // rides on the first, on the errand that ended.
    CHECK(f.session.history().back().content == street.thanks_onward + "\n\n" + notched.offer);
    CHECK(f.llm.calls.size() == calls);
    // The record now says the first is done, as the mod's next dump would.
    CHECK(standing_of(f.session.record()).facts_of(street.id).done);
    f.play_all(tailer);
    // The list after: the accepting row first, no reminder, none of the endings again, and
    // no offer of help -- the offer has been made.
    REQUIRE_FALSE(f.game.menu().empty());
    CHECK(f.game.menu()[0] == "I'll go to Kunigunde and get the truth of it for you.");
    for (const std::string& row : f.game.menu()) {
        CHECK(row != kRemindRow);
        CHECK(row != kFirstRoundSeeds[0]);
        CHECK(row != street.tell);
        CHECK(row != street.lie);
        CHECK(row != street.quiet);
    }
    f.game.picked(1, "accept", f.game.menu()[0]);
    f.drain(tailer);
    CHECK(f.said("picked as accept, answered with the second telling of the offer as written"));
    REQUIRE(f.recorded.size() == 2);
    CHECK(f.recorded[1] == std::make_pair(notched.id, std::string(kQuestAccept)));
    CHECK(f.session.history().back().content == notched.offer_2);
    f.play_all(tailer);
    // And the second errand taken up closes the conversation, as the first's would have.
    CHECK(f.game.directives().back() == "@end");
}

TEST_CASE("an errand paid off on the greeting leads on to the giver's next scene the same way") {
    // notes.md (9 Oct 2026): Benesh saw his chalice in Henry's hands and paid up on the
    // greeting, and the letter for Father Adam -- his own next scene -- was never offered:
    // the list under his thanks was the done plan's. The greeting is a hand-in like the
    // row: the thanks and the next offer go out as one reply in two beats, the complete
    // owed on the errand that ended, and the accepting row goes first.
    Quest chalice;
    chalice.id = "979c4fccd41b";
    chalice.kind = "retrieve";
    chalice.role = QuestRole::Giver;
    chalice.giver_soul = kBartel;
    chalice.giver_name = "Benesh";
    chalice.target_soul = "kkut_extras_man_24";
    chalice.target_name = "Paul Schmidt";
    chalice.item_class = "6dc80a04-dad0-4259-a854-e085caa74cc1";
    chalice.item_name = "silver chalice";
    chalice.reward_groschen = 20;
    chalice.offer = "Paul Schmidt has the parish cup in his chest. Get it back and clear my name.";
    chalice.thanks = "The cup! Saints be praised. Here, twenty groschen, and my thanks.";
    // The thanks written for leading on (notes.md 13 Oct 2026: the ones that do not close
    // anything), said in place of `thanks` where the next scene follows.
    chalice.thanks_onward = "Christ almighty -- that's it. The parish cup, the dent in the foot and all.";
    Quest letter = chalice;
    letter.thanks_onward.clear();
    letter.id = "895f926a2b54";
    letter.kind = "deliver";
    letter.name = "The Bell-Ringer's Due";
    letter.target_soul = "kkut_additive_man_135";
    letter.target_name = "Father Adam";
    letter.item_class = "8391c83b-f6e6-5bb0-868f-fbeb43852b6b";
    letter.item_name = "letter";
    letter.reward_groschen = 0;
    letter.after_quest_id = chalice.id;
    letter.after_how = "";
    letter.offer = "Take this letter to Father Adam. It names the sacristan for what he is.";
    letter.offer_2 = "He paces behind the church most hours.";

    Fixture f(false);
    f.session.set_persona_source(
        [&](std::string_view, const Approach& approach, std::string_view, const SoulRecord& record)
            -> std::optional<Persona> {
            const Standing standing = standing_of(record);
            Persona persona{"Benesh", "You are Benesh, a bellringer in Kuttenberg.", "Have you got it?"};
            QuestFacts first = standing.facts_of(chalice.id);
            if (!first.done) {
                first.accepted = first.handed = true;
                first.have = approach.have;
            }
            QuestFacts second = standing.facts_of(letter.id);
            second.locked = !first.done;
            const bool next = first.done && !second.locked;
            persona.quest_of = next ? letter : chalice;
            persona.quest = plan_quest(persona.quest_of, next ? second : first, approach);
            persona.scripted = plan_scripted("", {}, {}, persona.quest_of, next ? second : first, standing,
                                             {chalice, letter}, {first, second});
            return persona;
        });
    LogTailer tailer(f.game.log());
    f.game.launch();
    f.game.begin_with(kBartel, " have=1");
    f.drain(tailer);
    CHECK(f.said("the quest settles on this turn, on the line written for it"));
    CHECK(f.said("The Bell-Ringer's Due (895f926a2b54) to ask next: its offer is said with the thanks"));
    REQUIRE(f.recorded.size() == 1);
    CHECK(f.recorded[0] == std::make_pair(chalice.id, std::string(kQuestComplete)));
    CHECK(f.session.history().back().content == chalice.thanks_onward + "\n\n" + letter.offer);
    CHECK(standing_of(f.session.record()).facts_of(chalice.id).done);
    f.play_all(tailer);
    // The directive the mod reads names the errand that ended, not the one adopted: the
    // first pass in the game (9 Oct 2026) completed the letter before it was accepted.
    bool completed_chalice = false, completed_letter = false;
    for (const std::string& directive : f.game.directives()) {
        if (directive.find("@quest " + chalice.id + " complete") == 0) completed_chalice = true;
        if (directive.find("@quest " + letter.id + " ") == 0) completed_letter = true;
    }
    CHECK(completed_chalice);
    CHECK_FALSE(completed_letter);
    // The list under the thanks: the letter's accepting row first, no reminder, no offer of help.
    REQUIRE_FALSE(f.game.menu().empty());
    CHECK(f.offered[0].intent == "accept");
    for (const std::string& row : f.game.menu()) {
        CHECK(row != kRemindRow);
        CHECK(row != kFirstRoundSeeds[0]);
    }
    f.game.picked(1, "accept", f.game.menu()[0]);
    f.drain(tailer);
    REQUIRE(f.recorded.size() == 2);
    CHECK(f.recorded[1] == std::make_pair(letter.id, std::string(kQuestAccept)));
    CHECK(f.session.history().back().content == letter.offer_2);
}

TEST_CASE("a letter's writer may be asked what is in it, and answers by Henry's Speech against his own") {
    // notes.md (9 Oct 2026, Holy Jest): Henry should be able to ask Benesh what the letter
    // says, and get it out of him on a speech check. The row is laid while the letter is in
    // Henry's hand and unhanded; the check is the app's, from the two Speech stats on the
    // begin line, the game's own way (his over the writer's by a factor: 1.25 at medium);
    // the mod is told the result for the game's mark.
    auto writer_of = [](Fixture& f, const std::string& difficulty) {
        f.session.set_persona_source(
            [difficulty](std::string_view, const Approach& approach, std::string_view, const SoulRecord& record)
                -> std::optional<Persona> {
                QuestFacts facts = standing_of(record).facts_of("1b754e7e1163");
                if (!facts.done) facts.accepted = true;
                Persona persona = written_bartel(first_meeting(), facts);
                persona.quest_of.id = "1b754e7e1163";
                persona.quest_of.kind = "deliver";
                persona.quest_of.difficulty = difficulty;
                persona.quest_of.item_class = "8391c83b-f6e6-5bb0-868f-fbeb43852b6b";
                persona.quest_of.item_name = "letter";
                persona.quest_of.target_name = "Brother Thomas";
                persona.quest_of.pry = "What's in the letter, Benesh?";
                persona.quest_of.pry_yes = "Lord forgive me. It asks whether a priest may jest at mass.";
                persona.quest_of.pry_no = "A matter of church law, for the Abbot's eyes and nobody else's.";
                persona.quest = plan_quest(persona.quest_of, facts, approach);
                persona.scripted = plan_scripted("Bartel. I dig where I'm told.", {}, {}, persona.quest_of, facts,
                                                 first_meeting());
                return persona;
            });
    };
    SUBCASE("won: Henry's 15 over the writer's 10 at medium, told as written, @check speech pass") {
        Fixture f(false);
        writer_of(f, "medium");
        LogTailer tailer(f.game.log());
        f.game.launch();
        f.game.begin_with(kBartel, " spc=15 nspc=10 have=1");
        f.drain(tailer);
        f.play_all(tailer);
        REQUIRE(f.game.menu().size() >= 2);
        bool laid = false;
        int at = 0;
        for (std::size_t i = 0; i < f.offered.size(); ++i) {
            if (f.offered[i].intent == "pry") { laid = true; at = static_cast<int>(i) + 1; }
        }
        REQUIRE(laid);
        const std::size_t calls = f.llm.calls.size();
        f.game.picked(at, "pry", "What's in the letter, Benesh?");
        f.drain(tailer);
        CHECK(f.said("picked as pry, answered with the letter's matter, told after a speech check won (speech 15 against 10) as written"));
        CHECK(f.llm.calls.size() == calls);
        f.play_all(tailer);
        CHECK(f.session.history().back().content == "Lord forgive me. It asks whether a priest may jest at mass.");
        bool told = false;
        for (const std::string& directive : f.game.directives()) told = told || directive == "@check speech pass";
        CHECK(told);
        // Once: the row does not come back.
        for (const std::string& row : f.game.menu()) CHECK(row != "What's in the letter, Benesh?");
    }
    SUBCASE("lost: 12 against 10 at medium is under the factor, refused, @check speech fail") {
        Fixture f(false);
        writer_of(f, "medium");
        LogTailer tailer(f.game.log());
        f.game.launch();
        f.game.begin_with(kBartel, " spc=12 nspc=10 have=1");
        f.drain(tailer);
        f.play_all(tailer);
        int at = 0;
        for (std::size_t i = 0; i < f.offered.size(); ++i) {
            if (f.offered[i].intent == "pry") at = static_cast<int>(i) + 1;
        }
        REQUIRE(at > 0);
        f.game.picked(at, "pry", "What's in the letter, Benesh?");
        f.drain(tailer);
        CHECK(f.said("picked as pry, answered with a refusal, the speech check lost (speech 12 against 10) as written"));
        f.play_all(tailer);
        CHECK(f.session.history().back().content == "A matter of church law, for the Abbot's eyes and nobody else's.");
        bool told = false;
        for (const std::string& directive : f.game.directives()) told = told || directive == "@check speech fail";
        CHECK(told);
    }
    SUBCASE("easy takes any edge; unread stats fail, and the log says unread") {
        Fixture f(false);
        writer_of(f, "easy");
        LogTailer tailer(f.game.log());
        f.game.launch();
        f.game.begin_with(kBartel, " have=1");
        f.drain(tailer);
        f.play_all(tailer);
        int at = 0;
        for (std::size_t i = 0; i < f.offered.size(); ++i) {
            if (f.offered[i].intent == "pry") at = static_cast<int>(i) + 1;
        }
        REQUIRE(at > 0);
        f.game.picked(at, "pry", "What's in the letter, Benesh?");
        f.drain(tailer);
        CHECK(f.said("the speech check lost (speech -1 against -1, unread) as written"));
    }
}

TEST_CASE("a letter's writer may be asked what is in it before Henry agrees to carry it") {
    // notes.md (9 Oct 2026, Holy Jest): the pry is not gated on holding the letter. From
    // the offer on, the row is laid under the accepting row; asked, it is answered on the
    // check and the accepting row is laid again, the offer still open.
    Fixture f(false);
    f.session.set_persona_source(
        [](std::string_view, const Approach& approach, std::string_view, const SoulRecord& record)
            -> std::optional<Persona> {
            QuestFacts facts = standing_of(record).facts_of("1b754e7e1163");
            Persona persona = written_bartel(first_meeting(), facts);
            persona.quest_of.id = "1b754e7e1163";
            persona.quest_of.kind = "deliver";
            persona.quest_of.difficulty = "medium";
            persona.quest_of.item_class = "8391c83b-f6e6-5bb0-868f-fbeb43852b6b";
            persona.quest_of.item_name = "letter";
            persona.quest_of.target_name = "Brother Thomas";
            persona.quest_of.pry = "What's in the letter, Benesh?";
            persona.quest_of.pry_yes = "Lord forgive me. It asks whether a priest may jest at mass.";
            persona.quest_of.pry_no = "A matter of church law, for the Abbot's eyes and nobody else's.";
            persona.quest = plan_quest(persona.quest_of, facts, approach);
            persona.scripted = plan_scripted("Bartel. I dig where I'm told.", {}, {}, persona.quest_of, facts,
                                             first_meeting());
            return persona;
        });
    LogTailer tailer(f.game.log());
    f.game.launch();
    f.game.begin_with(kBartel, " spc=15 nspc=10 have=0");
    f.drain(tailer);
    f.play_all(tailer);
    // Not on the opening list: the letter has not been mentioned yet.
    for (const std::string& row : f.game.menu()) CHECK(row != "What's in the letter, Benesh?");
    f.game.picked(1, "help", std::string(kFirstRoundSeeds[0]));
    f.drain(tailer);
    f.play_all(tailer);
    REQUIRE(f.game.menu().size() >= 2);
    CHECK(f.game.menu()[0] == "I'll take your letter to Brother Thomas.");
    CHECK(f.game.menu()[1] == "What's in the letter, Benesh?");
    const std::size_t calls = f.llm.calls.size();
    f.game.picked(2, "pry", "What's in the letter, Benesh?");
    f.drain(tailer);
    CHECK(f.said("picked as pry, answered with the letter's matter, told after a speech check won (speech 15 against 10) as written"));
    CHECK(f.llm.calls.size() == calls);
    CHECK(f.recorded.empty());
    CHECK(f.session.history().back().content == "Lord forgive me. It asks whether a priest may jest at mass.");
    f.play_all(tailer);
    // Told, the accepting row leads the list again and the question is gone.
    REQUIRE_FALSE(f.game.menu().empty());
    CHECK(f.game.menu()[0] == "I'll take your letter to Brother Thomas.");
    for (const std::string& row : f.game.menu()) CHECK(row != "What's in the letter, Benesh?");
    f.game.picked(1, "accept", "I'll take your letter to Brother Thomas.");
    f.drain(tailer);
    REQUIRE(f.recorded.size() == 1);
    CHECK(f.recorded[0].second == kQuestAccept);
}

TEST_CASE("an errand settled with no scene to follow is planned as done for the rest of the conversation") {
    // The lie told to a giver with nothing after it: the reminder goes, the endings go, and
    // the plan is the done one -- no yes listened for, nothing to lead to.
    Fixture f(false);
    f.session.set_persona_source(
        [](std::string_view, const Approach& approach, std::string_view, const SoulRecord& record)
            -> std::optional<Persona> {
            QuestFacts facts = standing_of(record).facts_of("c00183e1da7e");
            if (!facts.done) facts.accepted = true;
            Persona persona = written_bartel(first_meeting(), facts);
            persona.quest_of.kind = "ask";
            persona.quest_of.item_name = "word";
            persona.quest_of.lie = "Julian never had your axe.";
            persona.quest_of.lie_reply = "Then it's lost. Here, for the walk.";
            persona.quest = plan_quest(persona.quest_of, facts, approach);
            persona.scripted = plan_scripted("Bartel. I dig where I'm told.", {}, {}, persona.quest_of, facts,
                                             first_meeting());
            return persona;
        });
    LogTailer tailer(f.game.log());
    f.game.launch();
    f.open(tailer);
    f.play_all(tailer);
    REQUIRE(f.game.menu().size() >= 2);
    CHECK(f.game.menu()[0] == "Julian never had your axe.");
    f.game.picked(1, "lie", "Julian never had your axe.");
    f.drain(tailer);
    CHECK(f.said("the errand is settled, and the rest of the conversation is planned as after it"));
    REQUIRE(f.recorded.size() == 1);
    CHECK(f.recorded[0].second == kQuestLie);
    CHECK(standing_of(f.session.record()).facts_of("c00183e1da7e").how == "lied");
    // Nothing leads on, so his answer closes the talk (notes.md 23 Oct 2026): the turn
    // flagged @end and no list offered after it. The menu the game shows is the one from
    // before the pick; the session's own rows are empty.
    CHECK(f.said("the errand is taken, so the conversation closes and no rows are offered"));
    f.play_all(tailer);
    bool ended = false;
    for (const std::string& directive : f.game.directives()) {
        if (directive == "@end") ended = true;
    }
    CHECK(ended);
    CHECK(f.session.rows().empty());
    CHECK(f.session.persona().quest.question.empty());
}

namespace {

// Words Must Not Fail: Christine's prayer, written at her dictation (quest.h, Quest::Dictation).
Quest christines_prayer() {
    Quest quest;
    quest.id = "b2cc2651365c";
    quest.kind = "deliver";
    quest.role = QuestRole::Giver;
    quest.giver_soul = "kkut_woman_223";
    quest.giver_name = "Christine";
    quest.target_soul = "kkut_additive_man_138";
    quest.target_name = "Father Ernest";
    quest.item_class = "8116ce80-e730-5666-a9fd-cf6ee1e357b2";
    quest.item_name = "letter";
    quest.reward_text = "her thanks, and a straight answer to whatever Henry asks";
    quest.skill = "persuade";
    quest.difficulty = "easy";
    quest.offer = "Father Ernest wants my prayer in ink. I can't write. Can you?";
    quest.agreed = "You'll find Ernest by the church, a short stroll towards the market.";
    quest.thanks = "Then it is said. You have my thanks.";
    quest.greet_owed = "Have you seen Father Ernest yet?";
    quest.report = "Your letter is with Father Ernest.";
    Quest::Dictation& d = quest.dictation;
    d.begin = "Sit there, at the master's desk. Write what I say and nothing else.";
    d.done = "There. Sign it, fold it, and put the wax to it. Now take it to him.";
    d.resume = "Let's get your prayer written.";
    d.greet_unwritten = "You're back. Sit, and we'll get it written.";
    for (int k = 0; k < Quest::Dictation::kParts; ++k) {
        Quest::Dictation::Part& part = d.parts[k];
        const std::string n = std::to_string(k + 1);
        part.dictate = "Write this, part " + n + ".";
        part.write = {"I'll write part " + n + " as you say it.", "Good. Every word."};
        part.soften = {"I'll put part " + n + " more gently.", "Fine. Write it so."};
        part.embellish = {"Let me give part " + n + " some weight.", "Yes. Write that."};
        if (k > 0) part.reconsider = {"Do you want part " + n + " in a prayer?", "No. Leave it out."};
    }
    return quest;
}

}  // namespace

TEST_CASE("a letter written at dictation is taken up, dictated a part at a time with Henry's ways with each, and sealed") {
    Fixture f(false);
    f.session.set_persona_source(
        [](std::string_view, const Approach& approach, std::string_view, const SoulRecord&) -> std::optional<Persona> {
            Persona persona{"Christine", "You are Christine, a housekeeper.", "What do you want?"};
            const Quest quest = christines_prayer();
            persona.quest_of = quest;
            persona.quest = plan_quest(quest, {}, approach);
            persona.scripted = plan_scripted("", {}, {}, quest, {}, first_meeting());
            return persona;
        });
    LogTailer tailer(f.game.log());
    f.game.launch();
    f.open(tailer);
    f.play_all(tailer);
    f.game.picked(1, "help", std::string(kFirstRoundSeeds[0]));
    f.drain(tailer);
    f.play_all(tailer);
    // The accepting row says what Henry is taking on: the writing as well as the carrying.
    REQUIRE(f.game.menu()[0] == "I'll write your letter down for you and take it to Father Ernest.");
    const std::size_t calls = f.llm.calls.size();
    f.game.picked(1, "accept", f.game.menu()[0]);
    f.drain(tailer);
    // Agreed: the yes owed, her sitting Henry down and the first part, in beats; and the
    // conversation does not close on it, since the writing is now. The way to Ernest
    // (`agreed`) is not said: there is nothing to carry until the letter is written.
    CHECK(f.said("answered with the dictation begun on his agreeing, with its first part as written"));
    REQUIRE(f.recorded.size() == 1);
    CHECK(f.recorded[0].second == kQuestAccept);
    CHECK(f.session.history().back().content.find("Write this, part 1.") != std::string::npos);
    CHECK(f.session.history().back().content.find("You'll find Ernest") == std::string::npos);
    f.play_all(tailer);
    for (const std::string& directive : f.game.directives()) CHECK(directive != "@end");
    // The list is the part's ways alone: as said, softened, made larger (the first part
    // cannot be left out), in that order, and nothing else.
    REQUIRE(f.game.menu().size() == 3);
    CHECK(f.game.menu()[0] == "I'll write part 1 as you say it.");
    CHECK(f.game.menu()[1] == "I'll put part 1 more gently.");
    CHECK(f.game.menu()[2] == "Let me give part 1 some weight.");
    CHECK(f.said("offered the dictation's 3 ways with part 1"));
    // The list is closed: the mod draws no say-something-else or leave row under it
    // (notes.md 21 Oct 2026), since the letter is written in these rows or not at all.
    {
        bool closed = false;
        for (const std::string& directive : f.game.directives()) if (directive == "@rows 3 closed") closed = true;
        CHECK(closed);
    }
    std::size_t at = f.offered.size();
    CHECK(f.offered[at - 3].intent == "write:1");
    CHECK(f.offered[at - 2].intent == "soften:1");
    CHECK(f.offered[at - 1].intent == "embellish:1");

    // As said: her answer and the second part together; the second part has all four ways.
    f.game.picked(1, "write:1", "I'll write part 1 as you say it.");
    f.drain(tailer);
    CHECK(f.said("answered with the writer's answer to write:1, and the next part as written"));
    CHECK(f.session.history().back().content == "Good. Every word.\n\nWrite this, part 2.");
    f.play_all(tailer);
    REQUIRE(f.game.menu().size() == 4);
    CHECK(f.game.menu()[1] == "Do you want part 2 in a prayer?");
    at = f.offered.size();
    CHECK(f.offered[at - 3].intent == "reconsider:2");

    // A way with a part not being chosen on is nobody's row, and the model does not answer it.
    f.game.picked(1, "write:1", "I'll write part 1 as you say it.");
    f.drain(tailer);
    CHECK(f.llm.calls.size() == calls);
    f.play_all(tailer);
    REQUIRE(f.game.menu().size() == 4);

    // Softened, then made larger: the sealing closes the conversation and owes the mod the
    // letter as written, 0 as said, 2 softened, 3 made larger.
    f.game.picked(3, "soften:2", "I'll put part 2 more gently.");
    f.drain(tailer);
    CHECK(f.session.history().back().content == "Fine. Write it so.\n\nWrite this, part 3.");
    f.play_all(tailer);
    REQUIRE(f.game.menu().size() == 4);
    f.game.picked(4, "embellish:3", "Let me give part 3 some weight.");
    f.drain(tailer);
    CHECK(f.said("answered with the writer's answer to embellish:3, and the sealing as written"));
    REQUIRE(f.recorded.size() == 2);
    CHECK(f.recorded[1].second == "wrote_023");
    // The sealing, and only now the way to Ernest.
    CHECK(f.session.history().back().content
          == "Yes. Write that.\n\n" + christines_prayer().dictation.done + "\n\n" + christines_prayer().agreed);
    f.play_all(tailer);
    bool wrote = false, ended = false;
    for (const std::string& directive : f.game.directives()) {
        if (directive == "@quest b2cc2651365c wrote_023") wrote = true;
        if (directive == "@end") ended = true;
    }
    CHECK(wrote);
    CHECK(ended);
    CHECK_FALSE(f.game.has_turn(f.session.played() + 1));
    // Not a word of it was the model's.
    CHECK(f.llm.calls.size() == calls);
}

TEST_CASE("a dictation left unwritten is picked up again on the next visit, from the start") {
    // Back at Christine with the errand taken up and the store's flags without a W: her
    // greeting for it, the row that picks the dictation up first, the give-up row last, and
    // no row about a letter that does not exist yet.
    Fixture f(false);
    f.session.set_persona_source(
        [](std::string_view, const Approach& approach, std::string_view, const SoulRecord&) -> std::optional<Persona> {
            Persona persona{"Christine", "You are Christine, a housekeeper.", "What do you want?"};
            const Quest quest = christines_prayer();
            QuestFacts facts;
            facts.accepted = true;
            facts.have = 0;
            persona.quest_of = quest;
            persona.quest = plan_quest(quest, facts, approach);
            persona.greeting = persona.quest.greeting;
            persona.scripted = plan_scripted("", {}, {}, quest, facts, first_meeting());
            return persona;
        });
    LogTailer tailer(f.game.log());
    f.game.launch();
    f.open(tailer);
    f.play_all(tailer);
    CHECK(f.session.history()[0].content == "You're back. Sit, and we'll get it written.");
    REQUIRE(f.game.menu().size() >= 2);
    CHECK(f.game.menu()[0] == "Let's get your prayer written.");
    CHECK(f.offered[0].intent == "dictate");
    CHECK(f.game.menu().back() == kGiveUpRow);
    for (const std::string& row : f.game.menu()) {
        CHECK(row.find("Father Ernest") == std::string::npos);
    }
    const std::size_t calls = f.llm.calls.size();
    f.game.picked(1, "dictate", "Let's get your prayer written.");
    f.drain(tailer);
    CHECK(f.said("answered with the dictation begun, with its first part as written"));
    CHECK(f.recorded.empty());
    f.play_all(tailer);
    REQUIRE(f.game.menu().size() == 3);
    CHECK(f.game.menu()[0] == "I'll write part 1 as you say it.");
    // Left out where it can be: the digit is 1, and the part is not read to the priest.
    f.game.picked(1, "write:1", f.game.menu()[0]);
    f.drain(tailer);
    f.play_all(tailer);
    f.game.picked(2, "reconsider:2", "Do you want part 2 in a prayer?");
    f.drain(tailer);
    CHECK(f.session.history().back().content == "No. Leave it out.\n\nWrite this, part 3.");
    f.play_all(tailer);
    f.game.picked(2, "reconsider:3", "Do you want part 3 in a prayer?");
    f.drain(tailer);
    REQUIRE(f.recorded.size() == 1);
    CHECK(f.recorded[0].second == "wrote_011");
    CHECK(f.llm.calls.size() == calls);
}

TEST_CASE("the errand taken up on a yes the judge read leaves the dictation due, and the row that picks it up is laid first") {
    Fixture f(false);
    f.session.set_persona_source(
        [](std::string_view, const Approach& approach, std::string_view, const SoulRecord&) -> std::optional<Persona> {
            Persona persona{"Christine", "You are Christine, a housekeeper.", "What do you want?"};
            Quest quest = christines_prayer();
            quest.agreed.clear();
            persona.quest_of = quest;
            persona.quest = plan_quest(quest, {}, approach);
            persona.scripted = plan_scripted("", {}, {}, quest, {}, first_meeting());
            return persona;
        });
    LogTailer tailer(f.game.log());
    f.game.launch();
    f.open(tailer);
    f.play_all(tailer);
    // Typed words the judge reads as a yes: the model answers with the direction (which
    // tells her not to begin dictating), the yes is owed, and the conversation stays open.
    f.llm.replies = {"YES", "Good. Sit, then."};
    f.game.typed("I can write. I'll do it.");
    f.drain(tailer);
    REQUIRE(f.recorded.size() == 1);
    CHECK(f.recorded[0].second == kQuestAccept);
    f.play_all(tailer);
    for (const std::string& directive : f.game.directives()) CHECK(directive != "@end");
    REQUIRE_FALSE(f.game.menu().empty());
    CHECK(f.game.menu()[0] == "Let's get your prayer written.");
    f.game.picked(1, "dictate", "Let's get your prayer written.");
    f.drain(tailer);
    CHECK(f.said("answered with the dictation begun, with its first part as written"));
    f.play_all(tailer);
    REQUIRE(f.game.menu().size() == 3);
    CHECK(f.offered.back().intent == "embellish:1");
}
