#include "prefill.h"

#include <chrono>
#include <string>

namespace moonshine {

namespace {

// A prompt is prefilled by asking for it with the reply budget set to nothing
// that matters. There is no endpoint for "process this and stop", so one token is
// the cheapest way to make the backend walk the prompt and keep what it built.
constexpr int kPrefillReplyTokens = 1;

// The throwaway turn that makes the request a conversation. Its content is
// irrelevant: what is being cached is the system block, which the chat template
// renders before this, so the real turn will still match the prefix.
constexpr std::string_view kNudge = ".";

// Text to append behind the nonce, to be prefilled for the sake of timing it.
//
// Long enough to time: a handful of tokens would be measuring the round trip to the
// backend rather than the prefill, and at the rates that matter here -- 111 tok/s
// appending to a cached prefix on the Windows laptop -- this is about one and a half seconds, paid once
// at start-up while the game is still loading a save.
//
// Prose rather than a repeated word, because a run of identical tokens is the one thing a
// language model prefills unrepresentatively fast.
constexpr std::string_view kFiller =
    " The road from Rattay to Sasau follows the river, and a carter who leaves at first "
    "light will reach the monastery gate before the bell for sext, unless the ford above "
    "the mill is running high, in which case he will go round by the bridge and arrive "
    "hungry and late and complaining of the toll. Millers keep dogs. Bakers keep cats. "
    "The smith at Ledetchko keeps neither and says so to anybody who stands still long "
    "enough to hear it, along with his view of the price of charcoal, the quality of "
    "Kuttenberg iron, and the morals of the men who dig it out of the ground. None of "
    "this is worth remembering, which is the point of it: it is here to be counted, not "
    "to be read, and a model will not have seen this paragraph behind this nonce before.";

}  // namespace

int Warmth::fresh_tokens() const {
    // The warm knowing its own cold tokens is the in-process case, where no filler was sent
    // and the prefix itself was the fresh text.
    if (probe.prompt_tokens == 0 && warm.cached_tokens >= 0) {
        return warm.prompt_tokens - warm.cached_tokens;
    }
    return probe.prompt_tokens - warm.prompt_tokens;
}

double Warmth::prefill_rate() const {
    const double over = (probe.prompt_tokens == 0 && warm.cached_tokens >= 0)
                            ? warm.prefill_seconds
                            : probe.prefill_seconds;
    if (fresh_tokens() <= 0 || over <= 0.0) return 0.0;
    return fresh_tokens() / over;
}

int tokens_already_cached(const Timing& timing, double prefill_rate) {
    // A backend that knows has already answered, and is not second-guessed by arithmetic on
    // a rate. llama.cpp owns the cache in this process and counts the tokens it skipped.
    if (timing.cached_tokens >= 0) return timing.cached_tokens;
    if (prefill_rate <= 0.0 || timing.prompt_tokens <= 0) return 0;
    const double walked = timing.prefill_seconds * prefill_rate;
    if (walked >= timing.prompt_tokens) return 0;
    return timing.prompt_tokens - static_cast<int>(walked + 0.5);
}

Prefiller::Prefiller(Llm& llm, Options options) : llm_(llm), options_(std::move(options)) {}

Timing Prefiller::prefill(const std::string& system) {
    Options opts = options_;
    opts.reply_tokens = kPrefillReplyTokens;

    const std::vector<Message> messages = {
        {"system", system},
        {"user", std::string(kNudge)},
    };
    // The one token that comes back is discarded; only the timing is wanted.
    return llm_.chat(messages, opts, [](std::string_view) {});
}

Timing Prefiller::warm_world(std::string_view world) {
    const std::string system = shared_prefix(world);
    // The branch first: the state after exactly the shared text, kept by a backend whose
    // cache cannot be cut back to it. Then the ordinary prefill, which on such a backend is
    // the first use of that branch. A backend that keeps none does nothing for the first and
    // all of the warming in the second.
    const Timing held = llm_.hold_branch(system);
    const Timing warmed = prefill(system);
    return held.prompt_tokens > 0 ? held : warmed;
}

Warmth Prefiller::prove_world(std::string_view world) {
    const std::string system = shared_prefix(world);
    Warmth warmth;
    const Timing held = llm_.hold_branch(system);
    warmth.warm = prefill(system);
    // The branch's own prefill is the cold one, and so the one the rate can be read off.
    if (held.prompt_tokens > 0) warmth.warm = held;

    // Nothing further to ask of a backend that counted its own cache hits. The prefix was
    // cold -- a process that has just started holds nothing -- so that prefill is itself the
    // rate, and the filler would be a second answer to a question already answered.
    if (warmth.warm.cached_tokens >= 0) return warmth;

    // A number no run has used before, so the filler behind it cannot be sitting in the
    // backend's cache from the last time the app started. The clock is enough for this --
    // there is nothing to defend against here, only a repeat to avoid.
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    warmth.probe = prefill(system + "\n\n" + std::to_string(nonce) +
                           std::string(kFiller));

    // Deliberately not put back. The prefix is still the start of what the backend now
    // holds, so a real prompt reuses it and discards the filler, which is what the next
    // conversation would do to any other soul's sheet anyway.
    return warmth;
}

Timing Prefiller::warm_persona(std::string_view world, const Persona& persona) {
    if (is_warm(persona)) return {};
    Timing timing = prefill(system_prompt(world, persona));
    warm_.insert(persona.sheet);
    return timing;
}

bool Prefiller::warm_persona(std::string_view world, const Persona& persona,
                             const std::function<bool()>& interrupted) {
    if (is_warm(persona)) return true;
    Options opts = options_;
    opts.reply_tokens = kPrefillReplyTokens;
    const std::vector<Message> messages = {
        {"system", system_prompt(world, persona)},
        {"user", std::string(kNudge)},
    };
    struct GivenUp {};
    try {
        llm_.chat(messages, opts, [&](std::string_view) {
            if (interrupted()) throw GivenUp{};
        });
    } catch (const GivenUp&) {
        return false;
    }
    warm_.insert(persona.sheet);
    return true;
}

bool Prefiller::is_warm(const Persona& persona) const {
    return warm_.count(persona.sheet) != 0;
}

void Prefiller::forget() {
    warm_.clear();
}

}  // namespace moonshine
