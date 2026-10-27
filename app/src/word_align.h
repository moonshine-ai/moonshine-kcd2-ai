// Where the words fall: from a talker's attention over the text to word timings.
//
// A text-to-speech transformer that is given the whole line and then emits one frame of
// audio codes at a time has, in a few of its heads, attention that walks along the text as
// the speech advances. Given those heads' weights -- frame by frame, over the text tokens
// -- a monotonic path through the frames x tokens matrix says where each token starts, and
// the tokens group into words. This is Whisper's find_alignment, in the small; it knows
// nothing of the model, only the numbers, so that it can be tested here and used by the
// clone engine (clone_llama.cpp, which chooses the heads and reads them).
#pragma once

#include <string>
#include <vector>

namespace moonshine {

struct WordSpan {
    std::string text;
    double start = 0;  // seconds
    double end = 0;
};

// The frame each token starts on. `weights` is frames x heads x tokens, row-major; the
// heads' weights are standardised over the tokens frame by frame, median-filtered along
// time (7 frames), averaged, and the best monotonic path from (first frame, first token)
// to (last frame, last token) through the negated result is followed; a token starts
// where the path reaches it. Non-decreasing; all zero when any dimension is empty.
std::vector<int> align_tokens(const std::vector<float>& weights, int frames, int heads, int tokens);

// The words, from the tokens' text and start frames. A token whose text begins with a
// space begins a word, unless it is only punctuation, which stays with the word before;
// the first token always begins one. A word runs from its first token's start to the next
// word's, the last to `frames`; every word gets at least one frame, taken from the word
// after it when the path gave it none. `lead` frames are added throughout (attention runs
// ahead of sound), and nothing goes past the `frames` made.
std::vector<WordSpan> words_of(const std::vector<std::string>& pieces, const std::vector<int>& starts, int frames,
                               int lead, double frame_seconds);

}  // namespace moonshine
