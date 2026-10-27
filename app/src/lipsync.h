// A mouth for the voice: which of the game's own facial clips to play while a line is spoken.
//
// Every line KCD2 ships has a baked facial animation named like its audio, and the mod can
// start any of them on a character (lipsync_probe.lua, 12 Oct 2026: a 10.5 s line's clip
// played on layer 13 opened the mouth through the words, speeds above and below 1 took, and
// a clip left to end holds its last frame until the layer is stopped). The app has no clip of
// its own words, so it borrows: a shipped clip of about the line's length, in the face of the
// actor whose clone is speaking, played at the speed that makes it fit. The visemes are
// another line's, which is what a non-English dub of the game looks like too
// (wh_dlg_ScaleLipsync does the same stretch); the mouth opens and closes at the pace of
// speech, and stops when the audio does.
//
// The clips are data/kcd2/voices/facials.txt (scripts/build_facial_index.py): each with its
// length and its mouth, a digit per 100 ms of how far open it is. The clips are the actors'
// recordings, silence included, and most end in about a second of closed mouth (29% of
// their length over the index; 12-13 Oct 2026, measured in the game and then read from the
// files with scripts/kcd2_facial.py), so a clip is fitted to a line by the stretch of it
// that speaks, and cut there, with the next clip following -- otherwise the player sees the
// lips close while the voice goes on, which is what they saw. The cue the app writes for the
// mod is a small text file beside the turn files, named by a hash of the line, so neither
// side has to count (lip_cue, lip_key; emit.cpp's LipPoll reads it).
//
// The clips are also cut at the pauses of the speech itself (13 Oct 2026). A silence of
// kPauseSeconds or more in the audio (Silences: 20 ms windows under a floor set a little
// below the line's loudness) is a pause: the plan puts a rest there -- the mouth's layer
// stopped, so the lips close with the voice -- and starts a fresh clip as the sound comes
// back, so no borrowed clip talks through a silence or closes mid-phrase. The silences are
// read off the sound and not the word timings the voice model gives (word_align.h), since
// those run word to word with no gap, a pause falling inside the word before it. A line all
// in hand at the start is planned whole; a streamed one is planned to its guessed length as
// the sound starts and revised from each pause as the audio arrives (the engine runs ahead
// of the speaker), each revision a cue replacing the plan from that second on (LipPlan::from).
//
// The mouth is let go softly (14 Oct 2026). Stopping the layer puts the engine's own face
// back in one frame, and at a rest or the end of a line that read as a snap (notes.md). So
// each plan names a shut clip -- a shipped clip whose mouth never opens, a hum or a breath,
// the actor's own where they have one -- and the mod blends into that at a rest and at the
// end instead of stopping, the layer stopped only once the lips have settled shut.

#ifndef MOONSHINE_LIPSYNC_H
#define MOONSHINE_LIPSYNC_H

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "visemes.h"

namespace moonshine {

// Henry's actor code in the shipped line ids (Tom McKay), for his own face.
inline constexpr std::string_view kHenryFace = "tmck";

struct LipClip {
    std::string clip;  // the shipped clip's stem, the name the animation set knows it by; empty for a rest
    double speed = 1.0;  // playback speed that fits it to the audio
    double seconds = 0.0;  // of audio it covers at that speed: the speaking stretch, the silent tail cut
    // A rest: the mouth shut (the layer stopped) for its seconds, where the speech pauses.
    bool rest() const { return clip.empty(); }
};

struct LipPlan {
    std::vector<LipClip> clips;  // clips and rests, one after another from `from`
    std::string shut;  // the face's shut clip, blended to where the layer would otherwise stop; empty when the index has none
    double from = 0.0;  // the second of the line the plan starts at: 0 for the opening cue, later for a revision replacing the plan from there on
    double seconds() const;
    bool empty() const { return clips.empty(); }
    std::size_t rests() const;
};

// A stretch of a line's audio with nothing said, in seconds from its start.
struct Pause {
    double start = 0.0;
    double end = 0.0;
};

// The silences of a line's audio, found as it is made. Fed the samples in order, it keeps
// the loudness of every 20 ms window; a run of windows under the floor lasting kPauseSeconds
// or more is a pause once sound has followed it (or the audio has ended), and until then the
// run is only "silent since". The floor is kDropDb below the loudest window so far, within
// [kFloorMinDb, kFloorMaxDb]: a cloned voice is clean, its silences far below any speech.
class Silences {
public:
    explicit Silences(int sample_rate);
    void push(const std::int16_t* samples, std::size_t count);
    void finish();  // the audio is all here; a silence it ends in is a pause to the end
    // Where the audio stands: pauses closed so far, the seconds heard, whether it is over,
    // and the second a silence still open began (or a negative number when it ends in sound).
    struct Heard {
        std::vector<Pause> pauses;
        double seconds = 0.0;
        bool over = false;
        double silent_since = -1.0;
    };
    Heard heard() const;

    static constexpr double kWindowSeconds = 0.02;
    static constexpr double kPauseSeconds = 0.25;  // a silence this long is a pause of the speech
    static constexpr double kDropDb = 15.0;
    static constexpr double kFloorMinDb = -45.0;
    static constexpr double kFloorMaxDb = -36.0;

private:
    std::size_t window_;  // samples
    std::vector<float> db_;  // loudness per whole window
    double sum_squares_ = 0.0;  // the window being filled
    std::size_t filled_ = 0;
    float peak_db_ = -120.0f;
    bool over_ = false;
};

class Facials {
public:
    // Reads facials.txt: "actor stem seconds mouth" a line, # for comments, the mouth a
    // digit per 100 ms (0 shut to 9 wide) and optional -- without it the whole clip is taken
    // to speak. False when the file is not there or holds nothing; then plan() returns
    // nothing and the mouth stays as it was (the mod's own jaw beats).
    bool load(const std::filesystem::path& file);
    std::size_t size() const { return clips_.size(); }
    // How many clips are in this actor's own face.
    std::size_t count(std::string_view actor) const;
    // The clip the face rests in: one whose mouth never opens, the actor's own if they have
    // one and anybody's otherwise, the one nearest kShutSeconds long. Empty when the index
    // has none, and then the mod stops the layer as it used to.
    std::string shut(std::string_view actor) const;

    // Clips covering `seconds` of speech, one after another, each at the speed that fits
    // its speaking stretch to its share and each cut where its mouth goes quiet: the actor's
    // own lines when they have enough, anybody's otherwise. Among the clips of a near
    // length the ones whose mouth keeps moving are preferred, and `seed` picks among those,
    // so the same words do not always wear the same face. No two consecutive clips are the
    // same, since the engine refuses a clip re-issued on a layer where it is still playing.
    // Empty when nothing is loaded or `seconds` is too short.
    LipPlan plan(std::string_view actor, double seconds, std::uint32_t seed) const;

    // The same, cut at the pauses of the speech: `heard` is what the audio has shown so far
    // (Silences). Each pause is a rest, with a clip after it starting kRestLead early so its
    // blend is over as the sound comes back; the speech between pauses is clips. Past the
    // seconds heard, the speech is taken to go on to `seconds` without a pause (a silence
    // still open at the end of what was heard counts as speech until it closes); when the
    // audio is over, what follows the last sound is a rest. `from` is the second of the
    // line the plan starts at, for a revision: what lies before it is left out, and the
    // piece it falls in is shortened to it. Empty when nothing is loaded, the stretch from
    // `from` to `seconds` is too short, or there is nothing but rest in it.
    LipPlan plan(std::string_view actor, const Silences::Heard& heard, double from, double seconds,
                 std::uint32_t seed) const;

    // Speeds outside this are slow motion or a gabble; a clip is chosen so it fits inside.
    static constexpr double kMinSpeed = 0.6;
    static constexpr double kMaxSpeed = 1.6;
    // A clip may be stretched or squeezed by this much before another is looked for.
    static constexpr double kPieceSeconds = 7.0;  // a long line is cut into pieces about this long
    static constexpr std::size_t kOwnFaceMinimum = 50;  // fewer than this and anybody's face is used
    static constexpr double kRestLead = 0.1;  // the clip after a rest starts this early (the mod blends it in over about this)
    static constexpr double kShortest = 0.3;  // no plan, or no piece of one, for less speech than this
    static constexpr double kShutSeconds = 1.0;  // the shut clip nearest this long is the one taken

private:
    struct Clip {
        std::string actor;
        std::string stem;
        double seconds;  // the file's length
        double talk;  // where the mouth last moves: the stretch of it that speaks
        double lead;  // how long the mouth is shut at the start
        double still;  // the share of the speaking stretch with the mouth shut
    };
    static constexpr double kMouthStep = 0.1;  // seconds a digit of the mouth stands for
    std::vector<Clip> clips_;  // sorted by actor, then talk
    std::vector<Clip> shut_;  // the clips whose mouth never opens, sorted the same way
    // The actor's run in clips_ as [first, last); the whole table for an empty actor.
    struct Range {
        std::size_t first = 0, last = 0;
    };
    Range range(std::string_view actor) const;
    const Clip* pick(Range pool, double target, std::string_view not_this, std::uint32_t& rng) const;
    // Clips covering `seconds` of unbroken speech appended to `plan`, each cut where its
    // speaking stretch ends or the stretch does, none the same as `last` (the clip before,
    // updated to the clip added last). The piece is covered to the second: a clip too long
    // even at the slowest speed is cut at its end.
    void fill(Range pool, double seconds, std::uint32_t& rng, std::string& last, LipPlan& plan) const;
};

// The line's name on both sides: h = h * 31 + byte over "<who> <words>", the words with their
// whitespace runs made one space and the ends trimmed, mod kLipKeyModulus. The mod computes
// the same in Lua (emit.cpp, LipKey), which is why it is this and not a library hash: Lua 5.1
// has no bit operations. The modulus is small because the game's Lua keeps its numbers in
// 32-bit floats (12 Oct 2026, lipsync_pass.lua: the mod's key for the first greeting came
// out 1465712640 against the app's 842313093, which is exactly what rounding the mod-2^32
// sum to 24 bits of mantissa at every step gives); with h below 2^19 the step's product
// stays under 2^24 and the two sides agree to the digit.
constexpr std::uint32_t kLipKeyModulus = 524287;  // 2^19 - 1, prime; 524286 * 31 + 255 < 2^24
std::uint32_t lip_key(std::string_view who, std::string_view text);

// The cue files, as the mod names them: lip_<key>_<seq>.txt, seq counting from 1 within a
// line; the mod reads them in order, so each is written only once the one before is. Every
// body opens with a "lip <who> <key>" header. The first, as the sound starts, is the plan,
// "play <clip> <speed> <seconds>" a line and "rest <seconds>" for a pause, under a "shut
// <clip>" line naming the face's shut clip when the index has one; a plan revised
// from a second of the line on is the same under a "from <seconds>" line, and the mod
// takes it up at that second. With the mouth shapes on instead (MOONSHINE_MOUTH_SHAPES),
// as the words' seconds become known (visemes.h, from the talker's attention) the shapes
// follow as "mouth <at> <viseme>" lines, each at its second from the start of the audio,
// and the mod drops the plan for them. The last cue says "stop" when the audio has ended
// or been cut.
std::string lip_file(std::uint32_t key, int seq);
std::string lip_cue(std::string_view who, std::uint32_t key, const LipPlan& plan);
std::string lip_mouth(std::string_view who, std::uint32_t key, const std::vector<Mouth>& mouth);
std::string lip_stop(std::string_view who, std::uint32_t key);

}  // namespace moonshine

#endif
