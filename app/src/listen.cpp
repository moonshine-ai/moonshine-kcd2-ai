#include "listen.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#ifdef MOONSHINE_HAS_STT
#include "moonshine-cpp.h"
#if defined(__APPLE__)
#include <AudioToolbox/AudioQueue.h>
#define MOONSHINE_CAN_LISTEN 1
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>
#define MOONSHINE_CAN_LISTEN 1
#endif
#endif

namespace moonshine {

namespace {

namespace fs = std::filesystem;

// The sample rate the transcriber wants; the capture is asked for it directly and the
// platform converts from whatever the microphone runs at.
constexpr int kRate = 16000;
// How much audio each capture buffer holds, and how much goes by between readings of the
// transcript: the mod shows a new reading every 400 ms at most (Session::kHeardEvery), so
// reading more often than that is work nobody sees.
constexpr int kChunkFrames = kRate / 10;
constexpr double kReadEvery = 0.4;

bool has_model_files(const fs::path& dir) {
    std::error_code error;
    if (!fs::is_directory(dir, error)) return false;
    // A streaming model has its tokenizer and a frontend (whole or split); the older
    // encoder/decoder pair has its tokenizer and encoder_model.ort.
    if (!fs::exists(dir / "tokenizer.bin", error)) return false;
    return fs::exists(dir / "frontend.ort", error) || fs::exists(dir / "frontend.model.ort", error) ||
           fs::exists(dir / "encoder_model.ort", error);
}

// The directory itself when it holds the files, else its quantized/ folder.
fs::path model_dir_within(const fs::path& dir) {
    if (has_model_files(dir)) return dir;
    if (has_model_files(dir / "quantized")) return dir / "quantized";
    return {};
}

std::vector<fs::path> model_caches() {
    std::vector<fs::path> caches;
    if (const char* env = std::getenv("MOONSHINE_STT_ROOT")) caches.emplace_back(env);
    if (const char* home = std::getenv("HOME")) {
        const fs::path base(home);
        caches.push_back(base / "Library/Caches/moonshine_voice/download.moonshine.ai/model");
        caches.push_back(base / ".cache/moonshine_voice/download.moonshine.ai/model");
    }
#ifdef _WIN32
    if (const char* local = std::getenv("LOCALAPPDATA")) {
        caches.push_back(fs::path(local) / "moonshine_voice" / "download.moonshine.ai" / "model");
        // Where the library's own downloads land on Windows (the Python package's cache on
        // the Windows laptop reads moonshine_voice\moonshine_voice\Cache\download.moonshine.ai\...).
        caches.push_back(fs::path(local) / "moonshine_voice" / "moonshine_voice" / "Cache" /
                         "download.moonshine.ai" / "model");
    }
    if (const char* profile = std::getenv("USERPROFILE")) {
        caches.push_back(fs::path(profile) / ".cache" / "moonshine_voice" / "download.moonshine.ai" /
                         "model");
    }
#endif
    return caches;
}

std::string lower(std::string text) {
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

}  // namespace

fs::path find_stt_model(const fs::path& asked) {
    if (!asked.empty()) {
        // A path first; a bare name is looked for in the caches.
        if (const fs::path within = model_dir_within(asked); !within.empty()) return within;
        if (asked.has_parent_path() && asked.parent_path() != fs::path(".")) return {};
    }
    const std::string name = asked.empty() ? std::string(kDefaultSttModel) : asked.string();
    for (const fs::path& cache : model_caches()) {
        if (const fs::path within = model_dir_within(cache / name); !within.empty()) {
            std::error_code error;
            const fs::path canonical = fs::weakly_canonical(within, error);
            return error ? within : canonical;
        }
    }
    return {};
}

int stt_arch_for(const fs::path& model_dir) {
    // Up to three levels: quantized/, the model's name, and whatever holds the names.
    fs::path dir = model_dir;
    for (int i = 0; i < 3 && !dir.empty(); ++i) {
        const std::string name = lower(dir.filename().string());
        if (name.find("streaming") != std::string::npos) {
            if (name.rfind("tiny", 0) == 0) return 2;    // MOONSHINE_MODEL_ARCH_TINY_STREAMING
            if (name.rfind("base", 0) == 0) return 3;    // BASE_STREAMING
            if (name.rfind("small", 0) == 0) return 4;   // SMALL_STREAMING
            if (name.rfind("medium", 0) == 0) return 5;  // MEDIUM_STREAMING
        } else {
            if (name.rfind("tiny", 0) == 0) return 0;  // TINY
            if (name.rfind("base", 0) == 0) return 1;  // BASE
        }
        dir = dir.parent_path();
    }
    return -1;
}

#ifdef MOONSHINE_CAN_LISTEN

static_assert(static_cast<int>(ModelArch::TINY) == 0 && static_cast<int>(ModelArch::BASE) == 1 &&
                  static_cast<int>(ModelArch::TINY_STREAMING) == 2 &&
                  static_cast<int>(ModelArch::BASE_STREAMING) == 3 &&
                  static_cast<int>(ModelArch::SMALL_STREAMING) == 4 &&
                  static_cast<int>(ModelArch::MEDIUM_STREAMING) == 5,
              "stt_arch_for spells the C API's constants");

bool listen_built() { return true; }

struct Ear::Impl {
    Transcriber transcriber;
    // The stream of the current listen; none between listens.
    std::unique_ptr<Stream> stream;

    // Audio from the capture to the transcribing thread, in capture-buffer chunks.
    std::mutex audio_lock;
    std::condition_variable audio_ready;
    std::deque<std::vector<float>> chunks;
    std::thread worker;
    std::atomic<bool> running{false};

    // The transcript as last read, for text().
    mutable std::mutex text_lock;
    std::string text;

#if defined(__APPLE__)
    AudioQueueRef queue = nullptr;
#elif defined(_WIN32)
    HWAVEIN wave = nullptr;
    HANDLE ready = nullptr;
    std::vector<WAVEHDR> headers;
    std::vector<std::vector<int16_t>> buffers;
    std::thread capture;
#endif

    explicit Impl(const fs::path& dir, ModelArch arch)
        // No automatic update: the worker reads the transcript on its own clock.
        : transcriber(dir.string(), arch, 1.0e9) {}

    void push(std::vector<float>&& chunk) {
        {
            std::lock_guard<std::mutex> held(audio_lock);
            chunks.push_back(std::move(chunk));
        }
        audio_ready.notify_one();
    }

    void set_text(std::string words) {
        std::lock_guard<std::mutex> held(text_lock);
        text = std::move(words);
    }

    // Feeds the stream and reads it every kReadEvery seconds of audio.
    void work() {
        double fed = 0.0;
        double read_at = 0.0;
        while (true) {
            std::vector<float> chunk;
            {
                std::unique_lock<std::mutex> held(audio_lock);
                audio_ready.wait(held, [&] { return !running.load() || !chunks.empty(); });
                if (!running.load() && chunks.empty()) return;
                chunk = std::move(chunks.front());
                chunks.pop_front();
            }
            if (!running.load()) return;
            try {
                stream->addAudio(chunk, kRate);
                fed += static_cast<double>(chunk.size()) / kRate;
                if (fed - read_at >= kReadEvery) {
                    read_at = fed;
                    const Transcript transcript = stream->updateTranscription(0);
                    std::string words;
                    for (const TranscriptLine& line : transcript.lines) {
                        std::string_view piece = line.text;
                        while (!piece.empty() && std::isspace(static_cast<unsigned char>(piece.front()))) {
                            piece.remove_prefix(1);
                        }
                        while (!piece.empty() && std::isspace(static_cast<unsigned char>(piece.back()))) {
                            piece.remove_suffix(1);
                        }
                        if (piece.empty()) continue;
                        if (!words.empty()) words += ' ';
                        words += piece;
                    }
                    set_text(std::move(words));
                }
            } catch (const std::exception&) {
                // A stream that fails stays quiet; the player can still type.
            }
        }
    }

#if defined(__APPLE__)
    static void captured(void* user, AudioQueueRef queue, AudioQueueBufferRef buffer,
                         const AudioTimeStamp*, UInt32 frames, const AudioStreamPacketDescription*) {
        Impl* self = static_cast<Impl*>(user);
        if (!self->running.load()) return;
        if (frames > 0) {
            const float* samples = static_cast<const float*>(buffer->mAudioData);
            self->push(std::vector<float>(samples, samples + frames));
        }
        AudioQueueEnqueueBuffer(queue, buffer, 0, nullptr);
    }

    void open_capture() {
        AudioStreamBasicDescription format{};
        format.mSampleRate = kRate;
        format.mFormatID = kAudioFormatLinearPCM;
        format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
        format.mChannelsPerFrame = 1;
        format.mBitsPerChannel = 32;
        format.mBytesPerFrame = 4;
        format.mFramesPerPacket = 1;
        format.mBytesPerPacket = 4;
        if (AudioQueueNewInput(&format, captured, this, nullptr, nullptr, 0, &queue) != noErr) {
            queue = nullptr;
            throw std::runtime_error("the microphone would not open (AudioQueueNewInput)");
        }
        for (int i = 0; i < 3; ++i) {
            AudioQueueBufferRef buffer = nullptr;
            if (AudioQueueAllocateBuffer(queue, kChunkFrames * 4, &buffer) != noErr ||
                AudioQueueEnqueueBuffer(queue, buffer, 0, nullptr) != noErr) {
                close_capture();
                throw std::runtime_error("the microphone's buffers would not allocate");
            }
        }
        if (AudioQueueStart(queue, nullptr) != noErr) {
            close_capture();
            throw std::runtime_error("the microphone would not start (AudioQueueStart)");
        }
    }

    void close_capture() {
        if (!queue) return;
        AudioQueueStop(queue, true);
        AudioQueueDispose(queue, true);
        queue = nullptr;
    }
#elif defined(_WIN32)
    // Waits for buffers to fill and hands them on. waveIn's own callback may not call back
    // into waveIn, so the buffers are re-added from here instead.
    void capture_loop() {
        while (running.load()) {
            WaitForSingleObject(ready, 50);
            for (std::size_t i = 0; i < headers.size(); ++i) {
                WAVEHDR& header = headers[i];
                if (!(header.dwFlags & WHDR_DONE)) continue;
                const std::size_t frames = header.dwBytesRecorded / sizeof(int16_t);
                if (frames > 0 && running.load()) {
                    std::vector<float> chunk(frames);
                    for (std::size_t n = 0; n < frames; ++n) chunk[n] = buffers[i][n] / 32768.0f;
                    push(std::move(chunk));
                }
                if (!running.load()) break;
                header.dwFlags &= ~WHDR_DONE;
                header.dwBytesRecorded = 0;
                waveInAddBuffer(wave, &header, sizeof(WAVEHDR));
            }
        }
    }

    void open_capture() {
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = 1;
        format.nSamplesPerSec = kRate;
        format.wBitsPerSample = 16;
        format.nBlockAlign = 2;
        format.nAvgBytesPerSec = kRate * 2;
        ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (waveInOpen(&wave, WAVE_MAPPER, &format, reinterpret_cast<DWORD_PTR>(ready), 0,
                       CALLBACK_EVENT) != MMSYSERR_NOERROR) {
            wave = nullptr;
            close_capture();
            throw std::runtime_error("the microphone would not open (waveInOpen)");
        }
        headers.assign(4, WAVEHDR{});
        buffers.assign(4, std::vector<int16_t>(kChunkFrames));
        for (std::size_t i = 0; i < headers.size(); ++i) {
            headers[i].lpData = reinterpret_cast<LPSTR>(buffers[i].data());
            headers[i].dwBufferLength = static_cast<DWORD>(buffers[i].size() * sizeof(int16_t));
            if (waveInPrepareHeader(wave, &headers[i], sizeof(WAVEHDR)) != MMSYSERR_NOERROR ||
                waveInAddBuffer(wave, &headers[i], sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
                close_capture();
                throw std::runtime_error("the microphone's buffers would not prepare");
            }
        }
        if (waveInStart(wave) != MMSYSERR_NOERROR) {
            close_capture();
            throw std::runtime_error("the microphone would not start (waveInStart)");
        }
        capture = std::thread([this] { capture_loop(); });
    }

    void close_capture() {
        if (capture.joinable()) capture.join();
        if (wave) {
            waveInStop(wave);
            waveInReset(wave);
            for (WAVEHDR& header : headers) {
                if (header.dwFlags & WHDR_PREPARED) waveInUnprepareHeader(wave, &header, sizeof(WAVEHDR));
            }
            waveInClose(wave);
            wave = nullptr;
        }
        headers.clear();
        buffers.clear();
        if (ready) {
            CloseHandle(ready);
            ready = nullptr;
        }
    }
#endif
};

Ear::Ear(fs::path model_dir) : model_dir_(std::move(model_dir)) {
    const int arch = stt_arch_for(model_dir_);
    if (arch < 0) {
        throw std::runtime_error("cannot tell which Moonshine model " + model_dir_.string() +
                                 " is from its name (tiny-streaming-en and the like)");
    }
    impl_ = std::make_unique<Impl>(model_dir_, static_cast<ModelArch>(arch));
}

Ear::~Ear() {
    try {
        stop();
    } catch (...) {
    }
}

void Ear::start() {
    if (impl_->running.load()) return;
    impl_->set_text({});
    {
        std::lock_guard<std::mutex> held(impl_->audio_lock);
        impl_->chunks.clear();
    }
    impl_->stream = std::make_unique<Stream>(impl_->transcriber.createStream(1.0e9, 0));
    impl_->stream->start();
    impl_->running.store(true);
    impl_->worker = std::thread([this] { impl_->work(); });
    try {
        impl_->open_capture();
    } catch (...) {
        stop();
        throw;
    }
}

void Ear::stop() {
    if (!impl_->running.load() && !impl_->worker.joinable()) return;
    impl_->running.store(false);
    impl_->close_capture();
    impl_->audio_ready.notify_all();
    if (impl_->worker.joinable()) impl_->worker.join();
    {
        std::lock_guard<std::mutex> held(impl_->audio_lock);
        impl_->chunks.clear();
    }
    if (impl_->stream) {
        try {
            impl_->stream->close();
        } catch (...) {
        }
        impl_->stream.reset();
    }
    impl_->set_text({});
}

bool Ear::listening() const { return impl_->running.load(); }

std::string Ear::text() const {
    std::lock_guard<std::mutex> held(impl_->text_lock);
    return impl_->text;
}

#else  // !MOONSHINE_CAN_LISTEN

bool listen_built() { return false; }

struct Ear::Impl {};

Ear::Ear(fs::path model_dir) : model_dir_(std::move(model_dir)) {
    throw std::runtime_error("this build cannot hear: no Moonshine Voice library, or no capture"
                             " for this platform");
}
Ear::~Ear() = default;
void Ear::start() {}
void Ear::stop() {}
bool Ear::listening() const { return false; }
std::string Ear::text() const { return {}; }

#endif

}  // namespace moonshine
