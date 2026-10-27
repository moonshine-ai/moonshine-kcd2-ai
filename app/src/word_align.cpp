#include "word_align.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>

namespace moonshine {

std::vector<int> align_tokens(const std::vector<float>& weights, int frames, int heads, int tokens) {
    std::vector<int> starts(static_cast<std::size_t>(std::max(0, tokens)), 0);
    if (frames <= 0 || heads <= 0 || tokens <= 0) return starts;
    if (weights.size() < static_cast<std::size_t>(frames) * heads * tokens) return starts;

    // Score matrix, frames x tokens: the heads' standardised, filtered weights averaged.
    std::vector<float> score(static_cast<std::size_t>(frames) * tokens, 0.f);
    std::vector<float> head_scores(static_cast<std::size_t>(frames) * tokens);
    std::vector<float> column(static_cast<std::size_t>(frames));
    for (int h = 0; h < heads; ++h) {
        for (int f = 0; f < frames; ++f) {
            const float* w = weights.data() + (static_cast<std::size_t>(f) * heads + h) * tokens;
            double mean = 0, var = 0;
            for (int t = 0; t < tokens; ++t) mean += w[t];
            mean /= tokens;
            for (int t = 0; t < tokens; ++t) var += (w[t] - mean) * (w[t] - mean);
            const double sd = std::sqrt(var / tokens) + 1e-6;
            float* out = head_scores.data() + static_cast<std::size_t>(f) * tokens;
            for (int t = 0; t < tokens; ++t) out[t] = static_cast<float>((w[t] - mean) / sd);
        }
        for (int t = 0; t < tokens; ++t) {
            for (int f = 0; f < frames; ++f) column[f] = head_scores[static_cast<std::size_t>(f) * tokens + t];
            for (int f = 0; f < frames; ++f) {
                float window[7];
                int n = 0;
                for (int k = -3; k <= 3; ++k) window[n++] = column[std::clamp(f + k, 0, frames - 1)];
                std::nth_element(window, window + 3, window + n);
                score[static_cast<std::size_t>(f) * tokens + t] += window[3] / heads;
            }
        }
    }

    // Dynamic time warping over tokens x frames on the negated score. Moves: diagonal,
    // next token (same frame), next frame (same token).
    const int N = tokens, M = frames;
    const float inf = std::numeric_limits<float>::infinity();
    std::vector<float> cost(static_cast<std::size_t>(N + 1) * (M + 1), inf);
    std::vector<uint8_t> trace(static_cast<std::size_t>(N + 1) * (M + 1), 0);
    auto C = [&](int i, int j) -> float& { return cost[static_cast<std::size_t>(i) * (M + 1) + j]; };
    auto T = [&](int i, int j) -> uint8_t& { return trace[static_cast<std::size_t>(i) * (M + 1) + j]; };
    C(0, 0) = 0;
    for (int i = 1; i <= N; ++i) {
        for (int j = 1; j <= M; ++j) {
            const float c0 = C(i - 1, j - 1), c1 = C(i - 1, j), c2 = C(i, j - 1);
            float best = c0;
            uint8_t move = 0;
            if (c1 < best) {
                best = c1;
                move = 1;
            }
            if (c2 < best) {
                best = c2;
                move = 2;
            }
            C(i, j) = -score[static_cast<std::size_t>(j - 1) * tokens + (i - 1)] + best;
            T(i, j) = move;
        }
    }
    // Backtrace: the first frame on each token is where the path arrives at it.
    int i = N, j = M;
    while (i > 0 && j > 0) {
        starts[static_cast<std::size_t>(i - 1)] = j - 1;
        const uint8_t move = T(i, j);
        if (move == 0) {
            --i;
            --j;
        } else if (move == 1) {
            --i;
        } else {
            --j;
        }
    }
    return starts;
}

std::vector<WordSpan> words_of(const std::vector<std::string>& pieces, const std::vector<int>& starts, int frames,
                               int lead, double frame_seconds) {
    std::vector<WordSpan> words;
    std::vector<int> begins;
    for (std::size_t i = 0; i < pieces.size() && i < starts.size(); ++i) {
        std::string p = pieces[i];
        const bool spaced = !p.empty() && p.front() == ' ';
        while (!p.empty() && p.front() == ' ') p.erase(p.begin());
        if (p.empty()) continue;
        bool wordy = false;
        for (unsigned char c : p) {
            if (std::isalnum(c) || c >= 0x80) wordy = true;
        }
        if (words.empty() || (spaced && wordy)) {
            words.push_back({p, 0, 0});
            begins.push_back(starts[i]);
        } else {
            words.back().text += p;
        }
    }
    // Starts never run backwards, and a word the path skipped over takes its first frame
    // from the word after it.
    for (std::size_t w = 1; w < begins.size(); ++w) begins[w] = std::max(begins[w], begins[w - 1]);
    for (std::size_t w = 0; w + 1 < begins.size(); ++w) {
        if (begins[w + 1] <= begins[w]) begins[w + 1] = begins[w] + 1;
    }
    const double made = frames * frame_seconds;
    for (std::size_t w = 0; w < words.size(); ++w) {
        const int begin = begins[w] + lead;
        const int end = w + 1 < words.size() ? begins[w + 1] + lead : std::max(begin + 1, frames);
        words[w].start = std::clamp(begin * frame_seconds, 0.0, made);
        words[w].end = std::clamp(end * frame_seconds, words[w].start, made);
    }
    return words;
}

}  // namespace moonshine
