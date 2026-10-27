// From words with their seconds to the mouth shapes to show, and when.
//
// A word's phonemes come from the grapheme-to-phoneme model Moonshine Voice ships with
// (IPA, en_gb); each phoneme is one of fifteen visemes -- the mouth shapes a viewer can
// tell apart, lips together for p/b/m, lower lip to the teeth for f/v, rounded for oo,
// wide for ee and so on -- and the word's measured span (word_align.h, from the talker's
// attention) is split among its phonemes by how long each kind of sound tends to last.
// The result is a timeline, "at this second show this shape", that the mod plays on the
// speaker's face (lipsync.h, lip_mouth; emit.cpp, LipMouth). The classes, the lengths and
// the IPA splitting mirror scripts/facial_corpus.py, which measured the game's own faces
// against the same classes.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "word_align.h"

namespace moonshine {

// The viseme names, SIL (the mouth between words) first. The table the game's faces were
// measured against (data/kcd2/voices/visemes.txt) and the mod's bank of poses are keyed by
// these.
extern const char* const kVisemes[15];

struct Mouth {
    double at = 0;  // seconds from the start of the line's audio
    std::string viseme;  // one of kVisemes
};

// The phonemes of a word in IPA as the G2P writes it: stress marks dropped, the two-symbol
// vowels and affricates (aɪ, tʃ, ...) kept whole and a length mark (ː) kept on its vowel.
std::vector<std::string> ipa_phonemes(std::string_view ipa_word);

// The viseme a phoneme is seen as, and how long it tends to last relative to the others
// (a vowel about 1, a stop about 0.5). Empty for a symbol the table does not know.
std::string_view viseme_of(std::string_view phoneme, double* length = nullptr);

// The words' sounds without a model: the English lexicon Moonshine Voice's G2P is built on
// (tts/en_us/dict_filtered_heteronyms.tsv in its download cache: "word<TAB>ipa" a line,
// 133k words, a heteronym listed more than once), looked up by the lowercased letters of
// the word; and for a word it lacks -- a Kuttenberg, a Groschen -- a reading from the
// letters alone (letters_ipa), which is wrong in detail and right enough in shape, since
// a viseme is only the class of the sound. The app on the Windows laptop has no Moonshine library to ask,
// and a line's words are a few dozen lookups, so this is what the mouth runs on everywhere.
class Lexicon {
public:
    // False when the file is not there; then every word is read from its letters.
    bool load(const std::string& path);
    std::size_t size() const { return words_.size(); }
    // The IPA of a word (letters, apostrophes; case and other marks ignored): the lexicon's
    // first entry for it, else the letters' reading. Empty for a word with no letters.
    std::string ipa(std::string_view word) const;
    // The lexicon's entry alone, empty when it has none (a possessive of a word it has
    // counts: "miller's" is miller plus z).
    std::string find(std::string_view word) const;

private:
    // word -> ipa, the words in lowercase; a flat sorted table, since it is built once.
    std::vector<std::pair<std::string, std::string>> words_;
};

// A reading of an English word from its spelling: digraphs (sh, ch, th, ph, ng, ck, wh,
// ee, oo, ou, ow, ai, ay, oa, ea, ie, igh) then letters, a final silent e dropped. Lowercase
// ASCII letters and apostrophes; anything else is skipped.
std::string letters_ipa(std::string_view word);

// The timeline for the words of a line, given each word's IPA (`ipa[i]` for `words[i]`; an
// empty string, or a word past the end of `ipa`, is a word with no mouth of its own and
// shows AX for its span). Each word's phonemes share its span by their lengths; a run of
// the same viseme is one entry; a shape that would show for less than `shortest` seconds
// is dropped unless it is a closure (PBM, FV), which is what a viewer misses most. A gap
// between words of `rest` or more, and the end of the last word, are SIL.
std::vector<Mouth> mouth_of(const std::vector<WordSpan>& words, const std::vector<std::string>& ipa,
                            double shortest = 0.04, double rest = 0.15);

}  // namespace moonshine
