// The cloned voices, spoken in this process.
//
// The Qwen3-TTS engine is linked in (vendor/qwen3-tts) and its model is loaded once, on a
// background thread started by the constructor, so the app comes up at once and the first
// line waits only for whatever loading is left. After that a line costs a generate call:
// the voices are the small part of a .qvoice (an x-vector, a transcript and its codec
// frames) applied to the resident model, so switching speakers between lines is free.
//
// Only moonshine_app compiles this; the tests build voice.cpp without MOONSHINE_HAS_CLONE
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

class CloneEngine {
public:
    struct Audio {
        std::vector<std::int16_t> samples;
        int rate = 0;
    };

    // model_dir holds the CustomVoice weights; voices_dir holds <name>.qvoice files.
    // Loading starts immediately and runs in the background.
    CloneEngine(std::filesystem::path model_dir, std::filesystem::path voices_dir);
    ~CloneEngine();
    CloneEngine(const CloneEngine&) = delete;
    CloneEngine& operator=(const CloneEngine&) = delete;

    // The weights: QWEN_MODEL_DIR, else models/qwen3-tts-0.6b under the working directory
    // or one of its parents, the way the LLM weights are found. Empty when absent.
    static std::filesystem::path find_model();

    // Blocks until the model has loaded (or failed). True when lines can be spoken.
    bool ready();
    // Why not, after ready() returned false.
    const std::string& failure() const;

    // Speak text in the named voice. Empty audio, with the reason on stderr, when the
    // voice has no .qvoice, the model did not load, or generation failed. One line at a
    // time; callers on several threads are serialised.
    Audio speak(std::string_view voice, std::string_view text);

    // The same line, handed over as it is made: on_audio gets each chunk of PCM16 at
    // sample rate `rate` as soon as the decoder has it, the first about two seconds in and
    // well before the line is finished, so playback can start then. It is called from a
    // thread of the engine's, not the caller's. Returning false from on_audio stops the
    // line; so does cancelled() returning true, which is asked before every frame. Returns
    // the samples delivered, 0 when nothing was (reason on stderr) or the line was stopped
    // before its first chunk.
    using OnAudio = std::function<bool(const std::int16_t* samples, std::size_t count, int rate)>;
    using Cancelled = std::function<bool()>;
    std::size_t speak(std::string_view voice, std::string_view text, const OnAudio& on_audio,
                      const Cancelled& cancelled);

    // The .qvoice names present in voices_dir, for --voices and for diagnostics.
    std::vector<std::string> voices() const;

    const std::filesystem::path& model_dir() const;
    const std::filesystem::path& voices_dir() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace moonshine
