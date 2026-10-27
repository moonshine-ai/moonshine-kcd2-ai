// What can be tested about the llama.cpp backend without weights.
//
// Loading a model is 2.5 GB and several seconds, so the tests that need one are not these;
// they are the play sessions and scripts/fake_game.py. What IS here is the cache decision,
// which is the whole reason for moving in-process and is a pure function over two token
// sequences. It is also the piece with a real trap in it, which is the reason it is a
// function at all rather than four lines inside chat().

#include "doctest.h"
#include "llamacpp.h"
#include "prefill.h"

using namespace moonshine;

namespace {
// Token ids stand in for a prompt. What they mean does not matter here -- only that the
// front of one prompt is the front of the next, which is what a shared world prefix is.
using Tokens = std::vector<std::int32_t>;
}  // namespace

TEST_CASE("an empty cache reuses nothing") {
    CHECK(reusable_prefix({}, Tokens{1, 2, 3}) == 0);
}

TEST_CASE("a prompt that shares nothing with the cache reuses nothing") {
    CHECK(reusable_prefix(Tokens{9, 8, 7}, Tokens{1, 2, 3}) == 0);
}

TEST_CASE("the shared front of the prompt is kept, and the rest is not") {
    // Which is the shape of every turn: the world prefix and the soul's sheet are the same
    // as last time, and the player has said something new.
    CHECK(reusable_prefix(Tokens{1, 2, 3, 4, 5}, Tokens{1, 2, 3, 9, 9, 9}) == 3);
}

TEST_CASE("a prompt already held in full still leaves a token to decode") {
    // The trap. Keeping all of it looks right and is wrong: decoding is what produces the
    // logits the first word is sampled from, so a prompt with nothing left to decode has
    // nothing to speak from. One token short, always.
    CHECK(reusable_prefix(Tokens{1, 2, 3}, Tokens{1, 2, 3}) == 2);

    // Including when the cache holds more than the prompt, which is what a second turn on a
    // conversation whose history was just cleared looks like.
    CHECK(reusable_prefix(Tokens{1, 2, 3, 4, 5}, Tokens{1, 2, 3}) == 2);
}

TEST_CASE("a single-token prompt is decoded rather than reused") {
    CHECK(reusable_prefix(Tokens{1}, Tokens{1}) == 0);
}

TEST_CASE("an empty prompt is not a request to keep the cache") {
    // Nothing to compare against, and resizing the held tokens to anything else would throw
    // away a prefix on the way to failing.
    CHECK(reusable_prefix(Tokens{1, 2, 3}, {}) == 0);
}

TEST_CASE("the world prefix survives a conversation ending and another starting") {
    // The thing the heartbeat existed to protect, here by construction rather than by hope.
    // Two souls, one world: the second conversation keeps the world and re-evaluates only
    // the sheet, and there is no third party that could have dropped it in between.
    const Tokens world{1, 2, 3, 4, 5, 6, 7, 8};
    Tokens bartel = world;
    bartel.insert(bartel.end(), {50, 51, 52});
    Tokens adam = world;
    adam.insert(adam.end(), {60, 61, 62});

    CHECK(reusable_prefix(bartel, adam) == world.size());
}

TEST_CASE("Gemma 4's template is recognised by its own markers and nobody else's") {
    // The one string that decides it. Gemma 3's spelling is what llama.cpp looks for and is
    // deliberately NOT matched here, because llama.cpp handles that one itself.
    CHECK(uses_gemma_turns("{{- '<|turn>' + role + '\\n' }}"));
    CHECK_FALSE(uses_gemma_turns("{{- '<start_of_turn>' + role + '\\n' }}"));
    // qwen3:4b-instruct, which is 4,040 bytes of Jinja and works fine. Being Jinja is not the
    // question; being on llama.cpp's list is, and ChatML is on it.
    CHECK_FALSE(uses_gemma_turns("{%- for message in messages %}<|im_start|>{{ message.role }}"));
    CHECK_FALSE(uses_gemma_turns(""));
}

TEST_CASE("Gemma 4 turns are formatted as its own template renders them") {
    // Every expectation below is the real 17,466-byte template put through Jinja and its
    // output copied here, minus the BOS token the tokeniser adds. Guessing at a prompt format
    // fails silently -- the model answers, just worse -- so these are transcribed, not written.
    const std::vector<Message> messages{{"system", "W"}, {"user", "U"}};
    CHECK(gemma_turns(messages) ==
          "<|turn>system\nW<turn|>\n"
          "<|turn>user\nU<turn|>\n"
          "<|turn>model\n<|channel>thought\n<channel|>");
}

TEST_CASE("Gemma 4 takes a system turn of its own, where Gemma 3 had none") {
    // Worth pinning because it is what the shared world prefix relies on: llama.cpp's Gemma 3
    // formatter merges a system message into the first user turn, and doing that here would
    // put the 846-token prefix somewhere the cache boundary does not expect it.
    const std::vector<Message> only_user{{"user", "U"}};
    CHECK(gemma_turns(only_user) == "<|turn>user\nU<turn|>\n<|turn>model\n<|channel>thought\n<channel|>");
}

TEST_CASE("the assistant is called the model, and content is trimmed") {
    const std::vector<Message> messages{
        {"system", "  W  "}, {"user", "\n U1 \n"}, {"assistant", "A1"}, {"user", "U2"}};
    CHECK(gemma_turns(messages) ==
          "<|turn>system\nW<turn|>\n"
          "<|turn>user\nU1<turn|>\n"
          "<|turn>model\nA1<turn|>\n"
          "<|turn>user\nU2<turn|>\n"
          "<|turn>model\n<|channel>thought\n<channel|>");
}

TEST_CASE("an empty message keeps its turn rather than collapsing it") {
    // All-whitespace content trims to nothing, and the turn markers still have to be there or
    // the roles after it shift by one.
    const std::vector<Message> messages{{"user", "   "}};
    CHECK(gemma_turns(messages) == "<|turn>user\n<turn|>\n<|turn>model\n<|channel>thought\n<channel|>");
}

TEST_CASE("the reply is asked for with the thinking channel already closed") {
    // The whole of how Gemma 4 is told not to reason. It is a fixed suffix, which is why this
    // model needs no Jinja engine where granite-4.2-8b and qwen3.5:4b do -- they need a
    // variable passed in, and llama_chat_apply_template has no way to pass one.
    const std::string out = gemma_turns({{"user", "U"}});
    CHECK(out.rfind("<|channel>thought\n<channel|>") == out.size() - 28);
}

TEST_CASE("prompt_tokens and cached_tokens are different questions") {
    // Ollama can only answer the first, which is why the prefill rate had to be measured
    // against a nonce. -1 is the backend saying it will not say, and is not zero: zero is a
    // backend that knows, and knows nothing was reused.
    const Timing silent;
    CHECK(silent.cached_tokens == -1);
}

TEST_CASE("a backend that counted its own cache hits is believed over the estimate") {
    // Measured on the Mac through llama.cpp: 978 tokens in the prompt, 0.11s of prefill,
    // because 964 of them were already there. The estimate would have to be handed a rate to
    // say anything at all, and at 219 tok/s it puts the figure at 954 -- close, and a guess.
    Timing exact;
    exact.prompt_tokens = 978;
    exact.cached_tokens = 964;
    exact.prefill_seconds = 0.11;

    CHECK(tokens_already_cached(exact, 219.0) == 964);
    // And with no rate at all, where the estimate could say nothing.
    CHECK(tokens_already_cached(exact, 0.0) == 964);
}

TEST_CASE("an exact zero is not mistaken for an unanswered question") {
    // A cold prompt through llama.cpp: nothing reused, and it says so. The estimate returns
    // zero for both this and "no rate to judge against", which is why the distinction has to
    // live in the Timing rather than in its return value.
    Timing cold;
    cold.prompt_tokens = 846;
    cold.cached_tokens = 0;
    cold.prefill_seconds = 3.86;

    CHECK(tokens_already_cached(cold, 219.0) == 0);
}

TEST_CASE("the prefill rate comes free of an exact backend, with no filler sent") {
    // A process that has just started holds nothing, so its first prefill is cold by
    // construction and is itself the measurement. Warmth::probe stays empty to say so,
    // which is 1.5s of filler not sent at start-up.
    Warmth warmth;
    warmth.warm.prompt_tokens = 846;
    warmth.warm.cached_tokens = 0;
    warmth.warm.prefill_seconds = 3.86;

    CHECK(warmth.fresh_tokens() == 846);
    CHECK(warmth.prefill_rate() == doctest::Approx(219.17).epsilon(0.01));
}

TEST_CASE("an exact backend that was already warm falls back to the filler") {
    // Not reachable in this process today, since a new process starts empty. Here because
    // prove_world's shortcut rests on the prefix being cold, and a future backend that
    // survives a restart -- a saved session file, say -- would break that quietly.
    Warmth warmth;
    warmth.warm.prompt_tokens = 846;
    warmth.warm.cached_tokens = 846;
    warmth.warm.prefill_seconds = 0.01;
    warmth.probe.prompt_tokens = 1048;
    warmth.probe.prefill_seconds = 1.80;

    CHECK(warmth.fresh_tokens() == 202);
    CHECK(warmth.prefill_rate() == doctest::Approx(112.2).epsilon(0.01));
}

TEST_CASE("a held branch stands in for the front of a prompt only when it is that front") {
    const std::vector<std::int32_t> world{1, 2, 3, 4};
    // The prompt goes on past the branch: the sheet after the world.
    CHECK(branch_covers(world, {1, 2, 3, 4, 5, 6}));
    // A prompt that is exactly the branch has nothing left to decode, so it is not covered --
    // the same one-short rule as reusable_prefix, for the same reason.
    CHECK_FALSE(branch_covers(world, {1, 2, 3, 4}));
    // A different world is a miss, however much of it matches.
    CHECK_FALSE(branch_covers(world, {1, 2, 9, 4, 5}));
    CHECK_FALSE(branch_covers(world, {1, 2}));
    // Nothing held covers nothing.
    CHECK_FALSE(branch_covers({}, {1, 2, 3}));
}
