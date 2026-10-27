#include "doctest.h"

#include <fstream>

#include "voice.h"

using namespace moonshine;

TEST_CASE("a kokoro_ prefix is not part of the voice id") {
    CHECK(plain_voice_id("kokoro_bm_p286") == "bm_p286");
    CHECK(plain_voice_id("bm_p286") == "bm_p286");
    CHECK(plain_voice_id("  kokoro_bf_emma  ") == "bf_emma");
    CHECK(plain_voice_id("").empty());
}

TEST_CASE("a soul is spoken with the stand-in until the actors are cleared") {
    CHECK(stand_in_voice("female") == "pete-female");
    CHECK(stand_in_voice("male") == "pete-male");
    CHECK(stand_in_voice("not-defined") == "pete-male");
    CHECK(stand_in_voice("") == "pete-male");
    CHECK(clone_for("henry", "pete-female") == "pete-henry");
    CHECK(clone_for("female", "") == "pete-female");
    CHECK(clone_for("npc", "pete-female") == "pete-female");
    CHECK(clone_for("npc", "") == "pete-male");
}

TEST_CASE("a voice id is the actor's abbreviation") {
    CHECK(actor_abbr(146) == "amon");
    CHECK(actor_abbr(81) == "dbro");
    CHECK(actor_abbr(108) == "tmck");
    CHECK(actor_abbr(0).empty());
    CHECK(actor_abbr(-1).empty());
}

TEST_CASE("the imported catalogue is the id and who it is") {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "moonshine-voices-test";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir / "voices.txt");
        out << "# a note\n\n"
            << "bm_p286\tNewcastle man\n"
            << "kokoro_bf_p269\tNewcastle woman\n";
    }
    std::ofstream(dir / "bm_extra.kokorovoice") << "not a real voice";

    const std::vector<ImportedVoice> voices = imported_voices(dir);
    REQUIRE(voices.size() == 3);
    CHECK(voices[0].id == "bm_p286");
    CHECK(voices[0].about == "Newcastle man");
    CHECK(voices[1].id == "bf_p269");
    CHECK(voices[1].about == "Newcastle woman");
    CHECK(voices[2].id == "bm_extra");
    CHECK(voices[2].about.empty());

    std::filesystem::remove_all(dir);
}
