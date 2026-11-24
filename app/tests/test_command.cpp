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

TEST_CASE("real npcs.sqlite: Father Ernest resolves Benesh via co-located parish priest Father Adam and suppresses KB ignorance fallback") {
    if (!std::filesystem::exists("data/kcd2/npcs.sqlite")) return;

    CommandGraph graph("data/kcd2/npcs.sqlite");
    REQUIRE(graph.ok());

    const auto cands = graph.candidates_for("kkut_additive_man_138");
    CHECK(!cands.empty());
    bool has_father_adam = false;
    bool has_benesh = false;
    for (const auto& c : cands) {
        if (c.soul_name == "kkut_additive_man_135") has_father_adam = true;
        if (c.soul_name == "kkut_additive_man_52") has_benesh = true;
    }
    CHECK(has_father_adam);
    CHECK(has_benesh);

    StubLlm sidecar;
    CommandDirector dir(nullptr, &sidecar, graph);
    SoulRecord rec;
    rec.quests.emplace_back("979c4fccd41b", "accepted");  // In the Stocks

    const CommandOutcome out = dir.link(
        "kkut_additive_man_138", "Father Ernest", parse_extracted_command("LOCATE: Benesh"), {}, rec);
    CHECK(out.status == CommandStatus::ResolvedSoul);
    CHECK(out.soul_name == "kkut_additive_man_52");
    CHECK(out.directive.find("@locate kkut_additive_man_52 ") == 0);
    CHECK(out.direction.find("through Father Adam") != std::string::npos);
    CHECK(out.direction.find("do not say you do not know them") != std::string::npos);

    // Father Clement + STT mishearing "Banash Bell" -> grounded Benesh Bell (parish clerk, right by the smithy)
    const std::string clement_p1 = dir.pass1_keyterms("kkut_additive_man_137", {}, {});
    CHECK(clement_p1.find("Benesh Bell") != std::string::npos);

    const CommandOutcome clem = dir.link(
        "kkut_additive_man_137", "Father Clement", parse_extracted_command("LOCATE: Banash Bell"), {}, {});
    CHECK(clem.status == CommandStatus::ResolvedSoul);
    CHECK(clem.soul_name == "kkut_additive_man_17");
    CHECK(clem.direction.find("You know of Benesh Bell through Father Dominic") != std::string::npos);
    CHECK(clem.direction.find("parish clerk") != std::string::npos);
    CHECK(clem.direction.find("right by the smithy") != std::string::npos);
    CHECK(clem.direction.find("do not invent any other trade, shop, market, forge, street or rumour") !=
          std::string::npos);

    // Father Dominic (KbGate::Shut because his chain quest awaits Holy Jest) still locates Benesh Bell
    const CommandOutcome dom = dir.link("kkut_additive_man_136", "Father Dominic",
                                        parse_extracted_command("LOCATE: Benesh Bell"), {}, {}, KbGate::Shut);
    CHECK(dom.status == CommandStatus::ResolvedSoul);
    CHECK(dom.soul_name == "kkut_additive_man_17");

    // Stranger unknown to Father Clement gets explicit anti-hallucination abstention
    const CommandOutcome unk = dir.link(
        "kkut_additive_man_137", "Father Clement", parse_extracted_command("LOCATE: Ulrich Potter"), {}, {});
    CHECK(unk.status == CommandStatus::UnknownToNpc);
    CHECK(unk.directive.empty());
    CHECK(unk.direction.find("do not guess, suggest a market, tavern or street, or invent any rumour") !=
          std::string::npos);
}

TEST_CASE("guard town/village directory: clean_guard_subject_mention, Burgher boundaries, ambiguity, and Session mode=guard_find") {
    CHECK(clean_guard_subject_mention("Benesh Bell") == "Benesh Bell");
    CHECK(clean_guard_subject_mention("  I'm looking for Benesh Bell, please! ") == "Benesh Bell");
    CHECK(clean_guard_subject_mention("Can you tell me where the blacksmith is?") == "the blacksmith");
    CHECK(clean_guard_subject_mention("Do you know where Father Adam is right now?") == "Father Adam");

    CommandGraph graph;
    CommandCandidate benesh;
    benesh.soul_name = "soul_benesh";
    benesh.full_name = "Benesh Bell";
    benesh.given_name = "Benesh";
    benesh.byname = "Bell";
    benesh.settlement = "Kuttenberg";
    benesh.trade = "parish clerk";
    benesh.person_token = "soul_benesh";

    CommandCandidate smith1;
    smith1.soul_name = "soul_smith_1";
    smith1.full_name = "Martin Hammer";
    smith1.given_name = "Martin";
    smith1.byname = "Hammer";
    smith1.settlement = "Kuttenberg";
    smith1.trade = "blacksmith";
    smith1.person_token = "soul_smith_1";

    CommandCandidate smith2;
    smith2.soul_name = "soul_smith_2";
    smith2.full_name = "Jakub Anvil";
    smith2.given_name = "Jakub";
    smith2.byname = "Anvil";
    smith2.settlement = "Kuttenberg";
    smith2.trade = "blacksmith";
    smith2.person_token = "soul_smith_2";

    graph.hold_location("location_kutnaHora", {benesh, smith1, smith2},
                        {{"soul_benesh", {{360, 1260, 3050.0, 760.0, 42.0}}},
                         {"soul_smith_1", {{360, 1260, 2980.0, 710.0, 40.0}}},
                         {"soul_smith_2", {{360, 1260, 3110.0, 820.0, 44.0}}}});

    // Notice sidecar_llm has ZERO replies queued -- guard subject lookup bypasses Stage-1/Stage-2!
    StubLlm sidecar_llm;
    CommandDirector cmd_dir(nullptr, &sidecar_llm, graph);

    StubLlm dialogue_llm;
    FakeGame game;
    TurnWriter writer(game.paths());
    Options opts;
    Prefiller prefiller(dialogue_llm, opts);
    Session session(dialogue_llm, writer, prefiller, opts);
    session.set_world("Bohemia, 1403.");
    session.set_stream_interval(std::chrono::hours(1));
    session.set_menu(true);
    session.set_persona_source(
        [](std::string_view, const Approach&, std::string_view, const SoulRecord&) -> std::optional<Persona> {
            return Persona{"Guard Radovan", "You are Guard Radovan on watch in Kuttenberg.", "Move along."};
        });
    session.set_commands(&cmd_dir);

    // 1. Opening with mode=guard_find greets with "Who are you looking for?" (turn 1)
    //    and offers "@saylabel 13:(Type a name) @rows 0" (turn 2).
    session.handle(classify(
        "<12:00:00> BRIDGE begin npc=soul_guard mode=guard_find loc=location_kutnaHora"));
    CHECK(game.whole(1) == "Who are you looking for?\n");
    CHECK(game.whole(2) == "@saylabel 13:(Type a name) @rows 0\n");
    session.handle(classify("<12:00:01> BRIDGE played turn=1 -- Who are you looking for?"));
    session.handle(classify("<12:00:01> BRIDGE played turn=2 -- (directives only)"));

    // 2. Ambiguous query ("the blacksmith") asks which blacksmith and keeps awaiting_guard_subject_ active!
    dialogue_llm.replies.push_back("We have Martin Hammer and Jakub Anvil — which smith are you after?");
    session.say("the blacksmith", true);
    CHECK(sidecar_llm.calls.empty());
    CHECK(dialogue_llm.calls.back().messages.back().content.find("Martin Hammer (blacksmith)") !=
          std::string::npos);
    CHECK(dialogue_llm.calls.back().messages.back().content.find("Jakub Anvil (blacksmith)") !=
          std::string::npos);
    CHECK(game.whole(3).find("@locate") == std::string::npos);
    CHECK(game.whole(4) == "@saylabel 13:(Type a name) @rows 0\n");
    session.handle(classify("<12:00:02> BRIDGE played turn=3 -- Which smith?"));
    session.handle(classify("<12:00:02> BRIDGE played turn=4 -- (directives only)"));

    // 3. Clarifying with bare name ("Martin Hammer") resolves immediately and places @locate!
    dialogue_llm.replies.push_back("Martin Hammer works at the upper forge.");
    session.say("Martin Hammer", true);
    CHECK(sidecar_llm.calls.empty());
    CHECK(game.whole(5).find("@locate soul_smith_1 26:360,1260,2980.0,710.0,40.0") != std::string::npos);
    CHECK(game.whole(6).find("guard_find_again") != std::string::npos);
    CHECK(game.whole(6).find("I'm looking for someone else.") != std::string::npos);
    session.handle(classify("<12:00:03> BRIDGE played turn=5 -- Martin Hammer works at the upper forge."));
    session.handle(classify("<12:00:03> BRIDGE played turn=6 -- (directives only)"));

    // 4. Picking "I'm looking for someone else." re-prompts "Who are you looking for?"
    session.handle(classify("<12:00:04> BRIDGE picked 1 guard_find_again -- I'm looking for someone else."));
    session.handle(classify("<12:00:04> BRIDGE said -- I'm looking for someone else."));
    CHECK(game.whole(7).find("Who are you looking for?") != std::string::npos);
    CHECK(game.whole(8) == "@saylabel 13:(Type a name) @rows 0\n");
    session.handle(classify("<12:00:05> BRIDGE played turn=7 -- Who are you looking for?"));
    session.handle(classify("<12:00:05> BRIDGE played turn=8 -- (directives only)"));

    // 5. Full sentence ("I'm looking for Banash Bell") resolves phonetically to Benesh Bell!
    dialogue_llm.replies.push_back("Benesh Bell is the parish clerk by the smithy.");
    session.say("I'm looking for Banash Bell", true);
    CHECK(game.whole(9).find("@locate soul_benesh 26:360,1260,3050.0,760.0,42.0") != std::string::npos);

    // 6. Real npcs.sqlite Burgher settlement boundary check (if present on disk)
    std::filesystem::path real_db = "data/kcd2/npcs.sqlite";
    if (!std::filesystem::exists(real_db) && std::filesystem::exists("../../data/kcd2/npcs.sqlite")) {
        real_db = "../../data/kcd2/npcs.sqlite";
    }
    if (std::filesystem::exists(real_db)) {
        CommandGraph real_graph(real_db);
        REQUIRE(real_graph.ok());
        const auto kut_cands = real_graph.candidates_in_location("location_kutnaHora");
        const auto tro_cands = real_graph.candidates_in_location("location_troskovice");
        CHECK(kut_cands.size() > 500);
        CHECK(tro_cands.size() >= 20);
        CommandDirector real_dir(nullptr, &sidecar_llm, real_graph);
        const CommandOutcome real_benesh = real_dir.resolve_guard_lookup(
            "kkut_commander", "Captain Plumel", "location_kutnaHora", "Benesh Bell", {}, {});
        CHECK(real_benesh.status == CommandStatus::ResolvedSoul);
        CHECK(real_benesh.soul_name == "kkut_additive_man_17");
        // Troskowitz guard does NOT claim to know Kuttenberg-only parish clerk Benesh Bell!
        const CommandOutcome wrong_town = real_dir.resolve_guard_lookup(
            "ttro_catchpole", "Bailiff Thrush", "location_troskovice", "Benesh Bell", {}, {});
        CHECK(wrong_town.status == CommandStatus::UnknownToNpc);

        // 7. Major friendly / main-story NPC resolution in the same settlement:
        //    - "Katherine" resolves directly to ttro_katerina (NOT AmbiguousSoul against background
        //      extras "Katherine Wald" / "Katherine Braun") when she is currently in Kuttenberg!
        //    - "Katherine Wald" still resolves to the specific townswoman kkut_extras_woman_21!
        //    - Zero-routine major friendly NPCs (Rosa Ruthard, Hans Capon, Godwin) resolve when
        //      currently inside Kuttenberg via GuardPresence, including noble/priest honorifics
        //      and surname aliases ("Capon", "Lord Capon", "Sir Hans", "Father Godwin").
        //    - Traveling NPCs like Katherine do NOT falsely resolve in Suchdol when live scan
        //      shows she is NOT currently in Suchdol!
        GuardPresence kut_live;
        kut_live.has_live_scan = true;
        kut_live.guard_x = 3002.0;
        kut_live.guard_y = 750.0;
        kut_live.live_souls = {
            {"ttro_katerina", {3163.7, 655.1, 53.0}},
            {"kkut_rozaRuthard", {2993.1, 665.1, 51.1}},
            {"tkop_ptacek", {2995.0, 667.0, 51.1}},
            {"tneb_bohuta", {2996.0, 668.0, 51.1}},
        };

        const CommandOutcome kat = real_dir.resolve_guard_lookup(
            "kkut_man_131", "Guard", "location_kutnaHora", "Katherine.", {}, {}, {}, &kut_live);
        CHECK(kat.status == CommandStatus::ResolvedSoul);
        CHECK(kat.soul_name == "ttro_katerina");
        CHECK(kat.display_name == "Katherine");
        CHECK(kat.directive == "@locate ttro_katerina 24:0,1440,3163.7,655.1,53.0");
        CHECK(kat.direction.find("Kuttenberg") != std::string::npos);
        CHECK(kat.direction.find("southeast") != std::string::npos);
        CHECK(kat.direction.find("katerina") == std::string::npos);

        const CommandOutcome kat_wald = real_dir.resolve_guard_lookup(
            "kkut_man_131", "Guard", "location_kutnaHora", "Katherine Wald", {}, {}, {}, &kut_live);
        CHECK(kat_wald.status == CommandStatus::ResolvedSoul);
        CHECK(kat_wald.soul_name == "kkut_extras_woman_21");

        const CommandOutcome rosa = real_dir.resolve_guard_lookup(
            "kkut_man_131", "Guard", "location_kutnaHora", "Rosa", {}, {}, {}, &kut_live);
        CHECK(rosa.status == CommandStatus::ResolvedSoul);
        CHECK(rosa.soul_name == "kkut_rozaRuthard");
        CHECK(rosa.directive.find("@locate kkut_rozaRuthard ") == 0);
        CHECK(rosa.directive.find("0,1440,2993.1,665.1,51.1") != std::string::npos);

        for (const char* q : {"Hans Capon", "Capon", "Lord Capon", "Sir Hans"}) {
            const CommandOutcome hans = real_dir.resolve_guard_lookup(
                "kkut_man_131", "Guard", "location_kutnaHora", q, {}, {}, {}, &kut_live);
            INFO("query=" << q);
            CHECK(hans.status == CommandStatus::ResolvedSoul);
            CHECK(hans.soul_name == "tkop_ptacek");
            CHECK(hans.direction.find("ptacek") == std::string::npos);
        }

        for (const char* q : {"Godwin", "Father Godwin"}) {
            const CommandOutcome godwin = real_dir.resolve_guard_lookup(
                "kkut_man_131", "Guard", "location_kutnaHora", q, {}, {}, {}, &kut_live);
            INFO("query=" << q);
            CHECK(godwin.status == CommandStatus::ResolvedSoul);
            CHECK(godwin.soul_name == "tneb_bohuta");
            CHECK(godwin.direction.find("bohuta") == std::string::npos);
        }

        // Title-prefixed resident ("Blacksmith Radovan" in Tachov) resolves on bare "Radovan"
        // without colliding with possessive-prefixed "Radovan's wife Milena" (ttac_woman_1),
        // while "Milena" resolves directly to ttac_woman_1!
        const CommandOutcome radovan = real_dir.resolve_guard_lookup(
            "ttac_guard", "Guard", "location_tachov", "Radovan", {}, {});
        CHECK(radovan.status == CommandStatus::ResolvedSoul);
        CHECK(radovan.soul_name == "ttac_blacksmith");

        const CommandOutcome milena = real_dir.resolve_guard_lookup(
            "ttac_guard", "Guard", "location_tachov", "Milena", {}, {});
        CHECK(milena.status == CommandStatus::ResolvedSoul);
        CHECK(milena.soul_name == "ttac_woman_1");

        // "Katharine" (and "Catherine" / "Catharine" / "Katerina") also resolves directly
        // to main-story Katherine (`ttro_katerina`), BOTH when live-loaded AND when her
        // `layers/katerina_*.xml` entity is unloaded at a distant Kuttenberg gate!
        for (const char* q : {"Katharine.", "Katharine", "Catherine", "Catharine", "Katerina"}) {
            const CommandOutcome kat_sp = real_dir.resolve_guard_lookup(
                "kkut_man_306", "Franz", "location_kutnaHora", q, {}, {}, {}, &kut_live);
            INFO("query=" << q);
            CHECK(kat_sp.status == CommandStatus::ResolvedSoul);
            CHECK(kat_sp.soul_name == "ttro_katerina");
            CHECK(kat_sp.display_name == "Katherine");
        }

        // Exact reproduction of wl1 West Gate (`kkut_man_306` at 2989,722) when only nearby
        // `objects_mission0.xml` `kkut_gerda` is loaded and `layers/katerina_*.xml` is unloaded:
        GuardPresence kut_gate_unloaded;
        kut_gate_unloaded.has_live_scan = true;
        kut_gate_unloaded.guard_x = 2989.0;
        kut_gate_unloaded.guard_y = 722.0;
        kut_gate_unloaded.live_souls = {{"kkut_gerda", {2961.9, 743.4, 56.0}}};
        kut_gate_unloaded.absent_souls = {"tkop_ptacek"};

        const CommandOutcome kat_from_gate = real_dir.resolve_guard_lookup(
            "kkut_man_306", "Franz", "location_kutnaHora", "Katharine.", {}, {}, {}, &kut_gate_unloaded);
        CHECK(kat_from_gate.status == CommandStatus::ResolvedSoul);
        CHECK(kat_from_gate.soul_name == "ttro_katerina");
        CHECK(kat_from_gate.display_name == "Katherine");

        // Conversely, if Katherine's entity WAS loaded and observed outside Kuttenberg
        // (`absent_souls` contains `ttro_katerina`), she is excluded from Kuttenberg:
        GuardPresence kut_kat_moved_away = kut_gate_unloaded;
        kut_kat_moved_away.absent_souls.insert("ttro_katerina");
        const CommandOutcome kat_moved = real_dir.resolve_guard_lookup(
            "kkut_man_306", "Franz", "location_kutnaHora", "Katherine", {}, {}, {}, &kut_kat_moved_away);
        CHECK(kat_moved.soul_name != "ttro_katerina");

        // When Katherine is in Kuttenberg (NOT Suchdol), a Suchdol guard does NOT falsely
        // report Katherine in Suchdol despite her static Suchdol routine rows in npcs.sqlite!
        GuardPresence suchdol_live;
        suchdol_live.has_live_scan = true;
        const CommandOutcome kat_absent = real_dir.resolve_guard_lookup(
            "ksuc_guard", "Guard", "location_suchdol", "Katharine", {}, {}, {}, &suchdol_live);
        CHECK(kat_absent.soul_name != "ttro_katerina");

        // End-to-end Session integration with BRIDGE guard_present markers before BRIDGE begin:
        StubLlm real_dlg_llm;
        FakeGame real_game;
        TurnWriter real_writer(real_game.paths());
        Prefiller real_prefiller(real_dlg_llm, opts);
        Session real_session(real_dlg_llm, real_writer, real_prefiller, opts);
        real_session.set_world("Bohemia, 1403.");
        real_session.set_stream_interval(std::chrono::hours(1));
        real_session.set_menu(true);
        real_session.set_persona_source(
            [](std::string_view, const Approach&, std::string_view, const SoulRecord&) -> std::optional<Persona> {
                return Persona{"Guard", "You are a Kuttenberg guard.", "God be with you."};
            });
        real_session.set_commands(&real_dir);

        real_session.handle(classify("<12:10:00> [moonshine_names] BRIDGE guard_present reset loc=location_kutnaHora"));
        real_session.handle(classify(
            "<12:10:00> [moonshine_names] BRIDGE guard_present add ttro_katerina@3163.7,655.1,53.0;tkop_ptacek@2995.0,667.0,51.1"));
        real_session.handle(classify(
            "<12:10:00> [moonshine_names] BRIDGE begin npc=kkut_man_131 x=3002.0 y=750.0 mode=guard_find loc=location_kutnaHora"));
        real_session.handle(classify("<12:10:01> BRIDGE played turn=1 -- Who are you looking for?"));
        real_session.handle(classify("<12:10:01> BRIDGE played turn=2 -- (directives only)"));

        real_dlg_llm.replies.push_back("You'll find Katherine down at the Hangman's Halter tavern to the southeast.");
        real_session.say("Katherine.", true);
        CHECK(real_game.whole(3).find("@locate ttro_katerina 24:0,1440,3163.7,655.1,53.0") != std::string::npos);
        CHECK(real_dlg_llm.calls.back().messages.back().content.find("katerina") == std::string::npos);
    }
}

TEST_CASE("Co-patrons at Havel's Tavern: Christina Alder knows Paul Schmidt on Where-is and Do-you-know without leaking secrets") {
    // 1. Stage1Gate weights clamp threshold <= 0.35 so "Where is Paul Schmidt?" (0.773) passes!
    std::filesystem::path weights_path = "tools/command_bench/stage1_gate_weights.json";
    if (!std::filesystem::exists(weights_path) &&
        std::filesystem::exists("../../tools/command_bench/stage1_gate_weights.json")) {
        weights_path = "../../tools/command_bench/stage1_gate_weights.json";
    }
    if (std::filesystem::exists(weights_path)) {
        const auto gate = Stage1Gate::load_json(weights_path);
        REQUIRE(gate.has_value());
        CHECK(gate->loaded());
        CHECK(gate->threshold() <= 0.351f);
    }

    // 2. gamelog parses `BRIDGE nearby reset/add` markers cleanly.
    const Event nr = classify("<16:00:02> [Lua] [moonshine] BRIDGE nearby reset npc=kkut_woman_22");
    CHECK(nr.marker == Marker::Nearby);
    CHECK(nr.intent == "reset");
    CHECK(nr.soul == "kkut_woman_22");
    const Event na = classify(
        "<16:00:02> [Lua] [moonshine] BRIDGE nearby add npc=kkut_woman_22 kkut_man_51@3093.0,872.0,60.4;kkut_havel@3096.0,876.0,60.4");
    CHECK(na.marker == Marker::Nearby);
    CHECK(na.intent == "add");
    CHECK(na.soul == "kkut_woman_22");
    CHECK(na.said == "kkut_man_51@3093.0,872.0,60.4;kkut_havel@3096.0,876.0,60.4");

    std::filesystem::path real_db = "data/kcd2/npcs.sqlite";
    if (!std::filesystem::exists(real_db) && std::filesystem::exists("../../data/kcd2/npcs.sqlite")) {
        real_db = "../../data/kcd2/npcs.sqlite";
    }
    if (!std::filesystem::exists(real_db)) return;

    CommandGraph real_graph(real_db);
    REQUIRE(real_graph.ok());

    StubLlm sidecar_llm;
    CommandDirector dir(nullptr, &sidecar_llm, real_graph);

    // Christina Alder (`kkut_woman_22`) seated at Havel's Tavern (`at=3094,875`), as in wl1 log:
    GuardPresence havel_tavern;
    havel_tavern.guard_x = 3094.0;
    havel_tavern.guard_y = 875.0;

    // 3. "Where is Paul Schmidt?" (and bare "Where is Paul?") resolves to Paul Schmidt (`kkut_man_51`),
    //    places `@locate` at his Havel's Tavern `routine_day_target` table (`3093.0,872.0,60.4`),
    //    includes his Havel's Tavern `life:day` habit, and NEVER leaks Sentence 2's stolen chalice!
    sidecar_llm.replies.push_back("LOCATE: Paul Schmidt");
    const CommandOutcome loc_paul = dir.resolve(
        "kkut_woman_22", "Christina Alder", "Kuttenberg",
        "Where is Paul Schmidt?", {}, {}, KbGate::Open, {}, {}, &havel_tavern);
    CHECK(loc_paul.status == CommandStatus::ResolvedSoul);
    CHECK(loc_paul.soul_name == "kkut_man_51");
    CHECK(loc_paul.display_name == "Paul Schmidt");
    CHECK(loc_paul.directive.find("@locate kkut_man_51 ") == 0);
    CHECK(loc_paul.directive.find("450,1440,3093.0,872.0,60.4") != std::string::npos);
    CHECK(loc_paul.direction.find("Havel's tavern") != std::string::npos);
    CHECK(loc_paul.direction.find("Ceska street") != std::string::npos);
    CHECK(loc_paul.direction.find("do not say you do not know them") != std::string::npos);
    CHECK(loc_paul.direction.find("chalice") == std::string::npos);
    CHECK(loc_paul.direction.find("false bottom") == std::string::npos);

    // Bare "Where is Paul?" disambiguates to Paul Schmidt (`kkut_man_51`) when *In the Stocks*
    // (`979c4fccd41b`) is active in `record.quests` (over generic miner `Paul` `kkut_additive_man_124`).
    SoulRecord active_stocks;
    active_stocks.quests.push_back({"979c4fccd41b", "accepted"});
    sidecar_llm.replies.push_back("LOCATE: Paul");
    const CommandOutcome loc_bare_paul = dir.resolve(
        "kkut_woman_22", "Christina Alder", "Kuttenberg",
        "Where is Paul?", {}, active_stocks, KbGate::Open, {}, {}, &havel_tavern);
    CHECK(loc_bare_paul.status == CommandStatus::ResolvedSoul);
    CHECK(loc_bare_paul.soul_name == "kkut_man_51");

    // 4. Non-command acquaintance questions ("Do you know Paul Schmidt?", "Who is Paul Schmidt?",
    //    "Tell me about Paul Schmidt", "Does Paul Schmidt drink here?") resolve via `resolve_acquaintance`!
    for (const char* q : {
             "Do you know Paul Schmidt?",
             "Who is Paul Schmidt?",
             "Tell me about Paul Schmidt.",
             "Does Paul Schmidt drink here?",
         }) {
        INFO("acquaintance query=" << q);
        const CommandOutcome acq = dir.resolve_acquaintance(
            "kkut_woman_22", "Christina Alder", q, {}, {}, KbGate::Open, &havel_tavern);
        CHECK(acq.status == CommandStatus::ResolvedSoul);
        CHECK(acq.action == CommandAction::None);
        CHECK(acq.directive.empty());
        CHECK(acq.soul_name == "kkut_man_51");
        CHECK(acq.heard_token == "kkut_man_51");
        CHECK(acq.direction.find("Havel's tavern") != std::string::npos);
        CHECK(acq.direction.find("chalice") == std::string::npos);
    }

    // Ordinary non-person questions never false-trigger `resolve_acquaintance`.
    for (const char* q : {
             "What do you think of Sigismund?",
             "How is the beer tonight?",
             "Do you know anything about the silver mines?",
         }) {
        INFO("non-person query=" << q);
        const CommandOutcome acq = dir.resolve_acquaintance(
            "kkut_woman_22", "Christina Alder", q, {}, {}, KbGate::Open, &havel_tavern);
        CHECK(acq.status == CommandStatus::NoAction);
        CHECK(acq.direction.empty());
    }

    // 5. Remembered tavern venue in `SoulRecord::memory` (`kMemoryGap` with `at=3094,875 in=true`):
    //    when Christina is met the next day out on Kupecka Street (`at=3195,812`), she still remembers
    //    Paul Schmidt from Havel's Tavern!
    SoulRecord remembered_tavern;
    remembered_tavern.soul = "kkut_woman_22";
    remembered_tavern.memory.push_back(
        {std::string(kMemoryGap), 1000.0, "rch=0.5 at=3094,875 in=true nspc=24 hour=22.2"});
    GuardPresence next_day_street;
    next_day_street.guard_x = 3195.0;
    next_day_street.guard_y = 812.0;
    const CommandOutcome from_memory = dir.resolve_acquaintance(
        "kkut_woman_22", "Christina Alder", "Do you know Paul Schmidt?",
        {}, remembered_tavern, KbGate::Open, &next_day_street);
    CHECK(from_memory.status == CommandStatus::ResolvedSoul);
    CHECK(from_memory.soul_name == "kkut_man_51");
    CHECK(from_memory.direction.find("from the tavern") != std::string::npos);

    // 6. Live `BRIDGE nearby` co-presence: Paul Schmidt knows Christina Alder when she is sitting
    //    3 metres away at Havel's Tavern (`nearby_souls`), even though Christina's static `npc_routine`
    //    rows are on Kupecka Street (`3195,812`)!
    GuardPresence paul_live;
    paul_live.guard_x = 3093.0;
    paul_live.guard_y = 872.0;
    paul_live.nearby_souls["kkut_woman_22"] = {3094.0, 875.0, 60.4};
    const CommandOutcome paul_knows_christina = dir.resolve_acquaintance(
        "kkut_man_51", "Paul Schmidt", "Do you know Christina Alder?",
        {}, {}, KbGate::Open, &paul_live);
    CHECK(paul_knows_christina.status == CommandStatus::ResolvedSoul);
    CHECK(paul_knows_christina.soul_name == "kkut_woman_22");
    CHECK(paul_knows_christina.direction.find("right nearby here right now") != std::string::npos);

    // 7. End-to-end Session test with `BRIDGE nearby` + `at=3094,875`: verifies both `"Where is Paul Schmidt?"`
    //    and `"Do you know Paul Schmidt?"` inject grounded facts and suppress `"you know nothing of it"`!
    StubLlm dlg_llm;
    FakeGame game;
    Options opts;
    TurnWriter writer(game.paths());
    Prefiller prefiller(dlg_llm, opts);
    Session session(dlg_llm, writer, prefiller, opts);
    session.set_world("Bohemia, 1403.");
    session.set_stream_interval(std::chrono::hours(1));
    session.set_persona_source(
        [](std::string_view, const Approach&, std::string_view, const SoulRecord&) -> std::optional<Persona> {
            return Persona{"Christina Alder", "You are Christina Alder, a carpenter's wife in Kuttenberg.", "Good evening."};
        });
    session.set_commands(&dir);

    session.handle(classify("<16:00:02> [Lua] [moonshine] BRIDGE nearby reset npc=kkut_woman_22"));
    session.handle(classify("<16:00:02> [Lua] [moonshine] BRIDGE nearby add npc=kkut_woman_22 kkut_man_51@3093.0,872.0,60.4"));
    session.handle(classify(
        "<16:00:02> [Lua] [moonshine] BRIDGE begin npc=kkut_woman_22 doing=SittingIdle prop=beer at=3094,875 in=true hour=22.28"));
    session.handle(classify("<16:00:03> BRIDGE played turn=1 -- Good evening."));

    sidecar_llm.replies.push_back("NONE");
    dlg_llm.replies.push_back("Paul Schmidt? Aye, he sits right over there with a jug most days.");
    session.say("Do you know Paul Schmidt?", true);
    REQUIRE(!dlg_llm.calls.empty());
    const std::string prompt_acq = dlg_llm.calls.back().messages.back().content;
    CHECK(prompt_acq.find("Paul Schmidt") != std::string::npos);
    CHECK(prompt_acq.find("Havel's tavern") != std::string::npos);
    CHECK(prompt_acq.find("you know nothing of it") == std::string::npos);
    CHECK(game.whole(2).find("@heard kkut_man_51") != std::string::npos);
    session.handle(classify("<16:00:05> BRIDGE played turn=2 -- Paul Schmidt? Aye, he sits right over there with a jug most days."));

    sidecar_llm.replies.push_back("LOCATE: Paul Schmidt");
    dlg_llm.replies.push_back("Paul's right over at the table here in Havel's tavern, or back at the gravedigger's house on Ceska street.");
    session.say("Where is Paul Schmidt?", true);
    CHECK(game.whole(3).find("@locate kkut_man_51 ") != std::string::npos);
    CHECK(game.whole(3).find("0,1440,3094.0,875.0,60.4") == std::string::npos);
    CHECK(game.whole(3).find("0,1440,3093.0,872.0,60.4") != std::string::npos);
}


