#include "phonon.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <list>
#include <mutex>
#include <set>

#include "moonshine-c-api.h"
#include "moonshine-cpp.h"

namespace moonshine {

namespace {

// Phonon's output rate, fixed by the Mimi codec it decodes with. The chunks say it too, and
// that is what is passed on; this is for the line that never gets a chunk.
constexpr int kSampleRate = 24000;

// How many voices stay resident, each about 350 MB (see phonon.h). Two is a conversation:
// Henry and whoever he is talking to.
constexpr std::size_t kDefaultResident = 2;

std::size_t resident_limit() {
    if (const char* env = std::getenv("MOONSHINE_PHONON_VOICES"); env && *env) {
        const long asked = std::strtol(env, nullptr, 10);
        if (asked >= 1) return static_cast<std::size_t>(asked);
        std::cerr << "voice: MOONSHINE_PHONON_VOICES=" << env << " is not a count of one or more; using "
                  << kDefaultResident << "\n";
    }
    return kDefaultResident;
}

std::string error_text(std::int32_t code) {
    const char* why = moonshine_error_to_string(code);
    return why && *why ? std::string(why) : "error " + std::to_string(code);
}

// A folder that holds a model the library can run: the graphs it cannot start without.
bool bundle_at(const std::filesystem::path& dir) {
    std::error_code error;
    return std::filesystem::is_regular_file(dir / "backbone.ort", error) &&
           std::filesystem::is_regular_file(dir / "model.json", error) &&
           std::filesystem::is_regular_file(dir / "tokenizer.bin", error);
}

}  // namespace

struct PhononEngine::Impl {
    std::filesystem::path model_dir;
    std::filesystem::path voices_dir;
    // The library's asset root: the folder with phonon/ in it.
    std::filesystem::path root;
    std::string failure;
    std::vector<std::string> voices;
    // The voice a line gets when it asks for one the model folder lacks.
    std::string stand_in;
    // Voices asked for that the model folder does not have, each said once.
    std::set<std::string> unknown;
    // One line at a time. The library serialises its own calls, but a line is a run of them
    // and a second line's chunks must not come out of the first's.
    std::mutex speaking;

    struct Resident {
        std::string voice;
        std::unique_ptr<::moonshine::TextToSpeech> tts;
    };
    // Most recently spoken first. Under `speaking`.
    std::list<Resident> resident;
    std::size_t limit = kDefaultResident;

    // The synthesizer for a voice, made if it is not already held and the least recently
    // used let go if that is more than `limit`. Null, with the reason on stderr, when the
    // library would not make it. Under `speaking`.
    ::moonshine::TextToSpeech* open(const std::string& voice) {
        for (auto it = resident.begin(); it != resident.end(); ++it) {
            if (it->voice != voice) continue;
            if (it != resident.begin()) resident.splice(resident.begin(), resident, it);
            return resident.front().tts.get();
        }
        // Made after letting one go, not before: two of them at once is the memory this is
        // keeping down.
        while (resident.size() >= limit) resident.pop_back();
        // The voice by its file rather than by the library's catalogue (phonon/voices.json),
        // so that any .safetensors in the folder is a voice, one Gradium Studio made as well
        // as the catalogue's.
        const std::filesystem::path file = model_dir / "voices" / (voice + ".safetensors");
        try {
            auto tts = std::make_unique<::moonshine::TextToSpeech>(
                "en_us", ::moonshine::Options{{"g2p_root", root.string()},
                                              {"voice", "phonon"},
                                              {"phonon_voice_file", file.string()}});
            resident.push_front({voice, std::move(tts)});
            return resident.front().tts.get();
        } catch (const std::exception& why) {
            std::cerr << "voice: phonon could not make the voice " << voice << " from " << file.string() << ": "
                      << why.what() << "\n";
            return nullptr;
        }
    }

    // The voice to speak a line in: the one asked for, or the stand-in, which is said once.
    std::string choose(std::string_view asked) {
        std::string wanted(asked);
        if (std::find(voices.begin(), voices.end(), wanted) != voices.end()) return wanted;
        if (unknown.insert(wanted).second) {
            std::cerr << "voice: phonon has no voice named " << wanted << " (" << model_dir.string() << "/voices/"
                      << wanted << ".safetensors); speaking it in " << stand_in << "\n";
        }
        return stand_in;
    }
};

PhononEngine::PhononEngine(std::filesystem::path model_dir, std::filesystem::path voices_dir)
    : impl_(std::make_unique<Impl>()) {
    impl_->voices_dir = std::move(voices_dir);
    impl_->limit = resident_limit();
    if (model_dir.empty()) {
        impl_->failure = "no phonon model (models/phonon, or PHONON_MODEL_DIR; scripts/fetch_models.sh --only phonon)";
        std::cerr << "voice: " << impl_->failure << "\n";
        return;
    }
    // A trailing separator would leave the folder's name empty.
    model_dir = model_dir.lexically_normal();
    if (model_dir.filename().empty()) model_dir = model_dir.parent_path();
    impl_->model_dir = model_dir;
    // The library reads <root>/phonon/<file>, so the folder's name is not ours to choose.
    if (model_dir.filename() != "phonon") {
        impl_->failure = "the phonon model folder must be named phonon (Moonshine Voice reads <root>/phonon/...), "
                         "and " + model_dir.string() + " is not";
        std::cerr << "voice: " << impl_->failure << "\n";
        return;
    }
    if (!bundle_at(model_dir)) {
        impl_->failure = model_dir.string() + " has no backbone.ort, model.json and tokenizer.bin "
                         "(scripts/fetch_models.sh --only phonon)";
        std::cerr << "voice: " << impl_->failure << "\n";
        return;
    }
    impl_->root = model_dir.parent_path();

    std::error_code error;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(model_dir / "voices", error)) {
        if (entry.path().extension() == ".safetensors") impl_->voices.push_back(entry.path().stem().string());
    }
    std::sort(impl_->voices.begin(), impl_->voices.end());
    if (impl_->voices.empty()) {
        impl_->failure = model_dir.string() + "/voices holds no .safetensors voice";
        std::cerr << "voice: " << impl_->failure << "\n";
        return;
    }
    // Plain voices first; any will do if neither is there.
    impl_->stand_in = impl_->voices.front();
    for (const char* plain : {"toby", "maeve"}) {
        if (std::find(impl_->voices.begin(), impl_->voices.end(), plain) != impl_->voices.end()) {
            impl_->stand_in = plain;
            break;
        }
    }
    // Made now, so that a model that will not load is known at start-up and not on the first
    // line, and so that ready() is the truth.
    if (!impl_->open(impl_->stand_in)) {
        impl_->failure = "Moonshine Voice did not load the phonon model in " + model_dir.string();
        return;
    }
}

PhononEngine::~PhononEngine() = default;

std::filesystem::path PhononEngine::find_model() {
    if (const char* env = std::getenv("PHONON_MODEL_DIR"); env && *env) {
        return std::filesystem::path(env);
    }
    std::error_code error;
    std::filesystem::path dir = std::filesystem::current_path(error);
    for (int i = 0; i < 4 && !dir.empty(); ++i) {
        const std::filesystem::path candidate = dir / "models" / "phonon";
        if (bundle_at(candidate)) return candidate;
        const std::filesystem::path parent = dir.parent_path();
        if (parent == dir) break;
        dir = parent;
    }
    // Where Moonshine Voice's own downloaders put it (the same tree Kokoro's assets are
    // found in, voice.cpp's find_tts_root).
    std::vector<std::filesystem::path> caches;
    if (const char* home = std::getenv("HOME")) {
        const std::filesystem::path base(home);
        caches.push_back(base / "Library/Caches/moonshine_voice/download.moonshine.ai/tts/phonon");
        caches.push_back(base / ".cache/moonshine_voice/download.moonshine.ai/tts/phonon");
    }
    if (const char* local = std::getenv("LOCALAPPDATA")) {
        caches.push_back(std::filesystem::path(local) / "moonshine_voice" / "download.moonshine.ai" / "tts" /
                         "phonon");
    }
    for (const std::filesystem::path& candidate : caches) {
        if (bundle_at(candidate)) return candidate;
    }
    return {};
}

bool PhononEngine::ready() {
    std::lock_guard<std::mutex> held(impl_->speaking);
    return !impl_->resident.empty();
}

const std::string& PhononEngine::failure() const { return impl_->failure; }

PhononEngine::Audio PhononEngine::speak(std::string_view voice, std::string_view text) {
    Audio audio;
    const std::size_t delivered = speak(
        voice, text,
        [&](const std::int16_t* samples, std::size_t count, int rate) {
            audio.rate = rate;
            audio.samples.insert(audio.samples.end(), samples, samples + count);
            return true;
        },
        [] { return false; });
    if (delivered == 0) audio = Audio{};
    return audio;
}

std::size_t PhononEngine::speak(std::string_view voice, std::string_view text, const OnAudio& on_audio,
                                const Cancelled& cancelled) {
    if (!impl_->failure.empty()) return 0;
    std::lock_guard<std::mutex> held(impl_->speaking);
    const std::string chosen = impl_->choose(voice);
    ::moonshine::TextToSpeech* tts = impl_->open(chosen);
    if (!tts) return 0;
    const std::int32_t handle = tts->getHandle();
    const std::string line(text);

    // The line goes in whole, and the end of input says so, which is what lets the library
    // cut it into sentences and speak the last without waiting for more text.
    if (const std::int32_t pushed = moonshine_tts_push_text(handle, line.c_str());
        pushed != MOONSHINE_ERROR_NONE) {
        std::cerr << "voice: phonon failed on \"" << line << "\": " << error_text(pushed) << "\n";
        moonshine_tts_cancel(handle);
        return 0;
    }
    moonshine_tts_end_input(handle);

    std::size_t delivered = 0;
    std::vector<std::int16_t> chunk;
    // Set when this call abandons the line: the library then holds a cancelled report for
    // the next pull (a line stopped on purpose is told from one that ran out of text), and
    // that pull has to be made here, or it ends the next line before it begins.
    bool stopped = false;
    while (true) {
        if (cancelled && cancelled()) {
            stopped = true;
            break;
        }
        const tts_chunk_t* made = nullptr;
        const std::int32_t status = moonshine_tts_next_chunk(handle, 0, &made);
        if (status == MOONSHINE_TTS_END_OF_STREAM) break;
        // A report left from a line cut earlier, by another call; not this line's.
        if (status == MOONSHINE_TTS_CANCELLED) continue;
        if (status == MOONSHINE_TTS_NEED_TEXT) break;
        if (status != MOONSHINE_ERROR_NONE || !made) {
            std::cerr << "voice: phonon failed on \"" << line << "\": " << error_text(status) << "\n";
            stopped = true;
            break;
        }
        const std::size_t count = static_cast<std::size_t>(made->audio_data_count);
        if (count == 0) continue;
        chunk.resize(count);
        for (std::size_t i = 0; i < count; ++i) {
            const float clamped = std::clamp(made->audio_data[i], -1.0f, 1.0f);
            chunk[i] = static_cast<std::int16_t>(std::lrint(clamped * 32767.0f));
        }
        delivered += count;
        const int rate = made->sample_rate > 0 ? made->sample_rate : kSampleRate;
        if (!on_audio(chunk.data(), count, rate)) {
            stopped = true;
            break;
        }
    }
    if (stopped) {
        moonshine_tts_cancel(handle);
        const tts_chunk_t* none = nullptr;
        (void)moonshine_tts_next_chunk(handle, 0, &none);
    }
    return delivered;
}

std::vector<std::string> PhononEngine::voices() const { return impl_->voices; }

const std::filesystem::path& PhononEngine::model_dir() const { return impl_->model_dir; }

const std::filesystem::path& PhononEngine::voices_dir() const { return impl_->voices_dir; }

}  // namespace moonshine
