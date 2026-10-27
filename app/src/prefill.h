// Paying prefill before the player is stood there waiting for it.
//
// On qwen3.5:4b, opening a conversation cold costs about 6.3s for the world
// setting and another 4.2s for the character sheet, and all of it lands on the
// opening line, after the player has already spoken. Neither has to. The world
// text is the same all game and can be prefilled at start-up, and the mod knows
// the player is near someone before they choose to talk, which is when that
// NPC's sheet can be prefilled. Measured, a conversation whose prompt had already
// been prefilled opened in 1.73s instead of 5.11s.
//
// This is what makes the better-dialogue model affordable at all: qwen3.5 reuses
// a shared prefix only 1.7x because 24 of its 32 layers are linear attention, so
// unlike a full-attention model it cannot be rescued by prefix sharing alone.
//
// Every call blocks, and a backend serving one slot will queue them, so this
// belongs on a worker thread and not on the thread answering the game.

#ifndef MOONSHINE_PREFILL_H
#define MOONSHINE_PREFILL_H

#include <functional>
#include <set>
#include <string>
#include <string_view>

#include "llm.h"
#include "prompt.h"

namespace moonshine {

// What the start-up prefill established: the world prefix cached, and the rate at which
// this machine prefills tokens it has not seen before.
//
// That rate is the thing every later claim about caching rests on, and it cannot be got
// by timing the world prefix itself. If the backend still holds the prefix from an earlier
// run of the app -- which is the usual case when restarting during development -- then
// that ask is a cache hit, and asking twice does not help: two hits differ by whatever
// noise is in them. Measured, the first ask came back in 0.09s and the second in 0.02s,
// which looks like a fourfold speed-up and is really two hits, and taking the first for
// a cold prefill put the rate out by sixty times.
//
// So the rate is measured against text the backend certainly has not got: the same prefix
// with a block of filler behind a nonce appended to it. Everything from the nonce on has
// to be evaluated, whatever is cached, so the time it takes divided by the tokens it added
// is a prefill rate that cannot be a hit in disguise. It costs the filler -- about 1.5s on
// a CPU-only machine -- and leaves the prefix itself cached, because a prompt sharing a
// prefix with the filler one still starts from that prefix.
// None of which is needed of a backend that counts for itself, and llama.cpp does: the cache
// is in this process, so it reports how many tokens it skipped instead of leaving the question
// to a stopwatch. There the world prefill IS the measurement -- a process that has just
// started holds nothing, so all 846 tokens of it are cold by construction -- and the filler is
// not sent at all, which is 1.5s off start-up and one fewer thing that has to be believed.
struct Warmth {
    // The real warm: the prefix every conversation in the game shares.
    Timing warm;
    // The same prefix with fresh filler behind it, which is the measurement. Not taken when
    // the warm already answered exactly, so prompt_tokens of 0 means it was not needed.
    Timing probe;

    // Tokens the filler added, according to the backend's own tokeniser rather than a
    // guess at how its tokeniser works.
    int fresh_tokens() const;

    // Tokens a second, prefilling text not already cached, on this machine with this
    // model and this much of the machine left over by the game. Zero when it could not
    // be measured, which is an unanswered question and not a slow machine.
    double prefill_rate() const;
};

// How many of a prefill's tokens the backend already had, judged against a rate measured
// on this machine rather than a threshold guessed for it. A wall-clock limit cannot do
// this job: two seconds is a cache miss on a Mac and a cache hit on a CPU-only Windows
// box with a game on it.
//
// Zero when there is no rate to compare against, which says nothing was proven rather
// than that nothing was cached.
//
// A backend that counted for itself is believed over any of this, rate or no rate.
int tokens_already_cached(const Timing& timing, double prefill_rate);

class Prefiller {
public:
    Prefiller(Llm& llm, Options options);

    // Once at start-up, before the player can reach anybody.
    Timing warm_world(std::string_view world);

    // The same, plus the measurement of what prefilling costs here. Use this at start-up
    // in preference to warm_world; the plain one remains for the repeats, where the rate
    // has already been measured and only the prefix needs putting back.
    Warmth prove_world(std::string_view world);

    // When the player comes near someone. Does nothing if that sheet is already
    // warm, so it is safe to call on every proximity event the game reports.
    Timing warm_persona(std::string_view world, const Persona& persona);
    // The same, given up when `interrupted` says so -- asked between pieces of the prefill
    // on a backend that reads the prompt in pieces (llm.h), and never on one that does not.
    // False when it was given up, in which case the sheet is not recorded as warm, though
    // the cache keeps what was read.
    bool warm_persona(std::string_view world, const Persona& persona,
                      const std::function<bool()>& interrupted);

    bool is_warm(const Persona& persona) const;
    // Records that a sheet is cached without prefilling it here: the model has just been
    // through it on some other errand -- an opening line guessed while the player looked --
    // and asking again would only cut the cache back to the sheet and lose what followed it.
    void mark_warm(const Persona& persona) { warm_.insert(persona.sheet); }

    // Dropped when the backend has been idle long enough to have forgotten, or
    // when the world text is edited and every cached prefix is stale.
    void forget();

private:
    Timing prefill(const std::string& system);

    Llm& llm_;
    Options options_;
    std::set<std::string> warm_;
};

}  // namespace moonshine

#endif  // MOONSHINE_PREFILL_H
