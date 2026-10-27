#include "doctest.h"

#include "prefill.h"
#include "stub_llm.h"

using namespace moonshine;

namespace {

constexpr std::string_view kWorld = "Bohemia, 1403. The king's silver is struck at Kuttenberg.";
const Persona kAdam{"Cross-eyed Adam", "You are Adam, a beggar. You fear the circator."};
const Persona kVojta{"Vojta", "You are Vojta, a watchman."};

}  // namespace

TEST_CASE("warming the world sends the shared prefix and nothing else") {
    StubLlm llm;
    Prefiller prefiller(llm, Options{});

    prefiller.warm_world(kWorld);

    REQUIRE(llm.calls.size() == 1);
    CHECK(StubLlm::system_of(llm.last()).content == shared_prefix(kWorld));
    // No character sheet, because at start-up there is no NPC in front of anyone.
    CHECK(llm.last().messages.size() == 2);
}

TEST_CASE("prefilling asks for one token, not a reply") {
    StubLlm llm;
    Options options;
    options.reply_tokens = 512;
    Prefiller prefiller(llm, options);

    prefiller.warm_world(kWorld);

    // The point is to make the backend walk the prompt and keep what it built. A
    // full reply budget here would spend seconds generating words nobody asked for.
    CHECK(llm.last().options.reply_tokens == 1);
}

TEST_CASE("warming a persona sends the prompt their first turn will use") {
    StubLlm llm;
    Prefiller prefiller(llm, Options{});

    prefiller.warm_persona(kWorld, kAdam);

    REQUIRE(llm.calls.size() == 1);
    // Byte-identical to what the opening line will send, or the prefill is wasted:
    // a prefix that differs by one character is not reused at all.
    CHECK(StubLlm::system_of(llm.last()).content == system_prompt(kWorld, kAdam));
}

TEST_CASE("a persona is only warmed once") {
    StubLlm llm;
    Prefiller prefiller(llm, Options{});

    CHECK_FALSE(prefiller.is_warm(kAdam));
    prefiller.warm_persona(kWorld, kAdam);
    CHECK(prefiller.is_warm(kAdam));

    // The game reports proximity continuously, so this is called repeatedly for
    // whoever the player is loitering near.
    prefiller.warm_persona(kWorld, kAdam);
    prefiller.warm_persona(kWorld, kAdam);
    CHECK(llm.calls.size() == 1);
}

TEST_CASE("each persona is warmed separately") {
    StubLlm llm;
    Prefiller prefiller(llm, Options{});

    prefiller.warm_persona(kWorld, kAdam);
    prefiller.warm_persona(kWorld, kVojta);

    REQUIRE(llm.calls.size() == 2);
    CHECK(StubLlm::system_of(llm.calls[0]).content == system_prompt(kWorld, kAdam));
    CHECK(StubLlm::system_of(llm.calls[1]).content == system_prompt(kWorld, kVojta));
    CHECK(prefiller.is_warm(kVojta));
}

TEST_CASE("forgetting makes a persona cold again") {
    StubLlm llm;
    Prefiller prefiller(llm, Options{});

    prefiller.warm_persona(kWorld, kAdam);
    prefiller.forget();

    CHECK_FALSE(prefiller.is_warm(kAdam));
    prefiller.warm_persona(kWorld, kAdam);
    CHECK(llm.calls.size() == 2);
}

TEST_CASE("prefill timings come back so the cache can be watched") {
    StubLlm llm;
    llm.next_timing = {.prompt_tokens = 928, .prefill_seconds = 4.72, .reply_tokens = 1, .decode_seconds = 0.08};
    Prefiller prefiller(llm, Options{});

    const Timing timing = prefiller.warm_persona(kWorld, kAdam);

    // The app asserts on this because a backend that has stopped reusing the
    // cached prefix still answers correctly, only ten times slower, and nothing
    // else reports it.
    CHECK(timing.prompt_tokens == 928);
    CHECK(timing.prefill_seconds == doctest::Approx(4.72));
}

TEST_CASE("a skipped warm reports no work rather than a stale timing") {
    StubLlm llm;
    llm.next_timing = {.prompt_tokens = 928, .prefill_seconds = 4.72, .reply_tokens = 1, .decode_seconds = 0.08};
    Prefiller prefiller(llm, Options{});

    prefiller.warm_persona(kWorld, kAdam);
    const Timing again = prefiller.warm_persona(kWorld, kAdam);

    CHECK(again.prompt_tokens == 0);
    CHECK(again.prefill_seconds == doctest::Approx(0.0));
}

TEST_CASE("the prefill rate is measured on text the backend cannot already have") {
    StubLlm llm;
    Prefiller prefiller(llm, Options{});
    // the Windows laptop's own figures: the prefix at 848 tokens, then the same prefix with filler behind
    // a nonce coming to 1,048, so 200 tokens had to be walked, and they took 1.80s.
    llm.timings = {Timing{.prompt_tokens = 848, .prefill_seconds = 6.07, .reply_tokens = 1, .decode_seconds = 0.01}, Timing{.prompt_tokens = 1048, .prefill_seconds = 1.80, .reply_tokens = 1, .decode_seconds = 0.01}};

    const Warmth warmth = prefiller.prove_world(kWorld);
    CHECK(warmth.fresh_tokens() == 200);
    CHECK(warmth.prefill_rate() == doctest::Approx(200.0 / 1.80));

    // Two asks, the second starting with the first. The prefix has to be a prefix or the
    // filler is not measuring an append, and the nonce has to be there or the filler could
    // be sitting in the cache from the last run of the app.
    REQUIRE(llm.calls.size() == 2);
    const std::string first = StubLlm::system_of(llm.calls[0]).content;
    const std::string second = StubLlm::system_of(llm.calls[1]).content;
    CHECK(second.rfind(first, 0) == 0);
    CHECK(second.size() > first.size() + 200);
}

TEST_CASE("the rate does not come from the world prefill, which may be a cache hit") {
    StubLlm llm;
    Prefiller prefiller(llm, Options{});
    // The case that defeated measuring this by asking twice and comparing: restarting the
    // app against a backend that still holds the prefix. The warm ask is a hit at 0.09s,
    // which taken for a cold prefill reads as 9,000 tok/s and is sixty times out. The
    // filler still has to be walked, so the rate comes back right regardless.
    llm.timings = {Timing{.prompt_tokens = 848, .prefill_seconds = 0.09, .reply_tokens = 1, .decode_seconds = 0.01}, Timing{.prompt_tokens = 1048, .prefill_seconds = 1.80, .reply_tokens = 1, .decode_seconds = 0.01}};

    const Warmth warmth = prefiller.prove_world(kWorld);
    CHECK(warmth.prefill_rate() == doctest::Approx(200.0 / 1.80));
    CHECK(warmth.prefill_rate() < 200.0);
}

TEST_CASE("a backend that reports no timings at all is not mistaken for a measurement") {
    // The canned model, which answers instantly and reports nothing. Every field zero must
    // not come out as a rate of zero tokens a second, or as any rate at all.
    const Warmth nothing{};
    CHECK(nothing.fresh_tokens() == 0);
    CHECK(nothing.prefill_rate() == doctest::Approx(0.0));

    // Nor a probe that somehow added no tokens, which would divide by nothing.
    const Warmth same{Timing{.prompt_tokens = 848, .prefill_seconds = 6.07}, Timing{.prompt_tokens = 848, .prefill_seconds = 0.01}};
    CHECK(same.prefill_rate() == doctest::Approx(0.0));
}

TEST_CASE("how much of a prefill was already cached, in tokens rather than seconds") {
    // 200 fresh tokens in 1.80s is 111 tok/s, which is the Windows laptop with the GPU left to the game.
    const double rate = 200.0 / 1.80;

    // The opening turn as measured there: 981 tokens in 0.45s. At that rate the time
    // accounts for about 50 of them, so the rest came free off the world prefix.
    CHECK(tokens_already_cached(Timing{.prompt_tokens = 981, .prefill_seconds = 0.45}, rate) == 931);

    // The 15.15s opening that started all this: 917 tokens, and the time is longer than
    // walking every one of them would take, so the answer is none rather than a negative.
    CHECK(tokens_already_cached(Timing{.prompt_tokens = 917, .prefill_seconds = 15.15}, rate) == 0);

    // A complete hit is the whole prompt, and no rate is no answer.
    CHECK(tokens_already_cached(Timing{.prompt_tokens = 848, .prefill_seconds = 0.0}, rate) == 848);
    CHECK(tokens_already_cached(Timing{.prompt_tokens = 981, .prefill_seconds = 0.45}, 0.0) == 0);
    CHECK(tokens_already_cached(Timing{.prompt_tokens = 0, .prefill_seconds = 0.0}, rate) == 0);
}
