#include "bridge.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>
#include <system_error>

namespace moonshine::gui {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

std::string trimmed(std::string text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.pop_back();
    std::size_t from = 0;
    while (from < text.size() && std::isspace(static_cast<unsigned char>(text[from]))) ++from;
    return text.substr(from);
}

std::string file_text(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return {};
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

// The version this installation says it is: the file the installer writes, the one at the
// top of a checkout, or what its manifest says.
std::string installed_version(const Paths& paths) {
    for (const char* name : {"VERSION.txt", "VERSION"}) {
        const std::string found = trimmed(file_text(paths.home / name));
        if (!found.empty() && found.size() < 64) return found;
    }
    if (const std::optional<Manifest> manifest = read_manifest(paths.home / "manifest.json")) return manifest->version;
    return "dev";
}

std::string megabytes(std::uint64_t bytes) {
    const std::uint64_t whole = (bytes + 512 * 1024) / (1024 * 1024);
    // 2381 reads as a number of megabytes; 2,381 reads as a size.
    std::string digits = std::to_string(whole);
    for (int at = static_cast<int>(digits.size()) - 3; at > 0; at -= 3) digits.insert(static_cast<std::size_t>(at), ",");
    return digits;
}

// The last piece of a manifest path, which is what a person recognises.
std::string leaf(const std::string& path) {
    const std::size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

json settings_json(const Settings& s) {
    return {{"root", s.root},           {"args", s.args},           {"mic", s.mic},
            {"launch_game", s.launch_game}, {"autostart", s.autostart}, {"api", s.api},
            {"api_url", s.api_url},     {"api_model", s.api_model}, {"api_extra", s.api_extra},
            {"api_guessing", s.api_guessing}};
}

// The settings the page sent, over the ones in force: a field it did not send is left as
// it was.
Settings settings_from(const json& in, Settings s) {
    const auto text = [&](const char* key, std::string& into) {
        const auto it = in.find(key);
        if (it != in.end() && it->is_string()) into = trimmed(it->get<std::string>());
    };
    const auto flag = [&](const char* key, bool& into) {
        const auto it = in.find(key);
        if (it != in.end() && it->is_boolean()) into = it->get<bool>();
    };
    text("root", s.root);
    text("args", s.args);
    flag("mic", s.mic);
    flag("launch_game", s.launch_game);
    flag("autostart", s.autostart);
    flag("api", s.api);
    text("api_url", s.api_url);
    text("api_model", s.api_model);
    text("api_extra", s.api_extra);
    flag("api_guessing", s.api_guessing);
    return s;
}

// What is wrong with the API half of some settings, in a sentence; empty when nothing is.
std::string api_trouble(const Settings& s) {
    if (!s.api) return {};
    if (s.api_url.empty()) return "Enter the API's address.";
    if (s.api_url.rfind("https://", 0) != 0 && s.api_url.rfind("http://", 0) != 0) {
        return "The API's address should start with https://";
    }
    if (s.api_model.empty()) return "Enter the name of the model to ask for.";
    if (!s.api_extra.empty()) {
        const json extra = json::parse(s.api_extra, nullptr, false);
        if (!extra.is_object()) return "The extra request JSON should be a JSON object, like {\"reasoning_effort\": \"low\"}.";
    }
    return {};
}

// The JSON object somewhere in what a program printed.
json object_in(const std::string& output) {
    const std::size_t from = output.find('{');
    const std::size_t to = output.rfind('}');
    if (from == std::string::npos || to == std::string::npos || to < from) return json();
    return json::parse(output.substr(from, to - from + 1), nullptr, false);
}

std::string last_line(const std::string& output) {
    std::string text = trimmed(output);
    const std::size_t newline = text.find_last_of('\n');
    if (newline != std::string::npos) text = trimmed(text.substr(newline + 1));
    if (text.size() > 300) text = text.substr(0, 300);
    return text;
}

fs::path powershell() {
    std::string root = environment("SystemRoot");
    if (root.empty()) root = "C:\\Windows";
    return path_from_utf8(root) / "System32" / "WindowsPowerShell" / "v1.0" / "powershell.exe";
}

bool missing_item(const CheckState& check, const std::string& name) {
    for (const json& item : check.items) {
        if (item.value("name", "") == name && item.value("state", "") == "missing") return true;
    }
    return false;
}

}  // namespace

HomeView home_view(const CheckState& check, const EngineState& engine, const TaskState& task,
                   const Settings& settings) {
    HomeView view;
    // A download in hand is the thing happening, whatever else is true.
    if (task.running && (task.kind == "update" || task.kind == "repair")) {
        view.tone = "busy";
        view.title = task.title;
        view.detail = task.detail;
        view.button = "Cancel";
        view.action = "cancelTask";
        return view;
    }

    const bool engine_idle = engine.state == "idle";
    if (engine.state == "failed") {
        view.tone = "problem";
        view.title = "Moonshine could not start";
        view.detail = engine.message;
        view.button = "Try again";
        view.action = "play";
        view.fix_label = "View logs";
        view.fix = "logs";
        return view;
    }
    if (engine_idle) {
        if (check.state == "checking") {
            view.tone = "busy";
            view.title = "Checking the installation";
            return view;
        }
        if (check.state == "failed") {
            view.tone = "problem";
            view.title = "Moonshine is not fully installed";
            view.detail = check.error;
            view.fix_label = "Reinstall";
            view.fix = "reinstall";
            return view;
        }
        if (!check.ready) {
            view.tone = "problem";
            if (missing_item(check, "game root")) {
                view.title = "Kingdom Come: Deliverance 2 was not found";
                view.detail = "Choose the folder the game is installed in.";
                view.fix_label = "Choose game folder";
                view.fix = "folder";
            } else if (settings.api && missing_item(check, "model")) {
                view.title = "The custom model API is not set up";
                view.detail = "Its address or model name is missing.";
                view.fix_label = "Open Settings";
                view.fix = "settings";
            } else {
                std::string names;
                for (const json& item : check.items) {
                    if (item.value("state", "") != "missing") continue;
                    if (!names.empty()) names += ", ";
                    names += item.value("name", "");
                }
                view.title = "Some of Moonshine's files are missing";
                view.detail = "Missing: " + names + ".";
                view.fix_label = "Repair files";
                view.fix = "repair";
            }
            return view;
        }
        view.tone = "idle";
        view.title = "Ready to play";
        view.detail = settings.launch_game ? "Starts Kingdom Come: Deliverance 2 with the people of Bohemia listening."
                                           : "Start the game yourself; Moonshine joins it.";
        view.button = "Play";
        view.action = "play";
        return view;
    }

    view.button = "Stop";
    view.action = "stop";
    if (engine.state == "stopping") {
        view.tone = "busy";
        view.title = "Stopping";
        view.button.clear();
        view.action.clear();
        return view;
    }
    if (engine.state == "restarting") {
        view.tone = "problem";
        view.title = "Starting again";
        view.detail = engine.message + " Anything that was cut off will need saying again.";
        view.fix_label = "View logs";
        view.fix = "logs";
        return view;
    }
    if (engine.phase == "ready" || engine.phase == "talking") {
        view.tone = "live";
        if (engine.phase == "talking") {
            view.title = engine.talking_to.empty() ? "In conversation" : "Talking with " + engine.talking_to;
        } else if (!engine.game_log) {
            view.title = "Waiting for the game";
            view.detail = "Moonshine is ready and joins in when the game is up.";
        } else {
            view.title = "Listening";
            view.detail = "Walk up to someone in the game and talk to them.";
        }
        // The last thing asked of the model failed: said, with where to go about it.
        if (!engine.error.empty()) {
            view.tone = "problem";
            view.detail = engine.error;
            view.fix_label = engine.remote_model ? "Open Settings" : "View logs";
            view.fix = engine.remote_model ? "settings" : "logs";
        }
        return view;
    }
    view.tone = "busy";
    view.title = "Starting";
    if (engine.phase == "loading-model") {
        view.detail = settings.api ? "Reaching the model API" : "Loading the language model";
    } else if (engine.phase == "warming") {
        view.detail = "Getting the voices ready";
    } else if (engine.phase == "failed") {
        view.detail = engine.error;
    }
    return view;
}

Controller::Parts Controller::Parts::system(const Paths& paths) {
    Parts parts;
    parts.paths = paths;
    parts.hooks = Supervisor::Hooks::system(paths);
    parts.secrets = system_secrets();
    parts.run = [](const Command& command, std::chrono::milliseconds limit, const std::function<bool()>& cancelled) {
        return gui::run(command, limit, cancelled);
    };
    parts.launch = [](const Command& command, std::string* trouble) { return gui::launch(command, trouble); };
    parts.open = [](const std::string& what) { return open_target(what); };
    parts.fetch_text = [](const std::string& url, std::string* trouble) { return gui::fetch_text(url, trouble); };
    parts.fetch_files = [](const Manifest& manifest, const FetchOptions& options) {
        return gui::fetch_files(manifest, options);
    };
    parts.uninstaller = [paths] { return installed_uninstaller(paths.home); };
    std::error_code ec;
    parts.temp = fs::temp_directory_path(ec);
    return parts;
}

Controller::Controller(Parts parts)
    : parts_(std::move(parts)),
      config_(parts_.paths.config),
      version_(installed_version(parts_.paths)),
      supervisor_(parts_.paths, [this] {
          Supervisor::Hooks hooks = parts_.hooks;
          const std::function<void()> also = hooks.changed;
          hooks.changed = [this, also] {
              if (also) also();
              changed();
          };
          return hooks;
      }()) {
    settings_ = config_.load();
    has_key_ = !parts_.secrets->get().empty();
}

Controller::~Controller() { shutdown(); }

void Controller::changed() {
    {
        std::lock_guard<std::mutex> held(lock_);
        revision_ += 1;
    }
    change_.notify_all();
}

void Controller::wait_for_change(std::chrono::milliseconds limit) {
    std::unique_lock<std::mutex> held(lock_);
    const std::uint64_t was = revision_;
    change_.wait_for(held, limit, [&] { return revision_ != was || closing_.load(); });
}

bool Controller::engine_active() const { return supervisor_.active(); }

void Controller::background(std::function<void()> work) {
    std::lock_guard<std::mutex> held(workers_lock_);
    if (closing_) return;
    // Collect the ones that have finished, so that an evening of checks is not an
    // evening's worth of threads waiting to be joined.
    for (auto it = workers_.begin(); it != workers_.end();) {
        if (it->done->load()) {
            it->thread.join();
            it = workers_.erase(it);
        } else {
            ++it;
        }
    }
    Worker worker;
    worker.done = std::make_shared<std::atomic<bool>>(false);
    worker.thread = std::thread([work = std::move(work), done = worker.done] {
        work();
        done->store(true);
    });
    workers_.push_back(std::move(worker));
}

void Controller::start(bool play) {
    // The installer the last update downloaded has been run (this may be the window it
    // opened at its end), so it goes. A download that was cut short stays, as a .part, to
    // be carried on from. Windows refuses while the installer is still closing; then it
    // goes on the start after.
    {
        std::error_code ec;
        std::vector<fs::path> spent;
        for (const auto& entry : fs::directory_iterator(parts_.temp / "moonshine-update", ec)) {
            if (entry.path().extension() == ".exe") spent.push_back(entry.path());
        }
        for (const fs::path& installer : spent) fs::remove(installer, ec);
    }
    const bool autostart = play || [this] {
        std::lock_guard<std::mutex> held(lock_);
        return settings_.autostart;
    }();
    run_check(autostart);
    check_update();
}

void Controller::shutdown() {
    closing_ = true;
    cancel_task_ = true;
    change_.notify_all();
    supervisor_.stop();
    std::vector<Worker> workers;
    {
        std::lock_guard<std::mutex> held(workers_lock_);
        workers.swap(workers_);
    }
    for (Worker& worker : workers) {
        if (worker.thread.joinable()) worker.thread.join();
    }
}

std::string Controller::game_root() {
    std::lock_guard<std::mutex> held(lock_);
    return check_.game_root.empty() ? settings_.root : check_.game_root;
}

// --- the check -----------------------------------------------------------------------------

void Controller::run_check(bool then_play) {
    std::uint64_t generation = 0;
    Settings settings;
    {
        std::lock_guard<std::mutex> held(lock_);
        generation = ++check_generation_;
        check_.state = "checking";
        settings = settings_;
    }
    changed();
    background([this, generation, settings, then_play] {
        CheckState found;
        found.state = "failed";
        std::error_code ec;
        if (!fs::exists(parts_.paths.engine, ec)) {
            found.error = "The Moonshine app itself (" + utf8_of(parts_.paths.engine.filename()) +
                          ") is missing from " + utf8_of(parts_.paths.home) + ".";
        } else {
            const std::string key = settings.api ? parts_.secrets->get() : std::string();
            const Ran ran = parts_.run(check_command(parts_.paths, settings, key), std::chrono::seconds(60),
                                       [this] { return closing_.load(); });
            const json report = object_in(ran.output);
            if (!ran.started) {
                found.error = "The Moonshine app could not be started: " + ran.trouble;
            } else if (!report.is_object() || !report.contains("items")) {
                found.error = "The Moonshine app could not say what it finds. " + last_line(ran.output);
            } else {
                found.state = "done";
                found.ready = report.value("ready", false);
                found.game_root = report.value("game_root", "");
                found.items = report["items"];
                // The engine reads an API with no address as no API at all, and would answer
                // with the local model. Somebody who chose an API did not choose that.
                if (settings.api && (settings.api_url.empty() || settings.api_model.empty())) {
                    found.ready = false;
                    for (json& item : found.items) {
                        if (item.value("name", "") != "model") continue;
                        item["state"] = "missing";
                        item["text"] = "a custom API is chosen in Settings, but its address or model name is empty";
                    }
                }
            }
        }
        bool play = false;
        {
            std::lock_guard<std::mutex> held(lock_);
            // A check started since has the newer settings; this one's answer is stale.
            if (generation != check_generation_) return;
            check_ = found;
            play = then_play && found.state == "done" && found.ready;
        }
        if (play) begin_play();
        changed();
    });
}

void Controller::begin_play() {
    Settings settings;
    {
        std::lock_guard<std::mutex> held(lock_);
        settings = settings_;
    }
    supervisor_.play(settings, settings.api ? parts_.secrets->get() : std::string());
}

// --- settings ------------------------------------------------------------------------------

json Controller::save_settings(const json& args) {
    Settings before;
    {
        std::lock_guard<std::mutex> held(lock_);
        before = settings_;
    }
    const Settings next = settings_from(args, before);
    if (const std::string trouble = api_trouble(next); !trouble.empty()) return {{"ok", false}, {"error", trouble}};

    // The key: absent from the call means leave what is kept; a string replaces it, and an
    // empty one removes it.
    bool key_changed = false;
    const auto key = args.find("key");
    if (key != args.end() && key->is_string()) {
        const std::string wanted = trimmed(key->get<std::string>());
        if (wanted != parts_.secrets->get()) {
            std::string trouble;
            if (!parts_.secrets->set(wanted, &trouble)) return {{"ok", false}, {"error", "The key was not saved: " + trouble}};
            key_changed = true;
        }
    }
    std::string trouble;
    if (!config_.save(next, &trouble)) return {{"ok", false}, {"error", "The settings were not saved: " + trouble}};

    // Everything but "start when Moonshine opens" is something the engine is started with.
    Settings compared = next;
    compared.autostart = before.autostart;
    const bool engine_changed = key_changed || compared != before;
    {
        std::lock_guard<std::mutex> held(lock_);
        settings_ = next;
        has_key_ = !parts_.secrets->get().empty();
    }
    const bool restart = engine_changed && supervisor_.active();
    if (restart) {
        background([this] {
            supervisor_.note("settings changed; starting the app again with them");
            supervisor_.stop();
            run_check(true);
        });
    } else if (engine_changed) {
        run_check(false);
    }
    changed();
    return {{"ok", true}, {"restarted", restart}};
}

json Controller::set_root(const std::string& root) {
    return save_settings({{"root", root}});
}

json Controller::test_api(const json& args) {
    Settings settings;
    {
        std::lock_guard<std::mutex> held(lock_);
        settings = settings_;
    }
    settings = settings_from(args, settings);
    settings.api = true;
    if (const std::string trouble = api_trouble(settings); !trouble.empty()) return {{"ok", false}, {"error", trouble}};
    std::error_code ec;
    if (!fs::exists(parts_.paths.engine, ec)) return {{"ok", false}, {"error", "The Moonshine app is missing, so nothing can be asked."}};

    // The key in the form if one was typed, otherwise the one that is kept.
    std::string key = parts_.secrets->get();
    const auto typed = args.find("key");
    if (typed != args.end() && typed->is_string()) key = trimmed(typed->get<std::string>());

    Command command;
    command.exe = parts_.paths.engine;
    command.cwd = parts_.paths.home;
    command.args = {"--api-test", "--json", "--api-url", settings.api_url, "--api-model", settings.api_model};
    command.env = {{"MOONSHINE_API_KEY", key}, {"MOONSHINE_API_EXTRA", settings.api_extra}};
    const Ran ran = parts_.run(command, std::chrono::seconds(150), [this] { return closing_.load(); });
    if (!ran.started) return {{"ok", false}, {"error", "The Moonshine app could not be started: " + ran.trouble}};
    json answer = object_in(ran.output);
    if (!answer.is_object() || !answer.contains("ok")) {
        return {{"ok", false},
                {"error", ran.finished ? "The test gave no answer. " + last_line(ran.output)
                                       : "The API did not answer in time."}};
    }
    return answer;
}

// --- logs ----------------------------------------------------------------------------------

json Controller::read_log(const json& args) {
    const std::string which = args.value("which", "app");
    const fs::path file = which == "launcher" ? parts_.paths.launcher_log : parts_.paths.engine_log;
    std::error_code ec;
    const std::uintmax_t found = fs::file_size(file, ec);
    const std::int64_t size = ec ? 0 : static_cast<std::int64_t>(found);
    std::int64_t from = args.value("from", static_cast<std::int64_t>(-1));
    constexpr std::int64_t kFirst = 96 * 1024;
    constexpr std::int64_t kMost = 256 * 1024;
    bool reset = false;
    bool skip_line = false;
    if (from < 0 || from > size) {
        // The first read, or the file is a new one shorter than what was read of the old
        // (the engine starts its log again each run): the end of it, from a whole line.
        reset = true;
        from = size > kFirst ? size - kFirst : 0;
        skip_line = from > 0;
    }
    std::string text;
    if (size > from) {
        std::ifstream in(file, std::ios::binary);
        in.seekg(from);
        text.resize(static_cast<std::size_t>(std::min(kMost, size - from)));
        in.read(text.data(), static_cast<std::streamsize>(text.size()));
        text.resize(static_cast<std::size_t>(std::max<std::streamsize>(0, in.gcount())));
    }
    std::int64_t next = from + static_cast<std::int64_t>(text.size());
    // Not half a character at the end: the rest of it comes with the next read.
    std::size_t tail = 0;
    while (tail < 3 && tail < text.size() && (static_cast<unsigned char>(text[text.size() - 1 - tail]) & 0xC0) == 0x80) {
        ++tail;
    }
    if (tail < text.size()) {
        const unsigned char lead = static_cast<unsigned char>(text[text.size() - 1 - tail]);
        const std::size_t wants = lead >= 0xF0 ? 3 : lead >= 0xE0 ? 2 : lead >= 0xC0 ? 1 : 0;
        if (wants > tail) {
            text.resize(text.size() - tail - 1);
            next -= static_cast<std::int64_t>(tail + 1);
        }
    }
    if (skip_line) {
        const std::size_t newline = text.find('\n');
        text = newline == std::string::npos ? std::string() : text.substr(newline + 1);
    }
    return {{"text", text}, {"next", next}, {"reset", reset}, {"file", utf8_of(file)}};
}

json Controller::open(const json& args) {
    const std::string what = args.value("what", "");
    const Paths& paths = parts_.paths;
    const auto first_of = [](std::initializer_list<fs::path> candidates) {
        std::error_code ec;
        for (const fs::path& candidate : candidates) {
            if (fs::exists(candidate, ec)) return candidate;
        }
        return fs::path();
    };
    fs::path target;
    if (what == "logs") {
        target = paths.logs;
    } else if (what == "home") {
        target = paths.home;
    } else if (what == "readme") {
        target = first_of({paths.home / "README.txt", paths.home / "README.md"});
    } else if (what == "licences") {
        target = first_of({paths.home / "LICENSES" / "README.txt", paths.home / "installer" / "LICENSES" / "README.txt"});
    } else if (what == "task") {
        std::lock_guard<std::mutex> held(lock_);
        target = path_from_utf8(task_.path).parent_path();
    } else if (what == "game") {
        target = path_from_utf8(game_root());
    }
    if (target.empty()) return {{"ok", false}, {"error", "There is nothing to open."}};
    return {{"ok", parts_.open(utf8_of(target))}};
}

// --- tasks ---------------------------------------------------------------------------------

bool Controller::begin_task(const std::string& kind, const std::string& title) {
    {
        std::lock_guard<std::mutex> held(lock_);
        if (task_.running) return false;
        task_ = TaskState{};
        task_.kind = kind;
        task_.title = title;
        task_.running = true;
        cancel_task_ = false;
    }
    changed();
    return true;
}

void Controller::end_task(const std::string& error, const std::string& result) {
    {
        std::lock_guard<std::mutex> held(lock_);
        task_.running = false;
        task_.error = error;
        task_.result = result;
        task_.detail.clear();
    }
    changed();
}

void Controller::fetch_log(const std::string& line) {
    std::ofstream out(parts_.paths.logs / "fetch.log", std::ios::binary | std::ios::app);
    const std::time_t now = parts_.hooks.wall ? parts_.hooks.wall() : std::time(nullptr);
    std::tm parts{};
#ifdef _WIN32
    localtime_s(&parts, &now);
#else
    localtime_r(&now, &parts);
#endif
    char stamp[32];
    std::strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", &parts);
    out << stamp << "  " << line << "\n";
}

FetchResult Controller::fetch_with_progress(const Manifest& manifest, const fs::path& dest, const std::string& doing) {
    FetchOptions options;
    options.dest = dest;
    options.cancelled = [this] { return cancel_task_.load() || closing_.load(); };
    options.log = [this](const std::string& line) { fetch_log(line); };
    options.progress = [this, doing](const Progress& progress) {
        std::string detail;
        const std::string name = leaf(progress.file);
        if (progress.phase == "checking") {
            detail = "Checking " + name;
        } else if (progress.phase == "waiting") {
            detail = "Trying " + name + " again shortly (" + progress.note + ")";
        } else {
            detail = doing + ": " + megabytes(progress.done) + " of " + megabytes(progress.total) + " MB";
            if (progress.bytes_per_second > 0) {
                char rate[32];
                std::snprintf(rate, sizeof rate, ", %.1f MB/s", progress.bytes_per_second / (1024.0 * 1024.0));
                detail += rate;
            }
        }
        {
            std::lock_guard<std::mutex> held(lock_);
            task_.detail = detail;
            task_.done = progress.done;
            task_.total = progress.total;
        }
        changed();
    };
    return parts_.fetch_files(manifest, options);
}

std::optional<Manifest> Controller::published(std::string* trouble) {
    const std::optional<Manifest> installed = read_manifest(parts_.paths.home / "manifest.json", trouble);
    if (!installed) {
        *trouble = "This installation has no manifest, so it cannot look for updates. Install the newest version over it.";
        return std::nullopt;
    }
    if (installed->latest.empty()) {
        *trouble = "This installation does not say where updates are published.";
        return std::nullopt;
    }
    std::string why;
    const std::optional<std::string> text = parts_.fetch_text(installed->latest, &why);
    if (!text) {
        *trouble = "Could not look for updates: " + why;
        return std::nullopt;
    }
    std::optional<Manifest> latest = parse_manifest(*text, &why);
    if (!latest) *trouble = "The published update could not be read: " + why;
    return latest;
}

void Controller::check_update() {
    {
        std::lock_guard<std::mutex> held(lock_);
        if (update_.state == "checking") return;
        update_ = UpdateState{};
        update_.state = "checking";
    }
    changed();
    background([this] {
        std::string trouble;
        const std::optional<Manifest> latest = published(&trouble);
        {
            std::lock_guard<std::mutex> held(lock_);
            if (!latest) {
                update_.state = "failed";
                update_.error = trouble;
            } else {
                update_.latest = latest->version;
                update_.state = compare_versions(latest->version, version_) > 0 ? "available" : "current";
                latest_ = latest;
            }
        }
        changed();
    });
}

void Controller::repair() {
    if (!begin_task("repair", "Repairing files")) return;
    background([this] {
        std::string trouble;
        const std::optional<Manifest> manifest = read_manifest(parts_.paths.home / "manifest.json", &trouble);
        if (!manifest) {
            end_task("This installation has no list of its files to check against. Install the newest version over it.", "");
            return;
        }
        // A file the engine has open cannot be replaced under it.
        supervisor_.stop();
        const FetchResult result = fetch_with_progress(*manifest, parts_.paths.home, "Downloading");
        if (result.cancelled) {
            end_task("", "Stopped. What was downloaded is kept, and Repair carries on from there.");
        } else if (!result.ok()) {
            // The trouble is the last file's; with several, how many says more than which.
            const std::string which = result.bad.size() > 1
                                          ? std::to_string(result.bad.size()) + " files could not be downloaded (the last, "
                                          : "A file could not be downloaded (";
            end_task(which + result.trouble + "). Try again; what was downloaded is kept.", "");
        } else {
            // The mod in the game's folder, when the installer put one there, is made the
            // one this installation carries. Somebody subscribed on the Workshop has no
            // such folder and is left alone.
            const fs::path ours = parts_.paths.home / "mod" / "moonshine_names";
            const std::string root = game_root();
            std::error_code ec;
            if (!root.empty() && fs::exists(ours / "mod.manifest", ec)) {
                const fs::path theirs = path_from_utf8(root) / "mods" / "moonshine_names";
                if (fs::is_directory(theirs, ec)) {
                    std::string why;
                    fetch_log(mirror_folder(ours, theirs, &why) ? "mod copied to " + utf8_of(theirs)
                                                                : "the mod was not copied: " + why);
                }
            }
            end_task("", result.fetched == 0 ? "Every file is in place and correct."
                                             : std::to_string(result.fetched) +
                                                   (result.fetched == 1 ? " file was" : " files were") +
                                                   " downloaded again. Everything is now in place.");
        }
        run_check(false);
    });
}

void Controller::apply_update(bool reinstall) {
    // The same version is a repair, as it was for Moonshine.ps1 -Update.
    bool same = false;
    {
        std::lock_guard<std::mutex> held(lock_);
        same = !reinstall && update_.state == "current";
    }
    if (same) {
        repair();
        return;
    }
    if (!begin_task("update", reinstall ? "Reinstalling Moonshine" : "Updating Moonshine")) return;
    background([this, reinstall] {
        std::string trouble;
        std::optional<Manifest> latest;
        {
            std::lock_guard<std::mutex> held(lock_);
            latest = latest_;
        }
        if (!latest) latest = published(&trouble);
        if (!latest) {
            end_task(trouble, "");
            return;
        }
        if (!reinstall && compare_versions(latest->version, version_) <= 0) {
            end_task("", "This is the newest version.");
            return;
        }
        const ManifestFile& setup = latest->setup;
        std::string name = leaf(setup.url);
        if (const std::size_t query = name.find_first_of("?#"); query != std::string::npos) name.resize(query);
        const bool plain = !name.empty() && std::all_of(name.begin(), name.end(), [](unsigned char c) {
            return std::isalnum(c) || c == '.' || c == '-' || c == '_';
        });
        if (setup.url.empty() || setup.sha256.size() != 64 || !plain || name.size() < 5 ||
            name.compare(name.size() - 4, 4, ".exe") != 0) {
            end_task("The published version does not come with an installer this can run. Download it from the Moonshine page instead.", "");
            return;
        }
        // Through the same fetch as everything else, from a manifest of one file, so that
        // it gets the same resume, retries and hash check.
        const fs::path folder = parts_.temp / "moonshine-update";
        std::error_code ec;
        fs::create_directories(folder, ec);
        Manifest one;
        one.version = latest->version;
        one.files.push_back({name, setup.bytes, setup.sha256, setup.url});
        const FetchResult result = fetch_with_progress(one, folder, "Downloading Moonshine " + latest->version);
        if (result.cancelled) {
            end_task("", "Stopped. Nothing was changed.");
            return;
        }
        if (!result.ok()) {
            end_task("The installer did not download (" + result.trouble + "). Try again later.", "");
            return;
        }
        {
            std::lock_guard<std::mutex> held(lock_);
            task_.detail = "Starting the installer";
        }
        changed();
        supervisor_.stop();
        // Into this folder, for this user, without its questions; it keeps every model and
        // data file that has not changed, downloads the rest, and opens Moonshine again
        // when it is done.
        Command installer;
        installer.exe = folder / path_from_utf8(name);
        // Not /SUPPRESSMSGBOXES: a download that fails should ask whether to try again,
        // not give up on a gigabyte in silence.
        installer.args = {"/SILENT", "/SP-", "/NORESTART", "/DIR=" + utf8_of(parts_.paths.home), "/RELAUNCH=1"};
        if (parts_.release_instance) parts_.release_instance();
        if (!parts_.launch(installer, &trouble)) {
            end_task("The installer would not start: " + trouble, "");
            return;
        }
        supervisor_.note("started the installer for Moonshine " + latest->version + "; closing");
        end_task("", "The installer is running. Moonshine opens again when it has finished.");
        if (parts_.quit) parts_.quit();
    });
}

void Controller::collect_logs() {
    if (!begin_task("collect", "Collecting logs")) return;
    background([this] {
        const fs::path script = parts_.paths.home / "Collect-Logs.ps1";
        std::error_code ec;
        if (!fs::exists(script, ec)) {
            end_task("The log collector (Collect-Logs.ps1) is not in this installation. The logs themselves are in the logs folder.", "");
            return;
        }
        Command command;
        command.exe = powershell();
        command.cwd = parts_.paths.home;
        command.args = {"-NoProfile", "-ExecutionPolicy", "Bypass", "-File", utf8_of(script)};
        // Without this it waits at the end for an Enter nobody can press.
        command.env = {{"MOONSHINE_NO_PAUSE", "1"}};
        const Ran ran = parts_.run(command, std::chrono::seconds(300), [this] { return closing_.load(); });
        std::string written;
        std::istringstream lines(ran.output);
        for (std::string line; std::getline(lines, line);) {
            line = trimmed(line);
            if (line.rfind("written: ", 0) == 0) written = trimmed(line.substr(9));
        }
        if (written.empty()) {
            end_task("The logs could not be collected. " + (ran.started ? last_line(ran.output) : ran.trouble), "");
            return;
        }
        {
            std::lock_guard<std::mutex> held(lock_);
            task_.path = written;
        }
        end_task("", "Saved to your desktop as " + leaf(written) + ". Send that file with a sentence or two about what you were doing.");
    });
}

json Controller::uninstall(const json& args) {
    const fs::path uninstaller = parts_.uninstaller ? parts_.uninstaller() : fs::path();
    if (uninstaller.empty()) {
        return {{"ok", false},
                {"error", "This copy of Moonshine was not put here by its installer, so there is no uninstaller to run. Delete its folder to remove it."}};
    }
    const bool remove_mod = args.value("remove_mod", false);
    background([this, uninstaller, remove_mod] {
        supervisor_.stop();
        // The key goes too: it was kept for this app and nothing else knows it is there.
        parts_.secrets->set("", nullptr);
        if (remove_mod) {
            const std::string root = game_root();
            const fs::path mod = path_from_utf8(root) / "mods" / "moonshine_names";
            std::error_code ec;
            if (!root.empty() && fs::is_directory(mod, ec)) {
                fs::remove_all(mod, ec);
                supervisor_.note(ec ? "the mod could not be removed from " + utf8_of(mod)
                                    : "removed the mod from " + utf8_of(mod));
            }
        }
        Command command;
        command.exe = uninstaller;
        // It has been confirmed in the window; the uninstaller is not to ask again. What
        // it did is written down outside the folder it is removing, for the day it does
        // not remove it.
        command.args = {"/SILENT", "/LOG=" + utf8_of(parts_.temp / "moonshine-uninstall.log")};
        if (parts_.release_instance) parts_.release_instance();
        std::string trouble;
        if (!parts_.launch(command, &trouble)) {
            supervisor_.note("the uninstaller would not start: " + trouble);
            if (begin_task("uninstall", "Removing Moonshine")) end_task("The uninstaller would not start: " + trouble, "");
            return;
        }
        if (parts_.quit) parts_.quit();
    });
    return {{"ok", true}};
}

// --- the surface ---------------------------------------------------------------------------

bool Controller::slow(const std::string& method) { return method == "testApi"; }

json Controller::call(const std::string& method, const json& args) {
    const json ok = {{"ok", true}};
    if (method == "getState") return state();
    if (method == "play") {
        run_check(true);
        return ok;
    }
    if (method == "stop") {
        background([this] {
            supervisor_.stop();
            changed();
        });
        return ok;
    }
    if (method == "recheck") {
        run_check(false);
        return ok;
    }
    if (method == "readLog") return read_log(args);
    if (method == "saveSettings") return save_settings(args);
    if (method == "testApi") return test_api(args);
    if (method == "open") return open(args);
    if (method == "checkUpdate") {
        check_update();
        return ok;
    }
    if (method == "applyUpdate") {
        apply_update(args.value("reinstall", false));
        return ok;
    }
    if (method == "repair") {
        repair();
        return ok;
    }
    if (method == "collectLogs") {
        collect_logs();
        return ok;
    }
    if (method == "cancelTask") {
        cancel_task_ = true;
        return ok;
    }
    if (method == "dismissTask") {
        {
            std::lock_guard<std::mutex> held(lock_);
            if (!task_.running) task_ = TaskState{};
        }
        changed();
        return ok;
    }
    if (method == "uninstall") return uninstall(args);
    return {{"ok", false}, {"error", "unknown call: " + method}};
}

json Controller::state() {
    const EngineState engine = supervisor_.state();
    std::lock_guard<std::mutex> held(lock_);
    const HomeView view = home_view(check_, engine, task_, settings_);
    json out;
    out["version"] = version_;
    out["folder"] = utf8_of(parts_.paths.home);
#ifdef _WIN32
    out["windows"] = true;
#else
    out["windows"] = false;
#endif
    out["home"] = {{"tone", view.tone},     {"title", view.title},   {"detail", view.detail},
                   {"button", view.button}, {"action", view.action}, {"fix_label", view.fix_label},
                   {"fix", view.fix}};
    out["engine"] = {{"state", engine.state},       {"message", engine.message}, {"restarts", engine.restarts},
                     {"phase", engine.phase},       {"talking_to", engine.talking_to}, {"model", engine.model},
                     {"remote_model", engine.remote_model}, {"voice", engine.voice},   {"ear", engine.ear},
                     {"game_log", engine.game_log}, {"error", engine.error}};
    out["check"] = {{"state", check_.state}, {"ready", check_.ready}, {"game_root", check_.game_root},
                    {"error", check_.error}, {"items", check_.items}};
    out["settings"] = settings_json(settings_);
    out["has_key"] = has_key_;
    out["task"] = {{"kind", task_.kind},   {"running", task_.running}, {"title", task_.title},
                   {"detail", task_.detail}, {"done", task_.done},     {"total", task_.total},
                   {"error", task_.error}, {"result", task_.result},   {"path", task_.path}};
    out["update"] = {{"state", update_.state}, {"latest", update_.latest}, {"error", update_.error}};
    return out;
}

}  // namespace moonshine::gui
