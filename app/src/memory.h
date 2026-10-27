// What a soul remembers of the player, as the save carries it and as the sheet says it.
//
// The record lives in the player's saved game, kept by the mod's Lua, and this app holds
// no copy of it between conversations: it arrives through the log as a dump that precedes
// every begin line, is turned into a block of the character sheet, and goes back only as
// directives on turn files -- the player's words the game did not hear, and now and then
// a summary that folds the oldest lines into a paragraph. The design and its measured
// limits are in data/kcd2/state-design/plan.md.
//
// Two of the mod's own rules shape what is here. Any string in the save has to stay
// under 64 KB or the save will not load again, so the mod chunks text on its side and this
// side never sends a summary longer than a paragraph. And the game's Lua carries numbers as
// 32-bit floats, so a world clock is exact only below 2^24 seconds -- about 194 game days --
// which is why gaps are said in days and weeks and never to the second.

#ifndef MOONSHINE_MEMORY_H
#define MOONSHINE_MEMORY_H

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "approach.h"
#include "llm.h"
#include "quest.h"
#include "scripted.h"

namespace moonshine {

// What a soul who has met the player calls them in their memory. The game has one player
// and he is Henry of Skalitz; the sheet says nothing about him until there is something to
// remember, so a stranger still opens as one.
inline constexpr std::string_view kPlayer = "Henry";

// The kinds of entry the mod appends, as it spells them in the save.
inline constexpr std::string_view kMemoryGap = "gap";      // a conversation opened; t is the world clock
inline constexpr std::string_view kMemoryPlayer = "p";     // a line the player said
inline constexpr std::string_view kMemoryNpc = "n";        // a line the soul said back
inline constexpr std::string_view kMemorySummary = "sum";  // earlier entries, folded by the model

struct MemoryEntry {
    std::string kind;
    // World seconds, on a gap and on a summary (the clock of the first entry it replaced).
    // kUnread when the mod could not read the calendar, or on a spoken line, which has none.
    double t = kUnread;
    // The words, unescaped. On a gap it is the begin line's raw readings ("rch=0.5 ...
    // hour=18.5 rain=0"), kept so a later reading can say what the evening was like.
    std::string text;
};

// One soul's record as the dump reports it: the header fields, then the entries, then
// whether the end line arrived. Empty when no dump has been seen, which is what a save
// made by a mod too old to keep one looks like.
struct SoulRecord {
    std::string soul;
    // Conversations, counting the one that is about to open: the mod increments before it
    // dumps, so 1 is a first meeting.
    int met = 0;
    double first = kUnread;
    double last = kUnread;
    // Bumped by every compaction. A compaction directive names the revision it was built
    // from and the mod refuses one that is out of date, so two conversations' worth of
    // summaries cannot both fold the same lines.
    int rev = 0;
    // How many entries the header promised, against the number that arrived.
    int entries = 0;
    // The quest whose facts the mod reported, when this soul is party to one.
    std::string quest;
    QuestFacts facts;
    // Every quest the save has moved, by id, to its furthest stage: "accepted", "handed" or
    // "done". The `quests=` field, which a mod from before it leaves out.
    std::vector<std::pair<std::string, std::string>> quests;
    // The letters after the stage on the same field, by id: a dispute's b (the player
    // offered to carry the grievance), p (the giver gave his word), r (he would not). Only
    // the quests that have any.
    std::vector<std::pair<std::string, std::string>> flags;
    // How each quest ended, after the slash on the same field (`/persuaded`, `/lied` ...),
    // by id. Only the quests the store has a how for.
    std::vector<std::pair<std::string, std::string>> hows;
    // Game hours since each quest was taken up, after a tilde on the same field (`~2.5`), by
    // id; only the quests the store has an accept time for (a mod from 9 Oct 2026 on).
    std::vector<std::pair<std::string, double>> ages;
    std::vector<std::pair<std::string, double>> stats;
    std::vector<MemoryEntry> memory;
    // Who the player knows of, as the save has it: every soul met, by id, and everyone an
    // NPC has named in his hearing (the mod's `@heard`), by the token the app sent -- a
    // soul id, or name:<name> for somebody not in the game (scripted.h, Person). From the
    // `BRIDGE known` lines that follow the header. A save from before them has none.
    std::vector<std::string> known;
    bool complete = false;

    bool knows(std::string_view token) const {
        for (const std::string& k : known) {
            if (k == token) return true;
        }
        return false;
    }

    bool empty() const { return soul.empty(); }
};

// The pieces of one `BRIDGE memory` line: which soul, which entry of how many, or the
// end marker.
struct MemoryLine {
    std::string soul;
    int index = 0;
    int total = 0;
    bool end = false;
    MemoryEntry entry;
};

// `BRIDGE state npc=<id> met= first= last= rev= entries= [quest= accepted= handed= done=]
// [quests=<id>:<a|h|d>[bpr...][/<how>],...] [stat:<name>=<value> ...]`, as Moonshine.Dump prints it. Nothing
// for any other line, including the mod's other `BRIDGE state` chatter.
std::optional<SoulRecord> read_state_line(std::string_view line);
// The player's standing with the soul, read off the record: the meetings, every quest's
// stage, and the trust number if a `stat:trust` was dumped.
Standing standing_of(const SoulRecord& record);
// `BRIDGE memory npc=<id> <i>/<n> k=<kind> t=<clock|nil> s=<escaped text>` or
// `BRIDGE memory npc=<id> end`.
std::optional<MemoryLine> read_memory_line(std::string_view line);
// `BRIDGE known npc=<id> <token> <token> ...`: the soul whose dump this is, and the tokens.
// Nothing for any other line.
struct KnownLine {
    std::string soul;
    std::vector<std::string> tokens;
};
std::optional<KnownLine> read_known_line(std::string_view line);
// A whole dump read from text, one line at a time, for inspection without a game: the
// header, then the entries that name the same soul, complete when the end line is seen.
// Empty when there is no header in it.
SoulRecord record_from_lines(std::string_view text);
// Undoes Moonshine.Escape: `\\` to a backslash, `\n` and `\r` to the line breaks they
// stand for, and `BR\IDGE` back to the word -- which is escaped so that no remembered line
// can be mistaken for a marker.
std::string unescape_memory(std::string_view text);

// How many words of remembered lines the sheet carries, and how many a soul may hold before
// the oldest are folded into a summary. One number, deliberately: a memory that was kept but
// not shown would be a memory the soul had forgotten without anyone deciding so.
inline constexpr int kMemoryRawWords = 600;
// Summaries this long or longer are refused, whatever the model was asked for.
inline constexpr int kSummaryMostWords = 200;

// A gap between two world clocks in words: "earlier today", "yesterday", "three days ago",
// "two weeks ago". Empty for a gap that cannot be told, which includes an unread clock.
std::string ago_words(double seconds);

// The block of the character sheet that says what this soul remembers of the player: any
// summaries, then the earlier conversations as they were spoken -- the latest in full and
// the rest as far as kMemoryRawWords allows -- and when they last spoke. `name` is what the
// soul calls the player. Empty on a first meeting or an empty record.
std::string memory_block(const SoulRecord& record, std::string_view player);

// A folding the record is due, if it is: everything before the conversation that has just
// opened, when those lines run past kMemoryRawWords. `n` is the count of entries to replace
// and `messages` the request that produces the paragraph to replace them with.
struct Compaction {
    int n = 0;
    std::vector<Message> messages;
};
std::optional<Compaction> compaction_for(const SoulRecord& record,
                                         std::string_view npc,
                                         std::string_view player);
// The sampling for that request: cooler than a conversation and long enough for a paragraph.
Options compaction_options(Options options);
// Why a summary the model wrote cannot be kept, or empty when it can. Length, an empty
// answer and a transcript dressed as a paragraph are the failures that have been seen.
std::string summary_trouble(std::string_view summary);

// The directives, as the mod's Directives() reads them off the front of a turn file. Both
// carry a byte count before the text, because the text is free prose and the mod has to
// know where it stops and the spoken words start.
std::string compact_directive(std::string_view soul, int rev, int n, std::string_view summary);
std::string said_directive(std::string_view words);

// One line with single spaces and nothing at either end, the same way the turn writer
// flattens words -- so a byte count taken here still holds after the file is written.
std::string flat(std::string_view words);
int words_in(std::string_view text);

}  // namespace moonshine

#endif  // MOONSHINE_MEMORY_H
