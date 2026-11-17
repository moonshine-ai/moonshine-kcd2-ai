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

// What a model writes of what its character does, between asterisks, taken out: only
// words are spoken and shown ("*rubs hands together* Good day, sir." is "Good day, sir.").
// Hosted models write these far more than the local one does (17 Nov 2026: Claude Haiku
// opened four replies in four with one). A pair of asterisks on one line around more than
// one word is a direction and goes, with the blank or the full stop that was its own. One
// word between asterisks goes too ("Aye. *nods*"), unless the sentence runs through it
// ("I *never* said so", "*Never* again"), which is stress, and the word stays. Asterisks
// that are neither (doubled for bold, or with no partner on the line) go and what is
// around them stays.
std::string undirected(std::string_view text);

// What of a reply is settled enough to say and show: directions out, then dashes. With
// `whole` false the reply is still being written, and whatever a later byte could change
// is held back (an asterisk whose partner has not come, and what undash_safe holds); up
// to there the result is a prefix of what the whole reply will give.
std::string speakable(std::string_view reply, bool whole);

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
