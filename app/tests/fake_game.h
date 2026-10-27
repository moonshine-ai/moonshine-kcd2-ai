// A stand-in for the game, for the half of the bridge that cannot be tested
// against the real thing on a laptop without Windows.
//
// It does the two things the game does that the bridge has to survive: it appends
// to its log while the log is being read, and it starts that log again from the
// beginning when it relaunches. It also reads turn files the way the mod does,
// which is what makes "did the words arrive" an assertion rather than a belief.

#ifndef MOONSHINE_TESTS_FAKE_GAME_H
#define MOONSHINE_TESTS_FAKE_GAME_H

#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "turnfile.h"

namespace moonshine {

// Somewhere no turn can be written, on either platform.
//
// A made-up absolute path does not do it: "/nowhere/game" is unwritable on POSIX
// but resolves under C:\ on Windows, where it is created quite happily, and two
// tests passed on macOS while failing on the machine that ships. A regular file
// standing where a parent directory should be fails everywhere, because a file is
// not a directory anywhere.
class BlockedPaths {
public:
    BlockedPaths() {
        std::random_device seed;
        file_ = std::filesystem::temp_directory_path() /
                ("moonshine-blocked-" + std::to_string(seed()));
        std::ofstream out(file_);
        out << "not a directory\n";

        paths_.root = file_ / "game";
        paths_.user = file_ / "user";
    }

    ~BlockedPaths() {
        std::error_code ec;
        std::filesystem::remove(file_, ec);
    }

    BlockedPaths(const BlockedPaths&) = delete;
    BlockedPaths& operator=(const BlockedPaths&) = delete;

    const GamePaths& paths() const { return paths_; }

private:
    std::filesystem::path file_;
    GamePaths paths_;
};

class FakeGame {
public:
    FakeGame() {
        std::random_device seed;
        dir_ = std::filesystem::temp_directory_path() /
               ("moonshine-fake-" + std::to_string(seed()));
        paths_.root = dir_ / "game";
        paths_.user = dir_ / "user";
        std::filesystem::create_directories(paths_.root);
        std::filesystem::create_directories(paths_.user);
        launch();
    }

    ~FakeGame() {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    FakeGame(const FakeGame&) = delete;
    FakeGame& operator=(const FakeGame&) = delete;

    const GamePaths& paths() const { return paths_; }
    std::filesystem::path log() const { return log_path(paths_); }

    // Truncates the log, as the engine does on every start.
    void launch() {
        log_.close();
        log_.clear();
        log_.open(log(), std::ios::binary | std::ios::trunc);
        turn_ = 0;
    }

    void say(const std::string& line) {
        log_ << line << "\n";
        log_.flush();
    }

    void begin(const std::string& npc) { say("<12:04:31> BRIDGE begin npc=" + npc); }
    // The same with the mod's fields after the name (" alms=mf", say).
    void begin_with(const std::string& npc, const std::string& fields) {
        say("<12:04:31> BRIDGE begin npc=" + npc + " doing=none" + fields);
    }
    // The player has the soul in their sights from talking distance and has not pressed:
    // what the mod logs once per look, after dumping the record as a begin would.
    void look(const std::string& npc) { say("<12:04:29> BRIDGE look npc=" + npc); }
    void left(const std::string& why = "player walked away") {
        say("<12:05:02> BRIDGE left, " + why + ", 3.4m away");
    }
    // The player typed a line on the mod's in-game keyboard and pressed Enter. With the
    // mod's own prefix, as the real line carries it.
    void typed(const std::string& words) {
        say("<12:04:40> [moonshine_names] BRIDGE said " + words);
    }

    // The save's record of a soul, dumped as the mod does just before it logs the begin:
    // the header, then each entry, then the end line. `entries` are (kind, clock, text)
    // triples; a clock below zero is written as nil, and the text is escaped as the mod
    // escapes it. `met` counts the conversation about to open.
    struct Entry {
        std::string kind;
        double t;
        std::string text;
    };
    // `known` are the tokens of who the player knows of (souls met, people named), on a
    // `BRIDGE known` line after the header as the mod writes it; none, no line.
    void dump(const std::string& npc, int met, const std::vector<Entry>& entries,
              const std::string& facts = {}, int rev = 0, const std::vector<std::string>& known = {}) {
        auto whole = [](double t) {
            return t < 0 ? std::string("nil") : std::to_string(static_cast<long long>(t));
        };
        say("<12:04:30> BRIDGE state npc=" + npc + " met=" + std::to_string(met) +
            " first=" + whole(entries.empty() ? -1 : entries.front().t) +
            " last=" + whole(entries.empty() ? -1 : entries.back().t) + " rev=" +
            std::to_string(rev) + " entries=" + std::to_string(entries.size()) +
            (facts.empty() ? "" : " " + facts));
        if (!known.empty()) {
            std::string line = "<12:04:30> BRIDGE known npc=" + npc;
            for (const std::string& token : known) line += " " + token;
            say(line);
        }
        for (std::size_t i = 0; i < entries.size(); ++i) {
            std::string escaped;
            for (const char c : entries[i].text) {
                if (c == '\\') escaped += "\\\\";
                else if (c == '\n') escaped += "\\n";
                else if (c == '\r') escaped += "\\r";
                else escaped += c;
            }
            for (std::size_t at = escaped.find("BRIDGE"); at != std::string::npos;
                 at = escaped.find("BRIDGE", at + 3)) {
                escaped.insert(at + 2, "\\");
            }
            say("<12:04:30> BRIDGE memory npc=" + npc + " " + std::to_string(i + 1) + "/" +
                std::to_string(entries.size()) + " k=" + entries[i].kind + " t=" +
                whole(entries[i].t) + " s=" + escaped);
        }
        say("<12:04:30> BRIDGE memory npc=" + npc + " end");
    }

    // Plays the next turn if its file is there, the way the mod's pump does: it
    // walks forward only while the next turn is readable.
    std::optional<std::string> play() {
        const int next = turn_ + 1;
        for (const std::filesystem::path& path : turn_paths(paths_, next)) {
            std::ifstream in(path, std::ios::binary);
            if (!in) continue;
            std::string words;
            std::getline(in, words);
            turn_ = next;
            words = strip_directives(words);
            say("<12:04:35> BRIDGE played turn=" + std::to_string(next) + " -- " + words);
            return words;
        }
        return std::nullopt;
    }

    // The whole of a turn file, as the mod's Fetch reads it: a quest directive rides on the
    // first line and the words follow, so a test about the directive needs both.
    std::string whole(int turn) const {
        for (const std::filesystem::path& path : turn_paths(paths_, turn)) {
            std::ifstream in(path, std::ios::binary);
            if (!in) continue;
            return std::string(std::istreambuf_iterator<char>(in), {});
        }
        return {};
    }

    // Leaves a turn file behind as a crashed run would, to check it gets swept.
    void strand(int turn, const std::string& words) {
        TurnWriter(paths_).write(turn, words);
    }

    // Every directive the mod would have taken off the front of a played turn, in order and
    // whole, so a test can say what the save was told without reading turn files itself.
    const std::vector<std::string>& directives() const { return directives_; }

    // The player's words the save was told about through @said, unwrapped.
    std::vector<std::string> remembered() const {
        std::vector<std::string> out;
        for (const std::string& directive : directives_) {
            if (directive.rfind("@said ", 0) != 0) continue;
            out.push_back(directive.substr(directive.find(':') + 1));
        }
        return out;
    }

    bool has_turn(int turn) const {
        for (const std::filesystem::path& path : turn_paths(paths_, turn)) {
            if (std::filesystem::exists(path)) return true;
        }
        return false;
    }

    // The Lua of a hook the app has left for the mod to run, or empty when there is none.
    std::string hook(int hook) const {
        for (const std::filesystem::path& path : hook_paths(paths_, hook)) {
            std::ifstream in(path, std::ios::binary);
            if (!in) continue;
            return std::string(std::istreambuf_iterator<char>(in), {});
        }
        return {};
    }

private:
    // The mod's Directives(), as far as a test needs it: each directive is taken off the
    // front by its own grammar -- three tokens for a quest command, a byte count and a colon
    // before free text -- and the words are what is left. Kept in directives_ for asserting.
    std::string strip_directives(std::string words) {
        auto token = [&]() {
            const std::size_t end = words.find(' ');
            const std::string out = words.substr(0, end);
            words.erase(0, end == std::string::npos ? words.size() : end);
            words.erase(0, words.find_first_not_of(' '));
            return out;
        };
        auto counted = [&]() {
            const std::size_t colon = words.find(':');
            const std::size_t length = static_cast<std::size_t>(std::stoul(words.substr(0, colon)));
            const std::string text = words.substr(colon + 1, length);
            words.erase(0, colon + 1 + length);
            words.erase(0, words.find_first_not_of(' '));
            return text;
        };
        while (!words.empty() && words[0] == '@') {
            const std::size_t end = words.find(' ');
            const std::string head = words.substr(0, end);
            if (head == "@more") {
                token();
            } else if (head == "@quest") {
                // One token per statement: the operands of + are unsequenced, and MSVC took
                // them right to left, giving "@quest complete <id>".
                std::string whole = token();
                whole += " " + token();
                whole += " " + token();
                directives_.push_back(whole);
            } else if (head == "@said") {
                token();
                const std::string text = counted();
                directives_.push_back("@said " + std::to_string(text.size()) + ":" + text);
            } else if (head == "@memory") {
                std::string whole = token();
                const std::string soul = token();
                const std::string verb = token();
                whole += " " + soul + " " + verb;
                if (verb == "compact") {
                    whole += " " + token();
                    whole += " " + token();
                    const std::string text = counted();
                    whole += " " + std::to_string(text.size()) + ":" + text;
                } else {
                    whole += " " + token();
                    whole += " " + token();
                }
                directives_.push_back(whole);
            } else if (head == "@opt") {
                std::string whole = token();
                whole += " " + token();
                whole += " " + token();
                const std::string text = counted();
                whole += " " + std::to_string(text.size()) + ":" + text;
                directives_.push_back(whole);
            } else if (head == "@rows" || head == "@alms" || head == "@heard") {
                std::string whole = token();
                whole += " " + token();
                // "@rows <n> closed": the list is these rows alone (menu.h).
                if (head == "@rows" && words.rfind("closed", 0) == 0
                    && (words.size() == 6 || words[6] == ' ')) {
                    whole += " " + token();
                }
                directives_.push_back(whole);
            } else if (head == "@check") {
                // @check <skill> <pass|fail>: two words after the head.
                std::string whole = token();
                whole += " " + token();
                whole += " " + token();
                directives_.push_back(whole);
            } else if (head == "@end" || head == "@beat" || head == "@voiced") {
                directives_.push_back(token());
            } else {
                break;
            }
        }
        return words;
    }

public:
    // The rows of the reply menu as the mod would have them after the turns played so far:
    // the last @opt turn carries the whole list, so only the last one counts.
    std::vector<std::string> menu() const {
        std::vector<std::string> rows;
        for (const std::string& directive : directives_) {
            // A count of none is a whole list with nothing on it.
            if (directive == "@rows 0") rows.clear();
            if (directive.rfind("@opt ", 0) != 0) continue;
            if (directive.rfind("@opt 1 ", 0) == 0) rows.clear();
            // "@opt <row> <intent> <len>:<text>", where the intent may carry a colon of its
            // own ("ask:his_trade"), so the count's is the one after the third space.
            std::size_t at = 0;
            for (int spaces = 0; spaces < 3 && at != std::string::npos; ++spaces) {
                at = directive.find(' ', at + 1);
            }
            rows.push_back(directive.substr(directive.find(':', at) + 1));
        }
        return rows;
    }

    // The player chose a row: the mod logs the pick, then the words as a said line.
    void picked(int row, const std::string& intent, const std::string& words) {
        say("<12:04:41> [moonshine_names] BRIDGE picked " + std::to_string(row) + " " + intent +
            " -- " + words);
        typed(words);
    }

private:

    std::filesystem::path dir_;
    GamePaths paths_;
    std::ofstream log_;
    int turn_ = 0;
    std::vector<std::string> directives_;
};

}  // namespace moonshine

#endif  // MOONSHINE_TESTS_FAKE_GAME_H
