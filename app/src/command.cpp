#include "command.h"
#include "../../mod/src/settlement_boundaries.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include "json.hpp"

namespace moonshine {

const char* kCompactCommandGrammar =
    "root ::= \"NONE\" | (\"LOCATE: \" | \"FOLLOW: \" | \"TRAVEL: \") target\n"
    "target ::= [A-Za-z0-9'\" .,\\-]{1,48}\n";

const char* kCompactCommandSystemPrompt =
    "You classify player utterances in a medieval RPG dialogue into an immediate NPC command or NONE.\n"
    "Output EXACTLY ONE line in one of these four formats:\n"
    "NONE\n"
    "LOCATE: <person name, role like 'your brother', trade like 'the blacksmith', or pronoun like 'him'/'her'>\n"
    "FOLLOW: <person name or 'me'>\n"
    "TRAVEL: <settlement, castle, or landmark name>\n\n"
    "Rules:\n"
    "- Output LOCATE only when the player asks where someone currently is or how to find them right now.\n"
    "- Output NONE for past/biographical questions ('Where did X grow up?', 'Where was X born?'), "
    "opinions/gossip ('Can I trust X?'), idioms ('I don't follow you'), and ordinary conversation.";

namespace {

using json = nlohmann::json;

struct CanonicalPlace {
    const char* id;
    const char* name;
    std::vector<const char*> aliases;
};

const std::vector<CanonicalPlace>& canonical_places() {
    static const std::vector<CanonicalPlace> places = {
        {"kuttenberg", "Kuttenberg", {"kutna hora", "the city", "silver city"}},
        {"troskowitz", "Troskowitz", {"troskovice"}},
        {"trosky_castle", "Trosky Castle", {"trosky", "the castle", "lord bergow's castle"}},
        {"tachov", "Tachov", {}},
        {"semine", "Semine", {"semin", "semine fortress"}},
        {"zhelejov", "Zhelejov", {"zelejov", "zhelejov wagoners inn"}},
        {"apollonia", "Apollonia", {}},
        {"nebakov", "Nebakov", {"nebakov mill", "nebakov fortress"}},
        {"nomads_camp", "Nomads' Camp", {"nomad camp", "nomads camp", "voivode's camp"}},
        {"suchdol", "Suchdol", {"suchdol castle"}},
        {"grund", "Grund", {}},
        {"horschan", "Horschan", {"horany"}},
        {"miskowitz", "Miskowitz", {"miskovice"}},
        {"bylany", "Bylany", {}},
        {"maleshov", "Maleshov", {"malesov"}},
        {"opatowitz", "Opatowitz", {"opatovice"}},
        {"prenzlau", " Rabstein ", {"rabstein", "rabstein mill"}},
        {"sedletz", "Sedletz", {"sedlec", "sedletz monastery", "the monastery"}},
        {"sigismunds_camp", "Sigismund's Camp", {"sigismunds camp", "royal army camp"}},
        {"devils_den", "Devil's Den", {"devils den"}},
    };
    return places;
}

std::string trim_ws(std::string_view s) {
    std::size_t a = 0;
    while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    std::size_t b = s.size();
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return std::string(s.substr(a, b - a));
}

// Fold common Czech/German UTF-8 diacritics to ASCII before lowercasing.
std::string fold_utf8_diacritics(std::string_view s) {
    static const std::pair<std::string_view, std::string_view> table[] = {
        {"á", "a"}, {"Á", "a"}, {"ä", "a"}, {"Ä", "a"},
        {"č", "c"}, {"Č", "c"}, {"ď", "d"}, {"Ď", "d"},
        {"é", "e"}, {"É", "e"}, {"ě", "e"}, {"Ě", "e"},
        {"í", "i"}, {"Í", "i"}, {"ň", "n"}, {"Ň", "n"},
        {"ó", "o"}, {"Ó", "o"}, {"ö", "o"}, {"Ö", "o"},
        {"ř", "r"}, {"Ř", "r"}, {"š", "s"}, {"Š", "s"},
        {"ť", "t"}, {"Ť", "t"}, {"ú", "u"}, {"Ú", "u"},
        {"ů", "u"}, {"Ů", "u"}, {"ü", "u"}, {"Ü", "u"},
        {"ý", "y"}, {"Ý", "y"}, {"ž", "z"}, {"Ž", "z"},
        {"ß", "ss"},
    };
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) {
        bool matched = false;
        if ((static_cast<unsigned char>(s[i]) & 0x80u) != 0) {
            for (const auto& [from, to] : table) {
                if (s.substr(i, from.size()) == from) {
                    out.append(to);
                    i += from.size();
                    matched = true;
                    break;
                }
            }
        }
        if (!matched) {
            out.push_back(s[i]);
            ++i;
        }
    }
    return out;
}

std::vector<std::string> split_words(std::string_view s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!cur.empty()) {
                out.push_back(std::move(cur));
                cur.clear();
            }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(std::move(cur));
    return out;
}

std::string join_words(const std::vector<std::string>& w, std::size_t from = 0) {
    std::string out;
    for (std::size_t i = from; i < w.size(); ++i) {
        if (!out.empty()) out.push_back(' ');
        out += w[i];
    }
    return out;
}

std::string strip_determiners(std::string_view text) {
    static const std::set<std::string_view> stops = {
        "the", "a", "an", "your", "my", "our", "his", "her", "their", "this", "that", "local", "old", "young"};
    std::vector<std::string> toks = split_words(command_normalize(text));
    while (toks.size() > 1 && stops.count(toks.front()) > 0) {
        toks.erase(toks.begin());
    }
    return join_words(toks);
}

bool is_male_pronoun(std::string_view s) {
    return s == "he" || s == "him" || s == "his" || s == "that man" || s == "the man" ||
           s == "that fellow" || s == "the fellow" || s == "that lad";
}

bool is_female_pronoun(std::string_view s) {
    return s == "she" || s == "her" || s == "hers" || s == "that woman" || s == "the woman" ||
           s == "that girl" || s == "the girl" || s == "that lady";
}

bool is_pronoun_mention(std::string_view s) {
    const std::string det = strip_determiners(s);
    const std::string norm = command_normalize(s);
    return is_male_pronoun(det) || is_female_pronoun(det) || is_male_pronoun(norm) ||
           is_female_pronoun(norm) || det == "they" || det == "them";
}

const std::unordered_map<std::string, std::vector<std::string>>& role_synonyms() {
    static const std::unordered_map<std::string, std::vector<std::string>> m = {
        {"husband", {"husband", "man", "spouse"}},
        {"wife", {"wife", "woman", "spouse", "missus"}},
        {"brother", {"brother", "kin", "kinsman"}},
        {"sister", {"sister", "kin", "kinswoman"}},
        {"father", {"father", "dad", "pa", "sire"}},
        {"mother", {"mother", "mum", "ma"}},
        {"son", {"son", "boy", "lad"}},
        {"daughter", {"daughter", "girl", "lass"}},
        {"employer", {"employer", "master", "boss", "lord"}},
        {"employee", {"servant", "hand", "hired hand", "worker", "helper"}},
        {"apprentice", {"apprentice", "student", "boy"}},
        {"master", {"master", "teacher", "employer"}},
        {"friend", {"friend", "companion", "mate"}},
        {"comrade", {"comrade", "fellow guard", "brother-in-arms", "partner"}},
        {"rival", {"rival", "enemy"}},
        {"neighbor", {"neighbor", "neighbour"}},
    };
    return m;
}

int edit_distance(std::string_view a, std::string_view b) {
    if (a == b) return 0;
    if (a.empty()) return static_cast<int>(b.size());
    if (b.empty()) return static_cast<int>(a.size());
    std::vector<int> prev(b.size() + 1);
    for (std::size_t j = 0; j <= b.size(); ++j) prev[j] = static_cast<int>(j);
    for (std::size_t i = 0; i < a.size(); ++i) {
        std::vector<int> cur(b.size() + 1);
        cur[0] = static_cast<int>(i + 1);
        for (std::size_t j = 0; j < b.size(); ++j) {
            const int cost = (a[i] == b[j]) ? 0 : 1;
            cur[j + 1] = std::min({cur[j] + 1, prev[j + 1] + 1, prev[j] + cost});
        }
        prev = std::move(cur);
    }
    return prev[b.size()];
}

std::string phonetic_phrase(std::string_view text) {
    const std::vector<std::string> toks = split_words(text);
    std::string out;
    for (const std::string& t : toks) {
        const std::string k = command_phonetic_key(t);
        if (!k.empty()) {
            if (!out.empty()) out.push_back(' ');
            out += k;
        }
    }
    return out;
}

double surface_and_phonetic_score(std::string_view q_norm,
                                  std::string_view q_phon,
                                  std::string_view c_norm,
                                  std::string_view c_phon) {
    if (q_norm.empty() || c_norm.empty()) return 0.0;
    if (q_norm == c_norm) return 1.0;

    const double jw = jaro_winkler_similarity(q_norm, c_norm);
    double tok_best = 0.0;
    const std::vector<std::string> q_toks = split_words(q_norm);
    const std::vector<std::string> c_toks = split_words(c_norm);
    if (q_toks.size() == 1 && c_toks.size() > 1) {
        for (const std::string& ct : c_toks) {
            if (q_toks[0] == ct) {
                tok_best = 0.96;
                break;
            }
            tok_best = std::max(tok_best, jaro_winkler_similarity(q_toks[0], ct) * 0.93);
        }
    }

    double phon_sim = 0.0;
    if (!q_phon.empty() && !c_phon.empty()) {
        if (q_phon == c_phon) {
            phon_sim = 0.95;
        } else {
            const int dist = edit_distance(q_phon, c_phon);
            const int denom = static_cast<int>(std::max(q_phon.size(), c_phon.size()));
            phon_sim = std::max(0.0, 1.0 - static_cast<double>(dist) / std::max(1, denom)) * 0.92;
        }
    }
    return std::max({jw, tok_best, phon_sim});
}

double salience_bonus_for(const CommandCandidate& cand,
                          const std::string& recent_text,
                          const SoulRecord& record) {
    double bonus = 0.0;
    const std::string gn = command_normalize(cand.given_name);
    const std::string fn = command_normalize(cand.full_name);

    // 1. Named in current dialogue history (+0.11)
    if (!recent_text.empty()) {
        if ((!fn.empty() && recent_text.find(fn) != std::string::npos) ||
            (gn.size() >= 3 && recent_text.find(" " + gn + " ") != std::string::npos)) {
            bonus += 0.11;
        }
    }

    // 2. Active/recorded quest participant (+0.08 when active, +0.04 when done)
    if (!cand.quest_ids.empty()) {
        for (const auto& [qid, qstage] : record.quests) {
            if (std::find(cand.quest_ids.begin(), cand.quest_ids.end(), qid) != cand.quest_ids.end()) {
                if (qstage == "accepted" || qstage == "handed") {
                    bonus += 0.08;
                    break;
                }
                if (qstage == "done") {
                    bonus += 0.04;
                    break;
                }
            }
        }
    }

    // 3. Previously met or `@heard` by Henry (+0.05)
    if ((!cand.person_token.empty() && record.knows(cand.person_token)) ||
        (!cand.soul_name.empty() && record.knows(cand.soul_name))) {
        bonus += 0.05;
    }

    // 4. Social closeness (Hop-1 > Hop-2)
    bonus += 0.05 * std::clamp(cand.closeness, 0.0, 1.0);
    return bonus;
}

const CommandCandidate* resolve_pronoun(std::string_view mention,
                                        const std::vector<CommandCandidate>& cands,
                                        const std::vector<Message>& history) {
    if (history.empty()) return nullptr;
    const std::string det = strip_determiners(mention);
    const std::string norm = command_normalize(mention);
    std::string want_gender;
    if (is_male_pronoun(det) || is_male_pronoun(norm)) want_gender = "male";
    else if (is_female_pronoun(det) || is_female_pronoun(norm)) want_gender = "female";

    std::unordered_map<std::string, std::pair<double, const CommandCandidate*>> best;
    const int total = static_cast<int>(history.size());
    for (int idx = 0; idx < total; ++idx) {
        const Message& msg = history[static_cast<std::size_t>(idx)];
        const std::string padded = " " + command_normalize(msg.content) + " ";
        const double recency = static_cast<double>(idx + 1) / std::max(1, total);
        const double role_w = (msg.role == "user") ? 2.2 : 1.0;

        for (const CommandCandidate& c : cands) {
            if (!want_gender.empty() && !c.gender.empty() && c.gender != want_gender) continue;
            std::string fn = command_normalize(c.full_name);
            std::string gn = command_normalize(c.given_name);
            std::size_t hit_pos = std::string::npos;
            double match_w = 0.0;
            if (!fn.empty()) {
                const std::size_t p = padded.find(" " + fn + " ");
                if (p != std::string::npos) {
                    hit_pos = p;
                    match_w = 1.25;
                }
            }
            if (hit_pos == std::string::npos && gn.size() >= 3) {
                const std::size_t p = padded.find(" " + gn + " ");
                if (p != std::string::npos) {
                    hit_pos = p;
                    match_w = 1.0;
                }
            }
            if (hit_pos != std::string::npos) {
                const double subj_bonus = (hit_pos <= 4) ? 0.65 : (hit_pos <= 24 ? 0.25 : 0.0);
                const double sc = role_w * match_w * (1.0 + 0.5 * recency) + subj_bonus + 0.05 * c.closeness;
                const std::string key = c.soul_name.empty() ? ("abs:" + c.full_name) : c.soul_name;
                auto it = best.find(key);
                if (it == best.end() || sc > it->second.first) {
                    best[key] = {sc, &c};
                }
            }
        }
    }
    const CommandCandidate* winner = nullptr;
    double best_sc = -1.0;
    for (const auto& [k, pair] : best) {
        (void)k;
        if (pair.first > best_sc) {
            best_sc = pair.first;
            winner = pair.second;
        }
    }
    return winner;
}

std::string col_str(sqlite3_stmt* st, int col) {
    const unsigned char* p = sqlite3_column_text(st, col);
    return p ? std::string(reinterpret_cast<const char*>(p)) : std::string();
}

std::string make_person_token(std::string_view id_or_name) {
    std::string out;
    for (char c : id_or_name) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        } else if (c == ' ' || c == '-') {
            out.push_back('_');
        }
    }
    return out;
}

}  // namespace

std::string command_normalize(std::string_view text) {
    const std::string folded = fold_utf8_diacritics(text);
    std::string cleaned;
    cleaned.reserve(folded.size());
    for (char c : folded) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc)) {
            cleaned.push_back(static_cast<char>(std::tolower(uc)));
        } else if (c == '\'' || c == '-') {
            // drop apostrophe / hyphen inside words or treat hyphen as space
            if (c == '-') cleaned.push_back(' ');
        } else {
            cleaned.push_back(' ');
        }
    }
    return join_words(split_words(cleaned));
}

std::string command_phonetic_key(std::string_view word) {
    std::string w = command_normalize(word);
    w.erase(std::remove(w.begin(), w.end(), ' '), w.end());
    if (w.empty()) return {};

    static const std::pair<std::string_view, std::string_view> rules[] = {
        {"sch", "s"}, {"cz", "s"}, {"cs", "s"}, {"sz", "s"}, {"zs", "s"},
        {"rz", "r"},  {"ch", "x"}, {"ph", "f"}, {"th", "t"}, {"ck", "k"},
        {"qu", "kv"}, {"dt", "t"}, {"tz", "s"}, {"gh", "g"}, {"kn", "n"},
        {"wr", "r"},
    };
    for (const auto& [pat, rep] : rules) {
        std::size_t pos = 0;
        while ((pos = w.find(pat, pos)) != std::string::npos) {
            w.replace(pos, pat.size(), rep);
            pos += rep.size();
        }
    }
    if (!w.empty() && (w[0] == 'j' || w[0] == 'y')) w[0] = 'i';

    std::string mapped;
    mapped.reserve(w.size());
    for (std::size_t i = 0; i < w.size(); ++i) {
        char c = w[i];
        switch (c) {
            case 'w': mapped.push_back('v'); break;
            case 'q':
            case 'k': mapped.push_back('k'); break;
            case 'c': {
                const char nxt = (i + 1 < w.size()) ? w[i + 1] : '\0';
                mapped.push_back((nxt == 'e' || nxt == 'i' || nxt == 'y') ? 's' : 'k');
                break;
            }
            case 'z': mapped.push_back('s'); break;
            case 'd': mapped.push_back('t'); break;
            case 'b': mapped.push_back('p'); break;
            case 'g': mapped.push_back('k'); break;
            case 'h':
                if (i == 0) mapped.push_back('h');
                break;
            default: mapped.push_back(c); break;
        }
    }
    if (mapped.empty()) return {};

    std::string out(1, mapped[0]);
    for (std::size_t i = 1; i < mapped.size(); ++i) {
        const char c = mapped[i];
        if (c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u' || c == 'y') continue;
        if (c != out.back()) out.push_back(c);
    }
    if (out.size() > 8) out.resize(8);
    return out;
}

double jaro_winkler_similarity(std::string_view s1, std::string_view s2) {
    if (s1 == s2) return 1.0;
    const std::size_t len1 = s1.size();
    const std::size_t len2 = s2.size();
    if (len1 == 0 || len2 == 0) return 0.0;

    const std::size_t match_dist = (std::max(len1, len2) / 2 > 0) ? (std::max(len1, len2) / 2 - 1) : 0;
    std::vector<bool> s1_m(len1, false);
    std::vector<bool> s2_m(len2, false);

    std::size_t matches = 0;
    for (std::size_t i = 0; i < len1; ++i) {
        const std::size_t start = (i > match_dist) ? (i - match_dist) : 0;
        const std::size_t end = std::min(i + match_dist + 1, len2);
        for (std::size_t j = start; j < end; ++j) {
            if (s2_m[j] || s1[i] != s2[j]) continue;
            s1_m[i] = true;
            s2_m[j] = true;
            ++matches;
            break;
        }
    }
    if (matches == 0) return 0.0;

    std::size_t k = 0;
    std::size_t transpositions = 0;
    for (std::size_t i = 0; i < len1; ++i) {
        if (!s1_m[i]) continue;
        while (!s2_m[k]) ++k;
        if (s1[i] != s2[k]) ++transpositions;
        ++k;
    }

    const double m = static_cast<double>(matches);
    const double jaro =
        (m / static_cast<double>(len1) + m / static_cast<double>(len2) +
         (m - static_cast<double>(transpositions) / 2.0) / m) /
        3.0;
    std::size_t prefix = 0;
    for (std::size_t i = 0; i < std::min<std::size_t>({4, len1, len2}); ++i) {
        if (s1[i] == s2[i]) ++prefix;
        else break;
    }
    return jaro + static_cast<double>(prefix) * 0.1 * (1.0 - jaro);
}

ExtractedCommand parse_extracted_command(std::string_view raw) {
    ExtractedCommand out;
    out.raw_output = std::string(raw);
    std::string text = trim_ws(raw);
    const std::size_t nl = text.find('\n');
    if (nl != std::string::npos) text = trim_ws(std::string_view(text).substr(0, nl));

    if (text.empty() || text == "NONE" || text == "NO_ACTION") return out;

    // FunctionGemma syntax: `call:locate_npc{target_mention:<escape>Margaret<escape>}`
    const std::size_t call_pos = text.find("call:");
    if (call_pos != std::string::npos) {
        const std::string_view rest = std::string_view(text).substr(call_pos + 5);
        if (rest.rfind("locate_npc", 0) == 0) out.action = CommandAction::LocateNpc;
        else if (rest.rfind("follow_npc", 0) == 0) out.action = CommandAction::FollowNpc;
        else if (rest.rfind("travel_to", 0) == 0) out.action = CommandAction::TravelTo;
        if (out.action != CommandAction::None) {
            const std::size_t tm = rest.find("target_mention:");
            if (tm != std::string::npos) {
                std::string span(rest.substr(tm + 15));
                const std::size_t end_brace = span.find('}');
                if (end_brace != std::string::npos) span.resize(end_brace);
                while (span.rfind("<escape>", 0) == 0) span.erase(0, 8);
                if (span.size() >= 8 && span.substr(span.size() - 8) == "<escape>") {
                    span.resize(span.size() - 8);
                }
                span = trim_ws(span);
                while (!span.empty() && (span.front() == '"' || span.front() == '\'')) span.erase(span.begin());
                while (!span.empty() && (span.back() == '"' || span.back() == '\'')) span.pop_back();
                out.target_mention = trim_ws(span);
                out.is_pronoun = is_pronoun_mention(out.target_mention);
            }
        }
        return out;
    }

    // Compact GBNF syntax: `LOCATE: Margaret` | `FOLLOW: Father Ernest` | `TRAVEL: Trosky Castle`
    const std::size_t colon = text.find(':');
    if (colon == std::string::npos) return out;
    std::string verb = trim_ws(std::string_view(text).substr(0, colon));
    for (char& c : verb) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    std::string span = trim_ws(std::string_view(text).substr(colon + 1));
    while (!span.empty() && (span.front() == '"' || span.front() == '\'')) span.erase(span.begin());
    while (!span.empty() && (span.back() == '"' || span.back() == '\'' || span.back() == '.')) span.pop_back();
    span = trim_ws(span);

    if (verb == "LOCATE" || verb == "LOCATE_NPC") out.action = CommandAction::LocateNpc;
    else if (verb == "FOLLOW" || verb == "FOLLOW_NPC") out.action = CommandAction::FollowNpc;
    else if (verb == "TRAVEL" || verb == "TRAVEL_TO") out.action = CommandAction::TravelTo;
    if (out.action != CommandAction::None) {
        out.target_mention = span;
        out.is_pronoun = is_pronoun_mention(span);
    }
    return out;
}

std::string format_locate_directive(std::string_view soul,
                                    const std::vector<RoutineSpot>& spots) {
    std::string payload;
    for (std::size_t i = 0; i < spots.size(); ++i) {
        if (i > 0) payload.push_back(';');
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%d,%d,%.1f,%.1f,%.1f",
                      spots[i].start_min, spots[i].end_min,
                      spots[i].x, spots[i].y, spots[i].z);
        payload += buf;
    }
    return "@locate " + std::string(soul) + " " + std::to_string(payload.size()) + ":" + payload;
}

std::string clean_guard_subject_mention(std::string_view raw) {
    auto trim_punct = [](std::string_view s) {
        std::size_t a = 0;
        std::size_t b = s.size();
        while (a < b && (std::isspace(static_cast<unsigned char>(s[a])) ||
                         s[a] == '"' || s[a] == '\'' || s[a] == '?' || s[a] == '!' ||
                         s[a] == '.' || s[a] == ',' || s[a] == ':' || s[a] == ';')) {
            ++a;
        }
        while (b > a && (std::isspace(static_cast<unsigned char>(s[b - 1])) ||
                         s[b - 1] == '"' || s[b - 1] == '\'' || s[b - 1] == '?' ||
                         s[b - 1] == '!' || s[b - 1] == '.' || s[b - 1] == ',' ||
                         s[b - 1] == ':' || s[b - 1] == ';')) {
            --b;
        }
        return std::string(s.substr(a, b - a));
    };

    const std::string fallback = trim_punct(raw);
    std::string cur = fallback;
    if (cur.empty()) return cur;

    static const char* const kPrefixes[] = {
        "can you tell me where ",
        "could you tell me where ",
        "do you know where ",
        "can you help me find ",
        "could you help me find ",
        "help me find ",
        "where can i find ",
        "where could i find ",
        "where might i find ",
        "i am looking for ",
        "i'm looking for ",
        "im looking for ",
        "looking for ",
        "i am searching for ",
        "i'm searching for ",
        "searching for ",
        "i need to find ",
        "i want to find ",
        "have you seen ",
        "where is ",
        "where's ",
        "wheres ",
        "find me ",
        "find ",
        "it is ",
        "it's ",
        "its ",
    };
    bool changed = true;
    while (changed && !cur.empty()) {
        changed = false;
        std::string low = cur;
        for (char& ch : low) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        for (const char* pref : kPrefixes) {
            const std::size_t plen = std::strlen(pref);
            if (low.size() > plen && low.compare(0, plen, pref) == 0) {
                cur = trim_punct(std::string_view(cur).substr(plen));
                changed = true;
                break;
            }
        }
    }

    static const char* const kSuffixes[] = {
        " right now",
        " around here",
        " in this town",
        " in this village",
        " in town",
        " here",
        " please",
        " is",
    };
    changed = true;
    while (changed && !cur.empty()) {
        changed = false;
        std::string low = cur;
        for (char& ch : low) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        for (const char* suf : kSuffixes) {
            const std::size_t slen = std::strlen(suf);
            if (low.size() > slen && low.compare(low.size() - slen, slen, suf) == 0) {
                cur = trim_punct(std::string_view(cur).substr(0, cur.size() - slen));
                changed = true;
                break;
            }
        }
    }

    return cur.empty() ? fallback : cur;
}

std::optional<Stage1Gate> Stage1Gate::load_json(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) return std::nullopt;
    try {
        json j = json::parse(in);
        Stage1Gate g;
        g.dim_ = j.value("dim", 768);
        g.threshold_ = j.value("recommended_cmd_threshold", 0.65f);
        const auto& w = j.at("weights");
        const auto& b = j.at("bias");
        if (w.size() != 4 || b.size() != 4) return std::nullopt;
        g.weights_.reserve(static_cast<std::size_t>(4 * g.dim_));
        for (std::size_t c = 0; c < 4; ++c) {
            if (static_cast<int>(w[c].size()) != g.dim_) return std::nullopt;
            for (int k = 0; k < g.dim_; ++k) {
                g.weights_.push_back(w[c][static_cast<std::size_t>(k)].get<float>());
            }
            g.bias_.push_back(b[c].get<float>());
        }
        return g;
    } catch (...) {
        return std::nullopt;
    }
}

float Stage1Gate::command_probability(const std::vector<float>& q) const {
    if (!loaded() || static_cast<int>(q.size()) < dim_) return 1.0f;
    float logits[4];
    float max_l = -1e30f;
    for (int c = 0; c < 4; ++c) {
        float acc = bias_[static_cast<std::size_t>(c)];
        const float* row = weights_.data() + c * dim_;
        for (int k = 0; k < dim_; ++k) {
            acc += row[k] * q[static_cast<std::size_t>(k)];
        }
        logits[c] = acc;
        if (acc > max_l) max_l = acc;
    }
    float sum_e = 0.0f;
    float e0 = std::exp(logits[0] - max_l);
    sum_e += e0;
    for (int c = 1; c < 4; ++c) sum_e += std::exp(logits[c] - max_l);
    return 1.0f - (e0 / sum_e);
}

bool Stage1Gate::should_run_extractor(const std::vector<float>& q) const {
    if (!loaded() || q.empty()) return true;
    return command_probability(q) >= threshold_;
}

CommandGraph::CommandGraph(std::filesystem::path db_path) : path_(std::move(db_path)) {
    if (path_.empty()) return;
    if (sqlite3_open_v2(path_.string().c_str(), &db_, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX,
                        nullptr) != SQLITE_OK) {
        trouble_ = db_ ? sqlite3_errmsg(db_) : "sqlite3_open_v2 failed";
        if (db_) {
            sqlite3_close(db_);
            db_ = nullptr;
        }
    }
}

CommandGraph::~CommandGraph() {
    if (db_) sqlite3_close(db_);
}

void CommandGraph::hold(std::string speaker_soul,
                        std::vector<CommandCandidate> candidates,
                        std::map<std::string, std::vector<RoutineSpot>> spots) {
    std::lock_guard<std::mutex> g(lock_);
    for (const CommandCandidate& c : candidates) {
        if (!c.soul_name.empty()) held_souls_[c.soul_name] = c;
    }
    held_[std::move(speaker_soul)] = std::move(candidates);
    for (auto& [k, v] : spots) held_spots_[k] = std::move(v);
}

void CommandGraph::hold_location(std::string location_id,
                                 std::vector<CommandCandidate> candidates,
                                 std::map<std::string, std::vector<RoutineSpot>> spots) {
    std::lock_guard<std::mutex> g(lock_);
    for (const CommandCandidate& c : candidates) {
        if (!c.soul_name.empty()) held_souls_[c.soul_name] = c;
    }
    held_locations_[std::move(location_id)] = std::move(candidates);
    for (auto& [k, v] : spots) held_spots_[k] = std::move(v);
}

std::vector<CommandCandidate> CommandGraph::candidates_for(std::string_view speaker_soul) {
    std::lock_guard<std::mutex> g(lock_);
    if (auto it = held_.find(speaker_soul); it != held_.end()) return it->second;
    if (auto it = cache_.find(speaker_soul); it != cache_.end()) return it->second;
    if (!db_ || speaker_soul.empty()) return {};
    std::vector<CommandCandidate> built = query_db_candidates(speaker_soul);
    cache_.emplace(std::string(speaker_soul), built);
    return built;
}

std::vector<CommandCandidate> CommandGraph::candidates_in_location(std::string_view location_id) {
    std::lock_guard<std::mutex> g(lock_);
    if (auto it = held_locations_.find(location_id); it != held_locations_.end()) return it->second;
    if (auto it = location_cache_.find(location_id); it != location_cache_.end()) return it->second;
    if (!db_ || location_id.empty()) return {};
    std::vector<CommandCandidate> built = query_db_location_candidates(location_id);
    location_cache_.emplace(std::string(location_id), built);
    return built;
}

std::optional<CommandCandidate> CommandGraph::candidate_by_soul(std::string_view soul) {
    std::lock_guard<std::mutex> g(lock_);
    if (soul.empty()) return std::nullopt;
    if (auto it = held_souls_.find(soul); it != held_souls_.end()) return it->second;
    if (auto it = soul_cache_.find(soul); it != soul_cache_.end()) return it->second;
    if (!db_) return std::nullopt;
    std::optional<CommandCandidate> built = query_db_single_candidate(soul);
    if (built.has_value()) {
        soul_cache_.emplace(std::string(soul), *built);
    }
    return built;
}

std::vector<CommandCandidate> CommandGraph::candidates_for_guard(
    std::string_view location_id,
    const GuardPresence* guard_presence) {
    std::vector<CommandCandidate> base = candidates_in_location(location_id);
    if (!guard_presence || !guard_presence->has_live_scan) return base;

    std::vector<CommandCandidate> out;
    out.reserve(base.size() + guard_presence->live_souls.size());
    std::set<std::string, std::less<>> seen;
    for (CommandCandidate& c : base) {
        if (c.is_traveling &&
            guard_presence->live_souls.find(c.soul_name) == guard_presence->live_souls.end()) {
            continue;
        }
        seen.insert(c.soul_name);
        out.push_back(std::move(c));
    }
    for (const auto& [soul, live_spot] : guard_presence->live_souls) {
        (void)live_spot;
        if (seen.find(soul) != seen.end()) continue;
        if (std::optional<CommandCandidate> extra = candidate_by_soul(soul)) {
            seen.insert(extra->soul_name);
            out.push_back(std::move(*extra));
        }
    }
    return out;
}

std::vector<RoutineSpot> CommandGraph::spots_of(std::string_view target_soul) {
    std::lock_guard<std::mutex> g(lock_);
    if (auto it = held_spots_.find(target_soul); it != held_spots_.end()) return it->second;
    if (!db_ || target_soul.empty()) return {};
    return query_db_spots(target_soul);
}

std::vector<RoutineSpot> CommandGraph::spots_of_in_location(
    std::string_view target_soul,
    std::string_view location_id,
    const GuardPresence* guard_presence) {
    std::vector<RoutineSpot> out;
    if (guard_presence) {
        if (auto it = guard_presence->live_souls.find(target_soul);
            it != guard_presence->live_souls.end()) {
            out.push_back({0, 1440, it->second.x, it->second.y, it->second.z});
        }
    }
    std::vector<RoutineSpot> db_spots = spots_of(target_soul);
    if (location_id.empty()) {
        for (const RoutineSpot& sp : db_spots) out.push_back(sp);
        return out;
    }
    std::vector<const kcd2_settlements::SettlementPolygon*> polys;
    for (std::size_t i = 0; i < kcd2_settlements::kSettlementPolygonCount; ++i) {
        if (location_id == kcd2_settlements::kSettlementPolygons[i].pop_center_id) {
            polys.push_back(&kcd2_settlements::kSettlementPolygons[i]);
        }
    }
    for (const RoutineSpot& sp : db_spots) {
        bool in_loc = polys.empty();
        for (const auto* poly : polys) {
            if (kcd2_settlements::point_in_polygon(sp.x, sp.y, *poly)) {
                in_loc = true;
                break;
            }
        }
        if (in_loc) {
            out.push_back(sp);
            if (out.size() >= 8) break;
        }
    }
    if (out.empty()) return db_spots;
    return out;
}

SoulWhereabouts CommandGraph::whereabouts_of(std::string_view target_soul) {
    std::lock_guard<std::mutex> g(lock_);
    if (!db_ || target_soul.empty()) return {};
    return query_db_whereabouts(target_soul);
}

namespace {

std::string take_sentences(std::string_view text, std::size_t max_sentences) {
    const std::string s = trim_ws(text);
    if (s.empty() || max_sentences == 0) return {};
    std::size_t count = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char ch = s[i];
        if ((ch == '.' || ch == '!' || ch == '?') &&
            (i + 1 == s.size() || std::isspace(static_cast<unsigned char>(s[i + 1])))) {
            ++count;
            if (count >= max_sentences) {
                return trim_ws(std::string_view(s).substr(0, i + 1));
            }
        }
    }
    return s;
}

std::vector<std::string> split_raw_words(std::string_view s) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        const std::size_t start = i;
        while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        if (i > start) out.emplace_back(s.substr(start, i - start));
    }
    return out;
}

bool is_leading_name_honorific(std::string_view norm_word) {
    static const std::unordered_set<std::string_view> kHonorifics = {
        "master",      "bailiff",     "innkeeper",    "blacksmith", "miller",
        "captain",     "father",      "sir",          "lord",       "lady",
        "knight",      "chamberlain", "abbot",        "rabbi",      "confessor",
        "monk",        "novice",      "brother",      "sister",     "executioner",
        "miner",       "butcher",     "alchemist",    "wagoner",    "mercenary",
        "armiger",     "adjutant",    "guardsman",    "cuman",      "margrave",
        "baron",       "hetman",      "ritterbruder", "doctor",
    };
    return kHonorifics.count(norm_word) != 0;
}

void populate_canonical_name_parts(CommandCandidate& c, std::string_view occupation = {}) {
    if (c.full_name.empty()) return;
    if (c.given_name.empty()) {
        const std::vector<std::string> raw = split_raw_words(c.full_name);
        if (!raw.empty()) {
            auto is_possessive = [](std::string_view w) {
                return (w.size() >= 3 && w.substr(w.size() - 2) == "'s") ||
                       (w.size() >= 4 && w.substr(w.size() - 4) == "\xe2\x80\x99s");
            };
            bool has_any_possessive = false;
            for (const auto& w : raw) {
                if (is_possessive(w)) {
                    has_any_possessive = true;
                    break;
                }
            }
            std::size_t idx = 0;
            bool skip_decomp = false;
            if (has_any_possessive) {
                // Possessive relationship labels like "Radovan's wife Milena", "Kreyzl's wife Milada",
                // "Mikush's Vendula", vs pure role labels like "Halama's wife" / "Vidlak fisherman's wife".
                if (is_possessive(raw[0]) && raw.size() >= 3 &&
                    std::islower(static_cast<unsigned char>(raw[1][0])) &&
                    std::isupper(static_cast<unsigned char>(raw[2][0]))) {
                    if (c.title.empty()) c.title = raw[0] + " " + raw[1];
                    idx = 2;
                } else if (is_possessive(raw[0]) && raw.size() == 2 &&
                           std::isupper(static_cast<unsigned char>(raw[1][0])) &&
                           command_normalize(raw[1]) != "man" &&
                           command_normalize(raw[1]) != "guard") {
                    if (c.title.empty()) c.title = raw[0];
                    idx = 1;
                } else {
                    skip_decomp = true;
                }
            } else {
                const std::string w0 = command_normalize(raw[0]);
                if (raw.size() >= 2 && c.title.empty() && is_leading_name_honorific(w0)) {
                    c.title = raw[0];
                    idx = 1;
                }
            }
            if (!skip_decomp && idx < raw.size()) {
                c.given_name = raw[idx];
                if (idx + 1 < raw.size()) {
                    std::string tail;
                    for (std::size_t k = idx + 1; k < raw.size(); ++k) {
                        if (!tail.empty()) tail.push_back(' ');
                        tail += raw[k];
                    }
                    if (c.byname.empty()) c.byname = tail;

                    const std::string last_norm = command_normalize(raw.back());
                    if (last_norm.size() >= 3 && last_norm != "ii" && last_norm != "iii" &&
                        last_norm != "iv") {
                        c.extra_aliases.push_back(raw.back());
                        if (!c.title.empty()) {
                            c.extra_aliases.push_back(c.title + " " + raw.back());
                        }
                    }
                    for (std::size_t k = idx + 1; k + 1 < raw.size(); ++k) {
                        const std::string prep = command_normalize(raw[k]);
                        if (prep == "of" || prep == "von") {
                            if (k > idx + 1) {
                                std::string pre;
                                for (std::size_t j = idx; j < k; ++j) {
                                    if (!pre.empty()) pre.push_back(' ');
                                    pre += raw[j];
                                }
                                c.extra_aliases.push_back(pre);
                            }
                            std::string post;
                            for (std::size_t j = k + 1; j < raw.size(); ++j) {
                                if (!post.empty()) post.push_back(' ');
                                post += raw[j];
                            }
                            if (command_normalize(post).size() >= 3) {
                                c.extra_aliases.push_back(post);
                            }
                            break;
                        }
                    }
                }
            }
        }
    }

    const std::string occ = command_normalize(occupation.empty() ? std::string_view(c.trade) : occupation);
    if (c.is_game_named && !c.given_name.empty()) {
        if (occ.find("priest") != std::string::npos) {
            c.extra_aliases.push_back("Father " + c.given_name);
        }
        if (occ.find("noble") != std::string::npos) {
            if (c.gender == "female" || occ.find("woman") != std::string::npos) {
                c.extra_aliases.push_back("Lady " + c.given_name);
                c.extra_aliases.push_back("Lady " + c.full_name);
            } else {
                c.extra_aliases.push_back("Sir " + c.given_name);
                c.extra_aliases.push_back("Sir " + c.full_name);
                c.extra_aliases.push_back("Lord " + c.given_name);
                const std::vector<std::string> rw = split_raw_words(c.full_name);
                if (rw.size() >= 2 && command_normalize(rw.back()).size() >= 3) {
                    c.extra_aliases.push_back("Lord " + rw.back());
                }
            }
        }
        if (occ.find("hetman") != std::string::npos || occ.find("soldier") != std::string::npos) {
            c.extra_aliases.push_back("Captain " + c.given_name);
            const std::vector<std::string> rw = split_raw_words(c.full_name);
            if (rw.size() >= 2 && command_normalize(rw.back()).size() >= 3) {
                c.extra_aliases.push_back("Captain " + rw.back());
            }
        }
    }
}

std::string sanitize_settlement_label(std::string_view raw) {
    static const std::unordered_set<std::string_view> kInternalGroups = {
        "new buddies", "old buddies", "katerina", "ptacek", "bohuta", "zizka",
        "racek", "hanus", "henry", "father martin", "certovka", "bohounovice",
        "hermits", "executioners", "farms", "fast travel nomads camp",
    };
    const std::string low = command_normalize(raw);
    if (low.empty() || kInternalGroups.count(low) != 0) return {};
    return std::string(raw);
}

std::string compass_bearing_phrase(double gx, double gy, double tx, double ty) {
    if (gx < 0.0 || gy < 0.0) return {};
    const double dx = tx - gx;
    const double dy = ty - gy;
    const double dist = std::hypot(dx, dy);
    if (dist < 18.0) return "right nearby";
    // CryEngine world space: +X is East, +Y is North.
    const double deg = std::atan2(dy, dx) * (180.0 / 3.14159265358979323846);
    const char* dir = "to the east";
    if (deg >= -22.5 && deg < 22.5) dir = "to the east";
    else if (deg >= 22.5 && deg < 67.5) dir = "to the northeast";
    else if (deg >= 67.5 && deg < 112.5) dir = "to the north";
    else if (deg >= 112.5 && deg < 157.5) dir = "to the northwest";
    else if (deg >= 157.5 || deg < -157.5) dir = "to the west";
    else if (deg >= -157.5 && deg < -112.5) dir = "to the southwest";
    else if (deg >= -112.5 && deg < -67.5) dir = "to the south";
    else dir = "to the southeast";
    if (dist < 75.0) return std::string("just ") + dir + " of here";
    return std::string(dir) + " of here";
}

}  // namespace

std::vector<CommandCandidate> CommandGraph::query_db_candidates(std::string_view speaker_soul) const {
    std::unordered_map<std::string, CommandCandidate> by_soul;
    std::vector<CommandCandidate> absentees;

    auto fetch_soul_meta = [&](const std::string& soul, int hop, double closeness,
                               const std::string& via_name = {}) -> CommandCandidate& {
        auto it = by_soul.find(soul);
        if (it != by_soul.end()) {
            if (hop < it->second.hop) {
                it->second.hop = hop;
                it->second.via_name = (hop <= 1) ? std::string() : via_name;
            } else if (it->second.hop > 1 && it->second.via_name.empty() && !via_name.empty()) {
                it->second.via_name = via_name;
            }
            it->second.closeness = std::max(it->second.closeness, closeness);
            return it->second;
        }
        CommandCandidate c;
        c.soul_name = soul;
        c.hop = hop;
        c.closeness = closeness;
        if (hop > 1) c.via_name = via_name;
        c.person_token = make_person_token(soul);

        sqlite3_stmt* st = nullptr;
        const char* sql =
            "SELECT COALESCE(g.full_name, s.full_name, s.ui_name, n.display_name, n.soul_name), "
            "COALESCE(g.given, ''), COALESCE(g.title, ''), COALESCE(g.byname, ''), "
            "COALESCE(s.ui_name, n.display_name, ''), LOWER(COALESCE(n.gender, '')), "
            "COALESCE(n.settlement, ''), COALESCE(t.trade, n.occupation, '') "
            "FROM npc n "
            "LEFT JOIN generated_name g ON g.soul_id = n.soul_id "
            "LEFT JOIN skald_character s ON s.skald_character_name = n.skald_character_name "
            "LEFT JOIN npc_trade t ON t.soul_name = n.soul_name "
            "WHERE n.soul_name = ?1";
        if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, soul.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(st) == SQLITE_ROW) {
                c.full_name = col_str(st, 0);
                c.given_name = col_str(st, 1);
                c.title = col_str(st, 2);
                c.byname = col_str(st, 3);
                c.ui_name = col_str(st, 4);
                c.gender = col_str(st, 5);
                c.settlement = col_str(st, 6);
                c.trade = col_str(st, 7);
            }
            sqlite3_finalize(st);
        }
        if (sqlite3_prepare_v2(
                db_, "SELECT quest_id FROM npc_quest WHERE giver_soul = ?1 OR target_soul = ?1",
                -1, &st, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, soul.c_str(), -1, SQLITE_TRANSIENT);
            while (sqlite3_step(st) == SQLITE_ROW) {
                const std::string qid = col_str(st, 0);
                if (!qid.empty()) c.quest_ids.push_back(qid);
            }
            sqlite3_finalize(st);
        }
        return by_soul.emplace(soul, std::move(c)).first->second;
    };

    // 1. Speaker's circles -> Hop-1 co-members + Hop-1 absentees + Hop-2 bridges
    std::vector<std::string> circles;
    std::vector<std::string> bridge_jsons;
    {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db_, "SELECT circle_id, COALESCE(bridges, '') FROM npc_circle_member WHERE soul_name = ?1",
                               -1, &st, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, speaker_soul.data(), static_cast<int>(speaker_soul.size()), SQLITE_TRANSIENT);
            while (sqlite3_step(st) == SQLITE_ROW) {
                circles.push_back(col_str(st, 0));
                bridge_jsons.push_back(col_str(st, 1));
            }
            sqlite3_finalize(st);
        }
    }

    for (const std::string& cid : circles) {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db_, "SELECT soul_name, COALESCE(bridges, '') FROM npc_circle_member WHERE circle_id = ?1",
                               -1, &st, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, cid.c_str(), -1, SQLITE_TRANSIENT);
            while (sqlite3_step(st) == SQLITE_ROW) {
                const std::string peer = col_str(st, 0);
                if (peer.empty() || peer == speaker_soul) continue;
                fetch_soul_meta(peer, 1, 0.85);
                const std::string bj = col_str(st, 1);
                if (!bj.empty()) bridge_jsons.push_back(bj);
            }
            sqlite3_finalize(st);
        }

        // Circle absentees (`npc_absentee`: circle_id, name, station, why_unmeetable)
        if (sqlite3_prepare_v2(
                db_,
                "SELECT name, COALESCE(station, ''), COALESCE(why_unmeetable, '') "
                "FROM npc_absentee WHERE circle_id = ?1",
                -1, &st, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, cid.c_str(), -1, SQLITE_TRANSIENT);
            while (sqlite3_step(st) == SQLITE_ROW) {
                CommandCandidate ab;
                ab.full_name = col_str(st, 0);
                const std::vector<std::string> w = split_words(ab.full_name);
                ab.given_name = w.empty() ? ab.full_name : w.front();
                const std::string stn = col_str(st, 1);
                if (!stn.empty()) ab.roles.push_back(stn);
                ab.why_unmeetable = col_str(st, 2);
                ab.hop = 1;
                ab.closeness = 0.80;
                ab.is_absentee = true;
                ab.person_token = make_person_token(ab.full_name);
                absentees.push_back(std::move(ab));
            }
            sqlite3_finalize(st);
        }
    }

    // 2. Hop-1 direct relationships (`npc_relationship`: soul_name, other_soul_name, relationship)
    std::vector<std::string> hop1_peers;
    {
        sqlite3_stmt* st = nullptr;
        const char* sql =
            "SELECT CASE WHEN soul_name = ?1 THEN other_soul_name ELSE soul_name END, "
            "COALESCE(relationship, '') "
            "FROM npc_relationship WHERE soul_name = ?1 OR other_soul_name = ?1";
        if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, speaker_soul.data(), static_cast<int>(speaker_soul.size()), SQLITE_TRANSIENT);
            while (sqlite3_step(st) == SQLITE_ROW) {
                const std::string peer = col_str(st, 0);
                const std::string rel = col_str(st, 1);
                if (peer.empty() || peer == speaker_soul) continue;
                CommandCandidate& c = fetch_soul_meta(peer, 1, 0.95);
                if (!rel.empty() && std::find(c.roles.begin(), c.roles.end(), rel) == c.roles.end()) {
                    c.roles.push_back(rel);
                }
                hop1_peers.push_back(peer);
            }
            sqlite3_finalize(st);
        }
    }

    // 2b. Co-located workplace/parish hub peers (`npc_routine.place_id` for intimate hubs <= 15 souls,
    // e.g. Father Ernest & Father Adam sharing the same Kuttenberg church hub).
    {
        sqlite3_stmt* st = nullptr;
        const char* sql =
            "SELECT DISTINCT r2.soul_name "
            "FROM npc_routine r1 "
            "JOIN npc_routine r2 ON r2.place_id = r1.place_id "
            "WHERE r1.soul_name = ?1 AND r1.place_id IS NOT NULL AND r1.place_id != '' "
            "AND r1.place_id NOT LIKE 'crowd:%' AND r1.place_id NOT LIKE 'tavern:%' "
            "AND (SELECT COUNT(DISTINCT r3.soul_name) FROM npc_routine r3 WHERE r3.place_id = r1.place_id) <= 15";
        if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, speaker_soul.data(), static_cast<int>(speaker_soul.size()), SQLITE_TRANSIENT);
            while (sqlite3_step(st) == SQLITE_ROW) {
                const std::string peer = col_str(st, 0);
                if (!peer.empty() && peer != speaker_soul) {
                    fetch_soul_meta(peer, 1, 0.82);
                }
            }
            sqlite3_finalize(st);
        }
    }

    for (const auto& [soul, c] : by_soul) {
        (void)c;
        if (std::find(hop1_peers.begin(), hop1_peers.end(), soul) == hop1_peers.end()) {
            hop1_peers.push_back(soul);
        }
    }

    // 3. Hop-2 bridges JSON (`["soul_a", ...]` or `[{"soul_name": "..."}]`)
    for (const std::string& bj : bridge_jsons) {
        if (bj.empty() || bj == "[]") continue;
        try {
            json arr = json::parse(bj);
            if (!arr.is_array()) continue;
            for (const auto& item : arr) {
                if (item.is_string()) {
                    const std::string s = item.get<std::string>();
                    if (!s.empty() && s != speaker_soul) {
                        fetch_soul_meta(s, 2, 0.55);
                        if (std::find(hop1_peers.begin(), hop1_peers.end(), s) == hop1_peers.end()) {
                            hop1_peers.push_back(s);
                        }
                    }
                } else if (item.is_object()) {
                    for (const char* k : {"soul_name", "soul", "target_soul", "other_soul"}) {
                        if (item.contains(k) && item[k].is_string()) {
                            const std::string s = item[k].get<std::string>();
                            if (!s.empty() && s != speaker_soul) {
                                fetch_soul_meta(s, 2, 0.55);
                                if (std::find(hop1_peers.begin(), hop1_peers.end(), s) == hop1_peers.end()) {
                                    hop1_peers.push_back(s);
                                }
                            }
                        }
                    }
                }
            }
        } catch (...) {
        }
    }

    // 4. Hop-2 adjacent circles + cross-circle relationships of Hop-1/bridge peers
    for (const std::string& h1 : hop1_peers) {
        std::string via;
        if (auto it = by_soul.find(h1); it != by_soul.end()) via = it->second.full_name;
        sqlite3_stmt* st = nullptr;
        const char* rel_sql =
            "SELECT CASE WHEN soul_name = ?1 THEN other_soul_name ELSE soul_name END "
            "FROM npc_relationship WHERE soul_name = ?1 OR other_soul_name = ?1";
        if (sqlite3_prepare_v2(db_, rel_sql, -1, &st, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, h1.c_str(), -1, SQLITE_TRANSIENT);
            while (sqlite3_step(st) == SQLITE_ROW) {
                const std::string p2 = col_str(st, 0);
                if (!p2.empty() && p2 != speaker_soul) fetch_soul_meta(p2, 2, 0.45, via);
            }
            sqlite3_finalize(st);
        }
        const char* circ_sql =
            "SELECT cm2.soul_name FROM npc_circle_member cm1 "
            "JOIN npc_circle_member cm2 ON cm2.circle_id = cm1.circle_id "
            "WHERE cm1.soul_name = ?1";
        if (sqlite3_prepare_v2(db_, circ_sql, -1, &st, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, h1.c_str(), -1, SQLITE_TRANSIENT);
            while (sqlite3_step(st) == SQLITE_ROW) {
                const std::string p2 = col_str(st, 0);
                if (!p2.empty() && p2 != speaker_soul) fetch_soul_meta(p2, 2, 0.50, via);
            }
            sqlite3_finalize(st);
        }
    }

    // 5. Hop-2 quest co-participants (`npc_quest`)
    {
        sqlite3_stmt* st = nullptr;
        const char* sql =
            "SELECT giver_soul, target_soul FROM npc_quest WHERE giver_soul = ?1 OR target_soul = ?1";
        if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, speaker_soul.data(), static_cast<int>(speaker_soul.size()), SQLITE_TRANSIENT);
            while (sqlite3_step(st) == SQLITE_ROW) {
                for (int col : {0, 1}) {
                    const std::string qpeer = col_str(st, col);
                    if (!qpeer.empty() && qpeer != speaker_soul) fetch_soul_meta(qpeer, 2, 0.60);
                }
            }
            sqlite3_finalize(st);
        }
    }

    std::vector<CommandCandidate> out;
    out.reserve(by_soul.size() + absentees.size());
    for (auto& [k, v] : by_soul) {
        (void)k;
        if (!v.full_name.empty()) out.push_back(std::move(v));
    }
    for (auto& ab : absentees) out.push_back(std::move(ab));
    return out;
}

std::vector<CommandCandidate> CommandGraph::query_db_location_candidates(
    std::string_view location_id) const {
    std::vector<const kcd2_settlements::SettlementPolygon*> polys;
    double min_x = 1e18, max_x = -1e18, min_y = 1e18, max_y = -1e18;
    for (std::size_t i = 0; i < kcd2_settlements::kSettlementPolygonCount; ++i) {
        const auto& p = kcd2_settlements::kSettlementPolygons[i];
        if (location_id == p.pop_center_id) {
            polys.push_back(&p);
            for (std::size_t v = 0; v < p.vertex_count; ++v) {
                min_x = std::min(min_x, static_cast<double>(p.vertices[v].x));
                max_x = std::max(max_x, static_cast<double>(p.vertices[v].x));
                min_y = std::min(min_y, static_cast<double>(p.vertices[v].y));
                max_y = std::max(max_y, static_cast<double>(p.vertices[v].y));
            }
        }
    }
    if (polys.empty()) return {};
    const char* level = polys.front()->level;

    auto inside_any = [&](double x, double y) {
        if (x < min_x || x > max_x || y < min_y || y > max_y) return false;
        for (const auto* poly : polys) {
            if (kcd2_settlements::point_in_polygon(x, y, *poly)) return true;
        }
        return false;
    };

    std::set<std::string> souls;
    auto collect_bbox = [&](const char* sql) {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return;
        sqlite3_bind_text(st, 1, level, -1, SQLITE_STATIC);
        sqlite3_bind_double(st, 2, min_x);
        sqlite3_bind_double(st, 3, max_x);
        sqlite3_bind_double(st, 4, min_y);
        sqlite3_bind_double(st, 5, max_y);
        while (sqlite3_step(st) == SQLITE_ROW) {
            const std::string s = col_str(st, 0);
            if (s.empty() || souls.count(s) != 0) continue;
            if (inside_any(sqlite3_column_double(st, 1), sqlite3_column_double(st, 2))) {
                souls.insert(s);
            }
        }
        sqlite3_finalize(st);
    };
    collect_bbox(
        "SELECT soul_name, dump_x, dump_y FROM npc_routine_hub "
        "WHERE level = ?1 AND dump_x BETWEEN ?2 AND ?3 AND dump_y BETWEEN ?4 AND ?5");
    collect_bbox(
        "SELECT soul_name, pos_x, pos_y FROM npc_routine "
        "WHERE level = ?1 AND pos_x BETWEEN ?2 AND ?3 AND pos_y BETWEEN ?4 AND ?5");
    {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(
                db_,
                "SELECT m.soul_name, p.pos FROM social_membership m "
                "JOIN social_place p USING (place_id) WHERE p.level = ?1 AND p.pos IS NOT NULL",
                -1, &st, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, level, -1, SQLITE_STATIC);
            while (sqlite3_step(st) == SQLITE_ROW) {
                const std::string s = col_str(st, 0);
                if (s.empty() || souls.count(s) != 0) continue;
                const std::string ps = col_str(st, 1);
                double px = 0.0, py = 0.0;
                if (std::sscanf(ps.c_str(), "%lf,%lf", &px, &py) == 2 && inside_any(px, py)) {
                    souls.insert(s);
                }
            }
            sqlite3_finalize(st);
        }
    }
    for (std::size_t i = 0; i < kcd2_settlements::kLevelSpawnFallbackCount; ++i) {
        const auto& sp = kcd2_settlements::kLevelSpawnFallbacks[i];
        if (std::strcmp(sp.level, level) == 0 && inside_any(sp.x, sp.y)) {
            souls.insert(sp.soul_name);
        }
    }

    const bool has_skald =
        sqlite3_table_column_metadata(db_, nullptr, "skald_character", "skald_character_name",
                                      nullptr, nullptr, nullptr, nullptr, nullptr) == SQLITE_OK;
    const bool has_trade =
        sqlite3_table_column_metadata(db_, nullptr, "npc_trade", "trade",
                                      nullptr, nullptr, nullptr, nullptr, nullptr) == SQLITE_OK;
    const bool has_kind =
        sqlite3_table_column_metadata(db_, nullptr, "npc", "kind",
                                      nullptr, nullptr, nullptr, nullptr, nullptr) == SQLITE_OK;
    const bool has_faction_cat =
        sqlite3_table_column_metadata(db_, nullptr, "npc", "faction_category",
                                      nullptr, nullptr, nullptr, nullptr, nullptr) == SQLITE_OK;
    const bool has_social_class =
        sqlite3_table_column_metadata(db_, nullptr, "npc", "social_class",
                                      nullptr, nullptr, nullptr, nullptr, nullptr) == SQLITE_OK;
    const bool has_is_named =
        sqlite3_table_column_metadata(db_, nullptr, "npc", "is_named",
                                      nullptr, nullptr, nullptr, nullptr, nullptr) == SQLITE_OK;
    const bool has_is_unique =
        sqlite3_table_column_metadata(db_, nullptr, "npc", "is_unique",
                                      nullptr, nullptr, nullptr, nullptr, nullptr) == SQLITE_OK;
    const bool has_vip_class =
        sqlite3_table_column_metadata(db_, nullptr, "npc", "vip_class",
                                      nullptr, nullptr, nullptr, nullptr, nullptr) == SQLITE_OK;
    const bool has_region =
        sqlite3_table_column_metadata(db_, nullptr, "npc", "region",
                                      nullptr, nullptr, nullptr, nullptr, nullptr) == SQLITE_OK;
    const bool has_quest =
        sqlite3_table_column_metadata(db_, nullptr, "npc_quest", "quest_id",
                                      nullptr, nullptr, nullptr, nullptr, nullptr) == SQLITE_OK;

    std::string meta_sql =
        "SELECT COALESCE(NULLIF(g.full_name, ''), ";
    if (has_skald) meta_sql += "NULLIF(s.full_name, ''), NULLIF(s.ui_name, ''), ";
    meta_sql +=
        "NULLIF(n.display_name, ''), ''), "
        "COALESCE(g.given, ''), COALESCE(g.title, ''), COALESCE(g.byname, ''), "
        "COALESCE(";
    if (has_skald) meta_sql += "NULLIF(s.ui_name, ''), ";
    meta_sql +=
        "NULLIF(n.display_name, ''), ''), LOWER(COALESCE(n.gender, '')), "
        "COALESCE(n.settlement, ''), COALESCE(";
    if (has_trade) meta_sql += "NULLIF(t.trade, ''), ";
    meta_sql +=
        "NULLIF(n.occupation, ''), ''), "
        "COALESCE(n.occupation, ''), "
        "CASE WHEN g.soul_id IS NULL AND ";
    meta_sql += has_is_named ? "COALESCE(n.is_named, 0) = 1 " : "0 ";
    meta_sql += "THEN 1 ELSE 0 END, ";
    meta_sql += has_is_unique ? "COALESCE(n.is_unique, 0), " : "0, ";
    meta_sql += has_vip_class ? "COALESCE(n.vip_class, 'none'), " : "'none', ";
    meta_sql += has_region ? "COALESCE(n.region, ''), " : "'', ";
    meta_sql += has_faction_cat ? "COALESCE(n.faction_category, '') " : "'' ";
    meta_sql +=
        "FROM npc n "
        "LEFT JOIN generated_name g ON g.soul_id = n.soul_id ";
    if (has_skald) {
        meta_sql += "LEFT JOIN skald_character s ON s.skald_character_name = n.skald_character_name ";
    }
    if (has_trade) {
        meta_sql += "LEFT JOIN npc_trade t ON t.soul_name = n.soul_name ";
    }
    meta_sql += "WHERE n.soul_name = ?1 "
                "AND NULLIF(COALESCE(n.soul_id, ''), '') IS NOT NULL "
                "AND LOWER(n.soul_name) NOT LIKE '%dead%' "
                "AND LOWER(n.soul_name) NOT LIKE '%corpse%' ";
    if (has_kind) meta_sql += "AND COALESCE(n.kind, 'human') = 'human' ";
    if (has_faction_cat) meta_sql += "AND COALESCE(n.faction_category, '') NOT IN ('enemies', 'badGuys', 'death') ";
    if (has_social_class) {
        meta_sql += has_is_named
                        ? "AND (COALESCE(n.is_named, 0) = 1 OR COALESCE(n.social_class, '') NOT IN ('bandit', 'renegade')) "
                        : "AND COALESCE(n.social_class, '') NOT IN ('bandit', 'renegade') ";
    }

    sqlite3_stmt* meta_st = nullptr;
    if (sqlite3_prepare_v2(db_, meta_sql.c_str(), -1, &meta_st, nullptr) != SQLITE_OK) return {};

    sqlite3_stmt* role_st = nullptr;
    sqlite3_prepare_v2(
        db_, "SELECT DISTINCT role FROM social_membership WHERE soul_name = ?1 AND role IS NOT NULL",
        -1, &role_st, nullptr);

    sqlite3_stmt* quest_st = nullptr;
    if (has_quest) {
        sqlite3_prepare_v2(
            db_, "SELECT quest_id FROM npc_quest WHERE giver_soul = ?1 OR target_soul = ?1",
            -1, &quest_st, nullptr);
    }

    std::vector<CommandCandidate> out;
    out.reserve(souls.size());
    for (const std::string& soul : souls) {
        if (!kcd2_settlements::is_permanent_settlement_soul(soul)) continue;
        sqlite3_reset(meta_st);
        sqlite3_bind_text(meta_st, 1, soul.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(meta_st) != SQLITE_ROW) continue;

        CommandCandidate c;
        c.soul_name = soul;
        c.full_name = col_str(meta_st, 0);
        if (c.full_name.empty()) continue;
        c.given_name = col_str(meta_st, 1);
        c.title = col_str(meta_st, 2);
        c.byname = col_str(meta_st, 3);
        c.ui_name = col_str(meta_st, 4);
        c.gender = col_str(meta_st, 5);
        c.settlement = col_str(meta_st, 6);
        c.trade = col_str(meta_st, 7);
        const std::string occ = col_str(meta_st, 8);
        c.is_game_named = sqlite3_column_int(meta_st, 9) != 0;
        const bool is_uniq = sqlite3_column_int(meta_st, 10) != 0;
        const std::string vip = col_str(meta_st, 11);
        const std::string region = col_str(meta_st, 12);
        const std::string fcat = col_str(meta_st, 13);
        c.is_major_friend = c.is_game_named &&
                            (region == "Player party" || fcat == "friends" || fcat == "allies" ||
                             is_uniq || (!vip.empty() && vip != "none"));
        c.is_traveling = c.is_game_named &&
                         (region == "Player party" || fcat == "friends" || fcat == "allies" ||
                          c.settlement == "new Buddies" || c.settlement == "old Buddies");
        c.hop = 1;
        c.closeness = 1.0;
        c.person_token = make_person_token(soul);

        populate_canonical_name_parts(c, occ);
        if (role_st) {
            sqlite3_reset(role_st);
            sqlite3_bind_text(role_st, 1, soul.c_str(), -1, SQLITE_TRANSIENT);
            while (sqlite3_step(role_st) == SQLITE_ROW) {
                const std::string r = col_str(role_st, 0);
                if (!r.empty() && r != "member" && r != "resident") c.roles.push_back(r);
            }
        }
        if (quest_st) {
            sqlite3_reset(quest_st);
            sqlite3_bind_text(quest_st, 1, soul.c_str(), -1, SQLITE_TRANSIENT);
            while (sqlite3_step(quest_st) == SQLITE_ROW) {
                const std::string qid = col_str(quest_st, 0);
                if (!qid.empty()) c.quest_ids.push_back(qid);
            }
        }
        out.push_back(std::move(c));
    }
    sqlite3_finalize(meta_st);
    if (role_st) sqlite3_finalize(role_st);
    if (quest_st) sqlite3_finalize(quest_st);
    return out;
}

std::optional<CommandCandidate> CommandGraph::query_db_single_candidate(
    std::string_view soul) const {
    if (!db_ || soul.empty() || !kcd2_settlements::is_permanent_settlement_soul(soul)) {
        return std::nullopt;
    }
    const bool has_skald =
        sqlite3_table_column_metadata(db_, nullptr, "skald_character", "skald_character_name",
                                      nullptr, nullptr, nullptr, nullptr, nullptr) == SQLITE_OK;
    const bool has_trade =
        sqlite3_table_column_metadata(db_, nullptr, "npc_trade", "trade",
                                      nullptr, nullptr, nullptr, nullptr, nullptr) == SQLITE_OK;
    const bool has_kind =
        sqlite3_table_column_metadata(db_, nullptr, "npc", "kind",
                                      nullptr, nullptr, nullptr, nullptr, nullptr) == SQLITE_OK;
    const bool has_faction_cat =
        sqlite3_table_column_metadata(db_, nullptr, "npc", "faction_category",
                                      nullptr, nullptr, nullptr, nullptr, nullptr) == SQLITE_OK;
    const bool has_is_named =
        sqlite3_table_column_metadata(db_, nullptr, "npc", "is_named",
                                      nullptr, nullptr, nullptr, nullptr, nullptr) == SQLITE_OK;
    const bool has_is_unique =
        sqlite3_table_column_metadata(db_, nullptr, "npc", "is_unique",
                                      nullptr, nullptr, nullptr, nullptr, nullptr) == SQLITE_OK;
    const bool has_vip_class =
        sqlite3_table_column_metadata(db_, nullptr, "npc", "vip_class",
                                      nullptr, nullptr, nullptr, nullptr, nullptr) == SQLITE_OK;
    const bool has_region =
        sqlite3_table_column_metadata(db_, nullptr, "npc", "region",
                                      nullptr, nullptr, nullptr, nullptr, nullptr) == SQLITE_OK;

    std::string sql =
        "SELECT COALESCE(NULLIF(g.full_name, ''), ";
    if (has_skald) sql += "NULLIF(s.full_name, ''), NULLIF(s.ui_name, ''), ";
    sql +=
        "NULLIF(n.display_name, ''), ''), "
        "COALESCE(g.given, ''), COALESCE(g.title, ''), COALESCE(g.byname, ''), "
        "COALESCE(";
    if (has_skald) sql += "NULLIF(s.ui_name, ''), ";
    sql +=
        "NULLIF(n.display_name, ''), ''), LOWER(COALESCE(n.gender, '')), "
        "COALESCE(n.settlement, ''), COALESCE(";
    if (has_trade) sql += "NULLIF(t.trade, ''), ";
    sql +=
        "NULLIF(n.occupation, ''), ''), "
        "COALESCE(n.occupation, ''), "
        "CASE WHEN g.soul_id IS NULL AND ";
    sql += has_is_named ? "COALESCE(n.is_named, 0) = 1 " : "0 ";
    sql += "THEN 1 ELSE 0 END, ";
    sql += has_is_unique ? "COALESCE(n.is_unique, 0), " : "0, ";
    sql += has_vip_class ? "COALESCE(n.vip_class, 'none'), " : "'none', ";
    sql += has_region ? "COALESCE(n.region, ''), " : "'', ";
    sql += has_faction_cat ? "COALESCE(n.faction_category, '') " : "'' ";
    sql +=
        "FROM npc n "
        "LEFT JOIN generated_name g ON g.soul_id = n.soul_id ";
    if (has_skald) {
        sql += "LEFT JOIN skald_character s ON s.skald_character_name = n.skald_character_name ";
    }
    if (has_trade) {
        sql += "LEFT JOIN npc_trade t ON t.soul_name = n.soul_name ";
    }
    sql += "WHERE n.soul_name = ?1 "
           "AND NULLIF(COALESCE(n.soul_id, ''), '') IS NOT NULL "
           "AND LOWER(n.soul_name) NOT LIKE '%dead%' "
           "AND LOWER(n.soul_name) NOT LIKE '%corpse%' ";
    if (has_kind) sql += "AND COALESCE(n.kind, 'human') = 'human' ";
    if (has_faction_cat) sql += "AND COALESCE(n.faction_category, '') NOT IN ('enemies', 'badGuys', 'death') ";

    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) return std::nullopt;
    sqlite3_bind_text(st, 1, soul.data(), static_cast<int>(soul.size()), SQLITE_TRANSIENT);
    std::optional<CommandCandidate> res;
    if (sqlite3_step(st) == SQLITE_ROW) {
        CommandCandidate c;
        c.soul_name = std::string(soul);
        c.full_name = col_str(st, 0);
        if (!c.full_name.empty()) {
            c.given_name = col_str(st, 1);
            c.title = col_str(st, 2);
            c.byname = col_str(st, 3);
            c.ui_name = col_str(st, 4);
            c.gender = col_str(st, 5);
            c.settlement = col_str(st, 6);
            c.trade = col_str(st, 7);
            const std::string occ = col_str(st, 8);
            c.is_game_named = sqlite3_column_int(st, 9) != 0;
            const bool is_uniq = sqlite3_column_int(st, 10) != 0;
            const std::string vip = col_str(st, 11);
            const std::string region = col_str(st, 12);
            const std::string fcat = col_str(st, 13);
            c.is_major_friend = c.is_game_named &&
                                (region == "Player party" || fcat == "friends" || fcat == "allies" ||
                                 is_uniq || (!vip.empty() && vip != "none"));
            c.is_traveling = c.is_game_named &&
                             (region == "Player party" || fcat == "friends" || fcat == "allies" ||
                              c.settlement == "new Buddies" || c.settlement == "old Buddies");
            c.hop = 1;
            c.closeness = 1.0;
            c.person_token = make_person_token(soul);
            populate_canonical_name_parts(c, occ);
            res = std::move(c);
        }
    }
    sqlite3_finalize(st);
    return res;
}

std::vector<RoutineSpot> CommandGraph::query_db_spots(std::string_view target_soul) const {
    std::vector<RoutineSpot> spots;
    sqlite3_stmt* st = nullptr;
    const char* sql =
        "SELECT COALESCE(start_min, 0), COALESCE(end_min, 0), pos_x, pos_y, COALESCE(pos_z, 0.0) "
        "FROM npc_routine WHERE soul_name = ?1 AND pos_x IS NOT NULL AND pos_y IS NOT NULL "
        "ORDER BY is_default DESC, start_min ASC LIMIT 8";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, target_soul.data(), static_cast<int>(target_soul.size()), SQLITE_TRANSIENT);
        while (sqlite3_step(st) == SQLITE_ROW) {
            RoutineSpot sp;
            sp.start_min = sqlite3_column_int(st, 0);
            sp.end_min = sqlite3_column_int(st, 1);
            sp.x = sqlite3_column_double(st, 2);
            sp.y = sqlite3_column_double(st, 3);
            sp.z = sqlite3_column_double(st, 4);
            spots.push_back(sp);
        }
        sqlite3_finalize(st);
    }
    if (spots.empty()) {
        for (std::size_t i = 0; i < kcd2_settlements::kLevelSpawnFallbackCount; ++i) {
            const auto& fb = kcd2_settlements::kLevelSpawnFallbacks[i];
            if (target_soul == fb.soul_name) {
                spots.push_back({0, 1440, fb.x, fb.y, 0.0});
                break;
            }
        }
    }
    return spots;
}

SoulWhereabouts CommandGraph::query_db_whereabouts(std::string_view target_soul) const {
    SoulWhereabouts out;
    sqlite3_stmt* st = nullptr;
    const char* home_sql =
        "SELECT fact FROM npc_kb_item WHERE owner = ?1 AND item_id = 'life:home' AND trust = 'open'";
    if (sqlite3_prepare_v2(db_, home_sql, -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, target_soul.data(), static_cast<int>(target_soul.size()),
                          SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_ROW) {
            out.home_fact = take_sentences(col_str(st, 0), 2);
        }
        sqlite3_finalize(st);
    }
    const char* person_sql = "SELECT self_account FROM npc_person WHERE soul_name = ?1";
    if (sqlite3_prepare_v2(db_, person_sql, -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, target_soul.data(), static_cast<int>(target_soul.size()),
                          SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_ROW) {
            out.self_intro = take_sentences(col_str(st, 0), 1);
        }
        sqlite3_finalize(st);
    }
    return out;
}

CommandDirector::CommandDirector(Embedder* embedder,
                                 Llm* extractor_llm,
                                 CommandGraph& graph,
                                 Stage1Gate gate,
                                 Options extractor_options)
    : embedder_(embedder),
      extractor_(extractor_llm),
      graph_(graph),
      gate_(std::move(gate)),
      extractor_options_(std::move(extractor_options)) {
    if (extractor_options_.grammar.empty()) {
        extractor_options_.grammar = kCompactCommandGrammar;
    }
    extractor_options_.temperature = 0.0;
    extractor_options_.disable_thinking = true;
    if (extractor_options_.reply_tokens <= 0 || extractor_options_.reply_tokens > 48) {
        extractor_options_.reply_tokens = 32;
    }
}

std::string CommandDirector::pass1_keyterms(std::string_view speaker_soul,
                                            const std::vector<Message>& history,
                                            const SoulRecord& record,
                                            std::size_t max_terms) {
    std::vector<CommandCandidate> cands = graph_.candidates_for(speaker_soul);
    std::string recent = " ";
    for (const Message& m : history) recent += command_normalize(m.content) + " ";

    std::vector<std::pair<double, const CommandCandidate*>> ranked;
    for (const CommandCandidate& c : cands) {
        const double hop_prior = (c.hop <= 1) ? 1.0 : 0.0;
        const double quest_prior = c.quest_ids.empty() ? 0.0 : 0.02;
        ranked.push_back({hop_prior + quest_prior + salience_bonus_for(c, recent, record), &c});
    }
    std::sort(ranked.begin(), ranked.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });

    std::vector<std::string> terms;
    std::set<std::string> seen;
    for (const auto& [sc, c] : ranked) {
        (void)sc;
        const std::string clean = trim_ws(c->full_name);
        if (clean.size() < 3) continue;
        const std::string low = command_normalize(clean);
        if (seen.insert(low).second) {
            terms.push_back(clean);
            if (terms.size() >= max_terms) break;
        }
    }
    std::string csv;
    for (std::size_t i = 0; i < terms.size(); ++i) {
        if (i > 0) csv += ", ";
        csv += terms[i];
    }
    return csv;
}

std::string CommandDirector::pass2_keyterms(std::string_view speaker_soul,
                                            const std::vector<Message>& history,
                                            const SoulRecord& record,
                                            std::size_t max_terms) {
    std::vector<CommandCandidate> cands = graph_.candidates_for(speaker_soul);
    std::string recent = " ";
    for (const Message& m : history) recent += command_normalize(m.content) + " ";

    std::vector<std::pair<double, const CommandCandidate*>> ranked;
    ranked.reserve(cands.size());
    for (const CommandCandidate& c : cands) {
        ranked.push_back({salience_bonus_for(c, recent, record), &c});
    }
    std::sort(ranked.begin(), ranked.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });

    std::vector<std::string> terms;
    std::set<std::string> seen;
    const std::size_t npc_cap = max_terms > 18 ? max_terms - 18 : max_terms;
    for (const auto& [sc, c] : ranked) {
        (void)sc;
        for (const std::string& t : {c->full_name, c->given_name}) {
            const std::string clean = trim_ws(t);
            if (clean.size() < 3) continue;
            const std::string low = command_normalize(clean);
            if (seen.insert(low).second) {
                terms.push_back(clean);
                if (terms.size() >= npc_cap) break;
            }
        }
        if (terms.size() >= npc_cap) break;
    }
    for (const CanonicalPlace& pl : canonical_places()) {
        const std::string clean = trim_ws(pl.name);
        const std::string low = command_normalize(clean);
        if (seen.insert(low).second) {
            terms.push_back(clean);
            if (terms.size() >= max_terms) break;
        }
    }
    std::string csv;
    for (std::size_t i = 0; i < terms.size(); ++i) {
        if (i > 0) csv += ", ";
        csv += terms[i];
    }
    return csv;
}

std::string CommandDirector::location_keyterms(std::string_view location_id,
                                               std::size_t max_terms,
                                               std::string_view query_hint,
                                               const GuardPresence* guard_presence) {
    std::vector<CommandCandidate> cands = graph_.candidates_for_guard(location_id, guard_presence);
    if (cands.empty() || max_terms == 0) return {};

    const std::string q_det = strip_determiners(query_hint);
    const std::string q_phon = q_det.empty() ? std::string() : phonetic_phrase(q_det);

    std::vector<std::pair<double, const CommandCandidate*>> ranked;
    ranked.reserve(cands.size());
    for (const CommandCandidate& c : cands) {
        double sc = c.trade.empty() ? 0.0 : 0.05;
        if (c.is_major_friend) sc += 0.25;
        else if (c.is_game_named || !c.quest_ids.empty()) sc += 0.12;
        if (!q_det.empty()) {
            for (const std::string& al : {c.full_name, c.given_name, c.byname, c.ui_name, c.trade}) {
                const std::string ad = strip_determiners(al);
                if (!ad.empty()) {
                    sc = std::max(sc, surface_and_phonetic_score(q_det, q_phon, ad, phonetic_phrase(ad)));
                }
            }
            for (const std::string& al : c.extra_aliases) {
                const std::string ad = strip_determiners(al);
                if (!ad.empty()) {
                    sc = std::max(sc, surface_and_phonetic_score(q_det, q_phon, ad, phonetic_phrase(ad)));
                }
            }
        }
        ranked.push_back({sc, &c});
    }
    std::sort(ranked.begin(), ranked.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });

    std::vector<std::string> terms;
    std::set<std::string> seen;
    for (const auto& [sc, c] : ranked) {
        (void)sc;
        for (const std::string& t : {c->full_name, c->given_name}) {
            const std::string clean = trim_ws(t);
            if (clean.size() < 3) continue;
            const std::string low = command_normalize(clean);
            if (seen.insert(low).second) {
                terms.push_back(clean);
                if (terms.size() >= max_terms) break;
            }
        }
        if (terms.size() >= max_terms) break;
    }
    std::string csv;
    for (std::size_t i = 0; i < terms.size(); ++i) {
        if (i > 0) csv += ", ";
        csv += terms[i];
    }
    return csv;
}

CommandOutcome CommandDirector::link(std::string_view speaker_soul,
                                     std::string_view speaker_name,
                                     const ExtractedCommand& extracted,
                                     const std::vector<Message>& history,
                                     const SoulRecord& record,
                                     KbGate kb_gate,
                                     std::string_view guard_location,
                                     const GuardPresence* guard_presence) {
    CommandOutcome out;
    out.action = extracted.action;
    if (extracted.action == CommandAction::None) return out;

    const std::string mention = trim_ws(extracted.target_mention);
    out.target_mention = mention;
    const std::string q_det = strip_determiners(mention);
    const std::string q_norm = command_normalize(mention);

    // 1. Travel destination linking
    if (extracted.action == CommandAction::TravelTo) {
        const std::string q_phon = phonetic_phrase(q_det);
        const CanonicalPlace* best_p = nullptr;
        double best_sc = 0.0;
        for (const CanonicalPlace& pl : canonical_places()) {
            std::vector<std::string> forms = {strip_determiners(pl.name)};
            for (const char* al : pl.aliases) forms.push_back(strip_determiners(al));
            for (const std::string& f : forms) {
                const double sc = surface_and_phonetic_score(q_det, q_phon, f, phonetic_phrase(f));
                if (sc > best_sc) {
                    best_sc = sc;
                    best_p = &pl;
                }
            }
        }
        if (best_p && best_sc >= 0.80) {
            if (kb_gate != KbGate::Open) {
                out.status = CommandStatus::GatedByTrust;
                out.place_id = best_p->id;
                out.display_name = trim_ws(best_p->name);
                out.confidence = best_sc;
                out.note = "travel gated by trust (" + out.place_id + ")";
                return out;
            }
            out.status = CommandStatus::ResolvedPlace;
            out.place_id = best_p->id;
            out.display_name = trim_ws(best_p->name);
            out.confidence = best_sc;
            out.directive = "@travel " + out.place_id;
            out.direction = "[Henry asks to travel to " + out.display_name +
                            ". Answer briefly in character as you prepare for the road.]";
            out.note = "travel_to -> " + out.place_id;
            return out;
        }
        out.status = CommandStatus::UnknownToNpc;
        out.direction = "[Henry names a place ('" + mention +
                        "') you cannot travel to right now; say so plainly and briefly in "
                        "character, without inventing directions, roads or landmarks.]";
        out.note = "travel_to unresolved ('" + mention + "')";
        return out;
    }

    // 2. Follow self/player ("Follow me") vs asking speaker where speaker is
    if (q_norm == "me" || q_norm == "myself" || q_norm == "with me" || q_norm == "along with me") {
        if (extracted.action == CommandAction::FollowNpc) {
            if (kb_gate != KbGate::Open) {
                out.status = CommandStatus::GatedByTrust;
                out.soul_name = std::string(speaker_soul);
                out.display_name = std::string(speaker_name);
                out.confidence = 1.0;
                out.note = "follow_npc gated by trust";
                return out;
            }
            out.status = CommandStatus::ResolvedSoul;
            out.soul_name = std::string(speaker_soul);
            out.display_name = std::string(speaker_name);
            out.confidence = 1.0;
            out.directive = "@follow " + out.soul_name;
            out.direction = "[Henry asks you to follow him. Agree briefly in your own voice.]";
            out.note = "follow_npc -> speaker (" + out.soul_name + ")";
            return out;
        }
    }
    if (q_norm == "you" || q_norm == "yourself" ||
        (!speaker_name.empty() && q_det == strip_determiners(speaker_name))) {
        if (extracted.action == CommandAction::LocateNpc) {
            out.action = CommandAction::None;
            out.status = CommandStatus::NoAction;
            return out;
        }
    }

    std::vector<CommandCandidate> cands;
    if (!guard_location.empty() && extracted.action == CommandAction::LocateNpc) {
        for (CommandCandidate& c : graph_.candidates_for_guard(guard_location, guard_presence)) {
            if (c.soul_name != speaker_soul) cands.push_back(std::move(c));
        }
    }
    if (cands.empty()) {
        cands = graph_.candidates_for(speaker_soul);
    }
    const std::string town_name =
        guard_location.empty()
            ? std::string()
            : std::string(kcd2_settlements::population_center_display_name(guard_location));
    std::string recent = " ";
    for (const Message& m : history) recent += command_normalize(m.content) + " ";

    const CommandCandidate* winner = nullptr;
    double best_surf = 0.0;

    // 3. Multi-turn pronoun coreference
    if (extracted.is_pronoun || is_pronoun_mention(mention)) {
        winner = resolve_pronoun(mention, cands, history);
        if (!winner) {
            out.status = CommandStatus::UnknownToNpc;
            out.direction = "[Henry asks after '" + mention +
                            "', and you are not sure whom he means; ask him plainly whom he is "
                            "looking for, without guessing or inventing any name or place.]";
            out.note = "pronoun coreference unresolved";
            return out;
        }
        best_surf = 0.95;
    } else {
        // 4. Surface + phonetic + relational/trade + salience scoring
        const std::string q_phon = phonetic_phrase(q_det);
        const std::vector<std::string> q_toks = split_words(q_det);

        struct Scored {
            double total = 0.0;
            double surf = 0.0;
            const CommandCandidate* cand = nullptr;
        };
        std::vector<Scored> scored;

        for (const CommandCandidate& c : cands) {
            double c_surf = 0.0;
            bool matched_role_or_trade = false;
            bool matched_explicit_multiword_alias = false;

            // Name aliases
            std::vector<std::string> name_aliases = {c.full_name, c.given_name, c.ui_name};
            if (!c.title.empty() && !c.given_name.empty()) {
                name_aliases.push_back(c.title + " " + c.given_name);
            }
            if (!c.byname.empty()) {
                name_aliases.push_back(c.byname);
                if (!c.given_name.empty()) name_aliases.push_back(c.given_name + " " + c.byname);
            }
            for (const std::string& ea : c.extra_aliases) name_aliases.push_back(ea);

            for (const std::string& al : name_aliases) {
                const std::string ad = strip_determiners(al);
                if (ad.empty()) continue;
                const double sc = surface_and_phonetic_score(q_det, q_phon, ad, phonetic_phrase(ad));
                c_surf = std::max(c_surf, sc);
                if (q_toks.size() >= 2 && split_words(ad).size() >= 2 && sc >= 0.86) {
                    matched_explicit_multiword_alias = true;
                }
            }

            // Leading honorific fallback ("Father Godwin" -> "Godwin", "Lord Capon" -> "Capon")
            if (q_toks.size() >= 2 && is_leading_name_honorific(q_toks[0])) {
                std::string stripped_q;
                for (std::size_t k = 1; k < q_toks.size(); ++k) {
                    if (!stripped_q.empty()) stripped_q.push_back(' ');
                    stripped_q += q_toks[k];
                }
                const std::string sq_phon = phonetic_phrase(stripped_q);
                for (const std::string& al : name_aliases) {
                    const std::string ad = strip_determiners(al);
                    if (ad.empty()) continue;
                    const double sc = surface_and_phonetic_score(stripped_q, sq_phon, ad, phonetic_phrase(ad));
                    if (sc >= 0.88) {
                        c_surf = std::max(c_surf, sc * 0.99);
                        matched_explicit_multiword_alias = true;
                    }
                }
            }

            // Relational & trade aliases
            std::vector<std::string> role_aliases;
            if (!c.trade.empty()) role_aliases.push_back(c.trade);
            for (const std::string& r : c.roles) {
                role_aliases.push_back(r);
                const std::string r_low = command_normalize(r);
                auto it = role_synonyms().find(r_low);
                if (it != role_synonyms().end()) {
                    for (const std::string& syn : it->second) role_aliases.push_back(syn);
                }
            }
            for (const std::string& ra : role_aliases) {
                const std::string ad = strip_determiners(ra);
                if (ad.empty()) continue;
                const double sc = surface_and_phonetic_score(q_det, q_phon, ad, phonetic_phrase(ad));
                if (sc >= 0.84) {
                    matched_role_or_trade = true;
                    c_surf = std::max(c_surf, sc);
                }
            }

            // Multi-word given-name consistency guard (prevents "Gilbert Miller" -> "Cecily Miller")
            if (q_toks.size() >= 2 && !matched_role_or_trade && !matched_explicit_multiword_alias) {
                const std::string c_full = strip_determiners(c.full_name);
                const std::vector<std::string> c_toks = split_words(c_full);
                const std::string c_gn = strip_determiners(c.given_name);
                const std::string title_gn =
                    (!c.title.empty() && !c.given_name.empty()) ? strip_determiners(c.title + " " + c.given_name) : "";
                if (!title_gn.empty() && jaro_winkler_similarity(q_det, title_gn) >= 0.84) {
                    // Valid title + given name match ("Father Ernest")
                } else if (c_toks.size() == 1) {
                    c_surf = std::min(c_surf, 0.72);
                } else if (!c_gn.empty()) {
                    const double gn_sim = surface_and_phonetic_score(
                        q_toks[0], command_phonetic_key(q_toks[0]), c_gn, command_phonetic_key(c_gn));
                    if (gn_sim < 0.76) c_surf *= 0.65;
                }
            }

            if (c_surf < 0.79) continue;
            double priority_bonus = 0.0;
            if (!matched_role_or_trade && c_surf >= 0.86) {
                if (strip_determiners(c.full_name) == q_det) {
                    priority_bonus += 0.035;
                }
                if (c.is_major_friend) {
                    priority_bonus += 0.09;
                } else if (c.is_game_named || !c.quest_ids.empty()) {
                    priority_bonus += 0.05;
                }
            }
            const double total = c_surf + priority_bonus + salience_bonus_for(c, recent, record);
            scored.push_back({total, c_surf, &c});
        }

        if (scored.empty()) {
            out.status = CommandStatus::UnknownToNpc;
            if (!town_name.empty()) {
                out.direction =
                    "[Henry asks where to find '" + mention + "'. Nobody of that name or trade is here in " +
                    town_name + ": say so plainly and briefly as a local guard, without guessing or inventing anyone.]";
            } else {
                out.direction =
                    "[Henry asks where to find '" + mention +
                    "'. You do not know anyone of that name or where they might be: say so plainly "
                    "and briefly in your own voice, and do not guess, suggest a market, tavern or "
                    "street, or invent any rumour, trade or place.]";
            }
            out.note = "unknown_to_npc ('" + mention + "')";
            return out;
        }
        std::sort(scored.begin(), scored.end(),
                  [](const Scored& a, const Scored& b) { return a.total > b.total; });
        if (!guard_location.empty() && scored.size() >= 2 &&
            scored[0].cand->soul_name != scored[1].cand->soul_name &&
            (scored[0].total - scored[1].total) < 0.035 &&
            strip_determiners(scored[0].cand->full_name) != q_det) {
            out.status = CommandStatus::AmbiguousSoul;
            out.confidence = scored[0].surf;
            std::string choices;
            std::size_t shown = 0;
            std::set<std::string> seen_names;
            for (const Scored& s : scored) {
                if ((scored[0].total - s.total) >= 0.06) break;
                if (!seen_names.insert(s.cand->full_name).second) continue;
                if (shown > 0) choices += "; ";
                choices += s.cand->full_name;
                if (!s.cand->trade.empty()) choices += " (" + s.cand->trade + ")";
                if (++shown >= 4) break;
            }
            if (shown >= 2) {
                out.direction =
                    "[Henry asks where to find '" + mention + "'" +
                    (town_name.empty() ? std::string() : (" in " + town_name)) +
                    ", and more than one person here fits: " + choices +
                    ". Briefly name these people and ask which one he is looking for.]";
                out.note = "ambiguous_guard_lookup ('" + mention + "' -> " + choices + ")";
                return out;
            }
        }
        winner = scored.front().cand;
        best_surf = scored.front().surf;
    }

    out.display_name = winner->full_name;
    out.settlement = winner->settlement;
    out.confidence = best_surf;

    if (winner->is_absentee) {
        out.status = CommandStatus::UnmeetableAbsentee;
        const std::string why = winner->why_unmeetable.empty()
                                    ? (winner->full_name + " is gone and cannot be met here")
                                    : winner->why_unmeetable;
        out.direction = "[Henry asks where " + winner->full_name +
                        " is. Do not give directions as if they were standing nearby: " + why + ".]";
        out.note = "absentee abstention (" + winner->full_name + ")";
        return out;
    }

    const bool gated =
        (extracted.action == CommandAction::LocateNpc)
            ? (kb_gate == KbGate::Asking || kb_gate == KbGate::Owed)
            : (kb_gate != KbGate::Open);
    if (gated) {
        out.status = CommandStatus::GatedByTrust;
        out.soul_name = winner->soul_name;
        out.note = "command gated by trust (" + winner->soul_name + ")";
        return out;
    }

    out.status = CommandStatus::ResolvedSoul;
    out.soul_name = winner->soul_name;
    if (!winner->person_token.empty() && !record.knows(winner->person_token)) {
        out.heard_token = winner->person_token;
    }

    if (extracted.action == CommandAction::LocateNpc) {
        const std::vector<RoutineSpot> spots =
            guard_location.empty()
                ? graph_.spots_of(winner->soul_name)
                : graph_.spots_of_in_location(winner->soul_name, guard_location, guard_presence);
        out.directive = format_locate_directive(winner->soul_name, spots);
        const SoulWhereabouts wb = graph_.whereabouts_of(winner->soul_name);

        const std::string how_known =
            !town_name.empty()
                ? ("As a guard here in " + town_name + ", you know everyone currently in " +
                   town_name + ", including " + winner->full_name +
                   " — do not say you do not know them.")
                : ((winner->hop > 1 && !winner->via_name.empty())
                       ? ("You know of " + winner->full_name + " through " + winner->via_name +
                          " — do not say you do not know them.")
                       : ("You know " + winner->full_name + " — do not say you do not know them."));

        std::string facts;
        if (!wb.self_intro.empty()) {
            facts += winner->full_name + " says of themselves: \"" + wb.self_intro + "\"";
        }
        if (!wb.home_fact.empty()) {
            if (!facts.empty()) facts += " ";
            facts += wb.home_fact;
        }
        if (facts.empty()) {
            facts = winner->full_name;
            if (!winner->trade.empty()) facts += " is a " + winner->trade;
            const std::string clean_sett =
                !town_name.empty() ? town_name : sanitize_settlement_label(winner->settlement);
            if (!clean_sett.empty()) {
                facts += (winner->trade.empty() ? " is here in " : " here in ") + clean_sett;
            } else if (winner->trade.empty()) {
                facts += " keeps nearby";
            }
            if (!town_name.empty() && guard_presence && !spots.empty()) {
                const std::string bearing = compass_bearing_phrase(
                    guard_presence->guard_x, guard_presence->guard_y, spots.front().x, spots.front().y);
                if (!bearing.empty()) {
                    facts += ", " + bearing;
                }
            }
            facts += ".";
        }

        out.direction =
            "[Henry asks where to find " + winner->full_name + ". " + how_known +
            " What you know of who they are and where to find them: " + facts +
            " Answer in your own voice in a sentence or two using only these particulars; "
            "do not invent any other trade, shop, market, forge, street or rumour.]";
        out.note = "locate_npc -> " + winner->soul_name + " (" + winner->full_name + ")";
    } else if (extracted.action == CommandAction::FollowNpc) {
        out.directive = "@follow " + winner->soul_name;
        out.direction = "[Henry asks to follow " + winner->full_name +
                        ". Reply briefly and naturally in character.]";
        out.note = "follow_npc -> " + winner->soul_name;
    }
    return out;
}

ExtractedCommand CommandDirector::run_extractor(std::string_view speaker_name,
                                                std::string_view speaker_settlement,
                                                std::string_view said,
                                                const std::vector<Message>& history) {
    if (!extractor_) return {};
    std::string user = "NPC Speaker: " +
                       (speaker_name.empty() ? std::string("Villager") : std::string(speaker_name)) +
                       " (" +
                       (speaker_settlement.empty() ? std::string("Bohemia") : std::string(speaker_settlement)) +
                       ")";
    const std::size_t start = history.size() > 4 ? history.size() - 4 : 0;
    for (std::size_t i = start; i < history.size(); ++i) {
        if (history[i].role != "user" && history[i].role != "assistant") continue;
        const std::string who =
            (history[i].role == "user")
                ? "Player"
                : (speaker_name.empty() ? std::string("Villager") : std::string(speaker_name));
        user += "\n" + who + ": " + history[i].content;
    }
    user += "\nPlayer: " + std::string(said);

    std::vector<Message> msgs = {
        {"system", kCompactCommandSystemPrompt},
        {"user", std::move(user)},
    };
    std::string raw;
    {
        std::lock_guard<std::mutex> g(llm_lock_);
        extractor_->chat(msgs, extractor_options_, [&](std::string_view piece) {
            raw.append(piece);
        });
    }
    return parse_extracted_command(raw);
}

CommandOutcome CommandDirector::resolve_guard_lookup(
    std::string_view guard_soul,
    std::string_view guard_name,
    std::string_view guard_location,
    std::string_view said,
    const std::vector<Message>& history,
    const SoulRecord& record,
    const std::function<std::string(std::string_view)>& retranscribe_fn,
    const GuardPresence* guard_presence) {
    const std::string subject = clean_guard_subject_mention(said);
    if (subject.empty()) return {};

    ExtractedCommand ext;
    ext.action = CommandAction::LocateNpc;
    ext.target_mention = subject;
    ext.is_pronoun = is_pronoun_mention(subject);

    CommandOutcome out = link(
        guard_soul, guard_name, ext, history, record, KbGate::Open, guard_location, guard_presence);

    if (retranscribe_fn && !ext.is_pronoun &&
        (out.status == CommandStatus::UnknownToNpc ||
         out.status == CommandStatus::AmbiguousSoul ||
         out.confidence < 0.88)) {
        const std::string p2_terms =
            guard_location.empty()
                ? pass2_keyterms(guard_soul, history, record)
                : location_keyterms(guard_location, 95, subject, guard_presence);
        const std::string retx = trim_ws(retranscribe_fn(p2_terms));
        const std::string subject2 = clean_guard_subject_mention(retx);
        if (!subject2.empty() && command_normalize(subject2) != command_normalize(subject)) {
            ExtractedCommand ext2;
            ext2.action = CommandAction::LocateNpc;
            ext2.target_mention = subject2;
            ext2.is_pronoun = is_pronoun_mention(subject2);
            CommandOutcome out2 = link(guard_soul, guard_name, ext2, history, record, KbGate::Open,
                                       guard_location, guard_presence);
            if ((out.status != CommandStatus::ResolvedSoul &&
                 out2.status == CommandStatus::ResolvedSoul) ||
                out2.confidence > out.confidence) {
                out = std::move(out2);
                out.pass2_used = true;
            }
        }
    }
    return out;
}

CommandOutcome CommandDirector::resolve(
    std::string_view speaker_soul,
    std::string_view speaker_name,
    std::string_view speaker_settlement,
    std::string_view said,
    const std::vector<Message>& history,
    const SoulRecord& record,
    KbGate kb_gate,
    const std::function<std::string(std::string_view)>& retranscribe_fn,
    std::string_view guard_location,
    const GuardPresence* guard_presence) {
    if (said.empty() || !extractor_) return {};

    // Stage 1: Fast EmbeddingGemma-300M 768-d linear intent gate
    if (embedder_ && gate_.loaded()) {
        const std::vector<float> q = embedder_->embed(said, /*query=*/true);
        if (!q.empty() && !gate_.should_run_extractor(q)) {
            return {};
        }
    }

    // Stage 2: Sidecar SLM extraction (`NONE | LOCATE: ... | FOLLOW: ... | TRAVEL: ...`)
    ExtractedCommand ext = run_extractor(speaker_name, speaker_settlement, said, history);
    if (ext.action == CommandAction::None) return {};

    // Stage 3: Salience-ranked social-graph (or Burgher settlement) entity resolution
    CommandOutcome out = link(
        speaker_soul, speaker_name, ext, history, record, kb_gate, guard_location, guard_presence);

    // Stage 4: Conditional STT Pass-2 buffered PCM replay on unresolved/low-confidence voice commands
    if (retranscribe_fn && !ext.is_pronoun &&
        (out.status == CommandStatus::UnknownToNpc || out.confidence < 0.88)) {
        const std::string p2_terms =
            (!guard_location.empty() && ext.action == CommandAction::LocateNpc)
                ? location_keyterms(guard_location, 95, ext.target_mention, guard_presence)
                : pass2_keyterms(speaker_soul, history, record);
        const std::string retx = trim_ws(retranscribe_fn(p2_terms));
        if (!retx.empty() && command_normalize(retx) != command_normalize(said)) {
            ExtractedCommand ext2 = run_extractor(speaker_name, speaker_settlement, retx, history);
            if (ext2.action != CommandAction::None) {
                CommandOutcome out2 = link(speaker_soul, speaker_name, ext2, history, record,
                                           kb_gate, guard_location, guard_presence);
                if ((out.status == CommandStatus::UnknownToNpc &&
                     out2.status != CommandStatus::UnknownToNpc) ||
                    out2.confidence > out.confidence) {
                    out = std::move(out2);
                    out.pass2_used = true;
                }
            }
        }
    }
    return out;
}

}  // namespace moonshine
