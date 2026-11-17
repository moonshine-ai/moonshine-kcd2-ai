// Starting other programs: the engine (moonshine_app), which is kept and talked to, and
// the few that are run to an end or set going and left (the log collector, the installer,
// Steam through its URL).
//
// The window is one process and the engine another (see supervisor.h for why), and this
// is the seam between them. What the engine needs of it:
//
//   - a keyboard: a pipe on its stdin, down which `/quit` is sent for a clean stop, and
//     whose closing is itself the stop (--quit-on-eof), so an engine never outlives a
//     window that died without saying goodbye;
//   - no console window of its own;
//   - an environment with one variable more than ours, which is how the API key reaches
//     it without ever being on a command line that Task Manager shows;
//   - to die with us if we are killed (a job object on Windows).
//
// Everything in and out is UTF-8; the Windows half widens it at the boundary.
//
// (Not process.h: this folder is on the include path, and MSVC's <thread> includes the C
// runtime's <process.h>, which a file of that name here would be found instead of.)

#ifndef MOONSHINE_GUI_CHILD_H
#define MOONSHINE_GUI_CHILD_H

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace moonshine::gui {

struct Command {
    std::filesystem::path exe;
    std::vector<std::string> args;
    // Where it runs; empty for where we are.
    std::filesystem::path cwd;
    // Set on top of this process's own environment.
    std::vector<std::pair<std::string, std::string>> env;
};

// A program that was started and is ours to stop.
class Child {
public:
    virtual ~Child() = default;
    // Its exit code once it has one; nothing while it is still running after `limit`.
    virtual std::optional<int> wait_for(std::chrono::milliseconds limit) = 0;
    // A line down its stdin. False when it is no longer reading.
    virtual bool say(std::string_view line) = 0;
    // Close its stdin.
    virtual void hang_up() = 0;
    virtual void kill() = 0;
    // What it has printed (stdout and stderr together) since this was last asked. Only the
    // most recent part is kept of a program that prints for hours.
    virtual std::string take_output() = 0;
};

// Null, with the reason, when it could not be started.
std::unique_ptr<Child> spawn(const Command& command, std::string* trouble = nullptr);

struct Ran {
    // It was started at all; `trouble` says why not.
    bool started = false;
    // It ran to its own end, rather than being stopped at the limit or by `cancelled`.
    bool finished = false;
    int code = -1;
    std::string output;
    std::string trouble;
};

// Run it to the end and say what it printed. Killed at `limit`, or when `cancelled` says so.
Ran run(const Command& command, std::chrono::milliseconds limit,
        const std::function<bool()>& cancelled = {});

// Set it going and leave it: with its own window, not stopped when we are. The installer
// and the uninstaller, which have to outlive the app they replace or remove.
bool launch(const Command& command, std::string* trouble = nullptr);

// Hand a URL, a file or a folder to the desktop to open with whatever opens it.
bool open_target(const std::string& what);

// Whether a program with this file name ("KingdomCome.exe") is running.
bool program_running(std::string_view image_name);
// The running copies of exactly this program file.
std::vector<long> running_copies(const std::filesystem::path& exe);
bool kill_process(long pid);

// The uninstaller the installer left for the installation at `home`; empty when it was not
// installed that way (a build tree, a folder copied by hand).
std::filesystem::path installed_uninstaller(const std::filesystem::path& home);

// One argument as CreateProcess wants it written so that the program's argv gets it back
// whole: quoted when it holds a space or a quote, backslashes doubled only where a quote
// follows them.
std::string quote_argument(std::string_view argument);
std::string command_line(const Command& command);

// Paths and text across the boundary: std::filesystem takes a narrow string as the
// system's own encoding, which on Windows is not UTF-8.
std::filesystem::path path_from_utf8(std::string_view text);
std::string utf8_of(const std::filesystem::path& path);

// This program's own file.
std::filesystem::path own_path();
// A variable of this process's environment, as UTF-8; empty when it is not set.
std::string environment(const char* name);

}  // namespace moonshine::gui

#endif  // MOONSHINE_GUI_CHILD_H
