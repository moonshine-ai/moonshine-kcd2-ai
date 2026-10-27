// The microphone, for the menu's say row (mods/kcd2/README.md, "Choosing a reply"): while
// the row is live the app listens, runs what it hears through Moonshine Voice's streaming
// transcriber, and the session sends the words so far to the mod, which shows them in the
// slot. The player's A sends the line on; nothing here decides anything.
//
// Built around the same library the Kokoro voice uses (voice.cpp), and optional in the
// same way: a build without it still has the row, and the player types into it.
#ifndef MOONSHINE_LISTEN_H
#define MOONSHINE_LISTEN_H

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace moonshine {

// Whether this binary can hear: the transcriber is linked and this platform's capture
// is written (macOS and Windows).
bool listen_built();

// The model directory to load: `asked` when it names a directory, or a model name in the
// moonshine_voice download cache (tiny-streaming-en and the like, whose files sit in a
// quantized/ folder beneath the name). Empty when nothing is found. Without `asked`, the
// first of kDefaultSttModels that is present.
std::filesystem::path find_stt_model(const std::filesystem::path& asked);
inline constexpr std::string_view kDefaultSttModel = "tiny-streaming-en";

// Which Moonshine architecture a model directory holds, from its name: "tiny-streaming",
// "base-streaming", "small-streaming", "medium-streaming", "tiny" or "base", looked for in
// the directory and its parents (the files sit under quantized/). The C API's constant,
// or -1 for a name that says nothing.
int stt_arch_for(const std::filesystem::path& model_dir);

class Ear {
public:
    // Loads the transcriber; throws when it will not load. The microphone is not opened
    // until start().
    explicit Ear(std::filesystem::path model_dir);
    ~Ear();

    Ear(const Ear&) = delete;
    Ear& operator=(const Ear&) = delete;

    // Open the microphone on a fresh stream. A start while listening is nothing.
    void start();
    // Close the microphone and drop the stream, transcript and all.
    void stop();
    bool listening() const;

    // The words heard so far on this stream, every line joined with a space, as the
    // transcriber has them now. Safe from any thread. Empty when not listening.
    std::string text() const;

    // The directory the model was loaded from.
    const std::filesystem::path& model() const { return model_dir_; }

private:
    std::filesystem::path model_dir_;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace moonshine

#endif  // MOONSHINE_LISTEN_H
