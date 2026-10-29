// Speaking a reply out loud, through Moonshine Voice's Kokoro.
//
// The C++ binding synthesizes. It does not open a playback device -- that is
// deliberate in the library, so this is where the samples become sound. say()
// returns at once and the line plays on its own thread, because the call arrives
// from the turn that is still generating the rest of the reply. hush() drops
// whatever has not been said yet, which is a player talking over them or walking
// away.

#ifndef MOONSHINE_VOICE_H
#define MOONSHINE_VOICE_H

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace moonshine {

class Facials;

// The mouth that goes with a line (lipsync.h): whose it is for the mod (henry, npc), the
// actor whose shipped facial clips to borrow, and the line's key, which names the cue
// files. An empty `who` is a line with no mouth.
struct LipTag {
    std::string who;
    std::string face;
    std::uint32_t key = 0;
    bool wanted() const { return !who.empty(); }
};

// Where the mouth cues go: a file written beside the turn files, and the previous line's
// files taken away. Both are called from the voice's own thread.
struct LipSink {
    std::function<void(const std::string& file, const std::string& body)> write;
    std::function<void(const std::vector<std::string>& files)> forget;
};

// The Newcastle man from the imported set. Spoken when a soul has no clone, or the
// clone cannot be reached. --voice picks another fallback.
inline constexpr std::string_view kDefaultVoice = "bm_p286";

struct ImportedVoice {
    std::string id;
    std::string about;
};

// Whether this binary can speak. True when Kokoro is linked, and also when the
// cloned voices can be played on this machine without it.
bool tts_built();

// Whether the souls' voice engine is in this build, and where its model is as the
// Speaker would find it (models/phonon-31m-en beside the working directory, or
// PHONON_MODEL_DIR; the Qwen3-TTS engine's folder in a build that still carries that
// engine), or empty. For the start-up check, which says so before the game.
bool clones_built();
std::filesystem::path find_clone_model();
// What the engine is called, for that same check: "Phonon" or "Qwen3-TTS".
std::string_view clone_engine_name();

// The Moonshine asset tree (kokoro/ and en_gb/ under it), or empty if none of
// the usual places has one. `asked` wins when it is non-empty.
std::filesystem::path find_tts_root(const std::filesystem::path& asked);

// Voices imported into `dir` (voices.txt, then any .kokorovoice beside it).
std::vector<ImportedVoice> imported_voices(const std::filesystem::path& dir);

// A Kokoro id with a leading "kokoro_" removed, so a file and a flag spell it once.
std::string plain_voice_id(std::string_view asked);

// voice.xml voice_id to the abbreviation of the clone, or empty when there is none.
std::string actor_abbr(int voice_id);

// The voices are Phonon's British English catalogue (gradium/phonon-voices, Apache 2.0),
// twelve of them, each a speaker embedding under models/phonon-31m-en/voices/ and named
// after its file. The game's own actors are not imitated: their voices are theirs.
//
// Henry is not a soul in the database, so his lines name kHenryVoice themselves; it is
// his alone, so that the player's own voice is never also the man he is talking to.
// Spencer, since 28 Oct 2026: heard on a carter in Kuttenberg and wanted for Henry, so
// the two were swapped, and teddy took spencer's places among the souls.
inline constexpr std::string_view kHenryVoice = "spencer";
// The voice a line is spoken in when it is nobody's in particular: a woman's when asked
// for one (the mod's female intent, a soul the database only knows the gender of), a man's
// otherwise, including for a soul with no gender.
std::string stand_in_voice(std::string_view gender);
// A soul's own voice, picked once from the catalogue by their gender and age (the game's
// voice.xml age label: young, adult, middle-aged, elderly, or nothing) and spread across
// the voices of that age by a hash of `soul`, so neighbours differ and the same soul is the
// same voice every time. Falls back to the stand-in when the gender says nothing.
std::string voice_for(std::string_view gender, std::string_view age, std::string_view soul);

// Which clone speaks a line the mod has put on screen. `who` is henry, male, female,
// or npc -- npc is whoever the conversation is open with, and `actor` is their stand-in.
std::string clone_for(std::string_view who, std::string_view actor);

class Speaker {
public:
    // `voice` is a Kokoro id. An imported one is read from voices_dir; any other
    // id is a stock voice in the asset tree. Throws if the voice will not load.
    Speaker(std::string voice, std::filesystem::path voices_dir,
            std::filesystem::path tts_root);
    ~Speaker();

    Speaker(const Speaker&) = delete;
    Speaker& operator=(const Speaker&) = delete;

    // `actor` is a voice-bank abbreviation (actors.txt). When that clone can be
    // reached, the line is spoken in it; otherwise the Kokoro voice is used. Said before
    // anything queued by prepare() or warm(), and a line being made ahead for other words
    // is set aside for it and made after.
    void say(std::string_view text, std::string_view actor = {}, LipTag lips = {});
    // The mouth cues: as a line's audio starts, a plan of the game's own facial clips to
    // play (Facials::plan), and when it ends or is cut, a stop. Nothing is written for a
    // line said without a LipTag, or when facials.txt was not found beside the voices.
    void set_lips(LipSink sink);
    // The clips loaded from facials.txt, for the start-up report.
    std::size_t facial_clips() const;
    // Make the line now and keep it, so that a say() of the same words plays at once.
    // The other party's greeting is known at the look, long before the mod has shown it
    // and asked for it to be spoken; made then, the first word follows the caption instead
    // of trailing it by the whole synthesis. Nothing is played. The words are matched with
    // their whitespace flattened, as the turn file flattens them; a line made for another
    // actor, or never made, is spoken live as before. hush() leaves these alone: dropping
    // a line not yet asked for would only mean making it again.
    void prepare(std::string_view text, std::string_view actor = {});
    // Drop every line waiting to be made ahead, and cut the one being made unless it is
    // already being handed to a say(). The look has moved to somebody else: the greetings
    // of the souls passed on the way are not going to be asked for, and each was a few
    // seconds of the GPU beside the game (14 Oct 2026: six souls looked at in twelve
    // seconds kept the voice making lines the whole time, and the driver reset). What is
    // already made and kept stays, since it costs nothing to hold.
    void forget_ahead();
    // Say a short line to nobody, so the first line of the session does not pay for the
    // engines' first use: the clone's tokenizer loads on its first line, which on the Windows laptop was
    // the difference between first audio in 4.1s and in 2.2s (11 Oct 2026). Queued like a
    // line, so the model stands aside for it as it would for one.
    void warm();
    void hush();
    // Block until everything queued has been spoken, or hush() has dropped it.
    void wait();
    // True while a line is being made or played, or waits its turn -- a line made ahead
    // (prepare) and the warm-up count too. The game loop holds off the model's guessing
    // ahead while it is: the clone engine and llama.cpp share the cores with the game, and
    // a line that gaps is worse than a guess that is late.
    bool speaking() const;
    // The lines say() is done with since the last call, in order: heard to the end, or
    // hushed part way, or never spoken for want of an engine. This is how the app learns
    // when the audio of a line actually finished, as against the reading time the mod
    // guessed from its length -- Henry's line, which the camera stays on until then, and
    // the closing line, which the conversation stays open for (session.h, Session::spoken).
    // A line dropped is reported too, so nothing waits on it.
    std::vector<std::string> take_spoken();
    // Synthesize `text` and write a 16-bit mono wav. Does not play it.
    void write(std::string_view text, const std::filesystem::path& wav,
               std::string_view actor = {});
    // The same samples, without a file. The phrase baker (phrase_cache.h) keeps them.
    struct RenderedLine {
        std::vector<std::int16_t> samples;
        int rate = 0;
    };
    RenderedLine render(std::string_view text, std::string_view actor = {});
    // Lines already spoken, under `dir` (phrase_cache.h). A say() or prepare() of
    // words that are there plays the file and does not ask the model. An empty
    // path, or a directory that is not there, leaves every line live.
    void set_phrase_cache(const std::filesystem::path& dir);
    std::size_t cached_lines() const;

    const std::string& voice() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace moonshine

#endif
