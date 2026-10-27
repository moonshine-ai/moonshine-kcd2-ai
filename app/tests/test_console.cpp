#include "doctest.h"

#include <sqlite3.h>

#include <random>

#include "console.h"
#include "personadb.h"

using namespace moonshine;

namespace {

// A database with two stories and a favour standing alone, in three settlements, dealt in
// an order that is not the list's: the list has to put a story's scenes together under
// its first scene's settlement, and number them so.
class Db {
public:
    Db() {
        std::random_device seed;
        path_ = std::filesystem::temp_directory_path() /
                ("moonshine-console-" + std::to_string(seed()) + ".sqlite");
        sqlite3* db = nullptr;
        REQUIRE(sqlite3_open(path_.string().c_str(), &db) == SQLITE_OK);
        run(db,
            "CREATE TABLE npc (soul_id TEXT PRIMARY KEY, soul_name TEXT NOT NULL,"
            " occupation TEXT, social_class TEXT, settlement TEXT, region TEXT, gender TEXT);"
            "CREATE TABLE generated_name (soul_id TEXT PRIMARY KEY, full_name TEXT NOT NULL);"
            "CREATE TABLE social_place (place_id TEXT PRIMARY KEY, settlement TEXT, name TEXT);"
            "CREATE TABLE npc_trade (soul_name TEXT PRIMARY KEY, trade TEXT);"
            "INSERT INTO social_place VALUES ('p1','Kuttenberg','crowd'), ('p2','Suchdol','village'),"
            " ('p3','Sedletz','monastery');"
            "INSERT INTO npc VALUES"
            " ('g1','kkut_heinz','varlet','peasant','Kuttenberg','r','male'),"
            " ('g2','kkut_emmeram','butcher','burgher','Kuttenberg','r','male'),"
            " ('g3','kkut_kohl','townsman','burgher','Kuttenberg','r','male'),"
            " ('g4','kkut_amice','wench','peasant','Kuttenberg','r','female'),"
            " ('g5','ksuc_hugo','poacher','peasant','poachers','r','male'),"
            " ('g6','ksed_basil','monk','clergy','Sedletz','r','male');"
            "INSERT INTO npc_trade VALUES ('kkut_heinz','hired hand');"
            "CREATE TABLE npc_quest (quest_id TEXT PRIMARY KEY, kind TEXT, giver_soul TEXT,"
            " giver_name TEXT, target_soul TEXT, target_name TEXT, item_class TEXT,"
            " item_name TEXT, reward_groschen INTEGER, skill TEXT, difficulty TEXT);"
            "CREATE TABLE npc_quest_text (quest_id TEXT PRIMARY KEY, name TEXT, offer TEXT, thanks TEXT);"
            "CREATE TABLE npc_slate (quest_id TEXT PRIMARY KEY, chain_id TEXT, chain_order INTEGER,"
            " requires_quest_id TEXT, dispute INTEGER, reward_kind TEXT, reward_text TEXT,"
            " after_quest_id TEXT, after_how TEXT);"
            // Dealt: the fox story's last scene first, then Basil's lone favour, then Hugo's
            // (no settlement), then the fox ask, Amice's lone one, and the fox word scene.
            "INSERT INTO npc_quest VALUES"
            " ('fox-beat','beat','kkut_heinz','Heinz','kkut_emmeram','Emmeram','','',0,'strength','hard'),"
            " ('basil','retrieve','ksed_basil','Brother Basil','kkut_amice','Amice','cup-class','silver cup',20,'speech','easy'),"
            " ('hugo','ask','ksuc_hugo','Hugo','kkut_kohl','Henslin Kohl','','the truth',12,'speech','medium'),"
            " ('fox-ask','ask','kkut_heinz','Heinz','kkut_emmeram','Emmeram','','the truth',8,'speech','medium'),"
            " ('amice','pray','kkut_amice','Amice','','','','',5,'','easy'),"
            " ('fox-word','deliver','kkut_heinz','Heinz','kkut_kohl','Henslin Kohl','','',0,'speech','easy');"
            "INSERT INTO npc_quest_text VALUES"
            " ('fox-beat','A Dog''s Dinner','',''),"
            " ('basil','Fools and their Money','',''),"
            " ('hugo','Birds of a Feather','',''),"
            " ('fox-ask','Barking Stew','',''),"
            " ('fox-word','A Bone to Pick','','');"
            "INSERT INTO npc_slate VALUES"
            " ('fox-ask',NULL,1,NULL,0,'money','8 groschen',NULL,NULL),"
            " ('fox-word',NULL,2,NULL,0,'none','',  'fox-ask','truth'),"
            " ('fox-beat',NULL,3,NULL,0,'none','',  'fox-word',NULL),"
            " ('basil',NULL,1,NULL,0,'money','20 groschen',NULL,NULL),"
            " ('hugo',NULL,1,NULL,0,'money','12 groschen',NULL,NULL),"
            " ('amice',NULL,1,NULL,0,'money','5 groschen',NULL,NULL);"
            // Heinz's day, as the routine build dumps it: the bed with no window, two spots
            // of the crowd with the same window, and a variation that is not the default.
            "CREATE TABLE npc_routine (soul_name TEXT, level TEXT, slot INTEGER, start_min INTEGER,"
            " end_min INTEGER, is_default INTEGER, pos_x REAL, pos_y REAL, pos_z REAL);"
            "INSERT INTO npc_routine VALUES"
            " ('kkut_heinz','k',0,0,0,1,3129.3,730.0,55.2),"
            " ('kkut_heinz','k',1,420,1200,1,3117.24,746.66,64.8),"
            " ('kkut_heinz','k',2,420,1200,1,3117.24,746.66,64.8),"
            " ('kkut_heinz','k',3,480,1200,1,3112.47,740.07,63.8),"
            " ('kkut_heinz','k',4,600,700,0,9999.0,9999.0,0.0),"
            " ('kkut_heinz','k',5,NULL,NULL,1,NULL,NULL,NULL);");
        sqlite3_close(db);
    }
    ~Db() {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }
    const std::filesystem::path& path() const { return path_; }

private:
    static void run(sqlite3* db, const char* sql) {
        char* problem = nullptr;
        if (sqlite3_exec(db, sql, nullptr, nullptr, &problem) != SQLITE_OK) {
            const std::string message = problem ? problem : "unknown";
            sqlite3_free(problem);
            FAIL("setting up the test database: ", message);
        }
    }
    std::filesystem::path path_;
};

}  // namespace

TEST_CASE("the quest list groups a story under its first scene, by settlement, giver and title") {
    Db db;
    PersonaDb personas(db.path());
    REQUIRE(personas.ok());
    const std::vector<QuestListing> quests = personas.quest_list();
    REQUIRE(quests.size() == 6);
    // Kuttenberg first: Amice before Heinz by name; Heinz's three scenes in order; then
    // Sedletz; then the one whose giver has no settlement worth the name.
    CHECK(quests[0].id == "amice");
    CHECK(quests[1].id == "fox-ask");
    CHECK(quests[2].id == "fox-word");
    CHECK(quests[3].id == "fox-beat");
    CHECK(quests[4].id == "basil");
    CHECK(quests[5].id == "hugo");
    CHECK(quests[1].depth == 0);
    CHECK(quests[2].depth == 1);
    CHECK(quests[3].depth == 2);
    CHECK(quests[3].after_id == "fox-word");
    // The trade as it is said, not the game's label; the settlement only when it is one.
    CHECK(quests[1].giver_trade == "hired hand");
    CHECK(quests[1].target_trade == "butcher");
    CHECK(quests[1].giver_settlement == "Kuttenberg");
    CHECK(quests[5].giver_settlement.empty());
    CHECK(quests[0].target_soul.empty());
}

TEST_CASE("the routine spots are the default routine's, distinct, the windowless last") {
    Db db;
    PersonaDb personas(db.path());
    const std::vector<RoutineSpot> spots = personas.spots_of("kkut_heinz");
    REQUIRE(spots.size() == 3);
    CHECK(spots[0].start_min == 420);
    CHECK(spots[0].x == doctest::Approx(3117.24));
    CHECK(spots[1].start_min == 480);
    CHECK(spots[2].start_min == 0);
    CHECK(spots[2].end_min == 0);
    CHECK(spots[2].x == doctest::Approx(3129.3));
    CHECK(personas.spots_of("kkut_emmeram").empty());
}

TEST_CASE("the list is numbered, headed by settlement, and says what each quest is") {
    Db db;
    PersonaDb personas(db.path());
    Console console(personas);
    CHECK(Console::is_command("/quests"));
    CHECK(Console::is_command("  /goto 2"));
    CHECK_FALSE(Console::is_command("Good day to you"));
    CHECK_FALSE(Console::is_command(""));

    const ConsoleOutcome all = console.run("/quests");
    CHECK_FALSE(all.lua.has_value());
    CHECK(all.text.find("Kuttenberg\n") == 0);
    CHECK(all.text.find("[ 2] Barking Stew -- ask (the truth); Heinz (hired hand) -> Emmeram (butcher)  fox-ask") != std::string::npos);
    // A later scene is indented, says which it follows, and a word deliver says so.
    CHECK(all.text.find("[ 3]   A Bone to Pick -- deliver (word); after 2; Heinz (hired hand) -> Henslin Kohl (townsman)") != std::string::npos);
    CHECK(all.text.find("[ 4]     A Dog's Dinner -- beat; after 3;") != std::string::npos);
    CHECK(all.text.find("\nSedletz\n  [ 5] Fools and their Money") != std::string::npos);
    CHECK(all.text.find("\n(no settlement)\n  [ 6] Birds of a Feather") != std::string::npos);
    CHECK(all.text.find("6 of 6 quests.") != std::string::npos);

    // A word narrows it, case blind, and a settlement counts as a mention.
    const ConsoleOutcome some = console.run("/q bone");
    CHECK(some.text.find("[ 3]") != std::string::npos);
    CHECK(some.text.find("[ 2]") == std::string::npos);
    CHECK(some.text.find("1 of 6 quests.") != std::string::npos);
    CHECK(console.run("/quests sedletz").text.find("[ 5]") != std::string::npos);
    CHECK(console.run("/quests nothing-of-the-kind").text == "no quest mentions \"nothing-of-the-kind\".");
}

TEST_CASE("/goto writes a teleport to the giver, or the other party, by number") {
    Db db;
    PersonaDb personas(db.path());
    Console console(personas);

    const ConsoleOutcome giver = console.run("/goto 2");
    REQUIRE(giver.lua.has_value());
    CHECK(*giver.lua ==
          "return Moonshine.Teleport(\"kkut_heinz\", \"Heinz\", { { 420, 1200, 3117.24, 746.66, 64.80 },"
          " { 480, 1200, 3112.47, 740.07, 63.80 }, { 0, 0, 3129.30, 730.00, 55.20 } })");
    CHECK(giver.label == "to Heinz (hired hand), giver of 2 Barking Stew (kkut_heinz)");
    CHECK(giver.text.find("3 routine spots to fall back on") != std::string::npos);

    // The other party, with no routine on file.
    const ConsoleOutcome target = console.run("/g 2 target");
    REQUIRE(target.lua.has_value());
    CHECK(*target.lua == "return Moonshine.Teleport(\"kkut_emmeram\", \"Emmeram\", {})");
    CHECK(target.text.find("only if the game has them loaded") != std::string::npos);

    // Refusals: no number, a number off the list, a party the quest has not got, a word
    // that is neither end.
    CHECK_FALSE(console.run("/goto").lua.has_value());
    CHECK(console.run("/goto 7").text.find("not a quest number") != std::string::npos);
    CHECK(console.run("/goto two").text.find("not a quest number") != std::string::npos);
    CHECK(console.run("/goto 1 target").text.find("has no other party") != std::string::npos);
    CHECK(console.run("/goto 2 sideways").text.find("goes to the giver") != std::string::npos);
    // The unknown and the help.
    CHECK(console.run("/dance").text.find("is not a command") != std::string::npos);
    CHECK(console.run("/help").text.find("/goto N") != std::string::npos);
}

TEST_CASE("the teleport Lua quotes what needs quoting") {
    const std::string lua = Console::teleport_lua("kkut_x", "Eberhard \"the Hook\"", {});
    CHECK(lua == "return Moonshine.Teleport(\"kkut_x\", \"Eberhard \\\"the Hook\\\"\", {})");
}

TEST_CASE("an answer for the game's console is logged a line at a time, the Lua run after") {
    // Words only: each line its own Log, blank lines dropped, the label returned.
    ConsoleOutcome words{"Kuttenberg\n  [1] One -- ask\n\n2 of 2 quests.", std::nullopt, {}};
    CHECK(Console::game_lua(words) ==
          "Moonshine.Log(\"Kuttenberg\")\n"
          "Moonshine.Log(\"  [1] One -- ask\")\n"
          "Moonshine.Log(\"2 of 2 quests.\")\n"
          "return \"answered\"\n");
    // With Lua: run as a function so its line reaches the HUD too, and a quote in a line
    // survives.
    ConsoleOutcome go{"going to Eberhard \"the Hook\"", std::string("return Moonshine.Teleport(\"kkut_x\", \"E\", {})"), "to E"};
    const std::string lua = Console::game_lua(go);
    CHECK(lua.find("Moonshine.Log(\"going to Eberhard \\\"the Hook\\\"\")\n") == 0);
    CHECK(lua.find("local said = (function()\nreturn Moonshine.Teleport(\"kkut_x\", \"E\", {})\nend)()\n") != std::string::npos);
    CHECK(lua.find("Game.SendInfoText(tostring(said), true, nil, 5)") != std::string::npos);
    CHECK(lua.rfind("return said\n") == lua.size() - 12);
}
