#include "doctest.h"

#include <cmath>

#include "word_align.h"

using namespace moonshine;

namespace {

// Attention that dwells on token t for frames [bounds[t], bounds[t+1]), with a little
// spread onto the neighbours and noise elsewhere, in `heads` heads.
std::vector<float> dwelling(const std::vector<int>& bounds, int frames, int heads, int tokens) {
    std::vector<float> w(static_cast<std::size_t>(frames) * heads * tokens, 0.01f);
    for (int f = 0; f < frames; ++f) {
        int on = tokens - 1;
        for (int t = 0; t + 1 < static_cast<int>(bounds.size()); ++t) {
            if (f >= bounds[t] && f < bounds[t + 1]) on = t;
        }
        for (int h = 0; h < heads; ++h) {
            float* row = w.data() + (static_cast<std::size_t>(f) * heads + h) * tokens;
            row[on] = 0.6f + 0.05f * h;
            if (on > 0) row[on - 1] += 0.1f;
            if (on + 1 < tokens) row[on + 1] += 0.1f;
        }
    }
    return w;
}

}  // namespace

TEST_CASE("the path follows attention that walks along the text") {
    const std::vector<int> bounds = {0, 5, 9, 16, 20};  // 4 tokens over 20 frames
    const std::vector<float> w = dwelling(bounds, 20, 2, 4);
    const std::vector<int> starts = align_tokens(w, 20, 2, 4);
    REQUIRE(starts.size() == 4);
    CHECK(starts[0] == 0);
    CHECK(std::abs(starts[1] - 5) <= 1);
    CHECK(std::abs(starts[2] - 9) <= 1);
    CHECK(std::abs(starts[3] - 16) <= 1);
    for (std::size_t i = 1; i < starts.size(); ++i) CHECK(starts[i] >= starts[i - 1]);
}

TEST_CASE("a token the attention never settles on still gets a place in order") {
    // Three tokens; the middle one is never attended to.
    const std::vector<int> bounds = {0, 8, 8, 16};
    const std::vector<float> w = dwelling(bounds, 16, 1, 3);
    const std::vector<int> starts = align_tokens(w, 16, 1, 3);
    CHECK(starts[0] == 0);
    CHECK(starts[1] >= starts[0]);
    CHECK(starts[2] >= starts[1]);
    CHECK(std::abs(starts[2] - 8) <= 1);
}

TEST_CASE("empty inputs align to nothing") {
    CHECK(align_tokens({}, 0, 1, 3) == std::vector<int>{0, 0, 0});
    CHECK(align_tokens({}, 5, 1, 0).empty());
    CHECK(align_tokens(std::vector<float>(3, 0.f), 5, 1, 3) == std::vector<int>{0, 0, 0});  // too few weights
}

TEST_CASE("tokens group into words by their leading space, punctuation staying put") {
    const std::vector<std::string> pieces = {"I", " have", " to", " help", " them", ",", " or", " winter", "."};
    const std::vector<int> starts = {0, 7, 9, 10, 15, 16, 17, 30, 35};
    const std::vector<WordSpan> words = words_of(pieces, starts, 40, 0, 0.1);
    REQUIRE(words.size() == 7);
    CHECK(words[0].text == "I");
    CHECK(words[4].text == "them,");
    CHECK(words[6].text == "winter.");
    CHECK(words[0].start == doctest::Approx(0.0));
    CHECK(words[0].end == doctest::Approx(0.7));
    CHECK(words[1].start == doctest::Approx(0.7));
    CHECK(words[4].start == doctest::Approx(1.5));
    CHECK(words[4].end == doctest::Approx(1.7));  // ends where "or" begins, not at the comma
    CHECK(words[6].start == doctest::Approx(3.0));
    CHECK(words[6].end == doctest::Approx(4.0));  // the last word runs to the end of the audio
}

TEST_CASE("the lead moves every word later and nothing passes the audio made") {
    const std::vector<std::string> pieces = {"Good", " morning", "."};
    const std::vector<int> starts = {0, 10, 18};
    const std::vector<WordSpan> words = words_of(pieces, starts, 20, 4, 0.08);
    REQUIRE(words.size() == 2);
    CHECK(words[0].start == doctest::Approx(0.32));
    CHECK(words[0].end == doctest::Approx(1.12));
    CHECK(words[1].start == doctest::Approx(1.12));
    CHECK(words[1].end == doctest::Approx(1.6));  // 20 frames of audio, not 24
}

TEST_CASE("a word the path skipped takes a frame from the next") {
    const std::vector<std::string> pieces = {" help", " them", ",", " or", " not"};
    const std::vector<int> starts = {0, 6, 6, 6, 12};
    const std::vector<WordSpan> words = words_of(pieces, starts, 20, 0, 0.1);
    REQUIRE(words.size() == 4);
    CHECK(words[1].text == "them,");
    CHECK(words[1].start == doctest::Approx(0.6));
    CHECK(words[1].end == doctest::Approx(0.7));
    CHECK(words[2].start == doctest::Approx(0.7));
    CHECK(words[2].end == doctest::Approx(1.2));
    for (std::size_t i = 1; i < words.size(); ++i) CHECK(words[i].start >= words[i - 1].end);
}

TEST_CASE("starts that run backwards are straightened") {
    const std::vector<std::string> pieces = {"One", " two", " three"};
    const std::vector<int> starts = {5, 3, 9};
    const std::vector<WordSpan> words = words_of(pieces, starts, 12, 0, 1.0);
    REQUIRE(words.size() == 3);
    CHECK(words[0].start == doctest::Approx(5));
    CHECK(words[1].start == doctest::Approx(6));
    CHECK(words[2].start == doctest::Approx(9));
}
