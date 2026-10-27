// The shipped schema and the app's readers agree.
//
// data/schema.sql is what tools/assemble_db.py builds npcs.sqlite from, and the loaders in
// personadb.cpp and knowledge.cpp are written against it by name. Neither side is told
// when the other moves: PersonaDb answers a missing column with an empty field rather than
// an error, which is right for a database built before some stage existed and wrong for a
// schema that quietly renamed something. So: the schema is run as written, one row goes
// into every table with every column filled, and the app is asked to read each of them
// back. A column renamed on one side and not the other comes out here as an empty name,
// a missing greeting, a soul with nothing to ask.
//
// The other fixtures (test_personadb.cpp) make their own slimmer tables on purpose, to
// say which columns each behaviour depends on; this one is the whole table.
#include "doctest.h"

#include <sqlite3.h>

#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

#include "knowledge.h"
#include "personadb.h"

using namespace moonshine;

#ifndef MOONSHINE_SCHEMA_SQL
#error "MOONSHINE_SCHEMA_SQL (the path of data/schema.sql) must be defined for this test"
#endif

namespace {

std::string slurp(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE_MESSAGE(in.good(), "cannot read " << path.string());
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

class SchemaDb {
public:
    SchemaDb() {
        std::random_device seed;
        path_ = std::filesystem::temp_directory_path() /
                ("moonshine-schema-" + std::to_string(seed()) + ".sqlite");
        REQUIRE(sqlite3_open(path_.string().c_str(), &db_) == SQLITE_OK);
        exec(slurp(MOONSHINE_SCHEMA_SQL));
        fill();
    }
    ~SchemaDb() {
        sqlite3_close(db_);
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }
    const std::filesystem::path& path() const { return path_; }

    void exec(const std::string& sql) {
        char* err = nullptr;
        const int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err);
        const std::string why = err ? err : "";
        sqlite3_free(err);
        REQUIRE_MESSAGE(rc == SQLITE_OK, why);
    }

    // Every column is named, so that a column added to the schema without a value here
    // is a NOT NULL failure at insert, and a column dropped from it is an unknown name.
    void fill() {
        exec(
            "INSERT INTO npc (soul_id, soul_name, source_file, display_name, kind, is_named, "
            "is_unique, skald_character_name, social_class_id, social_class, occupation, "
            "soul_archetype_id, archetype, gender, race, faction_name, region, settlement, "
            "faction_category, faction_detail, combat_level, voice_group_name, soul_class, "
            "vip_class, inventory_preset, brain_id, teacher_skills) VALUES "
            "('guid-a','kkut_man_a','souls.xml','Varlet','human',0,0,'skald_a',3,'burgher',"
            " 'carpenter',1,'man','male','human','Kuttenberg','Kuttenberg region','Kuttenberg',"
            " 'town','','1.5','male_1','soul','','preset','brain',''),"
            "('guid-b','kkut_man_b','souls.xml','Varlet','human',0,0,NULL,3,'peasant',"
            " 'miner',1,'man','male','human','Kuttenberg','Kuttenberg region','Kuttenberg',"
            " 'town','','1.0','male_2','soul','','preset','brain','');"
            "INSERT INTO skald_character (skald_character_name, ui_name, full_name, gender, "
            "age, age_label, body_type, voice_id, voice_categories, profession, "
            "profession_design, master_role_name, mortality_id, unique_assets, is_generic) "
            "VALUES ('skald_a','Varlet','','male',30,'adult',1,42,'','carpenter','','',0,'',1);"
            "INSERT INTO generated_name (soul_id, given, particle, byname, title, full_name, "
            "style, pool, household_key, reason) VALUES "
            "('guid-a','Wenzel',NULL,NULL,NULL,'Wenzel','given','czech','h1','pool'),"
            "('guid-b','Julian',NULL,NULL,NULL,'Julian','given','czech','h2','pool');"
            "INSERT INTO npc_trade (soul_name, trade, why) VALUES "
            "('kkut_man_a','joiner','routine: joinery');"
            "INSERT INTO anim_activity (fragment, clause, activity, model, prompt_sha, made_at, "
            "issues) VALUES ('LumberJackSawVAR','sawing wood','working','m','sha','2026',NULL);"

            "INSERT INTO npc_bio (soul_name, soul_id, name, bio, dossier, words, issues, model, "
            "prompt_sha, dossier_sha, written_at) VALUES ('kkut_man_a','guid-a','Wenzel',"
            " 'Wenzel is a carpenter who lent his saw.','dossier',8,NULL,'m','p','d','2026');"
            "INSERT INTO npc_card (soul_name, card, words) VALUES "
            "('kkut_man_a','Carpenter without his saw.',4);"
            "INSERT INTO npc_hook (hook_id, kind, premise, soul_name, other_soul_name, problem, "
            "desire, secret) VALUES ('hook-a','retrieve','The saw is lent.','kkut_man_a',"
            " 'kkut_man_b','His saw is with Julian.','His saw back.',0);"
            "INSERT INTO npc_person (soul_name, circle_id, long_bio, self_account, card, words, "
            "issues, circle_sha, prompt_sha, model, written_at, remark_issues) VALUES "
            "('kkut_man_a','circle-a','A long life.','Wenzel. Carpenter, when I have my tools.',"
            " 'Carpenter without his saw.',4,NULL,'c','p','m','2026',NULL);"
            "INSERT INTO npc_circle_member (circle_id, soul_name, role, refers_to_quest_id, "
            "bridges) VALUES ('circle-a','kkut_man_a','giver',NULL,NULL),"
            " ('circle-a','kkut_man_b','target',NULL,NULL);"
            "INSERT INTO npc_absentee (circle_id, name, station, why_unmeetable, to_the_circle) "
            "VALUES ('circle-a','Old Hedvika','widow','dead','Wenzel''s mother');"
            "INSERT INTO npc_relationship (soul_id, soul_name, other_soul_id, other_soul_name, "
            "relationship, category, certainty, evidence) VALUES "
            "('guid-a','kkut_man_a','guid-b','kkut_man_b','brother','kin','certain',NULL);"
            "INSERT INTO social_place (place_id, kind, name, level, settlement, region, "
            "faction_name, hub_entity, pos, comment, house_trade, size, confidence) VALUES "
            "('p1','settlement','Kuttenberg crowd','kutnohorsko','Kuttenberg','Kuttenberg region',"
            " NULL,NULL,NULL,NULL,NULL,20,'high');"
            "INSERT INTO social_membership (soul_id, soul_name, place_id, role, confidence) "
            "VALUES ('guid-a','kkut_man_a','p1','member','high');"

            "INSERT INTO npc_default_greeting (soul_name, met, words, issues, model) VALUES "
            "('kkut_man_a',0,'What do you want?','','m'),('kkut_man_a',1,'You again.','','m');"
            "INSERT INTO npc_knowledge (soul_name, topic_key, label, trust, text, after_quest, "
            "after_how) VALUES ('kkut_man_a','julian_saw','Julian and the saw','friendly',"
            " 'Julian has had my saw since Candlemas.',NULL,NULL);"
            "INSERT INTO npc_seed_answer (soul_name, intent, variant, text, refers_to_quest_id) "
            "VALUES ('kkut_man_a','about',0,'Wenzel. Carpenter.',NULL),"
            " ('kkut_man_a','about',1,'Still Wenzel.',NULL),"
            " ('kkut_man_a','help',0,'No. I have my peace.',NULL);"
            "INSERT INTO npc_remark (soul_name, remark_key, row, text, place) VALUES "
            "('kkut_man_a','cold_work','Cold work, this.','Try it in January.','');"

            "INSERT INTO npc_kb_owner (soul_name, owner) VALUES ('kkut_man_a','kkut_man_a');"
            "INSERT INTO npc_kb_item (owner, item_id, scope, topic, kind, label, trust, "
            "price_kind, price_what, menu, when_tag, fact, answer, withheld, source, flag, "
            "answer_sha, inputs_sha, model, written_at) VALUES ('kkut_man_a','life:work',"
            " 'self','work','chat','His work','open',NULL,NULL,1,NULL,'He is a carpenter.',"
            " 'I cut and join.',NULL,'bio',NULL,'sha-answer','sha-in','m','2026');"
            "INSERT INTO npc_kb_question (owner, item_id, n, text, sha) VALUES "
            "('kkut_man_a','life:work',0,'What do you do?','sha-q');"
            "INSERT INTO npc_kb_vector (sha, dim, vec) VALUES "
            "('sha-answer',2,X'0000803F00000000'),('sha-q',2,X'000000000000803F');"

            "INSERT INTO npc_quest (quest_id, hook_id, kind, giver_soul, giver_name, giver_trade, "
            "target_soul, target_name, target_trade, item_class, item_name, reward_tier, "
            "reward_groschen, skill, difficulty) VALUES ('quest-a','hook-a','retrieve',"
            " 'kkut_man_a','Wenzel','carpenter','kkut_man_b','Julian','miner','saw-class',"
            " 'hand saw','low',8,'speech','easy');"
            "INSERT INTO npc_quest_text (quest_id, name, description, objective, log_started, "
            "log_completed, log_giver_dead, offer, thanks) VALUES ('quest-a','The Borrowed Saw',"
            " 'Julian has the saw.','Get it back.','Started.','Done.','Dead.',"
            " 'Julian has my saw. Eight groschen.','That is my saw. Here.');"
            "INSERT INTO npc_quest_lines (quest_id, field, text) VALUES "
            "('quest-a','greet_owed','You have my saw?');"
            "INSERT INTO npc_slate (quest_id, circle_id, kind, giver_soul, target_soul, item, "
            "item_class, reward_groschen, existing, chain_id, chain_order, requires_quest_id, "
            "dispute, twist, clue, reward_kind, reward_text, item_count, place, story_id, "
            "story_role, after_quest_id, after_how) VALUES ('quest-a','circle-a','retrieve',"
            " 'kkut_man_a','kkut_man_b','hand saw','saw-class',8,0,NULL,1,NULL,1,NULL,0,"
            " 'money','8 groschen',1,NULL,NULL,NULL,NULL,NULL);"

            "INSERT INTO npc_routine (soul_name, level, slot, start_min, end_min, minutes, "
            "variation_min, priority, priorities, is_default, tag, conditions, roles, behavior, "
            "behaviors, activity_family, hub_guid, hub_extras, place_id, place_name, pos_x, "
            "pos_y, pos_z, path) VALUES ('kkut_man_a','kutnohorsko',0,360,1080,720,0,1,'1',1,"
            " NULL,NULL,NULL,'Work','Work','working','hub-1',NULL,'p1','Kuttenberg crowd',"
            " 1000.0,2000.0,300.0,'hub-1');"
            "INSERT INTO npc_routine_hub (soul_name, level, hub_guid, class, dump_x, dump_y, "
            "dump_z, dumped_at) VALUES ('kkut_man_a','kutnohorsko','hub-1','NPCHub',"
            " 1000.0,2000.0,300.0,'2026');"
            "INSERT INTO npc_routine_object (level, guid, class, prefab, name, pos_x, pos_y, "
            "pos_z) VALUES ('kutnohorsko','hub-1','NPCHub',NULL,'hub',1000.0,2000.0,300.0);");
    }

private:
    std::filesystem::path path_;
    sqlite3* db_ = nullptr;
};

}  // namespace

TEST_CASE("the shipped schema is read whole by the app's loaders") {
    SchemaDb db;
    PersonaDb personas(db.path());
    REQUIRE_MESSAGE(personas.ok(), personas.trouble());

    CHECK(personas.named_souls() == 2);

    const std::optional<Soul> wenzel = personas.find("kkut_man_a");
    REQUIRE(wenzel.has_value());
    // npc, generated_name, npc_trade, skald_character
    CHECK(wenzel->name == "Wenzel");
    CHECK(wenzel->occupation == "carpenter");
    CHECK(wenzel->trade == "joiner");
    CHECK(wenzel->social_class == "burgher");
    CHECK(wenzel->settlement == "Kuttenberg");
    CHECK(wenzel->voice_id == 42);
    // npc_bio, npc_card, npc_default_greeting, npc_hook
    CHECK(wenzel->bio.find("lent his saw") != std::string::npos);
    CHECK(wenzel->card == "Carpenter without his saw.");
    CHECK(wenzel->greeting_first == "What do you want?");
    CHECK(wenzel->greeting_again == "You again.");
    // premise and problem, joined
    CHECK(wenzel->trouble == "The saw is lent. His saw is with Julian.");
    CHECK(wenzel->wants == "His saw back.");
    // npc_relationship, joined back through npc and generated_name
    REQUIRE(wenzel->kin.size() == 1);
    CHECK(wenzel->kin[0].relationship == "brother");
    CHECK(wenzel->kin[0].name == "Julian");
    // npc_person, npc_seed_answer, npc_knowledge, npc_remark
    CHECK(wenzel->self_account == "Wenzel. Carpenter, when I have my tools.");
    CHECK(wenzel->about_again == "Still Wenzel.");
    REQUIRE(wenzel->knowledge.size() == 1);
    CHECK(wenzel->help_replies.size() == 1);
    REQUIRE(wenzel->remarks.size() == 1);
    // npc_circle_member and npc_absentee: Julian by soul, Hedvika by name
    CHECK(wenzel->people.size() == 2);
    // npc_routine
    REQUIRE(wenzel->spots.size() == 1);
    CHECK(wenzel->spots[0].first == doctest::Approx(1000.0));
    CHECK(personas.spots_of("kkut_man_a").size() == 1);
    // npc_quest, npc_quest_text, npc_slate, npc_quest_lines
    REQUIRE(wenzel->quest.exists());
    CHECK(wenzel->quest.id == "quest-a");
    CHECK(wenzel->quests.size() == 1);
    CHECK_FALSE(personas.quest_set().empty());
    REQUIRE(personas.quest_list().size() == 1);
    CHECK(personas.quest_list()[0].title == "The Borrowed Saw");
    CHECK(personas.quest_list()[0].giver_settlement == "Kuttenberg");
    // anim_activity
    CHECK(personas.doing("LumberJackSawVAR").clause == "sawing wood");
    // social_place and social_membership: the settlement comes through them for a soul
    // whose npc.settlement is not a curated place
    const std::optional<Soul> julian = personas.find("kkut_man_b");
    REQUIRE(julian.has_value());
    CHECK(julian->name == "Julian");
}

TEST_CASE("the shipped schema carries the knowledge base") {
    SchemaDb db;
    KnowledgeBase base(db.path());
    REQUIRE_MESSAGE(base.ok(), base.trouble());
    CHECK(base.has_tables());
    CHECK(base.souls_with_items() == 1);
    const SoulIndex index = base.load("kkut_man_a");
    REQUIRE(index.size() == 1);
    CHECK(index.dim() == 2);
    // The answer's vector and the question's, both found by sha in npc_kb_vector.
    CHECK(index.vectors() == 2);
    REQUIRE(index.find("life:work") != nullptr);
    CHECK(index.find("life:work")->answer == "I cut and join.");
}
