// What the app is doing, said in a form a program can read.
//
// The terminal and --log are for a person: prose, in the order things happened. The window
// that starts this app for a player (app/gui) wants two things neither gives it without
// reading prose back: whether the installation is whole, as a list it can show with a mark
// beside each line, and what the app is doing this second, as one word. Both are here, and
// both are files or stdout, as everything else this app says to another program is.
//
//   --where --json     the installation check (CheckReport), as JSON on stdout
//   --status FILE      a snapshot (Status), rewritten whenever it changes
//
// Neither replaces what is printed: --where without --json prints what it always has, and
// the log goes on saying everything.

#ifndef MOONSHINE_STATUS_H
#define MOONSHINE_STATUS_H

#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace moonshine {

// One line of the installation check.
struct CheckItem {
    // What it is: "game root", "model", "voice". Empty for a line that is only words.
    std::string name;
    // Where it was found, or what was looked for.
    std::string text;
    // "ok", "missing" (required, and the app cannot do its job without it), or "optional"
    // (not there, and the app runs without it and says so).
    std::string state;
};

// The whole of --where: every input the app is about to open, found the way the app itself
// finds it. Collected rather than printed as it goes, so that the same verdicts can be said
// as prose or as JSON.
struct CheckReport {
    std::vector<CheckItem> items;
    // The game, when it was found; the window needs it for the mod's folder and the game's
    // log, and should not have to read it out of an item's text.
    std::string game_root;

    void ok(std::string_view name, std::string text);
    void missing(std::string_view name, std::string text);
    void optional(std::string_view name, std::string text);
    // Words between the items, for the prose form only.
    void note(std::string text);

    // Nothing required is missing.
    bool ready() const;

    // What --where has always printed: "name:      text", the name padded to one column,
    // " -- MISSING" after what is, and "ready" or "NOT READY ..." as the last line.
    // Collect-Logs.ps1 reads the game root off this and testers are told to look for the
    // last line, so the shape is kept.
    std::string text() const;

    // {"ready": bool, "game_root": "...", "items": [{"name", "text", "state"}, ...]}
    std::string json() const;
};

// What the app is doing. The phases, in the order a run goes through them:
//
//   starting       the process is up and has read its arguments
//   loading-model  the weights are being read in (or the API being reached)
//   warming        the world is being prefilled, the voice opened
//   ready          following the game's log; nobody is being talked to
//   talking        a conversation is open, with `talking_to`
//   stopped        asked to stop, and stopping
//   failed         could not start; `error` says why
struct Status {
    std::string phase = "starting";
    // The soul's name while a conversation is open.
    std::string talking_to;
    // Llm::describe(), once there is a model.
    std::string model;
    // Whether the model is somebody else's server (--api-url), which the window says
    // plainly: the words of a conversation leave the machine then.
    bool remote_model = false;
    // Whether the voice and the microphone opened. A build or an installation without
    // either still talks, on the screen and from the keyboard.
    bool voice = false;
    bool ear = false;
    // Whether the game's log has been found to follow.
    bool game_log = false;
    // The last thing that went wrong, in a sentence a player can act on; empty when the
    // last thing asked of the model worked.
    std::string error;

    std::string json() const;
    bool operator==(const Status& other) const;
};

// The snapshot on disk. Written to a file beside the one named and renamed over it, so a
// reader never sees half of one; written only when something changed, so a loop can call
// update() on every pass. Safe from any thread.
class StatusFile {
public:
    // An empty path is no file: every call is then a no-op, which is how a run without
    // --status goes.
    explicit StatusFile(std::filesystem::path path);

    void update(const std::function<void(Status&)>& change);
    Status now() const;
    bool on() const { return !path_.empty(); }

private:
    void write(const Status& status);

    std::filesystem::path path_;
    mutable std::mutex lock_;
    Status status_;
    bool written_ = false;
};

}  // namespace moonshine

#endif  // MOONSHINE_STATUS_H
