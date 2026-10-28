// The voices, spoken in this process by Phonon (Gradium's 31M flow-matching model with the
// Mimi codec), through the C interface of its Rust runtime, vendor/xn-ptts/ptts-ffi.
//
// The model folder is laid out as the Hugging Face repo is (config.json, tokenizer.json,
// model.q8.gguf, default-voice.safetensors) with the voices under voices/<name>.safetensors,
// each a speaker embedding named after its file. The whole thing is 68 MB and loads in a
// tenth of a second, so it is loaded in the constructor; a line is then one ptts_speak, which
// hands the audio over 80 ms at a time at about a tenth of real time on the Windows laptop
// (benchmarks, 28 Oct 2026), the first chunk under 100 ms in.
//
// It replaced the Qwen3-TTS engine (clone.h) on 28 Oct 2026: 3.6 GB resident and real time
// or slower against 300 MB and a tenth of it. That engine's code stays in the tree, off.
//
// Only moonshine_app compiles this; the tests build voice.cpp without MOONSHINE_HAS_PHONON
// and never see it.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace moonshine {

class PhononEngine {
public:
    struct Audio {
        std::vector<std::int16_t> samples;
        int rate = 0;
    };

    // model_dir is the checkpoint folder; its voices are read from voices/ inside it.
    // voices_dir is the clones folder the other engines read, kept for the shape the
    // Speaker constructs engines in, and not read: Phonon's runtime takes its voices
    // from the checkpoint alone.
    PhononEngine(std::filesystem::path model_dir, std::filesystem::path voices_dir);
    ~PhononEngine();
    PhononEngine(const PhononEngine&) = delete;
    PhononEngine& operator=(const PhononEngine&) = delete;

    // The checkpoint: PHONON_MODEL_DIR, else models/phonon-31m-en under the working
    // directory or one of its parents, the way the other weights are found. Empty when
    // absent.
    static std::filesystem::path find_model();

    // True when lines can be spoken; failure() says why not otherwise.
    bool ready();
    const std::string& failure() const;

    // Speak text in the named voice. A voice the checkpoint does not have is said once
    // on stderr and spoken in the default voice. Empty audio, with the reason on stderr,
    // when the model did not load or generation failed. One line at a time; callers on
    // several threads are serialised.
    Audio speak(std::string_view voice, std::string_view text);

    // The same line, handed over as it is made: on_audio gets each chunk of PCM16 at
    // sample rate `rate` as the decoder finishes it, on the calling thread, and the
    // first about a tenth of a second in. Returning false from on_audio stops the line;
    // so does cancelled() returning true, which is asked before every chunk. Returns the
    // samples delivered, 0 when nothing was (reason on stderr) or the line was stopped
    // before its first chunk.
    using OnAudio = std::function<bool(const std::int16_t* samples, std::size_t count, int rate)>;
    using Cancelled = std::function<bool()>;
    std::size_t speak(std::string_view voice, std::string_view text, const OnAudio& on_audio,
                      const Cancelled& cancelled);

    // The voice names the checkpoint holds, sorted, for --voices and for diagnostics.
    std::vector<std::string> voices() const;

    const std::filesystem::path& model_dir() const;
    const std::filesystem::path& voices_dir() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace moonshine
