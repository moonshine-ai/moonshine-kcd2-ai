#include "doctest.h"

#include <algorithm>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include "voice.h"

using namespace moonshine;

TEST_CASE("a kokoro_ prefix is not part of the voice id") {
    CHECK(plain_voice_id("kokoro_bm_p286") == "bm_p286");
    CHECK(plain_voice_id("bm_p286") == "bm_p286");
    CHECK(plain_voice_id("  kokoro_bf_emma  ") == "bf_emma");
    CHECK(plain_voice_id("").empty());
}

TEST_CASE("a line that is nobody's in particular is spoken by the stand-in") {
    CHECK(stand_in_voice("female") == "maeve");
    CHECK(stand_in_voice("male") == "toby");
    CHECK(stand_in_voice("not-defined") == "toby");
    CHECK(stand_in_voice("") == "toby");
    CHECK(clone_for("henry", "maeve") == "spencer");
    CHECK(clone_for("female", "") == "maeve");
    CHECK(clone_for("npc", "maeve") == "maeve");
    CHECK(clone_for("npc", "") == "toby");
}

TEST_CASE("a soul's voice is picked from their gender and age, and is theirs for good") {
    const std::vector<std::string> men = {"toby", "alfie", "edwin", "hugh", "teddy"};
    const std::vector<std::string> women = {"maeve", "freya", "pippa", "elodie-rose", "imogen", "lottie"};
    auto one_of = [](const std::vector<std::string>& pool, const std::string& voice) {
        return std::find(pool.begin(), pool.end(), voice) != pool.end();
    };
    // The same soul, the same voice; a different soul of the same kind, often another.
    // The values are the hash's, pinned so a change to it is noticed: it moves every
    // soul's voice, which a player would hear.
    CHECK(voice_for("male", "adult", "miller_bartel") == "teddy");
    CHECK(voice_for("male", "middle-aged", "kkut_additive_man_47") == "edwin");
    CHECK(voice_for("female", "adult", "w7") == "lottie");
    std::set<std::string> seen;
    for (int i = 0; i < 40; ++i) seen.insert(voice_for("male", "adult", "soul_" + std::to_string(i)));
    CHECK(seen.size() >= 3);
    // Henry's voice is nobody else's.
    for (int i = 0; i < 200; ++i) {
        for (const char* age : {"young", "adult", "middle-aged", "elderly", "unspecified", ""}) {
            const std::string man = voice_for("male", age, "m" + std::to_string(i));
            CHECK(man != kHenryVoice);
            CHECK(one_of(men, man));
            CHECK(one_of(women, voice_for("female", age, "w" + std::to_string(i))));
        }
    }
    // Age narrows the pool: a boy is never spoken by the middle-aged men, nor an old man
    // by the catalogue's young one. Geoffrey the carter, whose voice Henry took, has
    // teddy in its place.
    CHECK(voice_for("male", "middle-aged", "kkut_extras_man_167") == "teddy");
    for (int i = 0; i < 100; ++i) {
        const std::string young = voice_for("male", "young", "y" + std::to_string(i));
        CHECK((young == "alfie" || young == "toby"));
        CHECK(voice_for("male", "elderly", "e" + std::to_string(i)) != "alfie");
        CHECK(voice_for("female", "elderly", "e" + std::to_string(i)) != "freya");
    }
    // No gender is a man; no soul is the stand-in.
    CHECK(one_of(men, voice_for("not-defined", "adult", "nobody_knows")));
    CHECK(voice_for("male", "adult", "") == "toby");
    CHECK(voice_for("female", "young", "") == "maeve");
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
