// moonshine.cfg as the window reads and writes it (gui/settings.h).
//
// The file is not the window's alone: the installer writes the game's folder into it, the
// console launcher and the log collector read it, and people edit it by hand when asked
// to. So what is pinned down here is mostly what a save leaves alone.

#include "doctest.h"

#include <filesystem>
#include <fstream>
#include <sstream>

#include "page.h"
#include "settings.h"

using namespace moonshine::gui;

namespace {

std::filesystem::path scratch_file(const char* name) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "moonshine-gui-settings-tests";
    std::filesystem::create_directories(dir);
    const std::filesystem::path file = dir / name;
    std::filesystem::remove(file);
    return file;
}

std::string slurp_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

}  // namespace

TEST_CASE("settings: a file nobody has written is the defaults") {
    const Settings s = parse_settings("");
    CHECK(s.root.empty());
    CHECK(s.mic);
    CHECK(s.launch_game);
    // Opening Moonshine starts it, unless that has been turned off.
    CHECK(s.autostart);
    CHECK_FALSE(s.api);
    CHECK_FALSE(s.api_guessing);
    CHECK(s == Settings{});
}

TEST_CASE("settings: the file the installer writes, and what a person adds to it") {
    const Settings s = parse_settings(
        "# the game, as Steam records it; written by the installer\r\n"
        "root = D:\\SteamLibrary\\steamapps\\common\\KingdomComeDeliverance2\r\n"
        "  ARGS=--no-voice --threads 6  \r\n"
        "\r\n"
        "not a setting\r\n"
        "= nothing\r\n");
    CHECK(s.root == "D:\\SteamLibrary\\steamapps\\common\\KingdomComeDeliverance2");
    CHECK(s.args == "--no-voice --threads 6");
}

TEST_CASE("settings: the model, the switches, and the last line for a key winning") {
    const Settings s = parse_settings(
        "mic = off\n"
        "launch_game = no\n"
        "autostart = off\n"
        "model = API\n"
        "api_url = https://api.example.com/v1\n"
        "api_model = some-model\n"
        "api_extra = {\"reasoning_effort\": \"low\"}\n"
        "api_guessing = true\n"
        "api_model = the-later-one\n");
    CHECK_FALSE(s.mic);
    CHECK_FALSE(s.launch_game);
    CHECK_FALSE(s.autostart);
    CHECK(s.api);
    CHECK(s.api_url == "https://api.example.com/v1");
    CHECK(s.api_model == "the-later-one");
    CHECK(s.api_extra == "{\"reasoning_effort\": \"low\"}");
    CHECK(s.api_guessing);
}

TEST_CASE("settings: a switch that says neither yes nor no is left as it was") {
    CHECK(parse_settings("mic = perhaps\n").mic);
    CHECK(parse_settings("autostart = perhaps\n").autostart);
    CHECK_FALSE(parse_settings("api_guessing = perhaps\n").api_guessing);
}

TEST_CASE("settings: a save rewrites its own lines where they stand and leaves the rest") {
    const std::string before =
        "# the game, as Steam records it; written by the installer\r\n"
        "root = C:\\Old\\Place\r\n"
        "# asked to add this by a tester\r\n"
        "args = --no-voice\r\n"
        "future_setting = kept\r\n";
    Settings s = parse_settings(before);
    s.root = "D:\\New\\Place";
    s.mic = false;
    const std::string after = merge_settings(before, s);
    CHECK(after ==
          "# the game, as Steam records it; written by the installer\r\n"
          "root = D:\\New\\Place\r\n"
          "# asked to add this by a tester\r\n"
          "args = --no-voice\r\n"
          "future_setting = kept\r\n"
          "mic = off\r\n");
    CHECK(parse_settings(after) == s);
}

TEST_CASE("settings: what is at its default is not written, and is taken out when it goes back") {
    Settings s;
    CHECK(merge_settings("", s).empty());
    s.autostart = false;
    const std::string with = merge_settings("# mine\n", s);
    CHECK(with == "# mine\nautostart = off\n");
    s.autostart = true;
    CHECK(merge_settings(with, s) == "# mine\n");
    // A line that says what the default says, written by hand or by a version whose
    // default was the other way, is taken out too.
    CHECK(merge_settings("# mine\nautostart = on\n", s) == "# mine\n");
}

TEST_CASE("settings: a key written twice by hand ends up written once") {
    Settings s = parse_settings("root = A\nroot = B\n");
    CHECK(s.root == "B");
    s.root = "C";
    CHECK(merge_settings("root = A\nroot = B\n", s) == "root = C\n");
}

TEST_CASE("settings: the API's own settings outlive a switch back to the local model") {
    Settings s;
    s.api = true;
    s.api_url = "https://api.example.com/v1";
    s.api_model = "m";
    std::string text = merge_settings("", s);
    s.api = false;
    text = merge_settings(text, s);
    const Settings back = parse_settings(text);
    CHECK_FALSE(back.api);
    CHECK(back.api_url == "https://api.example.com/v1");
    CHECK(back.api_model == "m");
    // "model = local" is the default, and so is not said.
    CHECK(("\n" + text).find("\nmodel") == std::string::npos);
}

TEST_CASE("settings: a value is one line, whatever was pasted") {
    Settings s;
    s.api = true;
    s.api_extra = "{\"a\": 1,\n \"b\": 2}";
    const Settings back = parse_settings(merge_settings("", s));
    CHECK(back.api_extra == "{\"a\": 1,  \"b\": 2}");
}

TEST_CASE("settings: a folder with an accent in it is written so that PowerShell reads it too") {
    Settings s;
    s.root = "D:\\Hry\\Kr\xC3\xA1lovstv\xC3\xAD";
    const std::string text = merge_settings("", s);
    // UTF-8, announced by the mark Windows PowerShell looks for.
    CHECK(text.substr(0, 3) == "\xEF\xBB\xBF");
    CHECK(parse_settings(text).root == s.root);
    // And the mark is not doubled by the next save.
    const std::string again = merge_settings(text, s);
    CHECK(again == text);
}

TEST_CASE("settings: the file on disk, saved and read back") {
    const std::filesystem::path path = scratch_file("moonshine.cfg");
    {
        std::ofstream out(path, std::ios::binary);
        out << "# by hand\nroot = C:\\Games\\KCD2\n";
    }
    const SettingsFile file(path);
    Settings s = file.load();
    CHECK(s.root == "C:\\Games\\KCD2");
    s.api = true;
    s.api_url = "http://localhost:8080/v1";
    s.api_model = "local";
    std::string trouble;
    REQUIRE(file.save(s, &trouble));
    CHECK(file.load() == s);
    const std::string text = slurp_file(path);
    CHECK(text.find("# by hand") == 0);
    // There is no setting a key could be written under: it is not this file's to keep.
    CHECK(text.find("key") == std::string::npos);
    // Nothing left beside it.
    std::filesystem::path part = path;
    part += ".part";
    CHECK_FALSE(std::filesystem::exists(part));
}

TEST_CASE("settings: no file is the defaults, and a save makes one") {
    const std::filesystem::path path = scratch_file("fresh.cfg");
    const SettingsFile file(path);
    CHECK(file.load() == Settings{});
    Settings s;
    s.mic = false;
    REQUIRE(file.save(s));
    CHECK_FALSE(file.load().mic);
}

TEST_CASE("settings: extra options are taken apart as a command line is") {
    CHECK(split_args("").empty());
    CHECK(split_args("  --no-voice   --threads 6 ") == std::vector<std::string>{"--no-voice", "--threads", "6"});
    CHECK(split_args("--root \"C:\\Program Files\\Game\" --x") ==
          std::vector<std::string>{"--root", "C:\\Program Files\\Game", "--x"});
    CHECK(split_args("--say \"\"") == std::vector<std::string>{"--say", ""});
    CHECK(split_args("a\\\"b") == std::vector<std::string>{"a\"b"});
}

TEST_CASE("page: the three files become one, without the line that is only for a browser") {
    const std::string html =
        "<head>\n"
        "<link rel=\"stylesheet\" href=\"app.css\">\n"
        "</head><body>\n"
        "<script src=\"mock.js\" data-dev></script>\n"
        "<script src=\"app.js\"></script>\n"
        "</body>\n";
    const std::string page = inline_page(html, "body { color: red; }", "var a = '</script>';");
    CHECK(page.find("mock.js") == std::string::npos);
    CHECK(page.find("href=\"app.css\"") == std::string::npos);
    CHECK(page.find("<style>\nbody { color: red; }\n</style>") != std::string::npos);
    // A script would end at the first closing tag inside it.
    CHECK(page.find("var a = '<\\/script>';") != std::string::npos);
    CHECK(page.find("</body>") != std::string::npos);
}
