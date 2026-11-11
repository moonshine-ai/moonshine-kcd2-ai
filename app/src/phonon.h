// The voices, spoken in this process by Phonon (Gradium's 31M flow-matching model with the
// Mimi codec), through Moonshine Voice: the library carries a C++ port of Phonon's runtime
// on ONNX Runtime, and this drives it through the library's C streaming API
// (moonshine_tts_push_text, moonshine_tts_next_chunk). It replaced the Rust runtime
// (xn-ptts, built here by cargo) on 10 Nov 2026, and with it the one-DLL-and-one-toolchain
// cost of shipping Phonon on its own: the library the ear already needed is the voice's too.
//
// The model folder is the library's own layout, named phonon: six ONNX Runtime graphs
// (embed_prompt.ort, embed_latent.ort, backbone.ort, flow_head.ort, mimi_transformer.ort,
// mimi_seanet.ort), model.json, tokenizer.bin and voices/<name>.safetensors, one speaker
// embedding per voice, named for the voice. The library reads it as <root>/phonon/..., so
// the folder has to be called phonon and its parent is the root it is handed. About 165 MB
// (scripts/fetch_models.sh --only phonon puts it under models/phonon).
//
// A voice is fixed when a synthesizer is made, and each synthesizer holds its own copy of
// the graphs, about 350 MB resident. So the engine keeps the most recently spoken few
// (two, or MOONSHINE_PHONON_VOICES) and makes another when a line wants a voice it does not
// hold: about a tenth of a second, which a conversation between two voices never pays and a
// line made ahead for a third pays once. That is the cost of choosing the voice per line.
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

    // model_dir is the phonon folder described above; its voices are read from voices/
    // inside it. voices_dir is the clones folder the other engines read, kept for the shape
    // the Speaker constructs engines in, and not read: Phonon takes its voices from the
    // model folder alone.
    PhononEngine(std::filesystem::path model_dir, std::filesystem::path voices_dir);
    ~PhononEngine();
    PhononEngine(const PhononEngine&) = delete;
    PhononEngine& operator=(const PhononEngine&) = delete;

    // The model folder: PHONON_MODEL_DIR, else models/phonon under the working directory or
    // one of its parents, the way the other weights are found, else the folder Moonshine
    // Voice's own downloads use (moonshine_voice/download.moonshine.ai/tts/phonon under the
    // user's cache). Empty when absent.
    static std::filesystem::path find_model();

    // True when lines can be spoken; failure() says why not otherwise.
    bool ready();
    const std::string& failure() const;

    // Speak text in the named voice. A voice the model folder does not have is said once
    // on stderr and spoken in a stand-in. Empty audio, with the reason on stderr, when the
    // model did not load or generation failed. One line at a time; callers on several
    // threads are serialised.
    Audio speak(std::string_view voice, std::string_view text);

    // The same line, handed over as it is made: on_audio gets each chunk of PCM16 at
    // sample rate `rate` as the library finishes it, on the calling thread. The first is
    // about 0.6 s of speech (the library's default for the first chunk of a streamed line),
    // generated in a small fraction of that. Returning false from on_audio stops the line;
    // so does cancelled() returning true, which is asked before every chunk. Returns the
    // samples delivered, 0 when nothing was (reason on stderr) or the line was stopped
    // before its first chunk.
    using OnAudio = std::function<bool(const std::int16_t* samples, std::size_t count, int rate)>;
    using Cancelled = std::function<bool()>;
    std::size_t speak(std::string_view voice, std::string_view text, const OnAudio& on_audio,
                      const Cancelled& cancelled);

    // The voice names the model folder holds, sorted, for --voices and for diagnostics.
    std::vector<std::string> voices() const;

    const std::filesystem::path& model_dir() const;
    const std::filesystem::path& voices_dir() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace moonshine
