// Lines already spoken, kept so a known phrase plays at once.
//
// A reply is made when it is asked for, and a long one -- Christine dictating the
// prayer -- is several seconds of the model before the first word. The lines of a
// quest are known before anyone says them. Made once and kept, they are read back
// instead of made again; a line that is not here is spoken live, as before.
//
// What is kept is Opus, mono, at 16 kbit/s (kPhraseBitrate). Raw 16-bit PCM at
// 16 kHz is 256 kbit/s, so a line lands at about sixteen times smaller, inside the
// ten-to-twenty the cache is aimed at. The voices themselves come out at 24 kHz;
// Opus stores that rate and playback is not resampled. The size is judged against
// 16 kHz (phrase_compression) because that is the PCM the reduction is measured from.
//
// A file is named by a hash of the actor and the words, with the words flattened
// the way the voice matches a line (phrase_flat): the turn file and the voice
// marker collapse each run of whitespace, and a line has to be found either way.

#ifndef MOONSHINE_PHRASE_CACHE_H
#define MOONSHINE_PHRASE_CACHE_H

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace moonshine {

// 16 kbit/s. Against 16-bit PCM at 16 kHz (256 kbit/s) that is sixteen times
// smaller, before the few bytes of framing on each packet.
inline constexpr int kPhraseBitrate = 16000;

// The PCM rate the size is compared with. Not the rate the audio is stored at.
inline constexpr int kPhrasePcmRate = 16000;

struct PhraseAudio {
    std::vector<std::int16_t> pcm;
    int rate = 0;
};

// How many times smaller `bytes` is than the same duration of 16-bit PCM at
// 16 kHz. Zero when there is nothing to compare.
double phrase_compression(std::size_t bytes, std::size_t samples, int rate);

// The words as the voice matches them: each run of whitespace one space, none
// at either end.
std::string phrase_flat(std::string_view text);

// The file stem for `actor` saying `text` (flattened here).
std::string phrase_key(std::string_view actor, std::string_view text);

// Mono PCM to the cache's container, and back. The container is a small header
// (the rate, the sample count, the encoder delay) and then the Opus packets;
// nothing else has to be installed to read it. Throws when the rate is one Opus
// cannot hold, or the bytes are not a cache file.
std::vector<std::uint8_t> encode_opus(const std::int16_t* pcm, std::size_t count, int rate);
PhraseAudio decode_opus(const std::uint8_t* bytes, std::size_t size);

struct StoredPhrase {
    std::filesystem::path path;
    std::size_t bytes = 0;
};

class PhraseCache {
public:
    PhraseCache() = default;
    // `dir` need not exist yet; store() creates it. open() counts the lines
    // already there, for the start-up line.
    explicit PhraseCache(std::filesystem::path dir);

    // False when `dir` is not a directory. An empty directory is a cache with
    // nothing in it, which is still open.
    bool open(const std::filesystem::path& dir);
    std::size_t size() const { return count_; }
    const std::filesystem::path& dir() const { return dir_; }

    // The line, or nothing when it was never made. `text` is flattened here.
    // A file that will not decode is nothing too, and the reason is on stderr:
    // the line is then spoken live rather than failing the conversation.
    std::optional<PhraseAudio> find(std::string_view actor, std::string_view text) const;

    // Encode `pcm` and write it under dir(), named by phrase_key, and a line of
    // index.tsv so the file can be read by a person. Replaces a line already
    // stored for the same words.
    StoredPhrase store(std::string_view actor, std::string_view text, const std::int16_t* pcm,
                       std::size_t count, int rate);

private:
    std::filesystem::path dir_;
    std::size_t count_ = 0;
};

}  // namespace moonshine

#endif
