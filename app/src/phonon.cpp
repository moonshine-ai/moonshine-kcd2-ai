#include "phonon.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <set>
#include <thread>

// The header is written for C and has no linkage guard of its own.
extern "C" {
#include "ptts.h"
}

namespace moonshine {

namespace {

// Phonon's output rate, fixed by the Mimi codec it decodes with.
constexpr int kSampleRate = 24000;

// The runtime's worker pool is sized from this variable, read once, before the first
// ptts_new; its own default is one thread per logical CPU, which is the worst setting
// measured. On the Windows laptop (32 threads, 28 Oct 2026), a 15.6 s line: 1 thread 2.8 s,
// 2 2.2 s, 4 1.8 s, 8 1.8 s, 16 2.0 s, 32 3.9 s. Eight is the knee, and leaves the rest of the
// machine to the game and the language model. A value set by hand is kept.
void size_thread_pool() {
    if (const char* set = std::getenv("RAYON_NUM_THREADS"); set && *set) return;
    unsigned all = std::thread::hardware_concurrency();
    if (all == 0) all = 8;
    const std::string threads = std::to_string(std::min(all, 8u));
#ifdef _WIN32
    _putenv_s("RAYON_NUM_THREADS", threads.c_str());
#else
    setenv("RAYON_NUM_THREADS", threads.c_str(), 1);
#endif
}

std::string last_error(const PttsHandle* handle) {
    const char* why = ptts_last_error(handle);
    return why ? std::string(why) : std::string("no reason given");
}

// The names in a ptts_voices block: NUL-separated, ending in a second NUL.
std::vector<std::string> names_in(const char* block) {
    std::vector<std::string> names;
    if (!block) return names;
    for (const char* at = block; *at; at += std::strlen(at) + 1) names.emplace_back(at);
    return names;
}

}  // namespace

struct PhononEngine::Impl {
    std::filesystem::path model_dir;
    std::filesystem::path voices_dir;
    PttsHandle* handle = nullptr;
    std::string failure;
    std::vector<std::string> voices;
    std::string current_voice;
    // Voices asked for that the checkpoint does not have, each said once.
    std::set<std::string> unknown;
    // One line at a time: the runtime wants its calls from one thread at a time.
    std::mutex speaking;

    // What on_audio and cancelled see from inside the C callback.
    struct Sink {
        const OnAudio* on_audio;
        const Cancelled* cancelled;
        std::vector<std::int16_t> chunk;
        std::size_t delivered = 0;
    };

    static bool deliver(const float* pcm, std::size_t count, void* user) {
        Sink& sink = *static_cast<Sink*>(user);
        if (sink.cancelled && *sink.cancelled && (*sink.cancelled)()) return false;
        sink.chunk.resize(count);
        for (std::size_t i = 0; i < count; ++i) {
            const float clamped = std::clamp(pcm[i], -1.0f, 1.0f);
            sink.chunk[i] = static_cast<std::int16_t>(std::lrint(clamped * 32767.0f));
        }
        sink.delivered += count;
        return (*sink.on_audio)(sink.chunk.data(), count, kSampleRate);
    }

    // Switch voices when the line's differs from the last; the runtime keeps the choice.
    bool choose(std::string_view voice) {
        std::string wanted(voice);
        if (std::find(voices.begin(), voices.end(), wanted) == voices.end()) {
            if (unknown.insert(wanted).second) {
                std::cerr << "voice: phonon has no voice named " << wanted << " (" << model_dir.string()
                          << "/voices/" << wanted << ".safetensors); speaking it in the default voice\n";
            }
            wanted = "default";
            if (std::find(voices.begin(), voices.end(), wanted) == voices.end()) return false;
        }
        if (wanted == current_voice) return true;
        if (!ptts_set_voice(handle, wanted.c_str())) {
            std::cerr << "voice: phonon could not switch to " << wanted << ": " << last_error(handle) << "\n";
            return false;
        }
        current_voice = wanted;
        return true;
    }
};

PhononEngine::PhononEngine(std::filesystem::path model_dir, std::filesystem::path voices_dir)
    : impl_(std::make_unique<Impl>()) {
    impl_->model_dir = std::move(model_dir);
    impl_->voices_dir = std::move(voices_dir);
    if (impl_->model_dir.empty()) {
        impl_->failure = "no phonon checkpoint (models/phonon-31m-en, or PHONON_MODEL_DIR)";
        std::cerr << "voice: " << impl_->failure << "\n";
        return;
    }
    size_thread_pool();
    const std::string dir = impl_->model_dir.string();
    impl_->handle = ptts_new(dir.c_str(), PTTS_UNIT_CPU, "en");
    if (!impl_->handle) {
        impl_->failure = "phonon did not load " + dir + ": " + last_error(nullptr);
        std::cerr << "voice: " << impl_->failure << "\n";
        return;
    }
    impl_->voices = names_in(ptts_voices(impl_->handle));
    if (impl_->voices.empty()) {
        std::cerr << "voice: phonon checkpoint " << dir << " has no voices; every line will be in no particular voice\n";
    }
}

PhononEngine::~PhononEngine() {
    if (impl_ && impl_->handle) ptts_free(impl_->handle);
}

std::filesystem::path PhononEngine::find_model() {
    if (const char* env = std::getenv("PHONON_MODEL_DIR"); env && *env) {
        return std::filesystem::path(env);
    }
    std::error_code error;
    std::filesystem::path dir = std::filesystem::current_path(error);
    for (int i = 0; i < 4 && !dir.empty(); ++i) {
        const std::filesystem::path candidate = dir / "models" / "phonon-31m-en";
        if (std::filesystem::is_regular_file(candidate / "config.json", error) &&
            (std::filesystem::is_regular_file(candidate / "model.q8.gguf", error) ||
             std::filesystem::is_regular_file(candidate / "model.safetensors", error))) {
            return candidate;
        }
        const std::filesystem::path parent = dir.parent_path();
        if (parent == dir) break;
        dir = parent;
    }
    return {};
}

bool PhononEngine::ready() { return impl_->handle != nullptr; }

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
    if (!impl_->handle) return 0;
    std::lock_guard<std::mutex> held(impl_->speaking);
    if (!impl_->voices.empty() && !impl_->choose(voice)) return 0;
    Impl::Sink sink{&on_audio, &cancelled, {}, 0};
    const std::string line(text);
    PttsResult result{};
    if (!ptts_speak(impl_->handle, line.c_str(), &Impl::deliver, &sink, &result)) {
        std::cerr << "voice: phonon failed on \"" << line << "\": " << last_error(impl_->handle) << "\n";
        return sink.delivered;
    }
    return sink.delivered;
}

std::vector<std::string> PhononEngine::voices() const { return impl_->voices; }

const std::filesystem::path& PhononEngine::model_dir() const { return impl_->model_dir; }

const std::filesystem::path& PhononEngine::voices_dir() const { return impl_->voices_dir; }

}  // namespace moonshine
