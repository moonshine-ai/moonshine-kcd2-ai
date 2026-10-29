#include "knowledge.h"
#include "sentence.h"

#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <regex>
#include <sstream>
#include <unordered_map>

namespace moonshine {

namespace {

std::string text_at(sqlite3_stmt* row, int column) {
    const unsigned char* value = sqlite3_column_text(row, column);
    // Every text column comes in undashed (sentence.h), so nothing read from the DB
    // reaches the screen, the voice or the model with a dash in it.
    return value ? undashed(reinterpret_cast<const char*>(value)) : std::string();
}

int words_in(std::string_view text) {
    int words = 0;
    bool in_word = false;
    for (const char c : text) {
        const bool space = std::isspace(static_cast<unsigned char>(c)) != 0;
        if (!space && !in_word) ++words;
        in_word = !space;
    }
    return words;
}

// The answer with its bracketed gestures taken out: "[spits] God keep us" -> "God keep us".
// The gesture is for the animator, not the mouth.
std::string without_gestures(std::string_view text) {
    std::string out;
    bool inside = false;
    for (const char c : text) {
        if (c == '[') { inside = true; continue; }
        if (c == ']') { inside = false; continue; }
        if (!inside) out += c;
    }
    // Collapse the doubled spaces the removal leaves, and trim.
    std::string tidy;
    for (const char c : out) {
        if (c == ' ' && (tidy.empty() || tidy.back() == ' ')) continue;
        tidy += c;
    }
    while (!tidy.empty() && tidy.back() == ' ') tidy.pop_back();
    return tidy;
}

// The plainest ways of offering help, for when the embedder scores them low or is not
// there. The same pattern the loop outside the app used (scripts/kb_viewer.py).
const std::regex& quest_ask_pattern() {
    static const std::regex pattern(
        R"(\b(what ails|anything (else )?i can (do|help)|need (any |a )?(help|hand)|work for me|)"
        R"(looking for work|a (job|task|quest)|what('s| is) (wrong|the matter|troubling)|)"
        R"(can i help|help you with)\b)",
        std::regex::icase | std::regex::optimize);
    return pattern;
}

// The bytes a sha's hex digits spell (the drop keys vectors by the first 16 digits of
// the sha256, so eight bytes; a whole sha256 would be 32), or empty for anything that
// is not hex: the vectors are keyed by the bytes, the item and question rows carry the hex.
std::string sha_bytes(std::string_view hex) {
    if (hex.empty() || hex.size() % 2 != 0 || hex.size() > 64) return {};
    std::string out(hex.size() / 2, '\0');
    for (std::size_t i = 0; i < out.size(); ++i) {
        int byte = 0;
        for (int half = 0; half < 2; ++half) {
            const char c = hex[2 * i + static_cast<std::size_t>(half)];
            int digit;
            if (c >= '0' && c <= '9') digit = c - '0';
            else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
            else return {};
            byte = byte * 16 + digit;
        }
        out[i] = static_cast<char>(byte);
    }
    return out;
}

// A stored vector as floats: `dim` int8s times `scale`, made a unit vector again (the
// rounding took it a hair off one), or -- a row from before the vectors were quantised --
// `dim` float32s as they are. Empty when the blob is neither.
std::vector<float> vector_from(int dim, double scale, const void* blob, int bytes) {
    std::vector<float> vec;
    if (!blob || dim <= 0) return vec;
    if (bytes == dim) {
        const auto* q = static_cast<const signed char*>(blob);
        vec.resize(static_cast<std::size_t>(dim));
        float sum = 0.0f;
        for (int i = 0; i < dim; ++i) {
            vec[static_cast<std::size_t>(i)] = static_cast<float>(q[i] * scale);
            sum += vec[static_cast<std::size_t>(i)] * vec[static_cast<std::size_t>(i)];
        }
        if (sum > 0.0f) {
            const float inv = 1.0f / std::sqrt(sum);
            for (float& x : vec) x *= inv;
        }
    } else if (bytes >= dim * static_cast<int>(sizeof(float))) {
        vec.resize(static_cast<std::size_t>(dim));
        std::memcpy(vec.data(), blob, static_cast<std::size_t>(dim) * sizeof(float));
    }
    return vec;
}

const std::regex& another_pattern() {
    static const std::regex pattern(
        R"(\b(other|another|the first|the second|the third|what about|and the|instead|else)\b)",
        std::regex::icase | std::regex::optimize);
    return pattern;
}

}  // namespace

const std::vector<std::string_view> kQuestAsks = {
    "Is there anything I can help you with?", "What ails you?", "Do you have any work for me?",
    "Is something troubling you?", "Can I do anything for you?", "Do you need a hand with anything?",
    "Is there a job you need doing?", "Anything I can do for you?", "You look worried. What's wrong?",
    "Do you need help?", "Have you a task for me?", "What's the matter?", "Is there something you need?",
    "Anything you need fetched or done?", "I'm looking for work.", "What troubles you, friend?",
};

// ---- SoulIndex ----------------------------------------------------------------------------

void SoulIndex::add(KbItem item, std::vector<std::vector<float>> vectors,
                    std::vector<std::string> texts) {
    items_.push_back(std::make_unique<KbItem>(std::move(item)));
    const std::size_t at = items_.size() - 1;
    for (std::size_t i = 0; i < vectors.size(); ++i) {
        const std::vector<float>& vec = vectors[i];
        if (vec.empty()) continue;
        if (dim_ == 0) dim_ = static_cast<int>(vec.size());
        if (static_cast<int>(vec.size()) != dim_) continue;
        rows_.push_back({at, i < texts.size() ? texts[i] : std::string()});
        mat_.insert(mat_.end(), vec.begin(), vec.end());
    }
}

std::vector<KbHit> SoulIndex::query(const std::vector<float>& query,
                                    const std::vector<std::string_view>& prefixes,
                                    std::size_t top) const {
    std::vector<KbHit> out;
    if (rows_.empty() || static_cast<int>(query.size()) < dim_) return out;
    const std::vector<float> q = truncated(query, static_cast<std::size_t>(dim_));
    const auto in_field = [&](const KbItem& item) {
        if (prefixes.empty()) return true;
        for (const std::string_view prefix : prefixes) {
            if (item.item_id.compare(0, prefix.size(), prefix) == 0) return true;
        }
        return false;
    };
    // The best text of each item, in one pass over the matrix.
    std::vector<int> best_row(items_.size(), -1);
    std::vector<float> best(items_.size(), -2.0f);
    const std::size_t dim = static_cast<std::size_t>(dim_);
    for (std::size_t r = 0; r < rows_.size(); ++r) {
        const std::size_t item = rows_[r].item;
        if (!in_field(*items_[item])) continue;
        const float* row = mat_.data() + r * dim;
        float dot = 0.0f;
        for (std::size_t k = 0; k < dim; ++k) dot += row[k] * q[k];
        if (dot > best[item]) {
            best[item] = dot;
            best_row[item] = static_cast<int>(r);
        }
    }
    for (std::size_t i = 0; i < items_.size(); ++i) {
        if (best_row[i] < 0) continue;
        out.push_back({items_[i].get(), best[i], rows_[static_cast<std::size_t>(best_row[i])].text});
    }
    std::sort(out.begin(), out.end(), [](const KbHit& a, const KbHit& b) { return a.score > b.score; });
    if (out.size() > top) out.resize(top);
    return out;
}

const KbItem* SoulIndex::find(std::string_view item_id) const {
    for (const auto& item : items_) {
        if (item->item_id == item_id) return item.get();
    }
    return nullptr;
}

// ---- the rules ----------------------------------------------------------------------------

std::vector<std::string_view> hub_span(std::string_view hub_id) {
    if (hub_id == "hub:tales") return {"tale:"};
    if (hub_id == "hub:life") return {"life:"};
    if (hub_id == "hub:times") return {"view:"};
    return {};
}

std::optional<KbHit> pick(const std::vector<KbHit>& hits, Trust tier) {
    std::vector<const KbHit*> above;
    for (const KbHit& hit : hits) {
        if (hit.item && hit.score >= kKnowledgeTau) above.push_back(&hit);
    }
    if (above.empty()) return std::nullopt;
    const KbHit* best = above.front();
    // A hub is an opener. Asked something specific that one of its items answers, the item
    // is the answer and the hub only a way of not giving it.
    if (best->item->is_hub()) {
        for (std::size_t i = 1; i < above.size(); ++i) {
            if (!above[i]->item->is_hub() && best->score - above[i]->score <= kHubMargin) {
                best = above[i];
                break;
            }
        }
    }
    // Something she will not tell them yet loses a near tie to something she will -- but not
    // to a hub, which is an offer of other things and not an answer to what they asked.
    if (!trust_allows(tier, best->item->trust)) {
        for (const KbHit* hit : above) {
            if (trust_allows(tier, hit->item->trust) && !hit->item->is_hub() &&
                best->score - hit->score <= kGateMargin) {
                return *hit;
            }
        }
    }
    return *best;
}

bool reads_as_quest_ask(std::string_view line) {
    return std::regex_search(line.begin(), line.end(), quest_ask_pattern());
}

bool asks_for_another(std::string_view line) {
    return std::regex_search(line.begin(), line.end(), another_pattern());
}

Advice direction_for_hit(const KbHit* hit, Trust tier, std::string_view followup) {
    Advice out;
    if (!hit || !hit->item) {
        // Not every line asks something. A greeting or a remark gets no knowledge and needs
        // none; the rule is only for a question, so that the answer to one nobody wrote is
        // honest ignorance rather than an invented fact.
        out.direction =
            "[If they asked about something in particular, you know nothing of it: say so in "
            "your own way, briefly, without inventing a fact, a name or a place. A greeting, "
            "thanks or small talk needs no knowledge; answer it naturally.]";
        out.note = "nothing above tau";
        return out;
    }
    const KbItem& item = *hit->item;
    out.item_id = item.item_id;
    out.knows = !item.is_taboo();
    if (!trust_allows(tier, item.trust)) {
        out.direction = "[You know something about this but will not tell them yet; they are not "
                        "trusted enough.";
        if (!item.price_what.empty()) {
            out.direction += " What would loosen your tongue: " + item.price_what + ".";
        }
        if (!item.withheld.empty()) {
            out.direction += " Deflect along these lines: " + item.withheld;
        }
        out.direction += "]";
        out.note = "withheld " + item.item_id + " (" + item.trust + ")";
        return out;
    }
    if (item.is_taboo()) {
        out.direction = "[Nobody of 1403 asks this; it is dangerous talk. You are shocked and shut "
                        "it down, more or less so: " + without_gestures(item.answer) +
                        " You do not answer the question itself or debate it, and speak only, "
                        "no gestures.]";
        out.note = "taboo " + item.item_id;
        return out;
    }
    if (followup == "more") {
        out.direction = "[They want more on the same matter. All you know of it is still this: " +
                        item.answer + " Say what of it bears on their question, or that that is "
                        "all you know; nothing new.]";
        out.note = "more on " + item.item_id;
        return out;
    }
    const bool chose = !followup.empty();
    const std::string chosen =
        chose ? "They have chosen this from what you offered; go straight on with it. " : "";
    if (item.is_tale()) {
        out.direction = "[" + chosen + "Tell this tale whole, start to end and about as long as "
                        "it is given here (" + std::to_string(words_in(item.answer)) +
                        " words), not a summary of it; the two-or-three-sentence rule does not "
                        "apply to a tale. Keep every turn and name it has and add none: " +
                        item.answer + "]";
        out.tale = true;
    } else {
        out.direction = "[" + chosen + "What you know about this: " + item.answer +
                        " Say it in your own words if it answers what they asked, keeping every "
                        "particular and adding none; if it does not answer them, say you cannot "
                        "help with that.]";
    }
    out.note = std::string(chose ? "chose " : "used ") + item.item_id;
    return out;
}

Advice quest_ask_direction(std::string_view quest_id, std::string_view ask, int reward_groschen,
                           AskState state) {
    Advice out;
    if (quest_id.empty() || state == AskState::None) {
        out.direction = "[They are offering to help. You have no task to give them and ask nothing "
                        "of them; say so in your own way, as the person you are, what weighs on "
                        "you may colour it, but do not invent an errand.]";
        out.note = "no quest to ask";
        return out;
    }
    out.item_id = "quest:" + std::string(quest_id);
    if (state == AskState::Settled) {
        out.direction = "[They are offering to help. The favour between you stands as you know it "
                        "does, taken up or done, and you have no new errand for them: say so, "
                        "or speak of where that one stands, and invent nothing to add to it.]";
        out.note = "quest " + std::string(quest_id) + " (settled)";
        return out;
    }
    if (state == AskState::Again) {
        out.direction = "[They are offering to help again. You have already asked them this and "
                        "have nothing else to ask; remind them of it briefly, in your own words, "
                        "and invent no second errand: " + std::string(ask) + "]";
        out.note = "quest " + std::string(quest_id) + " (again)";
        return out;
    }
    out.direction = "[They are offering to help, and you have something to ask. Put it to them "
                    "plainly in your own words, fitted to the talk so far, so that they know "
                    "exactly what is wanted: who, what, where, and why it matters to you. Every "
                    "particular comes from this and nothing is added, no sums, names or places "
                    "it does not give: " + std::string(ask);
    if (reward_groschen > 0) {
        // Not the figure: the speaking rule has no numbers said aloud.
        out.direction += " There is money in it for them, which you say without naming a sum.";
    }
    out.direction += " You ask; do not assume they have agreed.]";
    out.note = "quest " + std::string(quest_id);
    return out;
}

Advice gate_knowledge(Advice advice, KbGate gate, std::string_view quest_id, std::string_view ask,
                      bool again, std::string_view remind) {
    if (gate == KbGate::Open || !advice.knows) return advice;
    const std::string was = advice.note;
    Advice out;
    out.note = "gated (" + std::string(kb_gate_word(gate)) + "), would have " + was;
    switch (gate) {
        case KbGate::Asking:
            // The ask rides the gate the same way it rides an offer of help, so the judge
            // hears the yes that may follow and the second asking is a reminder, not the
            // whole offer again.
            out.item_id = "quest:" + std::string(quest_id);
            if (again || ask.empty()) {
                out.direction =
                    "[They ask you something you may well know, but you will not be drawn on "
                    "it: you have asked them for help already and had no answer, and what you "
                    "know is for those who help you. Say so briefly, in your own words, and "
                    "remind them in a word of what you asked; invent no fact, name or place, "
                    "and no second errand.]";
            } else {
                out.direction =
                    "[They ask you something you may well know, but you will not be drawn on "
                    "it yet: your own trouble comes first. Say in a line that you will gladly "
                    "tell them what you know once they have helped you, then put what you want "
                    "to them plainly in your own words, so that they know exactly what is "
                    "wanted: who, what, where, and why it matters to you. Every particular "
                    "comes from this and nothing is added, no sums, names or places it does "
                    "not give: " + std::string(ask) + " You ask; do not assume they have agreed.]";
            }
            return out;
        case KbGate::Owed:
            out.direction =
                "[They ask you something you may well know, but they have your errand in hand "
                "and nothing to show for it yet; what you know is the help you promised them, "
                "and they shall have it when it is done. Say so briefly, in your own words, "
                "and remind them of what they agreed to" +
                (remind.empty() ? std::string(" in a word, inventing nothing.]")
                                : ", every particular from this and nothing added: " +
                                      std::string(remind) + "]");
            return out;
        case KbGate::Shut:
        case KbGate::Open:
            break;
    }
    out.direction =
        "[They ask you something you may well know, but you will not be drawn on it: you have "
        "trouble of your own and no mind for a stranger's questions. Put them off in a line, in "
        "your own way, without inventing a fact, a name or a place.]";
    return out;
}

// ---- KnowledgeBase ------------------------------------------------------------------------

KnowledgeBase::KnowledgeBase(std::filesystem::path path) : path_(std::move(path)) {
    const int rc = sqlite3_open_v2(path_.string().c_str(), &db_, SQLITE_OPEN_READONLY, nullptr);
    if (rc != SQLITE_OK) {
        trouble_ = db_ ? sqlite3_errmsg(db_) : "out of memory";
        if (db_) sqlite3_close(db_);
        db_ = nullptr;
        return;
    }
    // The vectors: npcs-vectors.sqlite beside the database, attached as `vec` (read-only,
    // as the connection is). Without one, a database from before the split may hold the
    // table itself; otherwise there are no vectors, and has_tables() says so.
    std::filesystem::path vectors = path_;
    vectors.replace_filename(path_.stem().string() + "-vectors" + path_.extension().string());
    std::error_code ignored;
    if (std::filesystem::exists(vectors, ignored)) {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, "ATTACH DATABASE ?1 AS vec", -1, &stmt, nullptr) == SQLITE_OK) {
            const std::string where = vectors.string();
            sqlite3_bind_text(stmt, 1, where.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(stmt) == SQLITE_DONE) vectors_in_ = "vec";
            else trouble_ = "cannot attach " + vectors.filename().string() + ": " + sqlite3_errmsg(db_);
            sqlite3_finalize(stmt);
        }
    } else {
        sqlite3_stmt* stmt = nullptr;
        const char* sql = "SELECT count(*) FROM main.sqlite_master WHERE type = 'table' AND name = 'npc_kb_vector'";
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0) == 1) vectors_in_ = "main";
            sqlite3_finalize(stmt);
        }
        if (vectors_in_.empty()) trouble_ = "no " + vectors.filename().string() + " beside it";
    }
}

KnowledgeBase::~KnowledgeBase() {
    if (db_) sqlite3_close(db_);
}

bool KnowledgeBase::has_tables() const {
    if (!db_ || vectors_in_.empty()) return false;
    sqlite3_stmt* stmt = nullptr;
    const std::string sql =
        "SELECT (SELECT count(*) FROM main.sqlite_master WHERE type = 'table' AND name IN "
        "('npc_kb_owner', 'npc_kb_item', 'npc_kb_question')) + (SELECT count(*) FROM " +
        vectors_in_ + ".sqlite_master WHERE type = 'table' AND name = 'npc_kb_vector')";
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return false;
    int found = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) found = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return found == 4;
}

int KnowledgeBase::souls_with_items() const {
    if (!has_tables()) return 0;
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT count(DISTINCT soul_name) FROM npc_kb_owner o WHERE EXISTS "
                      "(SELECT 1 FROM npc_kb_item i WHERE i.owner = o.owner)";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return 0;
    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return count;
}

SoulIndex KnowledgeBase::load(std::string_view soul) const {
    SoulIndex index;
    if (!db_) return index;

    // The owners: the soul first, then the shared sets. A soul's own item wins a tie with a
    // shared one only by coming first, which is as far as the plan's soul > circle > station
    // priority is applied here.
    std::vector<std::string> owners;
    {
        sqlite3_stmt* stmt = nullptr;
        const char* sql = "SELECT owner FROM npc_kb_owner WHERE soul_name = ?1 "
                          "ORDER BY owner != ?1, owner";
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return index;
        sqlite3_bind_text(stmt, 1, soul.data(), static_cast<int>(soul.size()), SQLITE_TRANSIENT);
        while (sqlite3_step(stmt) == SQLITE_ROW) owners.push_back(text_at(stmt, 0));
        sqlite3_finalize(stmt);
    }
    if (owners.empty()) return index;

    struct Pending {
        KbItem item;
        std::vector<std::string> shas;
        std::vector<std::string> texts;
    };
    std::vector<Pending> pending;

    sqlite3_stmt* items = nullptr;
    sqlite3_stmt* questions = nullptr;
    const char* items_sql =
        "SELECT item_id, scope, topic, kind, label, trust, price_what, withheld, answer, flag, "
        "answer_sha FROM npc_kb_item WHERE owner = ? AND answer != '' ORDER BY scope, topic, item_id";
    const char* questions_sql =
        "SELECT text, sha FROM npc_kb_question WHERE owner = ? AND item_id = ? ORDER BY n";
    if (sqlite3_prepare_v2(db_, items_sql, -1, &items, nullptr) != SQLITE_OK) return index;
    if (sqlite3_prepare_v2(db_, questions_sql, -1, &questions, nullptr) != SQLITE_OK) {
        sqlite3_finalize(items);
        return index;
    }
    for (const std::string& owner : owners) {
        sqlite3_reset(items);
        sqlite3_bind_text(items, 1, owner.c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(items) == SQLITE_ROW) {
            Pending p;
            p.item.owner = owner;
            p.item.item_id = text_at(items, 0);
            p.item.scope = text_at(items, 1);
            p.item.topic = text_at(items, 2);
            p.item.kind = text_at(items, 3);
            p.item.label = text_at(items, 4);
            p.item.trust = text_at(items, 5);
            p.item.price_what = text_at(items, 6);
            p.item.withheld = text_at(items, 7);
            p.item.answer = text_at(items, 8);
            p.item.flag = text_at(items, 9);
            p.shas.push_back(text_at(items, 10));
            p.texts.push_back(p.item.answer);
            pending.push_back(std::move(p));
        }
        for (Pending& p : pending) {
            if (p.item.owner != owner || p.shas.size() > 1) continue;
            sqlite3_reset(questions);
            sqlite3_bind_text(questions, 1, owner.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(questions, 2, p.item.item_id.c_str(), -1, SQLITE_TRANSIENT);
            while (sqlite3_step(questions) == SQLITE_ROW) {
                p.texts.push_back(text_at(questions, 0));
                p.shas.push_back(text_at(questions, 1));
            }
        }
    }
    sqlite3_finalize(items);
    sqlite3_finalize(questions);

    // The vectors, one lookup each by primary key. A few thousand point reads on a local
    // file is a few milliseconds. The split layout keys them by the sha's bytes and
    // stores int8s with a scale; the one-file layout from before keyed them by the hex
    // and stored float32s (vector_from reads either blob).
    std::unordered_map<std::string, std::vector<float>> vectors;
    if (!vectors_in_.empty()) {
        const bool split = vectors_in_ == "vec";
        sqlite3_stmt* stmt = nullptr;
        const std::string sql = split ? "SELECT dim, vec, scale FROM vec.npc_kb_vector WHERE sha = ?"
                                      : "SELECT dim, vec, 1.0 FROM main.npc_kb_vector WHERE sha = ?";
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
            for (const Pending& p : pending) {
                for (const std::string& sha : p.shas) {
                    if (sha.empty() || vectors.count(sha)) continue;
                    sqlite3_reset(stmt);
                    if (split) {
                        const std::string key = sha_bytes(sha);
                        if (key.empty()) continue;
                        sqlite3_bind_blob(stmt, 1, key.data(), static_cast<int>(key.size()), SQLITE_TRANSIENT);
                    } else {
                        sqlite3_bind_text(stmt, 1, sha.c_str(), -1, SQLITE_TRANSIENT);
                    }
                    if (sqlite3_step(stmt) != SQLITE_ROW) continue;
                    const int dim = sqlite3_column_int(stmt, 0);
                    const void* blob = sqlite3_column_blob(stmt, 1);
                    const int bytes = sqlite3_column_bytes(stmt, 1);
                    std::vector<float> vec = vector_from(dim, sqlite3_column_double(stmt, 2), blob, bytes);
                    if (vec.empty()) continue;
                    vectors.emplace(sha, std::move(vec));
                }
            }
            sqlite3_finalize(stmt);
        }
    }
    for (Pending& p : pending) {
        std::vector<std::vector<float>> vecs;
        vecs.reserve(p.shas.size());
        for (const std::string& sha : p.shas) {
            const auto found = vectors.find(sha);
            vecs.push_back(found == vectors.end() ? std::vector<float>() : found->second);
        }
        index.add(std::move(p.item), std::move(vecs), std::move(p.texts));
    }
    return index;
}

// ---- KnowledgeDirector ----------------------------------------------------------------------------

KnowledgeDirector::KnowledgeDirector(Embedder& embedder, KnowledgeBase& base)
    : embedder_(embedder), base_(base) {}

const SoulIndex& KnowledgeDirector::index_of(std::string_view soul) {
    const auto found = indexes_.find(soul);
    if (found != indexes_.end()) return found->second;
    // A handful kept: the player goes back and forth between the same few souls, and an
    // index is a few megabytes.
    constexpr std::size_t kKeep = 6;
    while (order_.size() >= kKeep) {
        indexes_.erase(order_.front());
        order_.erase(order_.begin());
    }
    order_.emplace_back(soul);
    return indexes_.emplace(std::string(soul), base_.load(soul)).first->second;
}

std::size_t KnowledgeDirector::open(std::string_view soul) {
    std::lock_guard<std::mutex> held(lock_);
    return index_of(soul).size();
}

void KnowledgeDirector::hold(std::string soul, SoulIndex index) {
    std::lock_guard<std::mutex> held(lock_);
    if (indexes_.find(soul) == indexes_.end()) order_.push_back(soul);
    indexes_[soul] = std::move(index);
}

std::size_t KnowledgeDirector::items_of(std::string_view soul) const {
    const auto found = indexes_.find(soul);
    return found == indexes_.end() ? 0 : found->second.size();
}

void KnowledgeDirector::ensure_asks() {
    if (!ask_vectors_.empty()) return;
    for (const std::string_view ask : kQuestAsks) {
        ask_vectors_.push_back(embedder_.embed(ask, /*query=*/false));
    }
}

bool KnowledgeDirector::is_quest_ask(std::string_view line, float* score) {
    std::lock_guard<std::mutex> held(lock_);
    ensure_asks();
    const std::vector<float> q = embedder_.embed(line, /*query=*/true);
    float best = -1.0f;
    for (const std::vector<float>& ask : ask_vectors_) best = std::max(best, cosine(ask, q));
    if (score) *score = best;
    return reads_as_quest_ask(line) || best >= kQuestAskTau;
}

std::vector<KbHit> KnowledgeDirector::rank(std::string_view soul, std::string_view line) {
    std::lock_guard<std::mutex> held(lock_);
    const SoulIndex& index = index_of(soul);
    if (index.empty()) return {};
    return index.query(embedder_.embed(line, /*query=*/true));
}

Advice KnowledgeDirector::advise(std::string_view soul, std::string_view line, Trust tier,
                         const std::vector<Drew>& drew) {
    std::lock_guard<std::mutex> held(lock_);
    const SoulIndex& index = index_of(soul);
    if (index.empty()) {
        // Nothing written for them: no direction at all, and the model answers from the
        // sheet as it always did. Telling it to plead ignorance would make a soul with no
        // knowledge base know less than the sheet says.
        Advice none;
        none.note = "no knowledge base for this soul";
        return none;
    }
    const std::vector<KbHit> hits = index.query(embedder_.embed(line, /*query=*/true));
    if (const std::optional<KbHit> chosen = pick(hits, tier)) {
        return direction_for_hit(&*chosen, tier);
    }

    // Nothing scored. A short line after a reply that drew on something is read in its
    // light: the answer to the question the reply asked, or a request for more of it.
    const Drew* last = drew.empty() ? nullptr : &drew.back();
    if (last && !last->item_id.empty() && words_in(line) <= kFollowWords) {
        // The offer they are answering: the last reply if it was a hub, or -- when they ask
        // for "the other one" -- a hub within the two before, less what has been told since.
        const std::size_t from = drew.size() > 3 ? drew.size() - 3 : 0;
        const Drew* hub = nullptr;
        for (std::size_t i = drew.size(); i-- > from;) {
            if (!hub_span(drew[i].item_id).empty()) {
                hub = &drew[i];
                break;
            }
        }
        if (hub && (hub == last || asks_for_another(line))) {
            std::vector<std::string> taken;
            for (std::size_t i = from; i < drew.size(); ++i) {
                if (!drew[i].item_id.empty() && hub_span(drew[i].item_id).empty()) {
                    taken.push_back(drew[i].item_id);
                }
            }
            // The reply named what was offered, so the reply plus their answer finds the one
            // they took. Not filtered by standing: if they picked the thing she will not tell
            // a stranger, the right answer is her deflection, not the next thing on the list.
            const std::vector<float> q =
                embedder_.embed(hub->reply + " " + std::string(line), /*query=*/true);
            std::vector<KbHit> offered = index.query(q, hub_span(hub->item_id));
            if (hub != last) {
                offered.erase(std::remove_if(offered.begin(), offered.end(),
                                             [&](const KbHit& hit) {
                                                 return std::find(taken.begin(), taken.end(),
                                                                  hit.item->item_id) != taken.end();
                                             }),
                              offered.end());
            }
            if (!offered.empty() && offered.front().score >= kFollowTau) {
                return direction_for_hit(&offered.front(), tier, "chose");
            }
            if (hub == last) return direction_for_hit(nullptr, tier);
        }
        if (const KbItem* item = index.find(last->item_id)) {
            const KbHit again{item, 0.0f, {}};
            return direction_for_hit(&again, tier, "more");
        }
    }
    return direction_for_hit(nullptr, tier);
}

}  // namespace moonshine
