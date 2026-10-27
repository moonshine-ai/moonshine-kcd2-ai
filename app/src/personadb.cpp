#include "personadb.h"
#include "sentence.h"

#include "voice.h"

#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <utility>

namespace moonshine {

namespace {

// Kin worth mentioning. A soul in a crowded town has dozens of listed acquaintances
// and every one of them costs prefill on the opening line, measured at about 15ms a
// token on a 4B. Close relations first, then however many acquaintances fit.
constexpr std::size_t kMostKin = 6;

// Relationships that say something about a life, in the order they are worth hearing.
// Anything not listed here is kept but ranked after them.
int closeness(std::string_view relationship) {
    static constexpr std::string_view kOrder[] = {
        "wife", "husband", "mother", "father", "son", "daughter",
        "brother", "sister", "master", "apprentice", "servant", "neighbour",
    };
    const auto* found = std::find(std::begin(kOrder), std::end(kOrder), relationship);
    return found == std::end(kOrder) ? static_cast<int>(std::size(kOrder))
                                     : static_cast<int>(found - std::begin(kOrder));
}

std::string plural(const std::string& relationship) {
    if (relationship == "wife") return "wives";
    return relationship + "s";
}

std::string text_at(sqlite3_stmt* row, int column) {
    const unsigned char* value = sqlite3_column_text(row, column);
    // Every text column comes in undashed (sentence.h), so nothing read from the DB
    // reaches the screen, the voice or the model with a dash in it.
    return value ? undashed(reinterpret_cast<const char*>(value)) : std::string();
}

void bind(sqlite3_stmt* statement, std::string_view value) {
    sqlite3_bind_text(statement, 1, value.data(), static_cast<int>(value.size()),
                      SQLITE_TRANSIENT);
}

// A sentence listing things, with the commas and the "and" in the right places.
std::string listed(const std::vector<std::string>& parts) {
    std::string out;
    for (std::size_t at = 0; at < parts.size(); ++at) {
        if (at != 0) out += (at + 1 == parts.size()) ? " and " : ", ";
        out += parts[at];
    }
    return out;
}

}  // namespace

Persona stranger() {
    Persona persona;
    persona.name = "Stranger";
    // Deliberately thin. Someone the database cannot describe should come across as
    // having little to say about themselves, rather than having a personality invented
    // for them that contradicts whoever they turn out to be.
    persona.sheet =
        "You are a commoner going about your day. You do not know the person speaking to "
        "you, and have little to say about yourself.";
    // The one block there is, labelled like any other, so that a caller showing where the
    // sheet came from has something to show rather than an empty list for the one soul whose
    // sheet is not built from anything.
    persona.parts.push_back({"who", persona.sheet});
    return persona;
}

// Written in the third person and using the name rather than a pronoun, which is not a
// style choice.
//
// The generated bios are third person, and so are the hooks -- 3,665 of them describe
// the character with a verb ("Bartel works the far strip as though it were already
// theirs"), so turning them into second person would mean conjugating the verb, and
// getting that wrong reads worse than not trying. Naming the character instead of
// saying "he" also sidesteps agreement with "they", which the souls of undeclared
// gender would otherwise need. The shared prefix is what tells the model that this
// description is of itself.
bool away_from_places(const Soul& soul, const Approach& approach) {
    if (approach.x == kUnread || approach.y == kUnread || soul.spots.empty()) return false;
    for (const auto& [x, y] : soul.spots) {
        const double dx = approach.x - x;
        const double dy = approach.y - y;
        if (dx * dx + dy * dy <= kAwayMetres * kAwayMetres) return false;
    }
    return true;
}

Persona persona_for(const Soul& soul, Approach approach, const Doing& doing,
                    bool say_written, QuestFacts facts,
                    const std::string& memory, const Standing& standing) {
    // The activity is the one part of the state the mod cannot report, because turning
    // "LumberJackSawVAR" into "working" takes the database.
    if (approach.activity.empty()) approach.activity = doing.activity;

    Persona persona;
    // The quest, decided once for the whole conversation. A soul on several favours has the
    // save's facts for each read off the standing, and the one the conversation is about
    // picked from them (pick_quest); the `facts` given are the first favour's, for the
    // callers that know the soul by one, and win over the standing for it. The count comes
    // off the approach because the mod reports it on the same line as everything else it
    // read: `have=` for the first favour, `holding=` for every one.
    std::vector<Quest> quests = soul.quests;
    if (quests.empty() && soul.quest.exists()) quests.push_back(soul.quest);
    Quest chosen;
    std::vector<QuestFacts> all;
    if (!quests.empty()) {
        Standing known = standing;
        for (const auto& [id, count] : approach.holding) known.holding[id] = count;
        for (std::size_t i = 0; i < quests.size(); ++i) {
            const Quest& q = quests[i];
            QuestFacts f = known.facts_of(q.id, i == 0 ? approach.have : -1);
            if (i == 0) {
                // The given facts, where the caller set them; the count from the line when
                // they did not.
                if (facts.accepted || facts.handed || facts.done) {
                    f.accepted = facts.accepted;
                    f.handed = facts.handed;
                    f.done = facts.done;
                }
                if (facts.have >= 0) f.have = facts.have;
                if (facts.broker) f.broker = true;
                if (facts.apologised) f.apologised = true;
                if (facts.refused) f.refused = true;
                if (facts.seen) f.seen = true;
                if (facts.beaten) f.beaten = true;
                if (facts.dead) f.dead = true;
                if (facts.prayed) f.prayed = true;
                if (facts.left) f.left = true;
                if (facts.heard) f.heard = true;
                if (facts.looted) f.looted = true;
                if (facts.cursed) f.cursed = true;
                if (facts.directed) f.directed = true;
                if (facts.ambushed) f.ambushed = true;
                if (facts.onward) f.onward = true;
                if (facts.keepsake) f.keepsake = true;
                if (facts.rebuffed) f.rebuffed = true;
                if (facts.named) f.named = true;
                if (facts.letin) f.letin = true;
                if (facts.keyed) f.keyed = true;
                if (facts.denounced) f.denounced = true;
                if (facts.checks > f.checks) f.checks = facts.checks;
                if (facts.cannot_pray) f.cannot_pray = true;
                if (!facts.how.empty()) f.how = facts.how;
                if (!facts.wrote.empty()) f.wrote = facts.wrote;
            }
            // A pray is not asked of a player the game will not let pray (the mod reads the
            // perk and says pray=0 on the line); unread, nothing is withheld.
            if (q.is_pray() && approach.can_pray == 0) f.cannot_pray = true;
            all.push_back(f);
        }
        // A chain's later link waits on the one before it, and a story's later scene on the
        // scene before and how it ended. The one before is read from the save (the dump's
        // quests= list, with its /how), or from this soul's own list when it is theirs too --
        // with the caller's word on it -- which is why this is a second pass.
        for (std::size_t i = 0; i < quests.size(); ++i) {
            const Quest& q = quests[i];
            if (q.after_quest_id.empty()) continue;
            bool before_done = known.is_done(q.after_quest_id);
            std::string before_how = known.facts_of(q.after_quest_id).how;
            for (std::size_t j = 0; j < all.size(); ++j) {
                if (quests[j].id != q.after_quest_id) continue;
                before_done = all[j].done;
                before_how = all[j].how;
            }
            all[i].locked = !before_done;
            all[i].voided = before_done && !how_matches(before_how, q.after_how);
        }
        const std::size_t at = pick_quest(quests, all);
        chosen = quests[at];
        facts = all[at];
        persona.quest = plan_quest(chosen, facts, approach, standing.met);
        persona.quest_of = chosen;
        // Paid off on the greeting -- the giver sees the thing in the player's hands and the
        // director completes it before a word is said -- so by the time a row is picked the
        // errand is done, and the menu's answers below are planned as for one that is: the
        // topic and the remarks about the thing dropped, the reminder gone, help answered as
        // after. Planned on the save's accepted-and-not-done, Joan offered "Tell me about the
        // meadow flowers" after taking her marigolds and wanted them again (notes.md, 8 Oct
        // 2026). The save itself is told by the command; this is the conversation knowing it.
        if (std::find(persona.quest.opening_commands.begin(), persona.quest.opening_commands.end(),
                      std::string(kQuestComplete)) != persona.quest.opening_commands.end()) {
            facts.done = true;
            all[at].done = true;
        }
        // The others, in a sentence each, after the paragraph, so the soul is not made to
        // forget them.
        for (std::size_t i = 0; i < quests.size(); ++i) {
            if (i == at) continue;
            // A chain's link before the chosen one is already in its paragraph; a scene's is
            // not, and gets its sentence like any other.
            if (!chosen.chain_id.empty() && quests[i].id == chosen.after_quest_id) continue;
            const std::string aside = quest_aside(quests[i], all[i], quests, all);
            if (!aside.empty()) persona.quest.block += (persona.quest.block.empty() ? "" : " ") + aside;
        }
    }
    // And the menu's answers, on the same facts, so the offer said to `help` and the
    // paragraph that tells the model the favour is open cannot disagree. A remark on the
    // place they stand goes in only where they stand: met away from every spot of their
    // routine -- on the open road, say -- "this cellar" has no cellar to point at (noted
    // 4 Oct 2026), so those rows are kept back and the rest offered as ever.
    std::vector<Remark> remarks = soul.remarks;
    if (away_from_places(soul, approach)) {
        remarks.erase(std::remove_if(remarks.begin(), remarks.end(),
                                     [](const Remark& r) { return r.here; }),
                      remarks.end());
    }
    // And a remark on what they are doing only while they are seen doing it (Benesh Bell
    // asked why he prays on the square, notes.md 9 Oct 2026: odd to ask a man at his supper).
    remarks.erase(std::remove_if(remarks.begin(), remarks.end(),
                                 [&](const Remark& r) { return !r.doing.empty() && r.doing != approach.activity; }),
                  remarks.end());
    persona.scripted = plan_scripted(soul.self_account, soul.knowledge, soul.help_replies,
                                     chosen, facts, standing, quests, all, remarks,
                                     soul.about_again, soul.people);
    // The name is for the subtitle and the log. An unnamed soul is a stranger to the
    // player even though the database knows their trade.
    persona.name = soul.name.empty() ? std::string("Stranger") : soul.name;
    // The stand-in recording, from their gender. The actor's own clone is looked up and
    // not used: copying it waits on the rights. Kept off the sheet, the same as a name
    // placed before the world text would break the prefix.
    persona.actor = stand_in_voice(soul.gender);
    persona.face = actor_abbr(soul.voice_id);
    const std::string& who = persona.name;

    // Second person, and the name first, because this sheet is the standing instruction to
    // whatever is voicing them. Telling a model "Bartel is a varlet" and separately that it
    // is Bartel leaves it a step to make, and the step it sometimes makes instead is to
    // narrate: "Bartel grumbles about his shoulders" rather than grumbling about them.
    std::string sheet;
    // Every block goes through here so that the sheet and the labelled account of it cannot
    // drift apart: there is one place that knows a block is separated from the last by a blank
    // line, and one place that knows what the block is called.
    auto add = [&](std::string_view label, const std::string& text) {
        if (text.empty()) return;
        if (!sheet.empty()) sheet += "\n\n";
        sheet += text;
        persona.parts.push_back({std::string(label), text});
    };

    // The trade as it is said, never the game's label: a "varlet" is a miner or a groom here.
    const std::string& work = soul.trade.empty() ? soul.occupation : soul.trade;
    std::string who_line = "You are " + who + ", ";
    who_line += work.empty() ? "a commoner" : "a " + work;
    if (!soul.settlement.empty()) who_line += " in " + soul.settlement;
    who_line += ".";
    add("who", who_line);

    // What they say first: the line for somebody they have met, or the stranger's line,
    // which also stands in for a returning player when only the one was written -- a
    // stranger's greeting is a smaller lapse than seven seconds of silence.
    //
    // Left empty when written greetings are switched off, which is the same state as a soul
    // nobody has written for, and so takes the path that already exists and is already tested
    // rather than a second one added for the occasion.
    if (say_written) {
        persona.greeting = approach.met && !soul.greeting_again.empty() ? soul.greeting_again
                                                                       : soul.greeting_first;
        // Unless the errand makes this visit one of its own and the writer gave the giver a
        // line for it: the man who sees his axe in the player's hands does not say "How can
        // I serve, sir?", and the one owed a favour asks after it. Written for exactly this
        // visit, so nothing is gained by guessing a fresher one.
        if (!persona.quest.greeting.empty()) {
            persona.greeting = persona.quest.greeting;
            persona.greeting_exact = true;
        }
    }

    // Last in the sheet, because everything above it is the same text for this soul every
    // time and so is worth prefilling, while this changes with every conversation. The
    // greeting knows only whether they have met; the prose carries the whole state.
    const std::string caught = approach_prose(approach, doing.clause);
    persona.doing = doing.clause;

    // The card in preference to the bio. Both describe the same person, but the card was
    // written to be this prompt and the bio was written to be read: four hundred tokens
    // against forty, on text that is prefilled again for every soul the player meets.
    // The kin still earn their place, being cheap and the names the model needs to hand.
    if (!soul.card.empty()) {
        add("card", soul.card);
    } else if (!soul.bio.empty()) {
        // The line above is only there to fix the name and trade that the bio assumes
        // its reader already knows.
        add("bio", soul.bio);
        add("memory", memory);
        add("quest", persona.quest.block);
        add("approach", caught);
        persona.sheet = std::move(sheet);
        return persona;
    }

    if (!soul.kin.empty()) {
        std::vector<Kin> kin = soul.kin;
        std::stable_sort(kin.begin(), kin.end(), [](const Kin& a, const Kin& b) {
            return closeness(a.relationship) < closeness(b.relationship);
        });
        if (kin.size() > kMostKin) kin.resize(kMostKin);

        // Close relations named for what they are, the rest merely known, and grouped
        // so that three friends are "his friends" once rather than the word three
        // times. Reads like a life rather than a form, and costs fewer tokens for it.
        std::vector<std::pair<std::string, std::vector<std::string>>> groups;
        std::vector<std::string> known;
        for (const Kin& one : kin) {
            if (one.relationship == "acquaintance" || one.relationship.empty()) {
                known.push_back(one.name);
                continue;
            }
            const auto found = std::find_if(groups.begin(), groups.end(), [&](const auto& group) {
                return group.first == one.relationship;
            });
            if (found == groups.end()) {
                groups.push_back({one.relationship, {one.name}});
            } else {
                found->second.push_back(one.name);
            }
        }

        std::string kin_line;
        // One sentence: the first clause carries the verb and the rest lean on it, the
        // way anybody would say it aloud. Only that first clause starts a sentence, so
        // only it takes the capital.
        std::vector<std::string> clauses;
        clauses.reserve(groups.size());
        for (const auto& [relationship, names] : groups) {
            const bool many = names.size() > 1;
            // "Your" rather than his or hers, which also means the sheet no longer has to
            // know the soul's sex to say who their brother is.
            std::string clause = std::string(clauses.empty() ? "Your " : "your ") +
                                 (many ? plural(relationship) : relationship);
            if (clauses.empty()) clause += many ? " are" : " is";
            clauses.push_back(clause + " " + listed(names));
        }
        if (!clauses.empty()) kin_line += listed(clauses) + ".";
        if (!known.empty()) {
            if (!clauses.empty()) kin_line += " ";
            kin_line += clauses.empty() ? "You know " : "You also know ";
            kin_line += listed(known) + ".";
        }
        add("kin", kin_line);
    }

    // The card already says what is on their mind, in fewer words and better ones, so
    // the hook is only spelled out when there is no card to have said it.
    if (!soul.trouble.empty() && soul.card.empty()) {
        std::string hook_line = soul.trouble;
        // Both phrasings the generator produces read correctly after "wants": "wants to
        // have the strip settled" and "wants somewhere dry to sleep".
        if (!soul.wants.empty()) hook_line += " " + who + " wants " + soul.wants;
        add("hook", hook_line);
    }

    // What they remember of the player, after who they are and before what is wanted of
    // them, so the quest paragraph reads against what has already been said about it. It
    // changes with every conversation, like the quest, so it belongs below everything that
    // does not; the earlier conversations are the bulk of it and are only there when there
    // have been any.
    add("memory", memory);

    // Where the favour stands, after the card that already grumbles about it. Before the
    // approach because it changes between conversations rather than within one, and less
    // often than the weather does.
    add("quest", persona.quest.block);

    add("approach", caught);
    persona.sheet = std::move(sheet);
    return persona;
}

PersonaDb::PersonaDb(std::filesystem::path path) : path_(std::move(path)) {
    // Read-only, and with no attempt to create it: a missing database is a thing to
    // report, not a thing to make an empty one of and then look puzzled about.
    const int opened = sqlite3_open_v2(path_.string().c_str(), &db_, SQLITE_OPEN_READONLY,
                                       nullptr);
    if (opened != SQLITE_OK) {
        trouble_ = db_ ? sqlite3_errmsg(db_) : "cannot open";
        sqlite3_close(db_);
        db_ = nullptr;
        return;
    }
    learn_places();
}

// Whether what was written for this soul passed its own checks. A row with no verdict at
// all counts as unsound: the generator always records one, so its absence means the
// tables were filled by something that did not check, and unchecked is not the same as
// clean. Four of the first hundred were flagged, for a spouse the data does not give them
// or an unrelated NPC named.
//
// A soul the circle pipeline wrote has its verdict on npc_person instead, and that checker
// grades: an issue marked "(soft)" -- a purple word, a repeated greeting -- is a note to the
// writer, not a reason to withhold the card, where a claimed son or an invented brother is.
// The pipeline writes the card and greetings through to the old tables, so those are the
// rows this decides about; the bio table does not know these souls at all (55 of the pilot's
// 59 have no row there), which under the old rule alone left them faceless.
bool PersonaDb::written_is_sound(std::string_view soul) const {
    sqlite3_stmt* row = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT COALESCE(issues, '') FROM npc_person WHERE soul_name = ?",
                           -1, &row, nullptr) == SQLITE_OK) {
        bind(row, soul);
        if (sqlite3_step(row) == SQLITE_ROW) {
            const std::string issues = text_at(row, 0);
            sqlite3_finalize(row);
            // Every issue, split on the semicolons the checker joins them with, must be soft.
            std::size_t from = 0;
            while (from < issues.size()) {
                std::size_t to = issues.find(';', from);
                if (to == std::string::npos) to = issues.size();
                std::string_view one(issues.data() + from, to - from);
                while (!one.empty() && std::isspace(static_cast<unsigned char>(one.back()))) {
                    one.remove_suffix(1);
                }
                while (!one.empty() && std::isspace(static_cast<unsigned char>(one.front()))) {
                    one.remove_prefix(1);
                }
                constexpr std::string_view kSoft = "(soft)";
                if (!one.empty() &&
                    (one.size() < kSoft.size() || one.substr(one.size() - kSoft.size()) != kSoft)) {
                    return false;
                }
                from = to + 1;
            }
            return true;
        }
        sqlite3_finalize(row);
    }
    if (sqlite3_prepare_v2(db_, "SELECT COALESCE(issues, '') FROM npc_bio WHERE soul_name = ?",
                           -1, &row, nullptr) != SQLITE_OK) {
        return false;
    }
    bind(row, soul);
    const bool sound = sqlite3_step(row) == SQLITE_ROW && text_at(row, 0).empty();
    sqlite3_finalize(row);
    return sound;
}

// The clause for one animation name. An exact lookup on the name the engine returned,
// deliberately: the phases of one task -- LumberJackSaw, ...In, ...Out, ...VAR -- could
// have been collapsed to a stem and a third of the rows saved, but only by putting a
// stemming rule here that the generator had to match byte for byte, and a disagreement
// between the two would surface as a clause that silently went missing.
//
// A flagged row is withheld along with its activity, on the same grounds as a flagged
// bio: half a verdict is not a pass.
Doing PersonaDb::doing(std::string_view anim) const {
    // "none" is what the mod logs when it watched and saw nothing worth keeping, and the
    // empty string is what a mod too old to report it leaves. Neither is worth a query.
    if (!db_ || anim.empty() || anim == "none") return {};

    sqlite3_stmt* row = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT clause, activity FROM anim_activity "
                                "WHERE fragment = ? AND issues IS NULL",
                           -1, &row, nullptr) != SQLITE_OK) {
        // A database built before the table existed. No clause, and no complaint: the
        // conversation is the thing that matters and it does not need one.
        return {};
    }
    bind(row, anim);
    Doing got;
    if (sqlite3_step(row) == SQLITE_ROW) {
        got.clause = text_at(row, 0);
        got.activity = text_at(row, 1);
    }
    sqlite3_finalize(row);
    return got;
}

void PersonaDb::learn_places() {
    sqlite3_stmt* row = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT DISTINCT settlement FROM social_place "
                                "WHERE COALESCE(settlement, '') <> ''",
                           -1, &row, nullptr) != SQLITE_OK) {
        return;
    }
    while (sqlite3_step(row) == SQLITE_ROW) places_.insert(text_at(row, 0));
    sqlite3_finalize(row);
}

PersonaDb::~PersonaDb() {
    if (db_) sqlite3_close(db_);
}

std::string PersonaDb::quest_set() const {
    if (!db_) return {};
    sqlite3_stmt* row = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT quest_id FROM npc_quest ORDER BY quest_id", -1, &row,
                           nullptr) != SQLITE_OK) {
        return {};
    }
    // The same bytes the emitter hashes: the ids in byte order, newline between.
    std::string joined;
    bool first = true;
    while (sqlite3_step(row) == SQLITE_ROW) {
        if (!first) joined += '\n';
        first = false;
        joined += text_at(row, 0);
    }
    sqlite3_finalize(row);
    std::uint64_t hash = 14695981039346656037ULL;
    for (unsigned char c : joined) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    char hex[17];
    std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(hash));
    return hex;
}

int PersonaDb::named_souls() const {
    if (!db_) return 0;
    sqlite3_stmt* row = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM generated_name", -1, &row, nullptr) !=
        SQLITE_OK) {
        return 0;
    }
    const int count = sqlite3_step(row) == SQLITE_ROW ? sqlite3_column_int(row, 0) : 0;
    sqlite3_finalize(row);
    return count;
}

std::vector<QuestListing> PersonaDb::quest_list() const {
    std::vector<QuestListing> out;
    if (!db_) return out;
    const bool slate = sqlite3_table_column_metadata(db_, nullptr, "npc_slate", "chain_id", nullptr,
                                                     nullptr, nullptr, nullptr, nullptr) == SQLITE_OK;
    const bool scenes = slate && sqlite3_table_column_metadata(db_, nullptr, "npc_slate", "after_quest_id",
                                                               nullptr, nullptr, nullptr, nullptr,
                                                               nullptr) == SQLITE_OK;
    // The trades off npc.occupation through trade_of, as the sheet has them, rather than off
    // npc_quest's own columns, which a database from before the trade words lacks.
    std::string sql =
        "SELECT q.quest_id, q.kind, COALESCE(t.name, ''), q.giver_soul, q.giver_name, "
        "       COALESCE(n.occupation, ''), COALESCE(n.settlement, ''), q.target_soul, q.target_name, "
        "       COALESCE(m.occupation, ''), q.item_name, ";
    if (slate) {
        const std::string chain_before =
            "(SELECT p.quest_id FROM npc_slate p WHERE p.chain_id = s.chain_id "
            " AND p.chain_order = s.chain_order - 1)";
        sql += scenes ? "COALESCE(NULLIF(s.after_quest_id, ''), " + chain_before + ", '') "
                      : "COALESCE(" + chain_before + ", '') ";
        sql += "FROM npc_quest q LEFT JOIN npc_quest_text t USING (quest_id) "
               "LEFT JOIN npc_slate s USING (quest_id) ";
    } else {
        sql += "'' FROM npc_quest q LEFT JOIN npc_quest_text t USING (quest_id) ";
    }
    sql += "LEFT JOIN npc n ON n.soul_name = q.giver_soul "
           "LEFT JOIN npc m ON m.soul_name = q.target_soul ORDER BY q.rowid";
    sqlite3_stmt* row = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &row, nullptr) != SQLITE_OK) return out;
    while (sqlite3_step(row) == SQLITE_ROW) {
        QuestListing q;
        q.id = text_at(row, 0);
        q.kind = text_at(row, 1);
        q.title = text_at(row, 2);
        q.giver_soul = text_at(row, 3);
        q.giver_name = text_at(row, 4);
        q.giver_trade = trade_of(q.giver_soul, text_at(row, 5));
        // A quest group or an encounter group is not a place (see places_).
        const std::string settlement = text_at(row, 6);
        if (places_.count(settlement)) q.giver_settlement = settlement;
        q.target_soul = text_at(row, 7);
        q.target_name = text_at(row, 8);
        q.target_trade = trade_of(q.target_soul, text_at(row, 9));
        q.item_name = text_at(row, 10);
        q.after_id = text_at(row, 11);
        out.push_back(std::move(q));
    }
    sqlite3_finalize(row);

    // The first scene of each one's story, found by walking the links back; a link to a
    // quest that is not dealt (or a loop, which the lint forbids) stops the walk there.
    std::map<std::string, std::size_t> by_id;
    for (std::size_t i = 0; i < out.size(); ++i) by_id[out[i].id] = i;
    // Each quest's place in the list as a key worked out before the sort, since the sort
    // moves the entries a pointer into the list would be following: the first scene's
    // settlement (the ones with none last), giver, title and id, then the depth and the id.
    struct Key {
        bool unplaced;
        std::string settlement, giver, title, root, id;
        int depth;
    };
    std::vector<std::pair<Key, QuestListing>> keyed;
    keyed.reserve(out.size());
    for (QuestListing& q : out) {
        std::size_t at = by_id.at(q.id);
        int depth = 0;
        while (!out[at].after_id.empty() && depth < 16) {
            const auto found = by_id.find(out[at].after_id);
            if (found == by_id.end()) break;
            at = found->second;
            ++depth;
        }
        q.depth = depth;
        const QuestListing& root = out[at];
        keyed.push_back({{root.giver_settlement.empty(), root.giver_settlement, root.giver_name, root.title,
                          root.id, q.id, depth},
                         q});
    }
    std::stable_sort(keyed.begin(), keyed.end(), [](const auto& a, const auto& b) {
        const Key& ka = a.first;
        const Key& kb = b.first;
        if (ka.unplaced != kb.unplaced) return !ka.unplaced;
        if (ka.settlement != kb.settlement) return ka.settlement < kb.settlement;
        if (ka.giver != kb.giver) return ka.giver < kb.giver;
        if (ka.title != kb.title) return ka.title < kb.title;
        if (ka.root != kb.root) return ka.root < kb.root;
        if (ka.depth != kb.depth) return ka.depth < kb.depth;
        return ka.id < kb.id;
    });
    out.clear();
    for (auto& [key, q] : keyed) out.push_back(std::move(q));
    return out;
}

std::vector<RoutineSpot> PersonaDb::spots_of(std::string_view soul) const {
    std::vector<RoutineSpot> out;
    if (!db_) return out;
    // The default routine's spots when the soul has one, every spot otherwise; one entry
    // per distinct place and window, the day's in order and the windowless (the bed) last.
    // A routine dumped before the windows were (no start_min) gives its spots windowless.
    const bool windows = sqlite3_table_column_metadata(db_, nullptr, "npc_routine", "start_min", nullptr,
                                                       nullptr, nullptr, nullptr, nullptr) == SQLITE_OK &&
                         sqlite3_table_column_metadata(db_, nullptr, "npc_routine", "is_default", nullptr,
                                                       nullptr, nullptr, nullptr, nullptr) == SQLITE_OK;
    static constexpr char kSpots[] =
        "SELECT DISTINCT COALESCE(start_min, 0), COALESCE(end_min, 0), pos_x, pos_y, COALESCE(pos_z, 0) "
        "FROM npc_routine WHERE soul_name = ?1 AND pos_x IS NOT NULL AND pos_y IS NOT NULL "
        "AND (is_default = 1 OR NOT EXISTS (SELECT 1 FROM npc_routine d WHERE d.soul_name = ?1 "
        "                                   AND d.is_default = 1 AND d.pos_x IS NOT NULL)) "
        "ORDER BY CASE WHEN COALESCE(start_min, 0) = COALESCE(end_min, 0) THEN 1 ELSE 0 END, start_min";
    static constexpr char kBareSpots[] =
        "SELECT DISTINCT 0, 0, pos_x, pos_y, COALESCE(pos_z, 0) FROM npc_routine "
        "WHERE soul_name = ?1 AND pos_x IS NOT NULL AND pos_y IS NOT NULL ORDER BY rowid";
    sqlite3_stmt* row = nullptr;
    if (sqlite3_prepare_v2(db_, windows ? kSpots : kBareSpots, -1, &row, nullptr) != SQLITE_OK) return out;
    bind(row, soul);
    while (sqlite3_step(row) == SQLITE_ROW) {
        RoutineSpot spot;
        spot.start_min = sqlite3_column_int(row, 0);
        spot.end_min = sqlite3_column_int(row, 1);
        spot.x = sqlite3_column_double(row, 2);
        spot.y = sqlite3_column_double(row, 3);
        spot.z = sqlite3_column_double(row, 4);
        out.push_back(spot);
    }
    sqlite3_finalize(row);
    return out;
}

// The game's occupation labels that are not what anyone calls a person, and the word for
// each when npc_trade has nothing better. npc_trade (scripts/trade_words.py) knows a varlet's
// actual work from his routine -- the mine, the stable, the mill -- and is preferred; this is
// the floor under a database the sweep has not been run on, so that "You are Bartel, a
// varlet" cannot reach a prompt from here whatever the data.
static const char* said_for_label(const std::string& label) {
    static const std::pair<const char*, const char*> kSaid[] = {
        {"varlet", "hired hand"},
        {"wench", "maidservant"},
        {"security", "guard"},
        {"soldier crime authority", "soldier"},
        {"blacksmith apprentice", "apprentice"},
        {"gypsy", "nomad of the camp"},
    };
    for (const auto& [game, said] : kSaid)
        if (label == game) return said;
    return nullptr;
}

std::string PersonaDb::trade_of(std::string_view soul, const std::string& occupation) const {
    std::string label = occupation;
    for (char& c : label) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    // The table is newer than most of the schema, so its absence is a database from before
    // it, not a fault: the prepare fails and the label's fallback stands.
    sqlite3_stmt* row = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT trade FROM npc_trade WHERE soul_name = ?", -1, &row,
                           nullptr) == SQLITE_OK) {
        bind(row, soul);
        std::string trade;
        if (sqlite3_step(row) == SQLITE_ROW) trade = text_at(row, 0);
        sqlite3_finalize(row);
        if (!trade.empty()) return trade;
    }
    if (const char* said = said_for_label(label)) return said;
    return occupation;
}

std::optional<Soul> PersonaDb::find(std::string_view soul) const {
    if (!db_ || soul.empty()) return std::nullopt;

    Soul found;
    // generated_name joins on soul_id while the game only knows soul_name, so the id
    // is the hop between them. LEFT JOIN because a soul the generator did not rename
    // still exists and can still be talked to.
    static constexpr char kFacts[] =
        "SELECT n.soul_name, g.full_name, n.occupation, n.social_class, n.settlement, "
        "       n.region, n.gender "
        "FROM npc n LEFT JOIN generated_name g USING (soul_id) "
        "WHERE n.soul_name = ?";
    {
        sqlite3_stmt* row = nullptr;
        if (sqlite3_prepare_v2(db_, kFacts, -1, &row, nullptr) != SQLITE_OK) return std::nullopt;
        bind(row, soul);
        const bool got = sqlite3_step(row) == SQLITE_ROW;
        if (got) {
            found.soul = text_at(row, 0);
            found.name = text_at(row, 1);
            found.occupation = text_at(row, 2);
            found.social_class = text_at(row, 3);
            found.settlement = text_at(row, 4);
            found.region = text_at(row, 5);
            found.gender = text_at(row, 6);
        }
        sqlite3_finalize(row);
        if (!got) return std::nullopt;
    }
    // Optional, and a failure is the fixture databases, which have no skald_character
    // and no skald_character_name column. Folding it into kFacts would make that prepare
    // fail and find() return nothing.
    {
        static constexpr char kVoice[] =
            "SELECT s.voice_id FROM npc n "
            "LEFT JOIN skald_character s ON s.skald_character_name = n.skald_character_name "
            "WHERE n.soul_name = ? LIMIT 1";
        sqlite3_stmt* row = nullptr;
        if (sqlite3_prepare_v2(db_, kVoice, -1, &row, nullptr) == SQLITE_OK) {
            bind(row, soul);
            if (sqlite3_step(row) == SQLITE_ROW && sqlite3_column_type(row, 0) != SQLITE_NULL) {
                found.voice_id = sqlite3_column_int(row, 0);
            }
            sqlite3_finalize(row);
        }
    }
    found.trade = trade_of(soul, found.occupation);

    if (!found.settlement.empty() && places_.find(found.settlement) == places_.end()) {
        // Not a town. Where the soul is placed socially is the better answer, and saying
        // nothing is better than either if there is no such placing.
        found.settlement.clear();
        static constexpr char kWhere[] =
            "SELECT p.settlement FROM social_membership m JOIN social_place p USING (place_id) "
            "WHERE m.soul_name = ? AND COALESCE(p.settlement, '') <> '' LIMIT 1";
        sqlite3_stmt* row = nullptr;
        if (sqlite3_prepare_v2(db_, kWhere, -1, &row, nullptr) == SQLITE_OK) {
            bind(row, soul);
            if (sqlite3_step(row) == SQLITE_ROW) found.settlement = text_at(row, 0);
            sqlite3_finalize(row);
        }
    }

    // What was written for this soul: the bio and the card come out of one generation, and
    // npc_bio.issues is that generation's verdict. So they are taken or left together -- a
    // flagged run's card is as likely to claim the invented wife as its bio is. Most souls
    // have none of it yet, which is ordinary rather than a fault.
    if (written_is_sound(soul)) {
        sqlite3_stmt* row = nullptr;
        if (sqlite3_prepare_v2(db_, "SELECT bio FROM npc_bio WHERE soul_name = ?", -1, &row,
                               nullptr) == SQLITE_OK) {
            bind(row, soul);
            if (sqlite3_step(row) == SQLITE_ROW) found.bio = text_at(row, 0);
            sqlite3_finalize(row);
        }
        if (sqlite3_prepare_v2(db_, "SELECT card FROM npc_card WHERE soul_name = ?", -1, &row,
                               nullptr) == SQLITE_OK) {
            bind(row, soul);
            if (sqlite3_step(row) == SQLITE_ROW) found.card = text_at(row, 0);
            sqlite3_finalize(row);
        }
    }

    // The greetings are read whatever the verdict on the rest: they come from a pass of
    // their own over the whole cast, written from the dossier and not from the bio, so a
    // flawed bio says nothing about them.
    {
        sqlite3_stmt* row = nullptr;
        if (sqlite3_prepare_v2(db_, "SELECT met, words FROM npc_default_greeting WHERE soul_name = ?",
                               -1, &row, nullptr) == SQLITE_OK) {
            bind(row, soul);
            while (sqlite3_step(row) == SQLITE_ROW) {
                (sqlite3_column_int(row, 0) ? found.greeting_again : found.greeting_first) =
                    text_at(row, 1);
            }
            sqlite3_finalize(row);
        }
    }

    // Only kin who were themselves given a name: "your brother kkut_man_88" is worse
    // than not mentioning him.
    {
        static constexpr char kKin[] =
            "SELECT r.relationship, g.full_name "
            "FROM npc_relationship r "
            "JOIN npc o ON o.soul_name = r.other_soul_name "
            "JOIN generated_name g ON g.soul_id = o.soul_id "
            "WHERE r.soul_name = ? AND g.full_name <> ''";
        sqlite3_stmt* row = nullptr;
        if (sqlite3_prepare_v2(db_, kKin, -1, &row, nullptr) == SQLITE_OK) {
            bind(row, soul);
            while (sqlite3_step(row) == SQLITE_ROW) {
                found.kin.push_back({text_at(row, 0), text_at(row, 1)});
            }
            sqlite3_finalize(row);
        }
    }

    {
        // premise is the situation, problem is what it looks like day to day. The
        // premise is the one worth having -- it names the other party and what is
        // unsettled -- and for many hooks the two are the same sentence, so the problem
        // is only added when it says something the premise did not.
        static constexpr char kHook[] =
            "SELECT premise, problem, desire FROM npc_hook WHERE soul_name = ? LIMIT 1";
        sqlite3_stmt* row = nullptr;
        if (sqlite3_prepare_v2(db_, kHook, -1, &row, nullptr) == SQLITE_OK) {
            bind(row, soul);
            if (sqlite3_step(row) == SQLITE_ROW) {
                const std::string premise = text_at(row, 0);
                const std::string problem = text_at(row, 1);
                found.trouble = premise.empty() ? problem : premise;
                if (!premise.empty() && !problem.empty() && problem != premise) {
                    found.trouble += " " + problem;
                }
                found.wants = text_at(row, 2);
            }
            sqlite3_finalize(row);
        }
    }

    {
        // The quests this soul is one end of, from either end. LEFT JOIN on the text because
        // the deal is made before the words are written, and a holder's side needs no words
        // at all. A database from before quests existed has no table and gets no quest, which
        // is the right answer rather than an error. The slate (npc_slate) is joined when the
        // database has it, for the chain, the nest and the dispute; a database from before
        // the circle pipeline reads every quest as standing alone.
        const bool slate = sqlite3_table_column_metadata(db_, nullptr, "npc_slate", "chain_id",
                                                         nullptr, nullptr, nullptr, nullptr,
                                                         nullptr) == SQLITE_OK;
        std::string sql =
            "SELECT q.quest_id, q.kind, q.giver_soul, q.giver_name, q.target_soul, "
            "       q.target_name, q.item_class, q.item_name, q.reward_groschen, q.skill, "
            "       q.difficulty, COALESCE(t.name, ''), COALESCE(t.offer, ''), "
            "       COALESCE(t.thanks, ''), ";
        if (slate) {
            // The link before this one in its chain, with its item's name for the sheet.
            // The link before this one in its chain, with its item's name for the sheet; or
            // the scene before it, when the slate is new enough to carry scenes
            // (after_quest_id, after_how), which wins over the chain.
            const bool scenes = sqlite3_table_column_metadata(db_, nullptr, "npc_slate", "after_quest_id",
                                                              nullptr, nullptr, nullptr, nullptr,
                                                              nullptr) == SQLITE_OK;
            const std::string chain_before =
                "(SELECT p.quest_id FROM npc_slate p WHERE p.chain_id = s.chain_id "
                " AND p.chain_order = s.chain_order - 1)";
            sql += "COALESCE(s.chain_id, ''), COALESCE(s.chain_order, 0), "
                   "COALESCE(s.requires_quest_id, ''), COALESCE(s.dispute, 0), "
                   "CASE WHEN COALESCE(s.reward_kind, '') IN ('', 'none', 'money') THEN '' "
                   "     ELSE COALESCE(s.reward_text, '') END, ";
            sql += scenes ? "COALESCE(NULLIF(s.after_quest_id, ''), " + chain_before + ", ''), "
                          : "COALESCE(" + chain_before + ", ''), ";
            sql += "COALESCE((SELECT pq.item_name FROM npc_slate p JOIN npc_quest pq "
                   "          USING (quest_id) WHERE p.chain_id = s.chain_id "
                   "          AND p.chain_order = s.chain_order - 1), ''), ";
            sql += scenes ? "COALESCE(s.after_how, '') " : "'' ";
            sql += "FROM npc_quest q LEFT JOIN npc_quest_text t USING (quest_id) "
                   "LEFT JOIN npc_slate s USING (quest_id) ";
        } else {
            sql += "'', 0, '', 0, '', '', '', '' "
                   "FROM npc_quest q LEFT JOIN npc_quest_text t USING (quest_id) ";
        }
        // The soul an ask's word is carried on to is a third party, named only on the
        // quest's lines (onward_soul), when the database has the table.
        const bool lines = sqlite3_table_column_metadata(db_, nullptr, "npc_quest_lines", "field",
                                                         nullptr, nullptr, nullptr, nullptr,
                                                         nullptr) == SQLITE_OK;
        // A chain's links in order, so the first is the first, and a story's scenes by their
        // number (chain_order under story_id, with no chain_id): where one giver has two
        // scenes gated on the same one before, pick_quest offers the first of them, and
        // without this the id decided (the headless matrix found *Letter of the Law*,
        // scene 2, hidden behind scenes 3 and 4). Otherwise as dealt.
        sql += "WHERE q.giver_soul = ?1 OR q.target_soul = ?1 ";
        if (lines) {
            sql += "OR q.quest_id IN (SELECT l.quest_id FROM npc_quest_lines l "
                   "                  WHERE l.field = 'onward_soul' AND l.text = ?1) ";
        }
        const bool stories = slate && sqlite3_table_column_metadata(db_, nullptr, "npc_slate", "story_id",
                                                                    nullptr, nullptr, nullptr, nullptr,
                                                                    nullptr) == SQLITE_OK;
        sql += "ORDER BY ";
        sql += stories ? "COALESCE(s.chain_id, s.story_id, q.quest_id), COALESCE(s.chain_order, 0), q.rowid"
               : slate ? "COALESCE(s.chain_id, q.quest_id), COALESCE(s.chain_order, 0), q.rowid"
                       : "q.rowid";
        sqlite3_stmt* row = nullptr;
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &row, nullptr) == SQLITE_OK) {
            bind(row, soul);
            while (sqlite3_step(row) == SQLITE_ROW) {
                Quest quest;
                quest.id = text_at(row, 0);
                quest.kind = text_at(row, 1);
                quest.giver_soul = text_at(row, 2);
                quest.giver_name = text_at(row, 3);
                quest.target_soul = text_at(row, 4);
                quest.target_name = text_at(row, 5);
                quest.item_class = text_at(row, 6);
                quest.item_name = text_at(row, 7);
                quest.reward_groschen = sqlite3_column_int(row, 8);
                quest.skill = text_at(row, 9);
                quest.difficulty = text_at(row, 10);
                quest.name = text_at(row, 11);
                quest.offer = text_at(row, 12);
                quest.thanks = text_at(row, 13);
                quest.chain_id = text_at(row, 14);
                quest.chain_order = sqlite3_column_int(row, 15);
                quest.requires_quest_id = text_at(row, 16);
                quest.dispute = sqlite3_column_int(row, 17) != 0;
                quest.reward_text = text_at(row, 18);
                quest.after_quest_id = text_at(row, 19);
                quest.after_item_name = text_at(row, 20);
                quest.after_how = text_at(row, 21);
                quest.role = quest.giver_soul == soul    ? QuestRole::Giver
                             : quest.target_soul == soul ? QuestRole::Target
                                                         : QuestRole::Onward;
                found.quests.push_back(std::move(quest));
            }
            sqlite3_finalize(row);
        }
        // The giver's side of npc_quest_lines, when the database has the table and the
        // pipeline wrote them: the dispute's lines and a deliver's, a find's count, and for
        // every kind the openings for the visits the errand makes distinct. The two rows that
        // name an ask's onward soul are read for every role, since the holder's and the
        // onward soul's own paragraphs name the third party too.
        for (Quest& quest : found.quests) {
            const bool giver = quest.role == QuestRole::Giver;
            static constexpr char kLines[] =
                "SELECT field, text FROM npc_quest_lines WHERE quest_id = ? "
                "AND field IN ('apologise', 'apologise_yes', 'apologise_no', "
                "'report', 'keep', 'kept_reply', 'count', 'pry', 'pry_yes', 'pry_no', "
                "'greet_handed', 'told', 'told_reply', 'lie_soft', 'lie_soft_reply', 'lie_big', 'lie_big_reply', 'lie_caught', "
                "'tell', 'lie', 'quiet', 'lie_reply', 'quiet_reply', "
                "'fought', 'fought_reply', 'confess', 'dead_reply', 'spot_text', "
                "'greet_owed', 'greet_settle_unasked', 'greet_offer', 'thanks_onward', "
                "'offer_2', 'agreed', 'more', 'more_reply', 'opener', "
                "'onward_soul', 'onward_name', 'greet_onward', 'keepsake_name', 'thanks_keepsake', 'thanks_exposed', "
                "'item_kind', 'give', 'lie_have', 'lie_have_reply', 'lie_have_caught', 'more_2', 'more_2_reply', "
                "'told_truth', 'told_pinned', 'told_covered', 'told_fibbed', 'told_evaded', 'told_caught', "
                "'told_reply_truth', 'told_reply_pinned', 'told_reply_covered', 'told_reply_fibbed', 'told_reply_evaded', "
                "'told_reply_caught', "
                "'told_cursed', 'told_mocked', 'told_reply_cursed', 'told_reply_mocked', "
                "'told_denounced', 'told_reply_denounced', "
                "'unyielding', 'helper_name', 'greet_rebuffed', 'named_row', 'named_reply', 'give_up_reply', "
                "'seen_with_name', 'item_of', 'greet_unwritten') "
                "OR (quest_id = ? AND (field LIKE 'since%' OR field LIKE 'dictate%' OR field LIKE 'fast\\_travel%' ESCAPE '\\' "
                "OR field LIKE 'write\\_%' ESCAPE '\\' "
                "OR field LIKE 'reconsider\\_%' ESCAPE '\\' OR field LIKE 'soften\\_%' ESCAPE '\\' "
                "OR field LIKE 'embellish\\_%' ESCAPE '\\'))";
            // The receiver's own lines are not read: the owner of a thing carried back asks
            // how it was come by through the game's menu (the mod's interrogation), and the
            // app's conversation with him is either side of that. A holder who will not be
            // moved is told so, since his paragraph turns on it. Both sides read what stands
            // once the errand is done (since, since_<how>; since_target, since_target_<how>),
            // and keep their own.
            static constexpr char kPartyLines[] =
                "SELECT field, text FROM npc_quest_lines WHERE quest_id = ? "
                "AND (field IN ('onward_soul', 'onward_name', 'unyielding', 'seen_with_name', 'item_of') "
                "OR field LIKE 'since%')";
            if (sqlite3_prepare_v2(db_, giver ? kLines : kPartyLines, -1, &row, nullptr) != SQLITE_OK) break;
            // As a string_view in so many words: passed a std::string, the unqualified call
            // finds std::bind by argument lookup instead, which builds a binder and binds
            // nothing.
            bind(row, std::string_view(quest.id));
            if (giver) sqlite3_bind_text(row, 2, quest.id.c_str(), -1, SQLITE_TRANSIENT);
            while (sqlite3_step(row) == SQLITE_ROW) {
                const std::string field = text_at(row, 0);
                if (field == "onward_soul") quest.onward_soul = text_at(row, 1);
                else if (field == "onward_name") quest.onward_name = text_at(row, 1);
                else if (field == "greet_onward") quest.greet_onward = text_at(row, 1);
                else if (field == "keepsake_name") quest.keepsake_name = text_at(row, 1);
                else if (field == "thanks_keepsake") quest.thanks_keepsake = text_at(row, 1);
                else if (field == "thanks_exposed") quest.thanks_exposed = text_at(row, 1);
                else if (field == "fast_travel") quest.fast_travel_row = text_at(row, 1);
                else if (field == "fast_travel_reply") quest.fast_travel_reply = text_at(row, 1);
                else if (field == "fast_travel_later") quest.fast_travel_later_row = text_at(row, 1);
                else if (field == "fast_travel_later_reply") quest.fast_travel_later_reply = text_at(row, 1);
                else if (field == "fast_travel_hours") {
                    // "from-to", hours of the day; the written default stands over a bad one.
                    double from = 0, to = 0;
                    if (std::sscanf(text_at(row, 1).c_str(), "%lf-%lf", &from, &to) == 2 && from < to) {
                        quest.fast_travel_from = from;
                        quest.fast_travel_to = to;
                    }
                }
                else if (field == "apologise") quest.apologise = text_at(row, 1);
                else if (field == "apologise_yes") quest.apologise_yes = text_at(row, 1);
                else if (field == "apologise_no") quest.apologise_no = text_at(row, 1);
                else if (field == "report") quest.report = text_at(row, 1);
                else if (field == "keep") quest.keep = text_at(row, 1);
                else if (field == "kept_reply") quest.kept_reply = text_at(row, 1);
                else if (field == "pry") quest.pry = text_at(row, 1);
                else if (field == "pry_yes") quest.pry_yes = text_at(row, 1);
                else if (field == "pry_no") quest.pry_no = text_at(row, 1);
                else if (field == "greet_handed") quest.greet_handed = text_at(row, 1);
                else if (field == "told") quest.told = text_at(row, 1);
                else if (field == "told_reply") quest.told_reply = text_at(row, 1);
                else if (field == "lie_soft") quest.lie_soft = text_at(row, 1);
                else if (field == "lie_soft_reply") quest.lie_soft_reply = text_at(row, 1);
                else if (field == "lie_big") quest.lie_big = text_at(row, 1);
                else if (field == "lie_big_reply") quest.lie_big_reply = text_at(row, 1);
                else if (field == "lie_caught") quest.lie_caught = text_at(row, 1);
                else if (field == "count") quest.count = std::max(1, std::atoi(text_at(row, 1).c_str()));
                else if (field == "tell") quest.tell = text_at(row, 1);
                else if (field == "lie") quest.lie = text_at(row, 1);
                else if (field == "quiet") quest.quiet = text_at(row, 1);
                else if (field == "lie_reply") quest.lie_reply = text_at(row, 1);
                else if (field == "quiet_reply") quest.quiet_reply = text_at(row, 1);
                else if (field == "fought") quest.fought = text_at(row, 1);
                else if (field == "fought_reply") quest.fought_reply = text_at(row, 1);
                else if (field == "confess") quest.confess = text_at(row, 1);
                else if (field == "dead_reply") quest.dead_reply = text_at(row, 1);
                else if (field == "spot_text") quest.spot_text = text_at(row, 1);
                else if (field == "greet_owed") quest.greet_owed = text_at(row, 1);
                else if (field == "greet_settle_unasked") quest.greet_settle_unasked = text_at(row, 1);
                else if (field == "greet_offer") quest.greet_offer = text_at(row, 1);
                else if (field == "thanks_onward") quest.thanks_onward = text_at(row, 1);
                else if (field == "offer_2") quest.offer_2 = text_at(row, 1);
                else if (field == "agreed") quest.agreed = text_at(row, 1);
                else if (field == "more") quest.more = text_at(row, 1);
                else if (field == "more_reply") quest.more_reply = text_at(row, 1);
                else if (field == "opener") quest.opener = text_at(row, 1);
                else if (field == "item_kind") quest.item_kind = text_at(row, 1);
                else if (field == "give") quest.give = text_at(row, 1);
                else if (field == "lie_have") quest.lie_have = text_at(row, 1);
                else if (field == "lie_have_reply") quest.lie_have_reply = text_at(row, 1);
                else if (field == "lie_have_caught") quest.lie_have_caught = text_at(row, 1);
                else if (field == "more_2") quest.more_2 = text_at(row, 1);
                else if (field == "more_2_reply") quest.more_2_reply = text_at(row, 1);
                else if (field == "unyielding") quest.unyielding = std::atoi(text_at(row, 1).c_str()) != 0;
                else if (field == "helper_name") quest.helper_name = text_at(row, 1);
                else if (field == "seen_with_name") quest.seen_with_name = text_at(row, 1);
                else if (field == "item_of") quest.item_of = text_at(row, 1);
                else if (field == "greet_rebuffed") quest.greet_rebuffed = text_at(row, 1);
                else if (field == "named_row") quest.named_row = text_at(row, 1);
                else if (field == "named_reply") quest.named_reply = text_at(row, 1);
                else if (field == "give_up_reply") quest.give_up_reply = text_at(row, 1);
                else if (field == "greet_unwritten") quest.dictation.greet_unwritten = text_at(row, 1);
                else if (field == "dictate_begin") quest.dictation.begin = text_at(row, 1);
                else if (field == "dictate_done") quest.dictation.done = text_at(row, 1);
                else if (field == "dictate") quest.dictation.resume = text_at(row, 1);
                else if (field.compare(0, 8, "dictate_") == 0) {
                    // dictate_<k>: the giver's words for part k.
                    const int k = std::atoi(field.c_str() + 8);
                    if (k >= 1 && k <= Quest::Dictation::kParts) quest.dictation.parts[k - 1].dictate = text_at(row, 1);
                }
                else if (field.compare(0, 6, "write_") == 0 || field.compare(0, 11, "reconsider_") == 0 ||
                         field.compare(0, 7, "soften_") == 0 || field.compare(0, 10, "embellish_") == 0) {
                    // <way>_<k> is Henry's row for part k, <way>_<k>_reply the giver's answer.
                    const std::size_t under = field.find('_');
                    const std::string way = field.substr(0, under);
                    std::string rest = field.substr(under + 1);
                    bool reply = false;
                    if (rest.size() > 6 && rest.compare(rest.size() - 6, 6, "_reply") == 0) {
                        reply = true;
                        rest.resize(rest.size() - 6);
                    }
                    const int k = std::atoi(rest.c_str());
                    if (k >= 1 && k <= Quest::Dictation::kParts && !rest.empty() &&
                        std::all_of(rest.begin(), rest.end(), [](char c) { return c >= '0' && c <= '9'; })) {
                        Quest::Dictation::Part& part = quest.dictation.parts[k - 1];
                        Quest::Dictation::Choice& choice = way == "write" ? part.write : way == "reconsider" ? part.reconsider
                                                           : way == "soften" ? part.soften : part.embellish;
                        (reply ? choice.reply : choice.row) = text_at(row, 1);
                    }
                }
                else if (field.compare(0, 5, "since") == 0) {
                    // since / since_<how> are the giver's; since_target / since_target_<how>
                    // the other party's. Each side keeps its own.
                    const bool target_side = field.compare(0, 12, "since_target") == 0;
                    if (target_side == giver) continue;
                    std::string how;
                    if (target_side && field.size() > 13) how = field.substr(13);
                    else if (!target_side && field.size() > 6) how = field.substr(6);
                    quest.since_by_how.push_back({how, text_at(row, 1)});
                }
                else if (field.compare(0, 5, "told_") == 0) {
                    // told_<how> and told_reply_<how>: the report row and the giver's answer
                    // for the way the receiver's question was answered.
                    const bool reply = field.compare(0, 11, "told_reply_") == 0;
                    const std::string how = field.substr(reply ? 11 : 5);
                    Quest::Told* slot = nullptr;
                    for (Quest::Told& t : quest.told_by_how) if (t.how == how) slot = &t;
                    if (!slot) {
                        quest.told_by_how.push_back({how, "", ""});
                        slot = &quest.told_by_how.back();
                    }
                    (reply ? slot->reply : slot->row) = text_at(row, 1);
                }
            }
            sqlite3_finalize(row);
        }
        if (!found.quests.empty()) found.quest = found.quests.front();
    }

    // The menu's own answers, from the circle pipeline (scripted.h). Each table may be
    // missing from a database built before the pipeline, and then there is nothing to say
    // from it, which the model covers as it always did. The self-account is taken whether or
    // not the person passed the checks: it is the person's own words about themselves, and
    // the checks that fail a person are about the bio's claims of kin.
    {
        sqlite3_stmt* row = nullptr;
        if (sqlite3_prepare_v2(db_, "SELECT self_account FROM npc_person WHERE soul_name = ?", -1,
                               &row, nullptr) == SQLITE_OK) {
            bind(row, soul);
            if (sqlite3_step(row) == SQLITE_ROW) found.self_account = text_at(row, 0);
            sqlite3_finalize(row);
        }
        // The line for being asked about themselves a second time, written beside the
        // self-account as its variant 1 (variant 0 is the self-account over again).
        if (sqlite3_prepare_v2(db_, "SELECT text FROM npc_seed_answer "
                                    "WHERE soul_name = ? AND intent = 'about' AND variant = 1",
                               -1, &row, nullptr) == SQLITE_OK) {
            bind(row, soul);
            if (sqlite3_step(row) == SQLITE_ROW) found.about_again = text_at(row, 0);
            sqlite3_finalize(row);
        }
        // In the generator's order, which is the order the person call wrote them in and so
        // roughly the order they matter to the soul: the work first, the dead last.
        // A topic that waits on a favour's ending carries the quest and the hows (the
        // after_quest / after_how columns, 22 Oct 2026); a database from before them has
        // every topic theirs from the start, as it did then.
        if (sqlite3_prepare_v2(db_, "SELECT topic_key, label, trust, text, COALESCE(after_quest, ''), "
                                    "COALESCE(after_how, '') FROM npc_knowledge "
                                    "WHERE soul_name = ? ORDER BY rowid",
                               -1, &row, nullptr) == SQLITE_OK ||
            sqlite3_prepare_v2(db_, "SELECT topic_key, label, trust, text, '', '' FROM npc_knowledge "
                                    "WHERE soul_name = ? ORDER BY rowid",
                               -1, &row, nullptr) == SQLITE_OK) {
            bind(row, soul);
            while (sqlite3_step(row) == SQLITE_ROW) {
                found.knowledge.push_back({text_at(row, 0), text_at(row, 1), text_at(row, 2), text_at(row, 3),
                                           text_at(row, 4), text_at(row, 5)});
            }
            sqlite3_finalize(row);
        }
        if (sqlite3_prepare_v2(db_, "SELECT text, COALESCE(refers_to_quest_id, '') FROM npc_seed_answer "
                                    "WHERE soul_name = ? AND intent = 'help' ORDER BY variant",
                               -1, &row, nullptr) == SQLITE_OK) {
            bind(row, soul);
            while (sqlite3_step(row) == SQLITE_ROW) {
                found.help_replies.push_back({text_at(row, 0), text_at(row, 1)});
            }
            sqlite3_finalize(row);
        }
        // Henry's remarks and their answers, in the order the remarks call wrote them, each
        // marked whether it is on the place they stand (place = 'here'). A database from
        // before the column (4 Oct 2026) has every remark fit for anywhere, as it did then.
        if (sqlite3_prepare_v2(db_, "SELECT remark_key, row, text, place FROM npc_remark "
                                    "WHERE soul_name = ? ORDER BY rowid",
                               -1, &row, nullptr) == SQLITE_OK ||
            sqlite3_prepare_v2(db_, "SELECT remark_key, row, text, '' FROM npc_remark "
                                    "WHERE soul_name = ? ORDER BY rowid",
                               -1, &row, nullptr) == SQLITE_OK) {
            bind(row, soul);
            while (sqlite3_step(row) == SQLITE_ROW) {
                const std::string place = text_at(row, 3);
                found.remarks.push_back({text_at(row, 0), text_at(row, 1), text_at(row, 2),
                                         place == "here", place == "here" ? std::string() : place});
            }
            sqlite3_finalize(row);
        }
        // Where their day takes them: one spot per distinct routine position, for
        // away_from_places. A soul without a dumped routine has none and is never away.
        if (sqlite3_prepare_v2(db_, "SELECT DISTINCT pos_x, pos_y FROM npc_routine "
                                    "WHERE soul_name = ? AND pos_x IS NOT NULL AND pos_y IS NOT NULL",
                               -1, &row, nullptr) == SQLITE_OK) {
            bind(row, soul);
            while (sqlite3_step(row) == SQLITE_ROW) {
                found.spots.emplace_back(sqlite3_column_double(row, 0), sqlite3_column_double(row, 1));
            }
            sqlite3_finalize(row);
        }
        // The people their lines might name: everyone in a circle of theirs, by the given
        // name the generator gave them (a soul the generator did not rename has no name a
        // line could use), then the circles' absentees, who are in no save to be met.
        // Themselves left out: a topic does not name its own speaker.
        if (sqlite3_prepare_v2(db_, "SELECT DISTINCT m.soul_name, COALESCE(g.given, '') FROM npc_circle_member m "
                                    "JOIN npc_circle_member j ON j.circle_id = m.circle_id "
                                    "JOIN npc n ON n.soul_name = m.soul_name "
                                    "LEFT JOIN generated_name g ON g.soul_id = n.soul_id "
                                    "WHERE j.soul_name = ? AND m.soul_name <> ? ORDER BY m.soul_name",
                               -1, &row, nullptr) == SQLITE_OK) {
            bind(row, soul);
            sqlite3_bind_text(row, 2, soul.data(), static_cast<int>(soul.size()), SQLITE_TRANSIENT);
            while (sqlite3_step(row) == SQLITE_ROW) {
                const std::string given = text_at(row, 1);
                if (!given.empty()) found.people.push_back({text_at(row, 0), given});
            }
            sqlite3_finalize(row);
        }
        if (sqlite3_prepare_v2(db_, "SELECT DISTINCT a.name FROM npc_absentee a "
                                    "JOIN npc_circle_member j ON j.circle_id = a.circle_id "
                                    "WHERE j.soul_name = ? ORDER BY a.name",
                               -1, &row, nullptr) == SQLITE_OK) {
            bind(row, soul);
            while (sqlite3_step(row) == SQLITE_ROW) {
                const std::string name = text_at(row, 0);
                if (!given_name(name).empty()) found.people.push_back({name_token(name), given_name(name)});
            }
            sqlite3_finalize(row);
        }
    }

    return found;
}

Persona PersonaDb::persona(std::string_view soul, const Approach& approach,
                           std::string_view anim, bool say_written,
                           const QuestFacts& facts, const std::string& memory,
                           const Standing& standing) const {
    const Doing caught = doing(anim);
    if (std::optional<Soul> found = find(soul)) {
        return persona_for(*found, approach, caught, say_written, facts, memory, standing);
    }
    // A stranger still gets the prose. Not knowing who someone is does not stop the player
    // having walked up on them sawing wood, and it is the one concrete thing the sheet can
    // say about a soul the database has never heard of.
    Approach known = approach;
    if (known.activity.empty()) known.activity = caught.activity;
    Persona persona = stranger();
    // Even a soul the database has never heard of may have been talked to before, and the
    // save says so; nothing about being unknown to the database makes them forgetful.
    if (!memory.empty()) {
        persona.sheet += "\n\n" + memory;
        persona.parts.push_back({"memory", memory});
    }
    persona.sheet += "\n\n" + approach_prose(known, caught.clause);
    persona.doing = caught.clause;
    return persona;
}

}  // namespace moonshine
