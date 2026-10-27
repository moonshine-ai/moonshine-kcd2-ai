#include "console.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace moonshine {

namespace {

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::vector<std::string> split(std::string_view line) {
    std::vector<std::string> words;
    std::istringstream in{std::string(line)};
    std::string word;
    while (in >> word) words.push_back(word);
    return words;
}

// A Lua string literal: the names are plain, but a soul's or a title's quote would
// otherwise end the string early.
std::string lua_quoted(std::string_view s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '\\' || c == '"') out += '\\';
        if (c == '\n') { out += "\\n"; continue; }
        out += c;
    }
    out += '"';
    return out;
}

std::string party(std::string_view name, std::string_view trade) {
    std::string out(name);
    if (!trade.empty()) out += " (" + std::string(trade) + ")";
    return out;
}

}  // namespace

Console::Console(const PersonaDb& db) : db_(db), quests_(db.quest_list()) {}

bool Console::is_command(std::string_view line) {
    while (!line.empty() && std::isspace(static_cast<unsigned char>(line.front()))) line.remove_prefix(1);
    return !line.empty() && line.front() == '/';
}

std::string Console::help() const {
    return "commands:\n"
           "  /quests [word]        the quests dealt, numbered; with a word, only the rows that mention it\n"
           "  /goto N               put Henry beside the giver of quest N (the game must be in -devmode)\n"
           "  /goto N target        ... beside the other party instead: the holder, the one to be seen\n"
           "  /help                 this\n"
           "  /quit                 stop the app (here only)\n"
           "the first three work at the game's own console (~) too, once a save is loaded;\n"
           "anything else typed here is what Henry says to whoever he is talking to.";
}

std::string Console::list(std::string_view filter) const {
    if (quests_.empty()) return "the database has no quests dealt.";
    const std::string want = lower(filter);
    std::string out;
    std::string settlement_shown;
    std::string story_settlement;
    int shown = 0;
    for (std::size_t i = 0; i < quests_.size(); ++i) {
        const QuestListing& q = quests_[i];
        char head[32];
        std::snprintf(head, sizeof head, "[%2d] ", static_cast<int>(i + 1));
        std::string row = std::string(head);
        row += std::string(static_cast<std::size_t>(q.depth) * 2, ' ');
        row += q.title.empty() ? ("(untitled " + q.kind + ")") : q.title;
        row += " -- " + q.kind;
        if (q.kind == "deliver" && q.item_name.empty()) row += " (word)";
        else if (!q.item_name.empty()) row += " (" + q.item_name + ")";
        row += "; ";
        if (q.depth > 0) {
            // The scene before, by its number, so a story reads as a story.
            for (std::size_t j = 0; j < quests_.size(); ++j) {
                if (quests_[j].id != q.after_id) continue;
                row += "after " + std::to_string(j + 1) + "; ";
                break;
            }
        }
        row += party(q.giver_name, q.giver_trade);
        if (!q.target_soul.empty() && !q.target_name.empty()) row += " -> " + party(q.target_name, q.target_trade);
        row += "  " + q.id;
        // The settlement header belongs to the story's first scene; a later scene's giver
        // may live elsewhere, and is listed under the story all the same.
        if (q.depth == 0) story_settlement = q.giver_settlement.empty() ? "(no settlement)" : q.giver_settlement;
        if (!want.empty() && lower(row).find(want) == std::string::npos &&
            lower(story_settlement).find(want) == std::string::npos) {
            continue;
        }
        if (story_settlement != settlement_shown) {
            if (!out.empty()) out += "\n";
            out += story_settlement + "\n";
            settlement_shown = story_settlement;
        }
        out += "  " + row + "\n";
        ++shown;
    }
    if (shown == 0) return "no quest mentions \"" + std::string(filter) + "\".";
    out += std::to_string(shown) + " of " + std::to_string(quests_.size()) +
           " quests. /goto N puts Henry beside N's giver; /goto N target, beside the other party.";
    return out;
}

std::string Console::teleport_lua(std::string_view soul, std::string_view label,
                                  const std::vector<RoutineSpot>& spots) {
    std::string lua = "return Moonshine.Teleport(" + lua_quoted(soul) + ", " + lua_quoted(label) + ", {";
    for (std::size_t i = 0; i < spots.size(); ++i) {
        char one[128];
        std::snprintf(one, sizeof one, "%s{ %d, %d, %.2f, %.2f, %.2f }", i == 0 ? " " : ", ",
                      spots[i].start_min, spots[i].end_min, spots[i].x, spots[i].y, spots[i].z);
        lua += one;
    }
    lua += spots.empty() ? "})" : " })";
    return lua;
}

std::string Console::game_lua(const ConsoleOutcome& outcome) {
    // One Log per line: the console shows the log a line at a time, and a newline inside
    // one entry would be shown as the two characters.
    std::string lua;
    std::string_view rest = outcome.text;
    while (!rest.empty()) {
        const std::size_t nl = rest.find('\n');
        const std::string_view line = rest.substr(0, nl);
        if (!line.empty()) lua += "Moonshine.Log(" + lua_quoted(line) + ")\n";
        if (nl == std::string_view::npos) break;
        rest.remove_prefix(nl + 1);
    }
    if (!outcome.lua) {
        lua += "return " + lua_quoted(outcome.label.empty() ? "answered" : outcome.label) + "\n";
        return lua;
    }
    // The command's own chunk `return`s its line; run it as a function so that line can be
    // put on the HUD as well as into the log.
    lua += "local said = (function()\n" + *outcome.lua + "\nend)()\n";
    lua += "pcall(function() Game.SendInfoText(tostring(said), true, nil, 5) end)\n";
    lua += "return said\n";
    return lua;
}

ConsoleOutcome Console::go(const std::vector<std::string>& words) const {
    if (words.size() < 2) return {"which one? /goto N, with N off /quests.", std::nullopt, {}};
    char* end = nullptr;
    const long n = std::strtol(words[1].c_str(), &end, 10);
    if (*end != '\0' || n < 1 || n > static_cast<long>(quests_.size())) {
        return {"\"" + words[1] + "\" is not a quest number; /quests lists them, 1 to " +
                    std::to_string(quests_.size()) + ".",
                std::nullopt, {}};
    }
    const QuestListing& q = quests_[static_cast<std::size_t>(n - 1)];
    bool target = false;
    if (words.size() > 2) {
        const std::string which = lower(words[2]);
        if (which == "target" || which == "holder" || which == "t") target = true;
        else if (which != "giver" && which != "g") {
            return {"\"" + words[2] + "\"? /goto N goes to the giver, /goto N target to the other party.",
                    std::nullopt, {}};
        }
    }
    if (target && q.target_soul.empty()) {
        return {q.title + " has no other party: a " + q.kind + " is between Henry and " + q.giver_name + " alone.",
                std::nullopt, {}};
    }
    const std::string soul = target ? q.target_soul : q.giver_soul;
    const std::string name = target ? q.target_name : q.giver_name;
    const std::string trade = target ? q.target_trade : q.giver_trade;
    const std::vector<RoutineSpot> spots = db_.spots_of(soul);
    ConsoleOutcome out;
    out.lua = teleport_lua(soul, name, spots);
    out.label = "to " + party(name, trade) + ", " + (target ? "the other party of " : "giver of ") +
                std::to_string(n) + " " + q.title + " (" + soul + ")";
    out.text = "going " + out.label;
    if (spots.empty()) out.text += "; no routine spots on file, so only if the game has them loaded";
    else out.text += "; " + std::to_string(spots.size()) + " routine spots to fall back on";
    return out;
}

ConsoleOutcome Console::run(std::string_view line) {
    const std::vector<std::string> words = split(line);
    if (words.empty()) return {help(), std::nullopt, {}};
    const std::string verb = lower(words[0]);
    if (verb == "/quests" || verb == "/q" || verb == "/list") {
        std::string filter;
        for (std::size_t i = 1; i < words.size(); ++i) filter += (i > 1 ? " " : "") + words[i];
        return {list(filter), std::nullopt, {}};
    }
    if (verb == "/goto" || verb == "/g" || verb == "/go") return go(words);
    if (verb == "/help" || verb == "/?") return {help(), std::nullopt, {}};
    return {"\"" + words[0] + "\" is not a command.\n" + help(), std::nullopt, {}};
}

}  // namespace moonshine
