#include "doctest.h"

#include <string>

#include "inspect.h"
#include "prompt.h"

using namespace moonshine;

namespace {

// A soul assembled by hand rather than read from a database, because what is being tested
// is the handing over, not the looking up.
Soul bartel() {
    Soul soul;
    soul.soul = "kkut_additive_man_47";
    soul.name = "Bartel";
    soul.occupation = "varlet";
    soul.social_class = "varlet";
    soul.settlement = "Kuttenberg";
    soul.region = "Kuttenberg region";
    soul.gender = "male";
    soul.card = "You are tired, damp, and out of patience.";
    soul.greeting_first = "What is it? Make it quick.";
    soul.greeting_again = "You again. Make it quick.";
    return soul;
}

}  // namespace

TEST_CASE("a newline inside a character sheet does not break the JSON around it") {
    // The bug this exists for: std::quoted takes a std::string_view, so it is found by
    // argument-dependent lookup from an unqualified call and escapes only the quotes. Every
    // sheet has a paragraph break in it, so every one of them went out malformed.
    CHECK(json_string("two\nlines") == "\"two\\nlines\"");
    CHECK(json_string("a \"quoted\" word") == "\"a \\\"quoted\\\" word\"");
    CHECK(json_string("back\\slash") == "\"back\\\\slash\"");
    CHECK(json_string("tab\there") == "\"tab\\there\"");
    CHECK(json_string(std::string("bell\x07")) == "\"bell\\u0007\"");
    CHECK(json_string("") == "\"\"");
}

TEST_CASE("a Bohemian name keeps its bytes rather than being escaped into safety") {
    // Mojibake in the playground would be read as the app mangling names, and there is
    // nothing about UTF-8 that JSON needs escaped.
    CHECK(json_string("Frantiŝek") == "\"Frantiŝek\"");
    CHECK(json_string("\u2019") == "\"\u2019\"");
}

TEST_CASE("the handed-over system message is the one a real turn would use") {
    const Soul soul = bartel();
    const std::string world = "A kingdom, in 1403.";
    const std::string json = persona_json(soul, world, Approach{.blood = 0.5}, Doing{}, Options{}, 45);

    // The claim the playground rests on: it does not assemble a prompt, it is given one.
    // Asserted by building the same thing the other way and finding it in there.
    const Persona persona = persona_for(soul, Approach{.blood = 0.5});
    CHECK(json.find(json_string(system_prompt(world, persona))) != std::string::npos);

    // And both halves separately, since the playground shows which part is shared and so
    // already prefilled, and which is charged for again per soul.
    CHECK(json.find(json_string(shared_prefix(world))) != std::string::npos);
    CHECK(json.find(json_string(persona.sheet)) != std::string::npos);
}

TEST_CASE("every piece of the prompt is handed over labelled with where it came from") {
    const Soul soul = bartel();
    const std::string world = "A kingdom, in 1403.";
    const std::string json =
        persona_json(soul, world, Approach{.blood = 0.5}, Doing{"sawing wood", "working"},
                     Options{}, 45);

    // The shared prefix's own two halves, because one is a setting somebody might change and the
    // other is the speaking rules that are true of every soul in the game.
    CHECK(json.find("\"world\": " + json_string(world)) != std::string::npos);
    CHECK(json.find("\"speaking\": " + json_string(kSpeaking)) != std::string::npos);

    // And the sheet broken into the blocks it was built from. Not recoverable by splitting the
    // finished prompt, which is the whole reason for reporting it.
    for (const auto& [label, text] : persona_for(soul, Approach{.blood = 0.5},
                                                 Doing{"sawing wood", "working"}).parts) {
        CAPTURE(label);
        CHECK(json.find("{ \"label\": " + json_string(label) + ", \"text\": " +
                        json_string(text) + " }") != std::string::npos);
    }
    // The state the player walked up in is one of the blocks, so a caller can point at what
    // changes between two conversations with the same soul without diffing two prompts.
    CHECK(json.find("{ \"label\": \"approach\", \"text\": \"You are busy sawing wood. Someone "
                    "who is bloody walks up to you.\" }") != std::string::npos);
}

TEST_CASE("the greeting handed over is the written line, whatever the state asked about") {
    const std::string world = "A kingdom, in 1403.";
    // The state goes into the prose; the line is the same for a bloody man and a clean one.
    for (const Approach approach : {Approach{}, Approach{.blood = 0.5}, Approach{.dirt = 0.95}}) {
        CHECK(persona_json(bartel(), world, approach, Doing{}, Options{}, 45)
                  .find("\"greeting\": " + json_string("What is it? Make it quick.")) !=
              std::string::npos);
    }
}

TEST_CASE("with written greetings off, the words are still handed over but not to be said") {
    const std::string world = "A kingdom, in 1403.";
    const std::string quiet =
        persona_json(bartel(), world, {}, Doing{}, Options{}, 45, /*say_written=*/false);

    // Nothing to say, and the reason said out loud. Empty on its own is ambiguous -- a soul
    // nobody wrote for looks identical -- and the page draws a different note for each.
    CHECK(quiet.find("\"greeting\": \"\"") != std::string::npos);
    CHECK(quiet.find("\"say_written\": false") != std::string::npos);

    // The written lines still arrive, because the panel that lists them is worth reading either
    // way.
    CHECK(quiet.find(json_string("What is it? Make it quick.")) != std::string::npos);

    CHECK(persona_json(bartel(), world, {}, Doing{}, Options{}, 45)
              .find("\"say_written\": true") != std::string::npos);
}

TEST_CASE("which half the greeting came from is reported, not left to be inferred") {
    const std::string world = "A kingdom, in 1403.";
    // A caller holding a persona has to be able to say afterwards what it was built for. The
    // playground uses it to explain itself when an open conversation was begun by a stranger and
    // the panel has since moved on to showing the reunion lines.
    const std::string stranger = persona_json(bartel(), world, {}, Doing{}, Options{}, 45);
    CHECK(stranger.find("\"met\": false") != std::string::npos);
    CHECK(stranger.find(json_string("What is it? Make it quick.")) != std::string::npos);

    const std::string again =
        persona_json(bartel(), world, Approach{.met = true}, Doing{}, Options{}, 45);
    CHECK(again.find("\"met\": true") != std::string::npos);
    CHECK(again.find(json_string("You again. Make it quick.")) != std::string::npos);
}

TEST_CASE("both written lines are handed over, so the playground can show the other") {
    // Only one is said, but the panel shows both, and it must not have to ask the app twice.
    const std::string json = persona_json(bartel(), "A kingdom.", {}, Doing{}, Options{}, 45);
    CHECK(json.find("\"greetings\": { \"first\": " + json_string("What is it? Make it quick.") +
                    ", \"again\": " + json_string("You again. Make it quick.") + " }") !=
          std::string::npos);
}

TEST_CASE("the sampling handed over is the app's own, not the playground's idea of it") {
    // The playground exists to predict what the game will do. Left to pick its own
    // temperature it would answer a question nobody asked.
    const Options options;
    const std::string json = persona_json(bartel(), "A kingdom.", {}, Doing{}, options, 45);
    CHECK(json.find("\"model\": " + json_string(options.model)) != std::string::npos);
    CHECK(json.find("\"top_k\": " + std::to_string(options.top_k)) != std::string::npos);
    CHECK(json.find("\"disable_thinking\": true") != std::string::npos);
    // Read back as itself rather than as 0.94999999999999996.
    CHECK(json.find("\"top_p\": 0.95") != std::string::npos);
    CHECK(json.find("\"temperature\": 1") != std::string::npos);
    // The speech buffer too, since the playground reports when the voice would start and
    // would be answering about its own threshold otherwise.
    CHECK(json.find("\"buffer_chars\": 45") != std::string::npos);
}

TEST_CASE("a soul with nothing written says so, rather than offering an empty greeting") {
    Soul soul = bartel();
    soul.card.clear();
    soul.greeting_first.clear();
    soul.greeting_again.clear();
    const std::string json = persona_json(soul, "A kingdom.", {}, Doing{}, Options{}, 45);

    // Which is what sends the playground down the generated-opening path, the slow one
    // worth being able to see.
    CHECK(json.find("\"greeting\": \"\"") != std::string::npos);
    CHECK(json.find("\"greetings\": { \"first\": \"\", \"again\": \"\" }") != std::string::npos);
    CHECK(json.find("\"card\": false") != std::string::npos);
}
