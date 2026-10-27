#include "doctest.h"

#include "sentence.h"

using namespace moonshine;

namespace {

// Feeds a reply a character at a time, as the real stream does, and collects
// everything that became speakable.
std::vector<std::string> speak(std::string_view reply, std::size_t least_chars) {
    SentenceBuffer buffer(least_chars);
    std::vector<std::string> chunks;
    for (char c : reply) {
        for (std::string& ready : buffer.push(std::string_view(&c, 1))) {
            chunks.push_back(std::move(ready));
        }
    }
    if (auto last = buffer.flush()) chunks.push_back(*last);
    return chunks;
}

}  // namespace

TEST_CASE("a short opener is held until there is enough to say") {
    // This is the measured failure: "It's the circator, sir." is about 1.6s of
    // speech against 3.2s still needed to generate the rest, and the audio ran dry.
    const std::string reply =
        "It's the circator, sir. He watches the streets and moves us on if we stay.";

    const std::vector<std::string> eager = speak(reply, 15);
    REQUIRE(eager.size() == 2);
    CHECK(eager[0] == "It's the circator, sir.");

    // With the floor raised, the two sentences go out together and playback has
    // enough to cover the tail.
    const std::vector<std::string> patient = speak(reply, 45);
    REQUIRE(patient.size() == 1);
    CHECK(patient[0] == reply);
}

TEST_CASE("a long reply is handed over a sentence at a time") {
    const std::string reply =
        "My sister Marta brings me bread when her husband is not looking. "
        "My brother Jost took our room above the cooper's. "
        "Hard times for us all.";
    const std::vector<std::string> chunks = speak(reply, 45);

    REQUIRE(chunks.size() == 3);
    CHECK(chunks[0] == "My sister Marta brings me bread when her husband is not looking.");
    CHECK(chunks[1] == "My brother Jost took our room above the cooper's.");
    CHECK(chunks[2] == "Hard times for us all.");
}

TEST_CASE("the last sentence arrives even without trailing space") {
    // Nothing follows the final full stop, so only the end of the reply can
    // release it. A buffer that waited for whitespace would swallow the last line.
    const std::vector<std::string> chunks = speak("A bed by the fire, a roof, a full belly.", 45);
    REQUIRE(chunks.size() == 1);
    CHECK(chunks[0] == "A bed by the fire, a roof, a full belly.");
}

TEST_CASE("an ellipsis does not split a sentence") {
    const std::vector<std::string> chunks =
        speak("My brother Jost... he lives above the cooper's now, and cares nothing for me.", 20);
    REQUIRE(chunks.size() == 1);
}

TEST_CASE("exclamations and questions end sentences") {
    const std::vector<std::string> chunks = speak(
        "And who are you to ask me that, sir? Be off with you, I have nothing!", 30);
    REQUIRE(chunks.size() == 2);
    CHECK(chunks[0] == "And who are you to ask me that, sir?");
    CHECK(chunks[1] == "Be off with you, I have nothing!");
}

TEST_CASE("the whole reply is kept for the transcript") {
    SentenceBuffer buffer(45);
    buffer.push("Good day to you, sir. ");
    buffer.push("I have seen better times.");
    buffer.flush();
    CHECK(buffer.text() == "Good day to you, sir. I have seen better times.");
}

TEST_CASE("an empty reply produces nothing to say") {
    SentenceBuffer buffer(45);
    CHECK(buffer.push("").empty());
    CHECK_FALSE(buffer.flush().has_value());
}

TEST_CASE("whitespace only is not something to say") {
    SentenceBuffer buffer(45);
    buffer.push("   \n  ");
    CHECK_FALSE(buffer.flush().has_value());
}

TEST_CASE("dashes become full stops before a capital and commas otherwise") {
    CHECK(undashed("Sakra, I could have told you he would -- the man's made of brass.") ==
          "Sakra, I could have told you he would, the man's made of brass.");
    CHECK(undashed("You! Yes, you -- a word, if you've a moment!") == "You! Yes, you, a word, if you've a moment!");
    CHECK(undashed("Christ almighty -- That's it.") == "Christ almighty. That's it.");
    CHECK(undashed("I keep my mouth shut, though \xE2\x80\x94 his troops would hang me.") ==
          "I keep my mouth shut, though, his troops would hang me.");
    CHECK(undashed("the rich speak it \xE2\x80\x93 mostly.") == "the rich speak it, mostly.");
    CHECK(undashed("He was a mason - a good one.") == "He was a mason, a good one.");
}

TEST_CASE("a dash at the end stops the sentence, one at the start goes, and hyphens stay") {
    CHECK(undashed("Aquinas said no such --") == "Aquinas said no such.");
    CHECK(undashed("-- well, perhaps he did.") == "well, perhaps he did.");
    CHECK(undashed("Sixty -- (she looks up the lane) -- that's a year's pay.") ==
          "Sixty, (she looks up the lane), that's a year's pay.");
    CHECK(undashed("the house-girl, Elsbeth, and a well-made chest") == "the house-girl, Elsbeth, and a well-made chest");
    CHECK(undashed("Where -- how did you come by this?") == "Where, how did you come by this?");
    CHECK(undashed("No. No -- I don't care.") == "No. No. I don't care.");
    CHECK(undashed("Go -- and tell Benesh. -- ") == "Go, and tell Benesh.");
    CHECK(undashed("3014.8,880.6,68.8") == "3014.8,880.6,68.8");
    CHECK(undashed("6d6f6f6e-7368-6b00-8000-979c4fccd41b") == "6d6f6f6e-7368-6b00-8000-979c4fccd41b");
}

TEST_CASE("a streamed piece holds back what a later byte could change") {
    CHECK(undash_safe("He lodges up the stair") == std::string("He lodges up the stair").size());
    CHECK(undash_safe("He lodges up the stair ") == std::string("He lodges up the stair").size());
    CHECK(undash_safe("He lodges up the stair -") == std::string("He lodges up the stair").size());
    CHECK(undash_safe("He lodges up the stair --") == std::string("He lodges up the stair").size());
    CHECK(undash_safe("He lodges up the stair \xE2\x80\x94") == std::string("He lodges up the stair").size());
    CHECK(undash_safe("He lodges up the stair \xE2\x80") == std::string("He lodges up the stair").size());
    CHECK(undash_safe("caf\xC3\xA9") == 5);
    CHECK(undash_safe("caf\xC3") == 3);
    // Up to a safe point, the undashed prefix is a prefix of the undashed whole.
    const std::string whole = "Sakra, I could have told you -- the man's made of brass.";
    const std::string first = whole.substr(0, 31);  // "...told you --"
    const std::size_t safe = undash_safe(first);
    const std::string head = undashed(first.substr(0, safe));
    CHECK(undashed(whole).compare(0, head.size(), head) == 0);
}
