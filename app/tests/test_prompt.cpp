#include "doctest.h"

#include "prompt.h"

using namespace moonshine;

namespace {

// Short stand-ins. What is being checked is the shape of the assembly, and the
// real world text is 3199 characters of it.
constexpr std::string_view kWorld = "Bohemia, 1403. The king's silver is struck at Kuttenberg.";

const Persona kAdam{"Cross-eyed Adam", "You are Adam, a beggar. You fear the circator."};
const Persona kVojta{"Vojta", "You are Vojta, a watchman. You keep a quiet ward."};

}  // namespace

TEST_CASE("two NPCs share a byte-identical prefix") {
    const std::string adam = system_prompt(kWorld, kAdam);
    const std::string vojta = system_prompt(kWorld, kVojta);
    const std::string shared = shared_prefix(kWorld);

    // The measured 7-8x saving on a conversation open depends on exactly this, and
    // on nothing else. If this ever fails, every open goes back to paying for the
    // whole prompt and only the latency shows it.
    REQUIRE(adam.size() > shared.size());
    REQUIRE(vojta.size() > shared.size());
    CHECK(adam.compare(0, shared.size(), shared) == 0);
    CHECK(vojta.compare(0, shared.size(), shared) == 0);

    // And they must diverge no earlier than the end of the shared text, or some of
    // the world setting is being re-prefilled per NPC.
    std::size_t common = 0;
    while (common < adam.size() && common < vojta.size() && adam[common] == vojta[common]) ++common;
    CHECK(common >= shared.size());
}

TEST_CASE("the world setting comes first and is unmodified") {
    const std::string prompt = system_prompt(kWorld, kAdam);
    CHECK(prompt.compare(0, kWorld.size(), kWorld) == 0);
    CHECK(prompt.find(kAdam.sheet) != std::string::npos);
    CHECK(prompt.find(kAdam.sheet) > kWorld.size());
}

TEST_CASE("the NPC's name is not in the prompt unless their sheet says so") {
    // A name placed before the world text would push the divergence point to the
    // start and destroy the sharing, so the display name is deliberately not used
    // to build the prompt.
    const Persona nameless{"Cross-eyed Adam", "You are a beggar."};
    const std::string prompt = system_prompt(kWorld, nameless);
    CHECK(prompt.find("Cross-eyed Adam") == std::string::npos);
}

TEST_CASE("a conversation is system, then history, then what was just said") {
    const std::vector<Message> history = {
        {"user", "Good day."},
        {"assistant", "Ay, good day to you."},
    };
    const std::vector<Message> messages = conversation(kWorld, kAdam, history, "Who moves you on?");

    REQUIRE(messages.size() == 4);
    CHECK(messages[0].role == "system");
    CHECK(messages[0].content == system_prompt(kWorld, kAdam));
    // History carried through untouched. Tidying an earlier turn would invalidate
    // the cache from that turn on and make a warm line cold again.
    CHECK(messages[1].content == history[0].content);
    CHECK(messages[2].content == history[1].content);
    CHECK(messages[3].role == "user");
    CHECK(messages[3].content == "Who moves you on?");
}

TEST_CASE("an opening line has no history") {
    const std::vector<Message> messages = conversation(kWorld, kAdam, {}, "Good day.");
    REQUIRE(messages.size() == 2);
    CHECK(messages[0].role == "system");
    CHECK(messages[1].role == "user");
}
