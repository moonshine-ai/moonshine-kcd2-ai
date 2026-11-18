#include "supervisor.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <system_error>

#include "json.hpp"

namespace moonshine::gui {

namespace fs = std::filesystem;

namespace {

// 0xC000013A, as the int a process's exit code comes back as: Ctrl+C, or the console
// window closed on it. Deliberate.
constexpr int kControlC = -1073741510;

std::string formatted_time(std::time_t when, const char* format) {
    std::tm parts{};
#ifdef _WIN32
    localtime_s(&parts, &when);
#else
    localtime_r(&when, &parts);
#endif
    char buffer[64];
    const std::size_t wrote = std::strftime(buffer, sizeof buffer, format, &parts);
    return std::string(buffer, wrote);
}

// The first entry by name under `dir` that `wanted` accepts; empty when there is none.
std::string first_under(const fs::path& dir, const std::function<bool(const fs::directory_entry&)>& wanted) {
    std::vector<std::string> names;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (wanted(*it)) names.push_back(utf8_of(it->path().filename()));
    }
    if (names.empty()) return {};
    std::sort(names.begin(), names.end());
    return names.front();
}

}  // namespace

Paths Paths::at(const fs::path& home, const fs::path& engine_dir) {
    Paths paths;
    paths.home = home;
#ifdef _WIN32
    const char* engine = "moonshine_app.exe";
#else
    const char* engine = "moonshine_app";
#endif
    paths.engine = (engine_dir.empty() ? home : engine_dir) / engine;
    paths.config = home / "moonshine.cfg";
    paths.logs = home / "logs";
    paths.engine_log = paths.logs / "moonshine_app.log";
    paths.launcher_log = paths.logs / "moonshine.log";
    paths.status = paths.logs / "status.json";
    paths.stop_marker = paths.logs / "stop";
    return paths;
}

std::vector<std::string> common_arguments(const Paths& paths, const Settings& settings) {
    std::vector<std::string> args;
    const fs::path llm = fs::path("models") / "llm";
    const std::string gguf = first_under(paths.home / llm, [](const fs::directory_entry& entry) {
        std::error_code ec;
        return entry.is_regular_file(ec) && entry.path().extension() == ".gguf";
    });
    if (!gguf.empty()) {
        args.push_back("--gguf");
        args.push_back(utf8_of(llm / path_from_utf8(gguf)));
    }
    const fs::path stt = fs::path("models") / "stt";
    const std::string ear = first_under(paths.home / stt, [](const fs::directory_entry& entry) {
        std::error_code ec;
        return entry.is_directory(ec);
    });
    if (!ear.empty()) {
        args.push_back("--stt-model");
        args.push_back(utf8_of(stt / path_from_utf8(ear)));
    }
    if (!settings.root.empty()) {
        args.push_back("--root");
        args.push_back(settings.root);
    }
    if (settings.api) {
        // The window does not start an engine for an API with no address (bridge.cpp's
        // check): the engine reads an empty one as no API, and would answer locally.
        args.push_back("--api-url");
        args.push_back(settings.api_url);
        args.push_back("--api-model");
        args.push_back(settings.api_model);
        if (settings.api_guessing) args.push_back("--guessing");
    }
    if (!settings.mic) args.push_back("--no-mic");
    // Last, so that what somebody wrote by hand in moonshine.cfg has the final word.
    for (std::string& arg : split_args(settings.args)) args.push_back(std::move(arg));
    return args;
}

namespace {

Command engine_with(const Paths& paths, const Settings& settings, const std::string& api_key,
                    std::vector<std::string> first) {
    Command command;
    command.exe = paths.engine;
    command.cwd = paths.home;
    command.args = std::move(first);
    for (std::string& arg : common_arguments(paths, settings)) command.args.push_back(std::move(arg));
    if (settings.api) {
        // Set even when empty: a key left in the environment this window was started from
        // is not the key the player gave.
        command.env.emplace_back("MOONSHINE_API_KEY", api_key);
        command.env.emplace_back("MOONSHINE_API_EXTRA", settings.api_extra);
    }
    return command;
}

}  // namespace

Command engine_command(const Paths& paths, const Settings& settings, const std::string& api_key) {
    return engine_with(paths, settings, api_key,
                       {"--game", "--world", "setting.txt", "--log", utf8_of(paths.engine_log), "--status",
                        utf8_of(paths.status), "--quit-on-eof"});
}

Command check_command(const Paths& paths, const Settings& settings, const std::string& api_key) {
    return engine_with(paths, settings, api_key, {"--where", "--json"});
}

void rotate_logs(const fs::path& log, std::time_t now, int keep) {
    std::error_code ec;
    if (fs::exists(log, ec)) {
        fs::path kept = log;
        kept += "." + formatted_time(now, "%Y%m%d-%H%M%S");
        fs::remove(kept, ec);
        fs::rename(log, kept, ec);
    }
    const std::string prefix = log.filename().string() + ".";
    struct Old {
        fs::file_time_type written;
        fs::path path;
    };
    std::vector<Old> old;
    for (fs::directory_iterator it(log.parent_path(), ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (name.rfind(prefix, 0) != 0) continue;
        std::error_code when;
        old.push_back({it->last_write_time(when), it->path()});
    }
    // Newest first; the name, which is the time it was put aside, settles two written in
    // the same tick.
    std::sort(old.begin(), old.end(), [](const Old& a, const Old& b) {
        if (a.written != b.written) return a.written > b.written;
        return a.path.filename().string() > b.path.filename().string();
    });
    for (std::size_t i = static_cast<std::size_t>(std::max(keep, 0)); i < old.size(); ++i) {
        fs::remove(old[i].path, ec);
    }
}

Verdict RestartPolicy::after(int code, int life_seconds, bool asked) {
    Verdict verdict;
    if (code == 0 || code == kControlC) {
        verdict.line = "app stopped (exit " + std::to_string(code) + ").";
        return verdict;
    }
    if (asked) {
        verdict.line = "app stopped (asked to).";
        return verdict;
    }
    if (life_seconds < 20) {
        fast_ += 1;
    } else {
        fast_ = 0;
    }
    char hex[16];
    std::snprintf(hex, sizeof hex, "0x%08X", static_cast<unsigned int>(code));
    const std::string died =
        "the app exited (" + std::string(hex) + ") after " + std::to_string(life_seconds) + "s";
    if (fast_ >= 5) {
        verdict.gave_up = true;
        verdict.line = died + ", " + std::to_string(fast_) +
                       " times within 20s of starting. Not starting it again; press Send bug report on the Home screen, which sends these logs.";
        return verdict;
    }
    verdict.restart = true;
    verdict.wait_seconds = 2;
    if (fast_ > 1) verdict.wait_seconds = std::min(16, 1 << fast_);
    verdict.line = died + ". Starting it again in " + std::to_string(verdict.wait_seconds) +
                   "s. Wait until it is listening, then say again anything that was cut off.";
    return verdict;
}

bool read_status(const fs::path& file, EngineState* state) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    std::ostringstream text;
    text << in.rdbuf();
    const nlohmann::json found = nlohmann::json::parse(text.str(), nullptr, false);
    if (!found.is_object()) return false;
    const auto word = [&](const char* key) {
        const auto it = found.find(key);
        return it != found.end() && it->is_string() ? it->get<std::string>() : std::string();
    };
    const auto flag = [&](const char* key) {
        const auto it = found.find(key);
        return it != found.end() && it->is_boolean() && it->get<bool>();
    };
    state->phase = word("phase");
    state->talking_to = word("talking_to");
    state->model = word("model");
    state->remote_model = flag("remote_model");
    state->voice = flag("voice");
    state->ear = flag("ear");
    state->game_log = flag("game_log");
    state->error = word("error");
    return true;
}

Supervisor::Hooks Supervisor::Hooks::system(const Paths& paths) {
    Hooks hooks;
    hooks.spawn = [](const Command& command, std::string* trouble) { return gui::spawn(command, trouble); };
#ifdef _WIN32
    // Only where it can be known: the game is a Windows program, and with no way of
    // telling, the window says what the engine says rather than "not running".
    hooks.game_running = [] { return program_running("KingdomCome.exe"); };
#endif
    // Through Steam, which is the only start the Steam build accepts, and which also
    // brings the client up when it is not.
    hooks.start_game = [] { open_target("steam://rungameid/1771300"); };
    hooks.stop_strays = [paths] {
        std::vector<long> found = running_copies(paths.engine);
        if (found.empty()) return true;
        // The console launcher, if that is what is keeping one, starts its engine again two
        // seconds after it dies unless this file says the death was asked for.
        {
            std::ofstream marker(paths.stop_marker, std::ios::trunc);
            marker << "stopped by the Moonshine window\n";
        }
        for (const long pid : found) kill_process(pid);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (std::chrono::steady_clock::now() < deadline) {
            if (running_copies(paths.engine).empty()) {
                // Long enough for that launcher to have seen the marker.
                std::this_thread::sleep_for(std::chrono::seconds(1));
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        return false;
    };
    hooks.seconds = [] {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    };
    hooks.wall = [] { return std::time(nullptr); };
    return hooks;
}

Supervisor::Supervisor(Paths paths, Hooks hooks) : paths_(std::move(paths)), hooks_(std::move(hooks)) {
    std::error_code ec;
    fs::create_directories(paths_.logs, ec);
    // The launcher's log is added to for as long as the installation lives; the older half
    // goes when it has grown past anything a person would read.
    constexpr std::uintmax_t kTooLong = 512 * 1024;
    const std::uintmax_t size = fs::file_size(paths_.launcher_log, ec);
    if (!ec && size > kTooLong) {
        std::string text;
        {
            std::ifstream in(paths_.launcher_log, std::ios::binary);
            std::ostringstream all;
            all << in.rdbuf();
            text = all.str();
        }
        std::size_t from = text.size() - kTooLong / 2;
        const std::size_t line = text.find('\n', from);
        from = line == std::string::npos ? from : line + 1;
        std::ofstream out(paths_.launcher_log, std::ios::binary | std::ios::trunc);
        out << text.substr(from);
    }
}

Supervisor::~Supervisor() { stop(); }

void Supervisor::note(const std::string& line) {
    std::lock_guard<std::mutex> held(log_lock_);
    std::ofstream out(paths_.launcher_log, std::ios::binary | std::ios::app);
    const std::time_t now = hooks_.wall ? hooks_.wall() : std::time(nullptr);
    out << formatted_time(now, "%Y-%m-%d %H:%M:%S") << "  " << line << "\n";
}

void Supervisor::set(const std::function<void(EngineState&)>& change) {
    {
        std::lock_guard<std::mutex> held(lock_);
        change(state_);
    }
    if (hooks_.changed) hooks_.changed();
}

bool Supervisor::pause(double seconds) {
    if (hooks_.wait) {
        hooks_.wait(seconds);
        std::lock_guard<std::mutex> held(lock_);
        return !stop_asked_;
    }
    std::unique_lock<std::mutex> held(lock_);
    return !wake_.wait_for(held, std::chrono::duration<double>(seconds), [this] { return stop_asked_; });
}

bool Supervisor::active() const {
    std::lock_guard<std::mutex> held(lock_);
    return running_;
}

namespace {
// How often the game is looked for while the engine runs, and how long after Steam was
// asked for it "not running yet" still means "on its way": Steam may have to start
// itself first, and says so in its own window if it cannot.
constexpr double kGameLook = 2.0;
constexpr double kGameGrace = 90.0;
}  // namespace

void Supervisor::look_for_game() {
    if (!hooks_.game_running) return;
    const bool up = hooks_.game_running();
    const double now = hooks_.seconds();
    bool differs = false;
    {
        std::lock_guard<std::mutex> held(lock_);
        std::string game = "down";
        if (up) {
            game = "up";
        } else if (game_asked_ >= 0 && now - game_asked_ < kGameGrace) {
            game = "starting";
        }
        if (game != "starting") game_asked_ = -1;
        differs = state_.game != game;
        state_.game = game;
    }
    if (differs && hooks_.changed) hooks_.changed();
}

void Supervisor::start_game() {
    {
        std::lock_guard<std::mutex> held(lock_);
        if (!running_ || stop_asked_ || !hooks_.start_game) return;
    }
    if (hooks_.game_running && hooks_.game_running()) {
        look_for_game();
        return;
    }
    note("asking Steam to start Kingdom Come: Deliverance 2 again; the app is still running");
    hooks_.start_game();
    {
        std::lock_guard<std::mutex> held(lock_);
        game_asked_ = hooks_.seconds();
        if (hooks_.game_running) state_.game = "starting";
    }
    if (hooks_.changed) hooks_.changed();
}

EngineState Supervisor::state() const {
    EngineState now;
    {
        std::lock_guard<std::mutex> held(lock_);
        now = state_;
    }
    if (now.state == "running" || now.state == "stopping") read_status(paths_.status, &now);
    return now;
}

void Supervisor::play(const Settings& settings, const std::string& api_key) {
    std::lock_guard<std::mutex> one(control_);
    {
        std::lock_guard<std::mutex> held(lock_);
        if (running_) return;
    }
    // The last run's thread has finished its loop; this is only collecting it.
    if (thread_.joinable()) thread_.join();
    {
        std::lock_guard<std::mutex> held(lock_);
        running_ = true;
        stop_asked_ = false;
        game_asked_ = -1;
        state_ = EngineState{};
        state_.state = "starting";
    }
    if (hooks_.changed) hooks_.changed();
    thread_ = std::thread([this, settings, api_key] { loop(settings, api_key); });
}

void Supervisor::stop() {
    std::lock_guard<std::mutex> one(control_);
    {
        std::lock_guard<std::mutex> held(lock_);
        if (running_) {
            stop_asked_ = true;
            if (state_.state == "running" || state_.state == "starting") state_.state = "stopping";
        }
    }
    wake_.notify_all();
    if (hooks_.changed) hooks_.changed();
    if (thread_.joinable()) thread_.join();
    std::lock_guard<std::mutex> held(lock_);
    stop_asked_ = false;
}

void Supervisor::loop(Settings settings, std::string api_key) {
    const auto stop_asked = [this] {
        std::lock_guard<std::mutex> held(lock_);
        return stop_asked_;
    };
    const auto finish = [this](const std::string& state, const std::string& message) {
        set([&](EngineState& now) {
            const int restarts = now.restarts;
            now = EngineState{};
            now.state = state;
            now.message = message;
            now.restarts = restarts;
            running_ = false;
            game_asked_ = -1;
        });
    };
    std::error_code ec;
    fs::create_directories(paths_.logs, ec);

    if (hooks_.stop_strays && !hooks_.stop_strays()) {
        note("an app already running would not stop. Not starting another beside it.");
        finish("failed", "A Moonshine app that was already running would not stop. Restart the PC, then press Play.");
        return;
    }
    fs::remove(paths_.stop_marker, ec);

    // The game before the engine, so that its own load, which is the longer one, is under
    // way while the model loads; the engine follows the game's log from the moment it
    // appears.
    if (!settings.launch_game) {
        note("not starting the game; the app follows its log when it appears");
    } else if (hooks_.game_running && hooks_.game_running()) {
        note("the game is already running; joining it");
    } else {
        note("asking Steam to start Kingdom Come: Deliverance 2");
        if (hooks_.start_game) hooks_.start_game();
        // On its way, for the window, until it is seen running or has had long enough.
        std::lock_guard<std::mutex> held(lock_);
        game_asked_ = hooks_.seconds();
        if (hooks_.game_running) state_.game = "starting";
    }

    RestartPolicy policy;
    const Command command = engine_command(paths_, settings, api_key);
    while (true) {
        rotate_logs(paths_.engine_log, hooks_.wall());
        // The file of a run that is over says nothing about this one.
        fs::remove(paths_.status, ec);

        const double started = hooks_.seconds();
        std::string trouble;
        std::unique_ptr<Child> child = hooks_.spawn(command, &trouble);
        int code = 1;
        bool asked = false;
        if (!child) {
            note("could not start the app: " + trouble);
        } else {
            set([](EngineState& now) {
                if (now.state != "stopping") now.state = "running";
                now.message.clear();
            });
            // Long enough ago that the first look is at once.
            double looked = hooks_.seconds() - kGameLook;
            while (true) {
                if (const std::optional<int> exited = child->wait_for(std::chrono::milliseconds(200))) {
                    code = *exited;
                    break;
                }
                if (!stop_asked()) {
                    // The game comes and goes while the engine stays: a player quits it
                    // and leaves Moonshine open, and the window offers to start it again.
                    if (const double now = hooks_.seconds(); now - looked >= kGameLook) {
                        looked = now;
                        look_for_game();
                    }
                    continue;
                }
                asked = true;
                // /quit is read once the engine is following the game, where it tells the
                // game nobody is listening before it goes. An engine still loading its
                // model reads nothing until that is done, and has told the game nothing
                // that needs taking back.
                EngineState now;
                const bool up = read_status(paths_.status, &now) && (now.phase == "ready" || now.phase == "talking");
                child->say("/quit");
                child->hang_up();
                const std::optional<int> went = child->wait_for(std::chrono::milliseconds(up ? 8000 : 1500));
                if (went) {
                    code = *went;
                } else {
                    note("the app did not stop when asked; ending it");
                    child->kill();
                    code = child->wait_for(std::chrono::milliseconds(1000)).value_or(1);
                }
                break;
            }
        }
        // What the engine itself said was wrong, if it got as far as saying.
        EngineState last;
        read_status(paths_.status, &last);
        child.reset();

        if (!asked && fs::exists(paths_.stop_marker, ec)) {
            // Moonshine.ps1 -Stop from a console: the same wish, said the old way.
            fs::remove(paths_.stop_marker, ec);
            asked = true;
        }
        const int life = static_cast<int>(hooks_.seconds() - started);
        const Verdict verdict = policy.after(code, life, asked || stop_asked());
        note(verdict.line);
        const std::string why = last.phase == "failed" && !last.error.empty() ? last.error : std::string();
        if (verdict.gave_up) {
            finish("failed", why.empty() ? "Moonshine keeps stopping as soon as it starts." : why);
            return;
        }
        if (!verdict.restart) {
            finish("idle", "");
            return;
        }
        set([&](EngineState& now) {
            now.state = "restarting";
            now.restarts += 1;
            now.message = why.empty() ? "Moonshine stopped unexpectedly and is starting again." : why;
        });
        if (!pause(verdict.wait_seconds)) {
            note("stopped while waiting to start the app again.");
            finish("idle", "");
            return;
        }
    }
}

}  // namespace moonshine::gui
