// Where a streamed reply becomes something the voice can start saying.
//
// Waiting for a whole reply wastes most of the latency budget: measured, a
// mid-conversation line finishes in about 2s but reaches its first sentence in
// under half that. Starting at the first full stop is too eager in the other
// direction. A four-word opener like "It's the circator, sir." is about 1.6s of
// speech against 3.2s still needed to generate the rest, and that ran the audio
// dry on two turns in ten.
//
// Holding out for roughly a clause or two fixed it: with a 45-character minimum,
// no run produced a gap, and the cost was 0.37s of added first-audio time. That
// number is the default below, and benchmarks/README.md has the runs.

#ifndef MOONSHINE_SENTENCE_H
#define MOONSHINE_SENTENCE_H

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace moonshine {

// No dashes in anything the game shows (notes.md 13 Oct 2026): an em dash, an en dash, a
// double hyphen or a spaced hyphen between words becomes a full stop when a capital
// follows, a comma otherwise, and a full stop when nothing follows. Applied to what the
// model writes as it streams, to every text column read from the DB, and to the rows.
std::string undashed(std::string_view text);

// How much of a streamed piece can be undashed now without a later byte changing the
// result: a trailing hyphen, space, dash or part of a multi-byte character is held back
// until the next piece says what follows it.
std::size_t undash_safe(std::string_view text);

class SentenceBuffer {
public:
    explicit SentenceBuffer(std::size_t least_chars = 45);

    // Takes a streamed piece and returns whatever became speakable because of it,
    // which is usually nothing. More than one chunk can complete at once, so this
    // returns a list rather than an optional.
    std::vector<std::string> push(std::string_view delta);

    // The end of a reply. Whatever is left is speakable now regardless of length,
    // because nothing more is coming to lengthen it.
    std::optional<std::string> flush();

    // Everything pushed so far, for the transcript and for the history that the
    // next turn is built from.
    const std::string& text() const { return text_; }

private:
    std::size_t least_chars_;
    std::string text_;     // the whole reply, kept for the transcript
    std::string pending_;  // not yet handed over to be spoken
};

}  // namespace moonshine

#endif  // MOONSHINE_SENTENCE_H
