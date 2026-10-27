#include "visemes.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>

namespace moonshine {

const char* const kVisemes[15] = {"SIL", "PBM", "FV", "TH", "TD", "SH", "KG", "R",
                                  "W",   "IY",  "EH", "AA", "OH", "UW", "AX"};

namespace {

// Two code points that are one sound, as the G2P writes them (en_gb).
constexpr std::array<const char*, 11> kTwo = {"aɪ", "aʊ", "oʊ", "eɪ", "ɔɪ", "əʊ", "ɪə", "eə", "ʊə", "dʒ", "tʃ"};

struct Sound {
    const char* ipa;
    const char* viseme;
    double length;
};

// Each phoneme's viseme and its relative length (scripts/facial_corpus.py, VISEMES).
constexpr std::array<Sound, 63> kSounds = {{
    // bilabial: lips together
    {"p", "PBM", 0.6}, {"b", "PBM", 0.6}, {"m", "PBM", 0.8},
    // labiodental: lower lip to upper teeth
    {"f", "FV", 0.8}, {"v", "FV", 0.7},
    // tongue between teeth
    {"θ", "TH", 0.7}, {"ð", "TH", 0.5},
    // alveolar/dental: lips slightly parted
    {"t", "TD", 0.5}, {"d", "TD", 0.5}, {"n", "TD", 0.7}, {"s", "TD", 0.9}, {"z", "TD", 0.8},
    {"l", "TD", 0.7}, {"ɫ", "TD", 0.7},
    // palato-alveolar: rounded, parted
    {"ʃ", "SH", 0.9}, {"ʒ", "SH", 0.8}, {"tʃ", "SH", 0.9}, {"dʒ", "SH", 0.9},
    // velar/glottal: open a little, unshaped
    {"k", "KG", 0.6}, {"ɡ", "KG", 0.6}, {"g", "KG", 0.6}, {"ŋ", "KG", 0.7}, {"h", "KG", 0.6},
    // r and w, and y with the ee shape
    {"ɹ", "R", 0.7}, {"r", "R", 0.7}, {"w", "W", 0.7}, {"j", "IY", 0.6},
    // vowels
    {"i", "IY", 1.0}, {"iː", "IY", 1.3}, {"ɪ", "IY", 0.9}, {"ɪə", "IY", 1.3},
    {"e", "EH", 1.0}, {"ɛ", "EH", 1.0}, {"eɪ", "EH", 1.3}, {"eə", "EH", 1.3}, {"æ", "AA", 1.2},
    {"ʌ", "AA", 1.0}, {"ɑ", "AA", 1.2}, {"ɑː", "AA", 1.4}, {"a", "AA", 1.1}, {"aɪ", "AA", 1.4}, {"aʊ", "AA", 1.4},
    {"ɒ", "OH", 1.1}, {"ɔ", "OH", 1.1}, {"ɔː", "OH", 1.4}, {"ɔɪ", "OH", 1.4}, {"oʊ", "OH", 1.3}, {"əʊ", "OH", 1.3},
    {"o", "OH", 1.1}, {"ʊ", "UW", 0.9}, {"u", "UW", 1.1}, {"uː", "UW", 1.3}, {"ʊə", "UW", 1.3},
    {"ə", "AX", 0.7}, {"ɚ", "AX", 1.0}, {"ɜ", "AX", 1.0}, {"ɜː", "AX", 1.3}, {"ɝ", "AX", 1.1},
    // a glottal stop (the model's t in Kuttenberg), and a few other spellings of the above
    {"ʔ", "KG", 0.4}, {"ɐ", "AX", 0.7}, {"ʉ", "UW", 1.1}, {"ʉː", "UW", 1.3}, {"ɨ", "IY", 0.9},
}};

constexpr std::string_view kStress[] = {"ˈ", "ˌ"};
constexpr std::string_view kLong = "ː";

std::size_t cp_bytes(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

}  // namespace

std::vector<std::string> ipa_phonemes(std::string_view word) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < word.size()) {
        std::size_t n = cp_bytes(static_cast<unsigned char>(word[i]));
        if (i + n > word.size()) n = word.size() - i;
        std::string_view cp = word.substr(i, n);
        bool stress = false;
        for (std::string_view s : kStress) stress = stress || cp == s;
        // A combining mark (U+0300-U+036F, the syllabic tick under the n of Kuttenberg)
        // belongs to the sound before it and is dropped.
        const bool combining = n == 2 && (static_cast<unsigned char>(cp[0]) == 0xCC ||
                                          (static_cast<unsigned char>(cp[0]) == 0xCD &&
                                           static_cast<unsigned char>(cp[1]) <= 0xAF));
        if (stress || combining || cp == " " || cp == kLong) {
            i += n;
            continue;
        }
        std::string p(cp);
        i += n;
        // A second code point that makes a diphthong or an affricate with this one.
        if (i < word.size()) {
            std::size_t m = cp_bytes(static_cast<unsigned char>(word[i]));
            if (i + m <= word.size()) {
                const std::string pair = p + std::string(word.substr(i, m));
                for (const char* two : kTwo) {
                    if (pair == two) {
                        p = pair;
                        i += m;
                        break;
                    }
                }
            }
        }
        if (word.substr(i, kLong.size()) == kLong) {
            p += kLong;
            i += kLong.size();
        }
        out.push_back(std::move(p));
    }
    return out;
}

std::string_view viseme_of(std::string_view phoneme, double* length) {
    for (const Sound& s : kSounds) {
        if (phoneme == s.ipa) {
            if (length != nullptr) *length = s.length;
            return s.viseme;
        }
    }
    if (length != nullptr) *length = 0;
    return {};
}

namespace {

// The word's letters, lowercased, with apostrophes; nothing else.
std::string plain_word(std::string_view word) {
    std::string out;
    for (std::size_t i = 0; i < word.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(word[i]);
        // A curly apostrophe (U+2018/U+2019: e2 80 98/99) is an apostrophe.
        if (c == 0xE2 && i + 2 < word.size() && static_cast<unsigned char>(word[i + 1]) == 0x80 &&
            (static_cast<unsigned char>(word[i + 2]) == 0x98 || static_cast<unsigned char>(word[i + 2]) == 0x99)) {
            out.push_back('\'');
            i += 2;
            continue;
        }
        if (c >= 'A' && c <= 'Z') c = static_cast<unsigned char>(c - 'A' + 'a');
        if ((c >= 'a' && c <= 'z') || c == '\'') out.push_back(static_cast<char>(c));
    }
    return out;
}

}  // namespace

bool Lexicon::load(const std::string& path) {
    std::ifstream in(path);
    if (!in) return false;
    std::vector<std::pair<std::string, std::string>> words;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::size_t tab = line.find('\t');
        if (tab == std::string::npos || tab == 0 || tab + 1 >= line.size()) continue;
        std::string word = plain_word(line.substr(0, tab));
        if (word.empty()) continue;
        words.emplace_back(std::move(word), line.substr(tab + 1));
    }
    std::stable_sort(words.begin(), words.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    words_ = std::move(words);
    return !words_.empty();
}

std::string Lexicon::find(std::string_view word) const {
    const std::string plain = plain_word(word);
    if (plain.empty()) return {};
    const auto it = std::lower_bound(words_.begin(), words_.end(), plain,
                                     [](const auto& entry, const std::string& key) { return entry.first < key; });
    if (it != words_.end() && it->first == plain) return it->second;
    // "miller's" is miller plus z.
    if (plain.size() > 2 && plain.compare(plain.size() - 2, 2, "'s") == 0) {
        const std::string stem = find(plain.substr(0, plain.size() - 2));
        if (!stem.empty()) return stem + "z";
    }
    return {};
}

std::string Lexicon::ipa(std::string_view word) const {
    const std::string plain = plain_word(word);
    if (plain.empty()) return {};
    const std::string found = find(plain);
    return found.empty() ? letters_ipa(plain) : found;
}

std::string letters_ipa(std::string_view word) {
    struct Rule {
        const char* letters;
        const char* ipa;
    };
    // Longest first within each starting letter is not needed: the pairs are tried before
    // the single letters.
    static constexpr Rule kPairs[] = {
        {"igh", "aɪ"}, {"sh", "ʃ"}, {"ch", "tʃ"}, {"th", "θ"}, {"ph", "f"}, {"ng", "ŋ"}, {"ck", "k"},
        {"wh", "w"},   {"qu", "kw"}, {"ee", "iː"}, {"ea", "iː"}, {"ie", "iː"}, {"oo", "uː"}, {"ou", "aʊ"},
        {"ow", "aʊ"},  {"ai", "eɪ"}, {"ay", "eɪ"}, {"ei", "eɪ"}, {"ey", "eɪ"}, {"oa", "oʊ"}, {"oi", "ɔɪ"}, {"oy", "ɔɪ"}, {"au", "ɔː"},
        {"aw", "ɔː"},  {"ew", "uː"}, {"ur", "ɜː"}, {"er", "ɚ"},  {"ir", "ɜː"}, {"or", "ɔː"}, {"ar", "ɑː"},
    };
    static constexpr Rule kLetters[] = {
        {"a", "æ"}, {"b", "b"}, {"c", "k"}, {"d", "d"}, {"e", "ɛ"}, {"f", "f"}, {"g", "ɡ"}, {"h", "h"},
        {"i", "ɪ"}, {"j", "dʒ"}, {"k", "k"}, {"l", "l"}, {"m", "m"}, {"n", "n"}, {"o", "ɒ"}, {"p", "p"},
        {"q", "k"}, {"r", "ɹ"}, {"s", "s"}, {"t", "t"}, {"u", "ʌ"}, {"v", "v"}, {"w", "w"}, {"x", "ks"},
        {"y", "i"}, {"z", "z"},
    };
    std::string plain = plain_word(word);
    plain.erase(std::remove(plain.begin(), plain.end(), '\''), plain.end());
    // A final e after a consonant is silent (make, stone); "the", "he", "be" keep theirs.
    if (plain.size() > 3 && plain.back() == 'e' && std::strchr("aeiou", plain[plain.size() - 2]) == nullptr) {
        plain.pop_back();
    }
    std::string out;
    for (std::size_t i = 0; i < plain.size();) {
        bool done = false;
        for (const Rule& r : kPairs) {
            const std::size_t n = std::strlen(r.letters);
            if (plain.compare(i, n, r.letters) == 0) {
                out += r.ipa;
                i += n;
                done = true;
                break;
            }
        }
        if (done) continue;
        const char c = plain[i];
        if (c == 'y' && i == 0) {
            out += "j";
        } else if (c == 'c' && i + 1 < plain.size() && std::strchr("eiy", plain[i + 1]) != nullptr) {
            out += "s";
        } else if (c == 'g' && i + 1 < plain.size() && std::strchr("ei", plain[i + 1]) != nullptr) {
            out += "dʒ";
        } else {
            for (const Rule& r : kLetters) {
                if (r.letters[0] == c) {
                    out += r.ipa;
                    break;
                }
            }
        }
        ++i;
    }
    // A doubled consonant reads once (letter, letter -> the same sound twice).
    std::string squeezed;
    const std::vector<std::string> parts = ipa_phonemes(out);
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i > 0 && parts[i] == parts[i - 1] && viseme_of(parts[i]) != "AX" &&
            parts[i] != "æ" && parts[i] != "ɛ" && parts[i] != "ɪ" && parts[i] != "ɒ" && parts[i] != "ʌ") {
            continue;
        }
        squeezed += parts[i];
    }
    return squeezed;
}

std::vector<Mouth> mouth_of(const std::vector<WordSpan>& words, const std::vector<std::string>& ipa,
                            double shortest, double rest) {
    struct Span {
        double at, end;
        std::string viseme;
    };
    std::vector<Span> spans;
    for (std::size_t i = 0; i < words.size(); ++i) {
        const WordSpan& w = words[i];
        if (w.end <= w.start) continue;
        std::vector<std::pair<std::string, double>> phones;
        if (i < ipa.size()) {
            for (const std::string& p : ipa_phonemes(ipa[i])) {
                double length = 0;
                const std::string_view v = viseme_of(p, &length);
                if (!v.empty()) phones.emplace_back(std::string(v), length);
            }
        }
        if (phones.empty()) phones.emplace_back("AX", 1.0);
        double total = 0;
        for (const auto& [v, length] : phones) total += length;
        double at = w.start;
        for (const auto& [v, length] : phones) {
            const double end = at + (w.end - w.start) * length / total;
            spans.push_back({at, end, v});
            at = end;
        }
        // The lips rest between words that are apart, and after the last.
        const bool last = i + 1 == words.size();
        if (last || words[i + 1].start - w.end >= rest) {
            spans.push_back({w.end, last ? w.end : words[i + 1].start, "SIL"});
        }
    }
    // Runs of one shape are one entry; a flicker is dropped unless it is a closure.
    std::vector<Mouth> out;
    for (std::size_t i = 0; i < spans.size(); ++i) {
        const Span& s = spans[i];
        const bool closure = s.viseme == "PBM" || s.viseme == "FV";
        const bool brief = s.end - s.at < shortest && !closure && s.viseme != "SIL";
        if (brief && !out.empty() && i + 1 < spans.size()) continue;
        if (!out.empty() && out.back().viseme == s.viseme) continue;
        out.push_back({s.at, s.viseme});
    }
    return out;
}

}  // namespace moonshine
