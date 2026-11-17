// What the window can ask for and what it is told (gui/bridge.h), with no window.
//
// The page draws whatever state it is given, so the behaviour a player sees is decided
// here: which sentence is on the home screen, what Play starts, where a key goes when it
// is saved, what an update runs. Each of those is driven through call() as the page
// drives it, against a machine that is a record of what was asked of it.

#include "doctest.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "bridge.h"
#include "json.hpp"

using namespace moonshine::gui;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

fs::path fresh_install(const char* name) {
    const fs::path dir = fs::temp_directory_path() / "moonshine-gui-bridge-tests" / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

void put(const fs::path& file, const std::string& text = "x") {
    fs::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary);
    out << text;
}

std::string get(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool has_arg(const Command& command, const std::string& arg) {
    return std::find(command.args.begin(), command.args.end(), arg) != command.args.end();
}

std::string arg_after(const Command& command, const std::string& flag) {
    for (std::size_t i = 0; i + 1 < command.args.size(); ++i) {
        if (command.args[i] == flag) return command.args[i + 1];
    }
    return {};
}

std::string env_of(const Command& command, const std::string& name, const std::string& otherwise = "(unset)") {
    for (const auto& [key, value] : command.env) {
        if (key == name) return value;
    }
    return otherwise;
}

bool eventually(const std::function<bool()>& reached) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        if (reached()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return reached();
}

json where_report(bool game = true, bool model = true) {
    json items = json::array();
    items.push_back({{"name", "game root"}, {"state", game ? "ok" : "missing"}, {"text", game ? "C:/Games/KCD2" : "not found"}});
    items.push_back({{"name", "model"}, {"state", model ? "ok" : "missing"}, {"text", "models/llm/model.gguf"}});
    items.push_back({{"name", "voices"}, {"state", "ok"}, {"text", "37 voices"}});
    items.push_back({{"name", "phrases"}, {"state", "optional"}, {"text", "none"}});
    return {{"ready", game && model}, {"game_root", game ? "C:/Games/KCD2" : ""}, {"items", items}};
}

// An installation with nothing real in it, and a record of what the app did to it.
struct Machine {
    Paths paths;
    std::mutex lock;
    // What the engine answers when asked what it finds, and when asked to test an API.
    json where = where_report();
    std::string api_answer = R"({"ok":true,"model":"m","reply":"ready","seconds":0.4,"reasons":false})";
    std::string collect_output = "written: C:\\Users\\henry\\Desktop\\Moonshine-logs-1.zip\nSend that file along.\n";
    // What the download server publishes, and whether it can be reached.
    std::string published;
    bool online = true;
    FetchResult fetch_result;
    // A download that goes on until this is cleared, or until it is told to stop.
    std::atomic<bool> hold_fetch{false};
    fs::path uninstaller;

    std::vector<Command> ran;
    std::vector<Command> engines;
    std::vector<Command> launched;
    std::vector<std::string> opened;
    std::vector<std::pair<Manifest, fs::path>> fetched;
    std::vector<std::string> order;
    int quits = 0;
    MemorySecrets* secrets = nullptr;

    // The engine: runs until told /quit.
    class Engine : public Child {
    public:
        explicit Engine(Machine& machine) : machine_(machine) {}
        std::optional<int> wait_for(std::chrono::milliseconds) override {
            if (quit_) return 0;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return std::nullopt;
        }
        bool say(std::string_view line) override {
            if (line == "/quit") {
                quit_ = true;
                std::lock_guard<std::mutex> held(machine_.lock);
                machine_.order.push_back("engine stopped");
            }
            return true;
        }
        void hang_up() override {}
        void kill() override { quit_ = true; }
        std::string take_output() override { return {}; }

    private:
        Machine& machine_;
        std::atomic<bool> quit_{false};
    };

    explicit Machine(const char* name) : paths(Paths::at(fresh_install(name))) {
        put(paths.engine, "not really a program");
        put(paths.home / "VERSION.txt", "0.9.100");
    }

    Controller::Parts parts() {
        Controller::Parts parts;
        parts.paths = paths;
        parts.hooks.spawn = [this](const Command& command, std::string*) -> std::unique_ptr<Child> {
            std::lock_guard<std::mutex> held(lock);
            engines.push_back(command);
            put(paths.status, R"({"phase":"ready","voice":true,"ear":true,"game_log":true})");
            return std::make_unique<Engine>(*this);
        };
        parts.hooks.game_running = [] { return false; };
        parts.hooks.start_game = [] {};
        parts.hooks.stop_strays = [] { return true; };
        parts.hooks.seconds = [] {
            return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        };
        parts.hooks.wall = [] { return static_cast<std::time_t>(1794000000); };
        parts.hooks.wait = [](double) {};
        auto kept = std::make_unique<MemorySecrets>();
        secrets = kept.get();
        parts.secrets = std::move(kept);
        parts.run = [this](const Command& command, std::chrono::milliseconds, const std::function<bool()>&) {
            std::lock_guard<std::mutex> held(lock);
            ran.push_back(command);
            Ran result;
            result.started = true;
            result.finished = true;
            result.code = 0;
            if (has_arg(command, "--where")) {
                result.output = "log: somewhere\n" + where.dump(2) + "\n";
            } else if (has_arg(command, "--api-test")) {
                result.output = api_answer;
            } else {
                result.output = collect_output;
            }
            return result;
        };
        parts.launch = [this](const Command& command, std::string*) {
            std::lock_guard<std::mutex> held(lock);
            launched.push_back(command);
            order.push_back("launched " + command.exe.filename().string());
            return true;
        };
        parts.open = [this](const std::string& what) {
            std::lock_guard<std::mutex> held(lock);
            opened.push_back(what);
            return true;
        };
        parts.fetch_text = [this](const std::string&, std::string* trouble) -> std::optional<std::string> {
            if (!online) {
                *trouble = "could not reach the server";
                return std::nullopt;
            }
            return published;
        };
        parts.fetch_files = [this](const Manifest& manifest, const FetchOptions& options) {
            while (hold_fetch) {
                if (options.cancelled && options.cancelled()) {
                    FetchResult stopped;
                    stopped.cancelled = true;
                    return stopped;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            std::lock_guard<std::mutex> held(lock);
            fetched.emplace_back(manifest, options.dest);
            order.push_back("fetched");
            if (options.progress) {
                Progress progress;
                progress.phase = "downloading";
                progress.file = manifest.files.empty() ? "" : manifest.files[0].path;
                progress.done = 50;
                progress.total = 100;
                options.progress(progress);
            }
            return fetch_result;
        };
        parts.uninstaller = [this] { return uninstaller; };
        parts.temp = paths.home / "temp";
        parts.quit = [this] {
            std::lock_guard<std::mutex> held(lock);
            quits += 1;
            order.push_back("quit");
        };
        parts.release_instance = [this] {
            std::lock_guard<std::mutex> held(lock);
            order.push_back("released");
        };
        return parts;
    }

    int engine_count() {
        std::lock_guard<std::mutex> held(lock);
        return static_cast<int>(engines.size());
    }
    Command last_engine() {
        std::lock_guard<std::mutex> held(lock);
        return engines.back();
    }
    Command last_run() {
        std::lock_guard<std::mutex> held(lock);
        return ran.back();
    }

    void install_manifest(const std::string& version = "0.9.100") {
        const json manifest = {{"version", version},
                               {"latest", "https://download.example/latest/manifest.json"},
                               {"files",
                                {{{"path", "models/llm/model.gguf"},
                                  {"bytes", 10},
                                  {"sha256", std::string(64, 'a')},
                                  {"url", "https://download.example/model.gguf"}}}}};
        put(paths.home / "manifest.json", manifest.dump());
    }
    void publish(const std::string& version, bool with_setup = true) {
        json manifest = {{"version", version}, {"files", json::array()}};
        if (with_setup) {
            manifest["setup"] = {{"url", "https://download.example/Moonshine-" + version + "-setup.exe"},
                                 {"bytes", 1000},
                                 {"sha256", std::string(64, 'b')}};
        }
        published = manifest.dump();
    }
};

bool checked(Controller& controller) {
    return eventually([&] { return controller.state()["check"]["state"] != "checking"; });
}

bool task_over(Controller& controller) {
    return eventually([&] {
        const json task = controller.state()["task"];
        return !task["running"].get<bool>() && !task["kind"].get<std::string>().empty();
    });
}

EngineState engine(const char* state, const char* phase = "") {
    EngineState e;
    e.state = state;
    e.phase = phase;
    return e;
}

CheckState check_done(const json& report) {
    CheckState check;
    check.state = "done";
    check.ready = report["ready"].get<bool>();
    check.game_root = report["game_root"].get<std::string>();
    check.items = report["items"];
    return check;
}

}  // namespace

// --- the home screen's words -----------------------------------------------------------------------

TEST_CASE("home: nothing running, in the order the things that can be wrong are said") {
    const Settings settings;
    const TaskState no_task;

    CheckState checking;
    HomeView view = home_view(checking, engine("idle"), no_task, settings);
    CHECK(view.tone == "busy");
    CHECK(view.title == "Checking the installation");
    CHECK(view.button.empty());

    CheckState broken;
    broken.state = "failed";
    broken.error = "The Moonshine app itself (moonshine_app.exe) is missing.";
    view = home_view(broken, engine("idle"), no_task, settings);
    CHECK(view.tone == "problem");
    CHECK(view.title == "Moonshine is not fully installed");
    CHECK(view.detail == broken.error);
    CHECK(view.fix == "reinstall");
    CHECK(view.button.empty());

    view = home_view(check_done(where_report(false, false)), engine("idle"), no_task, settings);
    CHECK(view.title == "Kingdom Come: Deliverance 2 was not found");
    CHECK(view.fix == "folder");
    CHECK(view.fix_label == "Choose game folder");

    view = home_view(check_done(where_report(true, false)), engine("idle"), no_task, settings);
    CHECK(view.title == "Some of Moonshine's files are missing");
    CHECK(view.detail == "Missing: model.");
    CHECK(view.fix == "repair");
    CHECK(view.fix_label == "Repair files");

    Settings api;
    api.api = true;
    view = home_view(check_done(where_report(true, false)), engine("idle"), no_task, api);
    CHECK(view.title == "The custom model API is not set up");
    CHECK(view.fix == "settings");

    view = home_view(check_done(where_report()), engine("idle"), no_task, settings);
    CHECK(view.tone == "idle");
    CHECK(view.title == "Ready to play");
    CHECK(view.button == "Play");
    CHECK(view.action == "play");
    CHECK(view.fix.empty());

    Settings own_start;
    own_start.launch_game = false;
    view = home_view(check_done(where_report()), engine("idle"), no_task, own_start);
    CHECK(view.detail == "Start the game yourself; Moonshine joins it.");
}

TEST_CASE("home: the engine coming up, listening, talking and going") {
    const Settings settings;
    const TaskState no_task;
    const CheckState check = check_done(where_report());

    HomeView view = home_view(check, engine("starting"), no_task, settings);
    CHECK(view.tone == "busy");
    CHECK(view.title == "Starting");
    CHECK(view.button == "Stop");

    view = home_view(check, engine("running", "loading-model"), no_task, settings);
    CHECK(view.detail == "Loading the language model");
    Settings api;
    api.api = true;
    CHECK(home_view(check, engine("running", "loading-model"), no_task, api).detail == "Reaching the model API");
    CHECK(home_view(check, engine("running", "warming"), no_task, settings).detail == "Getting the voices ready");

    EngineState ready = engine("running", "ready");
    view = home_view(check, ready, no_task, settings);
    CHECK(view.tone == "live");
    CHECK(view.title == "Waiting for the game");
    ready.game_log = true;
    view = home_view(check, ready, no_task, settings);
    CHECK(view.title == "Listening");
    CHECK(view.button == "Stop");
    CHECK(view.action == "stop");

    EngineState talking = engine("running", "talking");
    talking.talking_to = "Innkeeper Betty";
    CHECK(home_view(check, talking, no_task, settings).title == "Talking with Innkeeper Betty");
    talking.talking_to.clear();
    CHECK(home_view(check, talking, no_task, settings).title == "In conversation");

    view = home_view(check, engine("stopping", "ready"), no_task, settings);
    CHECK(view.title == "Stopping");
    CHECK(view.button.empty());
}

TEST_CASE("home: what went wrong while running is a sentence and a way out") {
    const Settings settings;
    const TaskState no_task;
    const CheckState check = check_done(where_report());

    EngineState restarting = engine("restarting");
    restarting.message = "Moonshine stopped unexpectedly and is starting again.";
    HomeView view = home_view(check, restarting, no_task, settings);
    CHECK(view.tone == "problem");
    CHECK(view.detail == "Moonshine stopped unexpectedly and is starting again. Anything that was cut off will need saying again.");
    CHECK(view.fix == "logs");
    CHECK(view.button == "Stop");

    EngineState failed = engine("failed");
    failed.message = "Moonshine keeps stopping as soon as it starts.";
    view = home_view(check, failed, no_task, settings);
    CHECK(view.title == "Moonshine could not start");
    CHECK(view.detail == failed.message);
    CHECK(view.button == "Try again");
    CHECK(view.action == "play");
    CHECK(view.fix == "logs");

    // The API stopped answering: the engine is up, and says so in its status.
    EngineState deaf = engine("running", "ready");
    deaf.game_log = true;
    deaf.remote_model = true;
    deaf.error = "The API did not accept the key. Check it in Settings.";
    view = home_view(check, deaf, no_task, settings);
    CHECK(view.tone == "problem");
    CHECK(view.title == "Listening");
    CHECK(view.detail == deaf.error);
    CHECK(view.fix == "settings");
    deaf.remote_model = false;
    CHECK(home_view(check, deaf, no_task, settings).fix == "logs");
}

TEST_CASE("home: a download in hand is what is said, whatever else is true") {
    TaskState task;
    task.kind = "repair";
    task.running = true;
    task.title = "Repairing files";
    task.detail = "Downloading: 812 of 2,381 MB";
    const HomeView view = home_view(check_done(where_report(true, false)), engine("idle"), task, Settings{});
    CHECK(view.tone == "busy");
    CHECK(view.title == "Repairing files");
    CHECK(view.detail == "Downloading: 812 of 2,381 MB");
    CHECK(view.button == "Cancel");
    CHECK(view.action == "cancelTask");
    // Collecting logs is not that: it is said on the logs screen, and Play stays Play.
    task.kind = "collect";
    CHECK(home_view(check_done(where_report()), engine("idle"), task, Settings{}).button == "Play");
}

// --- the check, and Play ---------------------------------------------------------------------------

TEST_CASE("bridge: opening checks the installation, and Play starts what was checked") {
    Machine machine("play");
    put(machine.paths.config, "root = D:\\Games\\KCD2\r\n");
    Controller controller(machine.parts());
    controller.start();
    REQUIRE(checked(controller));

    json state = controller.state();
    CHECK(state["version"] == "0.9.100");
    CHECK(state["check"]["ready"] == true);
    CHECK(state["check"]["game_root"] == "C:/Games/KCD2");
    CHECK(state["check"]["items"].size() == 4);
    CHECK(state["home"]["title"] == "Ready to play");
    CHECK(state["settings"]["root"] == "D:\\Games\\KCD2");
    CHECK(state["engine"]["state"] == "idle");
    // Nothing is started by opening the window.
    CHECK(machine.engine_count() == 0);
    // What was asked is what would be started: the same game folder.
    CHECK(has_arg(machine.ran[0], "--where"));
    CHECK(arg_after(machine.ran[0], "--root") == "D:\\Games\\KCD2");

    CHECK(controller.call("play", json::object())["ok"] == true);
    REQUIRE(eventually([&] { return controller.state()["engine"]["phase"] == "ready"; }));
    CHECK(machine.engine_count() == 1);
    CHECK(has_arg(machine.last_engine(), "--game"));
    CHECK(arg_after(machine.last_engine(), "--root") == "D:\\Games\\KCD2");
    state = controller.state();
    CHECK(state["home"]["title"] == "Listening");
    CHECK(state["home"]["button"] == "Stop");
    CHECK(state["engine"]["voice"] == true);
    CHECK(controller.engine_active());

    controller.call("stop", json::object());
    REQUIRE(eventually([&] { return controller.state()["engine"]["state"] == "idle"; }));
    CHECK(controller.state()["home"]["title"] == "Ready to play");
    CHECK_FALSE(controller.engine_active());
}

TEST_CASE("bridge: Play does not start an engine over an installation that is not whole") {
    Machine machine("not-ready");
    machine.where = where_report(true, false);
    Controller controller(machine.parts());
    controller.start();
    REQUIRE(checked(controller));
    controller.call("play", json::object());
    REQUIRE(checked(controller));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(machine.engine_count() == 0);
    CHECK(controller.state()["home"]["fix"] == "repair");
}

TEST_CASE("bridge: no engine in the folder is said as that") {
    Machine machine("no-engine");
    fs::remove(machine.paths.engine);
    Controller controller(machine.parts());
    controller.start();
    REQUIRE(checked(controller));
    const json state = controller.state();
    CHECK(state["check"]["state"] == "failed");
    CHECK(state["home"]["title"] == "Moonshine is not fully installed");
    CHECK(state["home"]["fix"] == "reinstall");
    // It was not asked anything: there is nothing to ask.
    CHECK(machine.ran.empty());
}

TEST_CASE("bridge: an engine that answers with something that is not a report") {
    Machine machine("garbled");
    machine.where = "moonshine_app: the procedure entry point could not be located";
    Controller controller(machine.parts());
    controller.start();
    REQUIRE(checked(controller));
    CHECK(controller.state()["check"]["state"] == "failed");
}

TEST_CASE("bridge: pressing Play for them when the window opens is a setting") {
    Machine machine("autostart");
    put(machine.paths.config, "autostart = on\n");
    Controller controller(machine.parts());
    controller.start();
    REQUIRE(eventually([&] { return machine.engine_count() == 1; }));
    controller.shutdown();
    // Closing the window stops the engine by asking it to.
    CHECK(std::find(machine.order.begin(), machine.order.end(), "engine stopped") != machine.order.end());
}

// --- settings ----------------------------------------------------------------------------------------

TEST_CASE("bridge: saved settings are in moonshine.cfg, and the key is not") {
    Machine machine("save");
    put(machine.paths.config, "# by hand\nargs = --threads 6\n");
    Controller controller(machine.parts());
    controller.start();
    REQUIRE(checked(controller));

    const json answer = controller.call("saveSettings", {{"mic", false},
                                                         {"api", true},
                                                         {"api_url", " https://api.example.com/v1 "},
                                                         {"api_model", "some-model"},
                                                         {"api_extra", "{\"reasoning_effort\":\"low\"}"},
                                                         {"key", "sk-secret-0123456789"}});
    CHECK(answer["ok"] == true);
    CHECK(answer["restarted"] == false);

    const std::string text = get(machine.paths.config);
    CHECK(text.find("# by hand") == 0);
    CHECK(text.find("args = --threads 6") != std::string::npos);
    CHECK(text.find("mic = off") != std::string::npos);
    CHECK(text.find("model = api") != std::string::npos);
    CHECK(text.find("api_url = https://api.example.com/v1") != std::string::npos);
    // The key is in the system's store and nowhere in the file.
    CHECK(text.find("sk-secret") == std::string::npos);
    CHECK(machine.secrets->get() == "sk-secret-0123456789");

    const json state = controller.state();
    CHECK(state["has_key"] == true);
    CHECK(state["settings"]["api"] == true);
    // As typed, less the spaces a paste brings with it.
    CHECK(state["settings"]["api_url"] == "https://api.example.com/v1");
    CHECK(state["settings"]["args"] == "--threads 6");
    // Nor is it anywhere in what the page is told.
    CHECK(state.dump().find("sk-secret") == std::string::npos);

    // The check that follows is of the new settings, with the key in its environment.
    REQUIRE(checked(controller));
    CHECK(arg_after(machine.last_run(), "--api-url") == "https://api.example.com/v1");
    CHECK(env_of(machine.last_run(), "MOONSHINE_API_KEY") == "sk-secret-0123456789");
    CHECK(command_line(machine.last_run()).find("sk-secret") == std::string::npos);
}

TEST_CASE("bridge: a key is left alone unless the page says otherwise, and an empty one removes it") {
    Machine machine("key");
    Controller controller(machine.parts());
    machine.secrets->set("sk-kept-0123456789", nullptr);
    controller.start();
    REQUIRE(checked(controller));

    CHECK(controller.call("saveSettings", {{"mic", false}})["ok"] == true);
    CHECK(machine.secrets->get() == "sk-kept-0123456789");
    CHECK(controller.call("saveSettings", {{"key", ""}})["ok"] == true);
    CHECK(machine.secrets->get().empty());
    CHECK(controller.state()["has_key"] == false);
}

TEST_CASE("bridge: settings that cannot work are refused with a sentence, and nothing is written") {
    Machine machine("refuse");
    Controller controller(machine.parts());
    controller.start();
    REQUIRE(checked(controller));

    json answer = controller.call("saveSettings", {{"api", true}, {"api_url", ""}, {"api_model", "m"}});
    CHECK(answer["ok"] == false);
    CHECK(answer["error"] == "Enter the API's address.");
    answer = controller.call("saveSettings", {{"api", true}, {"api_url", "api.example.com/v1"}, {"api_model", "m"}});
    CHECK(answer["error"] == "The API's address should start with https://");
    answer = controller.call("saveSettings", {{"api", true}, {"api_url", "https://api.example.com/v1"}, {"api_model", ""}});
    CHECK(answer["error"] == "Enter the name of the model to ask for.");
    answer = controller.call("saveSettings", {{"api", true},
                                              {"api_url", "https://api.example.com/v1"},
                                              {"api_model", "m"},
                                              {"api_extra", "reasoning_effort=low"},
                                              {"key", "sk-not-kept-0123456789"}});
    CHECK(answer["ok"] == false);
    CHECK(answer["error"].get<std::string>().find("JSON object") != std::string::npos);

    CHECK_FALSE(fs::exists(machine.paths.config));
    CHECK(machine.secrets->get().empty());
    CHECK(controller.state()["settings"]["api"] == false);
    // An http address is allowed: a server on the player's own machine or network.
    CHECK(controller.call("saveSettings", {{"api", true}, {"api_url", "http://localhost:11434/v1"}, {"api_model", "m"}})["ok"] == true);
}

TEST_CASE("bridge: a change to the settings while playing starts the engine again with them") {
    Machine machine("restart");
    Controller controller(machine.parts());
    controller.start();
    REQUIRE(checked(controller));
    controller.call("play", json::object());
    REQUIRE(eventually([&] { return controller.state()["engine"]["phase"] == "ready"; }));
    CHECK_FALSE(has_arg(machine.last_engine(), "--no-mic"));

    const json answer = controller.call("saveSettings", {{"mic", false}});
    CHECK(answer["restarted"] == true);
    REQUIRE(eventually([&] { return machine.engine_count() == 2 && controller.state()["engine"]["phase"] == "ready"; }));
    CHECK(has_arg(machine.last_engine(), "--no-mic"));

    // What the engine is not started with does not restart it.
    CHECK(controller.call("saveSettings", {{"autostart", true}})["restarted"] == false);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(machine.engine_count() == 2);
    // And saving the same again changes nothing.
    CHECK(controller.call("saveSettings", {{"mic", false}})["restarted"] == false);
    CHECK(machine.engine_count() == 2);
}

TEST_CASE("bridge: an API chosen by hand with no address never becomes the local model") {
    Machine machine("half-api");
    put(machine.paths.config, "model = api\n");
    Controller controller(machine.parts());
    controller.start();
    REQUIRE(checked(controller));
    json state = controller.state();
    // The engine would have said ready: with no address it sees no API at all.
    CHECK(state["check"]["ready"] == false);
    CHECK(state["home"]["title"] == "The custom model API is not set up");
    CHECK(state["home"]["fix"] == "settings");
    controller.call("play", json::object());
    REQUIRE(checked(controller));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(machine.engine_count() == 0);
}

TEST_CASE("bridge: the folder picker's answer is saved and checked") {
    Machine machine("root");
    machine.where = where_report(false);
    Controller controller(machine.parts());
    controller.start();
    REQUIRE(checked(controller));
    CHECK(controller.state()["home"]["fix"] == "folder");
    machine.where = where_report();
    CHECK(controller.set_root("E:\\SteamLibrary\\steamapps\\common\\KingdomComeDeliverance2")["ok"] == true);
    REQUIRE(eventually([&] { return controller.state()["home"]["title"] == "Ready to play"; }));
    CHECK(arg_after(machine.last_run(), "--root") == "E:\\SteamLibrary\\steamapps\\common\\KingdomComeDeliverance2");
    CHECK(get(machine.paths.config).find("root = E:\\SteamLibrary") != std::string::npos);
}

// --- testing an API ----------------------------------------------------------------------------------

TEST_CASE("bridge: Test connection asks the API in the form, with the key typed or the key kept") {
    Machine machine("test-api");
    Controller controller(machine.parts());
    machine.secrets->set("sk-kept-0123456789", nullptr);
    controller.start();
    REQUIRE(checked(controller));

    // Nothing saved: the form's own values are what is tested.
    json answer = controller.call("testApi", {{"api_url", "https://api.example.com/v1"},
                                              {"api_model", "some-model"},
                                              {"api_extra", "{\"reasoning_effort\":\"low\"}"},
                                              {"key", "sk-typed-0123456789"}});
    CHECK(answer["ok"] == true);
    CHECK(answer["seconds"] == 0.4);
    Command asked = machine.last_run();
    CHECK(has_arg(asked, "--api-test"));
    CHECK(has_arg(asked, "--json"));
    CHECK(arg_after(asked, "--api-url") == "https://api.example.com/v1");
    CHECK(arg_after(asked, "--api-model") == "some-model");
    CHECK(env_of(asked, "MOONSHINE_API_KEY") == "sk-typed-0123456789");
    CHECK(env_of(asked, "MOONSHINE_API_EXTRA") == "{\"reasoning_effort\":\"low\"}");
    CHECK(command_line(asked).find("sk-typed") == std::string::npos);
    // Testing saves nothing.
    CHECK(machine.secrets->get() == "sk-kept-0123456789");
    CHECK_FALSE(fs::exists(machine.paths.config));

    answer = controller.call("testApi", {{"api_url", "https://api.example.com/v1"}, {"api_model", "some-model"}});
    CHECK(env_of(machine.last_run(), "MOONSHINE_API_KEY") == "sk-kept-0123456789");

    // What the engine says went wrong is passed on as it said it.
    machine.api_answer = R"({"ok":false,"error":"The API did not accept the key. Check it in Settings.","status":401,"kind":"key"})";
    answer = controller.call("testApi", {{"api_url", "https://api.example.com/v1"}, {"api_model", "some-model"}});
    CHECK(answer["ok"] == false);
    CHECK(answer["error"] == "The API did not accept the key. Check it in Settings.");

    machine.api_answer = "Segmentation fault";
    answer = controller.call("testApi", {{"api_url", "https://api.example.com/v1"}, {"api_model", "some-model"}});
    CHECK(answer["ok"] == false);
    CHECK(answer["error"].get<std::string>().find("gave no answer") != std::string::npos);

    const std::size_t before = machine.ran.size();
    answer = controller.call("testApi", {{"api_url", ""}, {"api_model", "some-model"}});
    CHECK(answer["error"] == "Enter the API's address.");
    CHECK(machine.ran.size() == before);
    CHECK(Controller::slow("testApi"));
    CHECK_FALSE(Controller::slow("getState"));
}

// --- the logs ----------------------------------------------------------------------------------------

TEST_CASE("bridge: the log view is given the end of the log, then what is added to it") {
    Machine machine("logs");
    Controller controller(machine.parts());
    std::string log;
    for (int i = 0; i < 4000; ++i) log += "line " + std::to_string(i) + " of the engine's log, which goes on\n";
    put(machine.paths.engine_log, log);

    json piece = controller.call("readLog", {{"which", "app"}, {"from", -1}});
    CHECK(piece["reset"] == true);
    std::string text = piece["text"];
    // The end of it, from the start of a line.
    CHECK(text.size() < log.size());
    CHECK(text.rfind("line ", 0) == 0);
    CHECK(text.substr(text.size() - 30) == log.substr(log.size() - 30));
    CHECK(piece["next"] == static_cast<std::int64_t>(log.size()));

    // Nothing new is nothing.
    piece = controller.call("readLog", {{"which", "app"}, {"from", piece["next"]}});
    CHECK(piece["text"] == "");
    CHECK(piece["reset"] == false);

    {
        std::ofstream out(machine.paths.engine_log, std::ios::binary | std::ios::app);
        out << "Betty: Aye, two groschen.\n";
    }
    piece = controller.call("readLog", {{"which", "app"}, {"from", piece["next"]}});
    CHECK(piece["text"] == "Betty: Aye, two groschen.\n");

    // The engine starts its log again each run: shorter than what was read is a new file.
    put(machine.paths.engine_log, "a new run\n");
    piece = controller.call("readLog", {{"which", "app"}, {"from", piece["next"]}});
    CHECK(piece["reset"] == true);
    CHECK(piece["text"] == "a new run\n");

    // The launcher's log is the other one, and one that is not there yet is empty.
    piece = controller.call("readLog", {{"which", "launcher"}, {"from", -1}});
    CHECK(piece["text"] == "");
    CHECK(piece["next"] == 0);
}

TEST_CASE("bridge: a read that ends half way through a character leaves it for the next") {
    Machine machine("logs-utf8");
    Controller controller(machine.parts());
    // "Kralovstvi" with its accents, cut after the first byte of the first one.
    put(machine.paths.engine_log, "Kr\xC3");
    json piece = controller.call("readLog", {{"which", "app"}, {"from", -1}});
    CHECK(piece["text"] == "Kr");
    CHECK(piece["next"] == 2);
    put(machine.paths.engine_log, "Kr\xC3\xA1lovstv\xC3\xAD\n");
    piece = controller.call("readLog", {{"which", "app"}, {"from", 2}});
    CHECK(piece["text"] == "\xC3\xA1lovstv\xC3\xAD\n");
}

TEST_CASE("bridge: collecting logs runs the collector without its pause and says where the zip is") {
    Machine machine("collect");
    put(machine.paths.home / "Collect-Logs.ps1", "# the script");
    Controller controller(machine.parts());
    controller.call("collectLogs", json::object());
    REQUIRE(task_over(controller));
    const json task = controller.state()["task"];
    CHECK(task["kind"] == "collect");
    CHECK(task["error"] == "");
    CHECK(task["path"] == "C:\\Users\\henry\\Desktop\\Moonshine-logs-1.zip");
    CHECK(task["result"].get<std::string>().find("Moonshine-logs-1.zip") != std::string::npos);
    const Command asked = machine.last_run();
    CHECK(asked.exe.filename() == "powershell.exe");
    CHECK(has_arg(asked, "-File"));
    CHECK(env_of(asked, "MOONSHINE_NO_PAUSE") == "1");
    // It is not the home screen's business.
    CHECK(controller.state()["home"]["button"] != "Cancel");

    controller.call("dismissTask", json::object());
    CHECK(controller.state()["task"]["kind"] == "");
}

TEST_CASE("bridge: a collector that wrote nothing is an error with its last words") {
    Machine machine("collect-fails");
    put(machine.paths.home / "Collect-Logs.ps1", "# the script");
    machine.collect_output = "Compress-Archive : Access to the path is denied.\n";
    Controller controller(machine.parts());
    controller.call("collectLogs", json::object());
    REQUIRE(task_over(controller));
    CHECK(controller.state()["task"]["error"].get<std::string>().find("Access to the path is denied") != std::string::npos);
}

// --- updating, repairing, removing -------------------------------------------------------------------

TEST_CASE("bridge: a newer version published is offered, and the same or an older one is not") {
    Machine machine("update-check");
    machine.install_manifest();
    machine.publish("0.9.101");
    Controller controller(machine.parts());
    controller.start();
    REQUIRE(eventually([&] { return controller.state()["update"]["state"] == "available"; }));
    CHECK(controller.state()["update"]["latest"] == "0.9.101");

    machine.publish("0.9.100");
    controller.call("checkUpdate", json::object());
    REQUIRE(eventually([&] { return controller.state()["update"]["state"] == "current"; }));
    machine.publish("0.9.99");
    controller.call("checkUpdate", json::object());
    REQUIRE(eventually([&] { return controller.state()["update"]["state"] == "current" &&
                                    controller.state()["update"]["latest"] == "0.9.99"; }));

    machine.online = false;
    controller.call("checkUpdate", json::object());
    REQUIRE(eventually([&] { return controller.state()["update"]["state"] == "failed"; }));
    CHECK(controller.state()["update"]["error"] == "Could not look for updates: could not reach the server");
}

TEST_CASE("bridge: an installation from before there was a manifest says so") {
    Machine machine("no-manifest");
    Controller controller(machine.parts());
    controller.call("checkUpdate", json::object());
    REQUIRE(eventually([&] { return controller.state()["update"]["state"] == "failed"; }));
    CHECK(controller.state()["update"]["error"].get<std::string>().find("has no manifest") != std::string::npos);
}

TEST_CASE("bridge: updating downloads the installer, stops the engine, and hands over to it") {
    Machine machine("update");
    machine.install_manifest();
    machine.publish("0.9.101");
    Controller controller(machine.parts());
    controller.start();
    REQUIRE(checked(controller));
    REQUIRE(eventually([&] { return controller.state()["update"]["state"] == "available"; }));
    controller.call("play", json::object());
    REQUIRE(eventually([&] { return controller.state()["engine"]["phase"] == "ready"; }));

    controller.call("applyUpdate", json::object());
    REQUIRE(eventually([&] { return machine.quits == 1; }));

    // The installer came through the same fetch as everything else: one file, with the
    // size and hash the published manifest gives, into a folder of its own.
    REQUIRE(machine.fetched.size() == 1);
    const Manifest& one = machine.fetched[0].first;
    REQUIRE(one.files.size() == 1);
    CHECK(one.files[0].path == "Moonshine-0.9.101-setup.exe");
    CHECK(one.files[0].url == "https://download.example/Moonshine-0.9.101-setup.exe");
    CHECK(one.files[0].bytes == 1000);
    CHECK(one.files[0].sha256 == std::string(64, 'b'));
    CHECK(machine.fetched[0].second == machine.paths.home / "temp" / "moonshine-update");

    // Into this folder, without its questions, and to open Moonshine again after.
    REQUIRE(machine.launched.size() == 1);
    const Command installer = machine.launched[0];
    CHECK(installer.exe == machine.paths.home / "temp" / "moonshine-update" / "Moonshine-0.9.101-setup.exe");
    CHECK(has_arg(installer, "/SILENT"));
    // Its questions are skipped; what goes wrong is still said.
    CHECK_FALSE(has_arg(installer, "/SUPPRESSMSGBOXES"));
    CHECK(has_arg(installer, "/RELAUNCH=1"));
    CHECK(has_arg(installer, "/DIR=" + machine.paths.home.string()));

    // In that order: nothing is replaced under a running engine, and the installer does
    // not find this app still claiming to be open.
    CHECK(machine.order == std::vector<std::string>{"fetched", "engine stopped", "released",
                                                    "launched Moonshine-0.9.101-setup.exe", "quit"});
}

TEST_CASE("bridge: an update that did not download changes nothing") {
    Machine machine("update-fails");
    machine.install_manifest();
    machine.publish("0.9.101");
    machine.fetch_result.bad = {"Moonshine-0.9.101-setup.exe"};
    machine.fetch_result.trouble = "could not reach the server";
    Controller controller(machine.parts());
    controller.start();
    REQUIRE(eventually([&] { return controller.state()["update"]["state"] == "available"; }));
    controller.call("applyUpdate", json::object());
    REQUIRE(task_over(controller));
    CHECK(controller.state()["task"]["error"].get<std::string>().find("did not download") != std::string::npos);
    CHECK(machine.launched.empty());
    CHECK(machine.quits == 0);
}

TEST_CASE("bridge: a published version with no installer, or one this would not run, is not run") {
    Machine machine("update-odd");
    machine.install_manifest();
    Controller controller(machine.parts());
    for (const char* url : {"", "https://download.example/setup.msi", "https://download.example/a%20b.exe",
                            "https://download.example/setup.exe?next=../../evil"}) {
        json manifest = {{"version", "0.9.101"}, {"files", json::array()}};
        if (*url) manifest["setup"] = {{"url", url}, {"bytes", 10}, {"sha256", std::string(64, 'b')}};
        machine.published = manifest.dump();
        controller.call("dismissTask", json::object());
        controller.call("applyUpdate", {{"reinstall", true}});
        REQUIRE(task_over(controller));
        CHECK_MESSAGE(controller.state()["task"]["error"].get<std::string>().find("does not come with an installer") !=
                          std::string::npos,
                      url);
    }
    CHECK(machine.launched.empty());
    CHECK(machine.fetched.empty());
}

TEST_CASE("bridge: the installer an update ran is gone the next time the window opens; a part download is kept") {
    Machine machine("update-leftover");
    const fs::path folder = machine.paths.home / "temp" / "moonshine-update";
    fs::create_directories(folder);
    std::ofstream(folder / "Moonshine-0.9.100-setup.exe") << "the last update";
    std::ofstream(folder / "Moonshine-0.9.101-setup.exe.part") << "half of the next";
    Controller controller(machine.parts());
    controller.start();
    CHECK_FALSE(fs::exists(folder / "Moonshine-0.9.100-setup.exe"));
    CHECK(fs::exists(folder / "Moonshine-0.9.101-setup.exe.part"));
}

TEST_CASE("bridge: updating to the version already here is a repair") {
    Machine machine("update-same");
    machine.install_manifest();
    machine.publish("0.9.100");
    Controller controller(machine.parts());
    controller.start();
    REQUIRE(eventually([&] { return controller.state()["update"]["state"] == "current"; }));
    controller.call("applyUpdate", json::object());
    REQUIRE(task_over(controller));
    CHECK(controller.state()["task"]["kind"] == "repair");
    REQUIRE(machine.fetched.size() == 1);
    CHECK(machine.fetched[0].first.files[0].path == "models/llm/model.gguf");
    CHECK(machine.launched.empty());
}

TEST_CASE("bridge: repair fetches the installed manifest's files over a stopped engine, and checks again") {
    Machine machine("repair");
    machine.install_manifest();
    machine.where = where_report(true, false);
    Controller controller(machine.parts());
    controller.start();
    REQUIRE(checked(controller));
    CHECK(controller.state()["home"]["fix"] == "repair");

    machine.fetch_result.fetched = 1;
    machine.where = where_report();
    controller.call("repair", json::object());
    REQUIRE(task_over(controller));
    json state = controller.state();
    CHECK(state["task"]["kind"] == "repair");
    CHECK(state["task"]["error"] == "");
    CHECK(state["task"]["result"] == "1 file was downloaded again. Everything is now in place.");
    REQUIRE(machine.fetched.size() == 1);
    CHECK(machine.fetched[0].second == machine.paths.home);
    CHECK(machine.fetched[0].first.version == "0.9.100");
    // And the installation is looked at again, so the home screen says what is now true.
    REQUIRE(eventually([&] { return controller.state()["home"]["title"] == "Ready to play"; }));

    // Nothing wrong is said as that.
    machine.fetch_result.fetched = 0;
    controller.call("dismissTask", json::object());
    controller.call("repair", json::object());
    REQUIRE(task_over(controller));
    CHECK(controller.state()["task"]["result"] == "Every file is in place and correct.");
}

TEST_CASE("bridge: repair that could not finish says so and keeps what it has") {
    Machine machine("repair-fails");
    machine.install_manifest();
    machine.fetch_result.bad = {"models/llm/model.gguf"};
    machine.fetch_result.trouble = "models/llm/model.gguf: the transfer stalled, after 5 attempts";
    Controller controller(machine.parts());
    controller.call("repair", json::object());
    REQUIRE(task_over(controller));
    const std::string error = controller.state()["task"]["error"];
    CHECK(error.find("the transfer stalled") != std::string::npos);
    CHECK(error.find("what was downloaded is kept") != std::string::npos);
}

TEST_CASE("bridge: repair puts the mod back in the game only where the installer had put one") {
    Machine machine("repair-mod");
    machine.install_manifest();
    const fs::path game = machine.paths.home / "game";
    json report = where_report();
    report["game_root"] = game.string();
    machine.where = report;
    put(machine.paths.home / "mod" / "moonshine_names" / "mod.manifest", "<mod version 2/>");
    Controller controller(machine.parts());
    controller.start();
    REQUIRE(checked(controller));

    // Subscribed on the Workshop: no folder in the game's mods, and none is made.
    controller.call("repair", json::object());
    REQUIRE(task_over(controller));
    CHECK_FALSE(fs::exists(game / "mods" / "moonshine_names"));

    put(game / "mods" / "moonshine_names" / "mod.manifest", "<mod version 1/>");
    controller.call("dismissTask", json::object());
    controller.call("repair", json::object());
    REQUIRE(task_over(controller));
    CHECK(get(game / "mods" / "moonshine_names" / "mod.manifest") == "<mod version 2/>");
}

TEST_CASE("bridge: one long job at a time") {
    Machine machine("one-task");
    machine.install_manifest();
    put(machine.paths.home / "Collect-Logs.ps1", "# the script");
    Controller controller(machine.parts());
    machine.hold_fetch = true;
    controller.call("repair", json::object());
    REQUIRE(eventually([&] { return controller.state()["task"]["running"] == true; }));
    // While it runs it is what the home screen is about.
    CHECK(controller.state()["home"]["title"] == "Repairing files");
    CHECK(controller.state()["home"]["action"] == "cancelTask");
    controller.call("collectLogs", json::object());
    controller.call("dismissTask", json::object());
    CHECK(controller.state()["task"]["kind"] == "repair");
    CHECK(controller.state()["task"]["running"] == true);
    machine.hold_fetch = false;
    REQUIRE(task_over(controller));
    // The second was not started over the first, nor after it.
    CHECK(controller.state()["task"]["kind"] == "repair");
    for (const Command& command : machine.ran) CHECK(command.exe.filename() != "powershell.exe");
}

TEST_CASE("bridge: a download that is cancelled stops, and says what it kept") {
    Machine machine("cancel");
    machine.install_manifest();
    Controller controller(machine.parts());
    machine.hold_fetch = true;
    controller.call("repair", json::object());
    REQUIRE(eventually([&] { return controller.state()["task"]["running"] == true; }));
    controller.call("cancelTask", json::object());
    REQUIRE(task_over(controller));
    const json task = controller.state()["task"];
    CHECK(task["error"] == "");
    CHECK(task["result"] == "Stopped. What was downloaded is kept, and Repair carries on from there.");
    // And the next one is not stopped before it starts by the last one's cancel.
    machine.hold_fetch = false;
    controller.call("dismissTask", json::object());
    controller.call("repair", json::object());
    REQUIRE(task_over(controller));
    CHECK(controller.state()["task"]["result"] == "Every file is in place and correct.");
}

TEST_CASE("bridge: uninstalling removes the key, and the mod only when asked, then runs the uninstaller") {
    Machine machine("uninstall");
    const fs::path game = machine.paths.home / "game";
    json report = where_report();
    report["game_root"] = game.string();
    machine.where = report;
    put(game / "mods" / "moonshine_names" / "mod.manifest", "<mod/>");
    put(game / "mods" / "someone_elses_mod" / "mod.manifest", "<mod/>");
    machine.uninstaller = machine.paths.home / "unins000.exe";
    {
        Controller controller(machine.parts());
        machine.secrets->set("sk-kept-0123456789", nullptr);
        controller.start();
        REQUIRE(checked(controller));
        controller.call("play", json::object());
        REQUIRE(eventually([&] { return controller.state()["engine"]["phase"] == "ready"; }));

        CHECK(controller.call("uninstall", {{"remove_mod", false}})["ok"] == true);
        REQUIRE(eventually([&] { return machine.quits == 1; }));
        CHECK(machine.secrets->get().empty());
        // Not asked for: the mod stays where it is.
        CHECK(fs::exists(game / "mods" / "moonshine_names" / "mod.manifest"));
        REQUIRE(machine.launched.size() == 1);
        CHECK(machine.launched[0].exe == machine.uninstaller);
        // Confirmed in the window; the uninstaller is not to ask again. Its log goes
        // where it will still be when the installation is not.
        const std::string log = "/LOG=" + (machine.paths.home / "temp" / "moonshine-uninstall.log").string();
        CHECK(machine.launched[0].args == std::vector<std::string>{"/SILENT", log});
        CHECK(machine.order == std::vector<std::string>{"engine stopped", "released", "launched unins000.exe", "quit"});
    }
    {
        Controller controller(machine.parts());
        controller.start();
        REQUIRE(checked(controller));
        CHECK(controller.call("uninstall", {{"remove_mod", true}})["ok"] == true);
        REQUIRE(eventually([&] { return machine.quits == 2; }));
        CHECK_FALSE(fs::exists(game / "mods" / "moonshine_names"));
        // Only ours.
        CHECK(fs::exists(game / "mods" / "someone_elses_mod" / "mod.manifest"));
    }
}

TEST_CASE("bridge: a copy the installer did not put there has nothing to uninstall it with") {
    Machine machine("no-uninstaller");
    Controller controller(machine.parts());
    const json answer = controller.call("uninstall", {{"remove_mod", true}});
    CHECK(answer["ok"] == false);
    CHECK(answer["error"].get<std::string>().find("was not put here by its installer") != std::string::npos);
    CHECK(machine.launched.empty());
    CHECK(machine.quits == 0);
}

TEST_CASE("bridge: the folders and files a button opens") {
    Machine machine("open");
    put(machine.paths.home / "README.txt", "read me");
    put(machine.paths.home / "LICENSES" / "README.txt", "licences");
    Controller controller(machine.parts());
    CHECK(controller.call("open", {{"what", "logs"}})["ok"] == true);
    CHECK(controller.call("open", {{"what", "readme"}})["ok"] == true);
    CHECK(controller.call("open", {{"what", "licences"}})["ok"] == true);
    CHECK(machine.opened == std::vector<std::string>{machine.paths.logs.string(),
                                                     (machine.paths.home / "README.txt").string(),
                                                     (machine.paths.home / "LICENSES" / "README.txt").string()});
    CHECK(controller.call("open", {{"what", "nothing"}})["ok"] == false);
    CHECK(controller.call("no such call", json::object())["ok"] == false);
}
