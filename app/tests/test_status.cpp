// What the app says to the window that started it (status.h): the installation check as a
// list, and the snapshot of what it is doing.
//
// The prose form of the check is tested for its shape because two things outside this
// program read it: Collect-Logs.ps1 takes the game's folder off the "game root:" line, and
// testers are told that the last line is `ready`.

#include "doctest.h"

#include <filesystem>
#include <fstream>
#include <sstream>

#include "json.hpp"
#include "status.h"

using namespace moonshine;
using json = nlohmann::json;

namespace {

std::string slurp(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

std::filesystem::path scratch(const char* name) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "moonshine-status-tests";
    std::filesystem::create_directories(dir);
    const std::filesystem::path path = dir / name;
    std::filesystem::remove(path);
    return path;
}

}  // namespace

TEST_CASE("a check with nothing missing ends in ready, in the shape it has always had") {
    CheckReport report;
    report.ok("cwd", "C:\\Users\\adam\\AppData\\Local\\Moonshine");
    report.ok("game root", "D:\\SteamLibrary\\steamapps\\common\\KingdomComeDeliverance2");
    report.note("turns will be written to:");
    report.note("  \"D:\\turns\\turn_1.txt\"");
    report.optional("embedder", "none found; souls answer from the sheet alone");

    CHECK(report.ready());
    const std::string text = report.text();
    // The name padded to one column of eleven, as Collect-Logs.ps1's pattern expects.
    CHECK(text.find("cwd:       C:\\Users\\adam") == 0);
    CHECK(text.find("\ngame root: D:\\SteamLibrary\\steamapps\\common\\KingdomComeDeliverance2\n") !=
          std::string::npos);
    CHECK(text.find("\nturns will be written to:\n  \"D:\\turns\\turn_1.txt\"\n") != std::string::npos);
    CHECK(text.find("MISSING") == std::string::npos);
    CHECK(text.substr(text.size() - 6) == "ready\n");
}

TEST_CASE("one required thing missing is NOT READY, and is marked where it is said") {
    CheckReport report;
    report.ok("world", "setting.txt (0 MB)");
    report.missing("model", "models\\llm\\qwen.gguf");
    report.optional("ear", "off (--no-mic); the say row is typed into");

    CHECK_FALSE(report.ready());
    const std::string text = report.text();
    CHECK(text.find("model:     models\\llm\\qwen.gguf -- MISSING\n") != std::string::npos);
    // Done without is not missing: the app runs, and says so itself.
    CHECK(text.find("typed into -- MISSING") == std::string::npos);
    CHECK(text.find("NOT READY: something required is missing, see above\n") != std::string::npos);
}

TEST_CASE("the same check as JSON carries a state for every item and leaves the notes out") {
    CheckReport report;
    report.ok("game root", "D:\\Games\\KCD2");
    report.game_root = "D:\\Games\\KCD2";
    report.note("turns will be written to:");
    report.missing("personas", "data/kcd2/npcs.sqlite");
    report.optional("lexicon", "no lexicon.tsv beside the voices");

    const json parsed = json::parse(report.json());
    CHECK(parsed["ready"] == false);
    CHECK(parsed["game_root"] == "D:\\Games\\KCD2");
    REQUIRE(parsed["items"].size() == 3);
    CHECK(parsed["items"][0] == json{{"name", "game root"}, {"text", "D:\\Games\\KCD2"}, {"state", "ok"}});
    CHECK(parsed["items"][1]["state"] == "missing");
    CHECK(parsed["items"][2]["state"] == "optional");
}

TEST_CASE("a path that is not UTF-8 does not stop the check being JSON") {
    // A folder named in the machine's code page: the bytes are what the filesystem gave.
    CheckReport report;
    report.ok("game root", std::string("D:\\Hry\\Kr\xE1lovstv\xED"));
    CHECK_NOTHROW((void)json::parse(report.json()));
}

TEST_CASE("a snapshot is written at once, and again only when something changed") {
    const std::filesystem::path path = scratch("status.json");
    StatusFile file(path);

    file.update([](Status& status) { status.phase = "loading-model"; });
    REQUIRE(std::filesystem::exists(path));
    json parsed = json::parse(slurp(path));
    CHECK(parsed["phase"] == "loading-model");
    CHECK(parsed["talking_to"] == "");
    CHECK(parsed["error"] == "");
    CHECK(parsed["pid"].get<long>() > 0);

    // The same again: nothing to write, which a loop calling this every pass relies on.
    std::filesystem::remove(path);
    file.update([](Status& status) { status.phase = "loading-model"; });
    CHECK_FALSE(std::filesystem::exists(path));

    file.update([](Status& status) {
        status.phase = "talking";
        status.talking_to = "Cross-eyed Adam";
        status.voice = true;
    });
    parsed = json::parse(slurp(path));
    CHECK(parsed["phase"] == "talking");
    CHECK(parsed["talking_to"] == "Cross-eyed Adam");
    CHECK(parsed["voice"] == true);
    CHECK(parsed["ear"] == false);
    // Nothing is left beside it.
    std::filesystem::path part = path;
    part += ".part";
    CHECK_FALSE(std::filesystem::exists(part));
}

TEST_CASE("the first update writes even when it changes nothing") {
    // "starting" is the default, and the window still wants a file saying so.
    const std::filesystem::path path = scratch("first.json");
    StatusFile file(path);
    file.update([](Status&) {});
    REQUIRE(std::filesystem::exists(path));
    CHECK(json::parse(slurp(path))["phase"] == "starting");
}

TEST_CASE("with no file named, a snapshot is kept and nothing is written") {
    StatusFile file{std::filesystem::path{}};
    CHECK_FALSE(file.on());
    file.update([](Status& status) { status.error = "no network"; });
    CHECK(file.now().error == "no network");
}
