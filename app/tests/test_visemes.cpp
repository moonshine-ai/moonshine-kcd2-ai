#include "doctest.h"

#include <cmath>
#include <filesystem>
#include <fstream>

#include "lipsync.h"
#include "visemes.h"

using namespace moonshine;

TEST_CASE("IPA splits into phonemes, stress dropped, diphthongs and length kept") {
    // "ɡˈʊd" "mˈɔɹnɪŋ" as the G2P writes good morning
    CHECK(ipa_phonemes("ɡˈʊd") == std::vector<std::string>{"ɡ", "ʊ", "d"});
    CHECK(ipa_phonemes("mˈɔɹnɪŋ") == std::vector<std::string>{"m", "ɔ", "ɹ", "n", "ɪ", "ŋ"});
    CHECK(ipa_phonemes("tʃˈɝtʃ") == std::vector<std::string>{"tʃ", "ɝ", "tʃ"});
    CHECK(ipa_phonemes("fˈɑːðɚ") == std::vector<std::string>{"f", "ɑː", "ð", "ɚ"});
    CHECK(ipa_phonemes("naʊ") == std::vector<std::string>{"n", "aʊ"});
    CHECK(ipa_phonemes("").empty());
    CHECK(ipa_phonemes("ˈ").empty());
}

TEST_CASE("each phoneme has a viseme and a length; an unknown symbol has none") {
    double length = 0;
    CHECK(viseme_of("m", &length) == "PBM");
    CHECK(length == doctest::Approx(0.8));
    CHECK(viseme_of("f") == "FV");
    CHECK(viseme_of("tʃ") == "SH");
    CHECK(viseme_of("oʊ") == "OH");
    CHECK(viseme_of("uː") == "UW");
    CHECK(viseme_of("i") == "IY");
    CHECK(viseme_of("?", &length).empty());
    CHECK(length == 0);
    // Every table entry names a known viseme.
    for (const char* v : kVisemes) CHECK(std::string(v).size() >= 1);
    CHECK(std::string(kVisemes[0]) == "SIL");
}

TEST_CASE("a word's span is shared among its phonemes by their lengths") {
    // "me" = m (0.8) + i (1.0) over 0.9 s from 1.0: m until 1.4, i until 1.9, then SIL.
    const std::vector<WordSpan> words = {{"me", 1.0, 1.9}};
    const std::vector<Mouth> m = mouth_of(words, {"mˈi"});
    REQUIRE(m.size() == 3);
    CHECK(m[0].viseme == "PBM");
    CHECK(m[0].at == doctest::Approx(1.0));
    CHECK(m[1].viseme == "IY");
    CHECK(m[1].at == doctest::Approx(1.4));
    CHECK(m[2].viseme == "SIL");
    CHECK(m[2].at == doctest::Approx(1.9));
}

TEST_CASE("words close together run on; a gap rests the lips") {
    const std::vector<WordSpan> words = {{"my", 0.0, 0.4}, {"name", 0.45, 0.9}, {"is", 1.3, 1.6}};
    const std::vector<Mouth> m = mouth_of(words, {"maɪ", "neɪm", "ɪz"});
    // my: PBM AA; name: TD EH PBM; (gap >= 0.15) SIL; is: IY TD; SIL
    std::vector<std::string> seen;
    for (const Mouth& x : m) seen.push_back(x.viseme);
    CHECK(seen == std::vector<std::string>{"PBM", "AA", "TD", "EH", "PBM", "SIL", "IY", "TD", "SIL"});
    CHECK(m[5].at == doctest::Approx(0.9));
    CHECK(m[6].at == doctest::Approx(1.3));
    CHECK(m.back().at == doctest::Approx(1.6));
    for (std::size_t i = 1; i < m.size(); ++i) CHECK(m[i].at >= m[i - 1].at);
}

TEST_CASE("a flicker is dropped, a closure is kept, repeats merge") {
    // "ten" in 0.1 s: t (0.5) e (1.0) n (0.7) -> TD 0.023 s, EH 0.045 s, TD 0.032 s. The
    // first TD opens the line (kept), EH is over the threshold, the last TD is brief and
    // dropped; "pop" in 0.08 s keeps both closures (PBM) and drops the 0.038 s OH between
    // them, so the two PBMs merge into one.
    std::vector<Mouth> m = mouth_of({{"ten", 0.0, 0.1}, {"pop", 0.1, 0.18}}, {"tɛn", "pɒp"});
    std::vector<std::string> seen;
    for (const Mouth& x : m) seen.push_back(x.viseme);
    CHECK(seen == std::vector<std::string>{"TD", "EH", "PBM", "SIL"});
    CHECK(m.back().at == doctest::Approx(0.18));
    // A word with no phonemes is AX.
    m = mouth_of({{"...", 0.0, 0.5}}, {""});
    REQUIRE(m.size() == 2);
    CHECK(m[0].viseme == "AX");
    CHECK(m[1].viseme == "SIL");
    // Fewer IPA words than words: the rest are AX too.
    m = mouth_of({{"a", 0.0, 0.2}, {"b", 0.2, 0.4}}, {"ə"});
    REQUIRE(m.size() == 2);
    CHECK(m[0].viseme == "AX");
    CHECK(m[1].viseme == "SIL");
    // Nothing from nothing, or from a word of no length.
    CHECK(mouth_of({}, {}).empty());
    CHECK(mouth_of({{"x", 1.0, 1.0}}, {"ks"}).empty());
}

TEST_CASE("a word's letters read as sounds of the right class") {
    // Shapes, not phonetics: the classes are what matter.
    auto classes = [](std::string_view word) {
        std::string out;
        for (const std::string& p : ipa_phonemes(letters_ipa(word))) {
            if (!out.empty()) out += ' ';
            out += viseme_of(p);
        }
        return out;
    };
    CHECK(classes("miller") == "PBM IY TD AX");
    CHECK(classes("Kuttenberg") == "KG AA TD EH TD PBM AX KG");  // u as in cut
    CHECK(classes("sheep") == "SH IY PBM");
    CHECK(classes("church") == "SH AX SH");
    CHECK(classes("stone") == "TD TD OH TD");  // the final e silent
    CHECK(classes("the") == "TH EH");
    CHECK(classes("yes") == "IY EH TD");
    CHECK(classes("city") == "TD IY TD IY");
    CHECK(classes("O'Neill") == "OH TD EH TD");  // ei as in eight
    CHECK(letters_ipa("...").empty());
    CHECK(letters_ipa("").empty());
    // The model's own marks are read: a glottal stop, a syllabic n.
    CHECK(ipa_phonemes("kˈʌʔn̩bˌɜːɡ") == std::vector<std::string>{"k", "ʌ", "ʔ", "n", "b", "ɜː", "ɡ"});
}

TEST_CASE("the lexicon answers by the word's letters, and the letters fill in for what it lacks") {
    const std::filesystem::path file = std::filesystem::temp_directory_path() / "moonshine_test_lexicon.tsv";
    {
        std::ofstream out(file);
        out << "miller\tmˈɪlɚ\nbailiff\tbˈeɪləf\nbailiff\tbˈeɪlɪf\nthe\tðə\n# not a word\n\nodd line without tab\n";
    }
    Lexicon lex;
    REQUIRE(lex.load(file.string()));
    CHECK(lex.size() == 4);
    CHECK(lex.find("Miller") == "mˈɪlɚ");
    CHECK(lex.find("miller,") == "mˈɪlɚ");
    CHECK(lex.find("\"The\"") == "ðə");
    CHECK(lex.find("bailiff") == "bˈeɪləf");  // the first entry of a heteronym
    CHECK(lex.find("miller's") == "mˈɪlɚz");
    CHECK(lex.find("miller\xE2\x80\x99s") == "mˈɪlɚz");  // a curly apostrophe
    CHECK(lex.find("groschen").empty());
    CHECK(lex.ipa("groschen") == letters_ipa("groschen"));
    CHECK(lex.ipa("miller") == "mˈɪlɚ");
    CHECK(lex.ipa("...").empty());
    std::filesystem::remove(file);
    Lexicon none;
    CHECK_FALSE(none.load((std::filesystem::temp_directory_path() / "no_such_lexicon.tsv").string()));
    CHECK(none.size() == 0);
    CHECK(none.ipa("stone") == letters_ipa("stone"));
}

TEST_CASE("the mouth cue lists the shapes with their seconds") {
    const std::string body = lip_mouth("npc", 123, {{0.0, "PBM"}, {0.41267, "AA"}, {1.0, "SIL"}});
    CHECK(body == "lip npc 123\nmouth 0.000 PBM\nmouth 0.413 AA\nmouth 1.000 SIL\n");
    CHECK(lip_mouth("henry", 5, {}) == "lip henry 5\n");
}
