// Keeping the engine running (gui/supervisor.h), with engines that die on cue.
//
// The loop is installer/Moonshine.ps1's, and the numbers in it -- twenty seconds, five
// deaths, two seconds growing to sixteen -- were arrived at on players' machines. They are
// pinned here so that a tidy-up cannot move them. Nothing is started and no time passes:
// the engine is a script of how long it lives and how it ends, and the clock is told the
// time.

#include "doctest.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "supervisor.h"

using namespace moonshine::gui;
namespace fs = std::filesystem;

namespace {

fs::path fresh_home(const char* name) {
    const fs::path dir = fs::temp_directory_path() / "moonshine-gui-supervisor-tests" / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

void touch(const fs::path& file, const std::string& text = "x") {
    fs::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary);
    out << text;
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

// How one run of the engine goes.
struct Run {
    // Seconds it lives before exiting on its own; negative for one that runs until told.
    double life = -1;
    int code = 1;
    // Ignores /quit, and has to be killed.
    bool deaf = false;
    // What it writes to its status file as it comes up.
    std::string status;
};

// A machine with no machine in it: the engines the test scripted, a clock that moves when
// something is said to take time, and a record of everything asked of it.
struct World {
    Paths paths;
    std::mutex lock;
    std::vector<Run> script;
    std::vector<Command> started;
    std::vector<double> waits;
    std::vector<std::string> said;
    int kills = 0;
    std::atomic<int> games_started{0};
    std::atomic<bool> game_running{false};
    // A machine where nothing can say whether the game is running (a Mac).
    bool can_see_game = true;
    bool strays_stop = true;
    std::atomic<double> clock{1000.0};

    class Engine : public Child {
    public:
        Engine(World& world, Run run) : world_(world), run_(std::move(run)) {}
        std::optional<int> wait_for(std::chrono::milliseconds) override {
            if (code_) return code_;
            if (run_.life >= 0) {
                world_.clock = world_.clock + run_.life;
                code_ = run_.code;
                return code_;
            }
            if (quit_ && !run_.deaf) {
                code_ = 0;
                return code_;
            }
            if (killed_) {
                code_ = 1;
                return code_;
            }
            // Still running. A moment of real time, so the loop asking does not spin.
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return std::nullopt;
        }
        bool say(std::string_view line) override {
            std::lock_guard<std::mutex> held(world_.lock);
            world_.said.emplace_back(line);
            if (line == "/quit") quit_ = true;
            return true;
        }
        void hang_up() override {}
        void kill() override {
            std::lock_guard<std::mutex> held(world_.lock);
            world_.kills += 1;
            killed_ = true;
        }
        std::string take_output() override { return {}; }

    private:
        World& world_;
        Run run_;
        std::optional<int> code_;
        bool quit_ = false;
        bool killed_ = false;
    };

    explicit World(const char* name) : paths(Paths::at(fresh_home(name))) {}

    Supervisor::Hooks hooks() {
        Supervisor::Hooks hooks;
        hooks.spawn = [this](const Command& command, std::string* trouble) -> std::unique_ptr<Child> {
            std::lock_guard<std::mutex> held(lock);
            started.push_back(command);
            if (script.empty()) {
                *trouble = "the script ran out";
                return nullptr;
            }
            Run run = script.front();
            script.erase(script.begin());
            if (!run.status.empty()) touch(paths.status, run.status);
            return std::make_unique<Engine>(*this, run);
        };
        if (can_see_game) hooks.game_running = [this] { return game_running.load(); };
        hooks.start_game = [this] { games_started += 1; };
        hooks.stop_strays = [this] { return strays_stop; };
        hooks.seconds = [this] { return clock.load(); };
        hooks.wall = [] { return static_cast<std::time_t>(1794000000); };
        hooks.wait = [this](double seconds) {
            std::lock_guard<std::mutex> held(lock);
            waits.push_back(seconds);
            clock = clock + seconds;
        };
        return hooks;
    }

    int starts() {
        std::lock_guard<std::mutex> held(lock);
        return static_cast<int>(started.size());
    }
};

// Wait, in real time, for the supervisor's thread to get somewhere.
bool eventually(const std::function<bool()>& reached) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        if (reached()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return reached();
}

std::string slurp(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

}  // namespace

// --- when to start it again ------------------------------------------------------------------

TEST_CASE("restart: a clean exit and a deliberate stop are the end") {
    RestartPolicy policy;
    Verdict v = policy.after(0, 300, false);
    CHECK_FALSE(v.restart);
    CHECK_FALSE(v.gave_up);
    // Ctrl+C, or the console window closed on it: 0xC000013A.
    v = policy.after(-1073741510, 300, false);
    CHECK_FALSE(v.restart);
    CHECK_FALSE(v.gave_up);
    // Killed because we asked, which is not a crash however it exited.
    v = policy.after(1, 3, true);
    CHECK_FALSE(v.restart);
    CHECK_FALSE(v.gave_up);
}

TEST_CASE("restart: a crash is two seconds, and the line says what happened") {
    RestartPolicy policy;
    const Verdict v = policy.after(-1073741819, 754, false);
    CHECK(v.restart);
    CHECK(v.wait_seconds == 2);
    CHECK(v.line.find("0xC0000005") != std::string::npos);
    CHECK(v.line.find("after 754s") != std::string::npos);
    CHECK(v.line.find("again in 2s") != std::string::npos);
}

TEST_CASE("restart: deaths within twenty seconds of starting wait longer, and the fifth is the last") {
    RestartPolicy policy;
    std::vector<int> waits;
    Verdict v;
    for (int i = 0; i < 5; ++i) {
        v = policy.after(1, 3, false);
        if (v.restart) waits.push_back(v.wait_seconds);
    }
    CHECK(waits == std::vector<int>{2, 4, 8, 16});
    CHECK_FALSE(v.restart);
    CHECK(v.gave_up);
    CHECK(v.line.find("5 times within 20s") != std::string::npos);
}

TEST_CASE("restart: an engine that came up starts the count again") {
    RestartPolicy policy;
    for (int i = 0; i < 4; ++i) policy.after(1, 3, false);
    // Twenty seconds is under a model load: this one had come up.
    Verdict v = policy.after(1, 20, false);
    CHECK(v.restart);
    CHECK(v.wait_seconds == 2);
    v = policy.after(1, 19, false);
    CHECK(v.restart);
    CHECK(v.wait_seconds == 2);
    v = policy.after(1, 19, false);
    CHECK(v.wait_seconds == 4);
}

// --- the dead run's log ------------------------------------------------------------------------

TEST_CASE("logs: the last run's log is put aside under the time, and five are kept") {
    const fs::path home = fresh_home("rotate");
    const fs::path log = home / "moonshine_app.log";
    std::time_t now = 1794000000;
    for (int i = 0; i < 8; ++i) {
        touch(log, "run " + std::to_string(i));
        rotate_logs(log, now + i * 60);
        CHECK_FALSE(fs::exists(log));
    }
    std::vector<std::string> kept;
    for (const auto& entry : fs::directory_iterator(home)) kept.push_back(slurp(entry.path()));
    std::sort(kept.begin(), kept.end());
    CHECK(kept == std::vector<std::string>{"run 3", "run 4", "run 5", "run 6", "run 7"});
    // Nothing to put aside is nothing done.
    rotate_logs(log, now + 1000);
    CHECK(std::distance(fs::directory_iterator(home), fs::directory_iterator()) == 5);
}

// --- what the engine is started with -------------------------------------------------------------

TEST_CASE("command: the weights are found by shape, and the settings become flags") {
    const Paths paths = Paths::at(fresh_home("command"));
    touch(paths.home / "models" / "llm" / "b-later.gguf");
    touch(paths.home / "models" / "llm" / "a-first.gguf");
    touch(paths.home / "models" / "llm" / "notes.txt");
    fs::create_directories(paths.home / "models" / "stt" / "tiny-streaming-en");
    Settings settings;
    settings.root = "D:\\Games\\Kingdom Come";
    settings.mic = false;
    settings.args = "--threads 6";

    const Command command = engine_command(paths, settings, "");
    CHECK(command.exe == paths.engine);
    CHECK(command.cwd == paths.home);
    CHECK(has_arg(command, "--game"));
    CHECK(arg_after(command, "--world") == "setting.txt");
    CHECK(fs::path(arg_after(command, "--log")) == paths.engine_log);
    CHECK(fs::path(arg_after(command, "--status")) == paths.status);
    // The window's going is the engine's stop.
    CHECK(has_arg(command, "--quit-on-eof"));
    CHECK(fs::path(arg_after(command, "--gguf")) == fs::path("models") / "llm" / "a-first.gguf");
    CHECK(fs::path(arg_after(command, "--stt-model")) == fs::path("models") / "stt" / "tiny-streaming-en");
    CHECK(arg_after(command, "--root") == "D:\\Games\\Kingdom Come");
    CHECK(has_arg(command, "--no-mic"));
    // What was written by hand comes last, and so has the last word.
    REQUIRE(command.args.size() >= 2);
    CHECK(command.args[command.args.size() - 2] == "--threads");
    CHECK(command.args.back() == "6");
    CHECK_FALSE(has_arg(command, "--api-url"));
    CHECK(command.env.empty());

    // The check asks about exactly what would be started.
    const Command check = check_command(paths, settings, "");
    CHECK(check.args[0] == "--where");
    CHECK(check.args[1] == "--json");
    CHECK(arg_after(check, "--root") == "D:\\Games\\Kingdom Come");
    CHECK_FALSE(has_arg(check, "--game"));
}

TEST_CASE("command: an API's key goes in the environment and nowhere on the command line") {
    const Paths paths = Paths::at(fresh_home("command-api"));
    Settings settings;
    settings.api = true;
    settings.api_url = "https://api.example.com/v1";
    settings.api_model = "some-model";
    settings.api_extra = "{\"reasoning_effort\":\"low\"}";
    const std::string key = "sk-secret-0123456789";

    Command command = engine_command(paths, settings, key);
    CHECK(arg_after(command, "--api-url") == "https://api.example.com/v1");
    CHECK(arg_after(command, "--api-model") == "some-model");
    // Guessing ahead is paid for by the token; off until asked for.
    CHECK_FALSE(has_arg(command, "--guessing"));
    for (const std::string& arg : command.args) CHECK(arg.find(key) == std::string::npos);
    CHECK(command_line(command).find(key) == std::string::npos);
    bool key_set = false, extra_set = false;
    for (const auto& [name, value] : command.env) {
        if (name == "MOONSHINE_API_KEY") key_set = value == key;
        if (name == "MOONSHINE_API_EXTRA") extra_set = value == settings.api_extra;
    }
    CHECK(key_set);
    CHECK(extra_set);

    settings.api_guessing = true;
    CHECK(has_arg(engine_command(paths, settings, key), "--guessing"));
    // The experimental quests: the engine's --all-quests, only when asked for.
    CHECK_FALSE(has_arg(engine_command(paths, settings, key), "--all-quests"));
    settings.experimental_quests = true;
    CHECK(has_arg(engine_command(paths, settings, key), "--all-quests"));
    settings.experimental_quests = false;

    // No key is an empty one said out loud: a key in the environment the window was itself
    // started from is not the player's.
    command = engine_command(paths, settings, "");
    bool emptied = false;
    for (const auto& [name, value] : command.env) emptied = emptied || (name == "MOONSHINE_API_KEY" && value.empty());
    CHECK(emptied);
}

TEST_CASE("command: an argument is written so that the program reads it back whole") {
    CHECK(quote_argument("plain") == "plain");
    CHECK(quote_argument("") == "\"\"");
    CHECK(quote_argument("two words") == "\"two words\"");
    CHECK(quote_argument("say \"hi\"") == "\"say \\\"hi\\\"\"");
    // A folder that ends in a backslash must not swallow the closing quote.
    CHECK(quote_argument("C:\\Program Files\\Game\\") == "\"C:\\Program Files\\Game\\\\\"");
    CHECK(quote_argument("C:\\NoSpaces\\") == "C:\\NoSpaces\\");
}

#ifdef _WIN32
TEST_CASE("uninstaller: the one in the folder that still has its record, the newest of them") {
    // A folder the installer has no record of under Moonshine's own name, which is what a
    // tester build's is: what is found is what is in the folder.
    const fs::path home = fresh_home("uninstaller");
    CHECK(installed_uninstaller(home).empty());
    // An uninstaller left behind without its record removes nothing, and says so in a box.
    touch(home / "unins000.exe");
    CHECK(installed_uninstaller(home).empty());
    // Inno Setup took the next number because that one was in the way.
    touch(home / "unins001.exe");
    touch(home / "unins001.dat");
    CHECK(installed_uninstaller(home).filename() == "unins001.exe");
    touch(home / "unins000.dat");
    CHECK(installed_uninstaller(home).filename() == "unins001.exe");
    // Other things that happen to begin the same way are not uninstallers.
    touch(home / "uninstall-notes.exe");
    touch(home / "uninsabc.exe");
    touch(home / "uninsabc.dat");
    CHECK(installed_uninstaller(home).filename() == "unins001.exe");
}
#endif

// --- the loop ------------------------------------------------------------------------------------

TEST_CASE("supervisor: Play asks Steam for the game, starts the engine, and a clean exit is the end") {
    World world("clean");
    world.script = {{.life = 600, .code = 0}};
    Supervisor supervisor(world.paths, world.hooks());
    CHECK(supervisor.state().state == "idle");
    supervisor.play(Settings{}, "");
    REQUIRE(eventually([&] { return !supervisor.active(); }));
    CHECK(world.starts() == 1);
    CHECK(world.games_started == 1);
    CHECK(world.waits.empty());
    CHECK(supervisor.state().state == "idle");
    const std::string log = slurp(world.paths.launcher_log);
    CHECK(log.find("asking Steam to start Kingdom Come: Deliverance 2") != std::string::npos);
    CHECK(log.find("app stopped (exit 0).") != std::string::npos);
}

TEST_CASE("supervisor: a game already up is joined, and one the player starts is left to them") {
    {
        World world("joined");
        world.game_running = true;
        world.script = {{.life = 60, .code = 0}};
        Supervisor supervisor(world.paths, world.hooks());
        supervisor.play(Settings{}, "");
        REQUIRE(eventually([&] { return !supervisor.active(); }));
        CHECK(world.games_started == 0);
        CHECK(slurp(world.paths.launcher_log).find("already running; joining it") != std::string::npos);
    }
    {
        World world("own-start");
        world.script = {{.life = 60, .code = 0}};
        Settings settings;
        settings.launch_game = false;
        Supervisor supervisor(world.paths, world.hooks());
        supervisor.play(settings, "");
        REQUIRE(eventually([&] { return !supervisor.active(); }));
        CHECK(world.games_started == 0);
    }
}

TEST_CASE("supervisor: an engine that dies is started again, and the player is told") {
    World world("crash");
    world.script = {{.life = 754, .code = -1073741819}, {.life = -1}};
    Supervisor supervisor(world.paths, world.hooks());
    supervisor.play(Settings{}, "");
    REQUIRE(eventually([&] { return world.starts() == 2 && supervisor.state().state == "running"; }));
    CHECK(world.waits == std::vector<double>{2});
    CHECK(supervisor.state().restarts == 1);
    // The game is asked for once, not once per engine.
    CHECK(world.games_started == 1);
    CHECK(slurp(world.paths.launcher_log).find("the app exited (0xC0000005) after 754s") != std::string::npos);

    supervisor.stop();
    CHECK_FALSE(supervisor.active());
    CHECK(supervisor.state().state == "idle");
    // Asked, not killed: /quit is how the engine tells the game nobody is listening.
    CHECK(world.said == std::vector<std::string>{"/quit"});
    CHECK(world.kills == 0);
}

TEST_CASE("supervisor: five deaths on starting and it stops trying, saying why if the engine did") {
    World world("hopeless");
    const std::string status = R"({"phase":"failed","error":"The model file is damaged. Use Repair files."})";
    for (int i = 0; i < 5; ++i) world.script.push_back({.life = 1, .code = 1, .status = status});
    Supervisor supervisor(world.paths, world.hooks());
    supervisor.play(Settings{}, "");
    REQUIRE(eventually([&] { return !supervisor.active(); }));
    CHECK(world.starts() == 5);
    CHECK(world.waits == std::vector<double>{2, 4, 8, 16});
    const EngineState state = supervisor.state();
    CHECK(state.state == "failed");
    CHECK(state.message == "The model file is damaged. Use Repair files.");
    CHECK(state.restarts == 4);

    // Play again is a fresh start, with a fresh count.
    world.script = {{.life = 30, .code = 0}};
    supervisor.play(Settings{}, "");
    REQUIRE(eventually([&] { return !supervisor.active(); }));
    CHECK(supervisor.state().state == "idle");
    CHECK(supervisor.state().restarts == 0);
}

TEST_CASE("supervisor: an engine that says nothing about why it died still gets a sentence") {
    World world("silent");
    for (int i = 0; i < 5; ++i) world.script.push_back({.life = 0, .code = 3});
    Supervisor supervisor(world.paths, world.hooks());
    supervisor.play(Settings{}, "");
    REQUIRE(eventually([&] { return !supervisor.active(); }));
    CHECK(supervisor.state().state == "failed");
    CHECK(supervisor.state().message == "Moonshine keeps stopping as soon as it starts.");
}

TEST_CASE("supervisor: an engine that cannot be started at all counts as one that died at once") {
    World world("unstartable");
    // Nothing scripted: every start fails.
    Supervisor supervisor(world.paths, world.hooks());
    supervisor.play(Settings{}, "");
    REQUIRE(eventually([&] { return !supervisor.active(); }));
    CHECK(world.starts() == 5);
    CHECK(supervisor.state().state == "failed");
    CHECK(slurp(world.paths.launcher_log).find("could not start the app: the script ran out") != std::string::npos);
}

TEST_CASE("supervisor: an engine that will not listen is ended") {
    World world("deaf");
    world.script = {{.life = -1, .deaf = true}};
    Supervisor supervisor(world.paths, world.hooks());
    supervisor.play(Settings{}, "");
    REQUIRE(eventually([&] { return supervisor.state().state == "running"; }));
    supervisor.stop();
    CHECK(world.kills == 1);
    CHECK(supervisor.state().state == "idle");
    // Killed at our asking is a stop, not a crash: nothing was started again.
    CHECK(world.starts() == 1);
}

TEST_CASE("supervisor: the stop marker a console wrote is honoured") {
    World world("marker");
    world.script = {{.life = 90, .code = 1}, {.life = -1}};
    // Moonshine.ps1 -Stop writes the marker and then kills the engine; the engine here
    // dies on its own script, with the marker already down.
    Supervisor::Hooks hooks = world.hooks();
    const auto spawn = hooks.spawn;
    hooks.spawn = [&, spawn](const Command& command, std::string* trouble) {
        touch(world.paths.stop_marker);
        return spawn(command, trouble);
    };
    Supervisor marked(world.paths, hooks);
    marked.play(Settings{}, "");
    REQUIRE(eventually([&] { return !marked.active(); }));
    CHECK(world.starts() == 1);
    CHECK(marked.state().state == "idle");
    CHECK_FALSE(fs::exists(world.paths.stop_marker));
}

TEST_CASE("supervisor: a second engine is never started beside one that will not stop") {
    World world("stray");
    world.strays_stop = false;
    world.script = {{.life = -1}};
    Supervisor supervisor(world.paths, world.hooks());
    supervisor.play(Settings{}, "");
    REQUIRE(eventually([&] { return !supervisor.active(); }));
    CHECK(world.starts() == 0);
    CHECK(world.games_started == 0);
    CHECK(supervisor.state().state == "failed");
}

TEST_CASE("supervisor: what the engine says it is doing is in the state while it runs") {
    World world("status");
    world.script = {{.life = -1,
                     .status = R"j({"phase":"talking","talking_to":"Innkeeper Betty","model":"Qwen3-4B (llama.cpp)",
                                    "remote_model":false,"voice":true,"ear":true,"game_log":true,"error":""})j"}};
    Supervisor supervisor(world.paths, world.hooks());
    supervisor.play(Settings{}, "");
    REQUIRE(eventually([&] { return supervisor.state().phase == "talking"; }));
    const EngineState state = supervisor.state();
    CHECK(state.state == "running");
    CHECK(state.talking_to == "Innkeeper Betty");
    CHECK(state.voice);
    CHECK(state.ear);
    CHECK(state.game_log);
    supervisor.stop();
    // And not after: the file of a run that is over says nothing about now.
    CHECK(supervisor.state().phase.empty());
}

TEST_CASE("supervisor: the game is looked for while the engine runs, and can be asked for again") {
    World world("game-again");
    world.script = {{.life = -1, .status = R"({"phase":"ready","game_log":true})"}};
    Supervisor supervisor(world.paths, world.hooks());
    // Before Play there is no engine for a game to be started beside.
    supervisor.start_game();
    CHECK(world.games_started == 0);

    supervisor.play(Settings{}, "");
    // Steam has been asked, and the game is not up yet: on its way.
    REQUIRE(eventually([&] { return supervisor.state().state == "running" && supervisor.state().game == "starting"; }));
    CHECK(world.games_started == 1);
    world.game_running = true;
    world.clock = world.clock + 3;
    REQUIRE(eventually([&] { return supervisor.state().game == "up"; }));
    // Asked for while it is up: nothing to do.
    supervisor.start_game();
    CHECK(world.games_started == 1);

    // The player quits the game and leaves Moonshine open. The engine's own status still
    // says it has the game's log, which is why that is not what is asked.
    world.game_running = false;
    world.clock = world.clock + 3;
    REQUIRE(eventually([&] { return supervisor.state().game == "down"; }));
    CHECK(supervisor.state().game_log);
    CHECK(supervisor.state().phase == "ready");

    // Play, from the window: Steam is asked again, and the engine is the one that was running.
    supervisor.start_game();
    CHECK(world.games_started == 2);
    CHECK(supervisor.state().game == "starting");
    CHECK(world.starts() == 1);
    CHECK(slurp(world.paths.launcher_log).find("asking Steam to start Kingdom Come: Deliverance 2 again") != std::string::npos);
    // Still on its way a minute later ...
    world.clock = world.clock + 60;
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK(supervisor.state().game == "starting");
    // ... and after long enough without it, it is not coming, and can be asked for again.
    world.clock = world.clock + 60;
    REQUIRE(eventually([&] { return supervisor.state().game == "down"; }));
    supervisor.start_game();
    CHECK(world.games_started == 3);
    world.game_running = true;
    world.clock = world.clock + 3;
    REQUIRE(eventually([&] { return supervisor.state().game == "up"; }));

    supervisor.stop();
    CHECK(supervisor.state().game.empty());
    supervisor.start_game();
    CHECK(world.games_started == 3);
}

TEST_CASE("supervisor: a game the player starts themselves is seen too, and where it cannot be seen nothing is said") {
    {
        World world("game-own");
        world.script = {{.life = -1, .status = R"({"phase":"ready"})"}};
        Settings settings;
        settings.launch_game = false;
        Supervisor supervisor(world.paths, world.hooks());
        supervisor.play(settings, "");
        REQUIRE(eventually([&] { return supervisor.state().game == "down"; }));
        world.game_running = true;
        world.clock = world.clock + 3;
        REQUIRE(eventually([&] { return supervisor.state().game == "up"; }));
        CHECK(world.games_started == 0);
        supervisor.stop();
    }
    {
        World world("game-unseen");
        world.can_see_game = false;
        world.script = {{.life = -1, .status = R"({"phase":"ready"})"}};
        Supervisor supervisor(world.paths, world.hooks());
        supervisor.play(Settings{}, "");
        REQUIRE(eventually([&] { return supervisor.state().phase == "ready"; }));
        world.clock = world.clock + 3;
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        CHECK(supervisor.state().game.empty());
        supervisor.stop();
    }
}

TEST_CASE("supervisor: each start puts the last run's log aside and clears its status") {
    World world("fresh-run");
    touch(world.paths.engine_log, "the run before");
    touch(world.paths.status, R"({"phase":"talking"})");
    world.script = {{.life = 60, .code = 0}};
    Supervisor supervisor(world.paths, world.hooks());
    supervisor.play(Settings{}, "");
    REQUIRE(eventually([&] { return !supervisor.active(); }));
    CHECK_FALSE(fs::exists(world.paths.engine_log));
    bool kept = false;
    for (const auto& entry : fs::directory_iterator(world.paths.logs)) {
        kept = kept || (entry.path().filename().string().rfind("moonshine_app.log.", 0) == 0 &&
                        slurp(entry.path()) == "the run before");
    }
    CHECK(kept);
    CHECK_FALSE(fs::exists(world.paths.status));
}

TEST_CASE("supervisor: Play while it is already playing changes nothing") {
    World world("twice");
    world.script = {{.life = -1}, {.life = -1}};
    Supervisor supervisor(world.paths, world.hooks());
    supervisor.play(Settings{}, "");
    REQUIRE(eventually([&] { return supervisor.state().state == "running"; }));
    supervisor.play(Settings{}, "");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(world.starts() == 1);
    supervisor.stop();
}

TEST_CASE("status: a file that is half written or not JSON is no status") {
    const fs::path home = fresh_home("status-file");
    EngineState state;
    CHECK_FALSE(read_status(home / "none.json", &state));
    touch(home / "half.json", "{\"phase\":\"rea");
    CHECK_FALSE(read_status(home / "half.json", &state));
    touch(home / "whole.json", R"({"phase":"ready","voice":true})");
    CHECK(read_status(home / "whole.json", &state));
    CHECK(state.phase == "ready");
    CHECK(state.voice);
    CHECK_FALSE(state.ear);
}
