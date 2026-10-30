// The microphone, for the menu's say row (README.md, "Step 3: Play"): while the player
// holds the talk button the app listens, runs what it hears through Moonshine Voice's
// streaming transcriber, and the session sends the words so far to the mod, which shows
// them in the slot. The transcriber also says when a line is finished -- a pause after the
// words -- and the mod sends the line on itself once the button is up. Nothing here
// decides anything.
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
// moonshine_voice download cache (medium-streaming-en and the like, whose files sit in a
// quantized/ folder beneath the name). Empty when nothing is found. Without `asked`,
// kDefaultSttModel.
//
// Medium Streaming since 30 Oct 2026, from Tiny: the names in these conversations --
// Kuttenberg, Trosky, a soul's own -- are what Tiny got wrong most, and Medium halves the
// error rate on exactly those (third_party/moonshine/docs/models/domain-customization.md,
// the biasing table). It is 270 MB to Tiny's 45 and slower per phrase, which the player
// does not feel: the words show as they are said, and the line goes on at a pause.
std::filesystem::path find_stt_model(const std::filesystem::path& asked);
inline constexpr std::string_view kDefaultSttModel = "medium-streaming-en";

// How hard the transcriber leans towards the words it is given as context (set_context),
// as the library's keyterm_boost. 2.0 is the library's default; 3.0 is where its own
// measurements put the trade when the listed words matter more than the ones around them
// (a third of the errors on the terms for up to half a point elsewhere), and above 4.0 it
// stops working (domain-customization.md, "Tuning the strength"). The terms here are the
// names in a conversation that has just happened, which is as close to "about to be said"
// as context gets.
inline constexpr float kContextBoost = 3.0f;
// At most this many terms are picked out of the passage: a long list is charged against
// every word that is not on it, and a conversation's worth of names is far fewer.
inline constexpr int kContextMaxTerms = 100;

// What the transcriber has of the stream so far: the words, and whether every line it
// has heard is finished -- a pause after the last word, or the stream stopped -- so a
// line can be sent on without anyone pressing anything. `complete` is false while nothing
// has been heard.
struct Heard {
    std::string text;
    bool complete = false;
};

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

    // A passage of text the next words are likely to share names with -- the lines of
    // the conversation so far -- from which the transcriber picks the unusual words and
    // leans towards them (kContextBoost) while decoding. Takes effect from the next
    // reading; safe to call between or during listens. Empty turns the bias off. Only
    // the streaming models apply it; on another the call is nothing.
    void set_context(std::string_view passage);

    // The words heard so far on this stream, every line joined with a space, as the
    // transcriber has them now, and whether the transcriber has finished every line it
    // has. Safe from any thread. Empty and not complete when not listening.
    Heard heard() const;
    std::string text() const { return heard().text; }

    // The directory the model was loaded from.
    const std::filesystem::path& model() const { return model_dir_; }

private:
    std::filesystem::path model_dir_;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace moonshine

#endif  // MOONSHINE_LISTEN_H
