#include "sentence.h"

#include <cctype>
#include <utility>

namespace moonshine {

namespace {

bool is_space(char c) {
    return static_cast<bool>(std::isspace(static_cast<unsigned char>(c)));
}

bool ends_sentence(char c) {
    return c == '.' || c == '!' || c == '?';
}

// A sentence ends at .!? followed by whitespace. Requiring the whitespace is what
// keeps an ellipsis or a decimal from splitting, and it means the last sentence of
// a reply never completes here -- flush() is what releases it, which is correct,
// because until the reply ends there is no way to know the full stop was final.
//
// An abbreviation followed by a space would split wrongly. In this dialogue that
// has not come up, and the length floor absorbs the short cases; if it starts
// happening, the fix belongs here rather than in the caller.
std::size_t sentence_end(std::string_view s, std::size_t least_chars) {
    for (std::size_t i = 0; i + 1 < s.size(); ++i) {
        if (!ends_sentence(s[i]) || !is_space(s[i + 1])) continue;
        if (i + 1 >= least_chars) return i + 1;
    }
    return std::string_view::npos;
}

std::string_view trim_front(std::string_view s) {
    std::size_t i = 0;
    while (i < s.size() && is_space(s[i])) ++i;
    return s.substr(i);
}

std::string_view trim(std::string_view s) {
    s = trim_front(s);
    while (!s.empty() && is_space(s.back())) s.remove_suffix(1);
    return s;
}

// The bytes of a dash at s[i], or 0: an em dash or an en dash (three bytes of UTF-8), a
// run of two or more hyphens, or a single hyphen with a space on either side.
std::size_t dash_at(std::string_view s, std::size_t i) {
    if (i + 2 < s.size() && static_cast<unsigned char>(s[i]) == 0xE2 &&
        static_cast<unsigned char>(s[i + 1]) == 0x80 &&
        (static_cast<unsigned char>(s[i + 2]) == 0x94 || static_cast<unsigned char>(s[i + 2]) == 0x93)) {
        return 3;
    }
    if (s[i] != '-') return 0;
    std::size_t j = i;
    while (j < s.size() && s[j] == '-') ++j;
    if (j - i >= 2) return j - i;
    if (i > 0 && s[i - 1] == ' ' && i + 1 < s.size() && s[i + 1] == ' ') return 1;
    return 0;
}

bool is_stop(char c) {
    return c == '.' || c == '!' || c == '?';
}

bool is_pause(char c) {
    return c == ',' || c == ';' || c == ':';
}

bool is_mark(char c) {
    return is_stop(c) || is_pause(c);
}

// A letter or a digit; every byte of a character outside ASCII counts as a letter.
bool is_word(char c) {
    const unsigned char u = static_cast<unsigned char>(c);
    return u >= 0x80 || std::isalnum(u) != 0;
}

// A direction is a phrase, not a paragraph: an asterisk with no partner this near is not
// the start of one, and a reply is not held back for longer than this waiting to find out.
constexpr std::size_t kLongestDirection = 200;

enum class Star {
    Marker,     // an asterisk that means nothing here: it goes, and nothing else does
    Direction,  // the start of a stage direction, which goes whole
    Stress,     // the start of one stressed word, which stays without its asterisks
    Unsettled,  // more of the reply is needed to say which
};

// What the asterisk at s[i] is, and where what it starts ends (the last byte of it).
std::pair<Star, std::size_t> star_at(std::string_view s, std::size_t i, bool whole) {
    std::size_t j = i;
    while (j < s.size() && s[j] == '*') ++j;
    if (j - i >= 2) return {Star::Marker, j - 1};
    if (j == s.size()) return {whole ? Star::Marker : Star::Unsettled, i};
    if (is_space(s[j])) return {Star::Marker, i};
    std::size_t close = j;
    for (; close < s.size() && s[close] != '*'; ++close) {
        if (s[close] == '\n' || close - i > kLongestDirection) return {Star::Marker, i};
    }
    if (close == s.size()) return {whole ? Star::Marker : Star::Unsettled, i};

    for (std::size_t at = j; at < close; ++at) {
        if (is_space(s[at])) return {Star::Direction, close};
    }
    // One word. Stress when the sentence runs through it: a word before it and, after it, a
    // small letter or the sentence's own mark hard against it ("I *never* said", "that I
    // *never*."); or a capital of its own with a small letter after, which is a sentence
    // opening on it ("*Never* again."). Anything else stands apart from the sentences
    // around it, as a direction does ("Aye. *nods*", "a mason, *shrugs* a good one").
    std::size_t before = i;
    while (before > 0 && s[before - 1] == ' ') --before;
    // Not a word when it is a dash, a curled quote or an ellipsis: U+2000 to U+203F, which
    // is every character whose UTF-8 opens E2 80.
    const bool typographic = before >= 3 && static_cast<unsigned char>(s[before - 3]) == 0xE2 &&
                             static_cast<unsigned char>(s[before - 2]) == 0x80;
    const bool follows_word = before > 0 && is_word(s[before - 1]) && !typographic;
    const bool capital = s[j] >= 'A' && s[j] <= 'Z';
    std::size_t after = close + 1;
    if (after < s.size() && is_mark(s[after])) return {follows_word ? Star::Stress : Star::Direction, close};
    if (!follows_word && !capital) return {Star::Direction, close};
    while (after < s.size() && (s[after] == ' ' || s[after] == '\t')) ++after;
    if (after == s.size()) return {whole ? Star::Direction : Star::Unsettled, close};
    return {s[after] >= 'a' && s[after] <= 'z' ? Star::Stress : Star::Direction, close};
}

// The text without its directions, as far as it is settled: all of it when `whole`.
std::string undirect(std::string_view s, bool whole) {
    std::string out;
    out.reserve(s.size());
    std::size_t i = 0;
    while (i < s.size()) {
        if (s[i] != '*') {
            out += s[i++];
            continue;
        }
        const auto [kind, end] = star_at(s, i, whole);
        if (kind == Star::Unsettled) break;
        if (kind == Star::Marker) {
            i = end + 1;
            continue;
        }
        if (kind == Star::Stress) {
            out.append(s.substr(i + 1, end - i - 1));
            i = end + 1;
            continue;
        }
        // A direction, gone. What was around it is closed up: a mark after it is its own
        // when no words came before it in the sentence ("Aye. *sighs*. Well"), and is the
        // sentence's when they did ("Aye *sighs*. Well"); the blank on one side is kept,
        // not both. Only spaces are taken back off the end, which a streamed piece has
        // not been given yet (undash_safe holds them).
        std::size_t next = end + 1;
        if (next < s.size() && is_mark(s[next])) {
            const std::size_t last = out.find_last_not_of(' ');
            if (last == std::string::npos || is_mark(out[last]) || is_space(out[last]) || out[last] == '(') {
                while (next < s.size() && is_mark(s[next])) ++next;
            } else {
                out.erase(last + 1);
            }
        }
        if (out.empty() || is_space(out.back())) {
            while (next < s.size() && is_space(s[next])) ++next;
        } else if (next < s.size() && is_word(s[next])) {
            out += ' ';
        }
        i = next;
    }
    return out;
}

}  // namespace

std::string undirected(std::string_view s) {
    return undirect(s, true);
}

std::string speakable(std::string_view reply, bool whole) {
    const std::string words = undirect(reply, whole);
    if (whole) return undashed(words);
    return undashed(std::string_view(words).substr(0, undash_safe(words)));
}

std::string undashed(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    std::size_t i = 0;
    while (i < s.size()) {
        const std::size_t len = dash_at(s, i);
        if (len == 0) {
            out += s[i++];
            continue;
        }
        std::size_t j = i + len;
        while (j < s.size() && s[j] == ' ') ++j;
        while (!out.empty() && out.back() == ' ') out.pop_back();
        const char prev = out.empty() ? '\0' : out.back();
        const char next = j < s.size() ? s[j] : '\0';
        i = j;
        // Nothing before it (a line's start, an open bracket): the dash just goes.
        if (prev == '\0' || prev == '\n' || prev == '(') continue;
        // Nothing after it, or a line's end: the sentence stops here.
        if (next == '\0' || next == '\n' || next == '\r') {
            if (!is_stop(prev) && !is_pause(prev)) out += '.';
            continue;
        }
        // Punctuation on either side already does the dash's work.
        if (is_stop(next) || is_pause(next) || next == ')') continue;
        if (is_stop(prev) || is_pause(prev)) {
            out += ' ';
            continue;
        }
        const unsigned char n = static_cast<unsigned char>(next);
        const bool capital = (n >= 'A' && n <= 'Z') || (n >= '0' && n <= '9') || n == '"' || n == '\'';
        out += capital ? ". " : ", ";
    }
    return out;
}

std::size_t undash_safe(std::string_view s) {
    std::size_t safe = s.size();
    while (safe > 0) {
        const unsigned char c = static_cast<unsigned char>(s[safe - 1]);
        if (c == '-' || c == ' ') {
            --safe;
            continue;
        }
        if (c < 0x80) break;
        // The last character is multi-byte: back up to its lead byte. Held back when it
        // is not whole yet, or when it is a dash, whose reading waits on the next word.
        std::size_t lead = safe - 1;
        while (lead > 0 && (static_cast<unsigned char>(s[lead]) & 0xC0) == 0x80) --lead;
        const unsigned char l = static_cast<unsigned char>(s[lead]);
        const std::size_t need = l >= 0xF0 ? 4 : l >= 0xE0 ? 3 : l >= 0xC0 ? 2 : 1;
        if (safe - lead != need || dash_at(s, lead) != 0) {
            safe = lead;
            continue;
        }
        break;
    }
    return safe;
}

SentenceBuffer::SentenceBuffer(std::size_t least_chars) : least_chars_(least_chars) {}

std::vector<std::string> SentenceBuffer::push(std::string_view delta) {
    text_.append(delta);
    pending_.append(delta);

    std::vector<std::string> ready;
    while (true) {
        const std::size_t cut = sentence_end(pending_, least_chars_);
        if (cut == std::string_view::npos) break;
        std::string_view chunk = trim(std::string_view(pending_).substr(0, cut));
        if (!chunk.empty()) ready.emplace_back(chunk);
        pending_.erase(0, cut);
        // The remainder opens with the space that closed the sentence.
        pending_.erase(0, pending_.size() - trim_front(pending_).size());
    }
    return ready;
}

std::optional<std::string> SentenceBuffer::flush() {
    std::string_view rest = trim(pending_);
    if (rest.empty()) {
        pending_.clear();
        return std::nullopt;
    }
    std::string out(rest);
    pending_.clear();
    return out;
}

}  // namespace moonshine
