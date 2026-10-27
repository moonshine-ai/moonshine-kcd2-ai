// Answering the game, which can only listen by reading a file.
//
// Three things about this shape were forced by the engine rather than chosen.
//
// A file per turn, not one file rewritten: the game reads through its own archive
// layer and caches what it finds by path, so a name it has already read is not a
// name to put different words in.
//
// The file holds the words and nothing else. No wrapper, no id. A turn used to be
// a localization row, and being a row is what made it finite -- an id keeps the
// first wording it is given for the rest of the run, so every line spent one of a
// fixed number of authored slots.
//
// Written to every path the mod is willing to look under. All five were measured
// readable from inside the game, so this is belt and braces rather than a search.

#ifndef MOONSHINE_TURNFILE_H
#define MOONSHINE_TURNFILE_H

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace moonshine {

// How far to sweep. Not a limit on the conversation, which has none: the mod says
// its lines through Game.SendInfoText and spends nothing per line. This only has
// to cover how many turn files an earlier run could have left behind -- and a reply
// streamed to the screen spends a turn on every update, twenty or so a line, so an
// evening's talking runs into the thousands.
constexpr int kSweepTurns = 4096;

// The heartbeat's bucket, in seconds: alive_<unix time / this>.txt is written beside the
// turn files while the app runs (TurnWriter::beat), and the mod looks for the current
// bucket's and its neighbours' when a conversation opens, telling the player the app is
// not running when none reads (emit.cpp, kAliveBucketSeconds and AppAlive). A name per
// bucket rather than one file rewritten, because the engine caches a path once read.
// Wide, because the engine's clock is a single-precision float that steps by 128 seconds.
constexpr int kAliveBucketSeconds = 300;

struct GamePaths {
    // Where kcd.log lives, beside the executable.
    std::filesystem::path root;
    // Whatever the engine means by %USER%: the Saved Games folder holding the
    // profile and the saves. It is the one place the engine is known to both read
    // and write outside a pak.
    std::filesystem::path user;
    std::string modid = "moonshine_names";
};

// Where the game is, found rather than configured. Returns an empty root when it
// cannot be found, which the caller should report instead of guessing.
GamePaths discover();

std::filesystem::path log_path(const GamePaths& paths);

// Every candidate for one turn, in the order the mod tries them.
std::vector<std::filesystem::path> turn_paths(const GamePaths& paths, int turn);
// The same folders for the heartbeat of one bucket of the clock.
std::vector<std::filesystem::path> alive_paths(const GamePaths& paths, long long bucket);
// And for one hook: hook_N.txt, Lua the mod runs (emit.cpp, Hook and Console).
std::vector<std::filesystem::path> hook_paths(const GamePaths& paths, int hook);

class TurnWriter {
public:
    explicit TurnWriter(GamePaths paths);

    // Writes the words for one turn. True when at least one path took it, since
    // the mod only needs to find it once.
    bool write(int turn, std::string_view words);

    // A turn file left behind by an earlier run is worse than a missing one: the
    // mod walks forward while the next turn is readable, so one stale file lets it
    // race through turns it can already see and outrun whatever is being said.
    //
    // Swept lowest first, because after a relaunch the mod is looking for turn 1 and
    // that is the file it could read from the last run.
    void sweep(int upto = kSweepTurns);

    // One turn, once the game has said it. Nothing needs it afterwards -- the engine
    // has it cached by then -- and a turn file that does not outlive the words is a
    // turn file that cannot be read by a later run.
    void forget(int turn);

    // The heartbeat: alive_<bucket>.txt for the bucket `now_seconds` (Unix time) falls in,
    // and the buckets behind it taken away. Called every few seconds while the app runs;
    // cheap when the bucket has not changed (the write is skipped).
    void beat(long long now_seconds);
    // The heartbeat taken away, for an app stopping on purpose: the current bucket's file
    // and its neighbours', so the mod does not read a stopped app as up for the rest of
    // the bucket.
    void hush(long long now_seconds);

    // A hook: Lua for the mod to run, numbered across the run of the game like a turn and
    // for the same reason. The console's teleports go this way (console.h). True when at
    // least one path took it. Taken away once the mod reports it ran (forget_hook), and
    // all of them at a sweep.
    bool hook(int hook, std::string_view lua);
    void forget_hook(int hook);

    // A mouth cue for the line being spoken (lipsync.h): the file is named by the line and a
    // sequence within it, so no counter is shared with the mod. Written from the voice's
    // own thread while the game loop may be writing a turn, which is safe: the paths are
    // fixed and each write is its own file. Taken away by the next line's first cue
    // (forget_lips) and at a sweep.
    bool lip(std::string_view file, std::string_view body);
    // The cue files of an earlier line, by their names.
    void forget_lips(const std::vector<std::string>& files);

    const GamePaths& paths() const { return paths_; }

private:
    GamePaths paths_;
    long long beaten_ = -1;
};

}  // namespace moonshine

#endif  // MOONSHINE_TURNFILE_H
