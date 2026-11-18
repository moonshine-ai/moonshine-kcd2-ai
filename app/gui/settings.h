// What the player has chosen, kept in moonshine.cfg beside the app.
//
// The file is the one the installer has always written (`root = ...` when the game was
// found somewhere unusual) and the console launcher has always read: one `key = value` a
// line, # for a comment. The window reads and writes the same file, so a setting made by
// hand, by the installer or here is the same setting, and a key this version does not
// know -- or a comment somebody wrote -- survives a save.
//
// One thing is deliberately NOT in it: the key to a model API. Collect-Logs.ps1 copies
// this file into the zip a player sends when something is wrong. The key lives in the
// system's credential store (secrets.h).

#ifndef MOONSHINE_GUI_SETTINGS_H
#define MOONSHINE_GUI_SETTINGS_H

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace moonshine::gui {

struct Settings {
    // The game's folder, when it is not under a Steam library the app finds for itself.
    std::string root;
    // Anything else for the app's command line (`args = --no-voice`): the console
    // launcher's escape hatch, kept.
    std::string args;
    // The talk button listens. Off, "(Say something else)" is typed into only.
    bool mic = true;
    // Play asks Steam for the game. Off for a build of the game that is not Steam's, or a
    // player who starts it some other way; the app follows the game's log when it appears.
    bool launch_game = true;
    // Play is pressed for them when the window opens, which is what opening Moonshine
    // means to a player: the desktop shortcut starts Moonshine and the game together, as
    // the console launcher before the window did. Off for one who wants to press it.
    bool autostart = true;

    // Which model answers: the one that ships, on this PC, or an OpenAI-compatible API
    // of the player's choosing (app/src/openai.h).
    bool api = false;
    std::string api_url;
    std::string api_model;
    // A JSON object merged into every request, on one line.
    std::string api_extra;
    // Work out replies before they are picked. Always so with the local model; with an
    // API each guess is paid for, so it is the player's to turn on.
    bool api_guessing = false;
    // Every quest on, not only the stories the generator switched on for a new game: the
    // ones still being written, played at the player's own risk (the engine's --all-quests;
    // what /enable all does by hand, for this run of the game).
    bool experimental_quests = false;

    // Who the last bug report was from, as they typed it, so that the next one does not
    // ask again. Both optional. Collect-Logs.ps1 leaves these two lines out of the copy of
    // this file it puts in a zip: a zip made by hand may be posted anywhere.
    std::string report_name;
    std::string report_email;

    bool operator==(const Settings& other) const;
    bool operator!=(const Settings& other) const { return !(*this == other); }
};

// A settings file's text, read and rewritten. Free functions over text, so that what they
// do to a file somebody edited by hand can be tested without one.
Settings parse_settings(std::string_view text);
// `text` with every setting in `settings` put in place: a key already there has its line
// rewritten where it stands, a new one is added at the end, and one at its default is
// taken out (the file says only what was chosen). Everything else is left as it was.
std::string merge_settings(std::string_view text, const Settings& settings);

class SettingsFile {
public:
    explicit SettingsFile(std::filesystem::path path) : path_(std::move(path)) {}
    const std::filesystem::path& path() const { return path_; }
    // The defaults when there is no file, or it cannot be read.
    Settings load() const;
    // False, with the reason, when the file could not be written.
    bool save(const Settings& settings, std::string* trouble = nullptr) const;

private:
    std::filesystem::path path_;
};

// `args` taken apart as a command line is: on spaces, with "double quotes" holding a
// piece with spaces in it together.
std::vector<std::string> split_args(std::string_view line);

}  // namespace moonshine::gui

#endif  // MOONSHINE_GUI_SETTINGS_H
