// What the window can ask for, and what it is told: the whole of the app that is not
// pixels.
//
// The page (app/gui/ui) knows how to draw a state and which call a button makes; it
// decides nothing. Everything it shows is in the one JSON object state() returns, down to
// the sentence on the home screen and the label on its button, and everything it can do is
// a name and a JSON object given to call(). So the behaviour of the window is the
// behaviour of this class, and this class has no window in it: it is in the library the
// unit tests link, and tests/test_gui_bridge.cpp drives it with engines that are not
// there and a clock that is told the time.
//
// The calls:
//
//   getState                         the state, now
//   play, stop                       the engine (and, through Steam, the game)
//   recheck                          ask the engine again what it finds
//   readLog {which, from}            the next piece of a log, for the log view
//   saveSettings {settings.., key}   write moonshine.cfg (and the key, to the system's
//                                    store); restarts a running engine
//   testApi {api_url, ..}            ask the API in the form one small question (slow)
//   checkUpdate, applyUpdate, repair, cancelTask, dismissTask
//   collectLogs                      the zip of logs, on the desktop
//   sendReport {name, email,         the same zip, sent to us with what the player wrote
//               description}         (cloud/bug-reports); on the desktop if it will not go
//   uninstall {remove_mod}
//   open {what}                      the logs folder, the read me, the licences
//
// Long work (a download, the log collector, a report on its way) is a task: one at a
// time, with its progress in the state.

#ifndef MOONSHINE_GUI_BRIDGE_H
#define MOONSHINE_GUI_BRIDGE_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "fetch.h"
#include "json.hpp"
#include "child.h"
#include "secrets.h"
#include "settings.h"
#include "supervisor.h"

namespace moonshine::gui {

// What the home screen says: one line, one more underneath, one button, and when
// something is wrong the one thing to do about it.
struct HomeView {
    // idle (nothing running), busy (getting there), live (the characters can hear),
    // problem.
    std::string tone = "idle";
    std::string title;
    std::string detail;
    // The main button: its label and the call it makes. No label, no button.
    std::string button;
    std::string action;
    // The way out of a problem: "repair", "folder", "settings", "logs", "reinstall".
    std::string fix_label;
    std::string fix;
};

// The result of the engine's --where --json, as the window keeps it.
struct CheckState {
    // "checking", "done", or "failed" (the engine could not be asked at all).
    std::string state = "checking";
    bool ready = false;
    std::string game_root;
    std::string error;
    nlohmann::json items = nlohmann::json::array();
};

struct TaskState {
    // "", "update", "repair", "collect", "report".
    std::string kind;
    bool running = false;
    std::string title;
    std::string detail;
    std::uint64_t done = 0;
    std::uint64_t total = 0;
    // How it ended: one of these is set once it is no longer running.
    std::string error;
    std::string result;
    // A file it left for the player (the zip of logs; a report's, only when it did not go).
    std::string path;
};

struct UpdateState {
    // "unknown", "checking", "current", "available", "failed".
    std::string state = "unknown";
    std::string latest;
    std::string error;
};

// The home screen's words, from everything else. A function of its arguments, so that
// every sentence a player can be shown is in one place and under test.
HomeView home_view(const CheckState& check, const EngineState& engine, const TaskState& task,
                   const Settings& settings);

class Controller {
public:
    // Everything that reaches outside the process, so that a test can stand in for it.
    struct Parts {
        Paths paths;
        Supervisor::Hooks hooks;
        std::unique_ptr<SecretStore> secrets;
        std::function<Ran(const Command&, std::chrono::milliseconds, const std::function<bool()>&)> run;
        std::function<bool(const Command&, std::string*)> launch;
        std::function<bool(const std::string&)> open;
        std::function<std::optional<std::string>(const std::string& url, std::string* trouble)> fetch_text;
        std::function<FetchResult(const Manifest&, const FetchOptions&)> fetch_files;
        // A bug report's zip, to the service that takes them (fetch.h, upload_file).
        std::function<Upload(const std::string& url, const std::filesystem::path& file,
                             const std::vector<std::pair<std::string, std::string>>& headers,
                             const std::function<void(std::uint64_t, std::uint64_t)>& progress,
                             const std::function<bool()>& cancelled)>
            upload;
        // The uninstaller this installation was given; empty when it has none.
        std::function<std::filesystem::path()> uninstaller;
        // Where an installer is downloaded to.
        std::filesystem::path temp;
        // Set by the window: close it, and let another instance (the installer's relaunch)
        // start.
        std::function<void()> quit;
        std::function<void()> release_instance;

        // The real ones, for an installation at `paths`.
        static Parts system(const Paths& paths);
    };

    explicit Controller(Parts parts);
    ~Controller();

    // The first check, and Play if the player asked for that on opening: by the setting,
    // or (play) by starting the window with --play.
    void start(bool play = false);
    // Stop the engine and wait for anything in hand.
    void shutdown();

    nlohmann::json state();
    nlohmann::json call(const std::string& method, const nlohmann::json& args);
    // A call that may take seconds, which the window makes off the thread it draws on.
    static bool slow(const std::string& method);

    // Run something off the thread that asked; joined by shutdown().
    void in_background(std::function<void()> work) { background(std::move(work)); }

    // Waits until something the state shows may have changed, or `limit`.
    void wait_for_change(std::chrono::milliseconds limit);
    // Whether the engine is running or meant to be, which is when closing the window is
    // worth a question.
    bool engine_active() const;

    const Paths& paths() const { return parts_.paths; }
    // The game's folder, as the last check found it or as the settings say.
    std::string game_root();
    // Put the game's folder in the settings (the folder picker's answer) and look again.
    nlohmann::json set_root(const std::string& root);

private:
    void changed();
    void background(std::function<void()> work);
    void run_check(bool then_play);
    void begin_play();
    nlohmann::json save_settings(const nlohmann::json& args);
    nlohmann::json test_api(const nlohmann::json& args);
    nlohmann::json read_log(const nlohmann::json& args);
    nlohmann::json open(const nlohmann::json& args);
    bool begin_task(const std::string& kind, const std::string& title);
    void end_task(const std::string& error, const std::string& result);
    void check_update();
    void apply_update(bool reinstall);
    void repair();
    void collect_logs();
    nlohmann::json send_report(const nlohmann::json& args);
    nlohmann::json uninstall(const nlohmann::json& args);
    std::optional<Manifest> published(std::string* trouble);
    FetchResult fetch_with_progress(const Manifest& manifest, const std::filesystem::path& dest,
                                    const std::string& doing);
    void fetch_log(const std::string& line);

    Parts parts_;
    SettingsFile config_;
    std::string version_;

    std::mutex lock_;
    std::condition_variable change_;
    std::uint64_t revision_ = 0;
    Settings settings_;
    bool has_key_ = false;
    CheckState check_;
    std::uint64_t check_generation_ = 0;
    TaskState task_;
    UpdateState update_;
    std::optional<Manifest> latest_;
    std::atomic<bool> cancel_task_{false};
    std::atomic<bool> closing_{false};

    struct Worker {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };
    std::mutex workers_lock_;
    std::vector<Worker> workers_;

    // Last, so that it is the first to go: stopping it says "changed", which is the lock
    // and the condition above.
    Supervisor supervisor_;
};

}  // namespace moonshine::gui

#endif  // MOONSHINE_GUI_BRIDGE_H
