#include "doctest.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

#include "command.h"
#include "fake_game.h"
#include "session.h"
#include "stub_llm.h"

using namespace moonshine;

namespace {

CommandCandidate make_cand(std::string soul,
                           std::string full_name,
                           std::string given,
                           std::string gender = "female",
                           int hop = 1,
                           double closeness = 0.9,
                           std::string trade = "",
                           std::vector<std::string> roles = {}) {
    CommandCandidate c;
    c.soul_name = std::move(soul);
    c.full_name = std::move(full_name);
    c.given_name = std::move(given);
    c.gender = std::move(gender);
    c.settlement = "Troskowitz";
    c.trade = std::move(trade);
    c.roles = std::move(roles);
    c.hop = hop;
    c.closeness = closeness;
    c.person_token = c.soul_name;
    return c;
}

}  // namespace

TEST_CASE("command parser handles compact GBNF and FunctionGemma formats") {
    const ExtractedCommand c1 = parse_extracted_command("LOCATE: Margaret\n");
    CHECK(c1.action == CommandAction::LocateNpc);
    CHECK(c1.target_mention == "Margaret");
    CHECK_FALSE(c1.is_pronoun);

    const ExtractedCommand c2 = parse_extracted_command("LOCATE: her");
    CHECK(c2.action == CommandAction::LocateNpc);
    CHECK(c2.target_mention == "her");
    CHECK(c2.is_pronoun);

    const ExtractedCommand c3 = parse_extracted_command(
        "<start_function_call>call:locate_npc{target_mention:<escape>Father Ernest<escape>}<end_function_call>");
    CHECK(c3.action == CommandAction::LocateNpc);
    CHECK(c3.target_mention == "Father Ernest");

    const ExtractedCommand c4 = parse_extracted_command("NONE");
    CHECK(c4.action == CommandAction::None);
}

TEST_CASE("medieval Bohemian and German phonetic keys align common ASR variants") {
    CHECK(command_phonetic_key("Konrad") == command_phonetic_key("Conrad"));
    CHECK(command_phonetic_key("Yarka") == command_phonetic_key("Jarka"));
    const bool schmidt_close =
        command_phonetic_key("Schmidt") == command_phonetic_key("Schmitz") ||
        jaro_winkler_similarity(command_phonetic_key("Schmidt"),
                                command_phonetic_key("Schmitz")) >= 0.80;
    CHECK(schmidt_close);
    CHECK(command_phonetic_key("Miskowitz") == command_phonetic_key("Miskovice"));
}

TEST_CASE("social-graph entity linker resolves names, roles, homonyms, pronouns, absentees, and strangers") {
    CommandGraph graph;
    CommandCandidate margaret = make_cand("soul_margaret", "Margaret", "Margaret", "female", 1, 0.90, "spinner");
    CommandCandidate cecily = make_cand("soul_cecily", "Cecily Miller", "Cecily", "female", 1, 0.85, "miller's wife");
    CommandCandidate richard = make_cand("soul_richard", "Richard", "Richard", "male", 1, 0.95, "apprentice", {"brother"});
    CommandCandidate robert = make_cand("soul_robert", "Robert Mailer", "Robert", "male", 2, 0.45, "armourer");
    CommandCandidate jan_far = make_cand("soul_jan_far", "Jan", "Jan", "male", 2, 0.40, "bailiff");
    CommandCandidate jan_near = make_cand("soul_jan_near", "Jan", "Jan", "male", 1, 0.90, "blacksmith");
    CommandCandidate absentee;
    absentee.full_name = "Constance";
    absentee.given_name = "Constance";
    absentee.gender = "female";
    absentee.hop = 1;
    absentee.closeness = 0.80;
    absentee.is_absentee = true;
    absentee.why_unmeetable = "fled to Prague before the siege";

    graph.hold("speaker_andrew",
               {margaret, cecily, richard, robert, jan_far, jan_near, absentee},
               {{"soul_margaret", {{480, 1080, 1250.5, 2340.0, 42.0}}}});

    StubLlm sidecar;
    CommandDirector dir(nullptr, &sidecar, graph);
    SoulRecord rec;

    SUBCASE("direct Hop-1 locate formats @locate with routine spots and @heard token") {
        const CommandOutcome out = dir.link(
            "speaker_andrew", "Andrew", parse_extracted_command("LOCATE: Margaret"), {}, rec);
        CHECK(out.status == CommandStatus::ResolvedSoul);
        CHECK(out.soul_name == "soul_margaret");
        CHECK(out.heard_token == "soul_margaret");
        CHECK(out.directive == "@locate soul_margaret 27:480,1080,1250.5,2340.0,42.0");
    }

    SUBCASE("kinship role and trade resolve cleanly") {
        const CommandOutcome kin = dir.link(
            "speaker_andrew", "Andrew", parse_extracted_command("LOCATE: your brother"), {}, rec);
        CHECK(kin.status == CommandStatus::ResolvedSoul);
        CHECK(kin.soul_name == "soul_richard");

        const CommandOutcome tr = dir.link(
            "speaker_andrew", "Andrew", parse_extracted_command("LOCATE: the blacksmith"), {}, rec);
        CHECK(tr.status == CommandStatus::ResolvedSoul);
        CHECK(tr.soul_name == "soul_jan_near");
    }

    SUBCASE("homonym disambiguation prefers salient Hop-1 peer over Hop-2 namesake") {
        const CommandOutcome out = dir.link(
            "speaker_andrew", "Andrew", parse_extracted_command("LOCATE: Jan"), {}, rec);
        CHECK(out.status == CommandStatus::ResolvedSoul);
        CHECK(out.soul_name == "soul_jan_near");
    }

    SUBCASE("centering-theory pronoun coreference picks topic subject over oblique mention") {
        const std::vector<Message> hist = {
            {"user", "Tell me about Richard."},
            {"assistant", "Richard works as an apprentice under Robert Mailer near the gate."},
        };
        const CommandOutcome out = dir.link(
            "speaker_andrew", "Andrew", parse_extracted_command("LOCATE: him"), hist, rec);
        CHECK(out.status == CommandStatus::ResolvedSoul);
        CHECK(out.soul_name == "soul_richard");
    }

    SUBCASE("absentee abstains from @locate and explains why unmeetable") {
        const CommandOutcome out = dir.link(
            "speaker_andrew", "Andrew", parse_extracted_command("LOCATE: Constance"), {}, rec);
        CHECK(out.status == CommandStatus::UnmeetableAbsentee);
        CHECK(out.directive.empty());
        CHECK(out.direction.find("fled to Prague") != std::string::npos);
    }

    SUBCASE("multi-word stranger shares surname but given-name guard abstains") {
        const CommandOutcome out = dir.link(
            "speaker_andrew", "Andrew", parse_extracted_command("LOCATE: Gilbert Miller"), {}, rec);
        CHECK(out.status == CommandStatus::UnknownToNpc);
        CHECK(out.directive.empty());
    }

    SUBCASE("trust gate suppresses @locate until favour is settled") {
        const CommandOutcome out = dir.link(
            "speaker_andrew", "Andrew", parse_extracted_command("LOCATE: Margaret"), {}, rec, KbGate::Asking);
        CHECK(out.status == CommandStatus::GatedByTrust);
        CHECK(out.directive.empty());
    }

    SUBCASE("follow and travel commands emit extensible directives") {
        const CommandOutcome fl = dir.link(
            "speaker_andrew", "Andrew", parse_extracted_command("FOLLOW: me"), {}, rec);
        CHECK(fl.status == CommandStatus::ResolvedSoul);
        CHECK(fl.directive == "@follow speaker_andrew");

        const CommandOutcome tv = dir.link(
            "speaker_andrew", "Andrew", parse_extracted_command("TRAVEL: Trosky Castle"), {}, rec);
        CHECK(tv.status == CommandStatus::ResolvedPlace);
        CHECK(tv.place_id == "trosky_castle");
        CHECK(tv.directive == "@travel trosky_castle");
    }
}

TEST_CASE("conditional Pass-2 STT replay recovers misheard Hop-2 medieval name") {
    CommandGraph graph;
    CommandCandidate jarda = make_cand("soul_jarda", "Jarda Tanner", "Jarda", "male", 2, 0.50, "tanner");
    graph.hold("speaker_hens", {jarda}, {{"soul_jarda", {{0, 0, 500.0, 600.0, 10.0}}}});

    StubLlm sidecar;
    // Pass 1 heard "Zorda Planer" (below surface threshold -> UnknownToNpc); Pass 2 replay hears "Jarda Tanner"
    sidecar.replies.push_back("LOCATE: Zorda Planer");
    sidecar.replies.push_back("LOCATE: Jarda Tanner");

    CommandDirector dir(nullptr, &sidecar, graph);
    SoulRecord rec;
    std::string captured_keyterms;
    const CommandOutcome out = dir.resolve(
        "speaker_hens", "Hens", "Kuttenberg",
        "Where can I find Zorda Planer?", {}, rec, KbGate::Open,
        [&](std::string_view keyterms) {
            captured_keyterms = std::string(keyterms);
            return "Where can I find Jarda Tanner?";
        });

    CHECK(out.pass2_used);
    CHECK(out.status == CommandStatus::ResolvedSoul);
    CHECK(out.soul_name == "soul_jarda");
    CHECK(captured_keyterms.find("Jarda Tanner") != std::string::npos);
}

TEST_CASE("Session emits @locate on command turns while preserving KnowledgeDirector and system KV prefix") {
    FakeGame game;
    StubLlm dialogue_llm;
    StubLlm sidecar_llm;
    TurnWriter writer(game.paths());
    Options opts;
    Prefiller prefiller(dialogue_llm, opts);
    Session session(dialogue_llm, writer, prefiller, opts);
    session.set_world("Bohemia, 1403.");
    session.set_stream_interval(std::chrono::hours(1));

    session.set_persona_source(
        [](std::string_view, const Approach&, std::string_view, const SoulRecord&) -> std::optional<Persona> {
            return Persona{"Andrew", "You are Andrew, an innkeeper in Troskowitz.", "God greet you."};
        });

    CommandGraph graph;
    graph.hold("soul_andrew",
               {make_cand("soul_margaret", "Margaret", "Margaret", "female", 1, 0.90, "spinner")},
               {{"soul_margaret", {{480, 1080, 100.0, 200.0, 15.0}}}});
    CommandDirector cmd_dir(nullptr, &sidecar_llm, graph);
    session.set_commands(&cmd_dir);

    // Open conversation & play written greeting (turn 1)
    session.handle(classify("<12:00:00> BRIDGE begin npc=soul_andrew"));
    session.handle(classify("<12:00:01> BRIDGE played turn=1 -- God greet you."));

    // Turn 2: Biographical question -> sidecar says NONE -> no @locate directive
    sidecar_llm.replies.push_back("NONE");
    dialogue_llm.replies.push_back("Margaret was born down in Semine.");
    session.say("Where was Margaret born?", true);
    const std::string turn2 = game.whole(2);
    CHECK(turn2.find("@locate") == std::string::npos);
    const std::string sys_turn2 = dialogue_llm.calls.back().messages.front().content;
    session.handle(classify("<12:00:02> BRIDGE played turn=2 -- Margaret was born down in Semine."));

    // Turn 3: Locate command -> sidecar says LOCATE: Margaret -> @locate + @heard emitted, system prompt unchanged!
    sidecar_llm.replies.push_back("LOCATE: Margaret");
    dialogue_llm.replies.push_back("Look by the spinner's bench behind the inn.");
    session.say("Where can I find Margaret right now?", true);
    const std::string turn3 = game.whole(3);
    CHECK(turn3.find("@locate soul_margaret 25:480,1080,100.0,200.0,15.0") != std::string::npos);
    CHECK(turn3.find("@heard soul_margaret") != std::string::npos);
    CHECK(dialogue_llm.calls.back().messages.front().content == sys_turn2);
}
