#include "turnfile.h"

#include <cstdlib>
#include <fstream>

namespace moonshine {

namespace {

namespace fs = std::filesystem;

constexpr std::string_view kGameDir = "KingdomComeDeliverance2";

// Extensions an earlier build wrote alongside the turn file, before the words were
// the file. Swept too, or a stale one is found in preference.
constexpr std::string_view kStaleExtensions[] = {".xml", ".lua"};

std::string env(const char* name) {
    const char* value = std::getenv(name);
    return value ? std::string(value) : std::string();
}

// Steam's default library, then the other places a second library commonly sits.
// A library folder listed only in libraryfolders.vdf is not parsed here; when this
// fails the app asks rather than guessing wrong.
std::vector<fs::path> steam_roots() {
    std::vector<fs::path> roots;
#ifdef _WIN32
    for (const char* var : {"ProgramFiles(x86)", "ProgramFiles"}) {
        const std::string base = env(var);
        if (!base.empty()) roots.push_back(fs::path(base) / "Steam");
    }
    for (const char* drive : {"C:", "D:", "E:"}) {
        roots.push_back(fs::path(drive) / "\\SteamLibrary");
        roots.push_back(fs::path(drive) / "\\Steam");
    }
#else
    const std::string home = env("HOME");
    if (!home.empty()) {
        roots.push_back(fs::path(home) / ".steam/steam");
        roots.push_back(fs::path(home) / ".local/share/Steam");
        roots.push_back(fs::path(home) / "Library/Application Support/Steam");
    }
#endif
    return roots;
}

fs::path find_game() {
    std::error_code ec;
    for (const fs::path& root : steam_roots()) {
        const fs::path candidate = root / "steamapps" / "common" / kGameDir;
        if (fs::exists(candidate / "kcd.log", ec) || fs::is_directory(candidate, ec)) {
            return candidate;
        }
    }
    return {};
}

fs::path find_user() {
    std::error_code ec;
#ifdef _WIN32
    // The engine writes under the real Saved Games folder on Windows. USERPROFILE
    // rather than the shell API keeps this testable and header-light.
    const std::string home = env("USERPROFILE");
    if (!home.empty()) {
        const fs::path saved = fs::path(home) / "Saved Games" / "kingdomcome2";
        if (fs::is_directory(saved, ec) || !fs::exists(saved, ec)) return saved;
    }
#else
    // Under Proton the same folder lives inside the prefix.
    const std::string home = env("HOME");
    if (!home.empty()) {
        return fs::path(home) /
               ".local/share/Steam/steamapps/compatdata/1771300/pfx/drive_c/users/steamuser/"
               "Saved Games/kingdomcome2";
    }
#endif
    return {};
}

// One line, because the mod reads one line.
//
// A model will put a break in a reply sooner or later, and the mod says the first
// line and drops the rest without complaining, so the second half of a sentence just
// never gets spoken. Flattened here rather than in the model layer: this is where the
// one-line-per-turn rule comes from, so it is where it belongs.
std::string one_line(std::string_view words) {
    std::string flat;
    flat.reserve(words.size());
    bool spacing = false;
    for (char c : words) {
        const bool blank = c == '\n' || c == '\r' || c == '\t' || c == ' ';
        if (blank) {
            // A run of whitespace becomes one space, so a paragraph break does not
            // leave a gap in the middle of a spoken line.
            spacing = !flat.empty();
            continue;
        }
        if (spacing) {
            flat.push_back(' ');
            spacing = false;
        }
        flat.push_back(c);
    }
    return flat;
}

bool write_one(const fs::path& path, const std::string& body) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);

    // Written beside the target and moved into place, so the game can never read a
    // file that is half written.
    const fs::path temp = path.string() + ".part";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << body;
        if (!out) return false;
    }
    fs::rename(temp, path, ec);
    if (ec) {
        fs::remove(temp, ec);
        return false;
    }
    return true;
}

}  // namespace

GamePaths discover() {
    GamePaths paths;
    paths.root = find_game();
    paths.user = find_user();
    return paths;
}

fs::path log_path(const GamePaths& paths) {
    return paths.root / "kcd.log";
}

std::vector<fs::path> turn_paths(const GamePaths& paths, int turn) {
    const std::string name = "turn_" + std::to_string(turn) + ".txt";
    return {
        paths.user / "moonshine" / name,
        paths.root / "moonshine" / name,
        paths.root / "Data" / "moonshine" / name,
        paths.root / "mods" / paths.modid / "Data" / "moonshine" / name,
        paths.root / "mods" / paths.modid / "moonshine" / name,
    };
}

std::vector<fs::path> alive_paths(const GamePaths& paths, long long bucket) {
    const std::string name = "alive_" + std::to_string(bucket) + ".txt";
    std::vector<fs::path> out;
    for (const fs::path& turn : turn_paths(paths, 1)) out.push_back(turn.parent_path() / name);
    return out;
}

std::vector<fs::path> hook_paths(const GamePaths& paths, int hook) {
    const std::string name = "hook_" + std::to_string(hook) + ".txt";
    std::vector<fs::path> out;
    for (const fs::path& turn : turn_paths(paths, 1)) out.push_back(turn.parent_path() / name);
    return out;
}

bool TurnWriter::hook(int hook, std::string_view lua) {
    // Lua as given, newlines and all: loadstring takes a chunk, not a line.
    const std::string body = std::string(lua) + "\n";
    bool any = false;
    for (const fs::path& path : hook_paths(paths_, hook)) {
        if (write_one(path, body)) any = true;
    }
    return any;
}

void TurnWriter::forget_hook(int hook) {
    std::error_code ec;
    for (const fs::path& path : hook_paths(paths_, hook)) {
        fs::remove(path, ec);
        fs::remove(fs::path(path.string() + ".part"), ec);
    }
}

bool TurnWriter::lip(std::string_view file, std::string_view body) {
    bool any = false;
    for (const fs::path& turn : turn_paths(paths_, 1)) {
        if (write_one(turn.parent_path() / std::string(file), std::string(body))) any = true;
    }
    return any;
}

void TurnWriter::forget_lips(const std::vector<std::string>& files) {
    std::error_code ec;
    for (const std::string& file : files) {
        for (const fs::path& turn : turn_paths(paths_, 1)) {
            const fs::path path = turn.parent_path() / file;
            fs::remove(path, ec);
            fs::remove(fs::path(path.string() + ".part"), ec);
        }
    }
}

TurnWriter::TurnWriter(GamePaths paths) : paths_(std::move(paths)) {}

void TurnWriter::beat(long long now_seconds) {
    const long long bucket = now_seconds / kAliveBucketSeconds;
    if (bucket == beaten_) return;
    const std::string body = "alive " + std::to_string(now_seconds) + "\n";
    for (const fs::path& path : alive_paths(paths_, bucket)) write_one(path, body);
    // The ones behind, so the folder does not fill with a file every five minutes. Two
    // back and further: the previous bucket is one the mod still accepts as alive.
    std::error_code ec;
    for (long long old = bucket - 2; old >= bucket - 4; --old) {
        for (const fs::path& path : alive_paths(paths_, old)) fs::remove(path, ec);
    }
    beaten_ = bucket;
}

void TurnWriter::hush(long long now_seconds) {
    const long long bucket = now_seconds / kAliveBucketSeconds;
    std::error_code ec;
    for (long long b = bucket - 1; b <= bucket + 1; ++b) {
        for (const fs::path& path : alive_paths(paths_, b)) fs::remove(path, ec);
    }
    beaten_ = -1;
}

bool TurnWriter::write(int turn, std::string_view words) {
    // The mod hands what it finds straight to Game.SendInfoText, which takes the
    // text rather than a name for it, so a trailing newline is all the framing.
    const std::string body = one_line(words) + "\n";

    bool any = false;
    for (const fs::path& path : turn_paths(paths_, turn)) {
        if (write_one(path, body)) any = true;
    }
    return any;
}

void TurnWriter::forget(int turn) {
    std::error_code ec;
    for (const fs::path& path : turn_paths(paths_, turn)) {
        fs::remove(path, ec);
        fs::remove(fs::path(path.string() + ".part"), ec);
        for (std::string_view extension : kStaleExtensions) {
            fs::path stale = path;
            stale.replace_extension(std::string(extension));
            fs::remove(stale, ec);
        }
    }
}

void TurnWriter::sweep(int upto) {
    // Ascending, and turn 1 therefore first. After a relaunch the mod counts from
    // one again and looks for turn 1 within a beat, so that is both the file most
    // likely to be read stale and the one worth removing first.
    for (int turn = 1; turn <= upto; ++turn) forget(turn);
    // And every heartbeat left by an app that did not get to take its own away: harmless
    // to the mod, which looks only at the clock's current bucket, but not worth keeping.
    // Hooks too: one left from an earlier run is Lua the mod would run again, numbered
    // from one, the moment the next run's console pump looked.
    std::error_code ec;
    for (const fs::path& turn : turn_paths(paths_, 1)) {
        const fs::path folder = turn.parent_path();
        if (!fs::is_directory(folder, ec)) continue;
        for (const fs::directory_entry& entry : fs::directory_iterator(folder, ec)) {
            const std::string name = entry.path().filename().string();
            if (entry.path().extension() != ".txt") continue;
            if (name.rfind("alive_", 0) == 0 || name.rfind("hook_", 0) == 0 ||
                name.rfind("lip_", 0) == 0) {
                fs::remove(entry.path(), ec);
            }
        }
    }
    beaten_ = -1;
}

}  // namespace moonshine
