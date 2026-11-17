// Keeping the engine running: what installer/Moonshine.ps1 did in a console window, done
// from the window instead.
//
// The engine is moonshine_app, a separate program, and stays one. It loads four gigabytes
// and holds the game's conversation; when it dies -- and a program that runs a model and
// a voice for hours beside a game does, now and then -- the player should be told in a
// sentence and have it back in two seconds, with the window still there to say so. A
// settings change is then a restart of the engine and nothing cleverer, and every way of
// running the engine by hand (the terminal, scripts/fake_game.py, the verify scripts) is
// untouched by there being a window.
//
// The loop is the script's, step for step, because the numbers in it were arrived at on
// players' machines:
//
//   1. an engine already running from this folder is stopped (two would both answer);
//   2. Steam is asked for the game, unless it is up or the player starts it themselves;
//   3. the last run's log is kept beside the new one, five of them;
//   4. the engine is started, and when it exits:
//        - exit 0, a deliberate stop, or the stop marker: that is the end;
//        - otherwise it is started again after two seconds, or longer if it keeps dying
//          within twenty seconds of starting, and not at all after five such deaths.
//
// What is new is the stop: `/quit` down the engine's stdin, which it answers by telling
// the game nobody is listening and exiting 0; killed only if that takes too long.
//
// Everything that touches the machine comes in through Hooks, so the loop is tested with
// a clock that is told the time and engines that die on cue (tests/test_gui_supervisor.cpp).

#ifndef MOONSHINE_GUI_SUPERVISOR_H
#define MOONSHINE_GUI_SUPERVISOR_H

#include <condition_variable>
#include <ctime>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "child.h"
#include "settings.h"

namespace moonshine::gui {

// Where things are in an installation: everything is beside the app, as the installer
// lays it out and as the engine expects of its working directory.
struct Paths {
    std::filesystem::path home;
    std::filesystem::path engine;
    std::filesystem::path config;
    std::filesystem::path logs;
    std::filesystem::path engine_log;
    std::filesystem::path launcher_log;
    std::filesystem::path status;
    std::filesystem::path stop_marker;

    // `home` is the installation; `engine_dir` is where moonshine_app is when that is
    // somewhere else (a build tree), and `home` when empty.
    static Paths at(const std::filesystem::path& home, const std::filesystem::path& engine_dir = {});
};

// --- the pieces, each a function of what it is given -----------------------------------

// What the engine is told that does not depend on how it is being run: which weights,
// which game, which model answers. --where is asked with exactly these, so that the check
// is of what will be started.
//
// The weights are found by shape, not by name -- the one .gguf under models/llm, the one
// folder under models/stt -- so a release that ships a different model changes nothing.
std::vector<std::string> common_arguments(const Paths& paths, const Settings& settings);

// The engine as it is started to play. The API key is in the environment and nowhere
// else: a command line is shown by Task Manager and written to the log.
Command engine_command(const Paths& paths, const Settings& settings, const std::string& api_key);
// The engine asked what it finds (--where --json).
Command check_command(const Paths& paths, const Settings& settings, const std::string& api_key);

// The dead run's log, kept beside the new one under the time it was put aside, and all
// but the newest `keep` of those removed. The engine truncates its log on opening it,
// which would erase the run it is being restarted from.
void rotate_logs(const std::filesystem::path& log, std::time_t now, int keep = 5);

// What to do when the engine has exited.
struct Verdict {
    bool restart = false;
    // It was not asked to stop and is not being started again: something is wrong that
    // starting it a sixth time will not fix.
    bool gave_up = false;
    int wait_seconds = 0;
    // For the launcher's log, and (when not deliberate) for the player.
    std::string line;
};

class RestartPolicy {
public:
    // `code` as the system gave it, `life_seconds` from start to exit, `asked` when the
    // stop was ours or the stop marker's.
    Verdict after(int code, int life_seconds, bool asked);
    void reset() { fast_ = 0; }

private:
    // Deaths in a row within twenty seconds of starting. Twenty seconds is under a model
    // load, so an engine that lived longer had come up.
    int fast_ = 0;
};

// --- the loop ----------------------------------------------------------------------------

// What the window shows of the engine.
struct EngineState {
    // idle        not running, and not meant to be
    // starting    being started (the game asked for, the logs put aside)
    // running     the engine is up; `phase` says how far it has got
    // restarting  it died and is about to be started again; `message` says so
    // stopping    asked to stop, and going
    // failed      it will not stay up; `message` says what to do
    std::string state = "idle";
    std::string message;
    // How many times it has had to be started again since Play.
    int restarts = 0;

    // From the engine's own status file (app/src/status.h) while it runs.
    std::string phase;
    std::string talking_to;
    std::string model;
    bool remote_model = false;
    bool voice = false;
    bool ear = false;
    bool game_log = false;
    std::string error;
};

class Supervisor {
public:
    struct Hooks {
        std::function<std::unique_ptr<Child>(const Command&, std::string* trouble)> spawn;
        // The game is already up.
        std::function<bool()> game_running;
        // Ask Steam for it.
        std::function<void()> start_game;
        // Stop any engine already running from this installation; false when one would not
        // stop.
        std::function<bool()> stop_strays;
        // Seconds on a clock that only goes forward, and the time of day.
        std::function<double()> seconds;
        std::function<std::time_t()> wall;
        // Wait this long. When not set the supervisor waits for real (and a stop cuts the
        // wait short); a test sets it and moves its clock on instead.
        std::function<void(double seconds)> wait;
        // Something changed that the window shows.
        std::function<void()> changed;

        // The real ones.
        static Hooks system(const Paths& paths);
    };

    Supervisor(Paths paths, Hooks hooks);
    ~Supervisor();

    // Start the engine and keep it running. Nothing happens if it already is.
    void play(const Settings& settings, const std::string& api_key);
    // Stop it and wait until it has gone.
    void stop();
    // Whether the loop is going (starting, running or between restarts).
    bool active() const;
    EngineState state() const;

    // A line in the launcher's log, with the time.
    void note(const std::string& line);

private:
    void loop(Settings settings, std::string api_key);
    void set(const std::function<void(EngineState&)>& change);
    bool pause(double seconds);

    const Paths paths_;
    Hooks hooks_;

    // One play or stop at a time, from whichever thread asks.
    std::mutex control_;
    mutable std::mutex lock_;
    std::condition_variable wake_;
    EngineState state_;
    bool stop_asked_ = false;
    bool running_ = false;
    std::thread thread_;
    std::mutex log_lock_;
};

// The engine's status file, read into `state`. False when there is none, or it is not
// whole.
bool read_status(const std::filesystem::path& file, EngineState* state);

}  // namespace moonshine::gui

#endif  // MOONSHINE_GUI_SUPERVISOR_H
