#include "sentence.h"

#include <cctype>

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

}  // namespace

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
