#include "doctest.h"

#include <sqlite3.h>

#include <random>

#include "personadb.h"

using namespace moonshine;

namespace {

// A real database with the real schema, rather than a stubbed lookup.
//
// The whole point of this layer is the joins: the game knows soul_name, generated_name
// is keyed on soul_id, and npc_bio and npc_relationship are keyed on soul_name again.
// A fake that answered by name would exercise none of that, and getting those hops
// wrong is exactly how the wrong person's sheet reaches a conversation.
class Db {
public:
    Db() {
        std::random_device seed;
        path_ = std::filesystem::temp_directory_path() /
                ("moonshine-personas-" + std::to_string(seed()) + ".sqlite");
        sqlite3* db = nullptr;
        REQUIRE(sqlite3_open(path_.string().c_str(), &db) == SQLITE_OK);
        run(db,
            "CREATE TABLE npc (soul_id TEXT PRIMARY KEY, soul_name TEXT NOT NULL,"
            " occupation TEXT, social_class TEXT, settlement TEXT, region TEXT, gender TEXT);"
            "CREATE TABLE generated_name (soul_id TEXT PRIMARY KEY, full_name TEXT NOT NULL);"
            "CREATE TABLE npc_bio (soul_name TEXT PRIMARY KEY, bio TEXT, issues TEXT);"
            "CREATE TABLE npc_relationship (soul_name TEXT, other_soul_name TEXT,"
            " relationship TEXT);"
            "CREATE TABLE npc_hook (soul_name TEXT, premise TEXT, problem TEXT, desire TEXT);"
            "CREATE TABLE npc_card (soul_name TEXT PRIMARY KEY, card TEXT, words INTEGER);"
            "CREATE TABLE npc_default_greeting (soul_name TEXT, met INTEGER, words TEXT,"
            " PRIMARY KEY (soul_name, met));"
            "CREATE TABLE social_place (place_id TEXT PRIMARY KEY, settlement TEXT, name TEXT);"
            "CREATE TABLE social_membership (soul_name TEXT, place_id TEXT);"
            "CREATE TABLE anim_activity (fragment TEXT PRIMARY KEY, clause TEXT,"
            " activity TEXT, issues TEXT);"

            // A handful of the 2,016 the generator writes, spelled exactly as the engine
            // returns them. The death one is here because the table stores every animation
            // a person can play, catchable or not, and the app has to drop those.
            "INSERT INTO anim_activity VALUES"
            " ('LumberJackSawVAR','sawing wood','working',NULL),"
            " ('SittingIdle','sitting doing nothing','resting',NULL),"
            " ('Quest_CombatAttackComboDeath','','none',NULL),"
            " ('SpaWashToBath','WashingUpInATub','washing','clause carries the animation''s"
            " own spelling');"

            // The curated places. npc.settlement is only believed when it names one of
            // these, because in the real data it holds encounter groups just as often.
            "INSERT INTO social_place VALUES ('p1','Kuttenberg','Kuttenberg crowd'),"
            " ('p2','Suchdol','Suchdol village');"

            // Bartel, the varlet the player actually met in Kuttenberg.
            "INSERT INTO npc VALUES ('guid-47','kkut_additive_man_47','varlet','varlet',"
            " 'Kuttenberg','Kuttenberg region','male');"
            "INSERT INTO generated_name VALUES ('guid-47','Bartel');"
            "INSERT INTO npc_hook VALUES ('kkut_additive_man_47',"
            " 'Bartel''s roof is failing and the man who owns it will not mend it.',"
            " 'Bartel wakes in the wet whenever it rains.',"
            " 'somewhere dry to sleep before the weather turns.');"

            // People he knows, one of whom was never given a name.
            "INSERT INTO npc VALUES ('guid-45','kkut_additive_man_45','varlet','varlet',"
            " 'Kuttenberg','Kuttenberg region','male');"
            "INSERT INTO generated_name VALUES ('guid-45','Julian');"
            "INSERT INTO npc VALUES ('guid-46','kkut_additive_man_46','miner','peasant',"
            " 'Kuttenberg','Kuttenberg region','male');"
            "INSERT INTO generated_name VALUES ('guid-46','Frantishek');"
            "INSERT INTO npc VALUES ('guid-99','kkut_nameless_99','miner','peasant',"
            " 'Kuttenberg','Kuttenberg region','male');"
            "INSERT INTO npc_relationship VALUES"
            " ('kkut_additive_man_47','kkut_additive_man_45','acquaintance'),"
            " ('kkut_additive_man_47','kkut_additive_man_46','brother'),"
            " ('kkut_additive_man_47','kkut_nameless_99','acquaintance');"

            // A soul with a written background.
            "INSERT INTO npc VALUES ('guid-208','kkut_additive_man_208','cook','burgher',"
            " 'Kuttenberg','Kuttenberg region','male');"
            "INSERT INTO generated_name VALUES ('guid-208','James Cook');"
            "INSERT INTO npc_bio (soul_name, bio) VALUES ('kkut_additive_man_208',"
            " 'James Cook feeds the hungry labourers of Kuttenberg from a clay pot by the"
            " market square. His hands are forever greasy and he smells of onions.');"
            "INSERT INTO npc_card VALUES ('kkut_additive_man_208',"
            " 'Street cook with greasy hands and a short temper.', 8);"
            // The two lines the greetings pass writes: one for a stranger, one for somebody
            // already spoken to.
            "INSERT INTO npc_default_greeting VALUES"
            " ('kkut_additive_man_208',0,'What do you want? The pot''s not ready.'),"
            " ('kkut_additive_man_208',1,'You again. Pot''s still not ready.');"

            // A bio with no card, as every soul written before cards existed has.
            "INSERT INTO npc VALUES ('guid-209','kkut_additive_man_209','fuller','burgher',"
            " 'Kuttenberg','Kuttenberg region','male');"
            "INSERT INTO generated_name VALUES ('guid-209','Nicholas Fuller');"
            "INSERT INTO npc_bio (soul_name, bio) VALUES ('kkut_additive_man_209',"
            " 'Nicholas Fuller treads cloth in a trough of stale urine from dawn. He is"
            " deaf in one ear and leans in to hear.');"

            // A greeting for a stranger and none for a reunion, which is what a soul the pass
            // half reached looks like. The point of having one is that they must not fall
            // silent when the player comes back.
            "INSERT INTO npc_default_greeting VALUES"
            " ('kkut_additive_man_209',0,'Speak into this ear.');"

            // A soul the generator never renamed.
            "INSERT INTO npc VALUES ('guid-300','kkut_unnamed_300','beggar','peasant',"
            " 'Kuttenberg','Kuttenberg region','female');"

            // A mercenary whose "settlement" is the company he marches with, which is
            // what the field really holds for a good many souls. He is placed socially
            // in a real village, so that is the answer worth giving.
            "INSERT INTO npc VALUES ('guid-400','trosky_company_man_4','mercenary',"
            " 'soldier','bohutova Vlozka Eriks Company','Trosky region','male');"
            "INSERT INTO generated_name VALUES ('guid-400','Roger');"
            "INSERT INTO social_membership VALUES ('trosky_company_man_4','p2');"

            // And one with no social placing at all, who should simply not be given a
            // town rather than be given a wrong one.
            "INSERT INTO npc VALUES ('guid-401','trosky_company_man_5','mercenary',"
            " 'soldier','bohutova Vlozka Eriks Company','Trosky region','male');"
            "INSERT INTO generated_name VALUES ('guid-401','Walter');"

            // A bio the checker flagged. Four of the first hundred were.
            "INSERT INTO npc VALUES ('guid-500','kkut_flagged_500','cooper','burgher',"
            " 'Kuttenberg','Kuttenberg region','male');"
            "INSERT INTO generated_name VALUES ('guid-500','Ondra');"
            "INSERT INTO npc_bio (soul_name, bio) VALUES ('kkut_flagged_500',"
            " 'Ondra works beside his invented brother, who is in no dossier.');"
            "INSERT INTO npc_card VALUES ('kkut_flagged_500',"
            " 'Cooper who works beside a brother he does not have.', 9);"
            "INSERT INTO npc_default_greeting VALUES ('kkut_flagged_500',0,'My brother is out.');"
            "UPDATE npc_bio SET issues = 'claims a brother the data does not give them'"
            " WHERE soul_name = 'kkut_flagged_500';"

            // The circle pipeline's tables (Phase 2 of the character design): a person with
            // their own account of themselves, what they know at each tier of trust, the
            // written answers to "help", and the favour between two of them. Wenzel wants
            // his saw back from Julian, who is Bartel's acquaintance above.
            "CREATE TABLE npc_person (soul_name TEXT PRIMARY KEY, circle_id TEXT,"
            " long_bio TEXT, self_account TEXT, card TEXT, words INTEGER, issues TEXT);"
            "CREATE TABLE npc_knowledge (soul_name TEXT, topic_key TEXT, label TEXT,"
            " trust TEXT, text TEXT, PRIMARY KEY (soul_name, topic_key));"
            "CREATE TABLE npc_seed_answer (soul_name TEXT, intent TEXT, variant INTEGER,"
            " text TEXT, refers_to_quest_id TEXT, PRIMARY KEY (soul_name, intent, variant));"
            "CREATE TABLE npc_remark (soul_name TEXT, remark_key TEXT, row TEXT, text TEXT,"
            " place TEXT NOT NULL DEFAULT '', PRIMARY KEY (soul_name, remark_key));"
            // Where Wenzel's day takes him, as the routine build dumps it: the one table of
            // the game's own the app reads for the menu, for the remarks on the place.
            "CREATE TABLE npc_routine (soul_name TEXT, level TEXT, slot INTEGER,"
            " pos_x REAL, pos_y REAL, pos_z REAL, PRIMARY KEY (soul_name, level, slot));"
            "INSERT INTO npc_routine VALUES ('kkut_additive_man_48','kutnohorsko',0,"
            " 1000.0, 2000.0, 300.0), ('kkut_additive_man_48','kutnohorsko',1,"
            " 1030.0, 2000.0, 300.0), ('kkut_additive_man_48','kutnohorsko',2, NULL, NULL, NULL);"
            "CREATE TABLE npc_quest (quest_id TEXT PRIMARY KEY, kind TEXT, giver_soul TEXT,"
            " giver_name TEXT, target_soul TEXT, target_name TEXT, item_class TEXT,"
            " item_name TEXT, reward_groschen INTEGER, skill TEXT, difficulty TEXT);"
            "CREATE TABLE npc_quest_text (quest_id TEXT PRIMARY KEY, name TEXT, offer TEXT,"
            " thanks TEXT);"

            "INSERT INTO npc VALUES ('guid-48','kkut_additive_man_48','carpenter','burgher',"
            " 'Kuttenberg','Kuttenberg region','male');"
            "INSERT INTO generated_name VALUES ('guid-48','Wenzel');"
            "INSERT INTO npc_person VALUES ('kkut_additive_man_48','kkut_pillory',"
            " 'Wenzel is a carpenter who lent his saw and has not seen it since.',"
            " 'Wenzel. Carpenter, when I have my tools.',"
            " 'Carpenter without his saw, sour about it.', 7, 'purple word: sour (soft)');"
            "INSERT INTO npc_card VALUES ('kkut_additive_man_48',"
            " 'Carpenter without his saw, sour about it.', 7);"
            "INSERT INTO npc_knowledge VALUES"
            " ('kkut_additive_man_48','daily_work','Waiting for work','open',"
            "  'I wait by the pillory like the rest.'),"
            " ('kkut_additive_man_48','julian_saw','Julian and the saw','friendly',"
            "  'Julian has had my saw since Candlemas.'),"
            " ('kkut_additive_man_48','his_wife','his wife','confided',"
            "  'She died the winter before last.');"
            "INSERT INTO npc_seed_answer VALUES"
            " ('kkut_additive_man_48','help',1,'Ask Bartel. He is always wanting something.',"
            "  'quest-bartel'),"
            " ('kkut_additive_man_48','help',0,'No. I have my saw and my peace.',NULL),"
            " ('kkut_additive_man_48','about',0,'Wenzel. Carpenter, when I have my tools.',NULL);"
            "INSERT INTO npc_remark VALUES"
            " ('kkut_additive_man_48','cold_work','Cold work, this.',"
            "  'Cold? You should try it in January.', ''),"
            " ('kkut_additive_man_48','good_timber','That''s good timber.',"
            "  'Oak. Paid for, too.', ''),"
            " ('kkut_additive_man_48','damp_yard','This yard is very damp.',"
            "  'The gutter above. Nobody mends it.', 'here'),"
            " ('kkut_additive_man_48','praying_outside','Why are you praying out here?',"
            "  'The church is locked to me. God is not.', 'praying');"
            "INSERT INTO npc_quest VALUES ('quest-wenzel','retrieve','kkut_additive_man_48',"
            " 'Wenzel','kkut_additive_man_45','Julian','saw-class','hand saw',8,"
            " 'speech','easy');"
            "INSERT INTO npc_quest_text VALUES ('quest-wenzel','The Borrowed Saw',"
            " 'Julian has my saw. Eight groschen to whoever brings it back.',"
            " 'That is my saw. Here, eight, as I said.');"

            // The slate (Phase 4): Ondra's two favours from Vit make a chain, the first a
            // dispute with the giver's lines written; the second pays in thanks.
            "CREATE TABLE npc_slate (quest_id TEXT PRIMARY KEY, circle_id TEXT, kind TEXT,"
            " giver_soul TEXT, target_soul TEXT, item TEXT, item_class TEXT,"
            " reward_groschen INTEGER, existing INTEGER, chain_id TEXT, chain_order INTEGER,"
            " requires_quest_id TEXT, dispute INTEGER, twist TEXT, clue INTEGER,"
            " reward_kind TEXT, reward_text TEXT, after_quest_id TEXT, after_how TEXT);"
            "CREATE TABLE npc_quest_lines (quest_id TEXT, field TEXT, text TEXT,"
            " PRIMARY KEY (quest_id, field));"
            "INSERT INTO npc VALUES ('guid-51','kkut_additive_man_51','varlet','peasant',"
            " 'Kuttenberg','Kuttenberg region','male');"
            "INSERT INTO generated_name VALUES ('guid-51','Ondra');"
            "INSERT INTO npc VALUES ('guid-52','kkut_additive_man_52','varlet','peasant',"
            " 'Kuttenberg','Kuttenberg region','male');"
            "INSERT INTO generated_name VALUES ('guid-52','Vit');"
            // Dealt saw first so the row order is not the chain's order.
            "INSERT INTO npc_quest VALUES ('quest-ondra-saw','retrieve','kkut_additive_man_51',"
            " 'Ondra','kkut_additive_man_52','Vit','saw-class','saw',0,'persuade','medium');"
            "INSERT INTO npc_quest VALUES ('quest-ondra-axe','retrieve','kkut_additive_man_51',"
            " 'Ondra','kkut_additive_man_52','Vit','axe-class','work axe',10,'persuade','medium');"
            "INSERT INTO npc_quest_text VALUES ('quest-ondra-axe','Axe to Grind',"
            " 'Vit has my axe and a grudge. Ten groschen to get it back.', 'Good. Here, ten.');"
            "INSERT INTO npc_quest_text VALUES ('quest-ondra-saw','Cutting Corners',"
            " 'I have my axe. Now the saw.', 'I owe you.');"
            "INSERT INTO npc_slate VALUES ('quest-ondra-axe','kkut_pillory','retrieve',"
            " 'kkut_additive_man_51','kkut_additive_man_52','work axe','axe-class',10,0,"
            " 'ondra_tools',1,NULL,1,NULL,0,'money','10 groschen',NULL,NULL);"
            "INSERT INTO npc_slate VALUES ('quest-ondra-saw','kkut_pillory','retrieve',"
            " 'kkut_additive_man_51','kkut_additive_man_52','saw','saw-class',0,0,"
            " 'ondra_tools',2,NULL,0,NULL,0,'gratitude','thanks and a favour owed',NULL,NULL);"

            // A story in two scenes (quest-design/scenes.md): Hugo wants the truth got out of
            // William (an ask), and only if it is carried back true does William's wife Marta
            // have a letter to send -- a scene of another giver's, linked by after_quest_id
            // and after_how, with no chain.
            "INSERT INTO npc VALUES ('guid-53','ksuc_poacher_3','poacher','peasant',"
            " 'Suchdol','Kuttenberg region','male');"
            "INSERT INTO generated_name VALUES ('guid-53','Hugo');"
            "INSERT INTO npc VALUES ('guid-54','ksuc_man_63','villager','peasant',"
            " 'Suchdol','Kuttenberg region','male');"
            "INSERT INTO generated_name VALUES ('guid-54','William');"
            "INSERT INTO npc VALUES ('guid-55','ksuc_woman_12','villager','peasant',"
            " 'Suchdol','Kuttenberg region','female');"
            "INSERT INTO generated_name VALUES ('guid-55','Marta');"
            "INSERT INTO npc_quest VALUES ('scene-ask','ask','ksuc_poacher_3',"
            " 'Hugo','ksuc_man_63','William','','the truth',12,'speech','medium');"
            "INSERT INTO npc_quest VALUES ('scene-letter','deliver','ksuc_woman_12',"
            " 'Marta','ksuc_poacher_3','Hugo','letter-class','letter',0,'speech','easy');"
            "INSERT INTO npc_quest_text VALUES ('scene-ask','Birds Do Not Just Die',"
            " 'William says the birds just died. Get the truth out of him.', 'Better to know. Here.');"
            "INSERT INTO npc_quest_text VALUES ('scene-letter','A Word to Hugo',"
            " 'Now Hugo knows, take him this.', 'Thank you.');"
            "INSERT INTO npc_slate VALUES ('scene-ask','ksuc_birds','ask',"
            " 'ksuc_poacher_3','ksuc_man_63','the truth',NULL,12,0,NULL,1,NULL,0,NULL,0,"
            " 'money','12 groschen',NULL,NULL);"
            "INSERT INTO npc_slate VALUES ('scene-letter','ksuc_birds','deliver',"
            " 'ksuc_woman_12','ksuc_poacher_3','letter','letter-class',0,0,NULL,2,NULL,0,NULL,0,"
            " 'gratitude','thanks',"
            " 'scene-ask','truth');"
            // A story's scenes carry story_id (and no chain_id) in a slate new enough; the
            // quests are ordered by scene under it. Oldrich has two scenes off the same gate,
            // with ids that sort the other way round from their numbers.
            "ALTER TABLE npc_slate ADD COLUMN story_id TEXT;"
            "UPDATE npc_slate SET story_id = 'ksuc_birds' WHERE quest_id IN ('scene-ask','scene-letter');"
            "INSERT INTO npc VALUES ('guid-56','ksuc_man_64','villager','peasant',"
            " 'Suchdol','Kuttenberg region','male');"
            "INSERT INTO generated_name VALUES ('guid-56','Oldrich');"
            "INSERT INTO npc VALUES ('guid-57','ksuc_woman_13','villager','peasant',"
            " 'Suchdol','Kuttenberg region','female');"
            "INSERT INTO generated_name VALUES ('guid-57','Dorota');"
            "INSERT INTO npc_quest VALUES ('scene-a-third','deliver','ksuc_man_64',"
            " 'Oldrich','ksuc_woman_13','Dorota','note-class','note',0,'speech','easy');"
            "INSERT INTO npc_quest VALUES ('scene-z-second','deliver','ksuc_man_64',"
            " 'Oldrich','ksuc_woman_13','Dorota','purse-class','purse',0,'speech','easy');"
            "INSERT INTO npc_quest_text VALUES ('scene-a-third','A Note for Dorota',"
            " 'And this note to Dorota, after.', 'Good.');"
            "INSERT INTO npc_quest_text VALUES ('scene-z-second','A Purse for Dorota',"
            " 'Take Dorota her purse back first.', 'Thank you.');"
            "INSERT INTO npc_slate (quest_id, circle_id, kind, giver_soul, target_soul, item, item_class,"
            " reward_groschen, existing, chain_id, chain_order, requires_quest_id, dispute, twist, clue,"
            " reward_kind, reward_text, after_quest_id, after_how, story_id) VALUES"
            " ('scene-a-third','ksuc_birds','deliver','ksuc_man_64','ksuc_woman_13','note','note-class',"
            "  0,0,NULL,3,NULL,0,NULL,0,'gratitude','thanks','scene-ask','truth','ksuc_birds'),"
            " ('scene-z-second','ksuc_birds','deliver','ksuc_man_64','ksuc_woman_13','purse','purse-class',"
            "  0,0,NULL,2,NULL,0,NULL,0,'gratitude','thanks','scene-ask','truth','ksuc_birds');"
            "INSERT INTO npc_quest_lines VALUES"
            " ('scene-ask','tell','William told me. The birds were poisoned.'),"
            " ('scene-ask','lie','The birds died, like he said.'),"
            " ('scene-ask','quiet','What he told me stays with me.');"
            "INSERT INTO npc_quest_lines VALUES"
            " ('quest-ondra-axe','apologise','Vit wants you to admit he paid your slate.'),"
            " ('quest-ondra-axe','apologise_yes','Fine. He paid. Tell him.'),"
            " ('quest-ondra-axe','apologise_no','I admit nothing.'),"
            " ('quest-ondra-axe','broker','What if I get him to admit it?'),"
            " ('quest-ondra-axe','give','Here. Your axe.'),"
            " ('quest-ondra-axe','lie_have','Vit never had it.'),"
            " ('quest-ondra-axe','lie_have_reply','Never had it? Then it is gone.'),"
            " ('quest-ondra-axe','lie_have_caught','Then what is that on your back?'),"
            " ('quest-ondra-axe','more_2','You want me to lie to him?'),"
            " ('quest-ondra-axe','more_2_reply','Not lie. Just do not say.'),"
            " ('quest-ondra-axe','greet_offer','You are the one who helps folk, they say.'),"
            " ('quest-ondra-axe','thanks_onward','My axe. And now -- wait.'),"
            " ('quest-ondra-axe','unyielding','1'),"
            " ('quest-ondra-axe','helper_name','Elsbeth'),"
            " ('quest-ondra-axe','greet_rebuffed','Would not hear of it? Then his chest. Elsbeth keeps the house.');"
            "INSERT INTO npc_quest_lines VALUES"
            " ('scene-z-second','item_kind','thing'),"
            " ('scene-z-second','told_truth','She has it, and I told her where it came from.'),"
            " ('scene-z-second','told_reply_truth','You told her. Well.'),"
            " ('scene-z-second','told_reply_pinned','You said my name.');"
            // The note is written at Oldrich's dictation (quest.h, Quest::Dictation).
            "INSERT INTO npc_quest_lines VALUES"
            " ('scene-a-third','dictate_begin','Sit. Write what I say.'),"
            " ('scene-a-third','dictate_done','Fold it. Take it to her.'),"
            " ('scene-a-third','dictate','Let us get your note written.'),"
            " ('scene-a-third','greet_unwritten','Back to the writing?'),"
            " ('scene-a-third','dictate_1','Dorota, the purse was yours.'),"
            " ('scene-a-third','write_1','As you say it.'),"
            " ('scene-a-third','write_1_reply','Good.'),"
            " ('scene-a-third','soften_1','I will put it more gently.'),"
            " ('scene-a-third','soften_1_reply','If you must.'),"
            " ('scene-a-third','dictate_2','And I never touched it.'),"
            " ('scene-a-third','write_2','As you say it.'),"
            " ('scene-a-third','write_2_reply','Good.'),"
            " ('scene-a-third','reconsider_2','Do you want that in it?'),"
            " ('scene-a-third','reconsider_2_reply','No. Leave it.'),"
            " ('scene-a-third','dictate_3','Forgive me.'),"
            " ('scene-a-third','write_3','As you say it.'),"
            " ('scene-a-third','write_3_reply','Good.'),"
            " ('scene-a-third','embellish_3','I will give it weight.'),"
            " ('scene-a-third','embellish_3_reply','Yes.'),"
            " ('scene-a-third','told_cursed','She tore it up.'),"
            " ('scene-a-third','told_reply_cursed','Tore it. Well.');"

            // A person the checker failed outright, whose card must not be served, and
            // one it passed clean who has no bio row at all, which is most of them.
            "INSERT INTO npc VALUES ('guid-49','kkut_additive_man_49','tanner','burgher',"
            " 'Kuttenberg','Kuttenberg region','male');"
            "INSERT INTO generated_name VALUES ('guid-49','Prokop');"
            "INSERT INTO npc_person VALUES ('kkut_additive_man_49','kkut_pillory','','',"
            " 'Tanner with a son in the guard.', 6,"
            " 'purple word: reek (soft); claims a son the data does not give them');"
            "INSERT INTO npc_card VALUES ('kkut_additive_man_49',"
            " 'Tanner with a son in the guard.', 6);"
            "INSERT INTO npc VALUES ('guid-50','kkut_additive_man_50','beggar','peasant',"
            " 'Kuttenberg','Kuttenberg region','female');"
            "INSERT INTO generated_name VALUES ('guid-50','Drahomira');"
            "INSERT INTO npc_person VALUES ('kkut_additive_man_50','kkut_pillory','',"
            " 'Drahomira. I beg here.', 'Beggar at the pillory steps.', 5, NULL);"
            "INSERT INTO npc_card VALUES ('kkut_additive_man_50',"
            " 'Beggar at the pillory steps.', 5);");
        sqlite3_close(db);
    }

    ~Db() {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }

    Db(const Db&) = delete;
    Db& operator=(const Db&) = delete;

    const std::filesystem::path& path() const { return path_; }

    // For a test that needs the file to be something other than what the fixture built,
    // such as the older shape a player who has not updated their data still has.
    void exec(const char* sql) {
        sqlite3* db = nullptr;
        REQUIRE(sqlite3_open(path_.string().c_str(), &db) == SQLITE_OK);
        run(db, sql);
        sqlite3_close(db);
    }

private:
    static void run(sqlite3* db, const char* sql) {
        char* problem = nullptr;
        const int ok = sqlite3_exec(db, sql, nullptr, nullptr, &problem);
        if (ok != SQLITE_OK) {
            const std::string message = problem ? problem : "unknown";
            sqlite3_free(problem);
            FAIL("setting up the test database: ", message);
        }
    }

    std::filesystem::path path_;
};

}  // namespace

TEST_CASE("a soul id becomes the person the mod named") {
    Db db;
    PersonaDb personas(db.path());
    REQUIRE(personas.ok());

    // The id the game logs is not a name, and this is the hop that turns it into one.
    const std::optional<Soul> bartel = personas.find("kkut_additive_man_47");
    REQUIRE(bartel.has_value());
    CHECK(bartel->name == "Bartel");
    CHECK(bartel->occupation == "varlet");
    CHECK(bartel->settlement == "Kuttenberg");
    CHECK(bartel->gender == "male");
}

TEST_CASE("a varlet is called by his trade when the trade table knows it") {
    Db db;
    // scripts/trade_words.py reads the work off the routine tables -- Bartel's is mining --
    // and writes it here. The label stays on npc for the code that classifies by it.
    db.exec("CREATE TABLE npc_trade (soul_name TEXT PRIMARY KEY, trade TEXT, why TEXT);"
            "INSERT INTO npc_trade VALUES ('kkut_additive_man_47','miner','routine: mining');");
    PersonaDb personas(db.path());

    const std::optional<Soul> bartel = personas.find("kkut_additive_man_47");
    REQUIRE(bartel.has_value());
    CHECK(bartel->occupation == "varlet");
    CHECK(bartel->trade == "miner");
    CHECK(personas.persona("kkut_additive_man_47").sheet.find("You are Bartel, a miner in Kuttenberg.") == 0);

    // A soul the sweep gave no row keeps the fallback, and a label that is a trade already
    // is its own word.
    CHECK(personas.find("kkut_additive_man_45")->trade == "hired hand");
    CHECK(personas.find("kkut_additive_man_46")->trade == "miner");
}

TEST_CASE("the sheet says who they are, where they are, and what is wrong") {
    Db db;
    const Persona bartel = PersonaDb(db.path()).persona("kkut_additive_man_47");

    CHECK(bartel.name == "Bartel");
    // The game files him as a "varlet", which is its class label and not a word anyone
    // said of a man. Without the trade table the sheet says the plainest true thing.
    CHECK(bartel.sheet.find("You are Bartel, a hired hand in Kuttenberg.") == 0);
    // The trouble matters more than the facts: it is what gives them something to want
    // from the conversation rather than an identity to recite.
    CHECK(bartel.sheet.find("roof is failing") != std::string::npos);
    // The premise sets the situation up and the problem says what it looks like day to
    // day, so both earn their place when they differ.
    CHECK(bartel.sheet.find("wakes in the wet") != std::string::npos);
    CHECK(bartel.sheet.find("Bartel wants somewhere dry to sleep") != std::string::npos);
}

TEST_CASE("close kin come before acquaintances, and the nameless are left out") {
    Db db;
    const Persona bartel = PersonaDb(db.path()).persona("kkut_additive_man_47");

    const std::size_t brother = bartel.sheet.find("Your brother is Frantishek");
    const std::size_t acquaintance = bartel.sheet.find("You also know Julian");
    REQUIRE(brother != std::string::npos);
    REQUIRE(acquaintance != std::string::npos);
    CHECK(brother < acquaintance);

    // "your acquaintance kkut_nameless_99" would be worse than saying nothing.
    CHECK(bartel.sheet.find("kkut_nameless_99") == std::string::npos);
    CHECK(bartel.sheet.find("nameless") == std::string::npos);
}

TEST_CASE("a written background is used instead of the assembled facts") {
    Db db;
    PersonaDb personas(db.path());
    // Nicholas has a bio and no card, which is what every soul written before cards
    // existed looks like.
    const std::optional<Soul> smith = personas.find("kkut_additive_man_209");
    REQUIRE(smith.has_value());
    REQUIRE_FALSE(smith->bio.empty());
    REQUIRE(smith->card.empty());

    const Persona persona = personas.persona("kkut_additive_man_209");
    CHECK(persona.name == "Nicholas Fuller");
    CHECK(persona.sheet.find("deaf in one ear") != std::string::npos);
    // The name and trade still lead, because the bio is written as though the reader
    // already knows them.
    CHECK(persona.sheet.find("You are Nicholas Fuller, a fuller in Kuttenberg.") == 0);
}

TEST_CASE("a soul the generator never renamed is still somebody to talk to") {
    Db db;
    PersonaDb personas(db.path());
    const std::optional<Soul> found = personas.find("kkut_unnamed_300");
    REQUIRE(found.has_value());
    CHECK(found->name.empty());

    // Their trade and town are known even though their name is not, which is more than
    // a stranger gets.
    const Persona persona = personas.persona("kkut_unnamed_300");
    CHECK(persona.name == "Stranger");
    CHECK(persona.sheet.find("a beggar in Kuttenberg") != std::string::npos);
}

TEST_CASE("a soul nobody has heard of falls back rather than failing") {
    Db db;
    PersonaDb personas(db.path());
    CHECK_FALSE(personas.find("kkut_not_in_the_database").has_value());

    // A save made before the generator ran should still open a conversation.
    const Persona persona = personas.persona("kkut_not_in_the_database");
    CHECK(persona.name == "Stranger");
    CHECK_FALSE(persona.sheet.empty());
}

TEST_CASE("a conversation with nobody named falls back too") {
    Db db;
    PersonaDb personas(db.path());
    CHECK_FALSE(personas.find("").has_value());
    CHECK(personas.persona("").name == "Stranger");
}

TEST_CASE("a missing database is reported, not fatal") {
    PersonaDb personas("/moonshine-no-such-database.sqlite");
    CHECK_FALSE(personas.ok());
    CHECK_FALSE(personas.trouble().empty());
    // Still answers, so a broken install talks to strangers rather than crashing.
    CHECK(personas.persona("kkut_additive_man_47").name == "Stranger");
    CHECK(personas.named_souls() == 0);
}

TEST_CASE("the database says how many souls it can name") {
    Db db;
    // Worth telling apart from a database that opened but holds nothing.
    CHECK(PersonaDb(db.path()).named_souls() == 18);
}

TEST_CASE("the database fingerprints the deal it holds, the way the mod's emitter does") {
    // FNV-1a 64 over the sorted quest ids joined by newlines, sixteen hex digits: the seven
    // ids the fixture seeds, in byte order. The same value is pinned in the emitter's tests
    // (mod/src/emit.cpp, quest_set_fingerprint) over the same ids, so a change to either side
    // alone fails there and here. A database with no quest table has no fingerprint.
    Db db;
    CHECK(PersonaDb(db.path()).quest_set() == "572bdf1db2c4d893");
    CHECK(PersonaDb("/moonshine-no-such-database.sqlite").quest_set().empty());
}

TEST_CASE("an encounter group is not a town") {
    Db db;
    PersonaDb personas(db.path());

    // "a mercenary in bohutova Vlozka Eriks Company" reads as a place and is not one.
    const Persona roger = personas.persona("trosky_company_man_4");
    CHECK(roger.sheet.find("Eriks Company") == std::string::npos);
    CHECK(roger.sheet.find("You are Roger, a mercenary in Suchdol.") == 0);

    // With nothing better known, no town at all beats the wrong one.
    const Persona walter = personas.persona("trosky_company_man_5");
    CHECK(walter.sheet.find("Eriks Company") == std::string::npos);
    CHECK(walter.sheet.find("You are Walter, a mercenary.") == 0);
}

TEST_CASE("a bio the generator flagged is not served") {
    Db db;
    PersonaDb personas(db.path());
    // The checks that set `issues` are the ones worth acting on: a character who
    // contradicts the rest of the world is worse than one described from facts alone.
    const std::optional<Soul> found = personas.find("kkut_flagged_500");
    REQUIRE(found.has_value());
    CHECK(found->bio.empty());
    CHECK(personas.persona("kkut_flagged_500").sheet.find("invented brother") ==
          std::string::npos);
}

TEST_CASE("the card is used in preference to the bio") {
    Db db;
    PersonaDb personas(db.path());
    const std::optional<Soul> cook = personas.find("kkut_additive_man_208");
    REQUIRE(cook.has_value());
    REQUIRE_FALSE(cook->bio.empty());
    REQUIRE_FALSE(cook->card.empty());

    // Both describe the same man, but the bio is four hundred tokens and the card forty,
    // on text prefilled again for every soul the player meets.
    const Persona persona = personas.persona("kkut_additive_man_208");
    CHECK(persona.sheet.find("greasy hands and a short temper") != std::string::npos);
    CHECK(persona.sheet.find("smells of onions") == std::string::npos);
    CHECK(persona.sheet.size() < cook->bio.size());
}

TEST_CASE("the greeting is the stranger's line, or the one for somebody already spoken to") {
    Db db;
    PersonaDb personas(db.path());

    CHECK(personas.persona("kkut_additive_man_208", {}).greeting ==
          "What do you want? The pot's not ready.");
    CHECK(personas.persona("kkut_additive_man_208", Approach{.met = true}).greeting ==
          "You again. Pot's still not ready.");
    // The state the player arrives in goes into the prose, not into the choice of line: a
    // version of this kept eighteen state-keyed greetings per soul, and the multiplication
    // cost more to write than the precision was worth.
    CHECK(personas.persona("kkut_additive_man_208", Approach{.blood = 0.5}).greeting ==
          "What do you want? The pot's not ready.");
    CHECK(personas.persona("kkut_additive_man_208", Approach{.blood = 0.5, .met = true})
              .greeting == "You again. Pot's still not ready.");
    // Neither is written for the very visit, so the session may ask the model for a fresher
    // one while the player is only looking.
    CHECK_FALSE(personas.persona("kkut_additive_man_208", {}).greeting_exact);
    CHECK_FALSE(personas.persona("kkut_additive_man_208", Approach{.met = true}).greeting_exact);
}

TEST_CASE("a soul with only the stranger's line says it to somebody met before") {
    Db db;
    // Better a stranger's greeting than seven seconds of silence while one is generated.
    CHECK(PersonaDb(db.path()).persona("kkut_additive_man_209", Approach{.met = true}).greeting ==
          "Speak into this ear.");
}

TEST_CASE("a soul with nothing written has no greeting to say") {
    Db db;
    // Which is what sends the session down the generating path.
    CHECK(PersonaDb(db.path()).persona("kkut_additive_man_47").greeting.empty());
}

TEST_CASE("the greetings are read for a soul with no bio or card at all") {
    Db db;
    // The greetings pass writes its two lines for every named soul from the dossier alone:
    // Bartel has no bio and no card here, and gets them anyway.
    db.exec("INSERT INTO npc_default_greeting VALUES"
            " ('kkut_additive_man_47', 0, 'Aye? What is it?'),"
            " ('kkut_additive_man_47', 1, 'You again. Go on, then.');");
    PersonaDb personas(db.path());
    CHECK(personas.persona("kkut_additive_man_47", Approach{.blood = 0.5}).greeting ==
          "Aye? What is it?");
    CHECK(personas.persona("kkut_additive_man_47", Approach{.met = true}).greeting ==
          "You again. Go on, then.");
    // And the soul is not told about them when written greetings are switched off.
    CHECK(personas.persona("kkut_additive_man_47", {}, {}, false).greeting.empty());
    // The table is optional, like every other written thing.
    db.exec("DROP TABLE npc_default_greeting;");
    CHECK(PersonaDb(db.path()).persona("kkut_additive_man_47").greeting.empty());
}

TEST_CASE("written greetings can be switched off, which puts the opening line on the model") {
    Db db;
    PersonaDb personas(db.path());

    // The same soul, the same state, with and without. A line is written for this one, so the
    // switch is the only thing that can account for the difference.
    const Approach seen{.charisma = 0.7};
    CHECK(personas.persona("kkut_additive_man_208", seen, {}, true).greeting ==
          "What do you want? The pot's not ready.");
    CHECK(personas.persona("kkut_additive_man_208", seen, {}, false).greeting.empty());

    // Everything else is untouched. The switch decides what is said, not what is known: a page
    // showing what was written for a soul still needs the words.
    const Persona quiet = personas.persona("kkut_additive_man_208", seen, {}, false);
    CHECK(quiet.sheet == personas.persona("kkut_additive_man_208", seen, {}, true).sheet);
    CHECK(personas.find("kkut_additive_man_208")->greeting_first ==
          "What do you want? The pot's not ready.");
}

TEST_CASE("a flagged generation loses its card too, not just its bio, and keeps its greeting") {
    Db db;
    PersonaDb personas(db.path());
    const std::optional<Soul> found = personas.find("kkut_flagged_500");
    REQUIRE(found.has_value());
    // One generation produced both, so one verdict covers both: a run that invented a
    // brother for the bio is no more trustworthy in the card.
    CHECK(found->bio.empty());
    CHECK(found->card.empty());
    // The greeting came from a pass of its own over the dossier, and the bio's verdict says
    // nothing about it.
    CHECK(personas.persona("kkut_flagged_500").greeting == "My brother is out.");
}

TEST_CASE("an animation name becomes a clause the sheet can say") {
    Db db;
    PersonaDb personas(db.path());

    const Doing sawing = personas.doing("LumberJackSawVAR");
    CHECK(sawing.clause == "sawing wood");
    CHECK(sawing.activity == "working");
    CHECK(sawing.caught());

    // Past tense, and last, so it is still true on the fourth turn of the conversation
    // rather than only on the opening line.
    const std::string sheet = personas.persona("kkut_additive_man_47", {},
                                               "LumberJackSawVAR").sheet;
    CHECK(sheet.find("You are busy sawing wood. Someone walks up to you.") !=
          std::string::npos);
    CHECK(sheet.rfind("You are busy sawing wood") ==
          sheet.size() - std::string("You are busy sawing wood. Someone walks up "
                                     "to you.").size());
}

TEST_CASE("an animation nobody can be caught at adds nothing to the sheet") {
    Db db;
    PersonaDb personas(db.path());

    // Every animation a person can play is in the table, including dying in a quest. The
    // activity says so and the clause is empty, and an empty clause must not reach the
    // sheet as "You were  when they walked up."
    CHECK_FALSE(personas.doing("Quest_CombatAttackComboDeath").caught());
    for (const std::string_view anim :
         {"Quest_CombatAttackComboDeath", "none", "", "NoSuchFragmentEver"}) {
        const std::string sheet = personas.persona("kkut_additive_man_47", {}, anim).sheet;
        CHECK(sheet.find("You are busy") == std::string::npos);
        // The approach itself is still reported -- somebody did walk up.
        CHECK(sheet.find("Someone walks up to you.") != std::string::npos);
    }
}

TEST_CASE("a clause the generator flagged is withheld, like a flagged bio") {
    Db db;
    // Half a verdict is not a pass: this one kept the animation's own spelling, and
    // "You were WashingUpInATub when they walked up" is worse than saying nothing.
    CHECK_FALSE(PersonaDb(db.path()).doing("SpaWashToBath").caught());
}

TEST_CASE("a stranger is still caught in the middle of something") {
    Db db;
    // Not knowing who somebody is does not stop the player having walked up on them
    // sawing, and it is the one concrete thing a stranger's sheet can say.
    const std::string sheet =
        PersonaDb(db.path()).persona("kkut_never_heard_of", {}, "LumberJackSawVAR").sheet;
    CHECK(sheet.find("You are busy sawing wood.") != std::string::npos);
}

TEST_CASE("a database built before the activity table still answers") {
    Db db;
    // A player who has not updated their data. The query fails to prepare, which has to
    // read as "no clause" and not take the conversation down with it.
    db.exec("DROP TABLE anim_activity;");

    PersonaDb personas(db.path());
    CHECK_FALSE(personas.doing("LumberJackSawVAR").caught());
    CHECK(personas.persona("kkut_additive_man_47", {}, "LumberJackSawVAR")
              .sheet.find("You are busy") == std::string::npos);
}

TEST_CASE("a database with no cards or greetings in it still answers") {
    // What a player who has not updated their data has: npc_bio and no more. Every query
    // for the newer tables fails to prepare, which has to read as "nothing written" rather
    // than take the lookup down -- the Windows laptop found this out by running a build against exactly
    // such a file and greeting people perfectly well from the model instead.
    Db db;
    db.exec("DROP TABLE npc_card; DROP TABLE npc_default_greeting;");

    PersonaDb personas(db.path());
    const std::optional<Soul> found = personas.find("kkut_additive_man_208");
    REQUIRE(found.has_value());
    CHECK(found->card.empty());
    CHECK(found->greeting_first.empty());

    const Persona persona = personas.persona("kkut_additive_man_208");
    CHECK(persona.name == "James Cook");
    CHECK(persona.greeting.empty());
    // And the bio is still served, so the sheet is no worse than it was before cards.
    CHECK(persona.sheet.find("smells of onions") != std::string::npos);
}

TEST_CASE("the sheet tells the character who they are, not who somebody else is") {
    // Second person throughout, and settled rather than incidental. Told "Bartel is a
    // varlet" and separately that it is Bartel, a model has a step to make, and the step
    // it sometimes makes instead is to narrate: "Bartel grumbles about his shoulders"
    // rather than grumbling about them.
    Db db;
    PersonaDb personas(db.path());

    for (const std::string_view soul : {"kkut_additive_man_47", "kkut_additive_man_208",
                                        "kkut_additive_man_209", "kkut_nameless_99"}) {
        const Persona persona = personas.persona(soul);
        CAPTURE(soul);
        CHECK(persona.sheet.rfind("You are", 0) == 0);
        // The name is given once, as an address, and never used as a subject afterwards.
        CHECK(persona.sheet.find(persona.name + " is a") == std::string::npos);
        CHECK(persona.sheet.find("His ") == std::string::npos);
        CHECK(persona.sheet.find("Her ") == std::string::npos);
    }

    // Including the soul nobody has heard of, who is still being spoken to and not about.
    CHECK(stranger().sheet.rfind("You are", 0) == 0);
}

TEST_CASE("the sheet carries nothing that is true of every soul") {
    Db db;
    const Persona bartel = PersonaDb(db.path()).persona("kkut_additive_man_47");
    // How to speak is in the shared prefix, so it is prefilled once for the whole game
    // rather than again for every NPC the player meets.
    CHECK(bartel.sheet.find("two or three sentences") == std::string::npos);
    CHECK(bartel.sheet.find("break character") == std::string::npos);
}

TEST_CASE("the account of how the sheet was built rebuilds the sheet exactly") {
    // The invariant the labelled parts are worth having at all: joined back together they are the
    // sheet, so an answer to "why is the model being told that" cannot quietly stop matching what
    // the model is actually told. Appending to the sheet without recording the block fails here.
    Db db;
    PersonaDb personas(db.path());

    for (const std::string_view soul : {"kkut_additive_man_47", "kkut_additive_man_208",
                                        "kkut_additive_man_209", "kkut_nameless_99"}) {
        CAPTURE(soul);
        const Persona persona = personas.persona(soul, {.charisma = 0.95}, "LumberJackSawVAR");
        std::string rebuilt;
        for (const Persona::Part& part : persona.parts) {
            if (!rebuilt.empty()) rebuilt += "\n\n";
            rebuilt += part.text;
            // An empty block would show up as a labelled nothing in the inspector and as a
            // doubled blank line in the prompt.
            CHECK_FALSE(part.text.empty());
        }
        CHECK(rebuilt == persona.sheet);
    }
    CHECK(stranger().parts.size() == 1);
}

TEST_CASE("the parts are labelled with where they came from, and the state comes last") {
    Db db;
    PersonaDb personas(db.path());

    auto labels = [&](std::string_view soul) {
        std::vector<std::string> found;
        for (const Persona::Part& part : personas.persona(soul, {}, "LumberJackSawVAR").parts) {
            found.push_back(part.label);
        }
        return found;
    };

    // Bartel has kin and a hook and nothing written, so both are spelled out.
    CHECK(labels("kkut_additive_man_47") ==
          std::vector<std::string>{"who", "kin", "hook", "approach"});
    // James Cook has a card, which stands in for the bio and says whatever the hook would have.
    CHECK(labels("kkut_additive_man_208") == std::vector<std::string>{"who", "card", "approach"});
    // Nicholas Fuller has a bio and no card, which is the path that stops there: kin and hook are
    // skipped because four hundred tokens have already been spent describing him.
    CHECK(labels("kkut_additive_man_209") == std::vector<std::string>{"who", "bio", "approach"});
    // And the soul nobody wrote anything about is two lines: who they are, and who walked up.
    CHECK(labels("kkut_nameless_99") == std::vector<std::string>{"who", "approach"});

    // Last in every case. Everything before it is the same text for this soul every time and so
    // is worth prefilling; this changes with every conversation, and put anywhere else it would
    // invalidate the cached prefix from that point on.
    for (const std::string_view soul : {"kkut_additive_man_47", "kkut_additive_man_208",
                                        "kkut_additive_man_209", "kkut_nameless_99"}) {
        CAPTURE(soul);
        CHECK(labels(soul).back() == "approach");
    }
}

TEST_CASE("a person the circle pipeline wrote is loaded whole: account, knowledge, help") {
    Db db;
    PersonaDb personas(db.path());
    const std::optional<Soul> wenzel = personas.find("kkut_additive_man_48");
    REQUIRE(wenzel.has_value());
    CHECK(wenzel->self_account == "Wenzel. Carpenter, when I have my tools.");
    // In the generator's order, not alphabetical: the work first, the dead last.
    REQUIRE(wenzel->knowledge.size() == 3);
    CHECK(wenzel->knowledge[0].key == "daily_work");
    CHECK(wenzel->knowledge[1].trust == "friendly");
    CHECK(wenzel->knowledge[2].label == "his wife");
    // Help replies by variant, the lead carrying the quest it leads to; the 'about' row is
    // not among them, being the self-account under another name.
    REQUIRE(wenzel->help_replies.size() == 2);
    CHECK(wenzel->help_replies[0].text == "No. I have my saw and my peace.");
    CHECK(wenzel->help_replies[0].refers_to_quest_id.empty());
    CHECK(wenzel->help_replies[1].refers_to_quest_id == "quest-bartel");
    // The remarks in the order written, each Henry's row with the answer, the one on the
    // place marked as such, and the one on what he is doing carrying the activity.
    REQUIRE(wenzel->remarks.size() == 4);
    CHECK(wenzel->remarks[3].doing == "praying");
    CHECK(wenzel->remarks[2].doing.empty());
    CHECK(wenzel->remarks[0].key == "cold_work");
    CHECK(wenzel->remarks[0].row == "Cold work, this.");
    CHECK(wenzel->remarks[1].text == "Oak. Paid for, too.");
    CHECK_FALSE(wenzel->remarks[1].here);
    CHECK(wenzel->remarks[2].here);
    // His routine's two spots, the row without a position left out.
    REQUIRE(wenzel->spots.size() == 2);
    CHECK(wenzel->spots[0].first == doctest::Approx(1000.0));
    // With no position from the mod, every remark on the place is on the menu; the one on
    // praying only when he is seen praying (notes.md 9 Oct 2026: Benesh on the square).
    const Persona whole = personas.persona("kkut_additive_man_48");
    REQUIRE(whole.scripted.remarks.size() == 3);
    CHECK(whole.scripted.remarks[1].row == "That's good timber.");
    Approach at_prayer;
    at_prayer.activity = "praying";
    const Persona praying = personas.persona("kkut_additive_man_48", at_prayer);
    REQUIRE(praying.scripted.remarks.size() == 4);
    CHECK(praying.scripted.remarks[3].row == "Why are you praying out here?");
    Approach at_supper;
    at_supper.activity = "eating";
    CHECK(personas.persona("kkut_additive_man_48", at_supper).scripted.remarks.size() == 3);
    // Met beside the bench, the same; met on the road, a long way from both spots, the
    // remark on the yard is kept back and the other two stay.
    Approach at_the_bench;
    at_the_bench.x = 1020.0;
    at_the_bench.y = 2140.0;
    CHECK_FALSE(away_from_places(*wenzel, at_the_bench));
    CHECK(personas.persona("kkut_additive_man_48", at_the_bench).scripted.remarks.size() == 3);
    Approach on_the_road;
    on_the_road.x = 1500.0;
    on_the_road.y = 2000.0;
    CHECK(away_from_places(*wenzel, on_the_road));
    const Persona away = personas.persona("kkut_additive_man_48", on_the_road);
    REQUIRE(away.scripted.remarks.size() == 2);
    CHECK(away.scripted.remarks[0].key == "cold_work");
    CHECK(away.scripted.remarks[1].key == "good_timber");
    // A soul with no routine dumped is never away, wherever they are met.
    Soul nowhere = *wenzel;
    nowhere.spots.clear();
    CHECK_FALSE(away_from_places(nowhere, on_the_road));
    // And the favour, from the giver's end.
    CHECK(wenzel->quest.id == "quest-wenzel");
    CHECK(wenzel->quest.role == QuestRole::Giver);
    CHECK(wenzel->quest.item_name == "hand saw");
    CHECK(wenzel->quest.offer == "Julian has my saw. Eight groschen to whoever brings it back.");
    const std::optional<Soul> julian = personas.find("kkut_additive_man_45");
    REQUIRE(julian.has_value());
    CHECK(julian->quest.id == "quest-wenzel");
    CHECK(julian->quest.role == QuestRole::Target);
}

TEST_CASE("the pipeline's verdict on a person decides their card, and a soft note is not a verdict") {
    Db db;
    PersonaDb personas(db.path());
    // A purple word is a note to the writer; the card is served.
    CHECK(personas.persona("kkut_additive_man_48").sheet.find("sour about it") !=
          std::string::npos);
    // A claimed son is the same failing the bio checker withholds a card for, soft note or no.
    const std::optional<Soul> prokop = personas.find("kkut_additive_man_49");
    REQUIRE(prokop.has_value());
    CHECK(prokop->card.empty());
    CHECK(personas.persona("kkut_additive_man_49").sheet.find("son in the guard") ==
          std::string::npos);
    // Passed clean, with no bio row at all: under the bio rule alone she was faceless.
    CHECK(personas.persona("kkut_additive_man_50").sheet.find("pillory steps") !=
          std::string::npos);
}

TEST_CASE("the persona carries the written answers for where the save says things stand") {
    Db db;
    PersonaDb personas(db.path());
    // A stranger: the favour is the answer to "help", with the row that agrees to it, the
    // account of himself is ready, and only the open topic is on offer.
    Standing first;
    first.met = 1;
    Persona persona = personas.persona("kkut_additive_man_48", {}, {}, true, {}, {}, first);
    CHECK(persona.scripted.offer == "Julian has my saw. Eight groschen to whoever brings it back.");
    CHECK(persona.scripted.accept_row == "I'll get your hand saw back from Julian.");
    CHECK(persona.scripted.about == "Wenzel. Carpenter, when I have my tools.");
    REQUIRE(persona.scripted.topics.size() == 1);
    CHECK(persona.scripted.topics[0].key == "daily_work");
    CHECK(persona.scripted.withheld);

    // Third meeting with the saw back: the written help stands in, the lead to Bartel
    // being the variant for a third meeting; a topic about Julian is no longer true and
    // goes, and the confided one opens.
    Standing third;
    third.met = 3;
    third.quests["quest-wenzel"] = "done";
    QuestFacts done;
    done.done = true;
    persona = personas.persona("kkut_additive_man_48", {}, {}, true, done, {}, third);
    CHECK(persona.scripted.offer.empty());
    CHECK(persona.scripted.help == "No. I have my saw and my peace.");
    CHECK(persona.scripted.about.empty());
    CHECK(persona.scripted.about_told);
    REQUIRE(persona.scripted.topics.size() == 2);
    CHECK(persona.scripted.topics[0].key == "daily_work");
    CHECK(persona.scripted.topics[1].key == "his_wife");
    CHECK_FALSE(persona.scripted.withheld);
    // Second meeting, same state: the other variant, the lead, since Bartel's is untaken.
    Standing second = third;
    second.met = 2;
    persona = personas.persona("kkut_additive_man_48", {}, {}, true, done, {}, second);
    CHECK(persona.scripted.help == "Ask Bartel. He is always wanting something.");
    second.quests["quest-bartel"] = "accepted";
    persona = personas.persona("kkut_additive_man_48", {}, {}, true, done, {}, second);
    CHECK(persona.scripted.help == "No. I have my saw and my peace.");

    // With no standing given at all, the plan is for a first meeting, as before the field.
    CHECK(personas.persona("kkut_additive_man_48").scripted.about ==
          "Wenzel. Carpenter, when I have my tools.");
}

TEST_CASE("a soul on a chain has every link, in order, and the conversation is about one of them") {
    Db db;
    PersonaDb personas(db.path());
    const std::optional<Soul> ondra = personas.find("kkut_additive_man_51");
    REQUIRE(ondra.has_value());
    // Both, the chain's order winning over the order dealt; the first is `quest` too.
    REQUIRE(ondra->quests.size() == 2);
    CHECK(ondra->quests[0].id == "quest-ondra-axe");
    CHECK(ondra->quests[1].id == "quest-ondra-saw");
    CHECK(ondra->quest.id == "quest-ondra-axe");
    const Quest& axe = ondra->quests[0];
    const Quest& saw = ondra->quests[1];
    CHECK(axe.chain_id == "ondra_tools");
    CHECK(axe.chain_order == 1);
    CHECK(axe.after_quest_id.empty());
    CHECK(axe.dispute);
    CHECK(axe.pays_money());
    CHECK(axe.reward_text.empty());
    CHECK(axe.apologise == "Vit wants you to admit he paid your slate.");
    CHECK(axe.apologise_yes == "Fine. He paid. Tell him.");
    CHECK(axe.apologise_no == "I admit nothing.");
    CHECK(saw.chain_order == 2);
    CHECK(saw.after_quest_id == "quest-ondra-axe");
    CHECK(saw.after_item_name == "work axe");
    CHECK_FALSE(saw.dispute);
    CHECK_FALSE(saw.pays_money());
    CHECK(saw.reward_text == "thanks and a favour owed");
    CHECK(saw.apologise.empty());
    // The lines written for In the Stocks' shape (notes.md 9 Oct 2026): the hand-in as a
    // row with a lie beside it, and Henry's second question under the offer.
    CHECK(axe.give == "Here. Your axe.");
    CHECK(axe.lie_have == "Vit never had it.");
    CHECK(axe.lie_have_reply == "Never had it? Then it is gone.");
    CHECK(axe.lie_have_caught == "Then what is that on your back?");
    CHECK(axe.has_lie_have());
    CHECK(axe.more_2 == "You want me to lie to him?");
    CHECK(axe.more_2_reply == "Not lie. Just do not say.");
    // The hail's follow-through and the thanks that lead on (notes.md 13 Oct 2026).
    CHECK(axe.greet_offer == "You are the one who helps folk, they say.");
    CHECK(axe.thanks_onward == "My axe. And now, wait.");  // undashed on the way in
    CHECK_FALSE(saw.has_lie_have());
    // The unyielding holder and the one named after his refusal (notes.md 13 Oct 2026).
    CHECK(axe.unyielding);
    CHECK(axe.helper_name == "Elsbeth");
    CHECK(axe.greet_rebuffed == "Would not hear of it? Then his chest. Elsbeth keeps the house.");
    CHECK(axe.has_helper());
    CHECK_FALSE(saw.unyielding);
    // The holder has both from his end, with no lines of his own here -- except that he
    // will not be moved, which his own paragraph turns on.
    const std::optional<Soul> vit = personas.find("kkut_additive_man_52");
    REQUIRE(vit.has_value());
    REQUIRE(vit->quests.size() == 2);
    CHECK(vit->quests[0].role == QuestRole::Target);
    CHECK(vit->quests[0].apologise.empty());
    CHECK(vit->quests[0].unyielding);
    CHECK(vit->quests[0].helper_name.empty());

    // Untouched: the axe is asked, the saw is not mentioned.
    Standing first;
    first.met = 1;
    Persona fresh = personas.persona("kkut_additive_man_51", {}, {}, true, {}, {}, first);
    CHECK(fresh.quest_of.id == "quest-ondra-axe");
    CHECK(fresh.scripted.offer == "Vit has my axe and a grudge. Ten groschen to get it back.");
    CHECK(fresh.quest.block.find("saw") == std::string::npos);
    CHECK(fresh.quest.question.find("work axe") != std::string::npos);

    // The axe accepted and the player back from Vit with his condition (the save's flag):
    // the row that puts it, and his answer ready. The first quest's facts come off the
    // dump's fields when the caller has them, else off the standing.
    Standing carrying;
    carrying.met = 2;
    carrying.quests["quest-ondra-axe"] = "accepted";
    carrying.flags["quest-ondra-axe"] = "b";
    Persona asked = personas.persona("kkut_additive_man_51", {}, {}, true, {}, {}, carrying);
    CHECK(asked.quest_of.id == "quest-ondra-axe");
    CHECK(asked.quest.apologise_row == "Vit wants you to admit he paid your slate.");
    CHECK(asked.quest.apologise_command == kQuestApologise);
    CHECK(asked.scripted.offer.empty());

    // The axe home: the saw is the quest of the moment, with the axe remembered in a clause
    // and not as a second paragraph.
    Standing after;
    after.met = 3;
    after.quests["quest-ondra-axe"] = "done";
    Persona next = personas.persona("kkut_additive_man_51", {}, {}, true, {}, {}, after);
    CHECK(next.quest_of.id == "quest-ondra-saw");
    CHECK(next.scripted.offer == "I have my axe. Now the saw.");
    CHECK(next.scripted.accept_row == "I'll get your saw back from Vit.");
    CHECK(next.quest.block.find("already got your work axe back") != std::string::npos);
    CHECK(next.quest.block.find("which is settled") == std::string::npos);
    CHECK(next.quest.question.find("saw") != std::string::npos);
    // The counts by id off the begin line decide which thing is in the player's hands.
    Approach hands;
    hands.have = 0;
    hands.holding = {{"quest-ondra-axe", 0}, {"quest-ondra-saw", 1}};
    Persona back = personas.persona("kkut_additive_man_51", hands, {}, true, {}, {}, after);
    CHECK(back.quest_of.id == "quest-ondra-saw");
    REQUIRE(back.quest.opening_commands.size() == 2);
    CHECK(back.quest.opening_commands[1] == kQuestComplete);
}

TEST_CASE("a story's later scene is locked until the scene before is done, open when it ended as the link says, and void otherwise") {
    Db db;
    PersonaDb personas(db.path());
    const std::optional<Soul> marta = personas.find("ksuc_woman_12");
    REQUIRE(marta.has_value());
    REQUIRE(marta->quests.size() == 1);
    const Quest& letter = marta->quests[0];
    CHECK(letter.is_deliver());
    CHECK(letter.chain_id.empty());
    CHECK(letter.chain_order == 2);
    CHECK(letter.after_quest_id == "scene-ask");
    CHECK(letter.after_how == "truth");
    CHECK(letter.after_item_name.empty());
    const std::optional<Soul> hugo = personas.find("ksuc_poacher_3");
    REQUIRE(hugo.has_value());
    REQUIRE(hugo->quests.size() == 2);
    const Quest& ask = hugo->quests[0].is_ask() ? hugo->quests[0] : hugo->quests[1];
    CHECK(ask.is_ask());
    CHECK(ask.after_quest_id.empty());
    CHECK(ask.tell == "William told me. The birds were poisoned.");

    // Nothing moved: Marta has no letter to send yet, and does not bring it up.
    Standing first;
    first.met = 1;
    Persona locked = personas.persona("ksuc_woman_12", {}, {}, true, {}, {}, first);
    CHECK(locked.quest_of.id == "scene-letter");
    CHECK(locked.quest.block.find("one thing at a time") != std::string::npos);
    CHECK(locked.quest.question.empty());
    CHECK(locked.scripted.offer.empty());

    // The ask done and the truth carried back (the dump's /persuaded): the letter is hers to ask.
    Standing told;
    told.met = 1;
    told.quests["scene-ask"] = "done";
    told.hows["scene-ask"] = "persuaded";
    Persona open = personas.persona("ksuc_woman_12", {}, {}, true, {}, {}, told);
    CHECK(open.quest.block.find("written a letter to Hugo") != std::string::npos);
    CHECK(open.quest.on_yes == kQuestAccept);
    CHECK(open.scripted.offer == "Now Hugo knows, take him this.");
    CHECK(open.scripted.accept_row == "I'll take your letter to Hugo.");
    // A plain complete has no how, and is the truth too.
    Standing plain = told;
    plain.hows.clear();
    CHECK(personas.persona("ksuc_woman_12", {}, {}, true, {}, {}, plain).quest.on_yes == kQuestAccept);

    // The ask ended on a lie: the story went past this scene. No paragraph, no offer, and
    // nothing to say that there might have been.
    Standing lied = told;
    lied.hows["scene-ask"] = "lied";
    Persona past = personas.persona("ksuc_woman_12", {}, {}, true, {}, {}, lied);
    CHECK(past.quest.block.empty());
    CHECK(past.quest.question.empty());
    CHECK(past.quest.rows.empty());
    CHECK(past.scripted.offer.empty());
    CHECK(past.scripted.accept_row.empty());

    // Two scenes of one giver off the same gate are offered in scene order, not id order
    // (the headless matrix found *Letter of the Law*, scene 2, hidden behind scenes 3 and 4).
    const std::optional<Soul> oldrich = personas.find("ksuc_man_64");
    REQUIRE(oldrich.has_value());
    REQUIRE(oldrich->quests.size() == 2);
    CHECK(oldrich->quests[0].id == "scene-z-second");
    CHECK(oldrich->quests[1].id == "scene-a-third");
    Persona both = personas.persona("ksuc_man_64", {}, {}, true, {}, {}, told);
    CHECK(both.quest_of.id == "scene-z-second");
    CHECK(both.scripted.offer == "Take Dorota her purse back first.");
    // The purse is a thing going back to its owner, not a letter, and the writer's report and
    // reply by what Henry told her are read in (told_<how>, told_reply_<how>).
    CHECK(both.quest_of.item_kind == "thing");
    CHECK(both.quest_of.carries_thing());
    CHECK_FALSE(oldrich->quests[1].carries_thing());
    REQUIRE(both.quest_of.told_by_how.size() == 2);
    for (const Quest::Told& t : both.quest_of.told_by_how) {
        if (t.how == "truth") {
            CHECK(t.row == "She has it, and I told her where it came from.");
            CHECK(t.reply == "You told her. Well.");
        } else {
            CHECK(t.how == "pinned");
            CHECK(t.row.empty());
            CHECK(t.reply == "You said my name.");
        }
    }
    Standing second_done = told;
    second_done.quests["scene-z-second"] = "done";
    const Persona note = personas.persona("ksuc_man_64", {}, {}, true, {}, {}, second_done);
    CHECK(note.quest_of.id == "scene-a-third");
    // The note's dictation is read in whole: the lines around it, each part's words and
    // Henry's ways with it, and the writer's report by the receiver's verdict.
    const Quest::Dictation& dictation = note.quest_of.dictation;
    CHECK(note.quest_of.has_dictation());
    CHECK(dictation.begin == "Sit. Write what I say.");
    CHECK(dictation.done == "Fold it. Take it to her.");
    CHECK(dictation.resume == "Let us get your note written.");
    CHECK(dictation.greet_unwritten == "Back to the writing?");
    CHECK(dictation.parts[0].dictate == "Dorota, the purse was yours.");
    CHECK(dictation.parts[0].soften.row == "I will put it more gently.");
    CHECK(dictation.parts[0].soften.reply == "If you must.");
    CHECK(dictation.parts[0].outcomes() == std::vector<int>{0, 2});
    CHECK(dictation.parts[1].reconsider.reply == "No. Leave it.");
    CHECK(dictation.parts[1].outcomes() == std::vector<int>{0, 1});
    CHECK(dictation.parts[2].embellish.row == "I will give it weight.");
    CHECK(dictation.parts[2].outcomes() == std::vector<int>{0, 3});
    CHECK(note.scripted.accept_row == "I'll write your note down for you and take it to Dorota.");
    bool cursed = false;
    for (const Quest::Told& t : note.quest_of.told_by_how) {
        if (t.how == "cursed") cursed = t.row == "She tore it up." && t.reply == "Tore it. Well.";
    }
    CHECK(cursed);

    // Hugo's own ask reads as an ask: the accept row, and the rows once William has told.
    Persona asking = personas.persona("ksuc_poacher_3", {}, {}, true, {}, {}, first);
    CHECK(asking.quest_of.id == "scene-ask");
    CHECK(asking.scripted.accept_row == "I'll go to William and get the truth of it for you.");
    Standing handed;
    handed.met = 2;
    handed.quests["scene-ask"] = "handed";
    handed.hows["scene-ask"] = "threatened";
    Persona back = personas.persona("ksuc_poacher_3", {}, {}, true, {}, {}, handed);
    REQUIRE(back.quest.rows.size() == 3);
    CHECK(back.quest.rows[0].text == "William told me. The birds were poisoned.");
    CHECK(back.quest.rows[0].command == kQuestComplete);
}

TEST_CASE("a database from before the pipeline has no written answers, and says nothing of it") {
    Db db;
    db.exec("DROP TABLE npc_person; DROP TABLE npc_knowledge; DROP TABLE npc_seed_answer;"
            " DROP TABLE npc_remark;");
    PersonaDb personas(db.path());
    const std::optional<Soul> wenzel = personas.find("kkut_additive_man_48");
    REQUIRE(wenzel.has_value());
    CHECK(wenzel->self_account.empty());
    CHECK(wenzel->knowledge.empty());
    CHECK(wenzel->help_replies.empty());
    CHECK(wenzel->remarks.empty());
    // His card has no bio row to vouch for it, so under the old rule it is withheld; the
    // favour is still his to offer, being in tables of its own.
    const Persona persona = personas.persona("kkut_additive_man_48");
    CHECK(persona.scripted.offer == "Julian has my saw. Eight groschen to whoever brings it back.");
    CHECK(persona.scripted.about.empty());
    CHECK(persona.scripted.topics.empty());
}
