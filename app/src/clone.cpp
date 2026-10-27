#include "clone.h"

#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <map>
#include <mutex>
#include <thread>

#include "qwen_tts_voice_file.h"

namespace moonshine {

namespace {

int env_int(const char* name, int fallback) {
    const char* value = std::getenv(name);
    if (!value || !*value) return fallback;
    return std::atoi(value);
}

bool voice_name_ok(std::string_view name) {
    if (name.empty() || name.size() > 64) return false;
    for (char c : name) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (!std::isalnum(u) && c != '-' && c != '_') return false;
    }
    return true;
}

void to_pcm16(const float* samples, std::size_t count, std::int16_t* out) {
    for (std::size_t i = 0; i < count; ++i) {
        float sample = samples[i];
        if (sample > 1.f) sample = 1.f;
        if (sample < -1.f) sample = -1.f;
        out[i] = static_cast<std::int16_t>(sample * 32767.f);
    }
}

}  // namespace

struct CloneEngine::Impl {
    std::filesystem::path model_dir;
    std::filesystem::path voices_dir;

    // The load runs on `loader`; everything after it is guarded by `mutex`, which also
    // serialises speak(): the engine has one context and one set of scratch buffers.
    std::mutex mutex;
    std::condition_variable loaded_cv;
    bool loaded = false;
    std::string failure;
    qwen_voice_engine_t* engine = nullptr;
    std::map<std::string, qwen_voice_t*> open_voices;
    std::thread loader;

    Impl(std::filesystem::path model, std::filesystem::path voices)
        : model_dir(std::move(model)), voices_dir(std::move(voices)) {
        loader = std::thread([this] { load(); });
    }

    ~Impl() {
        if (loader.joinable()) loader.join();
        std::lock_guard<std::mutex> held(mutex);
        for (auto& [name, voice] : open_voices) qwen_voice_close(voice);
        open_voices.clear();
        if (engine) qwen_voice_engine_close(engine);
        engine = nullptr;
    }

    void load() {
#ifdef _WIN32
        // The engine reports through C stderr, which the Windows CRT buffers when it is
        // a pipe; what it said last would otherwise be lost with the process.
        std::setvbuf(stderr, nullptr, _IONBF, 0);
#endif
        const auto started = std::chrono::steady_clock::now();
        char err[512] = {0};
        qwen_voice_engine_t* opened = nullptr;
        std::string why;
        if (model_dir.empty()) {
            why = "no Qwen3-TTS weights: set QWEN_MODEL_DIR or put them in models/qwen3-tts-0.6b";
        } else {
            // The engine reports every stage on stderr unless told to be quiet.
            const int silent = env_int("QWEN_VERBOSE", 0) ? 0 : 1;
            opened = qwen_voice_engine_open(model_dir.string().c_str(), env_int("QWEN_THREADS", 0),
                                            env_int("QWEN_BITS", 8), silent, err, sizeof err);
            if (!opened) why = err[0] ? err : "the engine could not load the model";
        }
        const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started);
        {
            std::lock_guard<std::mutex> held(mutex);
            engine = opened;
            failure = why;
            loaded = true;
        }
        loaded_cv.notify_all();
        if (opened) {
            std::cerr << "voice: Qwen3-TTS loaded from " << model_dir.string() << " in "
                      << (took.count() / 1000.0) << "s\n";
        } else {
            std::cerr << "voice: " << why << "\n";
        }
    }

    // Caller holds `mutex`.
    qwen_voice_t* voice_named(std::string_view name) {
        const std::string key(name);
        auto it = open_voices.find(key);
        if (it != open_voices.end()) return it->second;
        if (!voice_name_ok(name)) return nullptr;
        const std::filesystem::path file = voices_dir / (key + ".qvoice");
        std::error_code error;
        if (!std::filesystem::is_regular_file(file, error)) {
            std::cerr << "voice: no clone " << file.string() << "\n";
            open_voices[key] = nullptr;
            return nullptr;
        }
        char err[512] = {0};
        qwen_voice_t* voice = qwen_voice_open(engine, file.string().c_str(), err, sizeof err);
        if (!voice) std::cerr << "voice: " << (err[0] ? err : "could not read the clone") << "\n";
        open_voices[key] = voice;
        return voice;
    }
};

CloneEngine::CloneEngine(std::filesystem::path model_dir, std::filesystem::path voices_dir)
    : impl_(std::make_unique<Impl>(std::move(model_dir), std::move(voices_dir))) {}

CloneEngine::~CloneEngine() = default;

std::filesystem::path CloneEngine::find_model() {
    if (const char* env = std::getenv("QWEN_MODEL_DIR"); env && *env) {
        return std::filesystem::path(env);
    }
    std::error_code error;
    std::filesystem::path dir = std::filesystem::current_path(error);
    for (int i = 0; i < 4 && !dir.empty(); ++i) {
        const std::filesystem::path candidate = dir / "models" / "qwen3-tts-0.6b";
        if (std::filesystem::is_regular_file(candidate / "model.safetensors", error)) return candidate;
        const std::filesystem::path parent = dir.parent_path();
        if (parent == dir) break;
        dir = parent;
    }
    return {};
}

bool CloneEngine::ready() {
    std::unique_lock<std::mutex> held(impl_->mutex);
    impl_->loaded_cv.wait(held, [&] { return impl_->loaded; });
    return impl_->engine != nullptr;
}

const std::string& CloneEngine::failure() const { return impl_->failure; }

CloneEngine::Audio CloneEngine::speak(std::string_view voice, std::string_view text) {
    Audio audio;
    std::unique_lock<std::mutex> held(impl_->mutex);
    impl_->loaded_cv.wait(held, [&] { return impl_->loaded; });
    if (!impl_->engine) return audio;
    qwen_voice_t* clone = impl_->voice_named(voice);
    if (!clone) return audio;

    const std::string line(text);
    const auto started = std::chrono::steady_clock::now();
    float* samples = nullptr;
    int count = 0;
    char err[512] = {0};
    // The same seed every line, as the stand-in takes were made: one voice, not a new
    // reading each time. QWEN_SEED picks another; 0 lets the engine draw from the clock.
    const unsigned seed = static_cast<unsigned>(env_int("QWEN_SEED", 42));
    if (qwen_voice_speak(impl_->engine, clone, line.c_str(), seed, &samples, &count, err, sizeof err) != 0) {
        std::cerr << "voice: clone " << voice << ": " << (err[0] ? err : "generation failed") << "\n";
        return audio;
    }
    audio.rate = qwen_voice_sample_rate();
    audio.samples.resize(static_cast<std::size_t>(count > 0 ? count : 0));
    to_pcm16(samples, static_cast<std::size_t>(count > 0 ? count : 0), audio.samples.data());
    qwen_voice_free_samples(samples);
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    std::cerr << "voice: " << voice << " " << (count / static_cast<double>(audio.rate)) << "s in "
              << (took.count() / 1000.0) << "s\n";
    return audio;
}

namespace {

// What the engine's two callbacks are given: the caller's functions, a PCM16 scratch
// buffer reused across chunks, and the moment of the first chunk for the log.
struct Stream {
    const CloneEngine::OnAudio* on_audio = nullptr;
    const CloneEngine::Cancelled* cancelled = nullptr;
    std::vector<std::int16_t> pcm;
    std::chrono::steady_clock::time_point started;
    std::chrono::steady_clock::duration first{};
    bool stopped = false;
};

int stream_audio(const float* samples, int count, void* user) {
    auto* stream = static_cast<Stream*>(user);
    if (count <= 0) return 0;
    if (stream->first == std::chrono::steady_clock::duration{}) {
        stream->first = std::chrono::steady_clock::now() - stream->started;
    }
    stream->pcm.resize(static_cast<std::size_t>(count));
    to_pcm16(samples, static_cast<std::size_t>(count), stream->pcm.data());
    if ((*stream->on_audio)(stream->pcm.data(), stream->pcm.size(), qwen_voice_sample_rate())) return 0;
    stream->stopped = true;
    return 1;
}

int stream_cancelled(void* user) {
    auto* stream = static_cast<Stream*>(user);
    if (!stream->cancelled || !(*stream->cancelled)()) return 0;
    stream->stopped = true;
    return 1;
}

}  // namespace

std::size_t CloneEngine::speak(std::string_view voice, std::string_view text,
                               const OnAudio& on_audio, const Cancelled& cancelled) {
    std::unique_lock<std::mutex> held(impl_->mutex);
    impl_->loaded_cv.wait(held, [&] { return impl_->loaded; });
    if (!impl_->engine) return 0;
    qwen_voice_t* clone = impl_->voice_named(voice);
    if (!clone) return 0;
    if (cancelled && cancelled()) return 0;

    const std::string line(text);
    Stream stream;
    stream.on_audio = &on_audio;
    stream.cancelled = cancelled ? &cancelled : nullptr;
    stream.started = std::chrono::steady_clock::now();
    int count = 0;
    char err[512] = {0};
    const unsigned seed = static_cast<unsigned>(env_int("QWEN_SEED", 42));
    if (qwen_voice_speak_stream(impl_->engine, clone, line.c_str(), seed, stream_audio,
                                stream_cancelled, &stream, &count, err, sizeof err) != 0) {
        std::cerr << "voice: clone " << voice << ": " << (err[0] ? err : "generation failed") << "\n";
        return 0;
    }
    const auto seconds = [](std::chrono::steady_clock::duration d) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(d).count() / 1000.0;
    };
    const auto took = std::chrono::steady_clock::now() - stream.started;
    const double rate = qwen_voice_sample_rate();
    std::cerr << "voice: " << voice << " " << (count / rate) << "s, first audio in "
              << seconds(stream.first) << "s, done in " << seconds(took) << "s"
              << (stream.stopped ? ", cut off" : "") << "\n";
    return static_cast<std::size_t>(count > 0 ? count : 0);
}

std::vector<std::string> CloneEngine::voices() const {
    std::vector<std::string> names;
    std::error_code error;
    if (!std::filesystem::is_directory(impl_->voices_dir, error)) return names;
    for (const auto& entry : std::filesystem::directory_iterator(impl_->voices_dir, error)) {
        if (entry.path().extension() == ".qvoice") names.push_back(entry.path().stem().string());
    }
    return names;
}

const std::filesystem::path& CloneEngine::model_dir() const { return impl_->model_dir; }
const std::filesystem::path& CloneEngine::voices_dir() const { return impl_->voices_dir; }

}  // namespace moonshine
