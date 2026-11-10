// The slash commands typed at the app's keyboard, for somebody testing by hand.
//
// Everything else typed there is what Henry says, and goes to whoever he is talking to
// (main.cpp). A line beginning with a slash is for the app instead: the list of the quests
// the database has dealt, numbered, and a way to be put beside one's giver without a walk
// across the map or a quest title to spell. The game end of that is a hook (turnfile.h,
// TurnWriter::hook): Lua the mod's console pump runs within a second of its being written,
// which calls the mod's Teleport (emit.cpp) with the soul to go to and, for a soul the game
// has not loaded here, where their routine puts them at this hour.
//
// The numbers are the list's order, which is the database's (PersonaDb::quest_list), so a
// quest keeps its number from one run to the next as long as the database is the same.

#ifndef MOONSHINE_CONSOLE_H
#define MOONSHINE_CONSOLE_H

#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "personadb.h"

namespace moonshine {

// What a command came to: words for the keyboard, and when it asks the game to do
// something, the Lua for a hook and a short label for the status line.
struct ConsoleOutcome {
    std::string text;
    std::optional<std::string> lua;
    std::string label;
};

class Console {
public:
    // The list is read once; the database does not change under a running app.
    explicit Console(const PersonaDb& db);

    // Whether a typed line is for the console rather than for the soul being spoken to.
    static bool is_command(std::string_view line);

    // One command. Unknown ones get the help text.
    ConsoleOutcome run(std::string_view line);

    const std::vector<QuestListing>& quests() const { return quests_; }

    // The quests the game's save has off, by id, as the mod last said (gamelog.h,
    // Marker::Switches). Until it has, /quests marks none on or off and /enable and /disable
    // send their word to the game whatever the state, since the app cannot tell what would
    // change.
    void set_off(std::set<std::string> ids);
    bool knows_switches() const { return off_.has_value(); }
    bool is_off(std::string_view quest_id) const;

    // The listing's quests that belong to the same story as quest `at` (an index into
    // quests()): those it follows and those that follow it, through the links `after_id`
    // carries, either way round. Public for the tests.
    std::vector<std::size_t> story_of(std::size_t at) const;

    // The Lua for a hook that puts Henry beside `soul`, falling back to the soul's routine
    // spots when they are not loaded; what `/goto` writes. Public for the tests.
    static std::string teleport_lua(std::string_view soul, std::string_view label,
                                    const std::vector<RoutineSpot>& spots);

    // The same command asked from the game's own console (~) rather than the app's keyboard
    // (gamelog.h, Marker::ConsoleLine): the hook that carries the answer back. Each line of
    // the text is written to the game's log, which the console shows, and the command's Lua,
    // if any, is run after them with what it returns put on the HUD as well, since the
    // console will have been closed by the time a teleport has held.
    static std::string game_lua(const ConsoleOutcome& outcome);

private:
    std::string help() const;
    std::string list(std::string_view filter) const;
    ConsoleOutcome go(const std::vector<std::string>& words) const;
    ConsoleOutcome flip(const std::vector<std::string>& words, bool on);

    const PersonaDb& db_;
    std::vector<QuestListing> quests_;
    std::optional<std::set<std::string>> off_;
};

}  // namespace moonshine

#endif  // MOONSHINE_CONSOLE_H
