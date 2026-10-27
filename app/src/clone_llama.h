// The cloned voices, spoken on the GPU through llama.cpp.
//
// The same .qvoice files and the same words as clone.h, on a different engine: the Qwen3-TTS
// talker as a libllama model and the code predictor and code2wav from libmtmd, all resident
// on whatever ggml backend this build has -- Metal here, CUDA on the Windows laptop -- so that a line costs
// the CPU nothing while the game and the LLM have it. The vendored C engine stays the CPU
// path; voice.cpp picks between them at startup.
//
// Only moonshine_app compiles this, and only when libmtmd was built (MOONSHINE_HAS_GPU_CLONE).
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "word_align.h"

namespace moonshine {

class LlamaClone {
public:
    // A word of the line and when the voice says it, in seconds from the start of the
    // audio. Read off the talker's attention as the line is made (clone_llama.cpp, "Where
    // the words fall"), so the resolution is the codec's frame: 80 ms.
    using Word = WordSpan;

    struct Audio {
        std::vector<std::int16_t> samples;
        int rate = 0;
        std::vector<Word> words;
    };

    // model_dir holds <name>.gguf (the talker) and mmproj-<name>.gguf beside it; voices_dir
    // holds <name>.qvoice files. Loading starts immediately and runs in the background.
    LlamaClone(std::filesystem::path model_dir, std::filesystem::path voices_dir);
    ~LlamaClone();
    LlamaClone(const LlamaClone&) = delete;
    LlamaClone& operator=(const LlamaClone&) = delete;

    // The GGUF pair: QWEN_GGUF_DIR, else models/qwen3-tts-0.6b-gguf under the working
    // directory or one of its parents. Empty when absent.
    static std::filesystem::path find_model();
    // True when this build has a GPU device to run on. Without one the vendored engine is
    // the better CPU engine, and this one should not be chosen.
    static bool gpu_present();

    bool ready();
    const std::string& failure() const;

    Audio speak(std::string_view voice, std::string_view text);

    using OnAudio = std::function<bool(const std::int16_t* samples, std::size_t count, int rate)>;
    using Cancelled = std::function<bool()>;
    // The words placed so far, whole line each time: called after each batch of audio has
    // gone out, with the words whose timing is settled (their speech is in audio already
    // delivered), and once more with every word when the line ends.
    using OnWords = std::function<void(const std::vector<Word>& words, bool final)>;
    std::size_t speak(std::string_view voice, std::string_view text, const OnAudio& on_audio,
                      const Cancelled& cancelled, const OnWords& on_words = nullptr);

    std::vector<std::string> voices() const;
    const std::filesystem::path& model_dir() const;
    const std::filesystem::path& voices_dir() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace moonshine
