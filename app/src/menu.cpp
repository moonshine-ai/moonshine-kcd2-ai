#include "menu.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

#include "scripted.h"
#include "sentence.h"

namespace moonshine {

namespace {

std::string_view trim(std::string_view s) {
    auto space = [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; };
    while (!s.empty() && space(s.front())) s.remove_prefix(1);
    while (!s.empty() && space(s.back())) s.remove_suffix(1);
    return s;
}

}  // namespace

bool known_intent(std::string_view intent) {
    if (!ask_key(intent).empty() || !say_key(intent).empty() || !alms_kind(intent).empty()) return true;
    if (dictation_part(intent).first > 0) return true;
    if (std::find(std::begin(kDirectorIntents), std::end(kDirectorIntents), intent) !=
        std::end(kDirectorIntents)) {
        return true;
    }
    return std::find(std::begin(kIntents), std::end(kIntents), intent) != std::end(kIntents);
}

std::vector<MenuOption> first_round_rows() {
    return {{1, "help", std::string(kFirstRoundSeeds[0])},
            {2, "ask", std::string(kFirstRoundSeeds[1])},
            {3, "about", std::string(kFirstRoundSeeds[2])}};
}

std::string opt_directive(const MenuOption& option) {
    const std::string text = undashed(option.text);
    return "@opt " + std::to_string(option.row) + " " + option.intent + " " +
           std::to_string(text.size()) + ":" + text;
}

std::string opt_turn(const std::vector<MenuOption>& options) {
    std::string out;
    for (const MenuOption& option : options) {
        if (!out.empty()) out += " ";
        out += opt_directive(option);
    }
    return out;
}

std::string rows_directive(std::size_t count, bool closed) {
    return "@rows " + std::to_string(count) + (closed ? " closed" : "");
}

std::string whole_list_turn(const std::vector<MenuOption>& options, bool closed) {
    const std::string rows = opt_turn(options);
    // A closed list of nothing would hold the player in a menu with no way out, so the
    // word is only written over rows.
    return rows.empty() ? rows_directive(0) : rows + " " + rows_directive(options.size(), closed);
}

std::optional<Pick> read_pick(std::string_view line) {
    constexpr std::string_view kPicked = "BRIDGE picked ";
    const std::size_t at = line.find(kPicked);
    if (at == std::string_view::npos) return std::nullopt;
    std::string_view rest = trim(line.substr(at + kPicked.size()));
    Pick pick;
    char* end = nullptr;
    const std::string head(rest.substr(0, rest.find(' ')));
    pick.row = static_cast<int>(std::strtol(head.c_str(), &end, 10));
    if (end == head.c_str() || pick.row <= 0) return std::nullopt;
    rest = trim(rest.substr(head.size()));
    const std::size_t sep = rest.find(" -- ");
    if (sep == std::string_view::npos) {
        // A pick with no words after it: the intent alone.
        pick.intent = std::string(rest.substr(0, rest.find(' ')));
        return pick;
    }
    pick.intent = std::string(trim(rest.substr(0, sep)));
    pick.text = std::string(trim(rest.substr(sep + 4)));
    return pick;
}

}  // namespace moonshine
