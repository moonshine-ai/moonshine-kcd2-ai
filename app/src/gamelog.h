// Listening to the game, which can only talk by printing.
//
// The mod's Lua has no way to write a file and no socket, so everything coming
// out of a conversation arrives as lines in kcd.log. This follows that file and
// turns the markers the mod prints into events. Going the other way is turnfile.h.
//
// Two details are load-bearing and were learned the hard way. "Played" is not
// "spoke": a turn the mod asked for is not a turn the player heard, and answering
// requests rather than playbacks once spent a whole conversation's worth of lines
// in two minutes with three in four never said. And the game rewrites its log from
// the beginning on every launch, so a file that has grown shorter than the read
// position is a new run and not a truncation to recover from.

#ifndef MOONSHINE_GAMELOG_H
#define MOONSHINE_GAMELOG_H

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "approach.h"
#include "memory.h"

namespace moonshine {

enum class Marker {
    None,
    Begin,   // a conversation opened
    Played,  // a turn was said to the player
    Left,    // the conversation ended
    // The player typed a line in the game and pressed Enter. The mod's typing keyboard
    // is made of actionmaps and has no way to hand text over except by printing it, so
    // it arrives here like everything else, and is answered exactly as a line typed at
    // the app's own stdin would be.
    Said,
    // The game relaunched. Not something the mod prints -- the log getting shorter
    // is what says it -- but delivered in the same stream and in order, so that
    // resetting the turn count cannot be done to the wrong side of the new run's
    // first conversation.
    Restarted,
    // The header of a soul's record from the save, which the mod dumps before every begin
    // line so that the app has it by the time the conversation opens. Followed by one
    // Memory event per remembered entry and one with `end` set.
    State,
    Memory,
    // A `BRIDGE known` line of the same dump: who the player knows of (memory.h,
    // SoulRecord::known), in `known`. Between the header and the entries.
    Known,
    // The mod's store was reset or loaded, so any record held from before belongs to a
    // world that is gone. A save loaded, a new game, a return to the main menu.
    WorldChanged,
    // The player chose a row of the reply menu. The words follow at once as a Said line,
    // which is what gets answered; this one carries the row's intent, so the director
    // knows what the words meant without asking.
    Picked,
    // The player is looking at a soul from close enough for the game to show their name and
    // the talk prompt, and has not pressed it. The same fields as Begin, read at that
    // moment, and the same record dumped ahead of it as before a begin -- as it would be
    // dumped if the player pressed now -- so the app can have the opening line ready
    // before they do (Session::look). Most looks come to nothing; the mod sends one per
    // soul per look, not one per frame.
    Look,
    // The game is shutting down: the engine's own "CSystem::Quit invoked" line, the last
    // thing it prints. Not the mod's, but the one sign the app has that the run is over
    // while the file is still there to read. A canned app stops on it (main.cpp): one
    // started for a pass used to outlive the pass and answer the player's next game with
    // "This is canned turn ..." (notes.md, 5 Oct 2026).
    Quit,
    // The mod ran (or could not parse, or raised from) hook_N.txt: "BRIDGE hook N ran -- words".
    // `turn` carries N, `said` the words after it ("ran -- ...", "raised ...", "did not parse:
    // ..."). The app's console writes hooks (Session::hook) and counts from this.
    Hook,
    // A line the mod's console code wants shown to whoever typed: "BRIDGE console ..." --
    // the chain armed, a teleport that held or did not. `said` is the words after the marker.
    Console,
    // A command typed at the game's own console (~): "BRIDGE console line /quests bees".
    // `said` is the command as the app's keyboard would have it, slash and all; the app runs
    // it and sends the answer back as a hook that writes it into the game's log, which the
    // console shows (main.cpp, Console::game_lua).
    ConsoleLine,
    // A line is on the screen and should be spoken. `intent` is who: henry, male,
    // female, or npc (whoever the conversation is open with). `said` is the words.
    Voice,
    // The menu's "(Say something else)" row went live: the player may type into it on the
    // mod's keyboard or speak, and the app listens on its microphone between this and the
    // stop, sending the transcript so far back as hooks (Moonshine.Heard). SayStop's `said`
    // is the mod's reason: "sent" when the line went on as a Said line, else "back",
    // "moved ...", "conversation over".
    SayStart,
    SayStop,
};

struct Event {
    Marker marker = Marker::None;
    int turn = 0;
    // Which soul the player is talking to: an id like "kkut_additive_man_47", not a
    // name. It is what the game calls the soul internally, and the only identity Lua
    // can obtain -- there is no soul:GetName() -- so it is also what every table in
    // npcs.sqlite is keyed on. Reading it as a name is how a varlet in Kuttenberg
    // came to be handed a beggar's character sheet.
    //
    // Empty on a save made before the mod logged it, which has to fall back to a
    // stranger rather than refuse to talk.
    std::string soul;

    // The Mannequin animation the soul was last seen playing before being talked to --
    // "LumberJackSawVAR", "Bartender_TakeAndTapStein". Read while the player was only
    // looking at them, because by the time a conversation opens the saw has been put
    // down. Resolved to words through anim_activity in npcs.sqlite.
    //
    // "none" when the mod watched and saw nothing worth keeping, empty from a mod too old
    // to report it at all. Both end up with no clause, so the app need not tell them
    // apart, but the log does.
    std::string doing;

    // The state the mod read at the moment the player walked up, unjudged: how well dressed
    // the player is, whether they are bloody, hurt, drunk or armed, and whether this soul is
    // asleep or already talking to somebody. Which of those is worth saying something about,
    // and how it reads in the prompt, is decided in approach.cpp -- deliberately not in the
    // emitted Lua, where the same bands used to live in a second copy.
    Approach approach;

    // The player's words, on a Said marker only: everything after the marker, trimmed. On a
    // Picked marker, the row's words, with the row's intent alongside.
    std::string said;
    std::string intent;

    // On a State marker, the header of the record: who, how often, the quest's facts. The
    // entries follow as Memory markers, one each in `memory`, ending with one whose `end`
    // is set. On a WorldChanged marker, `said` carries the mod's own words for what happened.
    SoulRecord state;
    MemoryLine memory;
    KnownLine known;
};

// One log line. Anything that is not one of the mod's markers is not ours.
Event classify(std::string_view line);

// The highest turn the game reports having played in the log as it stands, up to `limit`
// bytes read from the end. For an app that opens after the game has been talking: the
// tailer starts at the end of the file, so this is the only way to learn where the mod's
// count has got to (Session::resume). 0 for no log, or none played.
int last_played_turn(const std::filesystem::path& log, std::uintmax_t limit = 64 * 1024 * 1024);

// Both counts the mod keeps across a run, read the same way: the highest turn played and
// the highest hook run (or tried). last_played_turn is this one's `played`.
struct RunCounts {
    int played = 0;
    int hooked = 0;
};
RunCounts run_counts(const std::filesystem::path& log, std::uintmax_t limit = 64 * 1024 * 1024);

class LogTailer {
public:
    // Opens at the end of the file, because a log from an earlier run would replay
    // turns that have already been spent.
    explicit LogTailer(std::filesystem::path log);

    bool ok() const { return static_cast<bool>(in_); }
    const std::filesystem::path& path() const { return path_; }

    // Whatever has been printed since the last call, oldest first. Never blocks.
    std::vector<Event> poll();
    // Whether the next poll() would have anything: a look at the file without taking the
    // events, for work that should stop the moment the game says something (the replies
    // guessed ahead, session.h). Cheap when nothing has been written.
    bool pending();

    std::uintmax_t position();

private:
    // Everything printed since the last read, classified; poll() and pending() share it.
    std::vector<Event> read();
    void reopen();
    // The game writes its log from the beginning on every start, so a file shorter
    // than the position being read from is a new run, not a truncation to recover
    // from. Checked before reading, or the new run's opening lines are read from a
    // stale offset and lost.
    bool rewound();

    std::filesystem::path path_;
    std::ifstream in_;
    // Events pending() has read and poll() has not yet handed out.
    std::vector<Event> buffered_;
    // The unterminated last line as of the previous read, where it starts, and when it was
    // first seen like that: the engine ends a line only when it begins the next, so one
    // that has sat unchanged this long is taken as whole (read()).
    static constexpr std::chrono::milliseconds kTailSettle{300};
    std::string tail_;
    std::uintmax_t tail_at_ = 0;
    std::chrono::steady_clock::time_point tail_since_{};
};

}  // namespace moonshine

#endif  // MOONSHINE_GAMELOG_H
