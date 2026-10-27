#include "npcdb.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>

namespace kcd2 {
namespace {

using DbHandle = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>;
using StmtHandle = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;

std::string placeholders(std::size_t n) {
    std::string s;
    for (std::size_t i = 0; i < n; ++i) {
        if (i) s += ",";
        s += "?";
    }
    return s;
}

[[noreturn]] void fail(sqlite3* db, const std::string& what) {
    throw std::runtime_error(what + ": " + sqlite3_errmsg(db));
}

void read_holder_lines(sqlite3* db, std::vector<Quest>& quests);
void read_brawlers(sqlite3* db, std::vector<Quest>& quests);
void read_household(sqlite3* db, std::vector<Quest>& quests);
std::vector<std::string> household_of(sqlite3* db, const std::string& guid);
void read_onward(sqlite3* db, std::vector<Quest>& quests, const std::set<std::string>& renamed);
void read_helper(sqlite3* db, std::vector<Quest>& quests, const std::set<std::string>& renamed);

}  // namespace

std::vector<Persona> read_personas(const std::string& db_path, const Filter& filter) {
    sqlite3* raw_db = nullptr;
    // Read-only so a half-written WAL from the Python name generator cannot be
    // damaged by opening it here.
    if (sqlite3_open_v2(db_path.c_str(), &raw_db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        DbHandle doomed(raw_db, &sqlite3_close);
        throw std::runtime_error("cannot open " + db_path + ": " +
                                 (raw_db ? sqlite3_errmsg(raw_db) : "out of memory"));
    }
    DbHandle db(raw_db, &sqlite3_close);

    std::string sql =
        "SELECT n.soul_name, g.full_name, g.household_key, n.gender, COALESCE(n.is_named, 0), "
        "COALESCE(n.occupation, '') "
        "FROM generated_name g JOIN npc n USING (soul_id) ";

    // Filters are ANDed, and every value is bound in the order the clauses were
    // appended so the placeholders line up.
    std::vector<std::string> bound;
    const struct {
        const char* column;
        const std::vector<std::string>* values;
    } clauses[] = {
        {"n.settlement", &filter.settlements},
        {"g.household_key", &filter.households},
        {"n.soul_name", &filter.souls},
    };
    for (const auto& c : clauses) {
        if (c.values->empty()) continue;
        sql += bound.empty() ? "WHERE " : "AND ";
        sql += std::string(c.column) + " IN (" + placeholders(c.values->size()) + ") ";
        bound.insert(bound.end(), c.values->begin(), c.values->end());
    }

    // Sorting keeps the generated XML stable so a rebuild produces no diff when
    // the database has not changed.
    sql += "ORDER BY n.soul_name";

    sqlite3_stmt* raw_stmt = nullptr;
    if (sqlite3_prepare_v2(db.get(), sql.c_str(), -1, &raw_stmt, nullptr) != SQLITE_OK) {
        fail(db.get(), "preparing query failed");
    }
    StmtHandle stmt(raw_stmt, &sqlite3_finalize);

    for (std::size_t i = 0; i < bound.size(); ++i) {
        sqlite3_bind_text(stmt.get(), static_cast<int>(i + 1), bound[i].c_str(), -1,
                          SQLITE_TRANSIENT);
    }

    auto text = [&](int col) {
        const unsigned char* p = sqlite3_column_text(stmt.get(), col);
        return p ? std::string(reinterpret_cast<const char*>(p)) : std::string();
    };

    std::vector<Persona> out;
    int rc;
    while ((rc = sqlite3_step(stmt.get())) == SQLITE_ROW) {
        Persona p;
        p.soul_name = text(0);
        p.full_name = text(1);
        p.household_key = text(2);
        p.gender = text(3);
        p.game_named = sqlite3_column_int(stmt.get(), 4) != 0;
        p.beggar = text(5) == "beggar";
        if (!p.soul_name.empty() && !p.full_name.empty()) {
            out.push_back(std::move(p));
        }
    }
    if (rc != SQLITE_DONE) {
        fail(db.get(), "reading rows failed");
    }
    return out;
}

std::vector<Quest> read_quests(const std::string& db_path, const std::vector<Persona>& personas) {
    sqlite3* raw_db = nullptr;
    if (sqlite3_open_v2(db_path.c_str(), &raw_db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        DbHandle doomed(raw_db, &sqlite3_close);
        throw std::runtime_error("cannot open " + db_path + ": " +
                                 (raw_db ? sqlite3_errmsg(raw_db) : "out of memory"));
    }
    DbHandle db(raw_db, &sqlite3_close);

    // Both tables are the bio generator's, and a database it has not run the quest
    // pass over has neither. That is not an error here: it means no quests.
    for (const char* table : {"npc_quest", "npc_quest_text"}) {
        if (sqlite3_table_column_metadata(db.get(), nullptr, table, "quest_id", nullptr, nullptr,
                                          nullptr, nullptr, nullptr) != SQLITE_OK) {
            return {};
        }
    }

    // Only quests with text: a quest the model has not written yet has no journal
    // entry to show and no card clause to be offered through. The slate is the circle
    // dealer's table and a database from before it has none, so its four columns are
    // read through a left join when the table is there and left at their defaults when
    // it is not.
    const bool slate = sqlite3_table_column_metadata(db.get(), nullptr, "npc_slate", "quest_id",
                                                     nullptr, nullptr, nullptr, nullptr,
                                                     nullptr) == SQLITE_OK;
    // The link columns came later than the slate (quest-design/scenes.md); a slate from
    // before them has no links.
    const bool links = slate && sqlite3_table_column_metadata(db.get(), nullptr, "npc_slate", "after_quest_id",
                                                              nullptr, nullptr, nullptr, nullptr,
                                                              nullptr) == SQLITE_OK;
    // Reward kinds likewise: a slate without them pays as the npc_quest row says.
    const bool rewards = slate && sqlite3_table_column_metadata(db.get(), nullptr, "npc_slate", "reward_kind",
                                                                nullptr, nullptr, nullptr, nullptr,
                                                                nullptr) == SQLITE_OK;
    const std::string sql =
        std::string("SELECT q.quest_id, q.kind, q.giver_soul, q.giver_name, g.soul_id, "
                    "q.target_soul, q.target_name, h.soul_id, q.item_class, q.item_name, "
                    "q.reward_tier, q.reward_groschen, t.name, t.description, t.objective, "
                    "t.log_started, t.log_completed, t.log_giver_dead, q.skill, q.difficulty") +
        (slate ? ", COALESCE(s.chain_id, ''), COALESCE(s.chain_order, 0), "
                 "COALESCE(s.requires_quest_id, ''), COALESCE(s.dispute, 0) "
               : ", '', 0, '', 0 ") +
        ", COALESCE(h.gender, '') " +
        (links ? ", COALESCE(s.after_quest_id, ''), COALESCE(s.after_how, '') " : ", '', '' ") +
        (rewards ? ", COALESCE(s.reward_kind, ''), COALESCE(s.reward_text, '') " : ", '', '' ") +
        ", COALESCE(g.gender, '') " +
        "FROM npc_quest q "
        "JOIN npc_quest_text t USING (quest_id) "
        "JOIN npc g ON g.soul_name = q.giver_soul "
        "LEFT JOIN npc h ON h.soul_name = q.target_soul " +
        (slate ? "LEFT JOIN npc_slate s ON s.quest_id = q.quest_id " : "") +
        "ORDER BY q.quest_id";

    sqlite3_stmt* raw_stmt = nullptr;
    if (sqlite3_prepare_v2(db.get(), sql.c_str(), -1, &raw_stmt, nullptr) != SQLITE_OK) {
        fail(db.get(), "preparing quest query failed");
    }
    StmtHandle stmt(raw_stmt, &sqlite3_finalize);

    std::set<std::string> renamed;
    for (const Persona& p : personas) renamed.insert(p.soul_name);

    auto text = [&](int col) {
        const unsigned char* p = sqlite3_column_text(stmt.get(), col);
        return p ? std::string(reinterpret_cast<const char*>(p)) : std::string();
    };

    std::vector<Quest> out;
    int rc;
    while ((rc = sqlite3_step(stmt.get())) == SQLITE_ROW) {
        Quest q;
        q.id = text(0);
        q.kind = text(1);
        q.giver_soul = text(2);
        q.giver_name = text(3);
        q.giver_guid = text(4);
        q.target_soul = text(5);
        q.target_name = text(6);
        q.target_guid = text(7);
        q.item_class = text(8);
        q.item_name = text(9);
        q.reward_tier = text(10);
        q.reward_groschen = sqlite3_column_int(stmt.get(), 11);
        q.name = text(12);
        q.description = text(13);
        q.objective = text(14);
        q.log_started = text(15);
        q.log_completed = text(16);
        q.log_giver_dead = text(17);
        q.skill = text(18);
        q.difficulty = text(19);
        q.chain_id = text(20);
        q.chain_order = sqlite3_column_int(stmt.get(), 21);
        q.requires_quest_id = text(22);
        q.dispute = sqlite3_column_int(stmt.get(), 23) != 0;
        q.target_gender = text(24);
        q.after_quest_id = text(25);
        q.after_how = text(26);
        q.reward_kind = text(27);
        q.reward_text = text(28);
        q.giver_gender = text(29);
        if (q.id.empty() || q.giver_guid.empty()) continue;
        if (!renamed.count(q.giver_soul)) continue;
        // A find, a pray, a leave or a return has no target at all; every other kind needs
        // a renamed one.
        if (q.is_find() || q.is_pray() || q.is_spot()) {
            q.target_soul.clear();
            q.target_name.clear();
            q.target_guid.clear();
        } else if (q.target_guid.empty() || !renamed.count(q.target_soul)) {
            continue;
        }
        // Every seeded quest pays the giver's money, and an errand the giver pays money
        // for has no pay option (he would have paid the holder himself); the generator's
        // npc_quest_lines row, read below, overrides this either way.
        q.pay_groschen = 0;
        out.push_back(std::move(q));
    }
    if (rc != SQLITE_DONE) {
        fail(db.get(), "reading quest rows failed");
    }
    read_holder_lines(db.get(), out);
    read_brawlers(db.get(), out);
    read_household(db.get(), out);
    read_onward(db.get(), out, renamed);
    read_helper(db.get(), out, renamed);
    return out;
}

namespace {

void read_helper(sqlite3* db, std::vector<Quest>& quests, const std::set<std::string>& renamed) {
    // The one who can let Henry in (In the Stocks' Elsbeth): by soul_name as the dealer
    // names everybody, resolved to a guid and a gender. They are spoken to through a menu
    // and a marker sits on them, so, like an onward soul, they must be renamed and
    // talkable; a quest whose helper is not is dropped rather than shipped with an
    // optional objective nobody can take up. The holder's household goes with it: the
    // `letin` contexts cover everyone who sleeps under the holder's roof, the helper among
    // them whether or not she sleeps there.
    const char* sql = "SELECT soul_id, COALESCE(gender, '') FROM npc WHERE soul_name = ?";
    sqlite3_stmt* raw_stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &raw_stmt, nullptr) != SQLITE_OK) {
        fail(db, "preparing helper query failed");
    }
    StmtHandle stmt(raw_stmt, &sqlite3_finalize);
    for (auto it = quests.begin(); it != quests.end();) {
        Quest& q = *it;
        if (q.unyielding && !q.is_retrieve()) {
            throw std::runtime_error("quest " + q.id + " is unyielding but is a " + q.kind +
                                     ", and only a retrieve's holder refuses outright");
        }
        if (!q.chest_pos.empty() && !q.is_retrieve()) {
            throw std::runtime_error("quest " + q.id + " names chest_pos but is a " + q.kind);
        }
        if (!q.fast_travel_to.empty() != (q.fast_travel_at >= 0)) {
            throw std::runtime_error("quest " + q.id + " has one of fast_travel_to and fast_travel_at; the carrying wants both");
        }
        // An exposure (npcdb.hpp, expose): the witness by soul_name, resolved to a guid --
        // they are never spoken to or marked, so they need not be renamed -- and the
        // household the holder is caught in, which is the witness's, for the welcome that
        // lets Henry stand in it at night. The earlier quest whose Document is the thing
        // must be one of ours, dealt before this one in the same run.
        if (!q.item_of.empty()) {
            if (!q.is_retrieve()) throw std::runtime_error("quest " + q.id + " names item_of but is a " + q.kind);
            const bool known = std::any_of(quests.begin(), quests.end(),
                                           [&](const Quest& o) { return o.id == q.item_of && o.has_dictation(); });
            if (!known) {
                throw std::runtime_error("quest " + q.id + " item_of names " + q.item_of +
                                         ", which is not a dictated letter among the quests read");
            }
        }
        if (!q.holder_enters.empty() && q.seen_with.empty()) {
            throw std::runtime_error("quest " + q.id + " has enters but no seen_with to be seen going in to");
        }
        if (q.any_follow() && q.seen_with.empty()) {
            throw std::runtime_error("quest " + q.id + " has the followed lines but no seen_with to be walking to");
        }
        if (!q.seen_with.empty()) {
            if (!q.is_retrieve()) throw std::runtime_error("quest " + q.id + " names seen_with but is a " + q.kind);
            if (!q.has_expose()) {
                throw std::runtime_error("quest " + q.id + " names seen_with but lacks expose, expose_yes, "
                                         "log_caught or an hour_from/hour_to window");
            }
            if (q.any_follow() && !q.has_follow()) {
                throw std::runtime_error("quest " + q.id + " has some of the followed lines but not all: followed, "
                                         "followed_reply and log_followed");
            }
            if ((!q.holder_follow_warn.empty() || !q.holder_follow_over.empty()) && !q.has_follow()) {
                throw std::runtime_error("quest " + q.id + " has follow_warn or follow_over but no followed street");
            }
            if (q.seen_with == q.giver_soul || q.seen_with == q.target_soul) {
                throw std::runtime_error("quest " + q.id + " names one of its own parties as seen_with");
            }
            if (q.any_bedside() && !q.has_bedside()) {
                throw std::runtime_error("quest " + q.id + " has some of the bedside's lines but not all: wake, "
                                         "wake_reply, denounce, denounce_reply, blackmail, blackmail_yes, "
                                         "blackmail_no, open_denounced, log_denounced, log_completed_denounced, "
                                         "log_completed_exposed_paid and blackmail_groschen");
            }
            // The caught token and the naming token are the quest's one token class.
            if (!q.helper_soul.empty()) {
                throw std::runtime_error("quest " + q.id + " has both a helper and seen_with; one way in per errand");
            }
            if (q.seen_with_name.empty()) q.seen_with_name = q.seen_with;
            sqlite3_reset(stmt.get());
            sqlite3_bind_text(stmt.get(), 1, q.seen_with.c_str(), -1, SQLITE_TRANSIENT);
            const int rc = sqlite3_step(stmt.get());
            if (rc == SQLITE_DONE) {
                throw std::runtime_error("quest " + q.id + " seen_with names " + q.seen_with +
                                         ", which npc does not have");
            }
            if (rc != SQLITE_ROW) fail(db, "reading seen_with soul failed");
            const unsigned char* p = sqlite3_column_text(stmt.get(), 0);
            q.seen_with_guid = p ? reinterpret_cast<const char*>(p) : "";
            if (q.seen_with_guid.empty()) {
                throw std::runtime_error("quest " + q.id + " seen_with " + q.seen_with + " has no soul guid");
            }
            q.holder_household_guids = household_of(db, q.seen_with_guid);
            if (q.holder_household_guids.empty()) q.holder_household_guids.push_back(q.seen_with_guid);
            if (std::find(q.holder_household_guids.begin(), q.holder_household_guids.end(), q.target_guid) ==
                q.holder_household_guids.end()) {
                q.holder_household_guids.push_back(q.target_guid);
            }
        }
        if (q.helper_soul.empty()) { ++it; continue; }
        if (!q.is_unyielding()) {
            throw std::runtime_error("quest " + q.id + " names helper_soul but its holder can be moved; "
                                     "a helper goes with unyielding");
        }
        if (q.helper_soul == q.giver_soul || q.helper_soul == q.target_soul) {
            throw std::runtime_error("quest " + q.id + " names one of its own parties as helper");
        }
        if (!q.has_helper()) {
            throw std::runtime_error("quest " + q.id + " names helper_soul but lacks helper_door, "
                                     "helper_door_yes, helper_door_price, log_named or objective_helper");
        }
        if (!q.helper_key.empty() && !q.has_key()) {
            throw std::runtime_error("quest " + q.id + " has a helper_key row but lacks chest_pos, "
                                     "helper_key_yes, helper_key_no or helper_key_price");
        }
        if (q.helper_name.empty()) q.helper_name = q.helper_soul;
        sqlite3_reset(stmt.get());
        sqlite3_bind_text(stmt.get(), 1, q.helper_soul.c_str(), -1, SQLITE_TRANSIENT);
        const int rc = sqlite3_step(stmt.get());
        if (rc == SQLITE_DONE) {
            throw std::runtime_error("quest " + q.id + " helper_soul names " + q.helper_soul +
                                     ", which npc does not have");
        }
        if (rc != SQLITE_ROW) fail(db, "reading helper soul failed");
        const unsigned char* p = sqlite3_column_text(stmt.get(), 0);
        q.helper_guid = p ? reinterpret_cast<const char*>(p) : "";
        p = sqlite3_column_text(stmt.get(), 1);
        q.helper_gender = p ? reinterpret_cast<const char*>(p) : "";
        if (q.helper_guid.empty() || !renamed.count(q.helper_soul)) {
            it = quests.erase(it);
            continue;
        }
        q.holder_household_guids = household_of(db, q.target_guid);
        if (q.holder_household_guids.empty()) q.holder_household_guids.push_back(q.target_guid);
        if (std::find(q.holder_household_guids.begin(), q.holder_household_guids.end(), q.helper_guid) ==
            q.holder_household_guids.end()) {
            q.holder_household_guids.push_back(q.helper_guid);
        }
        ++it;
    }
}

void read_onward(sqlite3* db, std::vector<Quest>& quests, const std::set<std::string>& renamed) {
    // The one an ask's word is carried on to, by soul_name as the dealer names everybody,
    // resolved to a guid. Unlike a brawler they are spoken to and a marker sits on them, so
    // they must be renamed and talkable the way a giver is; a quest whose onward soul is
    // not is dropped the way one with such a giver is, rather than shipped with a last
    // objective nobody can finish.
    const char* sql = "SELECT soul_id FROM npc WHERE soul_name = ?";
    sqlite3_stmt* raw_stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &raw_stmt, nullptr) != SQLITE_OK) {
        fail(db, "preparing onward query failed");
    }
    StmtHandle stmt(raw_stmt, &sqlite3_finalize);
    for (auto it = quests.begin(); it != quests.end();) {
        Quest& q = *it;
        if (q.onward_soul.empty()) { ++it; continue; }
        if (!q.is_ask()) {
            throw std::runtime_error("quest " + q.id + " names onward_soul but is a " + q.kind +
                                     ", and only an ask carries its word on");
        }
        if (q.onward_soul == q.giver_soul || q.onward_soul == q.target_soul) {
            throw std::runtime_error("quest " + q.id + " carries its word on to one of its own parties");
        }
        if (q.log_onward.empty() || q.objective_onward.empty() || q.onward_hand.empty()) {
            throw std::runtime_error("quest " + q.id + " carries its word on but lacks log_onward, "
                                     "objective_onward or onward_hand");
        }
        sqlite3_reset(stmt.get());
        sqlite3_bind_text(stmt.get(), 1, q.onward_soul.c_str(), -1, SQLITE_TRANSIENT);
        const int rc = sqlite3_step(stmt.get());
        if (rc == SQLITE_DONE) {
            throw std::runtime_error("quest " + q.id + " onward_soul names " + q.onward_soul +
                                     ", which npc does not have");
        }
        if (rc != SQLITE_ROW) fail(db, "reading onward soul failed");
        const unsigned char* p = sqlite3_column_text(stmt.get(), 0);
        q.onward_guid = p ? reinterpret_cast<const char*>(p) : "";
        if (q.onward_guid.empty() || !renamed.count(q.onward_soul)) {
            it = quests.erase(it);
            continue;
        }
        ++it;
    }
}

void read_holder_lines(sqlite3* db, std::vector<Quest>& quests) {
    // The holder's lines come from the holder's own generation call and live in their
    // own table, so a database from before the holder spoke has quests and no lines.
    // That is not an error: the mod has a plain line for every slot. The same table
    // carries the journal's log for each way the thing was won and the holder's death,
    // under the names the generator writes them.
    if (quests.empty()) return;
    if (sqlite3_table_column_metadata(db, nullptr, "npc_quest_lines", "quest_id", nullptr,
                                      nullptr, nullptr, nullptr, nullptr) != SQLITE_OK) {
        return;
    }
    const char* sql = "SELECT quest_id, field, text FROM npc_quest_lines";
    sqlite3_stmt* raw_stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &raw_stmt, nullptr) != SQLITE_OK) {
        fail(db, "preparing quest lines query failed");
    }
    StmtHandle stmt(raw_stmt, &sqlite3_finalize);

    auto text = [&](int col) {
        const unsigned char* p = sqlite3_column_text(stmt.get(), col);
        return p ? std::string(reinterpret_cast<const char*>(p)) : std::string();
    };
    auto slot = [](Quest& q, const std::string& field) -> std::string* {
        if (field == "open") return &q.holder_open;
        if (field == "refuse") return &q.holder_refuse;
        if (field == "persuade") return &q.holder_persuade;
        if (field == "persuade_yes") return &q.holder_persuade_yes;
        if (field == "persuade_no") return &q.holder_persuade_no;
        if (field == "threaten") return &q.holder_threaten;
        if (field == "threaten_yes") return &q.holder_threaten_yes;
        if (field == "threaten_no") return &q.holder_threaten_no;
        if (field == "pay") return &q.holder_pay;
        if (field == "pay_yes") return &q.holder_pay_yes;
        if (field == "leave") return &q.holder_leave;
        if (field == "leave_reply") return &q.holder_leave_reply;
        if (field == "broker") return &q.holder_broker;
        if (field == "broker_reply") return &q.holder_broker_reply;
        if (field == "brokered") return &q.holder_brokered;
        if (field == "brokered_yes") return &q.holder_brokered_yes;
        if (field == "nested") return &q.holder_nested;
        if (field == "nested_yes") return &q.holder_nested_yes;
        if (field == "log_completed_persuaded") return &q.log_completed_persuaded;
        if (field == "log_completed_threatened") return &q.log_completed_threatened;
        if (field == "log_completed_paid") return &q.log_completed_paid;
        if (field == "log_completed_stolen") return &q.log_completed_stolen;
        if (field == "log_completed_brokered") return &q.log_completed_brokered;
        if (field == "log_completed_lied") return &q.log_completed_lied;
        if (field == "log_completed_nested") return &q.log_completed_nested;
        if (field == "log_target_dead") return &q.log_target_dead;
        if (field == "letter") return &q.letter;
        if (field == "hand") return &q.receiver_hand;
        if (field == "hand_reply") return &q.receiver_hand_reply;
        if (field == "hand_read") return &q.receiver_hand_read;
        if (field == "hand_opened") return &q.receiver_hand_opened;
        if (field == "report") return &q.giver_report;
        if (field == "keep") return &q.giver_keep;
        if (field == "kept_reply") return &q.giver_kept_reply;
        if (field == "log_completed_kept") return &q.log_completed_kept;
        if (field == "log_opened") return &q.log_opened;
        if (field == "item_kind") return &q.item_kind;
        if (field == "tell_truth") return &q.tell_truth;
        if (field == "tell_truth_reply") return &q.tell_truth_reply;
        if (field == "tell_cover") return &q.tell_cover;
        if (field == "tell_cover_reply") return &q.tell_cover_reply;
        if (field == "tell_named") return &q.tell_named;
        if (field == "tell_named_reply") return &q.tell_named_reply;
        if (field == "tell_heard") return &q.tell_heard;
        if (field == "tell_heard_reply") return &q.tell_heard_reply;
        if (field == "tell_vision") return &q.tell_vision;
        if (field == "tell_vision_reply") return &q.tell_vision_reply;
        if (field == "tell_tale") return &q.tell_tale;
        if (field == "tell_tale_reply") return &q.tell_tale_reply;
        if (field == "log_completed_fibbed") return &q.log_completed_fibbed;
        if (field == "tell_evade") return &q.tell_evade;
        if (field == "tell_evade_yes") return &q.tell_evade_yes;
        if (field == "tell_evade_no") return &q.tell_evade_no;
        if (field == "tell_threat") return &q.tell_threat;
        if (field == "tell_threat_yes") return &q.tell_threat_yes;
        if (field == "tell_threat_no") return &q.tell_threat_no;
        if (field == "log_completed_pinned") return &q.log_completed_pinned;
        if (field == "log_completed_covered") return &q.log_completed_covered;
        if (field == "log_completed_evaded") return &q.log_completed_evaded;
        if (field == "log_completed_caught") return &q.log_completed_caught;
        if (field == "lie_have") return &q.giver_lie_have;
        if (field == "tell") return &q.giver_tell;
        if (field == "lie") return &q.giver_lie;
        if (field == "quiet") return &q.giver_quiet;
        if (field == "lie_reply") return &q.giver_lie_reply;
        if (field == "quiet_reply") return &q.giver_quiet_reply;
        if (field == "log_seen") return &q.log_seen;
        if (field == "seen_mood") return &q.seen_mood;
        if (field == "log_completed_quiet") return &q.log_completed_quiet;
        if (field == "fought") return &q.giver_fought;
        if (field == "confess") return &q.giver_confess;
        if (field == "fought_reply") return &q.giver_fought_reply;
        if (field == "dead_reply") return &q.giver_dead_reply;
        if (field == "log_beaten") return &q.log_beaten;
        if (field == "log_completed_fought") return &q.log_completed_fought;
        if (field == "log_completed_dead") return &q.log_completed_dead;
        if (field == "spot_pos") return &q.spot_pos;
        if (field == "spot_text") return &q.spot_text;
        if (field == "hail") return &q.hail;
        if (field == "dress_target") return &q.dress_target;
        if (field == "log_rebuffed") return &q.log_rebuffed;
        if (field == "log_abandoned") return &q.log_abandoned;
        if (field == "chest_pos") return &q.chest_pos;
        if (field == "chest_paper") return &q.chest_paper;
        if (field == "chest_paper_name") return &q.chest_paper_name;
        if (field == "helper_soul") return &q.helper_soul;
        if (field == "helper_name") return &q.helper_name;
        if (field == "helper_skill") return &q.helper_skill;
        if (field == "helper_open") return &q.helper_open;
        if (field == "helper_open_reply") return &q.helper_open_reply;
        if (field == "helper_door") return &q.helper_door;
        if (field == "helper_door_yes") return &q.helper_door_yes;
        if (field == "helper_key") return &q.helper_key;
        if (field == "helper_key_yes") return &q.helper_key_yes;
        if (field == "helper_key_no") return &q.helper_key_no;
        if (field == "helper_leave") return &q.helper_leave;
        if (field == "helper_leave_reply") return &q.helper_leave_reply;
        if (field == "log_named") return &q.log_named;
        if (field == "log_letin") return &q.log_letin;
        if (field == "log_keyed") return &q.log_keyed;
        if (field == "objective_helper") return &q.objective_helper;
        // An exposure (npcdb.hpp, expose): the row, the journal's lines, the earlier quest
        // whose Document is the thing, and the one the holder is caught with.
        if (field == "expose") return &q.holder_expose;
        if (field == "expose_yes") return &q.holder_expose_yes;
        if (field == "log_caught") return &q.log_caught;
        if (field == "log_completed_exposed") return &q.log_completed_exposed;
        // The bedside (npcdb.hpp has_bedside): the waking and the two further rows.
        if (field == "wake") return &q.holder_wake;
        if (field == "wake_reply") return &q.holder_wake_reply;
        if (field == "enters") return &q.holder_enters;
        // The holder turning on Henry in the street (npcdb.hpp has_follow).
        if (field == "followed") return &q.holder_followed;
        if (field == "followed_reply") return &q.holder_followed_reply;
        if (field == "log_followed") return &q.log_followed;
        if (field == "follow_warn") return &q.holder_follow_warn;
        if (field == "follow_over") return &q.holder_follow_over;
        if (field == "denounce") return &q.holder_denounce;
        if (field == "denounce_reply") return &q.holder_denounce_reply;
        if (field == "blackmail") return &q.holder_blackmail;
        if (field == "blackmail_yes") return &q.holder_blackmail_yes;
        if (field == "blackmail_no") return &q.holder_blackmail_no;
        if (field == "open_denounced") return &q.holder_open_denounced;
        if (field == "log_denounced") return &q.log_denounced;
        if (field == "log_completed_denounced") return &q.log_completed_denounced;
        if (field == "log_completed_exposed_paid") return &q.log_completed_exposed_paid;
        if (field == "item_of") return &q.item_of;
        if (field == "seen_with") return &q.seen_with;
        if (field == "seen_with_name") return &q.seen_with_name;
        if (field == "log_prayed") return &q.log_prayed;
        if (field == "cue_sound") return &q.cue_sound;
        if (field == "loot_classes") return &q.loot_classes;
        if (field == "log_left") return &q.log_left;
        if (field == "check") return &q.target_check;
        if (field == "check_reply_1") return &q.target_check_reply[0];
        if (field == "check_reply_2") return &q.target_check_reply[1];
        if (field == "check_reply_3") return &q.target_check_reply[2];
        if (field == "early") return &q.target_early;
        if (field == "early_reply") return &q.target_early_reply;
        if (field == "log_check_1") return &q.log_check[0];
        if (field == "log_check_2") return &q.log_check[1];
        if (field == "log_check_3") return &q.log_check[2];
        if (field == "log_told") return &q.log_told;
        if (field == "gift_classes") return &q.gift_classes;
        if (field == "gift_take") return &q.holder_gift_take;
        if (field == "gift_take_reply") return &q.holder_gift_take_reply;
        if (field == "gift_refuse") return &q.holder_gift_refuse;
        if (field == "gift_refuse_reply") return &q.holder_gift_refuse_reply;
        if (field == "reward_class") return &q.reward_class;
        if (field == "keepsake_class") return &q.keepsake_class;
        if (field == "keepsake_name") return &q.keepsake_name;
        if (field == "thanks_keepsake") return &q.giver_thanks_keepsake;
        if (field == "onward_soul") return &q.onward_soul;
        if (field == "onward_name") return &q.onward_name;
        if (field == "onward_hand") return &q.onward_hand;
        if (field == "onward_hand_reply") return &q.onward_hand_reply;
        if (field == "onward_hand_read") return &q.onward_hand_read;
        if (field == "log_onward") return &q.log_onward;
        if (field == "objective_onward") return &q.objective_onward;
        // An ambush's and a buy's Document pages go where a deliver's letter does: one
        // page per quest, whatever the kind calls it.
        if (field == "directions" || field == "charm") return &q.letter;
        if (field == "direct") return &q.target_direct;
        if (field == "direct_reply") return &q.target_direct_reply;
        if (field == "truth") return &q.target_truth;
        if (field == "truth_reply") return &q.target_truth_reply;
        if (field == "log_directed") return &q.log_directed;
        if (field == "log_ambushed") return &q.log_ambushed;
        if (field == "carry_class") return &q.carry_class;
        if (field == "carry_name") return &q.carry_name;
        if (field == "routine") return &q.routine;
        // A dictated letter's lines (npcdb.hpp, dictate): the parts said, the asking rows
        // (the rest of the giver's side is the app's), the page pieces by outcome, the
        // receiver's remarks by outcome, his verdicts and the journal's lines.
        if (field == "letter_open") return &q.letter_open;
        if (field == "letter_close") return &q.letter_close;
        if (field == "verdict_prayer") return &q.verdict_prayer;
        if (field == "verdict_cursed") return &q.verdict_cursed;
        if (field == "verdict_mocked") return &q.verdict_mocked;
        if (field == "log_written") return &q.log_written;
        if (field == "log_completed_cursed") return &q.log_completed_cursed;
        if (field == "log_completed_mocked") return &q.log_completed_mocked;
        for (int k = 0; k < Quest::kDictationParts; ++k) {
            const std::string n = std::to_string(k + 1);
            if (field == "dictate_" + n) return &q.dictate[k];
            if (field == "reconsider_" + n) return &q.reconsider_row[k];
            if (field == "letter_" + n) return &q.letter_part[k][0];
            if (field == "letter_" + n + "_soft") return &q.letter_part[k][2];
            if (field == "letter_" + n + "_over") return &q.letter_part[k][3];
            if (field == "read_" + n) return &q.read_part[k][0];
            if (field == "read_" + n + "_soft") return &q.read_part[k][2];
            if (field == "read_" + n + "_over") return &q.read_part[k][3];
        }
        return nullptr;
    };

    int rc;
    while ((rc = sqlite3_step(stmt.get())) == SQLITE_ROW) {
        const std::string id = text(0), field = text(1), words = text(2);
        for (Quest& q : quests) {
            if (q.id != id) continue;
            if (field == "pay_groschen") {
                // The generator's figure, whatever it is: 0 means the giver is paying
                // money, so Henry may not buy the thing off the holder (the giver
                // would have done that himself and kept Henry out of it), and the
                // menu shows no pay option at all.
                q.pay_groschen = std::atoi(words.c_str());
            } else if (field == "count") {
                // A find's number of the thing, a mind's of visits, a return's of the goods;
                // anything under one is the one.
                q.count = std::max(1, std::atoi(words.c_str()));
            } else if (field == "min_health") {
                // A find's least freshness, on 0..1; a figure over 1 is read as a percent.
                double h = std::atof(words.c_str());
                if (h > 1.0) h /= 100.0;
                q.min_health = (h < 0.0 || h > 1.0) ? 0.0 : h;
            } else if (field == "hour_from" || field == "hour_to") {
                // A leave's window, in game hours; blank or out of range is any hour.
                const int hour = std::atoi(words.c_str());
                (field == "hour_from" ? q.hour_from : q.hour_to) =
                    (words.empty() || hour < 0 || hour > 23) ? -1 : hour;
            } else if (field == "hail_hours") {
                // The giver's day, `H-H` in game hours; anything else is any hour.
                int from = -1, to = -1;
                if (std::sscanf(words.c_str(), "%d-%d", &from, &to) == 2 && from >= 0 && from <= 23 && to >= 0 &&
                    to <= 23 && from != to) {
                    q.hail_hour_from = from;
                    q.hail_hour_to = to;
                } else if (!words.empty()) {
                    throw std::runtime_error("quest " + q.id + " hail_hours wants H-H in game hours, got '" + words + "'");
                }
            } else if (field == "routine_bed_target") {
                // The scheduler override's bed, `Class/name@x,y`: the spot is the watch's
                // anchor for the door and the street (has_bed_spot). The class is the
                // scheduler script's business; a line with no spot anchors nothing.
                double x = 0, y = 0;
                const std::size_t at = words.find('@');
                if (at != std::string::npos && std::sscanf(words.c_str() + at + 1, "%lf,%lf", &x, &y) == 2) {
                    q.bed_x = x;
                    q.bed_y = y;
                }
            } else if (field == "fast_travel_to") {
                // The carrying's landing spot, "x,y,z" (has_fast_travel).
                double x = 0, y = 0, z = 0;
                if (std::sscanf(words.c_str(), "%lf,%lf,%lf", &x, &y, &z) != 3) {
                    throw std::runtime_error("quest " + q.id + " fast_travel_to wants x,y,z, got '" + words + "'");
                }
                q.fast_travel_to = words;
            } else if (field == "fast_travel_at") {
                // The hour the clock is wound on to, "hh:mm" of the day.
                int hh = -1, mm = 0;
                if (std::sscanf(words.c_str(), "%d:%d", &hh, &mm) < 1 || hh < 0 || hh > 23 || mm < 0 || mm > 59) {
                    throw std::runtime_error("quest " + q.id + " fast_travel_at wants hh:mm, got '" + words + "'");
                }
                q.fast_travel_at = hh + mm / 60.0;
            } else if (field == "fast_travel_hours" || field == "fast_travel" || field == "fast_travel_reply" ||
                       field == "fast_travel_later" || field == "fast_travel_later_reply") {
                // The app's: the rows, the answers and the hours Henry can ask it in.
            } else if (field == "blackmail_groschen") {
                // The bedside's price for Henry's silence, over the thing itself.
                q.blackmail_groschen = std::max(0, std::atoi(words.c_str()));
            } else if (field == "welcome") {
                // The giver's house open to Henry from accept on, which every quest has
                // unless this says a plain no (read_household resolves who lives there).
                q.welcome = !(words == "0" || words == "no" || words == "false");
            } else if (field == "unyielding") {
                // A holder who cannot be moved: the menu's rows are refusals (In the Stocks).
                q.unyielding = words == "1" || words == "yes" || words == "true";
            } else if (field == "chest_lock") {
                // The chest's lock difficulty on the engine's 0..1; blank or out of range is
                // the very hard one the sacristan boasts of.
                const double d = std::atof(words.c_str());
                q.chest_lock = (words.empty() || d <= 0.0 || d > 1.0) ? 0.9 : d;
            } else if (field == "helper_door_price" || field == "helper_key_price") {
                // What the helper asks, in groschen; nothing under one is no row.
                (field == "helper_door_price" ? q.helper_door_price : q.helper_key_price) =
                    std::max(0, std::atoi(words.c_str()));
            } else if (field == "curse_parts") {
                // The parts the receiver will not carry as said, 1-based.
                std::string part;
                for (char c : words + " ") {
                    if (c >= '0' && c <= '9') {
                        part += c;
                        continue;
                    }
                    if (!part.empty()) {
                        const int n = std::atoi(part.c_str());
                        if (n >= 1 && n <= Quest::kDictationParts) q.curse_parts.push_back(n);
                    }
                    part.clear();
                }
            } else if (field == "brawl_with") {
                // A travel's brawlers, by soul_name, however the generator ran them
                // together; resolved to guids once every quest is read (read_brawlers).
                std::string name;
                for (char c : words + " ") {
                    if (c == ' ' || c == ',' || c == '\t' || c == '\n' || c == ';') {
                        if (!name.empty()) q.brawl_souls.push_back(name);
                        name.clear();
                    } else {
                        name += c;
                    }
                }
            } else if (std::string* where = slot(q, field)) {
                *where = words;
            }
        }
    }
    if (rc != SQLITE_DONE) {
        fail(db, "reading quest lines failed");
    }
}

void read_brawlers(sqlite3* db, std::vector<Quest>& quests) {
    // The brawlers are named the way the dealer names everybody, by soul_name, and the graph
    // wants soul_ids. They need not be renamed or talkable -- nobody speaks to them and no
    // marker sits on them -- but they must exist: a name the npc table has not would be a
    // fight with nobody in it, and nothing in the game would say why.
    const char* sql = "SELECT soul_id FROM npc WHERE soul_name = ?";
    sqlite3_stmt* raw_stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &raw_stmt, nullptr) != SQLITE_OK) {
        fail(db, "preparing brawler query failed");
    }
    StmtHandle stmt(raw_stmt, &sqlite3_finalize);
    for (Quest& q : quests) {
        if (q.brawl_souls.empty()) continue;
        if (!q.is_travel() && !q.is_ambush()) {
            throw std::runtime_error("quest " + q.id + " names brawl_with but is a " + q.kind +
                                     ", and only a travel or an ambush stages a brawl");
        }
        for (const std::string& name : q.brawl_souls) {
            // The one looked at is the other side of the fight, not a member of this one.
            if (name == q.target_soul) continue;
            sqlite3_reset(stmt.get());
            sqlite3_bind_text(stmt.get(), 1, name.c_str(), -1, SQLITE_TRANSIENT);
            const int rc = sqlite3_step(stmt.get());
            if (rc == SQLITE_DONE) {
                throw std::runtime_error("quest " + q.id + " brawl_with names " + name +
                                         ", which npc does not have");
            }
            if (rc != SQLITE_ROW) fail(db, "reading brawler failed");
            const unsigned char* p = sqlite3_column_text(stmt.get(), 0);
            const std::string guid = p ? reinterpret_cast<const char*>(p) : "";
            if (guid.empty()) {
                throw std::runtime_error("quest " + q.id + " brawl_with names " + name +
                                         ", who has no soul_id");
            }
            if (std::find(q.brawl_guids.begin(), q.brawl_guids.end(), guid) == q.brawl_guids.end()) {
                q.brawl_guids.push_back(guid);
            }
        }
        if (q.brawl_guids.empty()) {
            throw std::runtime_error("quest " + q.id + " brawl_with names nobody but the target");
        }
        if (q.is_travel() && q.log_seen.empty()) {
            // The brawl hangs off the arrival trigger, which is only emitted when there is
            // a journal line for the arrival; a fight the journal cannot mention is one the
            // quest has no way to start.
            throw std::runtime_error("quest " + q.id + " names brawl_with but has no log_seen");
        }
        if (q.is_ambush() && q.log_ambushed.empty()) {
            // The same for an ambush: its fight hangs off the token's trigger, emitted only
            // with the journal line for it.
            throw std::runtime_error("quest " + q.id + " names brawl_with but has no log_ambushed");
        }
    }
    for (const Quest& q : quests) {
        if (q.is_ambush() && q.brawl_guids.empty()) {
            throw std::runtime_error("quest " + q.id + " is an ambush with nobody in brawl_with");
        }
        if (q.is_ambush() && q.spot_pos.empty()) {
            throw std::runtime_error("quest " + q.id + " is an ambush with no spot_pos");
        }
        if (q.is_buy() && q.carry_class.empty()) {
            throw std::runtime_error("quest " + q.id + " is a buy with no carry_class");
        }
        if (q.is_buy() && q.pay_groschen <= 0) {
            throw std::runtime_error("quest " + q.id + " is a buy with no pay_groschen");
        }
        if (!q.keepsake_class.empty() && !q.is_find()) {
            throw std::runtime_error("quest " + q.id + " has a keepsake and is not a find");
        }
        if (!q.keepsake_class.empty() && q.spot_pos.empty()) {
            throw std::runtime_error("quest " + q.id + " has a keepsake and no spot_pos to lay it at");
        }
    }
    for (Quest& q : quests) {
        // A dictated letter: the parts are all or nothing, since the app's menu, the
        // Documents and the receiver's reading each count on every one; the curse is the
        // last part unless the lines say otherwise.
        bool any = !q.letter_open.empty() || !q.letter_close.empty();
        for (int k = 0; k < Quest::kDictationParts; ++k) any = any || !q.dictate[k].empty() || !q.letter_part[k][0].empty();
        if (any && !q.has_dictation()) {
            throw std::runtime_error("quest " + q.id + " has some of a dictation's lines but not all of "
                                     "dictate_1..3, letter_1..3, letter_open, letter_close and the three verdicts");
        }
        if (q.has_dictation() && q.curse_parts.empty()) q.curse_parts.push_back(Quest::kDictationParts);
    }
}

std::vector<std::string> household_of(sqlite3* db, const std::string& guid) {
    // Who sleeps under a soul's roof: the routine dump (npc_routine) has a sleeping slot
    // per soul with the bed's position, and a house's beds stand within a few metres of
    // one another -- Joan's eight housemates all sleep at 2593.9, 2642.6 and the farm's two
    // catchpoles five metres off in the next room. The hub guid is no use for this: it is
    // one per soul (2915 distinct of 2921). The soul first, then the rest by name, so the
    // emitted asset is stable from run to run. A dormitory is not a household: a soul among
    // forty sleepers in a lodging house has none, and the list is the soul alone.
    const char* sql =
        "SELECT DISTINCT n.soul_id FROM npc_routine r JOIN npc n ON n.soul_name = r.soul_name "
        "WHERE r.activity_family = 'sleeping' AND r.level = ("
        "  SELECT g.level FROM npc_routine g JOIN npc gn ON gn.soul_name = g.soul_name "
        "  WHERE gn.soul_id = ?1 AND g.activity_family = 'sleeping' LIMIT 1) "
        "AND EXISTS (SELECT 1 FROM npc_routine g JOIN npc gn ON gn.soul_name = g.soul_name "
        "  WHERE gn.soul_id = ?1 AND g.activity_family = 'sleeping' AND g.level = r.level "
        "  AND abs(g.pos_x - r.pos_x) <= ?2 AND abs(g.pos_y - r.pos_y) <= ?2) "
        "AND n.soul_id <> ?1 ORDER BY r.soul_name";
    sqlite3_stmt* raw_stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &raw_stmt, nullptr) != SQLITE_OK) {
        fail(db, "preparing household query failed");
    }
    StmtHandle stmt(raw_stmt, &sqlite3_finalize);
    std::vector<std::string> out;
    out.push_back(guid);
    sqlite3_bind_text(stmt.get(), 1, guid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt.get(), 2, Quest::kHouseholdMetres);
    int rc;
    while ((rc = sqlite3_step(stmt.get())) == SQLITE_ROW) {
        const unsigned char* p = sqlite3_column_text(stmt.get(), 0);
        if (p != nullptr && *p != '\0') out.emplace_back(reinterpret_cast<const char*>(p));
    }
    if (rc != SQLITE_DONE) fail(db, "reading household failed");
    if (out.size() > Quest::kHouseholdMost) out.clear();
    return out;
}

void read_household(sqlite3* db, std::vector<Quest>& quests) {
    // The giver's household, for the welcome: a giver who sleeps in a dormitory opens
    // nobody's door (household_of gives nobody, and the welcome is off).
    for (Quest& q : quests) {
        if (!q.welcome) continue;
        q.welcome_guids = household_of(db, q.giver_guid);
        if (q.welcome_guids.empty()) q.welcome = false;
    }
}

}  // namespace

std::vector<std::string> Quest::dictation_variants() const {
    // Every combination of the parts' outcomes, first part slowest, so "000" comes first
    // and the order is the same on every build (the item table and the Lua are read
    // against each other by digit string, not by position).
    std::vector<std::string> out;
    if (!has_dictation()) return out;
    out.emplace_back("");
    for (int k = 0; k < kDictationParts; ++k) {
        std::vector<std::string> next;
        for (const std::string& head : out) {
            for (int outcome : dictation_outcomes(k)) next.push_back(head + static_cast<char>('0' + outcome));
        }
        out = std::move(next);
    }
    return out;
}

std::string Quest::dictated_letter(const std::string& wrote) const {
    // The open, then each part as chosen on its own line (a part left out leaves no gap),
    // then the close after a blank line; the parts run on as one paragraph, as the plain
    // `letter` has them.
    std::string body;
    for (int k = 0; k < kDictationParts && k < static_cast<int>(wrote.size()); ++k) {
        const int outcome = wrote[k] - '0';
        if (outcome < 0 || outcome > 3 || outcome == 1) continue;
        const std::string& part = letter_part[k][outcome];
        if (part.empty()) continue;
        if (!body.empty()) body += " ";
        body += part;
    }
    std::string page = letter_open;
    if (!body.empty()) page += " " + body;
    return page + "\n\n" + letter_close;
}

std::vector<std::string> Quest::resolutions() const {
    // Only the ways this errand can actually end. A pay log with no pay option, or a
    // brokered log on a quest with no dispute, would be a port nothing ever fires and a
    // journal line nobody reads; the generator leaves those blank, and blanks are skipped
    // here either way.
    std::vector<std::string> out;
    if (is_find()) return out;   // brought is the plain completion, and the only one
    if (is_travel() || is_spot()) {
        // Told as it was is the plain completion; the lie and the silence have their own.
        // (A leave's and a return's rows at the giver's are the travel's.)
        if (!log_completed_lied.empty()) out.emplace_back("lied");
        if (!log_completed_quiet.empty()) out.emplace_back("quiet");
        return out;
    }
    if (is_deliver()) {
        // Delivered is the plain completion; the letter's other two endings have their
        // own logs, and the ports are dealt only when the generator wrote them.
        if (!log_completed_kept.empty()) out.emplace_back("kept");
        if (!log_completed_lied.empty()) out.emplace_back("lied");
        // The interrogation's five other endings, only when the receiver asks (the rows
        // are written) and the journal has the line.
        if (has_interrogation()) {
            if (!log_completed_pinned.empty()) out.emplace_back("pinned");
            if (!log_completed_covered.empty()) out.emplace_back("covered");
            if (!log_completed_fibbed.empty()) out.emplace_back("fibbed");
            if (!log_completed_evaded.empty()) out.emplace_back("evaded");
            if (!log_completed_caught.empty()) out.emplace_back("caught");
        }
        // A dictated letter's two verdicts beside the prayer taken, when the journal has them.
        if (has_dictation()) {
            if (!log_completed_cursed.empty()) out.emplace_back("cursed");
            if (!log_completed_mocked.empty()) out.emplace_back("mocked");
        }
        return out;
    }
    if (is_beat()) {
        // The amends agreed is the plain completion, with a log per way of getting there
        // as a retrieve has; the fight, the lie and the death have their own.
        if (!log_completed_persuaded.empty()) out.emplace_back("persuaded");
        if (!log_completed_threatened.empty()) out.emplace_back("threatened");
        if (!log_completed_paid.empty() && pay_groschen > 0) out.emplace_back("paid");
        if (!log_completed_fought.empty()) out.emplace_back("fought");
        if (!log_completed_lied.empty()) out.emplace_back("lied");
        if (!log_completed_dead.empty()) out.emplace_back("dead");
        return out;
    }
    if (is_pray()) {
        // Knelt and said is the plain completion; said without going has its own.
        if (!log_completed_lied.empty()) out.emplace_back("lied");
        return out;
    }
    if (is_mind()) {
        // The count done and told is the plain completion; told before the count, and the
        // target dead before it, have their own.
        if (!log_completed_lied.empty()) out.emplace_back("lied");
        if (!log_completed_dead.empty()) out.emplace_back("dead");
        return out;
    }
    if (is_ask() || is_ambush()) {
        // The truth carried back is the plain completion, however it was got out of the
        // one asked; the lie, the silence and the death have their own. (An ambush's truth
        // is the way the frightened drinker gives up.)
        if (!log_completed_lied.empty()) out.emplace_back("lied");
        if (!log_completed_quiet.empty()) out.emplace_back("quiet");
        if (!log_completed_dead.empty()) out.emplace_back("dead");
        return out;
    }
    if (is_buy()) {
        // The charm bought and brought is the plain completion; the price overstated has
        // its own, and so has the charm got off the holder some other way.
        if (!log_completed_paid.empty() && pay_groschen > 0) out.emplace_back("paid");
        if (!log_completed_stolen.empty()) out.emplace_back("stolen");
        if (!log_completed_lied.empty()) out.emplace_back("lied");
        return out;
    }
    const struct {
        const char* how;
        const std::string* log;
        bool possible;
    } ways[] = {
        {"persuaded", &log_completed_persuaded, true},
        {"threatened", &log_completed_threatened, true},
        {"paid", &log_completed_paid, pay_groschen > 0},
        {"stolen", &log_completed_stolen, true},
        {"brokered", &log_completed_brokered, dispute},
        // The lie a dispute's broker row can be, or the lie told with the thing in Henry's
        // coat where the giver wrote the row for it (lie_have).
        {"lied", &log_completed_lied, dispute || !giver_lie_have.empty()},
        {"nested", &log_completed_nested, !requires_quest_id.empty()},
        // The holder caught where he should not be, and the thing given up for silence.
        {"exposed", &log_completed_exposed, has_expose()},
        // The bedside's two more (npcdb.hpp has_bedside).
        {"exposed_paid", &log_completed_exposed_paid, has_bedside()},
        {"denounced", &log_completed_denounced, has_bedside()},
    };
    for (const auto& way : ways) {
        if (way.possible && !way.log->empty()) out.emplace_back(way.how);
    }
    return out;
}

}  // namespace kcd2
