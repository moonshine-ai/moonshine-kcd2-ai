#include "voice.h"

#include "lipsync.h"
#include "phrase_cache.h"
#include "word_align.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>

#ifdef MOONSHINE_HAS_TTS
#include "moonshine-cpp.h"
#endif

// The engines the souls' lines can be spoken by. Phonon (phonon.h) is the one this build
// ships; the Qwen3-TTS pair (clone.h, clone_llama.h) is kept buildable and off. Any of them
// is "clones" below, the name from when every voice was a cloned recording.
#ifdef MOONSHINE_HAS_PHONON
#include "phonon.h"
#endif
#ifdef MOONSHINE_HAS_CLONE
#include "clone.h"
#endif
#ifdef MOONSHINE_HAS_GPU_CLONE
#include "clone_llama.h"
#endif
#if defined(MOONSHINE_HAS_PHONON) || defined(MOONSHINE_HAS_CLONE)
#define MOONSHINE_HAS_CLONES 1
#endif

#if (defined(MOONSHINE_HAS_TTS) || defined(MOONSHINE_HAS_CLONES)) && defined(__APPLE__)
#include <AudioToolbox/AudioQueue.h>
#include <cstring>
#endif

#if (defined(MOONSHINE_HAS_TTS) || defined(MOONSHINE_HAS_CLONES)) && defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>
#endif

namespace moonshine {

bool tts_built() {
#if defined(MOONSHINE_HAS_TTS) || defined(MOONSHINE_HAS_CLONES)
    return true;
#else
    return false;
#endif
}

bool clones_built() {
#ifdef MOONSHINE_HAS_CLONES
    return true;
#else
    return false;
#endif
}

std::filesystem::path find_clone_model() {
#if defined(MOONSHINE_HAS_PHONON)
    return PhononEngine::find_model();
#elif defined(MOONSHINE_HAS_CLONE)
    return CloneEngine::find_model();
#else
    return {};
#endif
}

std::string_view clone_engine_name() {
#if defined(MOONSHINE_HAS_PHONON)
    return "Phonon";
#elif defined(MOONSHINE_HAS_CLONE)
    return "Qwen3-TTS";
#else
    return "none";
#endif
}

std::string plain_voice_id(std::string_view asked) {
    while (!asked.empty() && (asked.front() == ' ' || asked.front() == '\t')) asked.remove_prefix(1);
    while (!asked.empty() && (asked.back() == ' ' || asked.back() == '\t')) asked.remove_suffix(1);
    constexpr std::string_view kPrefix = "kokoro_";
    if (asked.size() >= kPrefix.size() && asked.substr(0, kPrefix.size()) == kPrefix) {
        asked.remove_prefix(kPrefix.size());
    }
    return std::string(asked);
}

std::string actor_abbr(int voice_id) {
    struct Row {
        int id;
        const char* abbr;
    };
    static constexpr Row kActors[] = {
#include "actor_voices.inc"
    };
    for (const Row& row : kActors) {
        if (row.id == voice_id) return row.abbr;
    }
    return {};
}

std::string stand_in_voice(std::string_view gender) {
    // The two the catalogue describes the same way, "sparky, attentive British English
    // adult": the plainest of each.
    if (gender == "female") return "maeve";
    return "toby";
}

namespace {

// The catalogue's twelve British voices, by the age the game gives a soul. Henry's
// (kHenryVoice, spencer) is in no pool. Within a pool the choice is a hash of the soul, so
// the biggest group, adult men, is spread across four voices rather than one.
//
// The ages are voice.xml's: a young man is alfie (the catalogue's young adult) or toby;
// a middle-aged or elderly one is hugh, edwin or teddy. Teddy is the catalogue's young
// adult, and sits in the older men's places because he took them from spencer when
// spencer became Henry (28 Oct 2026); his pitch drops low for a clause, which carries it
// better than the description suggests. The women the same way: freya and elodie-rose
// are the catalogue's young voices, lottie its young adult, maeve, pippa and imogen its
// adults. A soul with no age is any of their gender's.
//
// A fixed array rather than an initializer_list member: the list's backing store has no
// promised lifetime inside a constexpr aggregate, and MSVC's is gone by the time it is read.
struct VoicePool {
    std::string_view age;
    std::size_t count;
    std::string_view voices[6];
};

constexpr VoicePool kMen[] = {
    {"young", 2, {"alfie", "toby"}},
    {"adult", 4, {"toby", "edwin", "hugh", "teddy"}},
    {"middle-aged", 3, {"hugh", "edwin", "teddy"}},
    {"elderly", 2, {"teddy", "hugh"}},
    {"", 5, {"toby", "alfie", "edwin", "hugh", "teddy"}},
};

constexpr VoicePool kWomen[] = {
    {"young", 3, {"freya", "elodie-rose", "lottie"}},
    {"adult", 4, {"maeve", "pippa", "imogen", "lottie"}},
    {"middle-aged", 3, {"maeve", "imogen", "pippa"}},
    {"elderly", 2, {"imogen", "maeve"}},
    {"", 6, {"maeve", "freya", "pippa", "elodie-rose", "imogen", "lottie"}},
};

// FNV-1a, so the pick is the same on every machine and every run.
std::uint32_t soul_hash(std::string_view soul) {
    std::uint32_t hash = 2166136261u;
    for (unsigned char c : soul) {
        hash ^= c;
        hash *= 16777619u;
    }
    return hash;
}

}  // namespace

std::string voice_for(std::string_view gender, std::string_view age, std::string_view soul) {
    const bool woman = gender == "female";
    const VoicePool* pools = woman ? kWomen : kMen;
    const std::size_t count = woman ? std::size(kWomen) : std::size(kMen);
    const VoicePool* pool = nullptr;
    for (std::size_t i = 0; i < count; ++i) {
        if (pools[i].age == age) pool = &pools[i];
    }
    if (!pool) pool = &pools[count - 1];  // the ageless pool, for a label the table lacks
    if (soul.empty()) return stand_in_voice(gender);
    return std::string(pool->voices[soul_hash(soul) % pool->count]);
}

std::string clone_for(std::string_view who, std::string_view actor) {
    if (who == "henry") return std::string(kHenryVoice);
    if (who == "female" || who == "male") return stand_in_voice(who);
    if (!actor.empty()) return std::string(actor);
    return stand_in_voice("");
}

namespace {

bool assets_present(const std::filesystem::path& root) {
    std::error_code error;
    return std::filesystem::is_regular_file(root / "kokoro" / "prosody.model.ort", error) &&
           std::filesystem::is_directory(root / "en_gb", error);
}

void consider(std::vector<std::filesystem::path>& out, std::filesystem::path candidate) {
    if (!candidate.empty()) out.push_back(std::move(candidate));
}

}  // namespace

std::filesystem::path find_tts_root(const std::filesystem::path& asked) {
    std::vector<std::filesystem::path> candidates;
    if (!asked.empty()) candidates.push_back(asked);
    if (const char* env = std::getenv("MOONSHINE_TTS_ROOT")) consider(candidates, env);

    if (const char* home = std::getenv("HOME")) {
        const std::filesystem::path base(home);
        consider(candidates, base / "Library/Caches/moonshine_voice/download.moonshine.ai/tts");
        consider(candidates, base / ".cache/moonshine_voice/download.moonshine.ai/tts");
    }
#ifdef _WIN32
    if (const char* local = std::getenv("LOCALAPPDATA")) {
        consider(candidates, std::filesystem::path(local) / "moonshine_voice" /
                                 "download.moonshine.ai" / "tts");
    }
#endif
    // A sibling checkout of moonshine, which is how this repo is laid out. Walked
    // from here because the app is started from the repo root and not from a fixed
    // install path.
    std::error_code error;
    std::filesystem::path dir = std::filesystem::current_path(error);
    for (int i = 0; i < 4 && !dir.empty(); ++i) {
        consider(candidates, dir / "moonshine" / "core" / "moonshine-tts" / "data");
        consider(candidates, dir / ".." / "moonshine" / "core" / "moonshine-tts" / "data");
        dir = dir.parent_path();
    }

    for (const std::filesystem::path& candidate : candidates) {
        if (!assets_present(candidate)) continue;
        std::error_code error;
        const std::filesystem::path canonical = std::filesystem::weakly_canonical(candidate, error);
        return error ? candidate : canonical;
    }
    return {};
}

std::vector<ImportedVoice> imported_voices(const std::filesystem::path& dir) {
    std::vector<ImportedVoice> voices;
    const std::filesystem::path catalog = dir / "voices.txt";
    std::ifstream in(catalog);
    if (in) {
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#') continue;
            const std::size_t tab = line.find('\t');
            ImportedVoice voice;
            if (tab == std::string::npos) {
                voice.id = plain_voice_id(line);
            } else {
                voice.id = plain_voice_id(line.substr(0, tab));
                voice.about = line.substr(tab + 1);
            }
            if (!voice.id.empty()) voices.push_back(std::move(voice));
        }
    }
    std::error_code error;
    if (!std::filesystem::is_directory(dir, error)) return voices;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.path().extension() != ".kokorovoice") continue;
        const std::string id = entry.path().stem().string();
        bool known = false;
        for (const ImportedVoice& voice : voices) {
            if (voice.id == id) known = true;
        }
        if (!known) voices.push_back({id, {}});
    }
    return voices;
}

#if defined(MOONSHINE_HAS_TTS) || defined(MOONSHINE_HAS_CLONES)

namespace {

#ifdef MOONSHINE_HAS_TTS
// Moonshine's English G2P drops punctuation, and Kokoro is the thing that turns
// ".", "?", and "," into a pause. Without them a line of two sentences is one
// phrase. These are the marks Kokoro's vocabulary actually has tokens for.
bool kokoro_punct(std::string_view cp) {
    static const std::string marks[] = {";", ":", ",", ".", "!", "?", "—", "…",
                                        "\"", "“", "”", "(", ")"};
    for (const std::string& mark : marks) {
        if (cp == mark) return true;
    }
    return false;
}

std::string next_cp(std::string_view text, std::size_t& i) {
    if (i >= text.size()) return {};
    const auto c = static_cast<unsigned char>(text[i]);
    std::size_t n = 1;
    if ((c & 0xe0) == 0xc0) n = 2;
    else if ((c & 0xf0) == 0xe0) n = 3;
    else if ((c & 0xf8) == 0xf0) n = 4;
    if (i + n > text.size()) n = 1;
    const std::string cp(text.substr(i, n));
    i += cp.size();
    return cp;
}

std::vector<std::string> codepoints(std::string_view text) {
    std::vector<std::string> out;
    for (std::size_t i = 0; i < text.size();) out.push_back(next_cp(text, i));
    return out;
}

bool word_cp(const std::string& cp) {
    if (cp.size() == 1) {
        const auto c = static_cast<unsigned char>(cp[0]);
        return std::isalnum(c) || c == '_';
    }
    return !kokoro_punct(cp);
}

std::string join_cps(const std::vector<std::string>& cps, std::size_t from, std::size_t to) {
    std::string out;
    for (std::size_t i = from; i < to; ++i) out += cps[i];
    return out;
}

std::string only_punct(std::string_view text) {
    std::string out;
    for (std::size_t i = 0; i < text.size();) {
        const std::string cp = next_cp(text, i);
        if (kokoro_punct(cp)) out += cp;
    }
    return out;
}

// "Mr." is not the end of a sentence. The period would be spoken as a full stop.
bool is_abbrev(std::string core) {
    for (char& c : core) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    static const std::string names[] = {
        "mr", "mrs", "ms", "dr", "st", "prof", "rev", "capt", "col", "gen",
        "lt",  "sgt", "hon", "jr", "sr", "messrs", "mme", "mlle", "vs", "no"};
    for (const std::string& name : names) {
        if (core == name) return true;
    }
    return false;
}

struct SpokenPiece {
    std::string text;
    // How long to rest after this piece. 0 on the last one: its own ending is enough.
    int pause_ms = 0;
};

// A sentence break (".", "?", "!") rests longer than a clause break (",", ";", ":").
std::vector<SpokenPiece> split_phrases(std::string_view text) {
    std::vector<SpokenPiece> pieces;
    std::string current;
    const auto flush = [&](int pause_ms) {
        while (!current.empty() && (current.front() == ' ' || current.front() == '\n')) {
            current.erase(current.begin());
        }
        while (!current.empty() && (current.back() == ' ' || current.back() == '\n')) {
            current.pop_back();
        }
        if (!current.empty()) pieces.push_back({current, pause_ms});
        current.clear();
    };
    for (std::size_t i = 0; i < text.size();) {
        const std::string cp = next_cp(text, i);
        current += cp;
        int pause_ms = 0;
        if (cp == "." || cp == "!" || cp == "?") pause_ms = 320;
        else if (cp == "," || cp == ";" || cp == ":") pause_ms = 160;
        else continue;
        if (cp == "." ) {
            // The word the period closed, so "Mr." stays with what follows.
            std::string word;
            for (std::size_t k = current.size() - cp.size(); k > 0;) {
                // Walk back one byte at a time only until whitespace; the period's
                // word is ASCII in this dialogue.
                --k;
                const char c = current[k];
                if (c == ' ' || c == '\n') break;
                word.insert(word.begin(), c);
            }
            if (!word.empty() && word.back() == '.') word.pop_back();
            if (is_abbrev(word)) continue;
        }
        // A mark only ends a piece when the sentence is actually over, or at the
        // end of the line. "3.14" and an ellipsis stay intact.
        if (i < text.size() && text[i] != ' ' && text[i] != '\n') continue;
        flush(pause_ms);
    }
    flush(0);
    if (!pieces.empty()) pieces.back().pause_ms = 0;
    return pieces;
}

std::string phrase_phonemes(::moonshine::GraphemeToPhonemizer& g2p, const std::string& text) {
    const std::vector<std::string> tokens = [&] {
        std::vector<std::string> out;
        std::string word;
        for (std::size_t i = 0; i < text.size();) {
            const std::string cp = next_cp(text, i);
            if (cp == " " || cp == "\n" || cp == "\t") {
                if (!word.empty()) out.push_back(std::move(word));
                word.clear();
            } else {
                word += cp;
            }
        }
        if (!word.empty()) out.push_back(std::move(word));
        return out;
    }();
    if (tokens.empty()) return {};

    std::vector<std::string> ipa;
    {
        std::string whole = g2p.toIpa(text);
        std::string word;
        for (char c : whole) {
            if (c == ' ') {
                if (!word.empty()) ipa.push_back(std::move(word));
                word.clear();
            } else {
                word.push_back(c);
            }
        }
        if (!word.empty()) ipa.push_back(std::move(word));
    }
    if (ipa.size() != tokens.size()) {
        ipa.clear();
        for (const std::string& token : tokens) {
            const std::vector<std::string> cps = codepoints(token);
            std::size_t from = 0;
            std::size_t to = cps.size();
            while (from < to && !word_cp(cps[from])) ++from;
            while (to > from && !word_cp(cps[to - 1])) --to;
            const std::string core = join_cps(cps, from, to);
            ipa.push_back(core.empty() ? std::string{} : g2p.toIpa(core));
        }
    }

    std::string phonemes;
    for (std::size_t n = 0; n < tokens.size(); ++n) {
        const std::vector<std::string> cps = codepoints(tokens[n]);
        std::size_t from = 0;
        std::size_t to = cps.size();
        while (from < to && !word_cp(cps[from])) ++from;
        while (to > from && !word_cp(cps[to - 1])) --to;
        std::string lead = only_punct(join_cps(cps, 0, from));
        const std::string core = join_cps(cps, from, to);
        std::string trail = only_punct(join_cps(cps, to, cps.size()));
        std::string ps = n < ipa.size() ? ipa[n] : std::string{};
        if (core.empty()) ps.clear();
        if (!trail.empty() && trail.front() == '.' && is_abbrev(core)) trail.erase(trail.begin());
        const std::string piece = lead + ps + trail;
        if (piece.empty()) continue;
        if (!phonemes.empty()) phonemes += ' ';
        phonemes += piece;
    }
    return phonemes;
}

// Each piece already fades in and out. Keep a little of that, and put the rest
// of the gap in ourselves, so a question and the next sentence don't share a breath.
void trim_edge_silence(std::vector<float>& samples, int sample_rate, bool drop_lead_side,
                       bool drop_tail_side) {
    const float quiet = 0.02f;
    const int keep = std::max(sample_rate * 40 / 1000, 0);
    std::size_t lead = 0;
    if (drop_lead_side) {
        while (lead < samples.size() && std::fabs(samples[lead]) < quiet) ++lead;
    }
    std::size_t tail = 0;
    if (drop_tail_side) {
        while (tail < samples.size() - lead &&
               std::fabs(samples[samples.size() - 1 - tail]) < quiet) {
            ++tail;
        }
    }
    const std::size_t cut_lead = lead > static_cast<std::size_t>(keep) ? lead - keep : 0;
    const std::size_t cut_tail = tail > static_cast<std::size_t>(keep) ? tail - keep : 0;
    if (cut_lead > 0) {
        samples.erase(samples.begin(), samples.begin() + static_cast<std::ptrdiff_t>(cut_lead));
    }
    if (cut_tail > 0) samples.resize(samples.size() - cut_tail);
}
#endif

void write_pcm16_wav(const std::filesystem::path& path, const std::vector<std::int16_t>& pcm,
                     int sample_rate) {
    if (sample_rate <= 0 || pcm.empty()) {
        throw std::runtime_error("voice: nothing to write");
    }
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("cannot write " + path.string());
    const auto put16 = [&](std::uint16_t value) {
        const char bytes[2] = {static_cast<char>(value & 0xff),
                               static_cast<char>((value >> 8) & 0xff)};
        out.write(bytes, 2);
    };
    const auto put32 = [&](std::uint32_t value) {
        const char bytes[4] = {static_cast<char>(value & 0xff),
                               static_cast<char>((value >> 8) & 0xff),
                               static_cast<char>((value >> 16) & 0xff),
                               static_cast<char>((value >> 24) & 0xff)};
        out.write(bytes, 4);
    };
    const std::uint32_t data_bytes =
        static_cast<std::uint32_t>(pcm.size() * sizeof(std::int16_t));
    out.write("RIFF", 4);
    put32(36u + data_bytes);
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    put32(16);
    put16(1);
    put16(1);
    put32(static_cast<std::uint32_t>(sample_rate));
    put32(static_cast<std::uint32_t>(sample_rate) * sizeof(std::int16_t));
    put16(sizeof(std::int16_t));
    put16(16);
    out.write("data", 4);
    put32(data_bytes);
    out.write(reinterpret_cast<const char*>(pcm.data()),
              static_cast<std::streamsize>(data_bytes));
    if (!out) throw std::runtime_error("cannot write " + path.string());
}

std::vector<std::int16_t> to_pcm16(const std::vector<float>& samples) {
    std::vector<std::int16_t> pcm(samples.size());
    for (std::size_t i = 0; i < samples.size(); ++i) {
        float sample = samples[i];
        if (sample > 1.f) sample = 1.f;
        if (sample < -1.f) sample = -1.f;
        pcm[i] = static_cast<std::int16_t>(sample * 32767.f);
    }
    return pcm;
}

// The samples of one line on their way to the speaker. They go in from whichever thread
// makes them -- the clone engine's decoder, a chunk at a time while the rest of the line
// is still being generated, or the Kokoro path all at once -- and come out in pieces the
// size the output wants.
//
// Playback does not begin until what is in hand will see the line out. The engine is not
// reliably faster than the speaker: alone on the Windows laptop it makes audio at about 1.3x real time
// once past the prefill, and beside the game, which holds five or six cores, at 0.6x. A
// line started on its first chunk would then be heard with silences in it, which is worse
// than a line started later and heard whole. So the pace is measured from the chunks as
// they arrive, the line's length is guessed from its text, and the start waits until the
// buffer covers the shortfall the rest of the line will run up at that pace -- with a
// floor of `lead`, and never past the line being finished. A guess that was short is met
// the same way mid-line: silence until the buffer is back to the floor, and a note in
// the log of how much. The line ends when it has been finished and taken, or cancelled.
class Feed {
public:
    using Clock = std::chrono::steady_clock;

    // `expected` is the guessed length of the whole line in samples (0 when unknown, which
    // falls back to the floor alone).
    Feed(std::size_t lead, std::size_t expected, int sample_rate, std::atomic<bool>& cancel)
        : lead_(lead), expected_(expected), rate_(static_cast<std::size_t>(std::max(sample_rate, 50))),
          cancel_(cancel), silences_(sample_rate) {}

    // What to tell as the sound begins and ends. `started` gets the line's length in
    // seconds: exact when the whole line was in hand at the start, else the guess from
    // its text. `ended` follows a start, whether the line was heard out or cut.
    // `words`, when the engine can place them (the GPU clone engine reads them off the
    // talker's attention), hears the words of the line and their seconds from its start:
    // those settled so far after each chunk, and the whole line with `final` at the end.
    // `heard` follows every push of samples and the finish with the silences of the audio
    // so far (lipsync.h, Silences), on the thread that pushed.
    struct Hooks {
        std::function<void(double seconds)> started;
        std::function<void()> ended;
        std::function<void(const std::vector<WordSpan>& words, bool final)> words;
        std::function<void(const Silences::Heard& heard)> heard;
    };
    void watch(Hooks hooks) { hooks_ = std::move(hooks); }
    void words(const std::vector<WordSpan>& placed, bool final) {
        if (hooks_.words) hooks_.words(placed, final);
    }

    // Called by play_feed once playback is over, however it ended.
    void over() {
        bool begun = false;
        {
            std::lock_guard<std::mutex> held(mutex_);
            begun = begun_;
            begun_ = false;
        }
        if (begun && hooks_.ended) hooks_.ended();
    }

    void push(const std::int16_t* samples, std::size_t count) {
        Silences::Heard heard;
        {
            std::lock_guard<std::mutex> held(mutex_);
            const Clock::time_point now = Clock::now();
            if (samples_.empty()) {
                first_at_ = now;
                first_count_ = count;
            }
            latest_at_ = now;
            samples_.insert(samples_.end(), samples, samples + count);
            silences_.push(samples, count);
            if (hooks_.heard) heard = silences_.heard();
        }
        changed_.notify_all();
        if (hooks_.heard) hooks_.heard(heard);
    }

    void finish() {
        Silences::Heard heard;
        {
            std::lock_guard<std::mutex> held(mutex_);
            finished_ = true;
            silences_.finish();
            if (hooks_.heard) heard = silences_.heard();
        }
        changed_.notify_all();
        if (hooks_.heard) hooks_.heard(heard);
    }

    // Blocks until there is enough in hand to begin. False when cancelled first, or when
    // the line finished with nothing in it.
    bool wait_to_start() {
        double seconds = 0.0;
        {
            std::unique_lock<std::mutex> held(mutex_);
            changed_.wait(held, [&] { return cancel_.load() || finished_ || enough(); });
            if (floor_at_ != Clock::time_point{}) {
                held_ = std::chrono::duration<double>(Clock::now() - floor_at_).count();
            }
            if (cancel_.load() || samples_.empty()) return false;
            begun_ = true;
            // The whole line in hand is its length; otherwise the guess from the text, or
            // failing that what there is.
            const std::size_t have = finished_ ? samples_.size() : std::max(expected_, samples_.size());
            seconds = static_cast<double>(have) / static_cast<double>(rate_);
        }
        if (hooks_.started) hooks_.started(seconds);
        return true;
    }

    // Up to `room` samples into `out`: what is waiting, else a short silence while more is
    // on its way. False, with nothing written, when the line is over or cancelled.
    bool take(std::int16_t* out, std::size_t room, std::size_t* taken) {
        *taken = 0;
        std::lock_guard<std::mutex> held(mutex_);
        if (cancel_.load()) return false;
        // Once dry, stay on silence until what is in hand should see the rest of the line
        // out (or the end), so a shortfall is one pause and not a stutter. The same sum as
        // the start, from the pace since; and the floor rises with each dry spell, since a
        // line that has run dry once at this pace will again on the same floor -- which was
        // the stutter heard through a long line beside the game (noted 12 Oct 2026).
        if (dry_ && (finished_ || buffered() >= need())) dry_ = false;
        if (!dry_ && at_ < samples_.size()) {
            const std::size_t count = std::min(room, samples_.size() - at_);
            std::memcpy(out, samples_.data() + at_, count * sizeof(std::int16_t));
            at_ += count;
            *taken = count;
            return true;
        }
        if (finished_ && at_ >= samples_.size()) return false;
        if (!dry_) {
            dry_ = true;
            ++dries_;
        }
        // A fiftieth of a second of silence at a time, so the return is prompt.
        const std::size_t count = std::min(room, rate_ / 50);
        std::memset(out, 0, count * sizeof(std::int16_t));
        *taken = count;
        waited_ += count;
        return true;
    }

    // Samples of silence played while the line was still being made: the gaps heard.
    std::size_t waited() {
        std::lock_guard<std::mutex> held(mutex_);
        return waited_;
    }

    // Seconds the start was held beyond the floor for the engine's pace, for the log.
    double held_for_pace() {
        std::lock_guard<std::mutex> held(mutex_);
        return held_;
    }

private:
    // Caller holds mutex_.
    std::size_t buffered() const { return samples_.size() - at_; }

    // Caller holds mutex_. Whether to begin: the floor, and the pace.
    bool enough() {
        if (samples_.size() < lead_) return false;
        if (floor_at_ == Clock::time_point{}) floor_at_ = Clock::now();
        return samples_.size() >= need();
    }

    // Caller holds mutex_. What should be in hand before the speaker starts, or starts
    // again: the floor, a lead's worth more for every dry spell so far, and the shortfall
    // the rest of the line will run up at the engine's pace.
    std::size_t need() const {
        const std::size_t have = samples_.size();
        const std::size_t floor = lead_ * (1 + dries_);
        if (expected_ == 0 || have <= first_count_) return floor;
        // Audio made per second of wall time, measured from the first chunk to the latest;
        // the first chunk itself carries the prefill and says nothing about the pace.
        const double seconds = std::chrono::duration<double>(latest_at_ - first_at_).count();
        if (seconds <= 0.0) return floor;
        const double made = static_cast<double>(have - first_count_);
        const double pace = made / seconds;  // samples per second
        if (pace >= static_cast<double>(rate_)) return floor;
        // Still to come at that pace, against what the speaker will have used by then.
        const double remaining = expected_ > have ? static_cast<double>(expected_ - have) : 0.0;
        const double shortfall = remaining * (static_cast<double>(rate_) / pace - 1.0);
        return static_cast<std::size_t>(shortfall * 1.2) + floor;
    }

    const std::size_t lead_;
    const std::size_t expected_;
    const std::size_t rate_;
    std::atomic<bool>& cancel_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<std::int16_t> samples_;
    std::size_t at_ = 0;
    std::size_t waited_ = 0;
    std::size_t first_count_ = 0;
    Clock::time_point first_at_{};
    Clock::time_point latest_at_{};
    Clock::time_point floor_at_{};  // when the floor was first met
    double held_ = 0.0;             // from then to the start, for the pace
    bool dry_ = false;
    std::size_t dries_ = 0;  // how many times the speaker has run out of samples
    bool finished_ = false;
    bool begun_ = false;  // playback started, so an end is owed to the hooks
    Hooks hooks_;
    Silences silences_;  // the pauses of the line, for the mouth
};

// The floor on what is held before a streamed line starts. Phonon hands over 80 ms frames
// at about a tenth of real time, the first under 100 ms in, so 100 ms is the second frame:
// enough for the pace to be read from, and the line begins almost as it is asked for. It
// was 400 ms for the Qwen3-TTS engine, whose chunks after the first were 0.8 s of audio
// each and which ran at real time, so the start had to be held for the second one.
std::size_t lead_samples(int sample_rate) {
    int ms = 100;
    if (const char* env = std::getenv("QWEN_LEAD_MS"); env && *env) ms = std::atoi(env);
    if (ms < 0) ms = 0;
    return static_cast<std::size_t>(sample_rate) * static_cast<std::size_t>(ms) / 1000;
}

// How long a line will take to say, from its text. The clones run at 14 to 20 characters
// a second (measured over four lines; longer lines are slower); the slow end, so that the
// start is held a little long rather than a little short.
std::size_t expected_samples(std::string_view text, int sample_rate) {
    return static_cast<std::size_t>(sample_rate) * text.size() / 14;
}

#if defined(__APPLE__)

struct Playback {
    Feed* feed = nullptr;
    std::atomic<int> active{0};
    std::atomic<bool> idle{false};
};

// True when this buffer was filled and queued. False when there is nothing left
// to queue: the caller decides whether that means playback has finished.
bool enqueue_more(Playback* play, AudioQueueRef queue, AudioQueueBufferRef buffer) {
    const std::size_t room = buffer->mAudioDataBytesCapacity / sizeof(std::int16_t);
    std::size_t frames = 0;
    if (!play->feed->take(static_cast<std::int16_t*>(buffer->mAudioData), room, &frames)) return false;
    buffer->mAudioDataByteSize = static_cast<UInt32>(frames * sizeof(std::int16_t));
    return AudioQueueEnqueueBuffer(queue, buffer, 0, nullptr) == noErr;
}

void refill(void* user, AudioQueueRef queue, AudioQueueBufferRef buffer) {
    auto* play = static_cast<Playback*>(user);
    // This buffer has finished playing. Nothing further to put in it means the line is
    // over once the other buffers have played out too; a buffer that was never started
    // does not come through here.
    if (!enqueue_more(play, queue, buffer) && play->active.fetch_sub(1) == 1) play->idle.store(true);
}

void play_feed_out(Feed& feed, int sample_rate, std::atomic<bool>& cancel) {
    if (!feed.wait_to_start()) return;

    AudioStreamBasicDescription format{};
    format.mSampleRate = sample_rate;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
    format.mBitsPerChannel = 16;
    format.mChannelsPerFrame = 1;
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = sizeof(std::int16_t);
    format.mBytesPerPacket = sizeof(std::int16_t);

    Playback play;
    play.feed = &feed;

    AudioQueueRef queue = nullptr;
    if (AudioQueueNewOutput(&format, refill, &play, nullptr, nullptr, 0, &queue) != noErr) {
        std::cerr << "voice: could not open the audio output\n";
        return;
    }
    // Short buffers, so hush() is heard within a fraction of a second; three of them, so
    // the queue never runs dry between refills.
    const UInt32 bytes = static_cast<UInt32>(sample_rate / 10) * sizeof(std::int16_t);
    for (int i = 0; i < 3; ++i) {
        AudioQueueBufferRef buffer = nullptr;
        if (AudioQueueAllocateBuffer(queue, bytes, &buffer) != noErr) break;
        if (!enqueue_more(&play, queue, buffer)) break;
        play.active.fetch_add(1);
    }
    if (play.active.load() == 0) {
        AudioQueueDispose(queue, true);
        return;
    }
    if (AudioQueueStart(queue, nullptr) != noErr) {
        std::cerr << "voice: could not start playback\n";
        AudioQueueDispose(queue, true);
        return;
    }
    while (!play.idle.load() && !cancel.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    AudioQueueStop(queue, true);
    AudioQueueDispose(queue, true);
}

#elif defined(_WIN32)

void play_feed_out(Feed& feed, int sample_rate, std::atomic<bool>& cancel) {
    if (!feed.wait_to_start()) return;

    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 1;
    format.nSamplesPerSec = static_cast<DWORD>(sample_rate);
    format.wBitsPerSample = 16;
    format.nBlockAlign = 2;
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

    HWAVEOUT wave = nullptr;
    if (waveOutOpen(&wave, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        std::cerr << "voice: could not open the audio output\n";
        return;
    }
    // A tenth of a second per buffer, so a hush cuts the line off promptly, and three in
    // flight, so the device is never waiting on this loop between them.
    constexpr int kBuffers = 3;
    const std::size_t chunk = std::max<std::size_t>(sample_rate / 10, 1);
    std::vector<std::int16_t> store[kBuffers];
    WAVEHDR headers[kBuffers] = {};
    bool queued[kBuffers] = {};
    for (auto& s : store) s.resize(chunk);
    bool over = false;
    while (true) {
        for (int i = 0; i < kBuffers; ++i) {
            if (queued[i] && (headers[i].dwFlags & WHDR_DONE)) {
                waveOutUnprepareHeader(wave, &headers[i], sizeof(WAVEHDR));
                queued[i] = false;
            }
        }
        for (int i = 0; i < kBuffers && !over; ++i) {
            if (queued[i]) continue;
            std::size_t frames = 0;
            if (!feed.take(store[i].data(), chunk, &frames)) {
                over = true;
                break;
            }
            headers[i] = {};
            headers[i].lpData = reinterpret_cast<LPSTR>(store[i].data());
            headers[i].dwBufferLength = static_cast<DWORD>(frames * sizeof(std::int16_t));
            if (waveOutPrepareHeader(wave, &headers[i], sizeof(WAVEHDR)) != MMSYSERR_NOERROR ||
                waveOutWrite(wave, &headers[i], sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
                waveOutUnprepareHeader(wave, &headers[i], sizeof(WAVEHDR));
                over = true;
                break;
            }
            queued[i] = true;
        }
        const bool any = std::any_of(std::begin(queued), std::end(queued), [](bool q) { return q; });
        if (cancel.load() || (over && !any)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (cancel.load()) waveOutReset(wave);
    for (int i = 0; i < kBuffers; ++i) {
        if (queued[i]) waveOutUnprepareHeader(wave, &headers[i], sizeof(WAVEHDR));
    }
    waveOutClose(wave);
}

#else

void play_feed_out(Feed& feed, int, std::atomic<bool>&) {
    feed.finish();
    std::cerr << "voice: playback is not implemented on this platform\n";
}

#endif

// The platform's playback, and the feed told when it is over, whichever way it ended.
void play_feed(Feed& feed, int sample_rate, std::atomic<bool>& cancel) {
    play_feed_out(feed, sample_rate, cancel);
    feed.over();
}

// A line that is all here already, as Kokoro's are.
void play_pcm(const std::vector<std::int16_t>& pcm, int sample_rate, std::atomic<bool>& cancel,
              Feed::Hooks hooks = {}, const std::vector<WordSpan>& words = {}) {
    if (pcm.empty() || cancel.load()) return;
    Feed feed(0, pcm.size(), sample_rate, cancel);
    feed.watch(std::move(hooks));
    if (!words.empty()) feed.words(words, true);
    feed.push(pcm.data(), pcm.size());
    feed.finish();
    play_feed(feed, sample_rate, cancel);
}

std::vector<std::uint8_t> read_bytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + path.string());
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in),
                                     std::istreambuf_iterator<char>());
}

// One spoken line in a soul's voice. The engine is in this process and its model is
// resident (phonon.h, or clone.h); without it, or with QWEN_OFF set, the line goes to
// Kokoro where that is linked and is otherwise not spoken.
struct CloneAudio {
    std::vector<std::int16_t> samples;
    int rate = 0;
    std::vector<WordSpan> words;  // when the engine places them; empty otherwise
};

// What every engine's streaming speak() takes: each chunk of PCM16 at its rate, and
// whether to stop. The same std::function types as PhononEngine::OnAudio and
// CloneEngine::OnAudio, spelt here so the interface below needs neither header.
using CloneOnAudio = std::function<bool(const std::int16_t* samples, std::size_t count, int rate)>;
using CloneCancelled = std::function<bool()>;

bool actor_token(std::string_view abbr) {
    if (abbr.empty() || abbr.size() > 64) return false;
    for (char c : abbr) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (!std::isalnum(u) && c != '-' && c != '_') return false;
    }
    return true;
}

#ifdef MOONSHINE_HAS_CLONES
// The engine the voices are spoken by. Three implement it: Phonon on the CPU (phonon.h),
// and the Qwen3-TTS pair, the vendored C engine on the CPU (clone.h) and llama.cpp on the
// GPU (clone_llama.h), which read the same .qvoice files. open_clones() picks one when the
// Speaker starts: Phonon when it is built in.
using OnWords = std::function<void(const std::vector<WordSpan>& words, bool final)>;

struct Clones {
    virtual ~Clones() = default;
    virtual CloneAudio speak(std::string_view voice, std::string_view text) = 0;
    // on_words may be empty; an engine that cannot place the words never calls it.
    virtual std::size_t speak(std::string_view voice, std::string_view text,
                              const CloneOnAudio& on_audio,
                              const CloneCancelled& cancelled, const OnWords& on_words) = 0;
};

// Which engines place the words (clone_llama.h's Audio::words and OnWords).
template <class Engine>
constexpr bool places_words = false;
#ifdef MOONSHINE_HAS_GPU_CLONE
template <>
constexpr bool places_words<LlamaClone> = true;
#endif

template <class Engine>
struct ClonesOf final : Clones {
    Engine engine;
    ClonesOf(std::filesystem::path model_dir, std::filesystem::path voices_dir)
        : engine(std::move(model_dir), std::move(voices_dir)) {}
    CloneAudio speak(std::string_view voice, std::string_view text) override {
        auto spoken = engine.speak(voice, text);
        CloneAudio audio{std::move(spoken.samples), spoken.rate, {}};
        if constexpr (places_words<Engine>) audio.words = std::move(spoken.words);
        return audio;
    }
    std::size_t speak(std::string_view voice, std::string_view text,
                      const CloneOnAudio& on_audio,
                      const CloneCancelled& cancelled, const OnWords& on_words) override {
        if constexpr (places_words<Engine>) {
            return engine.speak(voice, text, on_audio, cancelled, on_words);
        } else {
            (void)on_words;
            return engine.speak(voice, text, on_audio, cancelled);
        }
    }
};

// The CPU engine, unless MOONSHINE_TTS_BACKEND=gpu asks for the GPU one (which a build must
// have, with the GGUF pair present; it is taken even without a GPU device, which runs
// llama.cpp on the CPU -- for comparing the two engines, not for playing). The choice is
// said on stderr.
//
// The CPU is the default since 21 Oct 2026. The GPU engine is faster with the GPU to itself
// (RTF 0.25 against 0.72 on the Windows laptop), but the game has the GPU flat out, and beside it the
// voice ran 0.4-1.35 and, three times in a day, had the display driver reset the GPU under
// both of them (mods/kcd2/README.md, 12 Oct 2026). The CPU engine's int8 code predictor is
// the fastest of the CPU options measured (benchmarks, 15 Oct 2026), and what slows it
// beside the game is memory bandwidth, which the game's streaming shares whatever the
// threads' priority or cores; it stays near its idle speed while the game is quiet. The
// GPU gets its 2.3 GB back.
//
// Phonon, since 28 Oct 2026, wherever it is built in: 68 MB of weights against 2.5 GB, a
// tenth of real time against all of it, and 300 MB resident against 3.6. In a build with
// both, MOONSHINE_TTS_BACKEND=cpu or gpu still asks for a Qwen3-TTS engine.
std::unique_ptr<Clones> open_clones(const std::filesystem::path& voices_dir) {
    const char* wanted_env = std::getenv("MOONSHINE_TTS_BACKEND");
    const std::string wanted = wanted_env ? wanted_env : "";
#ifdef MOONSHINE_HAS_PHONON
#ifdef MOONSHINE_HAS_CLONE
    const bool phonon = wanted.empty() || wanted == "phonon";
#else
    const bool phonon = true;
#endif
    if (phonon) {
        if (!wanted.empty() && wanted != "phonon") {
            std::cerr << "voice: MOONSHINE_TTS_BACKEND=" << wanted << " asks for an engine this build has not; using Phonon\n";
        }
        std::cerr << "voice: spoken by Phonon on the CPU\n";
        return std::make_unique<ClonesOf<PhononEngine>>(PhononEngine::find_model(), voices_dir);
    }
#endif
#ifdef MOONSHINE_HAS_CLONE
    if (!wanted.empty() && wanted != "cpu" && wanted != "gpu") {
        std::cerr << "voice: MOONSHINE_TTS_BACKEND=" << wanted << " is not cpu or gpu\n";
    }
#ifdef MOONSHINE_HAS_GPU_CLONE
    if (wanted == "gpu") {
        const std::filesystem::path gguf = LlamaClone::find_model();
        if (gguf.empty()) {
            std::cerr << "voice: MOONSHINE_TTS_BACKEND=gpu but no GGUF pair found\n";
        } else {
            std::cerr << "voice: clones spoken by llama.cpp on the GPU\n";
            return std::make_unique<ClonesOf<LlamaClone>>(gguf, voices_dir);
        }
    }
#else
    if (wanted == "gpu") std::cerr << "voice: this build has no GPU clone engine\n";
#endif
    std::cerr << "voice: clones spoken by the Qwen3-TTS engine on the CPU\n";
    return std::make_unique<ClonesOf<CloneEngine>>(CloneEngine::find_model(), voices_dir);
#endif
}

CloneAudio qwen_clone(Clones* engine, std::string_view actor, const std::string& text) {
    CloneAudio audio;
    if (!engine || !actor_token(actor) || std::getenv("QWEN_OFF")) return audio;
    CloneAudio spoken = engine->speak(actor, text);
    if (spoken.samples.empty()) {
        std::cerr << "voice: clone " << actor << " produced no audio, "
#ifdef MOONSHINE_HAS_TTS
                     "using Kokoro\n";
#else
                     "not spoken\n";
#endif
        return audio;
    }
    audio.samples = std::move(spoken.samples);
    audio.rate = spoken.rate;
    audio.words = std::move(spoken.words);
    return audio;
}

// The line spoken and played as it is made. The engine's decoder hands over the first
// chunk about two seconds in and the rest at roughly the pace it is said; each goes into a
// Feed that a playback thread, started on the first chunk, drains to the speaker. So the
// player hears the line begin while the end of it is still being generated, which on the Windows laptop
// is the difference between waiting four and a half seconds and waiting two and a half. A
// hush stops the engine at the next frame and the playback at the next buffer.
//
// True when the engine spoke it (to the end or until hushed). False when it had nothing
// to say -- no engine, no such clone, QWEN_OFF, or a failure, each reported on stderr --
// and the caller should fall back to Kokoro.
bool qwen_stream(Clones* engine, std::string_view actor, const std::string& text,
                 std::atomic<bool>& cancel, const Feed::Hooks& hooks) {
    if (!engine || !actor_token(actor) || std::getenv("QWEN_OFF")) return false;
    std::unique_ptr<Feed> feed;
    std::thread player;
    int sample_rate = 0;
    const std::size_t delivered = engine->speak(
        actor, text,
        [&](const std::int16_t* samples, std::size_t count, int rate) {
            // On the engine's decoder thread. The first chunk sets the rate and starts
            // playback; nothing before it knew the rate.
            if (!feed) {
                sample_rate = rate;
                feed = std::make_unique<Feed>(lead_samples(rate), expected_samples(text, rate), rate, cancel);
                feed->watch(hooks);
                player = std::thread([&feed, rate, &cancel] { play_feed(*feed, rate, cancel); });
            }
            feed->push(samples, count);
            return !cancel.load();
        },
        [&] { return cancel.load(); },
        [&](const std::vector<WordSpan>& words, bool final) {
            if (feed) feed->words(words, final);
        });
    if (feed) feed->finish();
    if (player.joinable()) player.join();
    if (feed && !cancel.load()) {
        // What the pace cost: a start held back for it, or, when the guess at the line's
        // length was short, a pause in the middle.
        const double held = feed->held_for_pace();
        const double gapped = feed->waited() / static_cast<double>(sample_rate);
        if (held > 0.05 || gapped > 0.0) {
            std::cerr << "voice: " << actor << ": the engine ran behind the speaker";
            if (held > 0.05) std::cerr << ", start held " << held << "s for it";
            if (gapped > 0.0) std::cerr << ", paused " << gapped << "s mid-line";
            std::cerr << "\n";
        }
    }
    if (delivered == 0 && !feed) {
        if (cancel.load()) return true;  // hushed before the first chunk: nothing to fall back to
        std::cerr << "voice: clone " << actor << " produced no audio, "
#ifdef MOONSHINE_HAS_TTS
                     "using Kokoro\n";
#else
                     "not spoken\n";
#endif
        return false;
    }
    return true;
}
#else
CloneAudio qwen_clone(void*, std::string_view, const std::string&) { return {}; }
bool qwen_stream(void*, std::string_view, const std::string&, std::atomic<bool>&, const Feed::Hooks&) {
    return false;
}
#endif

}  // namespace

struct Speaker::Impl {
    std::string voice;
#ifdef MOONSHINE_HAS_TTS
    // Kept alive for the synthesizer: a voice passed from memory is not copied.
    std::vector<std::uint8_t> voice_bytes;
    ::moonshine::TextToSpeech tts;
    ::moonshine::GraphemeToPhonemizer g2p;
#endif
#ifdef MOONSHINE_HAS_CLONES
    // The resident engine the souls speak through: Phonon, or in a build that carries it
    // and asks, Qwen3-TTS on the CPU or GPU.
    std::unique_ptr<Clones> clones;
#else
    struct NoClones {
        void* get() const { return nullptr; }
    } clones;
#endif
    std::mutex mutex;
    std::condition_variable pending;
    std::condition_variable drained;
    struct Job {
        enum class Kind { Say, Prepare, Warm };
        Kind kind = Kind::Say;
        std::string text;
        std::string actor;
        LipTag lips;
    };
    // The mouth: the game's facial clips by actor and length, and where the cues go.
    Facials facials;
    LipSink lips;
    std::mutex lips_mutex;
    // The cue files of the line being spoken, taken away when the next line's cue goes.
    std::vector<std::string> lip_files;

    // The words' sounds: the lexicon beside the voices (visemes.h, Lexicon), then, in a
    // build with the Moonshine library, the G2P the Kokoro voice uses for a word it lacks,
    // and the word's letters where there is neither.
    Lexicon lexicon;

    // The IPA of each word, for its mouth shapes (visemes.h).
    std::vector<std::string> words_ipa(const std::vector<WordSpan>& words) {
        std::vector<std::string> ipa;
        for (const WordSpan& w : words) {
            std::string sound = lexicon.find(w.text);
#ifdef MOONSHINE_HAS_TTS
            if (sound.empty()) {
                const std::vector<std::string> cps = codepoints(w.text);
                std::size_t from = 0;
                std::size_t to = cps.size();
                while (from < to && !word_cp(cps[from])) ++from;
                while (to > from && !word_cp(cps[to - 1])) --to;
                const std::string core = join_cps(cps, from, to);
                if (!core.empty()) {
                    try {
                        sound = g2p.toIpa(core);
                    } catch (const std::exception&) {
                    }
                }
            }
#endif
            if (sound.empty()) sound = letters_ipa(w.text);
            ipa.push_back(std::move(sound));
        }
        return ipa;
    }

    // Whether the mouth is shaped word by word (MOONSHINE_MOUTH_SHAPES=1) or borrowed from
    // the game's own clips (the default). The shapes follow the words to the frame, but
    // fifteen poses held and blended are a long way from a face speaking, so the borrowed
    // clips -- an actor's real mouth, fitted by the stretch that speaks (Facials::plan) --
    // are what the game shows for now (13 Oct 2026, in play), cut at the pauses heard in
    // the audio. With the shapes off the clone engine is not asked for the words either
    // (main.cpp sets QWEN_GGUF_WORDS=0 by default).
    const bool shapes = [] {
        const char* env = std::getenv("MOONSHINE_MOUTH_SHAPES");
        return env && *env && std::strcmp(env, "0") != 0;
    }();

    // The cues of a line. From the playback thread: the plan as the sound starts and a
    // stop as it ends. Also from the engine's thread as the audio arrives, each cue
    // following the one before, which is how the mod reads them. By default the plan is
    // cut at the pauses of the speech, heard in the audio (Silences): those heard before
    // the sound starts go into the opening plan, and each pause closed after it is a
    // revision of the plan from that second on, written as the sound after the pause
    // arrives -- the engine runs ahead of the speaker by the lead it waits for and more,
    // so the cue is in the mod's hands before the pause comes, or near enough that the mod
    // joins it partway. With the shapes on, the cues are the mouth shapes of the words
    // settled so far instead (the GPU clone engine reads them off the talker's attention);
    // shapes known before the sound starts wait for it, and then go in place of the plan.
    // Nothing for a line without a tag or a sink, or when no clips were loaded.
    Feed::Hooks lip_hooks(const Job& job) {
        if (!job.lips.wanted() || !lips.write || facials.size() == 0) return {};
        const LipTag tag = job.lips;
        struct Line {
            std::mutex mutex;
            int seq = 0;  // the last cue written, 0 for none yet
            bool started = false;
            double seconds = 0;  // the line's length, as told at the start
            std::vector<Mouth> waiting;  // shapes known before the sound started
            double cued_until = 0;  // the words' seconds the shapes have been written to
            Silences::Heard heard;  // the audio's pauses so far
            double planned_until = 0;  // the second of the audio the plan's cuts are known to; past it, speech is assumed
        };
        auto line = std::make_shared<Line>();
        // Under line->mutex. The first cue of the line takes the previous line's files away.
        const auto write = [this, tag, line](const std::string& body) {
            std::vector<std::string> previous;
            const std::string file = lip_file(tag.key, ++line->seq);
            {
                std::lock_guard<std::mutex> held(lips_mutex);
                if (line->seq == 1) previous.swap(lip_files);
                lip_files.push_back(file);
            }
            if (lips.forget && !previous.empty()) lips.forget(previous);
            lips.write(file, body);
        };
        // Under line->mutex. The plan from `from` on, cut at the pauses heard so far; how
        // far the cuts are known is remembered, so a pause is planned once. A silence
        // still open is not known to be a pause, so the plan is known only to its start.
        const auto plan_from = [this, tag, line, write](double from) {
            const std::uint32_t seed = tag.key ^ static_cast<std::uint32_t>(from * 1000.0);
            const Silences::Heard& heard = line->heard;
            const LipPlan plan = facials.plan(tag.face, heard, from, line->seconds, seed);
            line->planned_until = heard.over ? std::numeric_limits<double>::infinity()
                                             : (heard.silent_since >= 0 ? heard.silent_since : heard.seconds);
            if (plan.empty()) return;
            write(lip_cue(tag.who, tag.key, plan));
            const std::size_t clips = plan.clips.size() - plan.rests();
            std::cerr << "voice: mouth for " << tag.who << " in " << tag.face << "'s face"
                      << (from > 0 ? " from " : ": ") << std::fixed << std::setprecision(1);
            if (from > 0) std::cerr << from << "s: ";
            std::cerr << clips << " clip" << (clips == 1 ? "" : "s");
            if (plan.rests() > 0) std::cerr << ", " << plan.rests() << " rest" << (plan.rests() == 1 ? "" : "s");
            std::cerr << " over " << plan.seconds() << "s for " << line->seconds << "s of speech, "
                      << heard.seconds << "s heard\n";
        };
        Feed::Hooks hooks;
        hooks.started = [tag, line, write, plan_from](double seconds) {
            std::lock_guard<std::mutex> held(line->mutex);
            line->started = true;
            line->seconds = seconds;
            if (!line->waiting.empty()) {
                write(lip_mouth(tag.who, tag.key, line->waiting));
                std::cerr << "voice: mouth for " << tag.who << ": " << line->waiting.size()
                          << " shapes to " << std::fixed << std::setprecision(1) << line->cued_until
                          << "s at the start of " << seconds << "s of speech\n";
                line->waiting.clear();
                return;
            }
            plan_from(0.0);
        };
        if (!shapes) hooks.heard = [line, plan_from](const Silences::Heard& heard) {
            std::lock_guard<std::mutex> held(line->mutex);
            line->heard = heard;
            // Before the sound starts the pauses wait for the opening plan. After it, the
            // first pause the plan does not know of (closed since, or the silence the audio
            // ended in) is where the plan is revised from.
            if (!line->started || std::isinf(line->planned_until)) return;
            double from = -1.0;
            for (const Pause& pause : heard.pauses) {
                if (pause.start < line->planned_until) continue;
                from = pause.start;
                break;
            }
            if (from < 0.0) {
                line->planned_until = heard.over ? std::numeric_limits<double>::infinity()
                                                 : (heard.silent_since >= 0 ? heard.silent_since : heard.seconds);
                return;
            }
            plan_from(from);
        };
        if (shapes) hooks.words = [this, tag, line, write](const std::vector<WordSpan>& words, bool final) {
            if (words.empty()) return;
            // The whole timeline so far, and of it what is new: a word's shapes, once
            // written, do not move, but the rest after the last word is only known to be a
            // rest once the line is over or the next word has come, so it waits.
            const std::vector<Mouth> mouth = mouth_of(words, words_ipa(words));
            const double until = final ? std::numeric_limits<double>::infinity() : words.back().end;
            std::vector<Mouth> fresh;
            {
                std::lock_guard<std::mutex> held(line->mutex);
                for (const Mouth& m : mouth) {
                    if (m.at >= line->cued_until && m.at < until) fresh.push_back(m);
                }
                line->cued_until = std::max(line->cued_until, final ? mouth.back().at + 1e-9 : words.back().end);
                if (!fresh.empty()) {
                    if (!line->started) {
                        line->waiting.insert(line->waiting.end(), fresh.begin(), fresh.end());
                    } else {
                        write(lip_mouth(tag.who, tag.key, fresh));
                    }
                }
            }
            if (!final) return;
            std::cerr << "voice: words of " << tag.who << ":";
            for (const WordSpan& w : words) {
                std::cerr << " " << w.text << "@" << std::fixed << std::setprecision(2) << w.start;
            }
            std::cerr << " end " << words.back().end << ", " << mouth.size() << " shapes\n";
        };
        hooks.ended = [tag, line, write] {
            std::lock_guard<std::mutex> held(line->mutex);
            // A line that never had a cue has no stop owed.
            if (line->seq == 0) return;
            write(lip_stop(tag.who, tag.key));
        };
        return hooks;
    }
    std::deque<Job> queue;
    // Lines made before they were asked for, by their flattened words and the actor they
    // were made in. A greeting is a few seconds of audio, so a few dozen is a few tens of
    // megabytes; the oldest goes when the next comes in.
    struct Made {
        std::string key;
        std::string actor;
        std::vector<std::int16_t> pcm;
        int rate = 0;
        std::vector<WordSpan> words;
        bool cached = false;  // read from the phrase cache, not made by the engine
    };
    static constexpr std::size_t kKeptMade = 32;
    std::deque<Made> made;
    // Lines already spoken (phrase_cache.h). Read from the voice's thread, replaced
    // from the main thread before anything is queued, under mutex_.
    std::shared_ptr<const PhraseCache> phrases;
    // Lines the voice is done with and has not yet reported (take_spoken): heard to the
    // end, or dropped.
    std::vector<std::string> spoken;
    bool stopping = false;
    bool busy = false;
    // What the worker is on, under the mutex: a hush cancels a line being spoken, and a
    // line made ahead only once it has been handed to the speaker (make), not before --
    // the greeting is being made as the press arrives, and the hush that press brings must
    // not stop it from being handed over a moment later. A say() of other words does cut
    // it, though (see say), and the key says whether the words are other.
    Job::Kind current = Job::Kind::Say;
    std::string current_key;
    std::string current_actor;
    bool handing = false;
    // Set with cancel by forget_ahead(): the line being made is not wanted after all, so
    // the worker does not put it back on the queue as it does one set aside for a say().
    bool dropped = false;
    std::atomic<bool> cancel{false};
    // Bumped by hush(), so a line that finished synthesizing after the hush is
    // thrown away instead of played.
    std::uint64_t epoch = 0;
    std::thread worker;

    Impl(std::string voice_id, std::filesystem::path voices_dir, std::filesystem::path tts_root)
        : voice(std::move(voice_id))
#ifdef MOONSHINE_HAS_TTS
        , tts(open_tts(voice, voices_dir, tts_root, voice_bytes)),
          g2p("en_gb", {{"g2p_root", tts_root.string()}})
#endif
    {
#ifndef MOONSHINE_HAS_TTS
        (void)tts_root;
#endif
#ifdef MOONSHINE_HAS_CLONES
        if (!std::getenv("QWEN_OFF")) {
            clones = open_clones(voices_dir / "clones");
        }
#endif
        // The game's facial clips, for the mouth (lipsync.h). Not having the file is a
        // voice without a mouth, which is how it was; said once at start-up by main.
        facials.load(voices_dir / "facials.txt");
        if (!lexicon.load((voices_dir / "lexicon.tsv").string())) {
            std::cerr << "voice: no lexicon.tsv beside the voices; the mouth's shapes will be read from the words' letters"
#ifdef MOONSHINE_HAS_TTS
                         " where the G2P has nothing"
#endif
                         "\n";
        }
        worker = std::thread([this] { run(); });
    }

#ifdef MOONSHINE_HAS_TTS
    // One phrase at a time, with the pause that belongs between them. Synthesizing
    // the whole line at once is what ran the sentences together.
    ::moonshine::TtsSynthesisResult synthesize_line(const std::string& text) {
        const std::vector<SpokenPiece> pieces = split_phrases(text);
        ::moonshine::TtsSynthesisResult spoken;
        if (pieces.empty()) return spoken;
        for (std::size_t i = 0; i < pieces.size(); ++i) {
            const std::string phonemes = phrase_phonemes(g2p, pieces[i].text);
            ::moonshine::TtsSynthesisResult part =
                phonemes.empty() ? tts.synthesize(pieces[i].text)
                                 : tts.synthesizeFromPhonemes(phonemes);
            if (part.samples.empty() || part.sampleRateHz <= 0) continue;
            if (spoken.sampleRateHz == 0) spoken.sampleRateHz = part.sampleRateHz;
            const bool followed = i + 1 < pieces.size() && pieces[i].pause_ms > 0;
            trim_edge_silence(part.samples, part.sampleRateHz, i > 0, followed);
            spoken.samples.insert(spoken.samples.end(), part.samples.begin(), part.samples.end());
            if (pieces[i].pause_ms > 0 && i + 1 < pieces.size()) {
                const int frames = part.sampleRateHz * pieces[i].pause_ms / 1000;
                spoken.samples.insert(spoken.samples.end(), static_cast<std::size_t>(frames), 0.f);
            }
        }
        return spoken;
    }

    static ::moonshine::TextToSpeech open_tts(const std::string& voice,
                                               const std::filesystem::path& voices_dir,
                                               const std::filesystem::path& tts_root,
                                               std::vector<std::uint8_t>& voice_bytes) {
        const ::moonshine::Options options = {
            {"g2p_root", tts_root.string()},
            {"voice", "kokoro_" + voice},
        };
        std::error_code error;
        const std::filesystem::path pack = voices_dir / (voice + ".kokorovoice");
        const std::filesystem::path stock =
            tts_root / "kokoro" / "voices" / (voice + ".kokorovoice");
        if (std::filesystem::is_regular_file(pack, error)) {
            // The imported packs live in this repo, not in the asset tree Moonshine
            // searches. Handing the bytes in under the voice's own key is how an extra
            // voice is added without copying it into that tree. Checked first, and the
            // stock file checked below, because a missing id is otherwise quietly
            // replaced with the language's default voice.
            voice_bytes = read_bytes(pack);
            const std::string key = "kokoro/voices/" + voice + ".kokorovoice";
            return ::moonshine::TextToSpeech::loadFromMemory(
                {{key, {voice_bytes.data(), voice_bytes.size()}}}, "en_gb", options);
        }
        if (std::filesystem::is_regular_file(stock, error)) {
            return ::moonshine::TextToSpeech("en_gb", options);
        }
        throw std::runtime_error("no Kokoro voice \"" + voice + "\" in " + voices_dir.string() +
                                 " or " + stock.parent_path().string() +
                                 " (--voices lists the imported ones)");
    }
#endif

    ~Impl() {
        {
            std::lock_guard<std::mutex> held(mutex);
            stopping = true;
            queue.clear();
            ++epoch;
            cancel.store(true);
        }
        pending.notify_all();
        drained.notify_all();
        if (worker.joinable()) worker.join();
    }

    // The words as the mod will have them: the turn file is one line with each run of
    // whitespace a single space, and the voice marker quotes it. A line made from the
    // app's own copy of the words, newlines and all, has to be found by that.
    // The same flattening the phrase cache keys on, so a line made ahead and a line
    // read back are found by the same words.
    static std::string flatten(std::string_view text) { return phrase_flat(text); }

    // Under the mutex.
    const Made* find_made(const std::string& key, const std::string& actor) const {
        for (const Made& line : made) {
            if (line.key == key && line.actor == actor) return &line;
        }
        return nullptr;
    }

    // Under the mutex: whether a say() of these words, in this actor, is waiting its turn.
    bool asked_for(const std::string& key, const std::string& actor, LipTag* lips = nullptr) const {
        for (const Job& job : queue) {
            if (job.kind == Job::Kind::Say && job.actor == actor && flatten(job.text) == key) {
                if (lips) *lips = job.lips;
                return true;
            }
        }
        return false;
    }

    // The whole line, made and kept. Nothing here is cancelled by a hush -- that is for
    // what is about to be heard, and this is not -- but the line may be asked for while it
    // is still being made: a look and then the press a second later, which is how a
    // conversation usually opens. Then it is played from here, chunk by chunk as a spoken
    // line is, with what is made so far ahead of the speaker; waiting for the whole line
    // first was the greeting arriving 3 to 4 seconds after its caption on the Windows laptop beside the
    // game, against under a second for every line after it (noted 11 Oct 2026). The say()
    // it answered is taken off the queue, and the line is kept all the same.
    //
    // False when the making was cut short: by a say() of other words, which wants the
    // engine now (the line goes back on the queue to be made after), or by a hush while
    // the line was being handed over. A line cut is not kept, since what there is of it
    // is not the line.
    // The line from the phrase cache, or nothing. The copy of the cache is taken under
    // the mutex and the file is read after, so a long decode does not hold it.
    std::optional<PhraseAudio> from_cache(const std::string& actor, std::string_view text) {
        std::shared_ptr<const PhraseCache> cache;
        {
            std::lock_guard<std::mutex> held(mutex);
            cache = phrases;
        }
        if (!cache || cache->size() == 0) return std::nullopt;
        return cache->find(actor, text);
    }

    bool make(const Job& job) {
        const std::string key = flatten(job.text);
        {
            std::lock_guard<std::mutex> held(mutex);
            if (find_made(key, job.actor)) return true;
        }
        // Known already: kept, and played from there when it is asked for. Nothing
        // for the engine, which is the whole point of having made it ahead of time.
        if (std::optional<PhraseAudio> hit = from_cache(job.actor, job.text)) {
            std::lock_guard<std::mutex> held(mutex);
            while (made.size() >= kKeptMade) made.pop_front();
            made.push_back({key, job.actor, std::move(hit->pcm), hit->rate, {}, true});
            std::cerr << "voice: cached, ready: " << key.substr(0, 60)
                      << (key.size() > 60 ? "..." : "") << "\n";
            return true;
        }
        std::vector<std::int16_t> pcm;
        int rate = 0;
        std::vector<WordSpan> words;
        std::unique_ptr<Feed> feed;
        std::thread player;
        const auto began = std::chrono::steady_clock::now();
        try {
#ifdef MOONSHINE_HAS_CLONES
            if (!job.actor.empty() && clones && actor_token(job.actor) && !std::getenv("QWEN_OFF")) {
                const std::size_t delivered = clones->speak(
                    job.actor, job.text,
                    [&](const std::int16_t* samples, std::size_t count, int chunk_rate) {
                        if (rate == 0) rate = chunk_rate;
                        pcm.insert(pcm.end(), samples, samples + count);
                        if (feed) {
                            feed->push(samples, count);
                        } else if (!cancel.load()) {
                            bool wanted = false;
                            Job asked = job;
                            {
                                std::lock_guard<std::mutex> held(mutex);
                                wanted = asked_for(key, job.actor, &asked.lips);
                                if (wanted) handing = true;
                            }
                            if (wanted) {
                                feed = std::make_unique<Feed>(lead_samples(chunk_rate),
                                                              expected_samples(job.text, chunk_rate),
                                                              chunk_rate, cancel);
                                feed->watch(lip_hooks(asked));
                                feed->push(pcm.data(), pcm.size());
                                player = std::thread([&feed, chunk_rate, this] {
                                    play_feed(*feed, chunk_rate, cancel);
                                });
                                const double at = std::chrono::duration_cast<std::chrono::milliseconds>(
                                                      std::chrono::steady_clock::now() - began).count() / 1000.0;
                                std::cerr << "voice: " << job.actor << ": asked for " << at
                                          << "s into being made ahead, spoken from here\n";
                            }
                        }
                        return !stopping && !cancel.load();
                    },
                    [&] { return stopping || cancel.load(); },
                    [&](const std::vector<WordSpan>& placed, bool final) {
                        if (final) words = placed;
                        if (feed) feed->words(placed, final);
                    });
                if (delivered == 0 && !feed && !cancel.load()) {
                    std::cerr << "voice: clone " << job.actor << " produced no audio, "
#ifdef MOONSHINE_HAS_TTS
                                 "using Kokoro\n";
#else
                                 "not spoken\n";
#endif
                    pcm.clear();
                    rate = 0;
                }
            }
#endif
#ifdef MOONSHINE_HAS_TTS
            if (pcm.empty()) {
                ::moonshine::TtsSynthesisResult spoken = synthesize_line(job.text);
                pcm = to_pcm16(spoken.samples);
                rate = spoken.sampleRateHz;
            }
#endif
        } catch (const std::exception& why) {
            std::cerr << "voice: " << why.what() << "\n";
        }
        const bool handed = feed != nullptr;
        const bool cut = cancel.load();
        const double took = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - began).count() / 1000.0;
        if (feed) feed->finish();
        if (player.joinable()) player.join();
        if (handed) {
            std::lock_guard<std::mutex> held(mutex);
            for (auto it = queue.begin(); it != queue.end(); ++it) {
                if (it->kind == Job::Kind::Say && it->actor == job.actor && flatten(it->text) == key) {
                    queue.erase(it);
                    break;
                }
            }
            // Done with, heard out or hushed: either way the mod is not to wait on it.
            spoken.push_back(job.text);
            const double held_start = feed->held_for_pace();
            const double gapped = feed->waited() / static_cast<double>(std::max(rate, 1));
            if (!cut && (held_start > 0.05 || gapped > 0.0)) {
                std::cerr << "voice: " << job.actor << ": the engine ran behind the speaker";
                if (held_start > 0.05) std::cerr << ", start held " << held_start << "s for it";
                if (gapped > 0.0) std::cerr << ", paused " << gapped << "s mid-line";
                std::cerr << "\n";
            }
        }
        if (cut) {
            bool forgotten = false;
            {
                std::lock_guard<std::mutex> held(mutex);
                forgotten = dropped;
            }
            std::cerr << "voice: " << (handed ? "hushed" : forgotten ? "dropped, the look moved on,"
                                                                     : "set aside for a line wanted now")
                      << " " << took << "s into being made ahead: " << key.substr(0, 60)
                      << (key.size() > 60 ? "..." : "") << "\n";
            return false;
        }
        if (pcm.empty() || rate <= 0) return true;
        std::cerr << "voice: made ahead in " << took << "s" << (handed ? " and spoken" : "")
                  << ": " << key.substr(0, 60) << (key.size() > 60 ? "..." : "") << "\n";
        std::lock_guard<std::mutex> held(mutex);
        while (made.size() >= kKeptMade) made.pop_front();
        made.push_back({key, job.actor, std::move(pcm), rate, std::move(words)});
        return true;
    }

    // Both engines once, on a line nobody hears. The clone engine loads its tokenizer on
    // its first line, and Kokoro its graph; either lands on the first thing a soul says
    // otherwise. The stand-in voice, which every soul without a clone of their own uses.
    void warm_up() {
        const auto began = std::chrono::steady_clock::now();
        const std::string line = "Good day to you.";
        try {
            CloneAudio cloned = qwen_clone(clones.get(), stand_in_voice(""), line);
            (void)cloned;
#ifdef MOONSHINE_HAS_TTS
            (void)synthesize_line(line);
#endif
        } catch (const std::exception& why) {
            std::cerr << "voice: " << why.what() << "\n";
        }
        const double took = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - began).count() / 1000.0;
        std::cerr << "voice: warmed up in " << took << "s\n";
    }

    void run() {
        while (true) {
            Job job;
            std::uint64_t epoch_now = 0;
            {
                std::unique_lock<std::mutex> held(mutex);
                pending.wait(held, [&] { return stopping || !queue.empty(); });
                if (stopping && queue.empty()) return;
                job = std::move(queue.front());
                queue.pop_front();
                busy = true;
                current = job.kind;
                current_key = flatten(job.text);
                current_actor = job.actor;
                handing = false;
                dropped = false;
                cancel.store(false);
                epoch_now = epoch;
            }
            bool again = false;
            if (job.kind == Job::Kind::Warm) {
                warm_up();
            } else if (job.kind == Job::Kind::Prepare) {
                again = !make(job);
            } else {
                speak(job, epoch_now);
            }
            {
                std::lock_guard<std::mutex> held(mutex);
                busy = false;
                // A line set aside for one wanted now is still wanted itself, after. Not
                // one hushed mid-handover: that line has been heard as far as it is going
                // to be.
                if (again && !stopping && !handing && !dropped) queue.push_back(std::move(job));
                if (queue.empty()) drained.notify_all();
            }
        }
    }

    void speak(const Job& job, std::uint64_t epoch_now) {
        std::vector<std::int16_t> pcm;
        int rate = 0;
        std::vector<WordSpan> words;
        bool was_cached = false;
        // Made ahead: nothing to synthesize, and the line starts the moment it is asked for.
        // A line still being made when it was asked for has already finished, since the one
        // worker makes and speaks in the order asked.
        {
            std::lock_guard<std::mutex> held(mutex);
            if (const Made* ready = find_made(flatten(job.text), job.actor)) {
                pcm = ready->pcm;
                rate = ready->rate;
                words = ready->words;
                was_cached = ready->cached;
            }
        }
        // Not made ahead, but spoken before and kept. The whole line is in hand, so it
        // plays as a made one does, mouth and all, and the model is not asked.
        if (pcm.empty()) {
            if (std::optional<PhraseAudio> hit = from_cache(job.actor, job.text)) {
                pcm = std::move(hit->pcm);
                rate = hit->rate;
                was_cached = true;
            }
        }
        if (!pcm.empty()) {
            std::cerr << "voice: " << (job.actor.empty() ? voice : job.actor)
                      << (was_cached ? " speaks a cached line\n" : " speaks a line made ahead\n");
        }
        // Set when the clone engine spoke the line itself, chunk by chunk, as it was
        // made; then there is nothing left to synthesize or play here.
        bool streamed = false;
        if (pcm.empty()) {
            try {
                if (!job.actor.empty()) {
                    streamed = qwen_stream(clones.get(), job.actor, job.text, cancel, lip_hooks(job));
                }
                if (!streamed) {
#ifdef MOONSHINE_HAS_TTS
                    ::moonshine::TtsSynthesisResult spoken = synthesize_line(job.text);
                    pcm = to_pcm16(spoken.samples);
                    rate = spoken.sampleRateHz;
#endif
                }
            } catch (const std::exception& why) {
                std::cerr << "voice: " << why.what() << "\n";
            }
        }
        bool dropped = false;
        {
            std::lock_guard<std::mutex> held(mutex);
            dropped = epoch_now != epoch;
        }
        if (!streamed && !dropped && !pcm.empty() && rate > 0) {
            play_pcm(pcm, rate, cancel, lip_hooks(job), words);
        }
        // Done with, whether heard to the end, hushed part way, or never spoken at all: the
        // mod holds the camera on a line until it hears this, and should not hold it on a
        // line nobody is going to hear.
        std::lock_guard<std::mutex> held(mutex);
        spoken.push_back(job.text);
    }
};

Speaker::Speaker(std::string voice, std::filesystem::path voices_dir,
                 std::filesystem::path tts_root) {
    const std::string id = plain_voice_id(voice);
    if (id.empty()) throw std::runtime_error("a Kokoro voice needs a name");
    impl_ = std::make_unique<Impl>(id, std::move(voices_dir), std::move(tts_root));
}

Speaker::~Speaker() = default;

void Speaker::say(std::string_view text, std::string_view actor, LipTag lips) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\n')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\n')) text.remove_suffix(1);
    if (text.empty()) return;
    {
        std::lock_guard<std::mutex> held(impl_->mutex);
        // Ahead of anything being made for later, behind what is already to be said: a
        // line asked for is wanted now, and the greetings of the souls looked at on the
        // way here were each a few seconds' work queued in front of it (noted 12 Oct 2026,
        // ten seconds and more before the first word of a conversation).
        const std::string key = Impl::flatten(text);
        // The same words waiting to be made ahead are made by saying them; the job would
        // only make them again afterwards, on the engine the conversation's next line wants.
        impl_->queue.erase(std::remove_if(impl_->queue.begin(), impl_->queue.end(),
                                          [&](const Impl::Job& job) {
                                              return job.kind == Impl::Job::Kind::Prepare &&
                                                     job.actor == actor && Impl::flatten(job.text) == key;
                                          }),
                           impl_->queue.end());
        auto at = impl_->queue.begin();
        while (at != impl_->queue.end() && at->kind == Impl::Job::Kind::Say) ++at;
        impl_->queue.insert(at, {Impl::Job::Kind::Say, std::string(text), std::string(actor), std::move(lips)});
        // And a line being made for later gives way too, unless it is this one, which is
        // about to be handed over (make); set aside, it is made after.
        if (impl_->busy && impl_->current == Impl::Job::Kind::Prepare && !impl_->handing &&
            !(impl_->current_key == key && impl_->current_actor == actor)) {
            impl_->cancel.store(true);
        }
    }
    impl_->pending.notify_one();
}

void Speaker::prepare(std::string_view text, std::string_view actor) {
    const std::string key = Impl::flatten(text);
    if (key.empty()) return;
    {
        std::lock_guard<std::mutex> held(impl_->mutex);
        if (impl_->find_made(key, std::string(actor))) return;
        // Asked for twice before it was made -- the look and then the press -- is one job.
        for (const Impl::Job& job : impl_->queue) {
            if (job.kind == Impl::Job::Kind::Prepare && job.actor == actor &&
                Impl::flatten(job.text) == key) {
                return;
            }
        }
        impl_->queue.push_back({Impl::Job::Kind::Prepare, std::string(text), std::string(actor)});
    }
    impl_->pending.notify_one();
}

void Speaker::forget_ahead() {
    std::size_t waiting = 0;
    bool cut = false;
    {
        std::lock_guard<std::mutex> held(impl_->mutex);
        const std::size_t before = impl_->queue.size();
        impl_->queue.erase(std::remove_if(impl_->queue.begin(), impl_->queue.end(),
                                          [](const Impl::Job& job) {
                                              return job.kind == Impl::Job::Kind::Prepare;
                                          }),
                           impl_->queue.end());
        waiting = before - impl_->queue.size();
        if (impl_->busy && impl_->current == Impl::Job::Kind::Prepare && !impl_->handing) {
            impl_->dropped = true;
            impl_->cancel.store(true);
            cut = true;
        }
    }
    if (waiting > 0 || cut) {
        std::cerr << "voice: the look moved on; " << waiting << " line" << (waiting == 1 ? "" : "s")
                  << " waiting to be made ahead dropped" << (cut ? ", and the one being made cut" : "")
                  << "\n";
    }
}

void Speaker::warm() {
    {
        std::lock_guard<std::mutex> held(impl_->mutex);
        impl_->queue.push_back({Impl::Job::Kind::Warm, {}, {}});
    }
    impl_->pending.notify_one();
}

void Speaker::hush() {
    {
        std::lock_guard<std::mutex> held(impl_->mutex);
        // What was going to be heard goes; a line being made ahead is still wanted.
        impl_->queue.erase(std::remove_if(impl_->queue.begin(), impl_->queue.end(),
                                          [](const Impl::Job& job) {
                                              return job.kind == Impl::Job::Kind::Say;
                                          }),
                           impl_->queue.end());
        ++impl_->epoch;
        // The line being heard stops; a line being made ahead goes on being made, unless
        // it is the one being heard.
        if (!impl_->busy || impl_->current != Impl::Job::Kind::Prepare || impl_->handing) {
            impl_->cancel.store(true);
        }
    }
    impl_->pending.notify_all();
    impl_->drained.notify_all();
}

void Speaker::wait() {
    std::unique_lock<std::mutex> held(impl_->mutex);
    impl_->drained.wait(held, [&] { return impl_->queue.empty() && !impl_->busy; });
}

bool Speaker::speaking() const {
    std::lock_guard<std::mutex> held(impl_->mutex);
    return impl_->busy || !impl_->queue.empty();
}

std::vector<std::string> Speaker::take_spoken() {
    std::lock_guard<std::mutex> held(impl_->mutex);
    std::vector<std::string> lines = std::move(impl_->spoken);
    impl_->spoken.clear();
    return lines;
}

const std::string& Speaker::voice() const { return impl_->voice; }

void Speaker::set_lips(LipSink sink) {
    std::lock_guard<std::mutex> held(impl_->lips_mutex);
    impl_->lips = std::move(sink);
}

std::size_t Speaker::facial_clips() const { return impl_->facials.size(); }

Speaker::RenderedLine Speaker::render(std::string_view text, std::string_view actor) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\n')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\n')) text.remove_suffix(1);
    if (text.empty()) throw std::runtime_error("voice: nothing to say");
    // The playback thread only synthesizes a queued line, and this path never queues one.
    RenderedLine line;
    if (!actor.empty()) {
        const CloneAudio cloned = qwen_clone(impl_->clones.get(), actor, std::string(text));
        if (!cloned.samples.empty()) {
            line.samples = std::move(cloned.samples);
            line.rate = cloned.rate;
            return line;
        }
    }
#ifdef MOONSHINE_HAS_TTS
    const ::moonshine::TtsSynthesisResult spoken = impl_->synthesize_line(std::string(text));
    line.samples = to_pcm16(spoken.samples);
    line.rate = spoken.sampleRateHz;
#else
    throw std::runtime_error("voice: clone " + std::string(actor) + " produced nothing to render");
#endif
    if (line.samples.empty() || line.rate <= 0) {
        throw std::runtime_error("voice: nothing to render");
    }
    return line;
}

void Speaker::write(std::string_view text, const std::filesystem::path& wav, std::string_view actor) {
    const RenderedLine line = render(text, actor);
    write_pcm16_wav(wav, line.samples, line.rate);
}

void Speaker::set_phrase_cache(const std::filesystem::path& dir) {
    auto cache = std::make_shared<PhraseCache>();
    if (!dir.empty()) cache->open(dir);
    std::lock_guard<std::mutex> held(impl_->mutex);
    impl_->phrases = std::move(cache);
}

std::size_t Speaker::cached_lines() const {
    std::lock_guard<std::mutex> held(impl_->mutex);
    return impl_->phrases ? impl_->phrases->size() : 0;
}

#else

struct Speaker::Impl {};

Speaker::Speaker(std::string, std::filesystem::path, std::filesystem::path) {
    throw std::runtime_error(
        "this build has no Kokoro voice: Moonshine Voice was not linked");
}

Speaker::~Speaker() = default;

void Speaker::say(std::string_view, std::string_view, LipTag) {}
void Speaker::set_lips(LipSink) {}
std::size_t Speaker::facial_clips() const { return 0; }
void Speaker::prepare(std::string_view, std::string_view) {}
void Speaker::forget_ahead() {}
void Speaker::warm() {}
void Speaker::hush() {}
void Speaker::wait() {}
bool Speaker::speaking() const { return false; }
std::vector<std::string> Speaker::take_spoken() { return {}; }
void Speaker::write(std::string_view, const std::filesystem::path&, std::string_view) {
    throw std::runtime_error("this build has no Kokoro voice: Moonshine Voice was not linked");
}
Speaker::RenderedLine Speaker::render(std::string_view, std::string_view) {
    throw std::runtime_error("this build has no Kokoro voice: Moonshine Voice was not linked");
}
void Speaker::set_phrase_cache(const std::filesystem::path&) {}
std::size_t Speaker::cached_lines() const { return 0; }
const std::string& Speaker::voice() const {
    static const std::string empty;
    return empty;
}

#endif

}  // namespace moonshine
