#include "doctest.h"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "phrase_cache.h"

using namespace moonshine;

namespace {

std::vector<std::int16_t> voiced(int rate, double seconds) {
    // A vowel: a fundamental and its harmonics, loudness moving the way speech does.
    // Silence compresses further than the bitrate asks, and noise does not, so neither
    // is what the size is judged on.
    std::vector<std::int16_t> pcm(static_cast<std::size_t>(static_cast<double>(rate) * seconds));
    for (std::size_t i = 0; i < pcm.size(); ++i) {
        const double t = static_cast<double>(i) / rate;
        // Quiet, then loud, so a decode that lost the front of the line is obvious.
        const double env = t < seconds * 0.4 ? 0.05 : 1.0;
        double sample = 0;
        for (int h = 1; h <= 6; ++h) sample += std::sin(2 * 3.141592653589793 * 140.0 * h * t) / h;
        const double scaled = sample * env * 9000.0;
        pcm[i] = static_cast<std::int16_t>(scaled > 32767 ? 32767 : scaled < -32767 ? -32767 : scaled);
    }
    return pcm;
}

double rms(const std::int16_t* pcm, std::size_t count) {
    if (count == 0) return 0;
    double sum = 0;
    for (std::size_t i = 0; i < count; ++i) sum += static_cast<double>(pcm[i]) * pcm[i];
    return std::sqrt(sum / static_cast<double>(count));
}

}  // namespace

TEST_CASE("the words are flattened the way the voice matches a line") {
    CHECK(phrase_flat("  a  b\nc\t") == "a b c");
    CHECK(phrase_flat("already flat") == "already flat");
    CHECK(phrase_flat("\n\n").empty());
    CHECK(phrase_key("pete-female", "a  b") == phrase_key("pete-female", "a b"));
    CHECK(phrase_key("pete-female", "a b") != phrase_key("pete-male", "a b"));
    CHECK(phrase_key("pete-female", "a b") != phrase_key("pete-female", "a c"));
}

TEST_CASE("the file name is the sha256 of the actor and the words") {
    // actor, a newline, then the flattened words. "\nabc" is the empty actor saying abc.
    CHECK(phrase_key("", "abc") ==
          "f39d39bfc99287b848780d4110eb663f0e7c344ae41f46a390c4135b89dc2756");
}

TEST_CASE("a line comes back the same length, and sixteen times smaller than 16 kHz PCM") {
    const int rate = 24000;
    for (const double seconds : {0.5, 3.0}) {
        const std::vector<std::int16_t> pcm = voiced(rate, seconds);
        const std::vector<std::uint8_t> bytes = encode_opus(pcm.data(), pcm.size(), rate);
        const double ratio = phrase_compression(bytes.size(), pcm.size(), rate);
        INFO("seconds " << seconds << " bytes " << bytes.size() << " ratio " << ratio);
        CHECK(ratio >= 10.0);
        CHECK(ratio <= 20.0);
        const PhraseAudio back = decode_opus(bytes.data(), bytes.size());
        CHECK(back.rate == rate);
        CHECK(back.pcm.size() == pcm.size());
        // The shape survives: the loud half is still the loud half. A delay of a
        // frame would move it, and a decode that dropped the lookahead would not.
        const std::size_t quiet = pcm.size() * 4 / 10;
        CHECK(rms(pcm.data(), quiet) < rms(pcm.data() + quiet, pcm.size() - quiet));
        CHECK(rms(back.pcm.data(), quiet) < rms(back.pcm.data() + quiet, back.pcm.size() - quiet));
        CHECK(rms(back.pcm.data(), back.pcm.size()) > 500);
    }
}

TEST_CASE("a stored line is found by its words, and a different actor is not") {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "moonshine-phrase-cache-test";
    std::filesystem::remove_all(dir);
    PhraseCache cache(dir);
    const std::vector<std::int16_t> pcm = voiced(24000, 0.4);
    cache.store("pete-female", "Write this.\nFather Ernest.", pcm.data(), pcm.size(), 24000);
    cache.store("pete-male", "Go with God, lad.", pcm.data(), pcm.size(), 24000);

    PhraseCache again(dir);
    CHECK(again.size() == 2);
    const std::optional<PhraseAudio> hit = again.find("pete-female", "Write this. Father Ernest.");
    REQUIRE(hit.has_value());
    CHECK(hit->rate == 24000);
    CHECK(hit->pcm.size() == pcm.size());
    CHECK_FALSE(again.find("pete-male", "Write this. Father Ernest."));
    CHECK_FALSE(again.find("pete-female", "something else"));
    CHECK_FALSE(again.find("pete-female", ""));

    std::filesystem::remove_all(dir);
}
