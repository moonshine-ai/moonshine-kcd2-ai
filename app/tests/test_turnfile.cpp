#include "doctest.h"

#include <fstream>
#include <iterator>

#include "fake_game.h"
#include "turnfile.h"

using namespace moonshine;

TEST_CASE("the file holds the words and nothing else") {
    FakeGame game;
    TurnWriter writer(game.paths());
    REQUIRE(writer.write(1, "Good day to you, sir."));

    // The mod hands what it reads straight to Game.SendInfoText. Any wrapper, id or
    // table around the words would be spoken aloud.
    std::ifstream in(game.paths().user / "moonshine" / "turn_1.txt", std::ios::binary);
    REQUIRE(in);
    std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(body == "Good day to you, sir.\n");
}

TEST_CASE("a turn keeps its bytes, which is what Czech needs") {
    FakeGame game;
    TurnWriter writer(game.paths());
    // Nearly every name in this game has a diacritic in it, so a turn file that
    // translates anything on the way out is a turn file that cannot say them. Written
    // in hex rather than as literals so the check does not depend on the encoding of
    // this source file or on what MSVC decides to do with it.
    const std::string czech =
        "Dob\xc3\xbd den, Hanu\xc5\xa1. Hav\xc3\xad\xc5\x99sk\xc3\xa1 ulice.";
    REQUIRE(writer.write(1, czech));

    std::ifstream in(game.paths().user / "moonshine" / "turn_1.txt", std::ios::binary);
    REQUIRE(in);
    const std::string body((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    CHECK(body == czech + "\n");
}

TEST_CASE("a newline in a reply cannot turn one turn into two") {
    FakeGame game;
    TurnWriter writer(game.paths());
    // The mod reads the first line and says it. A reply with a line break in it used
    // to lose everything after the break, silently.
    REQUIRE(writer.write(1, "God be with you.\nWhat do you want?"));

    const std::optional<std::string> said = game.play();
    REQUIRE(said.has_value());
    CHECK(*said == "God be with you. What do you want?");
}

TEST_CASE("a reply padded with blank lines does not come out gap-toothed") {
    FakeGame game;
    TurnWriter writer(game.paths());
    writer.write(1, "\n\n  God be with you.\r\n\r\n\tWhat do you want?  \n");

    // One space where each run of whitespace was, and nothing hanging off either end.
    CHECK(game.play() == "God be with you. What do you want?");
}

TEST_CASE("a turn is written everywhere the mod looks") {
    FakeGame game;
    TurnWriter writer(game.paths());
    REQUIRE(writer.write(2, "I have seen better times."));

    for (const std::filesystem::path& path : turn_paths(game.paths(), 2)) {
        CHECK(std::filesystem::exists(path));
    }
}

TEST_CASE("each turn gets its own name") {
    FakeGame game;
    TurnWriter writer(game.paths());
    writer.write(1, "First.");
    writer.write(2, "Second.");

    // The game caches what it reads by path, so a name that has been read once
    // cannot be given different words.
    CHECK(game.play() == "First.");
    CHECK(game.play() == "Second.");
}

TEST_CASE("nothing half written is ever left where the game reads") {
    FakeGame game;
    TurnWriter writer(game.paths());
    writer.write(1, "Good day to you, sir.");

    // Written beside the target and moved into place, so the part file is gone and
    // the mod cannot find it in preference.
    for (const std::filesystem::path& path : turn_paths(game.paths(), 1)) {
        CHECK_FALSE(std::filesystem::exists(path.string() + ".part"));
    }
}

TEST_CASE("a crashed run leaves nothing for the next one to race through") {
    FakeGame game;
    game.strand(1, "words from a run that died");
    game.strand(2, "and the turn after it");
    REQUIRE(game.has_turn(1));

    // One stale file is enough for the mod to walk forward through turns it can
    // already see and outrun whatever is being said.
    TurnWriter writer(game.paths());
    writer.sweep(8);
    CHECK_FALSE(game.has_turn(1));
    CHECK_FALSE(game.has_turn(2));
    CHECK_FALSE(game.play().has_value());
}

TEST_CASE("a turn can be dropped on its own, once it has been said") {
    FakeGame game;
    TurnWriter writer(game.paths());
    writer.write(1, "God be with you.");
    writer.write(2, "Fare well.");

    writer.forget(1);
    CHECK_FALSE(game.has_turn(1));
    // Only that one. Turn 2 has been written but not yet said.
    CHECK(game.has_turn(2));
}

TEST_CASE("what an older build wrote alongside is swept too") {
    FakeGame game;
    const std::filesystem::path dir = game.paths().user / "moonshine";
    std::filesystem::create_directories(dir);
    for (const char* name : {"turn_1.xml", "turn_1.lua"}) {
        std::ofstream out(dir / name);
        out << "a localization row from when a turn was one\n";
    }

    TurnWriter(game.paths()).sweep(4);
    CHECK_FALSE(std::filesystem::exists(dir / "turn_1.xml"));
    CHECK_FALSE(std::filesystem::exists(dir / "turn_1.lua"));
}

TEST_CASE("the heartbeat is a file named by the clock's bucket, beside the turns") {
    FakeGame game;
    TurnWriter writer(game.paths());
    const long long now = 1790000000;
    const long long bucket = now / kAliveBucketSeconds;
    writer.beat(now);
    for (const std::filesystem::path& path : alive_paths(game.paths(), bucket)) {
        CHECK(std::filesystem::exists(path));
    }
    // Written beside the turn files, where the mod swaps turn_#.txt for the name.
    CHECK(alive_paths(game.paths(), bucket)[0].parent_path() == turn_paths(game.paths(), 1)[0].parent_path());
    // Into the next bucket, and the one two behind goes.
    writer.beat(now - 2 * kAliveBucketSeconds);  // an older bucket, as if left from before
    writer.beat(now + kAliveBucketSeconds);
    CHECK(std::filesystem::exists(alive_paths(game.paths(), bucket + 1)[0]));
    CHECK(std::filesystem::exists(alive_paths(game.paths(), bucket)[0]));
    CHECK_FALSE(std::filesystem::exists(alive_paths(game.paths(), bucket - 2)[0]));
    // Stopping on purpose takes the current and neighbouring ones away.
    writer.hush(now + kAliveBucketSeconds);
    CHECK_FALSE(std::filesystem::exists(alive_paths(game.paths(), bucket + 1)[0]));
    CHECK_FALSE(std::filesystem::exists(alive_paths(game.paths(), bucket)[0]));
    // And a sweep clears whatever an app that died left.
    writer.beat(now - 10 * kAliveBucketSeconds);
    writer.sweep(1);
    CHECK_FALSE(std::filesystem::exists(alive_paths(game.paths(), bucket - 10)[0]));
}

TEST_CASE("a hook is Lua beside the turns, taken away once run and at a sweep") {
    FakeGame game;
    TurnWriter writer(game.paths());
    CHECK(writer.hook(2, "return Moonshine.Teleport(\"kkut_heinz\", \"Heinz\", {})"));
    const std::vector<std::filesystem::path> paths = hook_paths(game.paths(), 2);
    CHECK(paths[0].filename() == "hook_2.txt");
    CHECK(paths[0].parent_path() == turn_paths(game.paths(), 1)[0].parent_path());
    for (const std::filesystem::path& path : paths) CHECK(std::filesystem::exists(path));
    // As given, with a newline, since loadstring takes a chunk and not a line.
    std::string body;
    {
        // Closed before the file is taken away: Windows will not remove an open one.
        std::ifstream in(paths[0]);
        body.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    CHECK(body == "return Moonshine.Teleport(\"kkut_heinz\", \"Heinz\", {})\n");
    writer.forget_hook(2);
    CHECK_FALSE(std::filesystem::exists(paths[0]));
    writer.hook(3, "return 1");
    writer.sweep(1);
    CHECK_FALSE(std::filesystem::exists(hook_paths(game.paths(), 3)[0]));
}

TEST_CASE("a turn nobody can write is reported rather than assumed") {
    BlockedPaths blocked;
    // Reported, because a conversation where the player talks and nothing comes
    // back should say why.
    CHECK_FALSE(TurnWriter(blocked.paths()).write(1, "Good day to you."));
}

TEST_CASE("the log sits beside the executable") {
    GamePaths paths;
    paths.root = "/games/KingdomComeDeliverance2";
    CHECK(log_path(paths).filename() == "kcd.log");
    CHECK(log_path(paths).parent_path() == paths.root);
}

TEST_CASE("the mod id is part of the path, so a renamed mod is still found") {
    GamePaths paths;
    paths.root = "/games/KingdomComeDeliverance2";
    paths.user = "/user";
    paths.modid = "something_else";

    bool seen = false;
    for (const std::filesystem::path& path : turn_paths(paths, 1)) {
        if (path.string().find("something_else") != std::string::npos) seen = true;
    }
    CHECK(seen);
}
