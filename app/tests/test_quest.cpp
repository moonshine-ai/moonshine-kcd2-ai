#include "doctest.h"

#include "fake_game.h"
#include "gamelog.h"
#include "personadb.h"
#include "quest.h"
#include "session.h"
#include "stub_llm.h"

using namespace moonshine;

namespace {

// Bartel's quest as the generator dealt it: his work axe is in Julian's pocket.
Quest bartels_axe(QuestRole role) {
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
    quest.reward_groschen = 50;
    quest.skill = "persuade";
    quest.difficulty = "medium";
    quest.offer = "Julian's had my best work axe since Candlemas.";
    quest.thanks = "Finally, my axe.";
    return quest;
}

const std::string kWorld = "Bohemia, 1403.";

// A session talking to one end of the quest, with the model scripted.
struct Fixture {
    FakeGame game;
    StubLlm llm;
    TurnWriter writer{game.paths()};
    Options options;
    Prefiller prefiller{llm, options};
    Session session{llm, writer, prefiller, options};
    std::vector<std::pair<std::string, std::string>> recorded;
    std::vector<std::string> statuses;

    explicit Fixture(QuestRole role, QuestFacts facts = {}, std::string greeting = {}) {
        session.set_world(kWorld);
        session.set_persona_source(
            [role, facts, greeting](std::string_view, const Approach& approach, std::string_view,
                                    const SoulRecord&) -> std::optional<Persona> {
                Persona persona{"Bartel", "You are Bartel, a varlet in Kuttenberg.", greeting};
                persona.quest_of = bartels_axe(role);
                QuestFacts known = facts;
                if (known.have < 0) known.have = approach.have;
                persona.quest = plan_quest(persona.quest_of, known, approach);
                return persona;
            });
        Listener listener;
        listener.on_quest = [this](std::string_view id, std::string_view command) {
            recorded.emplace_back(std::string(id), std::string(command));
        };
        listener.on_status = [this](std::string_view text) { statuses.emplace_back(text); };
        session.set_listener(std::move(listener));
    }
};

}  // namespace

TEST_CASE("a verdict is read off the first word, whatever follows it") {
    CHECK(read_verdict("YES") == Verdict::Yes);
    CHECK(read_verdict("Yes.") == Verdict::Yes);
    CHECK(read_verdict("\"Aye\"") == Verdict::Yes);
    CHECK(read_verdict("No") == Verdict::No);
    CHECK(read_verdict("no, he did not") == Verdict::No);
    // A model that answered in a sentence anyway.
    CHECK(read_verdict("Well -- yes, he agreed.") == Verdict::Yes);
    CHECK(read_verdict("He said nothing of the kind.") == Verdict::Unclear);
    CHECK(read_verdict("Both yes and no.") == Verdict::Unclear);
    CHECK(read_verdict("") == Verdict::Unclear);
}

TEST_CASE("the judge is asked cold, briefly, and over the same conversation") {
    const std::vector<Message> turn = {
        {"system", "You are Bartel."},
        {"assistant", "What do you want?"},
        {"user", "I'll get your axe back."},
    };
    const std::vector<Message> asked = judge_messages(turn, "Did they agree?");
    REQUIRE(asked.size() == 3);
    // Every byte before the question is the reply's own, so the prefix is shared.
    CHECK(asked[0].content == turn[0].content);
    CHECK(asked[1].content == turn[1].content);
    CHECK(asked[2].content.rfind("I'll get your axe back.", 0) == 0);
    CHECK(asked[2].content.find("YES or NO") != std::string::npos);
    CHECK(asked[2].content.find("Did they agree?") != std::string::npos);

    Options options;
    options.temperature = 1.0;
    options.reply_tokens = 512;
    const Options cold = judge_options(options);
    CHECK(cold.temperature == 0.0);
    CHECK(cold.reply_tokens <= 8);
    CHECK(cold.model == options.model);
}

TEST_CASE("the giver asks until somebody agrees, then waits, then pays on sight") {
    const Quest quest = bartels_axe(QuestRole::Giver);
    Approach approach;

    QuestPlan fresh = plan_quest(quest, {}, approach);
    CHECK(fresh.block.find("Nobody has yet agreed") != std::string::npos);
    // Money without a figure: the mouth carries its weight, the journal the sum.
    CHECK(fresh.block.find("a good purse of silver") != std::string::npos);
    CHECK(fresh.block.find("50") == std::string::npos);
    CHECK(fresh.block.find(quest.offer) != std::string::npos);
    CHECK(fresh.on_yes == kQuestAccept);
    CHECK_FALSE(fresh.question.empty());
    CHECK(fresh.opening_commands.empty());

    QuestFacts agreed;
    agreed.accepted = true;
    QuestPlan waiting = plan_quest(quest, agreed, approach);
    CHECK(waiting.block.find("already agreed") != std::string::npos);
    CHECK(waiting.question.empty());
    CHECK(waiting.opening_commands.empty());

    agreed.have = 1;
    QuestPlan paying = plan_quest(quest, agreed, approach);
    CHECK(paying.block.find("has brought your work axe back") != std::string::npos);
    CHECK(paying.block.find(quest.thanks) != std::string::npos);
    REQUIRE(paying.opening_commands.size() == 1);
    CHECK(paying.opening_commands[0] == kQuestComplete);
    CHECK(paying.question.empty());

    // Axe in hand but never asked: the graph wants an acceptance before a completion.
    QuestFacts unasked;
    unasked.have = 1;
    QuestPlan surprised = plan_quest(quest, unasked, approach);
    REQUIRE(surprised.opening_commands.size() == 2);
    CHECK(surprised.opening_commands[0] == kQuestAccept);
    CHECK(surprised.opening_commands[1] == kQuestComplete);

    QuestFacts settled;
    settled.done = true;
    CHECK(plan_quest(quest, settled, approach).block.find("settled") != std::string::npos);
}

TEST_CASE("the holder keeps the thing in talk, and the game's own menu does the asking") {
    const Quest quest = bartels_axe(QuestRole::Target);
    Approach anyone;

    // Nothing is listened for and nothing is weighed: the sheet has them make excuses,
    // and whether Henry gets the axe is settled in the game's dialogue menu, not here.
    QuestPlan plan = plan_quest(quest, {}, anyone);
    CHECK(plan.question.empty());
    CHECK(plan.on_yes.empty());
    CHECK(plan.opening_commands.empty());
    CHECK(plan.block.find("Bartel's work axe") != std::string::npos);
    CHECK(plan.block.find("excuses") != std::string::npos);
    CHECK(plan.block.find("hand it over") == std::string::npos);
    CHECK(plan.block.find("brackets") == std::string::npos);

    // Once it has gone -- by the menu's handover, or because the player is seen carrying
    // it -- the sheet says so, and still asks nothing.
    QuestFacts handed;
    handed.handed = true;
    QuestPlan after = plan_quest(quest, handed, anyone);
    CHECK(after.block.find("You gave") != std::string::npos);
    CHECK(after.question.empty());
    QuestFacts carrying;
    carrying.have = 1;
    CHECK(plan_quest(quest, carrying, anyone).block.find("You gave") != std::string::npos);
    QuestFacts done;
    done.done = true;
    CHECK(plan_quest(quest, done, anyone).block.find("You gave") != std::string::npos);
}

TEST_CASE("the holder's turn goes straight to the reply, with no judge and no command") {
    Fixture f(QuestRole::Target);
    f.llm.replies = {"What?", "You'll not have it. Tell Bartel I need it a while yet."};
    f.session.handle({Marker::Begin, 0, "kkut_additive_man_45"});
    f.game.play();
    f.session.handle({Marker::Played, 1, {}});
    f.session.say("Bartel wants his axe back. Hand it over or I'll break your jaw.");
    // Two calls only: the opening and the reply. No one-word question was asked between.
    REQUIRE(f.llm.calls.size() == 2);
    CHECK(f.llm.calls[1].messages.back().content ==
          "Bartel wants his axe back. Hand it over or I'll break your jaw.");
    CHECK(f.recorded.empty());
    const std::string turn = f.game.whole(2);
    CHECK(turn.find("@quest") == std::string::npos);
}

TEST_CASE("the begin line's drawn weapon and looks are read; there are no stats to read") {
    Event event = classify(
        "BRIDGE begin npc=kkut_additive_man_45 doing=none rch=0.5 bad=0.7 drawn=true "
        "pspc=14 pstr=9 pcha=17 rep=0.55");
    CHECK(event.approach.charisma == 0.5);
    CHECK(event.approach.armed);
    CHECK_FALSE(classify("BRIDGE begin npc=x rch=0.5 drawn=false").approach.armed);
}

TEST_CASE("a yes from the judge rides into the turn file ahead of the words") {
    Fixture f(QuestRole::Giver);
    f.llm.replies = {"What do you want?", "YES", "Fifty groschen when it is in my hands."};
    f.session.handle({Marker::Begin, 0, "kkut_additive_man_47"});
    REQUIRE(f.game.play() == "What do you want?");
    f.session.handle({Marker::Played, 1, {}});

    f.session.say("I'll fetch it back for you.");
    // Two calls: the judge, then the reply. The judge asked the model over the same
    // conversation, cold and short.
    REQUIRE(f.llm.calls.size() == 3);
    const StubLlm::Call& judged = f.llm.calls[1];
    CHECK(judged.options.temperature == 0.0);
    CHECK(judged.options.reply_tokens <= 8);
    CHECK(judged.messages.back().content.find("YES or NO") != std::string::npos);
    // The reply was told what just became true.
    const StubLlm::Call& replied = f.llm.calls[2];
    CHECK(replied.messages.back().content.find("[They have agreed") != std::string::npos);

    // The turn file leads with the directive the mod strips, then the words, on one line.
    REQUIRE(f.game.play().has_value());
    const std::string turn = f.game.whole(2);
    CHECK(turn.rfind("@quest c00183e1da7e accept ", 0) == 0);
    CHECK(turn.find("Fifty groschen when it is in my hands.") != std::string::npos);
    REQUIRE(f.recorded.size() == 1);
    CHECK(f.recorded[0].second == "accept");

    // Asked once. The next turn goes straight to the reply.
    f.session.handle({Marker::Played, 2, {}});
    f.llm.replies.push_back("Off you go, then.");
    f.session.say("Where does he keep it?");
    CHECK(f.llm.calls.size() == 4);
    CHECK(f.game.play() == "Off you go, then.");
}

TEST_CASE("a no from the judge changes nothing") {
    Fixture f(QuestRole::Giver);
    f.llm.replies = {"What do you want?", "No.", "Then be off."};
    f.session.handle({Marker::Begin, 0, "kkut_additive_man_47"});
    f.game.play();
    f.session.handle({Marker::Played, 1, {}});
    f.session.say("Where is the well?");
    CHECK(f.game.play() == "Then be off.");
    CHECK(f.recorded.empty());
    // Still listening.
    f.session.handle({Marker::Played, 2, {}});
    f.llm.replies = {"YES", "Good man."};
    f.session.say("Fine, I'll get your axe.");
    CHECK(f.recorded.size() == 1);
}

TEST_CASE("the giver who sees the axe pays on the opening turn") {
    QuestFacts agreed;
    agreed.accepted = true;
    Fixture f(QuestRole::Giver, agreed);
    f.llm.replies = {"You actually got it."};
    Event begin{Marker::Begin, 0, "kkut_additive_man_47"};
    begin.approach.have = 1;
    f.session.handle(begin);
    REQUIRE(f.game.play().has_value());
    const std::string turn = f.game.whole(1);
    CHECK(turn.rfind("@quest c00183e1da7e complete ", 0) == 0);
    CHECK(turn.find("You actually got it.") != std::string::npos);
    REQUIRE(f.recorded.size() == 1);
    CHECK(f.recorded[0].second == "complete");
}

TEST_CASE("the state greeting is passed over on the turn the quest settles, for the giver's own line") {
    // "How can I serve, sir?" was written for a stranger at the door. From a man watching his
    // own axe come back it breaks the scene, so that opening is the thanks the writer gave
    // him -- said at once, with the completion riding on it, and no model call.
    QuestFacts agreed;
    agreed.accepted = true;
    Fixture f(QuestRole::Giver, agreed, "How can I serve, sir?");
    f.llm.replies = {"You actually got it."};
    Event begin{Marker::Begin, 0, "kkut_additive_man_47"};
    begin.approach.have = 1;
    f.session.handle(begin);
    // The fake game strips the directive the way the mod does, and keeps it for asking about.
    REQUIRE(f.game.play() == "Finally, my axe.");
    // The completion, and the close: with nothing of his to lead on to, the thanks are the
    // last word (notes.md 23 Oct 2026).
    REQUIRE(f.game.directives().size() == 2);
    CHECK(f.game.directives()[0] == "@quest c00183e1da7e complete");
    CHECK(f.game.directives()[1] == "@end");
    REQUIRE(f.recorded.size() == 1);
    CHECK(f.recorded[0].second == "complete");
    // No reply was generated for the opening: the first call to the model, if any, is a
    // prefill or a row, not a line.
    for (const StubLlm::Call& call : f.llm.calls) CHECK(call.options.reply_tokens <= 1);
    bool explained = false;
    for (const std::string& line : f.statuses) {
        if (line.find("settles on this turn, on the line written for it") != std::string::npos) explained = true;
    }
    CHECK(explained);

    // With nothing owed, the same greeting is said as written.
    Fixture g(QuestRole::Giver, {}, "How can I serve, sir?");
    g.session.handle({Marker::Begin, 0, "kkut_additive_man_47"});
    CHECK(g.game.play() == "How can I serve, sir?");
    CHECK(g.recorded.empty());
}

TEST_CASE("a settling turn with no line written for it is the one opening that is generated") {
    // A giver the writer never reached: the thanks is empty, so the model finds the words,
    // and the status line says why the player waits.
    QuestFacts agreed;
    agreed.accepted = true;
    Fixture f(QuestRole::Giver, agreed, "How can I serve, sir?");
    f.session.set_persona_source(
        [](std::string_view, const Approach& approach, std::string_view,
           const SoulRecord&) -> std::optional<Persona> {
            Persona persona{"Bartel", "You are Bartel, a varlet in Kuttenberg.", "How can I serve, sir?"};
            persona.quest_of = bartels_axe(QuestRole::Giver);
            persona.quest_of.thanks.clear();
            QuestFacts known;
            known.accepted = true;
            known.have = approach.have;
            persona.quest = plan_quest(persona.quest_of, known, approach);
            return persona;
        });
    f.llm.replies = {"You actually got it."};
    Event begin{Marker::Begin, 0, "kkut_additive_man_47"};
    begin.approach.have = 1;
    f.session.handle(begin);
    REQUIRE(f.game.play() == "You actually got it.");
    REQUIRE(f.game.directives().size() == 1);
    CHECK(f.game.directives()[0] == "@quest c00183e1da7e complete");
    REQUIRE(f.llm.calls.size() >= 1);
    CHECK(f.llm.calls.back().options.reply_tokens > 1);
    bool explained = false;
    for (const std::string& line : f.statuses) {
        if (line.find("no line is written for it, so the opening line is generated") != std::string::npos) explained = true;
    }
    CHECK(explained);
}

TEST_CASE("the visits an errand makes distinct open on the giver's lines for them") {
    // Back with the favour taken up and nothing to show: the owed line, and nothing owed to
    // the game. Back carrying the thing without ever having agreed: the unasked line, with
    // the accept and the completion both riding on it.
    const auto source = [](QuestFacts known) {
        return [known](std::string_view, const Approach& approach, std::string_view,
                       const SoulRecord&) -> std::optional<Persona> {
            Persona persona{"Bartel", "You are Bartel, a varlet in Kuttenberg.", "How can I serve, sir?"};
            persona.quest_of = bartels_axe(QuestRole::Giver);
            persona.quest_of.greet_owed = "Any luck with my axe?";
            persona.quest_of.greet_settle_unasked = "Is that -- that's my axe. How did you come by it?";
            QuestFacts facts = known;
            facts.have = approach.have;
            persona.quest = plan_quest(persona.quest_of, facts, approach);
            return persona;
        };
    };
    QuestFacts agreed;
    agreed.accepted = true;
    Fixture owed(QuestRole::Giver);
    owed.session.set_persona_source(source(agreed));
    Event back{Marker::Begin, 0, "kkut_additive_man_47"};
    back.approach.have = 0;
    owed.session.handle(back);
    CHECK(owed.game.play() == "Any luck with my axe?");
    CHECK(owed.game.directives().empty());
    CHECK(owed.recorded.empty());
    bool explained = false;
    for (const std::string& line : owed.statuses) {
        if (line.find("the errand is open between them") != std::string::npos) explained = true;
    }
    CHECK(explained);

    Fixture unasked(QuestRole::Giver);
    unasked.session.set_persona_source(source({}));
    Event carrying{Marker::Begin, 0, "kkut_additive_man_47"};
    carrying.approach.have = 1;
    unasked.session.handle(carrying);
    CHECK(unasked.game.play() == "Is that -- that's my axe. How did you come by it?");
    REQUIRE(unasked.recorded.size() == 2);
    CHECK(unasked.recorded[0].second == "accept");
    CHECK(unasked.recorded[1].second == "complete");

    // The plan alone: a locked link and a settled favour have no visit line, and the thanks
    // is the settling line only once the player agreed.
    Quest quest = bartels_axe(QuestRole::Giver);
    quest.greet_owed = "Any luck with my axe?";
    quest.greet_settle_unasked = "That's my axe.";
    QuestFacts facts;
    CHECK(plan_quest(quest, facts, {}).greeting.empty());
    // A first meeting with the favour there to be offered opens on the giver's own line for
    // it when one was written (notes.md 13 Oct 2026: Benesh, hailed over, says why), and
    // only the once -- back with it still unasked, the soul's own greeting serves.
    quest.greet_offer = "You're the lad who helps folk, aren't you? Then look at me.";
    CHECK(plan_quest(quest, facts, {}).greeting == quest.greet_offer);
    CHECK(plan_quest(quest, facts, {}, 1).greeting == quest.greet_offer);
    CHECK(plan_quest(quest, facts, {}, 2).greeting.empty());
    facts.accepted = true;
    CHECK(plan_quest(quest, facts, {}).greeting == "Any luck with my axe?");
    facts.locked = true;
    CHECK(plan_quest(quest, facts, {}).greeting.empty());
    facts.locked = false;
    facts.have = 1;
    CHECK(plan_quest(quest, facts, {}).greeting == "Finally, my axe.");
    facts.accepted = false;
    CHECK(plan_quest(quest, facts, {}).greeting == "That's my axe.");
    facts.done = true;
    facts.accepted = true;
    CHECK(plan_quest(quest, facts, {}).greeting.empty());
    // The holder has no visit lines: his side is the game's menu.
    quest.role = QuestRole::Target;
    facts = {};
    facts.accepted = true;
    CHECK(plan_quest(quest, facts, {}).greeting.empty());
}

TEST_CASE("the begin line carries the count of what the player holds") {
    Event event = classify("BRIDGE begin npc=kkut_additive_man_47 doing=none rch=0.5 have=2");
    CHECK(event.approach.have == 2);
    CHECK(classify("BRIDGE begin npc=kkut_additive_man_47").approach.have == -1);
    // And, for a soul on more than one favour, the count of each by id.
    Event two = classify("BRIDGE begin npc=kkut_additive_man_47 doing=none have=1 "
                         "holding=c00183e1da7e:1,df11490b3a9f:0 hour=9.5");
    CHECK(two.approach.have == 1);
    REQUIRE(two.approach.holding.size() == 2);
    CHECK(two.approach.holding[0] == std::pair<std::string, int>{"c00183e1da7e", 1});
    CHECK(two.approach.holding[1] == std::pair<std::string, int>{"df11490b3a9f", 0});
    CHECK(two.approach.hour == 9.5);
    // Whether the player can pray at a shrine, on a pray's giver; unread elsewhere.
    CHECK(two.approach.can_pray == -1);
    CHECK(classify("BRIDGE begin npc=kkut_man_246 doing=none have=0 holding=665815e379c8:0 pray=0")
              .approach.can_pray == 0);
    CHECK(classify("BRIDGE begin npc=kkut_man_246 doing=none have=0 pray=1 hour=9.5").approach.can_pray == 1);
    // What Henry could give a beggar, as letters; empty on everyone else.
    CHECK(two.approach.alms.empty());
    CHECK(classify("BRIDGE begin npc=kkut_additive_man_142 doing=none alms=mfc qset=0123456789abcdef").approach.alms == "mfc");
    CHECK(classify("BRIDGE begin npc=kkut_additive_man_142 doing=none alms=m hour=9.5").approach.alms == "m");
}

namespace {

// The pilot slate's chain: Bartel's axe from Julian, a dispute, then his saw from
// Frantishek, which pays in thanks.
Quest bartels_dispute() {
    Quest quest = bartels_axe(QuestRole::Giver);
    quest.chain_id = "bartel_tools";
    quest.chain_order = 1;
    quest.dispute = true;
    quest.apologise = "Julian says he won't give the axe back until you admit he paid your slate.";
    quest.apologise_yes = "Christ! Fine. He paid. Tell him I said it.";
    quest.apologise_no = "I'll admit no such thing.";
    return quest;
}

Quest bartels_saw() {
    Quest quest;
    quest.id = "df11490b3a9f";
    quest.kind = "retrieve";
    quest.role = QuestRole::Giver;
    quest.giver_soul = "kkut_additive_man_47";
    quest.giver_name = "Bartel";
    quest.target_soul = "kkut_additive_man_46";
    quest.target_name = "Frantishek";
    quest.item_name = "saw";
    quest.reward_groschen = 0;
    quest.reward_text = "thanks and a favour owed";
    quest.chain_id = "bartel_tools";
    quest.chain_order = 2;
    quest.after_quest_id = "c00183e1da7e";
    quest.after_item_name = "work axe";
    quest.offer = "I have my axe. Now I need my saw.";
    return quest;
}

// The nest: Alan's axe is with Ludwig, who will not part with it until his knife is back
// from Havel. Both as Ludwig sees them.
Quest alans_axe_held() {
    Quest quest;
    quest.id = "754fc3fa4b94";
    quest.kind = "retrieve";
    quest.role = QuestRole::Target;
    quest.giver_soul = "kkut_additive_man_50";
    quest.giver_name = "Alan";
    quest.target_soul = "kkut_additive_man_48";
    quest.target_name = "Ludwig";
    quest.item_name = "work axe";
    quest.reward_groschen = 15;
    quest.requires_quest_id = "96759bc0b28f";
    return quest;
}

Quest ludwigs_knife() {
    Quest quest;
    quest.id = "96759bc0b28f";
    quest.kind = "retrieve";
    quest.role = QuestRole::Giver;
    quest.giver_soul = "kkut_additive_man_48";
    quest.giver_name = "Ludwig";
    quest.target_soul = "kkut_additive_man_44";
    quest.target_name = "Havel";
    quest.item_name = "knife";
    quest.offer = "I can't give you the axe until I have my knife back.";
    return quest;
}

QuestFacts at(std::string_view stage, int have = -1) {
    QuestFacts facts;
    facts.accepted = stage == "accepted" || stage == "handed" || stage == "done";
    facts.handed = stage == "handed";
    facts.done = stage == "done";
    facts.have = have;
    return facts;
}

}  // namespace

TEST_CASE("a dispute's giver has his answer to the holder's condition ready, and it owes his word") {
    const Quest quest = bartels_dispute();
    // Before the player has been to the holder, nothing to answer: the offer stands, and
    // the sheet gives him his side of the grievance.
    QuestPlan fresh = plan_quest(quest, {}, {});
    CHECK(fresh.apologise_row.empty());
    CHECK(fresh.block.find("grievance against you") != std::string::npos);

    // The player offered to carry it. Well turned out, he gets the apology.
    QuestFacts carried = at("accepted");
    carried.broker = true;
    Approach smart;
    smart.charisma = 0.7;
    QuestPlan yes = plan_quest(quest, carried, smart);
    CHECK(yes.apologise_row == quest.apologise);
    CHECK(yes.apologise_reply == quest.apologise_yes);
    CHECK(yes.apologise_command == kQuestApologise);
    CHECK(yes.block.find("you have your answer ready") != std::string::npos);
    CHECK(yes.question.empty());

    // In rags, he does not -- until he has been asked enough times.
    Approach ragged;
    ragged.charisma = 0.1;
    QuestPlan no = plan_quest(quest, carried, ragged);
    CHECK(no.apologise_reply == quest.apologise_no);
    CHECK(no.apologise_command == kQuestRefuseApology);
    CHECK(plan_quest(quest, carried, ragged, 3).apologise_command == kQuestApologise);
    // A mod that did not report the bearing: he gives it, so the pilot plays through.
    CHECK(plan_quest(quest, carried, {}).apologise_command == kQuestApologise);

    // Refused once, the row comes back and the sheet says so; given, it does not.
    QuestFacts refused = carried;
    refused.refused = true;
    QuestPlan again = plan_quest(quest, refused, ragged);
    CHECK(again.apologise_row == quest.apologise);
    CHECK(again.block.find("once already") != std::string::npos);
    QuestFacts given = carried;
    given.apologised = true;
    QuestPlan settled = plan_quest(quest, given, ragged);
    CHECK(settled.apologise_row.empty());
    CHECK(settled.block.find("swallowed your pride") != std::string::npos);
    // Back with the axe, the pride is a thing not to be mentioned.
    QuestFacts home = given;
    home.have = 1;
    CHECK(plan_quest(quest, home, {}).block.find("cost you your pride") != std::string::npos);
}

namespace {

// The pilot's letter: Veit Berger the minter writes to Ulrich the Mouth, a miner.
Quest veits_letter(QuestRole role) {
    Quest quest;
    quest.id = "8ab11c4ab065";
    quest.kind = "deliver";
    quest.role = role;
    quest.giver_soul = "prepadeniVlasskehoDvora_worker_2";
    quest.giver_name = "Veit Berger";
    quest.target_soul = "prepadeniVlasskehoDvora_silverCarrier_4";
    quest.target_name = "Ulrich the Mouth";
    quest.item_class = "859d46b3-d739-5d45-a80e-64a232ab9ed3";
    quest.item_name = "letter";
    quest.reward_groschen = 5;
    quest.skill = "persuade";
    quest.difficulty = "medium";
    quest.offer = "I need a letter taken to the Osel mine, to a miner named Ulrich.";
    quest.thanks = "You actually gave it to him? Here is the five groschen.";
    quest.report = "I delivered your letter to Ulrich the Mouth.";
    quest.keep = "I'm sorry, but I lost your letter.";
    quest.kept_reply = "Lost it? Saints. Just leave me be.";
    return quest;
}

}  // namespace

TEST_CASE("a letter's writer asks, then takes Henry's word for it through two rows") {
    const Quest quest = veits_letter(QuestRole::Giver);
    // Before anyone agreed: the offer, and the judge listens for a yes.
    QuestPlan fresh = plan_quest(quest, {}, {});
    CHECK(fresh.block.find("written a letter to Ulrich the Mouth") != std::string::npos);
    CHECK(fresh.block.find(quest.offer) != std::string::npos);
    CHECK(fresh.question.find("carry your letter") != std::string::npos);
    CHECK(fresh.on_yes == kQuestAccept);
    CHECK(fresh.direction.find("a few groschen") != std::string::npos);
    CHECK(fresh.rows.empty());

    // Agreed and carrying: the writer cannot see, so nothing is owed on sight, and Henry has
    // two things he can say. "It is with Ulrich" is a lie here and is paid all the same.
    QuestFacts carrying = at("accepted");
    carrying.have = 1;
    QuestPlan out = plan_quest(quest, carrying, {});
    CHECK(out.opening_commands.empty());
    CHECK(out.question.empty());
    CHECK(out.block.find("no way of knowing") != std::string::npos);
    REQUIRE(out.rows.size() == 3);  // and the give-up row, last on every open errand's list
    CHECK(out.rows[2].intent == "give_up");
    CHECK(out.rows[0].intent == "report");
    // Said as the lie it is, and carried off on a Speech check (notes.md 9 Oct 2026).
    CHECK(out.rows[0].text == "(Lie) " + quest.report);
    CHECK(out.rows[0].reply == quest.thanks);
    CHECK(out.rows[0].command == kQuestLie);
    CHECK(out.rows[0].check == "speech");
    CHECK_FALSE(out.rows[0].fail_reply.empty());
    // And not at all until two game hours have passed since the letter changed hands.
    QuestFacts fresh_hands = carrying;
    fresh_hands.hours_since_accept = 0.5;
    REQUIRE(plan_quest(quest, fresh_hands, {}).rows.size() == 2);
    CHECK(plan_quest(quest, fresh_hands, {}).rows[0].intent == "keep");
    CHECK(plan_quest(quest, fresh_hands, {}).rows[1].intent == "give_up");
    fresh_hands.hours_since_accept = 2.5;
    CHECK(plan_quest(quest, fresh_hands, {}).rows.size() == 3);
    CHECK(out.rows[1].intent == "keep");

    // Delivered: the writer asks what was said (his own greeting for it), and Henry has the
    // truth or one of two lies to give him, each paid as the letter did arrive (notes.md
    // 9 Oct 2026, Holy Jest). Nothing written for the lies, the plain report alone.
    QuestFacts handed = carrying;
    handed.handed = true;
    handed.have = 0;
    QuestPlan handed_plain = plan_quest(quest, handed, {});
    REQUIRE(handed_plain.rows.size() == 1);
    CHECK(handed_plain.rows[0].intent == "report");
    CHECK(handed_plain.rows[0].text == quest.report);
    CHECK(handed_plain.rows[0].command == kQuestComplete);
    CHECK(handed_plain.rows[0].check.empty());
    Quest asked = quest;
    asked.greet_handed = "You are back. What did he say?";
    asked.told = "He read it, and said you were right to ask.";
    asked.told_reply = "Right to ask. Then it is settled. Here is your coin.";
    asked.lie_soft = "He is looking into it.";
    asked.lie_soft_reply = "Looking into it. Well, that is something.";
    asked.lie_big = "He was horrified. He is having the man excommunicated.";
    asked.lie_big_reply = "Excommunicated! Lord have mercy, I only wanted him told.";
    QuestPlan three = plan_quest(asked, handed, {});
    CHECK(three.greeting == "You are back. What did he say?");
    REQUIRE(three.rows.size() == 3);
    CHECK(three.rows[0].intent == "report");
    CHECK(three.rows[0].text == asked.told);
    CHECK(three.rows[0].reply == asked.told_reply);
    CHECK(three.rows[1].intent == "lie_soft");
    CHECK(three.rows[1].text == "(Lie) He is looking into it.");
    CHECK(three.rows[1].reply == asked.lie_soft_reply);
    CHECK(three.rows[1].command == kQuestComplete);
    CHECK(three.rows[2].intent == "lie_big");
    CHECK(three.rows[2].text == "(Lie) He was horrified. He is having the man excommunicated.");
    CHECK(three.rows[2].command == kQuestComplete);
    // Still carrying, the owed greeting as before, and no lies to tell about an answer.
    CHECK(plan_quest(asked, carrying, {}).greeting.empty());
    CHECK(plan_quest(asked, carrying, {}).rows.size() == 3);
    CHECK(out.rows[1].text == quest.keep);
    CHECK(out.rows[1].reply == quest.kept_reply);
    CHECK(out.rows[1].command == kQuestKeep);

    // Delivered, by the receiver's own Talk: the same words are the truth and complete it,
    // and there is nothing left to keep.
    QuestFacts delivered = at("handed");
    delivered.have = 0;
    QuestPlan home = plan_quest(quest, delivered, {});
    REQUIRE(home.rows.size() == 1);
    CHECK(home.rows[0].intent == "report");
    CHECK(home.rows[0].command == kQuestComplete);
    CHECK(home.opening_commands.empty());

    // Lost (accepted, nothing in hand, not delivered): both rows, and the keep row owns it.
    QuestFacts lost = at("accepted");
    lost.have = 0;
    QuestPlan gone = plan_quest(quest, lost, {});
    REQUIRE(gone.rows.size() == 3);
    CHECK(gone.rows[1].command == kQuestKeep);

    // Done, however it went: settled, no rows.
    QuestPlan done = plan_quest(quest, at("done"), {});
    CHECK(done.rows.empty());
    CHECK(done.block.find("settled") != std::string::npos);

    // A writer whose lines were never written still has rows, in plain words.
    Quest bare = quest;
    bare.report.clear();
    bare.keep.clear();
    bare.kept_reply.clear();
    bare.thanks.clear();
    QuestPlan plain = plan_quest(bare, carrying, {});
    REQUIRE(plain.rows.size() == 3);
    CHECK(plain.rows[0].text.find("Ulrich the Mouth") != std::string::npos);
    CHECK(plain.rows[0].reply.find("a few groschen") != std::string::npos);
    CHECK_FALSE(plain.rows[1].reply.empty());
}

TEST_CASE("a letter's receiver knows nothing until it is in his hand") {
    const Quest quest = veits_letter(QuestRole::Target);
    QuestPlan before = plan_quest(quest, at("accepted"), {});
    CHECK(before.block.find("know of no letter") != std::string::npos);
    CHECK(before.question.empty());
    CHECK(before.rows.empty());
    QuestPlan after = plan_quest(quest, at("handed"), {});
    CHECK(after.block.find("brought you a letter from Veit Berger") != std::string::npos);
    // As the other favour of a soul on two, one sentence each way.
    CHECK(quest_aside(quest, at("handed"), {quest}, {at("handed")}).find("letter") != std::string::npos);
    CHECK(quest_aside(quest, at("accepted"), {quest}, {at("accepted")}).empty());
    const Quest writer = veits_letter(QuestRole::Giver);
    CHECK(quest_aside(writer, at("accepted"), {writer}, {at("accepted")}).find("agreed to carry") !=
          std::string::npos);
}

namespace {

// In the Stocks' second scene (notes.md 9 Oct 2026): the parish chalice carried back to the
// priest, who asks how Henry came by it; Benesh's answer follows what Henry said.
Quest chalice_back(QuestRole role) {
    Quest quest;
    quest.id = "895f926a2b54";
    quest.kind = "deliver";
    quest.role = role;
    quest.giver_soul = "kkut_additive_man_52";
    quest.giver_name = "Benesh";
    quest.target_soul = "kkut_additive_man_135";
    quest.target_name = "Father Adam";
    quest.item_class = "6dc80a04-dad0-4259-a854-e085caa74cc1";
    quest.item_name = "silver chalice";
    quest.item_kind = "thing";
    quest.reward_text = "who is hiring this month";
    quest.skill = "persuade";
    quest.difficulty = "medium";
    quest.offer = "Take it to Father Adam. But say nothing of me, and nothing of Paul Schmidt.";
    quest.thanks = "It's back where it belongs. Listen, then: the stonemason pays on the day.";
    quest.more = "Why shouldn't I say who sent me?";
    quest.more_reply = "Because Paul swore he'd see me hang.";
    quest.more_2 = "You want me to lie to a priest?";
    quest.more_2_reply = "Not lie. Just -- don't say.";
    quest.greet_handed = "You're back. What did you tell him?";
    quest.report = "The chalice is back with Father Adam.";
    quest.told = "It's on the altar.";
    quest.told_by_how = {
        {"truth", "It's on the altar. And I told him the truth.", "You told him. Christ."},
        {"pinned", "I said a bandit dropped it, and that you'd recognised it.", "You said my name. To him."},
        {"covered", "", "Ha! A bandit. God forgive us both."},
        {"caught", "He's sending the bailiff's men after me.", ""},
    };
    quest.keep = "I haven't taken it to him. I'm keeping it.";
    quest.kept_reply = "Keeping it? Get out.";
    return quest;
}

}  // namespace

TEST_CASE("a thing carried back to its owner is a deliver in the owner's words, and the report follows what was said") {
    const Quest quest = chalice_back(QuestRole::Giver);
    // Offered as the thing it is, not a letter; a chalice already in Henry's pocket (the one
    // he just handed over) does not withhold the offer.
    QuestFacts fresh;
    fresh.have = 1;
    CHECK(offerable(quest, fresh));
    CHECK_FALSE(quest.brought(1));
    QuestPlan first = plan_quest(quest, fresh, {});
    CHECK(first.block.find("the silver chalice, which belongs to Father Adam") != std::string::npos);
    CHECK(first.block.find("letter") == std::string::npos);
    CHECK(first.question.find("take the silver chalice back to Father Adam") != std::string::npos);
    CHECK(first.direction.find("wrapped") != std::string::npos);

    // Agreed and carrying: the lie says the chalice, not a letter, and the keep row too.
    QuestFacts carrying = at("accepted");
    carrying.have = 1;
    QuestPlan out = plan_quest(quest, carrying, {});
    REQUIRE(out.rows.size() == 3);
    CHECK(out.rows[2].intent == "give_up");
    CHECK(out.rows[0].text == "(Lie) The chalice is back with Father Adam.");
    CHECK(out.rows[0].command == kQuestLie);
    CHECK(out.rows[1].intent == "keep");
    CHECK(out.block.find("take the silver chalice back") != std::string::npos);

    // Handed over, and the priest asked: Henry's report and Benesh's answer are the ones
    // written for what Henry said, with the plain pair where a half was not written.
    QuestFacts handed = at("handed");
    handed.have = 0;
    handed.how = "truth";
    QuestPlan told = plan_quest(quest, handed, {});
    CHECK(told.greeting == quest.greet_handed);
    REQUIRE(told.rows.size() == 1);
    CHECK(told.rows[0].intent == "report");
    CHECK(told.rows[0].text == "It's on the altar. And I told him the truth.");
    CHECK(told.rows[0].reply == "You told him. Christ.");
    CHECK(told.rows[0].command == kQuestComplete);
    handed.how = "pinned";
    CHECK(plan_quest(quest, handed, {}).rows[0].reply == "You said my name. To him.");
    handed.how = "covered";
    CHECK(plan_quest(quest, handed, {}).rows[0].text == quest.told);
    CHECK(plan_quest(quest, handed, {}).rows[0].reply == "Ha! A bandit. God forgive us both.");
    handed.how = "caught";
    CHECK(plan_quest(quest, handed, {}).rows[0].text == "He's sending the bailiff's men after me.");
    CHECK(plan_quest(quest, handed, {}).rows[0].reply == quest.thanks);
    handed.how = "evaded";
    CHECK(plan_quest(quest, handed, {}).rows[0].text == quest.told);
    CHECK(plan_quest(quest, handed, {}).rows[0].reply == quest.thanks);

    // Done: settled, in the thing's words.
    QuestPlan done = plan_quest(quest, at("done"), {});
    CHECK(done.block.find("took the silver chalice back to Father Adam") != std::string::npos);
    // With nothing written for after, the sheet stands as it was.
    CHECK(done.block.find("Since then") == std::string::npos);

    // What stands once it is done (npc_quest_lines since / since_<how>), over the rest of the
    // sheet: the ending's line where written, the plain one otherwise, and nothing while the
    // errand is open (notes.md 14 Oct 2026, Benesh asked after his bells).
    Quest after = quest;
    after.since_by_how = {{"", "The cup is back and your name is still the thief's."},
                          {"truth", "The priest knows it was Paul's chest; you expect your bells back."}};
    QuestFacts done_truth = at("done");
    done_truth.how = "truth";
    CHECK(plan_quest(after, done_truth, {}).block.find("Since then, and over anything else you have been told of your "
                                                       "situation: The priest knows it was Paul's chest") !=
          std::string::npos);
    QuestFacts done_covered = at("done");
    done_covered.how = "covered";
    CHECK(plan_quest(after, done_covered, {}).block.find("Since then") != std::string::npos);
    CHECK(plan_quest(after, done_covered, {}).block.find("your name is still the thief's") != std::string::npos);
    CHECK(plan_quest(after, done_covered, {}).block.find("expect your bells") == std::string::npos);
    CHECK(plan_quest(after, at("accepted"), {}).block.find("Since then") == std::string::npos);
    QuestFacts given_up = at("done");
    given_up.how = "abandoned";
    CHECK(plan_quest(after, given_up, {}).block.find("Since then") == std::string::npos);

    // The owner knows it is gone and nothing of this person; the asking is the game's menu.
    const Quest owner = chalice_back(QuestRole::Target);
    CHECK(plan_quest(owner, at("accepted"), {}).block.find("Your silver chalice is gone") != std::string::npos);
    CHECK(plan_quest(owner, at("handed"), {}).block.find("brought your silver chalice back") != std::string::npos);
    // And the owner's own line for after, on his side of the store.
    Quest owner_after = owner;
    owner_after.since_by_how = {{"truth", "You know now it was your sacristan's chest."}};
    CHECK(plan_quest(owner_after, done_truth, {}).block.find("your sacristan's chest") != std::string::npos);
    CHECK(plan_quest(owner_after, done_covered, {}).block.find("Since then") == std::string::npos);

    // The interrogation's dodges are not a truth for the scene after; the truth told is.
    CHECK(how_matches("truth", "truth"));
    CHECK(how_matches("delivered", "truth"));
    for (const char* how : {"pinned", "covered", "fibbed", "evaded", "caught"}) {
        CHECK_FALSE(how_matches(how, "truth"));
        CHECK(untrue_how(how));
    }
    CHECK(how_matches("covered", "truth|covered"));
}

namespace {

// Words Must Not Fail: Christine's prayer, written at her dictation in three parts, each
// with Henry's ways with it; the priest reads it part by part and gives a verdict.
Quest christines_prayer(QuestRole role) {
    Quest quest;
    quest.id = "b2cc2651365c";
    quest.kind = "deliver";
    quest.role = role;
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
    quest.told = "He read it. He said he'll say it at the altar.";
    quest.told_by_how = {{"cursed", "He called it a curse, not a prayer, and won't read it.", "Won't read it. Then he can rot."},
                         {"mocked", "He said no housekeeper talks so, and burned the rest.", "Burned it. What did you write?"}};
    Quest::Dictation& d = quest.dictation;
    d.begin = "Sit there, at the master's desk. Write what I say and nothing else.";
    d.done = "There. Sign it, fold it, and put the wax to it. Now take it to him.";
    d.resume = "Let's get your prayer written.";
    d.greet_unwritten = "You're back. Sit, and we'll get it written.";
    for (int k = 0; k < Quest::Dictation::kParts; ++k) {
        Quest::Dictation::Part& part = d.parts[k];
        const std::string n = std::to_string(k + 1);
        part.dictate = "Write this, part " + n + ".";
        part.write = {"I'll write it as you say it.", "Good. Every word."};
        part.soften = {"I'll put part " + n + " more gently.", "Fine. Write it so."};
        part.embellish = {"Let me give part " + n + " some weight.", "Yes. Write that."};
        if (k > 0) part.reconsider = {"Do you want part " + n + " in a prayer?", "No. Leave it out."};
    }
    return quest;
}

}  // namespace

TEST_CASE("a letter written at dictation is not written at accept, and the writer's rows wait on it") {
    const Quest quest = christines_prayer(QuestRole::Giver);
    CHECK(quest.has_dictation());
    // The first part cannot be left out; the others can, and every part can be changed.
    CHECK(quest.dictation.parts[0].outcomes() == std::vector<int>{0, 2, 3});
    CHECK(quest.dictation.parts[1].outcomes() == std::vector<int>{0, 1, 2, 3});
    CHECK(dictation_intent(2, 1) == "reconsider:2");
    CHECK(dictation_part("reconsider:2") == std::pair<int, int>(2, 1));
    CHECK(dictation_part("write:1") == std::pair<int, int>(1, 0));
    CHECK(dictation_part("embellish:3") == std::pair<int, int>(3, 3));
    CHECK(dictation_part("embellish:4") == std::pair<int, int>(0, -1));
    CHECK(dictation_part("report") == std::pair<int, int>(0, -1));
    CHECK(wrote_command("023") == "wrote_023");

    // Offered as a letter to be written, not one already written.
    QuestPlan first = plan_quest(quest, {}, {});
    CHECK(first.block.find("You cannot write") != std::string::npos);
    CHECK(first.block.find("hand it over") == std::string::npos);
    CHECK(first.question.find("write your letter down and carry it to Father Ernest") != std::string::npos);
    CHECK(first.direction.find("do not begin dictating yet") != std::string::npos);

    // Agreed and unwritten: nothing about the letter -- no report, no lie, no keep -- only
    // the row that picks the dictation up (the giver begins with its first part) and the
    // give-up row; the greeting is the one for sitting Henry down again.
    QuestFacts agreed = at("accepted");
    agreed.have = 0;
    QuestPlan unwritten = plan_quest(quest, agreed, {});
    CHECK(unwritten.block.find("not written yet") != std::string::npos);
    CHECK(unwritten.greeting == quest.dictation.greet_unwritten);
    REQUIRE(unwritten.rows.size() == 2);
    CHECK(unwritten.rows[0].intent == kDictateIntent);
    CHECK(unwritten.rows[0].text == quest.dictation.resume);
    CHECK(unwritten.rows[0].reply == quest.dictation.begin + "\n\n" + quest.dictation.parts[0].dictate);
    CHECK(unwritten.rows[0].command.empty());
    CHECK(unwritten.rows[1].intent == "give_up");

    // Written (the store's W digits) and carried: the letter's rows as for any letter, and
    // the owed greeting.
    QuestFacts written = agreed;
    written.wrote = "023";
    written.have = 1;
    QuestPlan carrying = plan_quest(quest, written, {});
    CHECK(carrying.greeting == quest.greet_owed);
    CHECK(carrying.block.find("at your dictation") != std::string::npos);
    REQUIRE(carrying.rows.size() == 3);
    CHECK(carrying.rows[0].intent == "report");
    CHECK(carrying.rows[0].command == kQuestLie);
    CHECK(carrying.rows[1].intent == "keep");

    // Delivered: the report follows the priest's verdict, as the chalice's did his question.
    QuestFacts handed = at("handed");
    handed.have = 0;
    handed.wrote = "020";
    handed.how = "cursed";
    QuestPlan cursed = plan_quest(quest, handed, {});
    REQUIRE(cursed.rows.size() == 1);
    CHECK(cursed.rows[0].text == "He called it a curse, not a prayer, and won't read it.");
    CHECK(cursed.rows[0].reply == "Won't read it. Then he can rot.");
    CHECK(cursed.rows[0].command == kQuestComplete);
    handed.how = "mocked";
    CHECK(plan_quest(quest, handed, {}).rows[0].reply == "Burned it. What did you write?");
    handed.how = "delivered";
    CHECK(plan_quest(quest, handed, {}).rows[0].text == quest.told);
    // Neither verdict is a lie of Henry's: a scene after "truth" follows both.
    CHECK(how_matches("cursed", "truth"));
    CHECK(how_matches("mocked", "truth"));

    // A quest with some of the dictation's lines and not all is not a dictation.
    Quest half = quest;
    half.dictation.parts[2].write.reply.clear();
    CHECK_FALSE(half.has_dictation());
    CHECK(plan_quest(half, agreed, {}).rows.size() == 3);

    // The receiver's side is the letter's as before.
    const Quest priest = christines_prayer(QuestRole::Target);
    CHECK(plan_quest(priest, at("accepted"), {}).block.find("know of no letter") != std::string::npos);
}

TEST_CASE("a retrieve's giver who may be lied to gets the hand-in as a row, with the lie on a Speech check beside it") {
    Quest quest = bartels_axe(QuestRole::Giver);
    quest.give = "Here. Your axe, out of Julian's shed.";
    quest.lie_have = "Julian doesn't have it. I looked.";
    quest.lie_have_reply = "Not there? Then it's gone. Thank you for looking.";
    quest.lie_have_caught = "Not there -- and what's that on your back? Give it here.";
    quest.greet_owed = "Any luck with Julian?";
    CHECK(quest.has_lie_have());

    // Agreed and carrying: nothing on sight; the giving and the lie are rows.
    QuestFacts carrying = at("accepted");
    carrying.have = 1;
    QuestPlan out = plan_quest(quest, carrying, {});
    CHECK(out.opening_commands.empty());
    CHECK(out.greeting == quest.greet_owed);
    CHECK(out.block.find("has not brought it yet, as far as you know") != std::string::npos);
    REQUIRE(out.rows.size() == 2);
    CHECK(out.rows[0].intent == "give");
    CHECK(out.rows[0].text == quest.give);
    CHECK(out.rows[0].reply == quest.thanks);
    CHECK(out.rows[0].command == kQuestComplete);
    CHECK(out.rows[0].check.empty());
    CHECK(out.rows[1].intent == "lie_have");
    CHECK(out.rows[1].text == "(Lie) " + quest.lie_have);
    CHECK(out.rows[1].reply == quest.lie_have_reply);
    CHECK(out.rows[1].command == kQuestLie);
    CHECK(out.rows[1].check == "speech");
    CHECK(out.rows[1].check_factor == doctest::Approx(1.25));
    CHECK(out.rows[1].fail_reply == quest.lie_have_caught);

    // Unasked and carrying: nobody to lie to about an errand nobody gave; settled on sight.
    QuestFacts unasked;
    unasked.have = 1;
    QuestPlan sight = plan_quest(quest, unasked, {});
    CHECK(sight.rows.empty());
    REQUIRE(sight.opening_commands.size() == 2);

    // Without the lie written, on sight as ever.
    Quest plain = quest;
    plain.lie_have.clear();
    CHECK_FALSE(plain.has_lie_have());
    CHECK(plan_quest(plain, carrying, {}).rows.empty());
    CHECK(plan_quest(plain, carrying, {}).opening_commands.size() == 1);

    // Lied and believed: done, nothing paid, the thing given up for lost.
    QuestFacts lied = at("done");
    lied.how = "lied";
    CHECK(plan_quest(quest, lied, {}).block.find("saying he never had it") != std::string::npos);
    CHECK(plan_quest(quest, lied, {}).block.find("paid") == std::string::npos);
    // Plain words where none were written.
    Quest bare = quest;
    bare.give.clear();
    bare.lie_have_caught.clear();
    QuestPlan words = plan_quest(bare, carrying, {});
    CHECK(words.rows[0].text == "Here is your work axe.");
    CHECK(words.rows[1].fail_reply.find("under your coat") != std::string::npos);
}

TEST_CASE("an errand given up to the giver's face is over: no rows, no offer, and the give-up row goes with the hand-in") {
    Quest quest = bartels_axe(QuestRole::Giver);
    QuestFacts gave_up = at("done");
    gave_up.how = "abandoned";
    QuestPlan over = plan_quest(quest, gave_up, {});
    CHECK(over.rows.empty());
    CHECK(over.question.empty());
    CHECK(over.opening_commands.empty());
    CHECK(over.greeting.empty());
    CHECK(over.block.find("giving it up") != std::string::npos);
    CHECK(over.block.find("do not ask it of them again") != std::string::npos);
    // Not a truthful ending: a scene that follows this one on the truth does not open.
    CHECK_FALSE(how_matches("abandoned", "truth"));
    CHECK(how_matches("abandoned", ""));
    // Carrying the thing back, the hand-in settles on the opening and there is nothing to give up.
    QuestPlan sight = plan_quest(quest, at("accepted", 1), {});
    CHECK(std::find(sight.opening_commands.begin(), sight.opening_commands.end(), kQuestComplete) !=
          sight.opening_commands.end());
    CHECK(sight.rows.empty());
    // A later link not yet reached: nothing to give up either.
    QuestFacts locked = at("accepted");
    locked.locked = true;
    CHECK(plan_quest(quest, locked, {}).rows.empty());
    // The holder's side has no such row.
    CHECK(plan_quest(bartels_axe(QuestRole::Target), at("accepted"), {}).rows.empty());
}

TEST_CASE("an unyielding holder's giver, told of the refusal, sends Henry to the chest and names the house-girl once") {
    Quest quest = bartels_axe(QuestRole::Giver);
    quest.unyielding = true;
    quest.helper_name = "Elsbeth";
    quest.greet_rebuffed = "He would not hear of it? Then take it from his chest. Elsbeth keeps the house.";
    quest.greet_owed = "Any luck with Julian?";
    CHECK(quest.has_helper());

    // Agreed and not yet refused: the owed visit, as any retrieve's.
    QuestFacts owed = at("accepted");
    CHECK(plan_quest(quest, owed, {}).greeting == quest.greet_owed);
    CHECK(plan_quest(quest, owed, {}).opening_commands.empty());
    CHECK(plan_quest(quest, owed, {}).block.find("Elsbeth") == std::string::npos);

    // Back with the refusal (the store's n): the line for it, the paragraph says what he is
    // telling Henry, and the naming rides on Henry's row taking it up (notes.md 13 Oct
    // 2026: a yes to give), with the give-up row behind it.
    QuestFacts rebuffed = at("accepted");
    rebuffed.rebuffed = true;
    QuestPlan out = plan_quest(quest, rebuffed, {});
    CHECK(out.greeting == quest.greet_rebuffed);
    CHECK(out.opening_commands.empty());
    CHECK(out.block.find("took offence and would not hear of it") != std::string::npos);
    CHECK(out.block.find("tell them so, and name Elsbeth") != std::string::npos);
    CHECK(out.block.find("put it to them again") != std::string::npos);
    REQUIRE(out.rows.size() == 2);
    CHECK(out.rows[0].intent == "accept_again");
    CHECK(out.rows[0].text == "His chest, then. I'll find this Elsbeth.");
    CHECK(out.rows[0].reply.empty());  // the model answers, told what became true
    CHECK(out.rows[0].command == kQuestNamed);
    CHECK(out.rows[1].intent == "give_up");
    CHECK(out.rows[1].text == kGiveUpRow);
    CHECK(out.rows[1].command == kQuestAbandon);
    CHECK(out.question.empty());
    // Written words for the row and its answer, where the lines have them.
    quest.named_row = "Then his chest it is. Where do I find this Elsbeth?";
    quest.named_reply = "Sweeping the step at Welvara's, any afternoon. Mind the master.";
    QuestPlan worded = plan_quest(quest, rebuffed, {});
    CHECK(worded.rows[0].text == quest.named_row);
    CHECK(worded.rows[0].reply == quest.named_reply);

    // Named already (the store's a): owed again, nothing fired, the paragraph in the past
    // tense, and only the give-up row left of the two.
    QuestFacts named = rebuffed;
    named.named = true;
    QuestPlan again = plan_quest(quest, named, {});
    CHECK(again.greeting == quest.greet_owed);
    CHECK(again.opening_commands.empty());
    CHECK(again.block.find("have told them so, and named Elsbeth") != std::string::npos);
    REQUIRE(again.rows.size() == 1);
    CHECK(again.rows[0].intent == "give_up");
    // Let in or keyed, the giver knows no more than that.
    named.letin = named.keyed = true;
    CHECK(plan_quest(quest, named, {}).block == again.block);

    // Back carrying it after the refusal: paid on sight, the stolen ending the mod's business.
    QuestFacts carrying = rebuffed;
    carrying.have = 1;
    QuestPlan sight = plan_quest(quest, carrying, {});
    REQUIRE(sight.opening_commands.size() == 1);
    CHECK(sight.opening_commands[0] == kQuestComplete);
    CHECK(sight.greeting == quest.thanks);

    // The holder's own paragraph: offended, and shorter still once he has said so.
    Quest holder = bartels_axe(QuestRole::Target);
    holder.unyielding = true;
    QuestFacts open = at("accepted");
    CHECK(plan_quest(holder, open, {}).block.find("take it as an accusation") != std::string::npos);
    CHECK(plan_quest(holder, rebuffed, {}).block.find("asked already and been told") != std::string::npos);
    CHECK(plan_quest(bartels_axe(QuestRole::Target), open, {}).block.find("offends you") == std::string::npos);

    // Without the helper's name or line there is nobody to name, and the refusal changes nothing.
    Quest plain = quest;
    plain.helper_name.clear();
    CHECK_FALSE(plain.has_helper());
    CHECK(plan_quest(plain, rebuffed, {}).opening_commands.empty());
    CHECK(plan_quest(plain, rebuffed, {}).greeting == quest.greet_owed);
}

TEST_CASE("a giver who can carry Henry to the errand's hour lays the rows after the yes, in the day's hours only") {
    Quest quest = bartels_axe(QuestRole::Giver);
    quest.unyielding = true;
    CHECK_FALSE(quest.has_fast_travel());
    quest.fast_travel_row = "Take me to the gate, then, and I'll wait for him.";
    quest.fast_travel_reply = "Come. I know a short way round the walls.";
    quest.fast_travel_later_row = "I've got things to do first.";
    quest.fast_travel_later_reply = "Then come back when you are free.";
    CHECK(quest.has_fast_travel());
    Approach noon;
    noon.hour = 12.0;

    // Fresh: the offer's question stands, and behind it the two rows for after the yes,
    // the carrying first, both the last word of the conversation.
    QuestPlan fresh = plan_quest(quest, {}, noon);
    CHECK_FALSE(fresh.question.empty());
    REQUIRE(fresh.rows.size() == 2);
    CHECK(fresh.rows[0].intent == "fast_travel");
    CHECK(fresh.rows[0].text == quest.fast_travel_row);
    CHECK(fresh.rows[0].reply == quest.fast_travel_reply);
    CHECK(fresh.rows[0].command == kQuestFastTravel);
    CHECK(fresh.rows[0].after_yes);
    CHECK(fresh.rows[0].closes);
    CHECK(fresh.rows[0].group == "fast_travel");
    CHECK(fresh.rows[1].intent == "fast_travel_later");
    CHECK(fresh.rows[1].text == quest.fast_travel_later_row);
    CHECK(fresh.rows[1].reply == quest.fast_travel_later_reply);
    CHECK(fresh.rows[1].command.empty());
    CHECK(fresh.rows[1].after_yes);
    CHECK(fresh.rows[1].closes);
    CHECK(fresh.rows[1].group == "fast_travel");

    // Back empty-handed: the carrying alone, on the list from the start, before the give-up row.
    QuestPlan back = plan_quest(quest, at("accepted"), noon);
    REQUIRE(back.rows.size() == 2);
    CHECK(back.rows[0].intent == "give_up");
    CHECK(back.rows[1].intent == "fast_travel");
    CHECK_FALSE(back.rows[1].after_yes);
    CHECK(back.rows[1].closes);

    // Not at night, before ten or from seven; not with the hour unread; and the written
    // hours stand over the default.
    Approach night;
    night.hour = 21.0;
    CHECK(plan_quest(quest, {}, night).rows.empty());
    Approach early;
    early.hour = 9.5;
    CHECK(plan_quest(quest, {}, early).rows.empty());
    Approach seven;
    seven.hour = 19.0;
    CHECK(plan_quest(quest, {}, seven).rows.empty());
    CHECK(plan_quest(quest, {}, {}).rows.empty());
    quest.fast_travel_from = 8.0;
    CHECK(plan_quest(quest, {}, early).rows.size() == 2);

    // Nor once the thing is in hand, handed over, or the errand done or given up.
    for (const DirectorRow& row : plan_quest(quest, at("accepted", 1), noon).rows) CHECK(row.intent != "fast_travel");
    QuestFacts handed = at("accepted");
    handed.handed = true;
    for (const DirectorRow& row : plan_quest(quest, handed, noon).rows) CHECK(row.intent != "fast_travel");
    for (const DirectorRow& row : plan_quest(quest, at("done"), noon).rows) CHECK(row.intent != "fast_travel");
}

namespace {

// The pilot's find: Mangy Margery the beggar wants four chamomile, and pays in a token.
Quest margerys_chamomile() {
    Quest quest;
    quest.id = "c9c431657008";
    quest.kind = "find";
    quest.role = QuestRole::Giver;
    quest.giver_soul = "kkut_woman_279";
    quest.giver_name = "Mangy Margery";
    quest.item_class = "7259b9bc-dfae-487e-a8bb-c1f500894e0c";
    quest.item_name = "chamomile";
    quest.count = 4;
    quest.reward_text = "a bent penny, pierced and threaded";
    quest.offer = "I need four chamomile to steep. Get them for me?";
    quest.thanks = "Finally. Here, take this bent penny.";
    return quest;
}

Quest beggars_look() {
    Quest quest;
    quest.id = "77b6f6794885";
    quest.kind = "travel";
    quest.role = QuestRole::Giver;
    quest.giver_soul = "kkut_beggar_1";
    quest.giver_name = "Old Vit";
    quest.target_soul = "kkut_man_141";
    quest.target_name = "Havel";
    quest.reward_text = "a prayer said for you";
    quest.offer = "Go and see if Havel is still at the wall. I can't be seen there.";
    quest.thanks = "At the wall. Then he lives. God keep you, lad.";
    quest.tell = "He's at the wall, same as ever, and looking well enough.";
    quest.lie = "He's gone from the wall. Nobody there knew where.";
    quest.quiet = "I went. What I saw is my own business.";
    quest.lie_reply = "Gone. Then it's begun. I'll pray for him all the same.";
    quest.quiet_reply = "Then you've had your walk for nothing, and so have I.";
    return quest;
}

}  // namespace

TEST_CASE("a travel is asked, lied about at any time, and told or kept quiet only once seen") {
    const Quest quest = beggars_look();
    // Nothing is carried, so what Henry has in his bag is nothing to it.
    QuestFacts fresh;
    fresh.have = 3;
    CHECK(offerable(quest, fresh));
    CHECK_FALSE(quest.brought(3));
    QuestPlan first = plan_quest(quest, fresh, {});
    CHECK(first.block.find("how things stand with Havel") != std::string::npos);
    CHECK(first.block.find(quest.offer) != std::string::npos);
    CHECK(first.block.find("only looked at") != std::string::npos);
    CHECK(first.question.find("look at Havel") != std::string::npos);
    CHECK(first.on_yes == kQuestAccept);
    CHECK(first.direction.find("a prayer said for you") != std::string::npos);
    CHECK(first.opening_commands.empty());
    CHECK(first.rows.empty());

    // Agreed and not yet been: the lie is the one row, paid, and nothing is owed on sight.
    QuestPlan waiting = plan_quest(quest, at("accepted"), {});
    CHECK(waiting.opening_commands.empty());
    CHECK(waiting.question.empty());
    REQUIRE(waiting.rows.size() == 2);
    CHECK(waiting.rows[1].intent == "give_up");
    CHECK(waiting.rows[0].intent == "lie");
    CHECK(waiting.rows[0].text == quest.lie);
    CHECK(waiting.rows[0].reply == quest.lie_reply);
    CHECK(waiting.rows[0].command == kQuestLie);

    // Been within sight: the truth first, then the lie, then silence, each with its answer
    // and what it owes -- the truth is the plain complete, silence is quiet and unpaid.
    QuestFacts seen = at("accepted");
    seen.seen = true;
    QuestPlan back = plan_quest(quest, seen, {});
    REQUIRE(back.rows.size() == 3);
    CHECK(back.rows[0].intent == "tell");
    CHECK(back.rows[0].text == quest.tell);
    CHECK(back.rows[0].reply == quest.thanks);
    CHECK(back.rows[0].command == kQuestComplete);
    CHECK(back.rows[1].intent == "lie");
    CHECK(back.rows[2].intent == "quiet");
    CHECK(back.rows[2].text == quest.quiet);
    CHECK(back.rows[2].reply == quest.quiet_reply);
    CHECK(back.rows[2].command == kQuestQuiet);
    CHECK(back.opening_commands.empty());

    // Unwritten lines fall back to the director's plain ones, naming the one looked at.
    Quest bare = quest;
    bare.tell.clear();
    bare.quiet_reply.clear();
    QuestPlan plain = plan_quest(bare, seen, {});
    CHECK(plain.rows[0].text.find("Havel") != std::string::npos);
    CHECK(plain.rows[2].reply.find("Nothing for nothing") != std::string::npos);

    // Done, and as the other favour of a soul on two.
    QuestPlan done = plan_quest(quest, at("done"), {});
    CHECK(done.block.find("settled") != std::string::npos);
    CHECK(done.rows.empty());
    CHECK(quest_aside(quest, at("accepted"), {quest}, {at("accepted")}).find("how things stand with Havel") !=
          std::string::npos);
    CHECK(quest_aside(quest, fresh, {quest}, {fresh}).empty());

    // The one looked at knows nothing, whatever the stage, and is never the quest of the moment.
    Quest looked = quest;
    looked.role = QuestRole::Target;
    CHECK(plan_quest(looked, seen, {}).block.find("know nothing") != std::string::npos);
    CHECK(plan_quest(looked, seen, {}).rows.empty());
    CHECK(quest_aside(looked, seen, {looked}, {seen}).empty());
    const Quest axe = bartels_axe(QuestRole::Giver);
    CHECK(pick_quest({looked, axe}, {seen, at("done")}) == 1);
}

namespace {

Quest oswalds_grudge() {
    Quest quest;
    quest.id = "d03e043982a9";
    quest.kind = "beat";
    quest.role = QuestRole::Giver;
    quest.giver_soul = "kkut_additive_man_54";
    quest.giver_name = "Oswald";
    quest.target_soul = "kkut_additive_man_55";
    quest.target_name = "Anthony";
    quest.reward_groschen = 8;
    quest.offer = "Anthony broke my hand cart and laughed. I want him to say sorry and pay for the wheel.";
    quest.thanks = "Says he will. We'll see. Here, what I promised, and I hope you're right.";
    quest.report = "Anthony's agreed. He'll say his sorry and pay for the wheel.";
    quest.fought = "He wouldn't hear of it, so I put him on the ground. He'll think twice now.";
    quest.fought_reply = "You did what? Well. He had it coming. Here, and not a word of this.";
    quest.confess = "Anthony's dead.";
    quest.dead_reply = "Dead? I wanted him bruised, not buried. Get away from me.";
    return quest;
}

Quest nickels_prayer() {
    Quest quest;
    quest.id = "665815e379c8";
    quest.kind = "pray";
    quest.role = QuestRole::Giver;
    quest.giver_soul = "kkut_man_246";
    quest.giver_name = "Nickel";
    quest.reward_groschen = 10;
    quest.spot_text = "the cross by the town gate below the Town Hall Quarter";
    quest.offer = "Say a prayer for my wife at the cross by the gate. My knees won't carry me.";
    quest.thanks = "God keep you for it. Ten groschen, as I said.";
    quest.report = "I knelt at the cross by the gate and prayed for her.";
    return quest;
}

Quest vasheks_worry() {
    Quest quest;
    quest.id = "56b3b9622348";
    quest.kind = "mind";
    quest.role = QuestRole::Giver;
    quest.giver_soul = "kkut_additive_man_307";
    quest.giver_name = "Vashek";
    quest.target_soul = "kkut_additive_man_308";
    quest.target_name = "Reginald";
    quest.reward_text = "a good word with the foreman";
    quest.count = 3;
    quest.offer = "Look in on Reginald now and then today. He's not right.";
    quest.thanks = "Still standing. Thank God. I'll speak to the foreman for you.";
    quest.report = "I've looked in on Reginald three times. He's holding up.";
    quest.confess = "Reginald's dead, Vashek.";
    quest.dead_reply = "No. No, I knew it. Leave me.";
    return quest;
}

Quest vendulas_milk() {
    Quest quest;
    quest.id = "7f22a52013d7";
    quest.kind = "leave";
    quest.role = QuestRole::Giver;
    quest.giver_soul = "tsem_woman_13";
    quest.giver_name = "Vendula";
    quest.item_name = "jug of milk";
    quest.reward_text = "a round of cheese and a basket of eggs";
    quest.spot_text = "the old idols in the wood below Semin";
    quest.offer = "Take this jug up to the old idols below the village and leave it there at midnight. Not a word to my husband.";
    quest.thanks = "Then they took it. Here, the cheese and the eggs, and not a word.";
    quest.tell = "I left it at the idols at midnight. Something cried out after, in the dark.";
    quest.lie = "I left it there. All was quiet.";
    quest.lie_reply = "Quiet. Well. Take the cheese anyway.";
    quest.quiet = "I went. What I heard is my own business.";
    quest.quiet_reply = "Then you've had your walk for nothing.";
    return quest;
}

Quest gingers_grave_goods() {
    Quest quest;
    quest.id = "e8ec9a1d5e47";
    quest.kind = "return";
    quest.role = QuestRole::Giver;
    quest.giver_soul = "kkut_additive_man_73";
    quest.giver_name = "Ginger";
    quest.item_name = "grave goods";
    quest.count = 3;
    quest.spot_text = "the coffin at the end of the wooden gallery past the crossroads, in the tunnels under the town";
    quest.offer = "Put these back on him. I'm not going down there again.";
    return quest;
}

}  // namespace

TEST_CASE("a beat asks for amends, and is paid on Henry's word: agreed, fought, or a lie; dead is unpaid") {
    const Quest quest = oswalds_grudge();
    QuestFacts fresh;
    fresh.have = 2;
    CHECK(offerable(quest, fresh));
    CHECK_FALSE(quest.brought(2));
    QuestPlan first = plan_quest(quest, fresh, {});
    CHECK(first.block.find("wronged by Anthony") != std::string::npos);
    CHECK(first.block.find("not asking for him to be beaten") != std::string::npos);
    CHECK(first.block.find(quest.offer) != std::string::npos);
    CHECK(first.question.find("make Anthony answer") != std::string::npos);
    CHECK(first.on_yes == kQuestAccept);
    CHECK(first.direction.find("a handful of groschen") != std::string::npos);
    CHECK(first.rows.empty());

    // Agreed and nothing done: the report is a lie, paid.
    QuestPlan waiting = plan_quest(quest, at("accepted"), {});
    CHECK(waiting.question.empty());
    REQUIRE(waiting.rows.size() == 2);
    CHECK(waiting.rows[1].intent == "give_up");
    CHECK(waiting.rows[0].intent == "report");
    CHECK(waiting.rows[0].text == quest.report);
    CHECK(waiting.rows[0].reply == quest.thanks);
    CHECK(waiting.rows[0].command == kQuestLie);

    // The man knocked down and no word got from him: the fought row, a complete the mod
    // turns into complete_fought off the store's beaten.
    QuestFacts beaten = at("accepted");
    beaten.beaten = true;
    QuestPlan fought = plan_quest(quest, beaten, {});
    REQUIRE(fought.rows.size() == 1);
    CHECK(fought.rows[0].intent == "fought");
    CHECK(fought.rows[0].text == quest.fought);
    CHECK(fought.rows[0].reply == quest.fought_reply);
    CHECK(fought.rows[0].command == kQuestComplete);
    CHECK(fought.block.find("did not ask for the man to be hurt") != std::string::npos);

    // His word got by the game's menu on him (handed): the same report as the lie, now the
    // truth, the plain complete -- the mod knows which log from the handover.
    QuestPlan agreed = plan_quest(quest, at("handed"), {});
    REQUIRE(agreed.rows.size() == 1);
    CHECK(agreed.rows[0].intent == "report");
    CHECK(agreed.rows[0].text == quest.report);
    CHECK(agreed.rows[0].reply == quest.thanks);
    CHECK(agreed.rows[0].command == kQuestComplete);

    // His word got and then knocked down anyway: the word is what the giver hears of.
    QuestFacts both = at("handed");
    both.beaten = true;
    CHECK(plan_quest(quest, both, {}).rows[0].intent == "report");

    // Dead: only the confession, unpaid.
    QuestFacts dead = at("accepted");
    dead.dead = true;
    QuestPlan buried = plan_quest(quest, dead, {});
    REQUIRE(buried.rows.size() == 1);
    CHECK(buried.rows[0].intent == "confess");
    CHECK(buried.rows[0].text == quest.confess);
    CHECK(buried.rows[0].reply == quest.dead_reply);
    CHECK(buried.rows[0].command == kQuestConfess);

    // Unwritten lines fall back to plain ones naming the man.
    Quest bare = quest;
    bare.fought.clear();
    bare.report.clear();
    bare.confess.clear();
    CHECK(plan_quest(bare, at("handed"), {}).rows[0].text.find("Anthony") != std::string::npos);
    CHECK(plan_quest(bare, beaten, {}).rows[0].text.find("Anthony") != std::string::npos);
    CHECK(plan_quest(bare, dead, {}).rows[0].text.find("Anthony is dead") != std::string::npos);

    // Done, and as an aside.
    CHECK(plan_quest(quest, at("done"), {}).block.find("settled") != std::string::npos);
    CHECK(quest_aside(quest, at("accepted"), {quest}, {at("accepted")}).find("Anthony") != std::string::npos);
    CHECK(quest_aside(quest, fresh, {quest}, {fresh}).empty());

    // The man himself: his own side before, the bruises after, nothing when dead.
    Quest anthony = quest;
    anthony.role = QuestRole::Target;
    CHECK(plan_quest(anthony, fresh, {}).block.find("bad blood") != std::string::npos);
    CHECK(plan_quest(anthony, beaten, {}).block.find("beat you") != std::string::npos);
    CHECK(plan_quest(anthony, at("handed"), {}).block.find("make things right") != std::string::npos);
    CHECK(plan_quest(anthony, dead, {}).block.empty());
    CHECK(quest_aside(anthony, beaten, {anthony}, {beaten}).find("beat you") != std::string::npos);
}

TEST_CASE("a pray is asked with its place, and paid on Henry's word, true once knelt") {
    const Quest quest = nickels_prayer();
    QuestFacts fresh;
    CHECK(offerable(quest, fresh));
    QuestPlan first = plan_quest(quest, fresh, {});
    CHECK(first.block.find("the cross by the town gate") != std::string::npos);
    CHECK(first.question.find("pray at the cross by the town gate") != std::string::npos);
    CHECK(first.on_yes == kQuestAccept);
    CHECK(first.direction.find("a handful of groschen") != std::string::npos);

    QuestPlan waiting = plan_quest(quest, at("accepted"), {});
    REQUIRE(waiting.rows.size() == 2);
    CHECK(waiting.rows[1].intent == "give_up");
    CHECK(waiting.rows[0].intent == "report");
    CHECK(waiting.rows[0].text == quest.report);
    CHECK(waiting.rows[0].reply == quest.thanks);
    CHECK(waiting.rows[0].command == kQuestLie);

    QuestFacts knelt = at("accepted");
    knelt.prayed = true;
    QuestPlan back = plan_quest(quest, knelt, {});
    REQUIRE(back.rows.size() == 1);
    CHECK(back.rows[0].command == kQuestComplete);

    // No place written: a plain phrase, not an empty one.
    Quest bare = quest;
    bare.spot_text.clear();
    bare.report.clear();
    CHECK(plan_quest(bare, fresh, {}).block.find("the place you named") != std::string::npos);
    CHECK(plan_quest(bare, knelt, {}).rows[0].text.find("the place you named") != std::string::npos);

    CHECK(plan_quest(quest, at("done"), {}).block.find("settled") != std::string::npos);
    CHECK(quest_aside(quest, knelt, {quest}, {knelt}).find("pray for you") != std::string::npos);

    // A player the game will not let pray at a shrine (no Basic Theology) is not asked: the
    // want is held back, and there is no question to put to the judge.
    QuestFacts unlearned;
    unlearned.cannot_pray = true;
    CHECK_FALSE(offerable(quest, unlearned));
    QuestPlan withheld = plan_quest(quest, unlearned, {});
    CHECK(withheld.block.find("do not ask") != std::string::npos);
    CHECK(withheld.question.empty());
    CHECK(withheld.rows.empty());
    // Nobody is at the other end.
    Quest nobody = quest;
    nobody.role = QuestRole::Target;
    CHECK(plan_quest(nobody, knelt, {}).block.empty());
}

TEST_CASE("a leave hands Henry the thing and is paid on his word: the truth and silence once it is down, a lie at any time") {
    const Quest quest = vendulas_milk();
    QuestFacts fresh;
    CHECK(offerable(quest, fresh));
    CHECK(quest.thingless());
    CHECK_FALSE(quest.brought(1));

    QuestPlan first = plan_quest(quest, fresh, {});
    CHECK(first.block.find("the old idols in the wood below Semin") != std::string::npos);
    CHECK(first.block.find("midnight") != std::string::npos);
    CHECK(first.block.find(quest.offer) != std::string::npos);
    CHECK(first.question.find("jug of milk") != std::string::npos);
    CHECK(first.on_yes == kQuestAccept);
    CHECK(first.direction.find("give them the jug of milk") != std::string::npos);
    CHECK(first.rows.empty());

    // Carrying it and not yet there: only the lie is on the menu, and it pays.
    QuestPlan carrying = plan_quest(quest, at("accepted", 1), {});
    REQUIRE(carrying.rows.size() == 2);
    CHECK(carrying.rows[1].intent == "give_up");
    CHECK(carrying.rows[0].intent == "lie");
    CHECK(carrying.rows[0].text == quest.lie);
    CHECK(carrying.rows[0].reply == quest.lie_reply);
    CHECK(carrying.rows[0].command == kQuestLie);

    // Put down at the place: the truth (paid) and silence (unpaid) join it, in the written words.
    QuestFacts left = at("accepted", 0);
    left.left = true;
    left.heard = true;
    QuestPlan back = plan_quest(quest, left, {});
    REQUIRE(back.rows.size() == 3);
    CHECK(back.rows[0].intent == "tell");
    CHECK(back.rows[0].text == quest.tell);
    CHECK(back.rows[0].reply == quest.thanks);
    CHECK(back.rows[0].command == kQuestComplete);
    CHECK(back.rows[1].command == kQuestLie);
    CHECK(back.rows[2].intent == "quiet");
    CHECK(back.rows[2].text == quest.quiet);
    CHECK(back.rows[2].reply == quest.quiet_reply);
    CHECK(back.rows[2].command == kQuestQuiet);

    CHECK(plan_quest(quest, at("done"), {}).block.find("settled") != std::string::npos);
    CHECK(quest_aside(quest, left, {quest}, {left}).find("jug of milk") != std::string::npos);
    QuestFacts locked;
    locked.locked = true;
    CHECK(plan_quest(quest, locked, {}).block.find("one thing at a time") != std::string::npos);
    CHECK(plan_quest(quest, locked, {}).question.empty());

    // Nothing written: plain rows, not empty ones, and the place's stand-in phrase.
    Quest bare = quest;
    bare.tell.clear();
    bare.lie.clear();
    bare.quiet.clear();
    bare.spot_text.clear();
    QuestPlan plain = plan_quest(bare, left, {});
    REQUIRE(plain.rows.size() == 3);
    for (const auto& row : plain.rows) {
        CHECK_FALSE(row.text.empty());
        CHECK_FALSE(row.reply.empty());
    }
    CHECK(plain.rows[0].text.find("cried out") != std::string::npos);
    CHECK(plan_quest(bare, fresh, {}).block.find("the place you named") != std::string::npos);
}

TEST_CASE("a return is the same errand with the things going back to the grave; the curse is not the giver's business") {
    const Quest quest = gingers_grave_goods();
    QuestFacts fresh;
    CHECK(offerable(quest, fresh));
    QuestPlan first = plan_quest(quest, fresh, {});
    CHECK(first.block.find("put your grave goods back") != std::string::npos);
    CHECK(first.block.find("the coffin at the end of the wooden gallery") != std::string::npos);
    CHECK(first.block.find("midnight") == std::string::npos);
    CHECK(first.block.find("will not go down there again") != std::string::npos);
    CHECK(first.on_yes == kQuestAccept);

    QuestFacts put = at("accepted", 0);
    put.left = true;
    put.looted = true;
    put.cursed = true;
    QuestPlan back = plan_quest(quest, put, {});
    REQUIRE(back.rows.size() == 3);
    CHECK(back.rows[0].command == kQuestComplete);
    CHECK(back.rows[0].text.find("coffin") != std::string::npos);
    CHECK(back.rows[1].command == kQuestLie);
    CHECK(back.rows[2].command == kQuestQuiet);
    // The giver never hears of the gold or the sickness.
    CHECK(back.block.find("curse") == std::string::npos);
    CHECK(back.block.find("sick") == std::string::npos);
    CHECK(quest_aside(quest, put, {quest}, {put}).find("put back") == std::string::npos);
    CHECK(quest_aside(quest, put, {quest}, {put}).find("grave goods") != std::string::npos);

    // Nobody is at the other end.
    Quest nobody = quest;
    nobody.role = QuestRole::Target;
    CHECK(plan_quest(nobody, put, {}).block.empty());
}

TEST_CASE("a mind is asked for its visits, and paid on Henry's word once they are all made") {
    const Quest quest = vasheks_worry();
    QuestFacts fresh;
    CHECK(offerable(quest, fresh));
    QuestPlan first = plan_quest(quest, fresh, {});
    CHECK(first.block.find("look in on him 3 times") != std::string::npos);
    CHECK(first.question.find("look in on Reginald") != std::string::npos);
    CHECK(first.on_yes == kQuestAccept);
    CHECK(first.direction.find("3 visits") != std::string::npos);

    // None made, one made, all made: the report is a lie until the count is in.
    QuestPlan none = plan_quest(quest, at("accepted"), {});
    REQUIRE(none.rows.size() == 2);
    CHECK(none.rows[1].intent == "give_up");
    CHECK(none.rows[0].intent == "report");
    CHECK(none.rows[0].command == kQuestLie);
    QuestFacts one = at("accepted");
    one.checks = 1;
    QuestPlan partly = plan_quest(quest, one, {});
    CHECK(partly.block.find("been once") != std::string::npos);
    CHECK(partly.rows[0].command == kQuestLie);
    QuestFacts three = at("accepted");
    three.checks = 3;
    QuestPlan all = plan_quest(quest, three, {});
    REQUIRE(all.rows.size() == 1);
    CHECK(all.rows[0].text == quest.report);
    CHECK(all.rows[0].reply == quest.thanks);
    CHECK(all.rows[0].command == kQuestComplete);

    // Dead: the confession, unpaid, and nothing else.
    QuestFacts dead = at("accepted");
    dead.dead = true;
    dead.checks = 2;
    QuestPlan gone = plan_quest(quest, dead, {});
    REQUIRE(gone.rows.size() == 1);
    CHECK(gone.rows[0].intent == "confess");
    CHECK(gone.rows[0].reply == quest.dead_reply);
    CHECK(gone.rows[0].command == kQuestConfess);

    CHECK(plan_quest(quest, at("done"), {}).block.find("settled") != std::string::npos);
    CHECK(quest_aside(quest, one, {quest}, {one}).find("Reginald") != std::string::npos);

    // The one looked in on: nothing until a visit, then the visits, nothing when dead.
    Quest reginald = quest;
    reginald.role = QuestRole::Target;
    CHECK(plan_quest(reginald, at("accepted"), {}).block.find("know nothing") != std::string::npos);
    CHECK(plan_quest(reginald, one, {}).block.find("looked in on you once") != std::string::npos);
    CHECK(plan_quest(reginald, three, {}).block.find("3 times") != std::string::npos);
    CHECK(plan_quest(reginald, dead, {}).block.empty());
}

TEST_CASE("a find is asked whatever Henry carries, and paid only for the full count") {
    const Quest quest = margerys_chamomile();
    // Before anyone agreed, with two chamomile already in the bag: still asked, since they
    // are not hers until he says so; the judge listens for a yes, and nothing is owed.
    QuestFacts fresh;
    fresh.have = 2;
    CHECK(offerable(quest, fresh));
    QuestPlan first = plan_quest(quest, fresh, {});
    CHECK(first.block.find("4 chamomile") != std::string::npos);
    CHECK(first.block.find(quest.offer) != std::string::npos);
    CHECK(first.block.find("how this person comes by them is their affair") != std::string::npos);
    CHECK(first.question.find("bring you 4 chamomile") != std::string::npos);
    CHECK(first.on_yes == kQuestAccept);
    CHECK(first.direction.find("bent penny") != std::string::npos);
    CHECK(first.opening_commands.empty());
    CHECK(first.rows.empty());

    // Agreed and short: nothing is paid, and the sheet says how many are still wanted.
    QuestFacts three = at("accepted");
    three.have = 3;
    QuestPlan waiting = plan_quest(quest, three, {});
    CHECK(waiting.opening_commands.empty());
    CHECK(waiting.question.empty());
    CHECK(waiting.block.find("They have 3 on them") != std::string::npos);
    CHECK(waiting.block.find("give nothing") != std::string::npos);

    // Agreed and carrying the four: completed on the opening turn, thanks in her words, and
    // no asking where they came from.
    QuestFacts four = at("accepted");
    four.have = 4;
    QuestPlan home = plan_quest(quest, four, {});
    REQUIRE(home.opening_commands.size() == 1);
    CHECK(home.opening_commands[0] == kQuestComplete);
    CHECK(home.block.find(quest.thanks) != std::string::npos);
    CHECK(home.block.find("do not ask") != std::string::npos);
    CHECK(home.question.empty());

    // Five is as good as four; four before agreeing is not a hand-in.
    QuestFacts five = at("accepted");
    five.have = 5;
    CHECK(plan_quest(quest, five, {}).opening_commands.size() == 1);
    QuestFacts unasked;
    unasked.have = 4;
    CHECK(plan_quest(quest, unasked, {}).opening_commands.empty());
    CHECK(plan_quest(quest, unasked, {}).on_yes == kQuestAccept);

    // Done: settled.
    QuestPlan done = plan_quest(quest, at("done"), {});
    CHECK(done.block.find("settled") != std::string::npos);
    CHECK(done.opening_commands.empty());

    // As the other favour of a soul on two.
    CHECK(quest_aside(quest, at("accepted"), {quest}, {at("accepted")}).find("agreed to bring you 4 chamomile") !=
          std::string::npos);
    CHECK(quest_aside(quest, fresh, {quest}, {fresh}).empty());

    // Among a soul's quests, the find with its count in hand is the one of the moment, and
    // a find with too few is not preferred over a retrieve whose thing is in hand.
    const Quest axe = bartels_axe(QuestRole::Giver);
    QuestFacts axe_carrying;
    axe_carrying.have = 1;
    CHECK(pick_quest({quest, axe}, {three, axe_carrying}) == 1);
    CHECK(pick_quest({quest, axe}, {four, axe_carrying}) == 0);
}

TEST_CASE("paid off on the opening line, the menu's answers are planned as for an errand done") {
    // notes.md (8 Oct 2026): with the marigolds handed over on the greeting, Joan's list
    // still offered "Tell me about the meadow flowers", answered as if she wanted them yet.
    // The complete the opening carries makes the facts done before the answers are laid.
    Soul margery;
    margery.soul = "kkut_woman_279";
    margery.name = "Mangy Margery";
    margery.quests = {margerys_chamomile()};
    margery.knowledge = {
        {"the_chamomile", "the chamomile she wants", "open", "I need four chamomile to steep, and nobody brings them."},
        {"her_stall", "her stall", "open", "I sit here by the wall most days."},
    };
    margery.remarks = {{"crop", "You've a good crop of chamomile there.", "Aye, and I'd give a penny for four of them.", false},
                       {"cold", "Cold today.", "Every day is cold at my age.", false}};
    margery.help_replies = {{"Nothing now; you brought me what I wanted.", ""}};
    Standing met;
    met.met = 2;
    QuestFacts agreed;
    agreed.accepted = true;

    // Carrying the four: the opening completes it, and the topic and the remark about the
    // thing go with it, as does the reminder; help is answered as after the hand-in.
    Approach carrying;
    carrying.have = 4;
    Persona paid = persona_for(margery, carrying, {}, true, agreed, "", met);
    REQUIRE(paid.quest.opening_commands.size() == 1);
    CHECK(paid.quest.opening_commands[0] == kQuestComplete);
    for (const Topic& topic : paid.scripted.topics) CHECK(topic.key != "the_chamomile");
    CHECK(paid.scripted.topics.size() == 1);
    CHECK(paid.scripted.remarks.size() == 1);
    CHECK(paid.scripted.remind.empty());
    CHECK(paid.scripted.help == "Nothing now; you brought me what I wanted.");

    // Short of the count, nothing is settled and she still wants to talk about them.
    Approach short_of;
    short_of.have = 3;
    Persona waiting = persona_for(margery, short_of, {}, true, agreed, "", met);
    CHECK(waiting.quest.opening_commands.empty());
    CHECK(waiting.scripted.topics.size() == 2);
    CHECK(waiting.scripted.remarks.size() == 2);
    CHECK_FALSE(waiting.scripted.remind.empty());
}

TEST_CASE("a chain's second link waits on the first and pays in kind") {
    Quest saw = bartels_saw();
    QuestFacts waiting;
    waiting.locked = true;
    CHECK_FALSE(waiting.offerable());
    QuestPlan locked = plan_quest(saw, waiting, {});
    CHECK(locked.block.find("one thing at a time") != std::string::npos);
    CHECK(locked.question.empty());

    QuestPlan open = plan_quest(saw, {}, {});
    CHECK(open.block.find("already got your work axe back") != std::string::npos);
    CHECK(open.block.find("thanks and a favour owed") != std::string::npos);
    CHECK(open.block.find("groschen") == std::string::npos);
    CHECK(open.on_yes == kQuestAccept);
    QuestPlan paid = plan_quest(saw, at("accepted", 1), {});
    CHECK(paid.block.find("give them what you said you would") != std::string::npos);
    REQUIRE(paid.opening_commands.size() == 1);
    CHECK(paid.opening_commands[0] == kQuestComplete);
}

TEST_CASE("the quest of the moment is the one the conversation turns on") {
    const std::vector<Quest> bartel = {bartels_dispute(), bartels_saw()};
    // Nothing moved: the first link.
    CHECK(pick_quest(bartel, {at("none"), at("none")}) == 0);
    // The axe out on the errand: still the axe, and the saw is not yet asked.
    CHECK(pick_quest(bartel, {at("accepted"), at("none")}) == 0);
    // The axe home: the saw.
    QuestFacts saw_open = at("none");
    CHECK(pick_quest(bartel, {at("done"), saw_open}) == 1);
    // The saw in the player's hands beats everything.
    CHECK(pick_quest(bartel, {at("done"), at("accepted", 1)}) == 1);

    // Ludwig: holds Alan's axe, wants his knife. His own favour first while it is there to
    // ask or out on the errand; the axe he holds once his knife is home.
    const std::vector<Quest> ludwig = {alans_axe_held(), ludwigs_knife()};
    CHECK(pick_quest(ludwig, {at("accepted"), at("none")}) == 1);
    CHECK(pick_quest(ludwig, {at("accepted"), at("accepted")}) == 1);
    CHECK(pick_quest(ludwig, {at("accepted"), at("done")}) == 0);
    CHECK(pick_quest(ludwig, {at("none"), at("done")}) == 0);
    CHECK(pick_quest({}, {}) == 0);
}

TEST_CASE("the other favour gets a sentence, and the nest's holder names his price") {
    const std::vector<Quest> ludwig = {alans_axe_held(), ludwigs_knife()};
    std::vector<QuestFacts> facts = {at("accepted"), at("none")};
    const std::string holding = quest_aside(ludwig[0], facts[0], ludwig, facts);
    CHECK(holding.find("Alan's work axe") != std::string::npos);
    CHECK(holding.find("until your own knife is back from Havel") != std::string::npos);
    facts[1] = at("done");
    CHECK(quest_aside(ludwig[0], facts[0], ludwig, facts).find("no more excuse") != std::string::npos);
    facts[0] = at("handed");
    CHECK(quest_aside(ludwig[0], facts[0], ludwig, facts).find("You gave Alan's work axe up") !=
          std::string::npos);

    // A giver's other errand: out, home, or -- not yet asked -- nothing at all.
    const std::vector<Quest> bartel = {bartels_dispute(), bartels_saw()};
    facts = {at("done"), at("none")};
    CHECK(quest_aside(bartel[1], facts[1], bartel, facts).empty());
    facts[1] = at("accepted");
    CHECK(quest_aside(bartel[1], facts[1], bartel, facts).find("have not brought it yet") !=
          std::string::npos);
    CHECK(quest_aside(bartel[0], facts[0], bartel, facts).find("which is settled") !=
          std::string::npos);
}


namespace {

Quest hugos_doubt() {
    Quest quest;
    quest.id = "62f0a838564e";
    quest.kind = "ask";
    quest.role = QuestRole::Giver;
    quest.giver_soul = "ksuc_poacher_3";
    quest.giver_name = "Hugo";
    quest.target_soul = "ksuc_man_63";
    quest.target_name = "William";
    quest.reward_groschen = 12;
    quest.offer = "William says the birds just died. Birds don't just die. Get the truth out of him.";
    quest.thanks = "So that's how it was. Well. Better to know. Here, twelve, as I said.";
    quest.tell = "I got it out of William. The birds were poisoned, and he knows by whom.";
    quest.lie = "I spoke to William. The birds died, like he said. Nothing in it.";
    quest.lie_reply = "Then I've been fretting over nothing. Here, you've earned it.";
    quest.quiet = "William told me. What he told me stays with me.";
    quest.quiet_reply = "Then you've wasted my time and your walk. Nothing for nothing.";
    quest.confess = "William's dead. Whatever he knew went with him.";
    quest.dead_reply = "Dead? Then I'll never know. Leave me be.";
    return quest;
}

}  // namespace

TEST_CASE("an ask wants the truth got out of somebody, and pays on Henry's word once it is told") {
    const Quest quest = hugos_doubt();
    QuestFacts fresh;
    fresh.have = 2;
    CHECK(quest.thingless());
    CHECK(offerable(quest, fresh));
    CHECK_FALSE(quest.brought(2));
    QuestPlan first = plan_quest(quest, fresh, {});
    CHECK(first.block.find("truth of the matter got out of William") != std::string::npos);
    CHECK(first.block.find("do not invent what the truth might be") != std::string::npos);
    CHECK(first.block.find(quest.offer) != std::string::npos);
    // Where William is to be found is the map's to show: a word of direction, no paces or
    // doors invented (Anna's "a few doors down"; notes.md 8 Oct 2026). Only while the
    // sending is being put.
    CHECK(first.block.find("say no more than a word of direction") != std::string::npos);
    CHECK(plan_quest(quest, at("accepted"), {}).block.find("word of direction") == std::string::npos);
    CHECK(first.question.find("get the truth of it") != std::string::npos);
    CHECK(first.on_yes == kQuestAccept);
    CHECK(first.rows.empty());

    // Agreed and nothing told: only the lie, paid; the truth and silence wait on the telling.
    QuestPlan waiting = plan_quest(quest, at("accepted"), {});
    CHECK(waiting.question.empty());
    REQUIRE(waiting.rows.size() == 2);
    CHECK(waiting.rows[1].intent == "give_up");
    CHECK(waiting.rows[0].intent == "lie");
    CHECK(waiting.rows[0].text == quest.lie);
    CHECK(waiting.rows[0].reply == quest.lie_reply);
    CHECK(waiting.rows[0].command == kQuestLie);

    // Told (the holder menu's handover): the truth, the lie and silence, in that order.
    QuestPlan told = plan_quest(quest, at("handed"), {});
    REQUIRE(told.rows.size() == 3);
    CHECK(told.rows[0].intent == "tell");
    CHECK(told.rows[0].text == quest.tell);
    CHECK(told.rows[0].reply == quest.thanks);
    CHECK(told.rows[0].command == kQuestComplete);
    CHECK(told.rows[1].command == kQuestLie);
    CHECK(told.rows[2].intent == "quiet");
    CHECK(told.rows[2].reply == quest.quiet_reply);
    CHECK(told.rows[2].command == kQuestQuiet);
    CHECK(told.greeting.empty());

    // The one who knows dead: only the confession, unpaid.
    QuestFacts dead = at("accepted");
    dead.dead = true;
    QuestPlan confess = plan_quest(quest, dead, {});
    REQUIRE(confess.rows.size() == 1);
    CHECK(confess.rows[0].intent == "confess");
    CHECK(confess.rows[0].text == quest.confess);
    CHECK(confess.rows[0].reply == quest.dead_reply);
    CHECK(confess.rows[0].command == kQuestConfess);

    CHECK(plan_quest(quest, at("done"), {}).block.find("settled between you") != std::string::npos);
    CHECK(plan_quest(quest, at("done"), {}).rows.empty());

    // The one who knows: keeps to the tale until the menu gets it out of them, then would
    // sooner not go over it; nothing at all once dead.
    Quest knows = quest;
    knows.role = QuestRole::Target;
    CHECK(plan_quest(knows, at("none"), {}).block.find("not the whole truth") != std::string::npos);
    CHECK(plan_quest(knows, at("none"), {}).question.empty());
    CHECK(plan_quest(knows, at("handed"), {}).block.find("got the truth out of you") != std::string::npos);
    CHECK(plan_quest(knows, dead, {}).block.empty());
    CHECK(quest_aside(knows, at("handed"), {knows}, {at("handed")}).find("on Hugo's account") != std::string::npos);
    CHECK(quest_aside(quest, at("accepted"), {quest}, {at("accepted")}).find("truth of a matter out of William") != std::string::npos);
    CHECK(quest_aside(quest, at("none"), {quest}, {at("none")}).empty());
}

TEST_CASE("an ambush is an ask whose drinker sends Henry wrong first; the giver's rows are the ask's") {
    Quest quest = hugos_doubt();
    quest.id = "5f8274531a7f";
    quest.kind = "ambush";
    quest.giver_name = "Ernest";
    quest.target_name = "Roger";
    quest.item_name = "directions";
    quest.tell = "Roger gave me the name. Stinky William, who sleeps by the church.";
    quest.lie = "Roger knows nobody. It was drink talking.";
    QuestFacts fresh;
    CHECK(quest.thingless());
    CHECK(offerable(quest, fresh));
    CHECK_FALSE(quest.brought(1));   // the directions are never brought to the giver
    QuestPlan first = plan_quest(quest, fresh, {});
    CHECK(first.block.find("Roger, who drinks at the Hole in the Wall") != std::string::npos);
    CHECK(first.block.find("do not invent who the curse-lifter might be") != std::string::npos);
    CHECK(first.question.find("lift the curse") != std::string::npos);
    CHECK(first.on_yes == kQuestAccept);
    CHECK(first.rows.empty());

    // The false way given and the men met change nothing at the giver's: the lie alone until
    // the true way is given (handed), then the truth, the lie and silence.
    QuestFacts directed = at("accepted");
    directed.directed = directed.ambushed = true;
    QuestPlan waiting = plan_quest(quest, directed, {});
    REQUIRE(waiting.rows.size() == 2);
    CHECK(waiting.rows[1].intent == "give_up");
    CHECK(waiting.rows[0].intent == "lie");
    CHECK(waiting.rows[0].text == quest.lie);
    CHECK(waiting.rows[0].command == kQuestLie);
    QuestPlan told = plan_quest(quest, at("handed"), {});
    REQUIRE(told.rows.size() == 3);
    CHECK(told.rows[0].intent == "tell");
    CHECK(told.rows[0].text == quest.tell);
    CHECK(told.rows[0].reply == quest.thanks);
    CHECK(told.rows[0].command == kQuestComplete);
    CHECK(told.rows[1].command == kQuestLie);
    CHECK(told.rows[2].command == kQuestQuiet);
    QuestFacts dead = at("accepted");
    dead.dead = true;
    QuestPlan confess = plan_quest(quest, dead, {});
    REQUIRE(confess.rows.size() == 1);
    CHECK(confess.rows[0].command == kQuestConfess);
    CHECK(plan_quest(quest, at("done"), {}).rows.empty());

    // The drinker: nothing unasked, the paper's secret kept once written, the fright after.
    Quest drinker = quest;
    drinker.role = QuestRole::Target;
    CHECK(plan_quest(drinker, at("none"), {}).block.find("say nothing of it unasked") != std::string::npos);
    CHECK(plan_quest(drinker, directed, {}).block.find("not the true one") != std::string::npos);
    CHECK(plan_quest(drinker, at("handed"), {}).block.find("came back on their feet") != std::string::npos);
    CHECK(plan_quest(drinker, dead, {}).block.empty());
    CHECK(quest_aside(drinker, directed, {drinker}, {directed}).find("not the true one") != std::string::npos);
    CHECK(quest_aside(quest, at("accepted"), {quest}, {at("accepted")}).find("out of Roger") != std::string::npos);
}

TEST_CASE("a buy sends Henry with the price, and pays on the charm: the truth, or a lie about the cost") {
    Quest quest = hugos_doubt();
    quest.id = "3fd6ec9c8213";
    quest.kind = "buy";
    quest.giver_name = "Ernest";
    quest.target_name = "William";
    quest.item_class = "a73dc870-f7c7-547e-ba96-e81944def5ab";
    quest.item_name = "charm";
    quest.reward_groschen = 0;
    quest.reward_text = "a rumour worth hearing";
    quest.tell = "Here's your charm. William took the hood and the twenty, as you said.";
    quest.lie = "Here's your charm. William wanted forty; I made up the rest myself.";
    quest.lie_reply = "Forty? The thief. Here's what you're out, then.";
    QuestFacts fresh;
    CHECK_FALSE(quest.thingless());
    CHECK(offerable(quest, fresh));
    CHECK(quest.brought(1));
    QuestPlan first = plan_quest(quest, fresh, {});
    CHECK(first.block.find("garment of yours off your own head") != std::string::npos);
    CHECK(first.question.find("hood and your money to William") != std::string::npos);
    CHECK(first.on_yes == kQuestAccept);
    CHECK(first.rows.empty());
    CHECK(first.opening_commands.empty());

    // Agreed and the charm not yet bought: no rows, nothing paid, no lie without the charm.
    QuestPlan waiting = plan_quest(quest, at("accepted"), {});
    REQUIRE(waiting.rows.size() == 1);  // only the give-up row
    CHECK(waiting.rows[0].intent == "give_up");
    CHECK(waiting.opening_commands.empty());
    CHECK(waiting.block.find("give nothing until the charm is in your hand") != std::string::npos);

    // The charm in hand, by the seller's menu or otherwise: the truth and the lie, both paid.
    for (const QuestFacts& carrying : {at("handed"), at("accepted", 1)}) {
        QuestPlan brought = plan_quest(quest, carrying, {});
        REQUIRE(brought.rows.size() == 2);
        CHECK(brought.rows[0].intent == "tell");
        CHECK(brought.rows[0].text == quest.tell);
        CHECK(brought.rows[0].reply == quest.thanks);
        CHECK(brought.rows[0].command == kQuestComplete);
        CHECK(brought.rows[1].intent == "lie");
        CHECK(brought.rows[1].text == quest.lie);
        CHECK(brought.rows[1].reply == quest.lie_reply);
        CHECK(brought.rows[1].command == kQuestLie);
        CHECK(brought.opening_commands.empty());   // the rows settle it, not the sight of the thing
    }
    CHECK(plan_quest(quest, at("done"), {}).rows.empty());

    // The seller: terms are the game's menu; afterwards the bargain is made.
    Quest seller = quest;
    seller.role = QuestRole::Target;
    CHECK(plan_quest(seller, at("none"), {}).block.find("nothing said in passing moves your price") != std::string::npos);
    CHECK(plan_quest(seller, at("handed"), {}).block.find("bargain is made") != std::string::npos);
    CHECK(quest_aside(seller, at("handed"), {seller}, {at("handed")}).find("bought a charm off you") != std::string::npos);
    CHECK(quest_aside(quest, at("accepted"), {quest}, {at("accepted")}).find("hood and your money") != std::string::npos);
}

TEST_CASE("a scene follows the endings its link names, and a scene the story went past is void") {
    // The mod's QuestHowMatches, token for token.
    CHECK(how_matches("persuaded", "truth"));
    CHECK(how_matches("", "truth"));          // the plain complete
    CHECK(how_matches("stolen", "truth"));
    CHECK(how_matches("fought", "truth|lied"));
    CHECK(how_matches("lied", "truth|lied"));
    CHECK_FALSE(how_matches("lied", "truth"));
    CHECK_FALSE(how_matches("quiet", "truth"));
    CHECK_FALSE(how_matches("kept", "truth"));
    CHECK_FALSE(how_matches("dead", "truth|lied"));
    CHECK(how_matches("dead", "dead"));
    CHECK(how_matches("quiet", ""));          // any ending
    CHECK_FALSE(how_matches("paid", "lied|quiet"));

    // Void: no paragraph, no aside, not offerable, whatever the kind.
    QuestFacts past;
    past.voided = true;
    CHECK_FALSE(past.offerable());
    CHECK_FALSE(offerable(hugos_doubt(), past));
    CHECK_FALSE(offerable(bartels_saw(), past));
    CHECK(plan_quest(hugos_doubt(), past, {}).block.empty());
    CHECK(plan_quest(bartels_saw(), past, {}).block.empty());
    CHECK(plan_quest(bartels_saw(), past, {}).question.empty());
    CHECK(quest_aside(bartels_saw(), past, {bartels_saw()}, {past}).empty());
    // Locked is still the chain's "one thing at a time".
    QuestFacts waiting;
    waiting.locked = true;
    CHECK(plan_quest(hugos_doubt(), waiting, {}).block.find("one thing at a time") != std::string::npos);
    // A scene's link before is not a chain's: the "already got your ... back" clause is
    // only a chain's.
    Quest scene = bartels_saw();
    scene.chain_id.clear();
    scene.after_how = "truth";
    CHECK(plan_quest(scene, {}, {}).block.find("already got your") == std::string::npos);
    CHECK(plan_quest(bartels_saw(), {}, {}).block.find("already got your") != std::string::npos);
}

TEST_CASE("a written line splits into beats on blank lines only") {
    // Two beats for Joan's offer (notes.md 7 Oct 2026): the blank line is the break, a
    // single newline is wrapping, and stray blank lines at the ends make no silent beat.
    const std::vector<std::string> one = beats_of("It's my Humphrey.\nA year gone this week.");
    REQUIRE(one.size() == 1);
    CHECK(one[0] == "It's my Humphrey.\nA year gone this week.");
    const std::vector<std::string> two = beats_of("\n\nWhy she wants them.\n \nWhat she asks.\n\n");
    REQUIRE(two.size() == 2);
    CHECK(two[0] == "Why she wants them.");
    CHECK(two[1] == "What she asks.");
    CHECK(beats_of("").size() == 1);
    CHECK(beats_of("\r\n\r\nFirst.\r\n\r\nSecond.") == std::vector<std::string>{"First.", "Second."});
}
