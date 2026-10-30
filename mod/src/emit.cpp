#include "emit.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace kcd2 {
namespace {

// What the menu would say for entering the loop, which the player never sees: the root
// decision holds one sequence, and a lone available entry is played rather than offered.
// It is here at all because a sequence with no prompt cannot be presented, and a dialogue
// with nothing presentable does not open.
constexpr const char* kHoldPrompt = "...";

// The movie clip the game says its dialogue lines in, by the instance name hud.gfx gives
// it: <MovieClip name="Subtitles" instancename="bc.subtitles" />. One of the 72 HUD.xml
// declares, and the one thing standing between a silent dialogue and a caption saying so.
constexpr const char* kCaptionClip = "bc.subtitles";

// The app's heartbeat file is named by the Unix time in buckets this many seconds wide
// (app/src/turnfile.h, kAliveBucketSeconds; the Lua's aliveBucket). Wide enough that the
// engine's single-precision clock, which steps by 128 seconds, lands in the right bucket
// or the one beside it, both of which are looked for.
constexpr int kAliveBucketSeconds = 300;

// What the player reads when they talk to somebody and the app that writes the words is
// not running. The mod's own line, shown bare in the dialogue and closed on reading: a
// conversation that opens and never answers was the one failure with nothing to say for
// itself.
constexpr const char* kAppDownLine =
    "[Moonshine] The conversation app is not running, so nobody here can answer you. "
    "Start moonshine_app and talk to them again.";
// And when it is running (its heartbeat was there) but has not sent a first line in the
// time the pump gives it: busy, or stuck, which the player cannot tell apart and need not.
// Seen 29 Oct 2026, the app folding a long memory on the loop that answers the game, and
// the player held in the camera by Christine for as long as they cared to wait.
constexpr const char* kAppBusyLine =
    "[Moonshine] The conversation app did not answer in time. Give it a moment and talk to "
    "them again.";

// Apostrophes are left alone: every attribute written here is double-quoted, so
// they need no escaping, and the shipped files never write &apos; -- 3420 raw
// apostrophes in text_ui_quest.xml and none escaped -- which makes the raw form
// the one the game's parser is known to read.
std::string xml_escape(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += c; break;
        }
    }
    return out;
}

std::string xml_unescape(std::string text) {
    auto replace = [&](const char* from, char to) {
        const std::size_t n = std::char_traits<char>::length(from);
        for (std::size_t at = 0; (at = text.find(from, at)) != std::string::npos; ) {
            text.replace(at, n, 1, to);
            ++at;
        }
    };
    replace("&quot;", '"');
    replace("&lt;", '<');
    replace("&gt;", '>');
    replace("&amp;", '&');
    return text;
}

// A Lua single-quoted string. The graph's EntryScript is that language.
std::string lua_single(const std::string& text) {
    std::string out = "'";
    for (unsigned char c : text) {
        if (c == '\\' || c == '\'') out += '\\';
        if (c == '\n' || c == '\r') {
            out += ' ';
            continue;
        }
        out += static_cast<char>(c);
    }
    out += '\'';
    return out;
}

// `<Sequence` is also the start of `<Sequences>`. Only the element that opens one
// exchange counts, which is the one whose next character is not a letter.
bool sequence_open_at(const std::string& xml, std::size_t at) {
    if (at + 9 > xml.size() || xml.compare(at, 9, "<Sequence") != 0) return false;
    if (at + 9 == xml.size()) return false;
    const char next = xml[at + 9];
    return next == ' ' || next == '>' || next == '\n' || next == '\r';
}

std::size_t find_sequence_open(const std::string& xml, std::size_t from) {
    for (std::size_t at = from;;) {
        at = xml.find("<Sequence", at);
        if (at == std::string::npos) return at;
        if (sequence_open_at(xml, at)) return at;
        at += 9;
    }
}

// The lines this sequence says itself, not the ones in a sequence nested under it:
// those are said later, when that sequence is entered.
std::string voice_script(const std::string& body, const std::string& npc, const std::string& soul) {
    std::string script;
    int depth = 0;
    for (std::size_t at = 0; at < body.size();) {
        if (sequence_open_at(body, at)) {
            ++depth;
            at += 9;
            continue;
        }
        if (body.compare(at, 12, "</Sequence>") == 0) {
            if (depth > 0) --depth;
            at += 12;
            continue;
        }
        constexpr std::string_view kRole = "<Response Role=\"";
        if (depth == 0 && body.compare(at, kRole.size(), kRole.data()) == 0) {
            const std::size_t role_at = at + kRole.size();
            const std::size_t role_end = body.find('"', role_at);
            const std::size_t resp_end = body.find("</Response>", role_at);
            if (role_end == std::string::npos || resp_end == std::string::npos) {
                at += kRole.size();
                continue;
            }
            const std::string role = body.substr(role_at, role_end - role_at);
            const std::string chunk = body.substr(role_end, resp_end - role_end);
            const std::size_t text_at = chunk.find("Text=\"");
            if (text_at != std::string::npos) {
                const std::size_t begin = text_at + 6;
                const std::size_t end = chunk.find('"', begin);
                if (end != std::string::npos) {
                    const std::string words = xml_unescape(chunk.substr(begin, end - begin));
                    if (!words.empty()) {
                        const std::string who = role == "HENRY" ? "henry" : npc;
                        if (!script.empty()) script += "; ";
                        // The speaker's entity name rides on the other party's lines, so
                        // the Lua can put the mouth on them outside any app session (the
                        // game's own Talk has no Moonshine.npc; notes.md 13 Oct 2026).
                        script += "Moonshine.Voice('" + who + "', " + lua_single(words) +
                                  (role == "HENRY" || soul.empty() ? std::string() : ", " + lua_single(soul)) + ")";
                    }
                }
            }
            at = resp_end + 12;
            continue;
        }
        ++at;
    }
    return script;
}

// The value of `name="..."` in an element's open tag, still escaped, or empty.
std::string tag_attr(const std::string& open, const std::string& name) {
    const std::string key = " " + name + "=\"";
    const std::size_t at = open.find(key);
    if (at == std::string::npos) return {};
    const std::size_t begin = at + key.size();
    const std::size_t end = open.find('"', begin);
    return end == std::string::npos ? std::string() : open.substr(begin, end - begin);
}

// The open tag without its `name="..."`.
std::string drop_attr(std::string open, const std::string& name) {
    const std::string key = " " + name + "=\"";
    const std::size_t at = open.find(key);
    if (at == std::string::npos) return open;
    const std::size_t end = open.find('"', at + key.size());
    if (end == std::string::npos) return open;
    open.erase(at, end + 1 - at);
    return open;
}

// Ask the app to speak each line of a scripted exchange as that sequence is entered.
// `npc_gender` is the other party's: henry is always his own recording.
//
// The engine has no entry hook (an EntryScript attribute is not in WHGame.dll and is
// ignored), and ExitScript on a sequence that ends on a nested decision runs only when
// that decision has resolved -- a holder's open, row and outcome scripts all landed in
// the log together at the end of the conversation (11 Oct 2026). So the lines are
// asked for from the exit of a sequence that ends at once: each voiced sequence is
// split into the row the player sees, which keeps the prompt, the conditions and the
// skill check and plays one bare response, and whose exit asks for the lines before a
// GoTo to the body, an Autoselected decision holding the lines, the triggers, the
// original end and exit script, and whatever was nested under them. The bodies live in
// a sequence of the root decision that is never available (EntryCondition="FALSE",
// as 400 shipped sequences spell it), reached by name alone: 16806 shipped GoTos
// target a decision that is not an ancestor, and 299 shipped skill checks end on a
// GoTo and are judged (SkillcheckCondition) on the far side of it.
std::string voiced_sequences(const std::string& xml, const std::string& npc, const std::string& soul,
                             std::string& bodies) {
    std::string out;
    std::size_t i = 0;
    while (i < xml.size()) {
        const std::size_t at = find_sequence_open(xml, i);
        if (at == std::string::npos) {
            out.append(xml, i, std::string::npos);
            return out;
        }
        out.append(xml, i, at - i);
        const std::size_t tag_end = xml.find('>', at);
        if (tag_end == std::string::npos) {
            out.append(xml, at, std::string::npos);
            return out;
        }
        std::size_t j = tag_end + 1;
        int depth = 1;
        while (j < xml.size() && depth > 0) {
            const std::size_t next_open = find_sequence_open(xml, j);
            const std::size_t next_close = xml.find("</Sequence>", j);
            if (next_close == std::string::npos) {
                out.append(xml, at, std::string::npos);
                return out;
            }
            if (next_open != std::string::npos && next_open < next_close) {
                ++depth;
                j = next_open + 9;
            } else {
                --depth;
                j = next_close + 12;
            }
        }
        const std::size_t close_at = j - 12;
        const std::string open = xml.substr(at, tag_end - at);
        const std::string body =
            voiced_sequences(xml.substr(tag_end + 1, close_at - (tag_end + 1)), npc, soul, bodies);
        const std::string script = voice_script(body, npc, soul);
        const std::string name = tag_attr(open, "Name");
        if (script.empty() || name.empty()) {
            out += open;
            out += '>';
            out += body;
            out += "</Sequence>";
            i = j;
            continue;
        }
        // This sequence's own prompt and first speaker: the ones before any sequence
        // nested under it. The row's one bare response is given to that speaker, so the
        // camera it opens on is the body's first.
        std::string prompt;
        std::string first_role = "HENRY";
        {
            const std::size_t nested = find_sequence_open(body, 0);
            const std::size_t p = body.find("<UiPrompt ");
            if (p != std::string::npos && (nested == std::string::npos || p < nested)) {
                const std::size_t e = body.find("/>", p);
                if (e != std::string::npos) prompt = body.substr(p, e + 2 - p);
            }
            const std::size_t r = body.find("<Response Role=\"");
            if (r != std::string::npos && (nested == std::string::npos || r < nested)) {
                const std::size_t begin = r + 16;
                const std::size_t e = body.find('"', begin);
                if (e != std::string::npos) first_role = body.substr(begin, e - begin);
            }
        }
        std::string row = drop_attr(drop_attr(drop_attr(open, "EndType"), "GoToDecision"), "ExitScript");
        row += " EndType=\"GoTo\" GoToDecision=\"" + name + "__v\" ExitScript=\"" +
               xml_escape(script) + "\"";
        out += row;
        out += '>';
        out += prompt;
        out += "<Elements><Response Role=\"" + first_role + "\" /></Elements></Sequence>";
        i = j;

        std::string inner = "<Sequence Name=\"" + name + "__b\"";
        for (const char* keep : {"EndType", "GoToDecision", "ExitScript"}) {
            const std::string value = tag_attr(open, keep);
            if (!value.empty()) inner += " " + std::string(keep) + "=\"" + value + "\"";
        }
        inner += '>';
        inner += body;
        inner += "</Sequence>";
        bodies += "<Sequence EntryCondition=\"FALSE\" EndType=\"Decision\" Name=\"" + name +
                  "__c\"><Decision Name=\"" + name + "__v\" Autoselect=\"true\"><Sequences>" +
                  inner + "</Sequences></Decision></Sequence>\r\n";
    }
    return out;
}

std::string voiced_dialogue(const std::string& xml, const std::string& npc_gender, const std::string& soul) {
    const std::string npc = npc_gender == "female" ? "female" : "male";
    std::string bodies;
    std::string out = voiced_sequences(xml, npc, soul, bodies);
    if (bodies.empty()) return out;
    // The last </Sequences> closes the root decision, the outermost element that has one.
    const std::size_t root_end = out.rfind("</Sequences>");
    if (root_end == std::string::npos) return out;
    const std::string holder =
        "<Sequence EntryCondition=\"FALSE\" EndType=\"Decision\" Name=\"__voiced\"><Decision "
        "Name=\"__voiced_dec\" Autoselect=\"true\"><Sequences>\r\n" +
        bodies + "</Sequences></Decision></Sequence>\r\n";
    out.insert(root_end, holder);
    return out;
}

// The game's skill id behind a "training" reward's words (the ids SkillReward takes in
// the shipped quests: heavy_weapons, weapon_sword, weapon_unarmed, marksmanship,
// fencing, horse_riding, stealth, thievery, scholarship, craftsmanship, survival,
// alchemy, drinking). The words are the slate's reward_text; a lesson nothing here
// matches is taken for scholarship, the catch-all a townsman can teach.
std::string skill_taught(const std::string& words) {
    std::string w;
    for (char c : words) w += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    auto has = [&](const char* s) { return w.find(s) != std::string::npos; };
    if (has("heavy weapon") || has("mace") || has("axe") || has("polearm")) return "heavy_weapons";
    if (has("sword")) return "weapon_sword";
    if (has("fist") || has("unarmed") || has("wrestl") || has("brawl")) return "weapon_unarmed";
    if (has("bow") || has("crossbow") || has("marksman") || has("shoot")) return "marksmanship";
    if (has("horse") || has("riding")) return "horse_riding";
    if (has("stealth") || has("sneak") || has("quiet feet")) return "stealth";
    if (has("lockpick") || has("thiev") || has("pick a lock") || has("pocket")) return "thievery";
    if (has("alchem") || has("potion") || has("herb")) return "alchemy";
    if (has("drink")) return "drinking";
    if (has("craft") || has("smith") || has("forge") || has("repair")) return "craftsmanship";
    if (has("hunt") || has("track") || has("survival") || has("wood")) return "survival";
    return "scholarship";
}

// Lua long-bracket strings would be simpler, but soul names and the on-screen
// text both come from outside, so quote them properly.
std::string lua_escape(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            default: out += c; break;
        }
    }
    return out;
}

// A space-, comma- or semicolon-separated list of item class guids (npc_quest_lines'
// loot_classes, gift_classes) as the inside of a Lua table: "a", "b". Empty for none.
std::string lua_class_list(const std::string& classes) {
    std::string out;
    std::string part;
    for (char c : classes + " ") {
        if (c == ' ' || c == ',' || c == '\t' || c == '\n' || c == ';') {
            if (!part.empty()) out += (out.empty() ? "" : ", ") + ("\"" + lua_escape(part) + "\"");
            part.clear();
        } else {
            part += c;
        }
    }
    return out;
}

// Every shipped quest XML opens with a BOM and names the database it belongs to.
// A project and its definition files always agree on that name -- the Skald
// projects under Quests/Testing use "hammerheart" throughout while the shipped
// game uses "brambora" -- so the name is ours to pick but not ours to vary.
// The mod's own name, and that is fine. Every dialogue the game ships declares
// brambora or hammerheart, which reads like a registry and is not one: a database
// named for the mod was measured working, camera and spoken line and all, before
// any of this was rewritten. The shipped pair are a convention, not a requirement.
std::string skald_header(const std::string& database) {
    return "\xEF\xBB\xBF<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
           "<Database xmlns:xsd=\"http://www.w3.org/2001/XMLSchema\" "
           "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" Name=\"" +
           xml_escape(database) + "\">\r\n\t<Skald>\r\n";
}

// A role name reaches the game through <addRole> and through a dialogue's Role
// attribute, and both are matched literally, so anything that is not a plain
// identifier can only fail somewhere far from here.
void require_identifier(const std::string& name, const char* what) {
    if (name.empty()) throw std::runtime_error(std::string(what) + " cannot be empty");
    for (char c : name) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') {
            throw std::runtime_error(std::string(what) + " '" + name +
                                     "' must be letters, digits and underscores only");
        }
    }
}

// A number written the way the files that hold it are written. Shipped
// ReferenceLengths go both ways -- round "5" alongside measured "1.63" -- so trailing
// zeros are trimmed rather than padded out to a fixed width, which suits a screen
// position in Lua as well as a length in seconds in XML.
std::string trim_number(double value) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", value);
    std::string out = buf;
    while (out.size() > 1 && out.back() == '0') out.pop_back();
    if (!out.empty() && out.back() == '.') out.pop_back();
    return out;
}

}  // namespace

std::string ui_name_key(const std::string& modid, const std::string& soul_name) {
    return modid + "_" + soul_name;
}

std::string conversation_hint_key(const std::string& modid) {
    return modid + "_conversation";
}

std::string favour_hint_key(const std::string& modid) {
    return modid + "_favour";
}

std::string emit_storm_root(const std::string& modid, bool with_roles) {
    // The task names and classes must match the game's own, since setUiName is
    // only honoured under the names class and addRole under the roles class.
    // Source paths are relative to Libs/Storm and use backslashes, as every
    // shipped storm root does.
    std::string s;
    s += "<?xml version=\"1.0\"?>\r\n";
    s += "<storm>\r\n";
    s += "\t<tasks>\r\n";
    s += "\t\t<task name=\"names\" class=\"names\">\r\n";
    s += "\t\t\t<source path=\"names\\" + modid + ".xml\" />\r\n";
    s += "\t\t</task>\r\n";
    if (with_roles) {
        s += "\t\t<task name=\"roles\" class=\"roles\">\r\n";
        s += "\t\t\t<source path=\"roles\\" + modid + ".xml\" />\r\n";
        s += "\t\t</task>\r\n";
    }
    s += "\t</tasks>\r\n";
    s += "</storm>\r\n";
    return s;
}

namespace {

// us-ascii, tabs and CRLF are what every shipped table under Libs/Tables uses,
// and the database name there is "barbora" -- not the "brambora" the Skald quest
// files carry. A fragment repeats the whole wrapper, since the loader merges
// documents rather than concatenating rows, and says so when it works:
// "Table 'role' is patched by 'role__moonshine_names', lines added: 1".
std::string table_header() {
    return "<?xml version=\"1.0\" encoding=\"us-ascii\"?>\r\n"
           "<database xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" name=\"barbora\" "
           "xsi:noNamespaceSchemaLocation=\"../database.xsd\">\r\n";
}

}  // namespace

std::string emit_metarole_table(const std::string& modid, const std::string& metarole,
                                const std::vector<Quest>& quests) {
    require_identifier(metarole, "dialogue metarole");
    (void)modid;

    std::string s = table_header();
    s += "\t<metaroles version=\"1\">\r\n";
    s += "\t\t<metarole metarole_name=\"" + metarole + "\" />\r\n";
    // One more per quest, for the holder alone. A dialogue speaks to whoever holds its
    // Role, so a quest's own dialogue needs a role nobody but its holder has, or every
    // talkable soul would be offering to give the axe back.
    for (const Quest& q : quests) {
        if (q.has_holder()) {   // a find or a travel has no holder to speak through one
            s += "\t\t<metarole metarole_name=\"" + holder_metarole(q.id) + "\" />\r\n";
        }
        // And one for the soul an ask's word is carried on to, for the same reason.
        if (q.carries_on()) {
            s += "\t\t<metarole metarole_name=\"" + onward_metarole(q.id) + "\" />\r\n";
        }
        // And one for the one who can let Henry in (In the Stocks' Elsbeth).
        if (q.has_helper()) {
            s += "\t\t<metarole metarole_name=\"" + helper_metarole(q.id) + "\" />\r\n";
        }
    }
    s += "\t</metaroles>\r\n";
    s += "</database>\r\n";
    return s;
}

std::string emit_role_table(const std::string& modid,
                            const std::string& role,
                            const std::string& metarole,
                            const std::vector<Quest>& quests) {
    require_identifier(role, "dialogue role");
    require_identifier(metarole, "dialogue metarole");
    if (role == metarole) {
        throw std::runtime_error("role and metarole need different names, the way SMLOUVANI_MUZ "
                                 "sits under SMLOUVANI");
    }
    (void)modid;

    // metarole_name is the group this role belongs to. Pointing it at our own
    // group rather than the shipped NPC one is what makes the role reachable at
    // runtime, since granting a metarole satisfies the roles beneath it -- which
    // is how borrowing SMLOUVANI produced a haggling dialogue whose lines belong
    // to SMLOUVANI_MUZ. _role_string_name is the role's own display key, unused
    // here but present on all 5430 shipped rows.
    //
    // The gendered pair is load-bearing for AddMetaRoleByName. A metarole with only
    // an ungendered child was declared and merged, then silently refused to attach
    // to a living soul: after grant the metarole list was unchanged, RequestDialog
    // returned true, and no session opened. SMLOUVANI ships MUZ and ZENA; matching
    // that shape is what makes the grant take. The ungendered row stays because the
    // dialogue's Role attribute names it.
    std::string s = table_header();
    s += "\t<roles version=\"1\">\r\n";
    auto row = [&](const std::string& name, const std::string& group,
                   const std::string& string_name) {
        return "\t\t<role _locked=\"false\" _role_string_name=\"" + string_name +
               "\" metarole_name=\"" + group + "\" role_name=\"" + name + "\" />\r\n";
    };
    auto triple = [&](const std::string& name, const std::string& group) {
        s += row(name, group, "role_" + name);
        s += row(name + "_MUZ", group, "role_" + name + "_(MUZ)");
        s += row(name + "_ZENA", group, "role_" + name + "_(ZENA)");
    };
    triple(role, metarole);
    // The holder roles, the same shape under their own metaroles, since the gendered
    // pair is what lets AddMetaRoleByName reach a soul on an old save.
    for (const Quest& q : quests) {
        if (q.has_holder()) triple(holder_role(q.id), holder_metarole(q.id));
        if (q.carries_on()) triple(onward_role(q.id), onward_metarole(q.id));
        if (q.has_helper()) triple(helper_role(q.id), helper_metarole(q.id));
    }
    s += "\t</roles>\r\n";
    s += "</database>\r\n";
    return s;
}

std::string emit_storm_roles(const std::string& modid,
                             const std::vector<Persona>& personas,
                             const std::vector<std::string>& only_souls,
                             const std::string& role,
                             const std::vector<Quest>& quests,
                             std::size_t* granted) {
    require_identifier(role, "dialogue role");

    const std::set<std::string> allowed(only_souls.begin(), only_souls.end());

    std::string s;
    s += "<?xml version=\"1.0\"?>\r\n";
    s += "<!DOCTYPE storm SYSTEM \"..\\storm.dtd\">\r\n";
    s += "<storm>\r\n";
    s += "\t<rules>\r\n";
    std::size_t count = 0;
    auto rule = [&](const std::string& tag, const std::string& soul_name, const std::string& r) {
        const std::string soul = xml_escape(soul_name);
        s += "\t\t<rule name=\"" + modid + "_" + tag + "_" + soul + "\">\r\n";
        s += "\t\t\t<selectors><hasName name=\"" + soul + "\" /></selectors>\r\n";
        s += "\t\t\t<operations><addRole name=\"" + xml_escape(r) + "\" /></operations>\r\n";
        s += "\t\t</rule>\r\n";
    };
    for (const Persona& p : personas) {
        if (!allowed.empty() && allowed.count(p.soul_name) == 0) continue;
        rule("talk", p.soul_name, role);
        ++count;
    }
    // The holder of each quest gets that quest's role as well, so its dialogue has a
    // speaker from the moment the soul is made. Souls made before the mod was enabled
    // are reached at runtime instead, the same way as the talk role.
    for (const Quest& q : quests) {
        if (q.has_holder()) rule("hold", q.target_soul, holder_role(q.id));
        if (q.carries_on()) rule("onward", q.onward_soul, onward_role(q.id));
        if (q.has_helper()) rule("helper", q.helper_soul, helper_role(q.id));
    }
    s += "\t</rules>\r\n";
    s += "</storm>\r\n";
    if (granted) *granted = count;
    return s;
}

std::vector<std::string> quest_commands(const Quest& quest) {
    // accept and the plain complete first, in the order the codes were always dealt, then
    // a complete for each way the journal has its own line for. The Lua picks among the
    // completes by how the handover went and falls back to the plain one.
    std::vector<std::string> out = {"accept", "complete"};
    for (const std::string& how : quest.resolutions()) out.push_back("complete_" + how);
    // An ask whose word is carried on has a beat between the telling and the end: the
    // giver told and paid, Henry sent on (the objective's update and the last objective).
    if (quest.carries_on()) out.push_back("onward");
    // And the errand given up to the giver's face (the app's give-up row, notes.md 13 Oct
    // 2026): the quest fails in the book with its own Canceled line, nothing is paid.
    out.push_back("abandon");
    return out;
}

std::vector<QuestBeat> quest_beats(const std::vector<Quest>& quests) {
    std::vector<QuestBeat> out;
    int code = 1;
    for (const Quest& q : quests) {
        for (const std::string& command : quest_commands(q)) {
            out.push_back({q.id, command, code++});
        }
    }
    return out;
}

std::string quest_flag_variable(const std::string& modid, const std::string& quest_id,
                                const std::string& flag) {
    require_identifier(flag, "quest flag");
    return modid + "_" + quest_node(quest_id) + "_" + flag;
}

std::string objective_type(const std::string& quest_id) {
    return holder_metarole(quest_id) + "_FETCH";
}

// Whether the quest has a moment in the middle that sends Henry back to the giver, and so
// a second objective saying so: the one told, seen, beaten, prayed for, put down, or the
// last of the visits. The graph and the localization both ask this, so they agree.
bool quest_returns(const Quest& quest) {
    if (quest.is_mind()) return quest.count > 0 && !quest.log_check[0].empty();
    if (quest.is_ask() || quest.is_ambush()) return !quest.log_told.empty();
    if (quest.is_travel()) return !quest.log_seen.empty();
    if (quest.is_beat()) return !quest.log_beaten.empty();
    if (quest.is_pray()) return !quest.log_prayed.empty();
    if (quest.is_spot()) return !quest.log_left.empty();
    // A retrieve or a buy: the holder's handover is the mid-moment (notes.md, 5 Oct
    // 2026 -- Father Nicholas gave the hood and the journal still said fetch it). A
    // deliver's the same, the other way round: the receiver has taken the letter (notes.md,
    // 9 Oct 2026 -- Holy Jest delivered and "Deliver the letter" still open).
    if (quest.is_retrieve() || quest.is_buy() || quest.is_deliver()) return quest.has_holder();
    // A find: the count in Henry's pocket (the ItemDescriptorTrigger's target amount;
    // notes.md 6 Oct 2026 -- five marigolds picked and the book still said bring them).
    if (quest.is_find()) return quest.count > 0;
    return false;
}

// A 0..1 item health as the XML and the Lua write it: "0.9", not "0.900000".
std::string health_text(double h) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.3g", h);
    return buf;
}

std::string gathered_log(const Quest& quest) {
    const std::string thing = quest.item_name.empty() ? "what was asked for" : quest.item_name;
    return "I have the " + thing + " " + quest.giver_name + " asked for. Time to take them back.";
}

std::string handed_log(const Quest& quest) {
    std::string thing = quest.item_name;
    if (thing.empty()) {
        thing = "what I came for";
    } else if (thing.compare(0, 4, "the ") != 0 && thing.compare(0, 2, "a ") != 0 &&
               thing.compare(0, 3, "an ") != 0) {
        thing = "the " + thing;
    }
    // A deliver's handover runs the other way: the thing (or the word) went to the one it
    // was for, and the giver is owed the news (notes.md, 9 Oct 2026 -- Holy Jest's letter
    // given at Sedletz and "Deliver the letter" still standing in the book).
    if (quest.is_deliver()) {
        return (quest.carries_word() ? "I gave " + quest.target_name + " " + quest.giver_name + "'s message."
                                     : "I handed " + thing + " to " + quest.target_name + ".") +
               " " + quest.giver_name + " will want to hear it.";
    }
    return "I have " + thing + " from " + quest.target_name + ". I should take it back to " +
           quest.giver_name + ".";
}

// The opening of every menu of ours the player starts (the holder's, the onward
// receiver's, the helper's, the held loop's): no shipped greeting or farewell around it.
const char* const kMenuDialogue =
    "<Dialogue TechnicalStatus=\"Enabled\" Initiator=\"Player\" AllowGreeting=\"false\" AllowFarewell=\"false\">\r\n";

std::string return_objective(const Quest& quest) {
    return "Return to " + quest.giver_name;
}

const std::string* resolution_log(const Quest& quest, const std::string& how) {
    if (how == "persuaded") return &quest.log_completed_persuaded;
    if (how == "threatened") return &quest.log_completed_threatened;
    if (how == "paid") return &quest.log_completed_paid;
    if (how == "stolen") return &quest.log_completed_stolen;
    if (how == "brokered") return &quest.log_completed_brokered;
    if (how == "lied") return &quest.log_completed_lied;
    if (how == "nested") return &quest.log_completed_nested;
    if (how == "kept") return &quest.log_completed_kept;
    if (how == "quiet") return &quest.log_completed_quiet;
    if (how == "fought") return &quest.log_completed_fought;
    if (how == "dead") return &quest.log_completed_dead;
    if (how == "pinned") return &quest.log_completed_pinned;
    if (how == "covered") return &quest.log_completed_covered;
    if (how == "fibbed") return &quest.log_completed_fibbed;
    if (how == "evaded") return &quest.log_completed_evaded;
    if (how == "caught") return &quest.log_completed_caught;
    if (how == "cursed") return &quest.log_completed_cursed;
    if (how == "mocked") return &quest.log_completed_mocked;
    if (how == "exposed") return &quest.log_completed_exposed;
    if (how == "exposed_paid") return &quest.log_completed_exposed_paid;
    if (how == "denounced") return &quest.log_completed_denounced;
    return &quest.log_completed;
}

std::string resolution_state(const std::string& how) {
    // "persuaded" -> "Persuaded": the state names read like the shipped ones
    // (BroughtCorrectCure, AxeShown), and the port they are set from keeps the lowercase.
    std::string out = how;
    if (!out.empty()) out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
    return out;
}

std::string quest_node(const std::string& quest_id) {
    require_identifier(quest_id, "quest id");
    return "q_" + quest_id;
}

std::string routine_tag(const std::string& modid, const std::string& quest_id) {
    require_identifier(quest_id, "quest id");
    return modid + "_routine_" + quest_id;
}

std::string alert_tag(const std::string& modid, const std::string& quest_id) {
    require_identifier(quest_id, "quest id");
    return modid + "_alert_" + quest_id;
}

std::string quest_port(const QuestBeat& beat) {
    require_identifier(beat.command, "quest command");
    return quest_node(beat.quest_id) + "_" + beat.command;
}

std::string quest_variable(const std::string& modid) { return modid + "_quest"; }

std::string quest_set_fingerprint(const std::vector<std::string>& quest_ids) {
    std::vector<std::string> ids = quest_ids;
    std::sort(ids.begin(), ids.end());
    std::string joined;
    for (std::size_t i = 0; i < ids.size(); ++i) {
        if (i) joined += '\n';
        joined += ids[i];
    }
    std::uint64_t hash = 14695981039346656037ULL;
    for (unsigned char c : joined) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    char hex[17];
    std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(hash));
    return hex;
}

std::string quest_key(const std::string& modid, const std::string& quest_id,
                      const std::string& field) {
    return modid + "_" + quest_node(quest_id) + "_" + field;
}

std::string holder_metarole(const std::string& quest_id) {
    require_identifier(quest_id, "quest id");
    std::string upper;
    for (char c : quest_id) upper += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return "MSQ_" + upper;
}

std::string holder_role(const std::string& quest_id) {
    return holder_metarole(quest_id) + "_HOLDER";
}

std::string holder_node(const std::string& quest_id) {
    return quest_node(quest_id) + "_holder";
}

std::string token_class(const Quest& quest) {
    // A pray's token item class: a guid of the mod's own, made from the quest id (twelve
    // hex characters, which is the last group of a guid) under a fixed prefix, so it is
    // stable across builds and collides with nothing shipped. The item row is emitted with
    // it (emit_item_table) and the Lua makes and unmakes one of it (Moonshine.Token).
    require_identifier(quest.id, "quest id");
    std::string tail = quest.id;
    while (tail.size() < 12) tail = "0" + tail;
    return "6d6f6f6e-7368-5000-8000-" + tail.substr(tail.size() - 12);
}

std::string token_name(const Quest& quest) { return "token_" + quest_node(quest.id); }

std::string key_class(const Quest& quest) {
    // A chest's key item class: the token's shape under its own prefix (6b, the letter k),
    // so a quest with both has two classes that collide with nothing.
    require_identifier(quest.id, "quest id");
    std::string tail = quest.id;
    while (tail.size() < 12) tail = "0" + tail;
    return "6d6f6f6e-7368-6b00-8000-" + tail.substr(tail.size() - 12);
}

std::string key_name(const Quest& quest) { return "key_" + quest_node(quest.id); }

std::string taken_class(const Quest& quest) {
    // The chest's taken token: the same shape under 74 (the letter t).
    require_identifier(quest.id, "quest id");
    std::string tail = quest.id;
    while (tail.size() < 12) tail = "0" + tail;
    return "6d6f6f6e-7368-7400-8000-" + tail.substr(tail.size() - 12);
}

std::string taken_name(const Quest& quest) { return "taken_" + quest_node(quest.id); }

std::string hail_class(const Quest& quest) {
    // The hail's token: the same shape under 48 (the letter H). Made by the watch when the
    // giver shouts, so the graph can put the shout in his mouth (RequestMonologue).
    require_identifier(quest.id, "quest id");
    std::string tail = quest.id;
    while (tail.size() < 12) tail = "0" + tail;
    return "6d6f6f6e-7368-4800-8000-" + tail.substr(tail.size() - 12);
}

std::string hail_name(const Quest& quest) { return "hail_" + quest_node(quest.id); }

std::string follow_class(const Quest& quest) {
    // The followed token: the same shape under 46 (the letter F). Made by the Lua when it
    // judges a sighting the graph reported to be the holder catching Henry behind him on
    // his way to the witness (GraphSaid), so the graph can flip his bed and write the line.
    require_identifier(quest.id, "quest id");
    std::string tail = quest.id;
    while (tail.size() < 12) tail = "0" + tail;
    return "6d6f6f6e-7368-4600-8000-" + tail.substr(tail.size() - 12);
}

std::string follow_name(const Quest& quest) { return "follow_" + quest_node(quest.id); }

std::string mark_shown_class(const Quest& quest) {
    // The street night's marker tokens (emit.hpp): the same shape under 4d (the letter M)
    // for the marker put on him and 6d (m) for it taken off. Made by the Lua's WatchMarker
    // as his hours come and go, for the graph's two triggers on the fetch objective.
    require_identifier(quest.id, "quest id");
    std::string tail = quest.id;
    while (tail.size() < 12) tail = "0" + tail;
    return "6d6f6f6e-7368-4d00-8000-" + tail.substr(tail.size() - 12);
}

std::string mark_shown_name(const Quest& quest) { return "shown_" + quest_node(quest.id); }

std::string mark_hidden_class(const Quest& quest) {
    require_identifier(quest.id, "quest id");
    std::string tail = quest.id;
    while (tail.size() < 12) tail = "0" + tail;
    return "6d6f6f6e-7368-6d00-8000-" + tail.substr(tail.size() - 12);
}

std::string mark_hidden_name(const Quest& quest) { return "hidden_" + quest_node(quest.id); }

std::string hail_node(const std::string& quest_id) { return quest_node(quest_id) + "_hail"; }

std::string paper_class(const Quest& quest) {
    // The chest's paper: the same shape under 70 (the letter p).
    require_identifier(quest.id, "quest id");
    std::string tail = quest.id;
    while (tail.size() < 12) tail = "0" + tail;
    return "6d6f6f6e-7368-7000-8000-" + tail.substr(tail.size() - 12);
}

std::string paper_name(const Quest& quest) { return "paper_" + quest_node(quest.id); }

std::string paper_page_key(const std::string& modid, const Quest& quest) {
    return modid + "_" + paper_name(quest) + "_page";
}

std::string helper_metarole(const std::string& quest_id) { return holder_metarole(quest_id) + "_HELPER"; }
std::string helper_role(const std::string& quest_id) { return helper_metarole(quest_id) + "_LETIN"; }
std::string helper_node(const std::string& quest_id) { return quest_node(quest_id) + "_helper"; }

namespace {

// The holder's lines with the gaps filled. The generator writes the holder's own words
// from the holder's own call; until it has, or for a slot it left blank, these plain
// lines keep the errand finishable. They name the parties and the thing and nothing
// else, so they are never wrong, only dull.
Quest with_holder_lines(Quest q) {
    const std::string& owner = q.giver_name;
    const std::string& item = q.item_name;
    auto fill = [](std::string& slot, const std::string& plain) {
        if (slot.empty()) slot = plain;
    };
    if (q.carries_word()) {
        // Word, not a letter: Henry says what he was sent to say, and the one told answers.
        fill(q.receiver_hand, owner + " sent me with a word for you, which he dare not bring himself.");
        fill(q.receiver_hand_reply, "Did he. Say it, then, and I will decide what it is worth.");
        fill(q.holder_leave, "Never mind. Another time.");
        fill(q.holder_leave_reply, "As you like.");
        return q;
    }
    if (q.carries_thing()) {
        // A thing of the game's own brought back to the one it belongs to: no letter to
        // write, and the receiver's menu is his asking how Henry came by it. The rows that
        // make the interrogation are written or there is no interrogation (has_interrogation),
        // so only the door and the way out are filled here.
        fill(q.receiver_hand, "I have something that belongs to you: " + (item.empty() ? std::string("this") : "the " + item) + ".");
        fill(q.receiver_hand_reply, "Have you now. Give it here, and tell me how it comes to be in your hands.");
        fill(q.holder_leave, "Never mind. Another time.");
        fill(q.holder_leave_reply, "As you like.");
        return q;
    }
    if (q.is_deliver()) {
        // The receiver's two rows, and the letter itself when the generator has not
        // written one: a page that says who it is from and to, so it can still be read.
        fill(q.receiver_hand, "I have a letter for you, from " + owner + ".");
        fill(q.receiver_hand_reply, "From " + owner + "? Give it here, then.");
        fill(q.holder_leave, "Never mind. Another time.");
        fill(q.holder_leave_reply, "As you like.");
        fill(q.letter, "To " + q.target_name + ".\nThe bearer of this has it from me, " + owner +
                       ", and is to be trusted with the answer.\n" + owner);
        return q;
    }
    if (q.is_beat()) {
        // The man who must answer: Henry names who sent him and what is asked, the man
        // answers; the argument, the threat, the pay-off (always priced: it is the man's own
        // price, not the giver's), leaving.
        fill(q.holder_open, owner + " sent me. He says you have wronged him, and he wants it made right.");
        fill(q.holder_refuse, "Did he. And he sent a stranger to say so. What of it?");
        fill(q.holder_persuade, "It would cost you a word to make it right, and it is costing you more every day you do not.");
        fill(q.holder_persuade_yes, "All right. Tell him it will be made right. I want no more of this.");
        fill(q.holder_persuade_no, "Go back and tell him to come himself. I have nothing to say to you.");
        fill(q.holder_threaten, "Make it right with him, or I make it right with you, here, and you will not like my way.");
        fill(q.holder_threaten_yes, "All right, all right. Tell him he will have what he wants. No need for that.");
        fill(q.holder_threaten_no, "You do not frighten me. Go and tell him that.");
        fill(q.holder_pay, "Here is silver for your trouble. Make it right with " + owner + " and we are done.");
        fill(q.holder_pay_yes, "For that, I can be reasonable. Tell him it is settled.");
        fill(q.holder_leave, "Never mind. Another time.");
        fill(q.holder_leave_reply, "See that there is no other time.");
        return q;
    }
    if (q.is_ask()) {
        // The one who knows: Henry names who sent him and what he wants to know, the other
        // tells the tale he tells everyone; the argument, the threat and the pay-off (always
        // priced: it is Henry's own silver) each get the same bare truth, and leaving.
        fill(q.holder_open, owner + " sent me. He wants to know a thing he says only you can tell him.");
        fill(q.holder_refuse, "Then he knows all there is, and so do you. Tell him what I have told him.");
        fill(q.holder_persuade, "He will go on guessing, and his guesses are worse than the truth. Tell me, and it stops.");
        fill(q.holder_persuade_yes, "All right. It is as he fears, near enough. Tell him, and tell him I am sorry for it.");
        fill(q.holder_persuade_no, "I have said what I have to say. Go back and tell him that.");
        fill(q.holder_threaten, "Tell me the truth of it, or I tell him the worst of what I can guess, and he believes me.");
        fill(q.holder_threaten_yes, "All right, all right. It is as he fears. There. You have it. Now leave me be.");
        fill(q.holder_threaten_no, "Tell him what you like. He will not believe you over me.");
        fill(q.holder_pay, "Here is silver for your trouble. The truth, and " + owner + " never hears who told me.");
        fill(q.holder_pay_yes, "For that, then. It is as he fears. Keep my name out of it.");
        fill(q.holder_leave, "Never mind. Another time.");
        fill(q.holder_leave_reply, "There is nothing more to tell, now or another time.");
        // The bribe's menu, only drawn when there is one (gift_classes): plain words for a
        // holder the generator has not written these for yet.
        if (!q.gift_classes.empty()) {
            fill(q.holder_gift_take, "I will take them. And he hears nothing from me.");
            fill(q.holder_gift_take_reply, "Good. Wear them in health, and keep your mouth shut.");
            fill(q.holder_gift_refuse, "Keep them. I am not bought.");
            fill(q.holder_gift_refuse_reply, "Not bought? Then have this instead, and tell him what you like!");
        }
        // The word carried on (onward_soul): plain words at the one sent to's, and the
        // journal's for the last leg, until the generator has written them.
        if (q.carries_on()) {
            fill(q.onward_hand, owner + " sent me to you with what he has learned. He dare not bring it himself.");
            fill(q.onward_hand_reply, "Did he. Say it, then, and I will decide what it is worth.");
            fill(q.onward_hand_read, "I see. Well. That is for me to deal with now, and not for you. Go on.");
            fill(q.log_onward, owner + " paid me and asked one thing more: to carry what I had learned to " +
                               q.onward_name + ", since he will not go himself.");
            fill(q.objective_onward, "Tell " + q.onward_name);
        }
        return q;
    }
    if (q.is_mind()) {
        // The one looked in on: Henry says who sent him, and one answer per visit; and the
        // answer to coming back too soon.
        fill(q.target_check, owner + " asked me to look in on you. How are you keeping?");
        fill(q.target_check_reply[0], "Did he. Well, I am still here, as you see. Tell him that.");
        fill(q.target_check_reply[1], "You again. No worse than this morning, and no better. Tell him.");
        fill(q.target_check_reply[2], "Still here. Tell " + owner + " he can stop sending people; I am not dying today.");
        fill(q.target_early, "Just looking in.");
        fill(q.target_early_reply, "You were here not an hour since. Come back later, if you must.");
        return q;
    }
    if (q.is_ambush()) {
        // The one who knows the way: Henry says who sent him and what he is after, the man
        // gives him a way (the wrong one, written down); back too soon, he is sent on; back
        // after the fight, frightened, he gives the right one.
        fill(q.target_direct, owner + " sent me. He says you know of somebody who can help him, and where to find them.");
        fill(q.target_direct_reply, "I might. Here, I will set it down for you. Go there, and go at the hour it says, or they will not see you.");
        fill(q.target_early, "I have not been yet. Is there anything more I should know?");
        fill(q.target_early_reply, "Only what I wrote. Go at the hour, and go alone.");
        fill(q.target_truth, "Your directions led me to three men in the dark. Now the true way, before I lose my temper.");
        fill(q.target_truth_reply, "Christ. You are still standing. All right -- all right. The real one is this, and I never sent you anywhere, you understand?");
        fill(q.letter, "Go to the churchyard when the bell has gone midnight. Wait by the wall. Somebody will come.\nTell nobody.");
        return q;
    }
    if (q.is_buy()) {
        // The one with the price: Henry says who sent him, the holder names the terms --
        // the thing and the sum -- and the pay row is Henry meeting them.
        fill(q.holder_open, owner + " sent me. He says you can help him, and he will pay.");
        fill(q.holder_refuse, "Help him I can. The price is a thing he has worn, and " + std::to_string(q.pay_groschen) +
                              " groschen. Bring both, or do not come back.");
        fill(q.holder_pay, "Here. The " + (q.carry_name.empty() ? std::string("thing") : q.carry_name) + ", and your " +
                           std::to_string(q.pay_groschen) + " groschen.");
        fill(q.holder_pay_yes, "Good. Take this to him. He is to keep it on him and tell nobody what it says.");
        fill(q.holder_leave, "I will come back when I have what you ask.");
        fill(q.holder_leave_reply, "Do.");
        fill(q.letter, "By the three names and the seven, bound and turned back.\nWhat was sent, returns; what was wished, withers.\nCarry this and fear nothing.");
        return q;
    }
    fill(q.holder_open, owner + " says you have his " + item + ". He wants it back.");
    fill(q.holder_refuse, "Does he now. I have not finished with it.");
    fill(q.holder_persuade, "It is his " + item + ". You have had your use of it. Be fair.");
    fill(q.holder_persuade_yes, "Aye, well. Fair is fair. Take it, and tell him I was going to bring it.");
    fill(q.holder_persuade_no, "I will bring it back when I am done with it, and not before.");
    fill(q.holder_threaten, "Hand it over, or I take it off you and " + owner + " gets it either way.");
    fill(q.holder_threaten_yes, "All right, all right. Take the damned thing. No need for that.");
    fill(q.holder_threaten_no, "You do not frighten me. Go and tell him that.");
    if (q.pay_groschen > 0) {
        fill(q.holder_pay, "Here is silver for your trouble. Now give me the " + item + ".");
        fill(q.holder_pay_yes, "For that I can find another. Take it.");
    }
    if (q.dispute) {
        fill(q.holder_broker, "What if I put that to " + owner + " and he owns to it?");
        fill(q.holder_broker_reply, "Do that. He says it, and he can have his " + item + ".");
        fill(q.holder_brokered, owner + " owns to it. Now the " + item + ".");
        fill(q.holder_brokered_yes, "Does he. Well then. Take it to him.");
    }
    if (!q.requires_quest_id.empty()) {
        fill(q.holder_nested, "Your own business is settled, as you asked. Now " + owner + "'s " + item + ".");
        fill(q.holder_nested_yes, "So it is. Fair is fair. Take it to him.");
    }
    fill(q.holder_leave, "Never mind. I will leave it.");
    fill(q.holder_leave_reply, "Do that.");
    // An unyielding holder's refusals are the persuade_no / threaten_no above, said to
    // every attempt. The one who can let Henry in has plain words for the door, the key
    // and the way out until the generator writes hers.
    if (q.is_unyielding()) {
        fill(q.log_rebuffed, q.target_name + " would not hear a word of it and showed me the door. " + owner +
                                 " should know.");
    }
    if (q.has_helper()) {
        const std::string& who = q.target_name;
        fill(q.helper_open, "You keep house here? " + who + " lodges under this roof.");
        fill(q.helper_open_reply, "He does. He is out. What is it to you?");
        fill(q.helper_door_yes, "Keep your voice down. The door is on the latch. I never saw you.");
        if (q.has_key()) {
            fill(q.helper_key_yes, "Wait here. (She is gone a moment, and comes back with a key.) If he asks, "
                                   "I never saw you.");
            fill(q.helper_key_no, "No. The door I can forget to bar; a key is my neck. Go.");
        }
        fill(q.helper_leave, "Never mind. Forget I asked.");
        fill(q.helper_leave_reply, "Already have.");
        fill(q.log_letin, q.helper_name + " took the money and will leave the door on the latch while " + who +
                              " is out.");
        if (q.has_key()) {
            fill(q.log_keyed, q.helper_name + " got me the key to " + who + "'s chest. It cost me dear, but it spares "
                                  "me the lock.");
        }
    }
    return q;
}

// The nodes that make a travel's arrival a fight, appended inside the quest's <Nodes>
// after the `seen` DistanceTrigger they hang off. The shape is the one proven on the Windows laptop
// (behaviors.md §7): a relation of -1 between the brawlers and the target, the contexts
// that keep it bare-handed and nobody's business, and the game's own first-punch message
// (`crime:attackInitiatedByConcept`, what the shipped utils/combat/fightstart sends) to
// every brawler a few seconds on -- the relation alone starts nothing. The shipped
// skirmish modules add an EnableBehavior on a level SmartArea a mod cannot bind; the
// message is enough without it.
//
// One bool state, `brawl`, is the whole fight: true on the match, false at the first
// knock-out on either side or when the timer runs out, and every effect node reverts
// with it, so the same edge that ends the fight puts the relation and contexts back.
// StopFight on its OnFalse is what makes the last man standing stop swinging at a body
// (seen in the probe from the knock-out at 55 s until the timer at 100 s). The waking
// reaction is held down for the aftermath separately, since the loser comes round after
// the fight is over.
//
// An ambush (set-aside-stories.md §5) is the same module with Henry for the victim: the
// start edge is the token trigger's OnAcquire (the Lua has him at the place in the hour),
// the other side is the `player` asset, the contexts leave out the invisibility (he is in
// it) and the never-surrender (robbers run), and the knock-out trigger covers him too, so
// his going down ends it as a brawler's does. The waking suppression on the brawlers keeps
// them from coming round shouting robbery at the man they set on.
std::string emit_brawl(const Quest& quest, bool seen) {
    const bool ambush = quest.is_ambush();
    if (!seen) {
        throw std::runtime_error("quest " + quest.id + " stages a brawl but has no " +
                                 (ambush ? "log_ambushed" : "log_seen") + " to hang it on");
    }
    const std::string start = ambush ? "ambushed.OnAcquire" : "seen.OnMatch";
    const std::string victim = ambush ? "player" : "target";
    const int seconds = ambush ? kAmbushSeconds : kBrawlSeconds;
    auto t = [](int n) { return std::string(static_cast<std::size_t>(n), '\t'); };
    std::string s;
    s += t(4) + "<State Name=\"brawl\" TypeT=\"bool\">\r\n";
    s += t(5) + "<Edge From=\"" + start + "\" To=\"SetTrue\" />\r\n";
    s += t(5) + "<Edge From=\"brawlKo.OnUnconscious\" To=\"SetFalse\" />\r\n";
    s += t(5) + "<Edge From=\"brawlStop.OnFinished\" To=\"SetFalse\" />\r\n";
    s += t(4) + "</State>\r\n";
    s += t(4) + "<Timer Name=\"brawlStop\">\r\n";
    s += t(5) + "<Constant Name=\"Duration\" Value=\"" + std::to_string(seconds) + "s\" />\r\n";
    s += t(5) + "<Constant Name=\"TimeType\" Value=\"GameTime\" />\r\n";
    s += t(5) + "<Edge From=\"" + start + "\" To=\"SetRunning\" />\r\n";
    s += t(4) + "</Timer>\r\n";
    // Everybody in the fight, for the contexts, the knock-out trigger and the stop.
    s += t(4) + "<Function Name=\"brawlAll\" TypeT=\"wh::rpgmodule::Souls\" "
                "MethodName=\"utilities::array::JoinArrays\" DeclaringType=\"utilities::array\">\r\n";
    s += t(5) + "<Asset Name=\"A\" Alias=\"brawlers\" />\r\n";
    s += t(5) + "<Asset Name=\"B\" Alias=\"" + victim + "\" />\r\n";
    s += t(4) + "</Function>\r\n";
    s += t(4) + "<AddFactionRelationBetweenArrays Name=\"brawlHostile\">\r\n";
    s += t(5) + "<Constant Name=\"RelationValue\" Value=\"-1\" />\r\n";
    s += t(5) + "<Asset Name=\"SoulArray0\" Alias=\"brawlers\" />\r\n";
    s += t(5) + "<Asset Name=\"SoulArray1\" Alias=\"" + victim + "\" />\r\n";
    s += t(5) + "<Edge From=\"brawl.State\" To=\"IsActive\" />\r\n";
    s += t(4) + "</AddFactionRelationBetweenArrays>\r\n";
    // And on one side among themselves, so that two brawlers who happen to be enemies do
    // not take the occasion.
    s += t(4) + "<AddFactionRelationWithinArray Name=\"brawlFriends\">\r\n";
    s += t(5) + "<Constant Name=\"RelationValue\" Value=\"1\" />\r\n";
    s += t(5) + "<Asset Name=\"SoulArray\" Alias=\"brawlers\" />\r\n";
    s += t(5) + "<Edge From=\"brawl.State\" To=\"IsActive\" />\r\n";
    s += t(4) + "</AddFactionRelationWithinArray>\r\n";
    int n = 0;
    auto context_node = [&](const char* context, const char* souls) {
        s += t(4) + "<SetEntityContext Name=\"brawlContext" + std::to_string(++n) + "\">\r\n";
        s += t(5) + "<Constant Name=\"Context\" Value=\"" + std::string(context) + "\" />\r\n";
        if (std::string(souls) == "brawlAll") {
            s += t(5) + "<Edge From=\"brawlAll.Array\" To=\"Souls\" />\r\n";
        } else {
            s += t(5) + "<Asset Name=\"Souls\" Alias=\"" + std::string(souls) + "\" />\r\n";
        }
        s += t(5) + "<Edge From=\"brawl.State\" To=\"IsActive\" />\r\n";
        s += t(4) + "</SetEntityContext>\r\n";
    };
    if (ambush) {
        // The contexts go on the brawlers only: Henry fights as the player always does.
        for (const char* context : kAmbushContexts) context_node(context, "brawlers");
    } else {
        for (const char* context : kBrawlContexts) context_node(context, "brawlAll");
    }
    // The first punch: each brawler is told to go for the target, a moment after the
    // relation has taken (it reads -1 within the same tick, but the shipped fightstart
    // is never sent in the frame the relation is set either).
    s += t(4) + "<Timer Name=\"brawlKick\">\r\n";
    s += t(5) + "<Constant Name=\"Duration\" Value=\"" + std::to_string(kBrawlKickSeconds) + "s\" />\r\n";
    s += t(5) + "<Constant Name=\"TimeType\" Value=\"GameTime\" />\r\n";
    s += t(5) + "<Edge From=\"" + start + "\" To=\"SetRunning\" />\r\n";
    s += t(4) + "</Timer>\r\n";
    s += t(4) + "<ForEach Name=\"brawlEach\" TypeT=\"wh::rpgmodule::Souls\">\r\n";
    s += t(5) + "<Asset Name=\"Array\" Alias=\"brawlers\" />\r\n";
    s += t(5) + "<Edge From=\"brawlKick.OnFinished\" To=\"Begin\" />\r\n";
    s += t(4) + "</ForEach>\r\n";
    s += t(4) + "<FirstElement Name=\"brawlVictim\" TypeT=\"wh::rpgmodule::Souls\">\r\n";
    s += t(5) + "<Asset Name=\"Array\" Alias=\"" + victim + "\" />\r\n";
    s += t(4) + "</FirstElement>\r\n";
    s += t(4) + "<InstantSendMessage Name=\"brawlAttack\" MessageType=\"crime:attackInitiatedByConcept\">\r\n";
    s += t(5) + "<Edge From=\"brawlEach.Value\" To=\"Receiver\" />\r\n";
    s += t(5) + "<Edge From=\"brawlVictim.Value\" To=\"Content_target\" />\r\n";
    s += t(5) + "<Constant Name=\"Content_priorityTarget\" Value=\"true\" />\r\n";
    s += t(5) + "<Edge From=\"brawlEach.OnValue\" To=\"Exec\" />\r\n";
    s += t(4) + "</InstantSendMessage>\r\n";
    // The end: the first one down on either side, or the clock.
    s += t(4) + "<SoulUnconsciousTrigger Name=\"brawlKo\">\r\n";
    s += t(5) + "<Edge From=\"brawlAll.Array\" To=\"Souls\" />\r\n";
    s += t(5) + "<Edge From=\"brawl.State\" To=\"IsActive\" />\r\n";
    s += t(4) + "</SoulUnconsciousTrigger>\r\n";
    s += t(4) + "<Function Name=\"brawlStopFight\" MethodName=\"wh::rpgmodule::StopFight\" "
                "DeclaringType=\"wh::rpgmodule\">\r\n";
    s += t(5) + "<Edge From=\"brawl.OnFalse\" To=\"Exec\" />\r\n";
    s += t(5) + "<Edge From=\"brawlAll.Array\" To=\"Souls\" />\r\n";
    s += t(4) + "</Function>\r\n";
    // The aftermath: the loser wakes into the game's robbed-and-beaten reaction unless told
    // not to (the shipped rvacka event holds this context over its fighters for the same
    // reason), and he wakes after the fight is over, so this outlasts `brawl`.
    s += t(4) + "<State Name=\"brawlAftermath\" TypeT=\"bool\">\r\n";
    s += t(5) + "<Edge From=\"" + start + "\" To=\"SetTrue\" />\r\n";
    s += t(5) + "<Edge From=\"brawlAftermathOver.OnFinished\" To=\"SetFalse\" />\r\n";
    s += t(4) + "</State>\r\n";
    s += t(4) + "<Timer Name=\"brawlAftermathOver\">\r\n";
    s += t(5) + "<Constant Name=\"Duration\" Value=\"" + std::to_string(kBrawlAftermathMinutes) + "m\" />\r\n";
    s += t(5) + "<Constant Name=\"TimeType\" Value=\"GameTime\" />\r\n";
    s += t(5) + "<Edge From=\"" + start + "\" To=\"SetRunning\" />\r\n";
    s += t(4) + "</Timer>\r\n";
    s += t(4) + "<SetEntityContext Name=\"brawlWaking\">\r\n";
    s += t(5) + "<Constant Name=\"Context\" Value=\"crime_suppressReactionAfterGainingConsciousness\" />\r\n";
    if (ambush) {
        s += t(5) + "<Asset Name=\"Souls\" Alias=\"brawlers\" />\r\n";
    } else {
        s += t(5) + "<Edge From=\"brawlAll.Array\" To=\"Souls\" />\r\n";
    }
    s += t(5) + "<Edge From=\"brawlAftermath.State\" To=\"IsActive\" />\r\n";
    s += t(4) + "</SetEntityContext>\r\n";
    return s;
}

}  // namespace

std::string emit_dialogue_project(const std::string& modid) {
    // Shipped projects carry PositionX/PositionY on each node for the Skald
    // canvas. They are editor state, so they are left off here.
    std::string s = skald_header(modid);
    s += "\t\t<Project Name=\"" + xml_escape(modid) + "\">\r\n";
    s += "\t\t\t<Definitions>\r\n";
    s += "\t\t\t\t<Definition File=\"" + xml_escape(modid + "/" + std::string(kQuestLevel) + ".xml") +
         "\" />\r\n";
    s += "\t\t\t</Definitions>\r\n";
    s += "\t\t\t<Nodes>\r\n";
    s += "\t\t\t\t<" + std::string(kQuestLevel) + " Name=\"" + std::string(kQuestLevel) + "\" />\r\n";
    s += "\t\t\t</Nodes>\r\n";
    s += "\t\t</Project>\r\n";
    s += "\t</Skald>\r\n";
    s += "</Database>\r\n";
    return s;
}

std::string emit_quest_level(const std::string& modid,
                             const std::string& dialogue_name,
                             const std::vector<Quest>& quests) {
    std::string s = skald_header(modid);
    // The Level, under the Project above, as the shipped concept_test project nests its
    // own (Quests/Testing/daniel.mikes/concept_test). A Level as the *root* of the mod's
    // graph was tried first (4 Oct 2026), with and without HibernateMode="Auto": the game
    // loaded it and ran the errand the same, and the journal still said "Different
    // region", so a root is not a level to the journal whatever it is called. No
    // HibernateMode: the shipped Levels' "Auto" would put the graph to sleep -- the
    // dialogue with it -- whenever Kuttenberg is not the level loaded.
    s += "\t\t<Level Name=\"" + std::string(kQuestLevel) + "\">\r\n";
    s += "\t\t\t<Definitions>\r\n";
    const std::string dir = std::string(kQuestLevel) + "/";
    s += "\t\t\t\t<Definition File=\"" + xml_escape(dir + dialogue_name + ".xml") + "\" />\r\n";
    // One file and one node per run (stages.md): a quest of one scene as before, a story's
    // same-giver scenes gathered under their head's name.
    const std::vector<QuestRun> runs = quest_runs(quests);
    for (const QuestRun& run : runs) {
        s += "\t\t\t\t<Definition File=\"" + xml_escape(dir + quest_node(run.legs.front()->id) + ".xml") + "\" />\r\n";
    }
    s += "\t\t\t</Definitions>\r\n";
    s += "\t\t\t<Nodes>\r\n";
    s += "\t\t\t\t<" + dialogue_name + " Name=\"" + dialogue_name + "\" />\r\n";
    // Every quest hangs off the one dialogue: an Edge from the port a sequence fires
    // to the In port of the quest it belongs to, the way a shipped Level wires its
    // dialogues into its quests. From is node.port, To is the port on this node.
    // A scene's link is told when the favour before it is done the way it wants, for its
    // giver's marker: the before's `done` ports (done, done_truth, done_<untrue way>)
    // feed the link's `before_open`, through an Or here when the link follows more than
    // one way. A way the before cannot end is left out; a link whose before is not among
    // the quests is left unwired, and its marker never shows, as the Lua never offers it.
    std::vector<std::string> ors;
    const auto before_sources = [&](const Quest& q) {
        std::vector<std::string> out;
        const auto before = std::find_if(quests.begin(), quests.end(),
                                         [&](const Quest& other) { return other.id == q.after_quest_id; });
        if (before == quests.end()) return out;
        // The before's ports sit on its run's node: bare on a run of one scene, prefixed
        // with the scene on a run of several.
        const Quest& before_head = run_head(runs, before->id);
        const bool before_multi = before_head.id != before->id || [&] {
            for (const QuestRun& run : runs) {
                if (run.legs.front()->id == before_head.id) return run.legs.size() > 1;
            }
            return false;
        }();
        const std::string from = quest_node(before_head.id) + "." + (before_multi ? quest_node(before->id) + "_done" : "done");
        if (q.after_how.empty()) {
            out.push_back(from);
            return out;
        }
        const std::vector<std::string> ways = before->resolutions();
        std::size_t start = 0;
        while (start <= q.after_how.size()) {
            const std::size_t bar = q.after_how.find('|', start);
            const std::string token = q.after_how.substr(start, bar == std::string::npos ? std::string::npos : bar - start);
            if (token == "truth") {
                out.push_back(from + "_truth");
            } else if (!token.empty() && std::find(ways.begin(), ways.end(), token) != ways.end() &&
                       untrue_how(token)) {
                out.push_back(from + "_" + token);
            }
            if (bar == std::string::npos) break;
            start = bar + 1;
        }
        return out;
    };
    for (const QuestRun& run : runs) {
        const Quest& q = *run.legs.front();
        const bool multi = run.legs.size() > 1;
        const std::string node = quest_node(q.id);
        // RequiredForOutput names the Level the node is exported with, as on every quest's
        // instance node in the shipped Levels (<bratriZCimburka Name="…"
        // RequiredForOutput="kutnohorsko">). It is the enclosing Level's name, always: the
        // five Trosky givers' errands sit in this Level too (one dialogue instance, so one
        // Level for its edges), and a node tagged for another level than its own has no
        // measured meaning. On its own it did nothing for the journal's region; the Level did.
        s += "\t\t\t\t<" + node + " Name=\"" + node + "\" RequiredForOutput=\"" + kQuestLevel + "\">\r\n";
        for (const Quest* leg : run.legs) {
            for (const std::string& command : quest_commands(*leg)) {
                const std::string port = quest_port({leg->id, command, 0});
                s += "\t\t\t\t\t<Edge From=\"" + dialogue_name + "." + port + "\" To=\"" +
                     (multi ? port : command) + "\" />\r\n";
            }
        }
        // A run's head may follow another giver's scene; its legs follow only each other.
        if (q.is_link()) {
            std::vector<std::string> sources = before_sources(q);
            if (sources.size() == 1) {
                s += "\t\t\t\t\t<Edge From=\"" + sources[0] + "\" To=\"before_open\" />\r\n";
            } else if (sources.size() > 1) {
                // Or'd two at a time in nodes of this project's own, named for the link.
                std::string any = sources[0];
                for (std::size_t i = 1; i < sources.size(); ++i) {
                    const std::string name = "open_" + q.id + "_" + std::to_string(i);
                    ors.push_back("\t\t\t\t<Function Name=\"" + name +
                                  "\" MethodName=\"math::boolean::Or\" DeclaringType=\"math::boolean\">\r\n"
                                  "\t\t\t\t\t<Edge From=\"" + any + "\" To=\"A\" />\r\n"
                                  "\t\t\t\t\t<Edge From=\"" + sources[i] + "\" To=\"B\" />\r\n"
                                  "\t\t\t\t</Function>\r\n");
                    any = name + ".bool";
                }
                s += "\t\t\t\t\t<Edge From=\"" + any + "\" To=\"before_open\" />\r\n";
            }
        }
        s += "\t\t\t\t</" + node + ">\r\n";
    }
    for (const std::string& node : ors) s += node;
    s += "\t\t\t</Nodes>\r\n";
    s += "\t\t</Level>\r\n";
    s += "\t</Skald>\r\n";
    s += "</Database>\r\n";
    return s;
}

namespace {

std::string replace_all(std::string text, const std::string& from, const std::string& to) {
    std::size_t at = 0;
    while ((at = text.find(from, at)) != std::string::npos) {
        text.replace(at, from.size(), to);
        at += to.size();
    }
    return text;
}

// Whether an ending on a hand-in port leads on to a scene gated by `after_how` -- the
// Level's before_sources rule, read from the port's side: "" is the plain complete (the
// truth, as far as the graph can tell: every untrue way with a journal line has a port of
// its own), a way is itself, and an empty gate follows any ending.
bool port_leads_on(const std::string& how, const std::string& after_how) {
    if (after_how.empty()) return true;
    std::size_t start = 0;
    while (start <= after_how.size()) {
        const std::size_t bar = after_how.find('|', start);
        const std::string token = after_how.substr(start, bar == std::string::npos ? std::string::npos : bar - start);
        if (token == "truth") {
            if (how != "lied" && how != "quiet" && how != "kept" && how != "dead") return true;
        } else if (!token.empty() && token == how) {
            return true;
        }
        if (bar == std::string::npos) break;
        start = bar + 1;
    }
    return false;
}

}  // namespace

std::vector<QuestRun> quest_runs(const std::vector<Quest>& quests) {
    // A scene joins its predecessor's run when the giver is the same; the head of a run
    // is a scene with no such predecessor. Runs in slate order of their heads, legs in link
    // order from the head (a branch's legs side by side, in slate order).
    std::map<std::string, const Quest*> by_id;
    for (const Quest& q : quests) by_id[q.id] = &q;
    const auto joins = [&](const Quest& q) -> const Quest* {
        if (q.after_quest_id.empty()) return nullptr;
        const auto it = by_id.find(q.after_quest_id);
        if (it == by_id.end() || it->second->giver_soul != q.giver_soul) return nullptr;
        return it->second;
    };
    std::vector<QuestRun> runs;
    for (const Quest& q : quests) {
        if (joins(q)) continue;
        QuestRun run;
        run.legs.push_back(&q);
        for (std::size_t i = 0; i < run.legs.size(); ++i) {
            for (const Quest& other : quests) {
                if (joins(other) == run.legs[i]) run.legs.push_back(&other);
            }
            if (run.legs.size() > quests.size()) throw std::runtime_error("scene links loop at " + q.id);
        }
        runs.push_back(std::move(run));
    }
    return runs;
}

const Quest& run_head(const std::vector<QuestRun>& runs, const std::string& quest_id) {
    for (const QuestRun& run : runs) {
        for (const Quest* leg : run.legs) {
            if (leg->id == quest_id) return *run.legs.front();
        }
    }
    throw std::runtime_error("quest " + quest_id + " is in no run");
}

namespace {

// One scene's share of a quest file: everything but the run's own nodes (questState, the
// giver's death, the household, the routine, the giver's marker, the Output). A quest of
// one scene is a run of one leg, and its text is byte for byte what it was before runs
// (stages.md); a run of several gets each leg's names suffixed (suffix_leg) and its ports
// the talk's own.
struct LegCtx {
    bool multi = false;
    // Whether the ending on this port ("" for the plain complete, else the way) leads on
    // to another scene of the run -- then the run is not done and the return stands.
    std::function<bool(const std::string&)> leads_on = [](const std::string&) { return false; };
    // The accept ports of the scenes that follow this one, which close its return.
    std::vector<std::string> follower_accepts;
};

struct LegOut {
    std::string before;       // objState .. the onward nodes
    std::string after;        // targetDeath .. the holder
    std::string done_logic;   // isDone_<how>, untrue_N, not_untrue, isDoneTruth
    std::string types;        // the StateTypeEnumerations of the leg's Type
    std::string assets_a;     // target, onward, keepsake
    std::string assets_b;     // brawlers
    std::string objectives;
    std::string definitions;  // the holder's and the onward's files
    std::vector<std::string> in_ports, out_ports;
    std::vector<std::string> done_edges;  // Output edges, "<from> <to>"
    bool wants_player = false;
    std::vector<std::string> ways;
    std::vector<std::string> untrue_ways;
    std::vector<std::string> complete_ports;  // the leg's complete* ports, as named
};

LegOut emit_leg(const std::string& modid, const Quest& quest, const LegCtx& ctx) {
    LegOut out;
    if (quest.giver_guid.empty() ||
        (quest.target_guid.empty() && !quest.is_find() && !quest.is_pray() && !quest.is_spot())) {
        throw std::runtime_error("quest " + quest.id + " needs both souls' guids");
    }
    // A find has nobody at the other end: no holder menu, no receiver, no default item,
    // and the marker sits on the giver, since the thing is wherever Henry finds it. A
    // travel has somebody at the other end but no dealings with them: the target is the
    // marker and the DistanceTrigger's origin, and nothing else -- no holder menu, no
    // item at all.
    // A beat has somebody at the other end with a menu of his own and no thing: the
    // SoulUnconsciousTrigger on him is the moment the journal marks. A pray has nobody and
    // nothing: the Lua's token item in the player's pocket is what the graph sees. A mind has
    // somebody with a menu, and its Out ports are the visits. A leave or a return is a
    // pray with a thing in Henry's pocket (made at accept, as a letter is) and a place to
    // put it down; the Lua's token is again what the graph sees, and the marker stays on
    // the giver (nothing can be added to the level, so the place is named in words).
    const bool find = quest.is_find();
    const bool travel = quest.is_travel();
    const bool beat = quest.is_beat();
    const bool pray = quest.is_pray();
    const bool spot = quest.is_spot();
    const bool mind = quest.is_mind();
    const bool ask = quest.is_ask();
    // An ambush has somebody at the other end with a three-stage menu and a Document (the
    // directions, made in Henry's pocket by the Lua on the first stage's exit); the Lua's
    // token at the place in the hour is what the graph sees, and it sets the brawlers on
    // the player. A buy is a retrieve with a two-part price: the charm is seeded on the
    // holder as any retrieve's thing is, and the menu is the holder's own.
    const bool ambush = quest.is_ambush();
    const bool holder = quest.has_holder();
    const bool thingless = travel || beat || pray || mind || ask || quest.carries_word();
    // A find with a keepsake is marked on the keepsake itself, lying where the things grow
    // (an ItemOnGroundAsset, behaviors.md §10.4; the Lua lays it at accept): the first
    // marker on a place rather than a soul.
    const bool keepsake = quest.has_keepsake();
    const char* marker = keepsake ? "keepsake" : (find || pray || spot) ? "giver" : "target";
    if (quest.item_class.empty() && !thingless) {
        throw std::runtime_error("quest " + quest.id + " names no item class");
    }
    auto key = [&](const char* field) { return xml_escape(quest_key(modid, quest.id, field)); };
    // The port a command arrives on: bare in a quest of one scene, the talk's own
    // q_<id>_<command> in a run of several (the Level wires like to like).
    auto P = [&](const std::string& command) {
        return ctx.multi ? quest_port({quest.id, command, 0}) : command;
    };
    // What the leg's own nodes read for "this errand is on": the run's questState in a
    // quest of one scene, the leg's own bool in a run (legActive, suffixed below).
    const std::string active_bool = ctx.multi ? "legActive.State" : "questState.Active";
    const std::string on_active = ctx.multi ? P("accept") : "questState.OnActive";
    const std::string done_bool = ctx.multi ? "legDone.State" : "questState.Done";
    auto t = [](int n) { return std::string(static_cast<std::size_t>(n), '\t'); };

    const std::vector<std::string> commands = quest_commands(quest);
    const std::vector<std::string> ways = quest.resolutions();
    const std::vector<std::string> untrue_ways = [&] {
        std::vector<std::string> list;
        for (const std::string& how : ways) {
            if (untrue_how(how)) list.push_back(how);
        }
        return list;
    }();
    out.ways = ways;
    out.untrue_ways = untrue_ways;
    for (const std::string& command : commands) {
        out.in_ports.push_back(P(command));
        if (command != "accept" && command != "onward" && command != "abandon") out.complete_ports.push_back(P(command));
    }
    // What fires the leg's reward and save: the quest's Done in a quest of one scene, the
    // leg's own hand-in ports in a run (the run is done only at its last).
    const std::vector<std::string> on_done =
        ctx.multi ? out.complete_ports : std::vector<std::string>{"questState.OnDone"};
    const bool onward = quest.carries_on();
    if (holder) out.definitions += t(4) + "<Definition File=\"" + xml_escape(holder_node(quest.id) + ".xml") + "\" />\r\n";
    if (onward) out.definitions += t(4) + "<Definition File=\"" + xml_escape(onward_node(quest.id) + ".xml") + "\" />\r\n";
    if (quest.has_helper()) {
        out.definitions += t(4) + "<Definition File=\"" + xml_escape(helper_node(quest.id) + ".xml") + "\" />\r\n";
    }
    if (!quest.hail.empty()) {
        out.definitions += t(4) + "<Definition File=\"" + xml_escape(hail_node(quest.id) + ".xml") + "\" />\r\n";
    }
    const std::string rfo = std::string(" RequiredForOutput=\"") + kQuestLevel + "\"";
    std::string s;
    // The one objective follows the quest state, and its own State is what the logs hang
    // off. Its type is the quest's own (Types, below), the way the shipped quests write
    // a journal line per ending (nemocnaDevecka's BroughtWrongCure / BroughtCorrectCure
    // are both Completed): Started when the quest goes Active, one Completed per way
    // the thing was won -- set straight from the complete port that says which, since
    // the quest's own OnDone cannot -- and Canceled on Failed. Updated, with the holder's
    // name still on the marker, when the holder dies with the thing on him.
    s += t(4) + "<State Name=\"objState\"" + rfo + " TypeT=\"" + xml_escape(objective_type(quest.id)) + "\">\r\n";
    s += t(5) + "<Edge From=\"" + on_active + "\" To=\"SetActive\" />\r\n";
    s += t(5) + "<Edge From=\"" + P("complete") + "\" To=\"SetDone\" />\r\n";
    for (const std::string& how : ways) {
        s += t(5) + "<Edge From=\"" + P("complete_" + how) + "\" To=\"Set" + resolution_state(how) + "\" />\r\n";
    }
    if (!quest.log_target_dead.empty()) {
        s += t(5) + "<Edge From=\"targetDeath.OnDeath\" To=\"SetTargetDead\" />\r\n";
    }
    // Given up to the giver's face (the app's `abandon`): the objective's own Canceled
    // line, and the quest's failure follows (questState below) for everything else.
    s += t(5) + "<Edge From=\"" + P("abandon") + "\" To=\"SetAbandoned\" />\r\n";
    // A letter read on the way: the seal is broken the moment the page opens, and the
    // objective says so (Updated, log_opened), whoever it was meant for.
    const bool opened = quest.is_deliver() && !quest.log_opened.empty();
    if (opened) {
        s += t(5) + "<Edge From=\"opened.OnOpened\" To=\"SetOpened\" />\r\n";
    }
    // A travel's arrival: Henry within a dozen paces of the one he was sent to look at,
    // and the objective says so (Updated, log_seen) with the marker back on the giver.
    const bool seen = travel && !quest.log_seen.empty();
    if (seen) {
        s += t(5) + "<Edge From=\"seen.OnMatch\" To=\"SetSeen\" />\r\n";
    }
    // A beat's moment: the man goes down, and the objective says so (Updated, log_beaten)
    // with the marker back on the giver.
    const bool beaten = beat && !quest.log_beaten.empty();
    if (beaten) {
        s += t(5) + "<Edge From=\"beaten.OnUnconscious\" To=\"SetBeaten\" />\r\n";
    }
    // A pray's: the token the Lua makes when Henry has knelt (Updated, log_prayed).
    const bool prayed = pray && !quest.log_prayed.empty();
    // A leave's or a return's: the token the Lua makes when the thing is put down (Updated,
    // log_left).
    const bool left = spot && !quest.log_left.empty();
    if (prayed) {
        s += t(5) + "<Edge From=\"prayed.OnAcquire\" To=\"SetPrayed\" />\r\n";
    }
    if (left) {
        s += t(5) + "<Edge From=\"left.OnAcquire\" To=\"SetLeft\" />\r\n";
    }
    // An ask's moment: the one asked gives up the truth, which is the menu's handover port
    // (whichever of persuade, threaten or pay won it), and the objective says so (Updated,
    // log_told) with the marker back on the giver.
    const bool told = (ask || ambush) && !quest.log_told.empty();
    if (told) {
        s += t(5) + "<Edge From=\"holder.handover\" To=\"SetTold\" />\r\n";
    }
    // A retrieve's or a buy's mid-moment: the holder has given the thing up, and the
    // journal says so (Updated, log_handed) with the marker back on the giver. A
    // deliver's: the receiver has taken it (its menu's handover port is the same).
    const bool handed = (quest.is_retrieve() || quest.is_buy() || quest.is_deliver()) && holder;
    if (handed) {
        s += t(5) + "<Edge From=\"holder.handover\" To=\"SetHanded\" />\r\n";
        // Or taken out of the chest (the watch's taken token; notes.md 14 Oct 2026).
        if (quest.has_chest()) s += t(5) + "<Edge From=\"taken.OnAcquire\" To=\"SetHanded\" />\r\n";
    }
    // An unyielding holder's refusal (In the Stocks): the menu's `rebuffed` port, the
    // journal's Updated line with the marker back on the giver. With a helper, the giver's
    // naming of them (the Lua's token on the app's `named` directive: Updated, log_named,
    // the optional objective opened), the door bought (the helper menu's `door` port:
    // Updated, log_letin) and the key got (its `key` port: Updated, log_keyed).
    const Quest filled = with_holder_lines(quest);
    const bool rebuffed = quest.is_unyielding();
    const bool named = quest.has_helper();
    const bool keyed = quest.has_key();
    if (rebuffed) s += t(5) + "<Edge From=\"holder.rebuffed\" To=\"SetRebuffed\" />\r\n";
    if (named) {
        s += t(5) + "<Edge From=\"named.OnAcquire\" To=\"SetNamed\" />\r\n";
        s += t(5) + "<Edge From=\"helperTalk.door\" To=\"SetLetin\" />\r\n";
        if (keyed) s += t(5) + "<Edge From=\"helperTalk.key\" To=\"SetKeyed\" />\r\n";
    }
    // A dictated letter written (the Lua's token on the app's `wrote_<digits>` directive,
    // the naming's shape): Updated, log_written, the marker on the receiver.
    const bool written = quest.has_written_log();
    if (written) s += t(5) + "<Edge From=\"written.OnAcquire\" To=\"SetWritten\" />\r\n";
    // An exposure's holder caught where he should not be (the Lua's token when Henry finds
    // him with the witness in the night hours: WatchExpose): Updated, log_caught, the marker
    // staying on the holder, who is now to be faced with it.
    const bool caught = quest.has_expose();
    if (caught) s += t(5) + "<Edge From=\"caught.OnAcquire\" To=\"SetCaught\" />\r\n";
    // The holder turning on Henry behind him in the street (the Lua's token on a sighting
    // the graph's PerceptionTrigger reported and the Lua judged: GraphSaid): Updated,
    // log_followed, the marker staying on him.
    const bool followed = quest.has_follow();
    if (followed) s += t(5) + "<Edge From=\"followed.OnAcquire\" To=\"SetFollowed\" />\r\n";
    // The street night's marker on him and off again (emit.hpp mark_shown_class): the two
    // tokens the Lua makes as his hours come and go (WatchMarker), through the triggers
    // below, live while there is a night still to be caught (marking).
    if (followed) {
        s += t(5) + "<Edge From=\"markShown.OnAcquire\" To=\"SetShown\" />\r\n";
        s += t(5) + "<Edge From=\"markHidden.OnAcquire\" To=\"SetHidden\" />\r\n";
    }
    // The bedside's denouncing (the holder menu's `denounced` port): Henry walked out with
    // nothing, and the giver is to be told so (Updated, log_denounced, the marker on her).
    const bool bedside = quest.has_bedside();
    if (bedside) s += t(5) + "<Edge From=\"holder.denounced\" To=\"SetDenouncing\" />\r\n";
    // A find's mid-moment: the count of the thing in Henry's pocket reaches what was asked
    // (the ItemDescriptorTrigger below, the shape of the shipped zasobovani/sber_raku).
    // The gathering objective is Completed at that moment (log_gathered), the way the
    // game's own collecting errands tick the gather off and open the return, so the
    // "5/5" does not stand as an open objective beside "Return to Joan" (notes.md,
    // 7 Oct 2026). Dropping below the count afterwards changes nothing: the trigger is
    // switched off at the same moment (the `gathering` state below), as the shipped
    // graph's hidden Progress state switches its own off, so the hand-in's taking of
    // the things cannot flash "1/5" on the HUD while the quest closes.
    const bool gathered = find && quest.count > 0;
    if (gathered) {
        s += t(5) + "<Edge From=\"gathered.OnTargetAmountAcquire\" To=\"SetGathered\" />\r\n";
    }
    // And, for an ask whose word is carried on, the giver told and Henry sent on (Updated,
    // log_onward) with the marker on the one he is sent to; the last objective (below)
    // opens off the same port.
    if (onward) {
        s += t(5) + "<Edge From=\"" + P("onward") + "\" To=\"SetOnward\" />\r\n";
    }
    // An ambush's two moments before that: the directions given (the menu's first stage,
    // its own Out port: Updated, log_directed) and the trap sprung (the token the Lua makes
    // with Henry at the place in the hour: Updated, log_ambushed).
    const bool directed = ambush && !quest.log_directed.empty();
    const bool ambushed = ambush && !quest.log_ambushed.empty();
    if (directed) {
        s += t(5) + "<Edge From=\"holder.directed\" To=\"SetDirected\" />\r\n";
    }
    if (ambushed) {
        s += t(5) + "<Edge From=\"ambushed.OnAcquire\" To=\"SetAmbushed\" />\r\n";
    }
    // A mind's visits: the target's own menu fires one port per visit (Updated, log_check_N).
    int checks = 0;
    if (mind) {
        for (int i = 0; i < 3 && i < quest.count && !quest.log_check[i].empty(); ++i) {
            s += t(5) + "<Edge From=\"holder.check" + std::to_string(i + 1) + "\" To=\"SetCheck" +
                 std::to_string(i + 1) + "\" />\r\n";
            checks = i + 1;
        }
    }
    // Failed on the giver's death alone, not on the quest's OnFailed, which the give-up
    // raises too and whose line would land on top of the Abandoned one.
    s += t(5) + "<Edge From=\"giverDeath.OnDeath\" To=\"SetFailed\" />\r\n";
    s += t(4) + "</State>\r\n";
    // The objective's node is named by the objective, the way the shipped files do it
    // (<predej_vdove_krizek Name="objectiveVisual5">): the element is the objective,
    // the Progress edge is what it shows.
    s += t(4) + "<fetch Name=\"objectiveVisual\"" + rfo + ">\r\n";
    s += t(5) + "<Edge From=\"objState.State\" To=\"Progress\" />\r\n";
    // The trigger's Tracker on the objective is what draws "2/5" in the book and on the
    // HUD (the shipped nasbirej_raky objective wires its tracker the same way).
    if (gathered) s += t(5) + "<Edge From=\"gathered.Tracker\" To=\"Tracker\" />\r\n";
    s += t(4) + "</fetch>\r\n";
    // The second objective, "Return to <giver>", for the kinds with a moment in the middle
    // that sends Henry back: the journal's objective line never changed on the first alone
    // (noted 4 Oct 2026 at an ask: the beggar told, and the book still said find out what
    // he knows), since one objective has one name. It goes Active off the same port that
    // writes the first's Updated line, so the journal gains the line and the marker moves
    // to the giver, and it is done or cancelled with the quest. ProgressFailable is the
    // game's own type for it (poustevnik_a_vdova.xml), with no log of its own: the first
    // objective's Updated entry already tells the story of the moment, and self-closing
    // EnumLogs are what 274 of the shipped objectives write where there is nothing to say.
    const std::string return_from = [&]() -> std::string {
        if (checks > 0) return "holder.check" + std::to_string(checks);
        if (told || handed) return "holder.handover";
        if (seen) return "seen.OnMatch";
        if (beaten) return "beaten.OnUnconscious";
        if (prayed) return "prayed.OnAcquire";
        if (left) return "left.OnAcquire";
        if (gathered) return "gathered.OnTargetAmountAcquire";
        return "";
    }();
    if (!return_from.empty()) {
        s += t(4) + "<State Name=\"returnState\"" + rfo + " TypeT=\"ProgressFailable\">\r\n";
        s += t(5) + "<Edge From=\"" + return_from + "\" To=\"SetActive\" />\r\n";
        // Done at the hand-in -- unless the ending leads on to another scene of this run,
        // when "Return to <giver>" stands until the next errand is taken up (its accept
        // closes it), so a player who walks out mid-offer still has the marker.
        if (!ctx.leads_on("")) s += t(5) + "<Edge From=\"" + P("complete") + "\" To=\"SetDone\" />\r\n";
        for (const std::string& how : ways) {
            if (!ctx.leads_on(how)) s += t(5) + "<Edge From=\"" + P("complete_" + how) + "\" To=\"SetDone\" />\r\n";
        }
        for (const std::string& port : ctx.follower_accepts) {
            s += t(5) + "<Edge From=\"" + port + "\" To=\"SetDone\" />\r\n";
        }
        // Henry is back, and sent on: the return is done and the last leg opens.
        if (onward) s += t(5) + "<Edge From=\"" + P("onward") + "\" To=\"SetDone\" />\r\n";
        s += t(5) + "<Edge From=\"questState.OnFailed\" To=\"SetCanceled\" />\r\n";
        s += t(4) + "</State>\r\n";
        s += t(4) + "<" + std::string(kReturnObjective) + " Name=\"objectiveVisual2\"" + rfo + ">\r\n";
        s += t(5) + "<Edge From=\"returnState.State\" To=\"Progress\" />\r\n";
        s += t(4) + "</" + std::string(kReturnObjective) + ">\r\n";
    }
    // The third objective, for an ask whose word is carried on: "Tell <onward>", open from
    // the `onward` port (the giver told and paid, Henry sent on) with the marker on the one
    // sent to, done with the quest (the plain complete, which the onward menu fires), and
    // cancelled if the giver dies first. The same ProgressFailable shape as the return.
    if (onward) {
        s += t(4) + "<State Name=\"onwardState\"" + rfo + " TypeT=\"ProgressFailable\">\r\n";
        s += t(5) + "<Edge From=\"" + P("onward") + "\" To=\"SetActive\" />\r\n";
        s += t(5) + "<Edge From=\"" + P("complete") + "\" To=\"SetDone\" />\r\n";
        for (const std::string& how : ways) {
            s += t(5) + "<Edge From=\"" + P("complete_" + how) + "\" To=\"SetDone\" />\r\n";
        }
        s += t(5) + "<Edge From=\"questState.OnFailed\" To=\"SetCanceled\" />\r\n";
        s += t(4) + "</State>\r\n";
        s += t(4) + "<" + std::string(kOnwardObjective) + " Name=\"objectiveVisual3\"" + rfo + ">\r\n";
        s += t(5) + "<Edge From=\"onwardState.State\" To=\"Progress\" />\r\n";
        s += t(4) + "</" + std::string(kOnwardObjective) + ">\r\n";
        // The onward soul's menu: offered while the last leg is open, and closed the moment
        // the word is said (its own handover port; the Lua on that row's exit completes
        // the quest).
        s += t(4) + "<State Name=\"carried\" TypeT=\"bool\">\r\n";
        s += t(5) + "<Edge From=\"onwardTalk.handover\" To=\"SetTrue\" />\r\n";
        s += t(4) + "</State>\r\n";
        s += t(4) + "<" + onward_node(quest.id) + " Name=\"onwardTalk\">\r\n";
        s += t(5) + "<Edge From=\"onwardState.Active\" To=\"active\" />\r\n";
        s += t(5) + "<Edge From=\"carried.State\" To=\"handed\" />\r\n";
        s += t(4) + "</" + onward_node(quest.id) + ">\r\n";
    }
    out.before = s;
    s.clear();
    // And the holder, when the journal has a line for it: the errand goes on -- the
    // thing is on the body -- so this only updates the objective.
    if (!quest.log_target_dead.empty()) {
        s += t(4) + "<SoulDeathTrigger Name=\"targetDeath\">\r\n";
        s += t(5) + "<Asset Name=\"Souls\" Alias=\"target\" />\r\n";
        s += t(5) + "<Edge From=\"" + active_bool + "\" To=\"IsActive\" />\r\n";
        s += t(4) + "</SoulDeathTrigger>\r\n";
    }
    if (travel) {
        // Nothing to seed and nothing to hand over. What the graph watches is the
        // ground: a DistanceTrigger between the one looked at and the player, the way
        // the shipped graphs watch a soul coming near (Operator/Distance constants,
        // OnMatch when it holds), live from the moment the errand is taken up until the
        // first match, so a second visit does not write the journal twice -- and not past
        // the death of the one looked at either: the journal has said there is nobody left
        // to observe, and standing over the body must not then say what they were doing
        // (measured: the pass that kills her put Henry beside her to do it, and the watch
        // wrote log_seen over log_target_dead).
        if (seen) {
            s += t(4) + "<State Name=\"watching\" TypeT=\"bool\">\r\n";
            s += t(5) + "<Edge From=\"" + on_active + "\" To=\"SetTrue\" />\r\n";
            s += t(5) + "<Edge From=\"seen.OnMatch\" To=\"SetFalse\" />\r\n";
            if (!quest.log_target_dead.empty()) {
                s += t(5) + "<Edge From=\"targetDeath.OnDeath\" To=\"SetFalse\" />\r\n";
            }
            s += t(4) + "</State>\r\n";
            s += t(4) + "<DistanceTrigger Name=\"seen\">\r\n";
            s += t(5) + "<Asset Name=\"Origin\" Alias=\"target\" />\r\n";
            s += t(5) + "<Asset Name=\"Target\" Alias=\"player\" />\r\n";
            s += t(5) + "<Constant Name=\"Operator\" Value=\"LessEquals\" />\r\n";
            s += t(5) + "<Constant Name=\"Distance\" Value=\"" + std::to_string(kTravelSeenMetres) + "\" />\r\n";
            s += t(5) + "<Edge From=\"watching.State\" To=\"IsActive\" />\r\n";
            s += t(4) + "</DistanceTrigger>\r\n";
        }
        if (quest.has_brawl()) s += emit_brawl(quest, seen);
    } else if (beat) {
        // Nothing to seed. What the graph watches is the man: a SoulUnconsciousTrigger on
        // him (In IsActive, Souls; Out OnUnconscious), live from the moment the errand is
        // taken up until he goes down or dies, so a second knock-down does not write the
        // journal twice. His menu closes at the same moment (handed, below).
        if (beaten) {
            s += t(4) + "<State Name=\"watching\" TypeT=\"bool\">\r\n";
            s += t(5) + "<Edge From=\"" + on_active + "\" To=\"SetTrue\" />\r\n";
            s += t(5) + "<Edge From=\"beaten.OnUnconscious\" To=\"SetFalse\" />\r\n";
            if (!quest.log_target_dead.empty()) {
                s += t(5) + "<Edge From=\"targetDeath.OnDeath\" To=\"SetFalse\" />\r\n";
            }
            s += t(4) + "</State>\r\n";
            s += t(4) + "<SoulUnconsciousTrigger Name=\"beaten\">\r\n";
            s += t(5) + "<Asset Name=\"Souls\" Alias=\"target\" />\r\n";
            s += t(5) + "<Edge From=\"watching.State\" To=\"IsActive\" />\r\n";
            s += t(4) + "</SoulUnconsciousTrigger>\r\n";
        }
    } else if (pray || spot) {
        // Nothing to seed and nobody at the other end. The graph cannot see Henry kneel
        // (the shrines' interactions are bound per level in waitinglinks.xml, which a mod
        // cannot author), so the Lua watches his position and, when he has stood at the spot
        // long enough, makes a token of this quest's own item class in his pocket for a
        // moment; an ItemDescriptorTrigger on the player's inventory (the shape of the shipped
        // utils/item/itemclasstrigger_playerinventory module) fires OnAcquire on it. Live until
        // the first, so a second kneeling does not write the journal twice. A leave's and a
        // return's thing put down at the place is the same token under another name.
        if (prayed || left) {
            const char* trigger = prayed ? "prayed" : "left";
            s += t(4) + "<State Name=\"watching\" TypeT=\"bool\">\r\n";
            s += t(5) + "<Edge From=\"" + on_active + "\" To=\"SetTrue\" />\r\n";
            s += t(5) + "<Edge From=\"" + std::string(trigger) + ".OnAcquire\" To=\"SetFalse\" />\r\n";
            s += t(4) + "</State>\r\n";
            s += t(4) + "<ObjectProperties Name=\"playerSoul\" DeclaringType=\"wh::rpgmodule::I_Soul\">\r\n";
            s += t(5) + "<Asset Name=\"I_Soul\" Alias=\"player\" />\r\n";
            s += t(4) + "</ObjectProperties>\r\n";
            s += t(4) + "<Function Name=\"tokenClass\" MethodName=\"wh::entitymodule::CreateItemClassDescriptor\" "
                        "DeclaringType=\"wh::entitymodule\">\r\n";
            s += t(5) + "<Constant Name=\"ItemClass\" Value=\"" + xml_escape(token_class(quest)) + "\" />\r\n";
            s += t(5) + "<Constant Name=\"Amount\" Value=\"1\" />\r\n";
            s += t(4) + "</Function>\r\n";
            s += t(4) + "<ItemDescriptorTrigger Name=\"" + std::string(trigger) + "\">\r\n";
            s += t(5) + "<Edge From=\"tokenClass.ItemDescriptor\" To=\"ItemDescriptor\" />\r\n";
            s += t(5) + "<Edge From=\"playerSoul.Inventory\" To=\"Inventory\" />\r\n";
            s += t(5) + "<Edge From=\"watching.State\" To=\"IsActive\" />\r\n";
            s += t(4) + "</ItemDescriptorTrigger>\r\n";
        }
    } else if (ambush) {
        // Nothing to seed: the directions are made in Henry's pocket by the Lua when the
        // menu's first stage is played. What the graph watches is the token (the pray's
        // shape): the Lua makes one of this quest's class in his pocket when he is at the
        // place in the hour with the directions on him, the ItemDescriptorTrigger fires
        // OnAcquire, and the brawl module below sets the three on him. Live until the first,
        // so a second night at the place is not a second fight.
        if (ambushed) {
            s += t(4) + "<State Name=\"watching\" TypeT=\"bool\">\r\n";
            s += t(5) + "<Edge From=\"" + on_active + "\" To=\"SetTrue\" />\r\n";
            s += t(5) + "<Edge From=\"ambushed.OnAcquire\" To=\"SetFalse\" />\r\n";
            s += t(4) + "</State>\r\n";
            s += t(4) + "<ObjectProperties Name=\"playerSoul\" DeclaringType=\"wh::rpgmodule::I_Soul\">\r\n";
            s += t(5) + "<Asset Name=\"I_Soul\" Alias=\"player\" />\r\n";
            s += t(4) + "</ObjectProperties>\r\n";
            s += t(4) + "<Function Name=\"tokenClass\" MethodName=\"wh::entitymodule::CreateItemClassDescriptor\" "
                        "DeclaringType=\"wh::entitymodule\">\r\n";
            s += t(5) + "<Constant Name=\"ItemClass\" Value=\"" + xml_escape(token_class(quest)) + "\" />\r\n";
            s += t(5) + "<Constant Name=\"Amount\" Value=\"1\" />\r\n";
            s += t(4) + "</Function>\r\n";
            s += t(4) + "<ItemDescriptorTrigger Name=\"ambushed\">\r\n";
            s += t(5) + "<Edge From=\"tokenClass.ItemDescriptor\" To=\"ItemDescriptor\" />\r\n";
            s += t(5) + "<Edge From=\"playerSoul.Inventory\" To=\"Inventory\" />\r\n";
            s += t(5) + "<Edge From=\"watching.State\" To=\"IsActive\" />\r\n";
            s += t(4) + "</ItemDescriptorTrigger>\r\n";
            if (quest.has_brawl()) s += emit_brawl(quest, ambushed);
        }
    } else if (mind) {
        // Nothing to seed and nothing to watch here: the visits are the target's own menu,
        // whose ports are wired to the objective above and whose rows the Lua gates by game
        // time through dialogue globals.
    } else if (ask) {
        // Nothing to seed and nothing to watch but the menu: the truth is given up through
        // it, and its handover port is wired to the objective above. No unconscious trigger
        // -- knocking the one asked down is not a way of asking -- so the menu stays open
        // until the truth is told or the one asked is dead (the Lua watches IsDead).
    } else if (find) {
        // Nothing to seed: the thing is the shipped tables' and is wherever Henry finds
        // it, and the Lua takes the count of it at the hand-in. What the graph watches is
        // his pocket: an ItemDescriptorTrigger on the player's inventory for `count` of the
        // class (the shipped utils/item/itemclasstrigger_playerinventory, unwrapped), live
        // from the moment the errand is taken up until the count is reached (`gathering`,
        // the pray's `watching` shape; the shipped sber_raku gates its own on a hidden
        // Progress state the same way). Its Tracker is the "2/5" on the objective above;
        // OnTargetAmountAcquire is the mid-moment, which completes that objective. Off from
        // then on, so the hand-in's TakeFresh does not count the things down on the HUD
        // ("1/5" seen at the hand-in, notes.md 7 Oct 2026) and a stale or dropped flower
        // does not reopen a ticked objective; the app's hand-in still wants the fresh
        // count (CountFresh), so a stale lot is turned back in words at the door.
        if (gathered) {
            s += t(4) + "<State Name=\"gathering\" TypeT=\"bool\">\r\n";
            s += t(5) + "<Edge From=\"" + on_active + "\" To=\"SetTrue\" />\r\n";
            s += t(5) + "<Edge From=\"gathered.OnTargetAmountAcquire\" To=\"SetFalse\" />\r\n";
            s += t(4) + "</State>\r\n";
            s += t(4) + "<ObjectProperties Name=\"playerSoul\" DeclaringType=\"wh::rpgmodule::I_Soul\">\r\n";
            s += t(5) + "<Asset Name=\"I_Soul\" Alias=\"player\" />\r\n";
            s += t(4) + "</ObjectProperties>\r\n";
            s += t(4) + "<Function Name=\"gatherClass\" MethodName=\"wh::entitymodule::CreateItemClassDescriptor\" "
                        "DeclaringType=\"wh::entitymodule\">\r\n";
            s += t(5) + "<Constant Name=\"ItemClass\" Value=\"" + xml_escape(quest.item_class) + "\" />\r\n";
            s += t(5) + "<Constant Name=\"Amount\" Value=\"" + std::to_string(quest.count) + "\" />\r\n";
            // Fresh ones only, where the giver wants them so: the descriptor's own
            // MinimumHealth (the Cumans-at-Trosky playeritemtrigger and the wedding's
            // descriptor_jidla_pro_chude wire it), on the engine's 0..1 item health. The
            // Lua's hand-in counts and takes by the same figure (CountFresh / TakeFresh).
            if (quest.min_health > 0) {
                s += t(5) + "<Constant Name=\"MinimumHealth\" Value=\"" + health_text(quest.min_health) + "\" />\r\n";
            }
            s += t(4) + "</Function>\r\n";
            s += t(4) + "<ItemDescriptorTrigger Name=\"gathered\">\r\n";
            s += t(5) + "<Edge From=\"gatherClass.ItemDescriptor\" To=\"ItemDescriptor\" />\r\n";
            s += t(5) + "<Edge From=\"playerSoul.Inventory\" To=\"Inventory\" />\r\n";
            s += t(5) + "<Edge From=\"gathering.State\" To=\"IsActive\" />\r\n";
            s += t(4) + "</ItemDescriptorTrigger>\r\n";
        }
    } else if (quest.is_deliver()) {
        // A letter is not in anybody's pocket until the writer hands it over, which the Lua
        // does at accept (GiveLetter); so no default item. What the graph watches instead
        // is the page: UseBookTrigger fires OnOpened when the player reads a Document of
        // this class, the way dopis_od_rynese.xml watches Rynesh's letter, and the
        // objective's Opened state above is what it drives. Only while Active, so a letter
        // read after it is delivered (the receiver's, pickpocketed back) says nothing.
        if (opened) {
            s += t(4) + "<UseBookTrigger Name=\"opened\">\r\n";
            s += t(5) + "<Constant Name=\"Book\" Value=\"" + xml_escape(quest.item_class) + "\" />\r\n";
            s += t(5) + "<Edge From=\"" + active_bool + "\" To=\"IsActive\" />\r\n";
            s += t(4) + "</UseBookTrigger>\r\n";
        }
    } else if (quest.has_chest()) {
        // The thing lies in a chest the Lua spawns at chest_pos when Henry first comes near
        // (WatchChest; behaviors.md §6.3), locked and keyed to the quest's own key class, so
        // nothing is seeded on the holder: his pocket is empty and the chest is the errand.
        // Its leaving the chest is the taken trigger below.
    } else if (quest.takes_letter_of()) {
        // The thing is the Document an earlier quest of ours put in the holder's hands (the
        // dictated letter he was given and kept): nothing is seeded, and the Lua names the
        // class from that quest's store at the handover (QuestFlags, letterOf).
    } else {
        // The thing to be fetched, in the holder's pockets from the start rather than
        // from the moment of asking, since the bio says they have had it since Candlemas.
        // Default items are how the shipped quests seed an inventory (355 uses); Purpose
        // Inventory rather than an equip slot because a work axe is carried, not worn.
        // The IsActive bool is a constant-true State because the node wants a bool port
        // and there is no literal.
        s += t(4) + "<State Name=\"always\" TypeT=\"bool\">\r\n";
        s += t(5) + "<Constant Name=\"DefaultValue\" Value=\"true\" />\r\n";
        s += t(4) + "</State>\r\n";
        s += t(4) + "<AddSoulDefaultItem Name=\"holderItem\">\r\n";
        s += t(5) + "<Constant Name=\"ItemClass\" Value=\"" + xml_escape(quest.item_class) + "\" />\r\n";
        s += t(5) + "<Constant Name=\"Purpose\" Value=\"Inventory\" />\r\n";
        s += t(5) + "<Constant Name=\"Mode\" Value=\"Create\" />\r\n";
        s += t(5) + "<Asset Name=\"Souls\" Alias=\"target\" />\r\n";
        s += t(5) + "<Edge From=\"always.State\" To=\"IsActive\" />\r\n";
        s += t(4) + "</AddSoulDefaultItem>\r\n";
    }
    {
        // What the graph sees of the helper is the token the Lua makes on the app's `named`
        // directive -- the giver has named the one who can let Henry in -- an
        // ItemDescriptorTrigger of the pray's shape, live from accept until it fires, so a
        // second naming does not reopen the objective. A chest's taken token is another of
        // the same shape, on its own class: the thing has left the chest for Henry's pocket.
        if (named || quest.has_chest() || written || caught) {
            s += t(4) + "<ObjectProperties Name=\"playerSoul\" DeclaringType=\"wh::rpgmodule::I_Soul\">\r\n";
            s += t(5) + "<Asset Name=\"I_Soul\" Alias=\"player\" />\r\n";
            s += t(4) + "</ObjectProperties>\r\n";
        }
        if (caught) {
            // The exposure's token (WatchExpose: Henry at the holder's bedside with the witness
            // in it, in the night window), live from the accept until it fires or the thing is
            // handed over some other way, so a second night does not write the line twice.
            s += t(4) + "<State Name=\"catching\" TypeT=\"bool\">\r\n";
            s += t(5) + "<Edge From=\"" + on_active + "\" To=\"SetTrue\" />\r\n";
            s += t(5) + "<Edge From=\"caught.OnAcquire\" To=\"SetFalse\" />\r\n";
            s += t(5) + "<Edge From=\"holder.handover\" To=\"SetFalse\" />\r\n";
            s += t(4) + "</State>\r\n";
            s += t(4) + "<Function Name=\"caughtClass\" MethodName=\"wh::entitymodule::CreateItemClassDescriptor\" "
                        "DeclaringType=\"wh::entitymodule\">\r\n";
            s += t(5) + "<Constant Name=\"ItemClass\" Value=\"" + xml_escape(token_class(quest)) + "\" />\r\n";
            s += t(5) + "<Constant Name=\"Amount\" Value=\"1\" />\r\n";
            s += t(4) + "</Function>\r\n";
            s += t(4) + "<ItemDescriptorTrigger Name=\"caught\">\r\n";
            s += t(5) + "<Edge From=\"caughtClass.ItemDescriptor\" To=\"ItemDescriptor\" />\r\n";
            s += t(5) + "<Edge From=\"playerSoul.Inventory\" To=\"Inventory\" />\r\n";
            s += t(5) + "<Edge From=\"catching.State\" To=\"IsActive\" />\r\n";
            s += t(4) + "</ItemDescriptorTrigger>\r\n";
        }
        if (quest.has_follow()) {
            // The holder turning on Henry in the street (npcdb.hpp has_follow; emit.hpp
            // kFollow*). The sighting is the Lua's (WatchFollow: the hour, the street, the
            // holder's facing, the distance, the line of sight), and its verdict comes as
            // the followed token (follow_class), the way the catching comes: `followed`
            // fires, the night is `alerted` -- the scheduler tag on, his sleep links swapped
            // to his own bed (alert_tag; the override's split of them in
            // build_scheduler.py), the journal's line -- until the rest Timer runs out
            // in the morning (WorldTime, the calendar's hours, past his waking), when the
            // tag drops and `watching` is up again for another night. The catching or the
            // handover ends both for good. The Timer's TimeType matters: the shipped graphs
            // pair every hour and day duration with WorldTime and GameTime only with seconds
            // and minutes (GameTime is play time, which sleep, wait and the clock's winding
            // do not advance), and this one was GameTime until 26 Oct 2026, so the tag stayed
            // on for the rest of the session and he walked past her door the next night too
            // (notes.md). The graph's own perception was tried first (a PerceptionTrigger on the
            // holder handing each cone to the Lua through ExecuteConsoleString, the shape of
            // the shipped utils/debug/DelayedConsoleExecution): the trigger fires, but the
            // console node does nothing in the release build (probes of 24 Oct 2026).
            s += t(4) + "<State Name=\"watching\" TypeT=\"bool\">\r\n";
            s += t(5) + "<Edge From=\"" + on_active + "\" To=\"SetTrue\" />\r\n";
            s += t(5) + "<Edge From=\"followed.OnAcquire\" To=\"SetFalse\" />\r\n";
            s += t(5) + "<Edge From=\"followRest.OnFinished\" To=\"SetTrue\" />\r\n";
            s += t(5) + "<Edge From=\"caught.OnAcquire\" To=\"SetFalse\" />\r\n";
            s += t(5) + "<Edge From=\"holder.handover\" To=\"SetFalse\" />\r\n";
            s += t(4) + "</State>\r\n";
            s += t(4) + "<Function Name=\"followClass\" MethodName=\"wh::entitymodule::CreateItemClassDescriptor\" "
                        "DeclaringType=\"wh::entitymodule\">\r\n";
            s += t(5) + "<Constant Name=\"ItemClass\" Value=\"" + xml_escape(follow_class(quest)) + "\" />\r\n";
            s += t(5) + "<Constant Name=\"Amount\" Value=\"1\" />\r\n";
            s += t(4) + "</Function>\r\n";
            s += t(4) + "<ItemDescriptorTrigger Name=\"followed\">\r\n";
            s += t(5) + "<Edge From=\"followClass.ItemDescriptor\" To=\"ItemDescriptor\" />\r\n";
            s += t(5) + "<Edge From=\"playerSoul.Inventory\" To=\"Inventory\" />\r\n";
            s += t(5) + "<Edge From=\"watching.State\" To=\"IsActive\" />\r\n";
            s += t(4) + "</ItemDescriptorTrigger>\r\n";
            s += t(4) + "<State Name=\"alerted\" TypeT=\"bool\">\r\n";
            s += t(5) + "<Edge From=\"followed.OnAcquire\" To=\"SetTrue\" />\r\n";
            s += t(5) + "<Edge From=\"followRest.OnFinished\" To=\"SetFalse\" />\r\n";
            s += t(5) + "<Edge From=\"caught.OnAcquire\" To=\"SetFalse\" />\r\n";
            s += t(5) + "<Edge From=\"holder.handover\" To=\"SetFalse\" />\r\n";
            s += t(4) + "</State>\r\n";
            s += t(4) + "<Timer Name=\"followRest\">\r\n";
            s += t(5) + "<Constant Name=\"Duration\" Value=\"" + std::to_string(kFollowRestHours) + "h\" />\r\n";
            s += t(5) + "<Constant Name=\"TimeType\" Value=\"WorldTime\" />\r\n";
            s += t(5) + "<Edge From=\"followed.OnAcquire\" To=\"SetRunning\" />\r\n";
            s += t(4) + "</Timer>\r\n";
            s += t(4) + "<SchedulerLinkActivation Name=\"alertLink\">\r\n";
            s += t(5) + "<Constant Name=\"LinkTag\" Value=\"" + xml_escape(alert_tag(modid, quest.id)) + "\" />\r\n";
            s += t(5) + "<Edge From=\"alerted.State\" To=\"IsActive\" />\r\n";
            s += t(4) + "</SchedulerLinkActivation>\r\n";
            // The street night's marker (emit.hpp mark_shown_class; bugs.txt 28 Oct 2026):
            // the Lua's WatchMarker makes the shown token as his homecoming hour comes and
            // the hidden one as his night's window closes or the night is `followed`, and
            // each moves the objective to its marker-only state. Live from the accept until
            // the night is caught or given up (the catching, the handover, the denouncing,
            // his death), after which the objective's own states carry the marker.
            s += t(4) + "<State Name=\"marking\" TypeT=\"bool\">\r\n";
            s += t(5) + "<Edge From=\"" + on_active + "\" To=\"SetTrue\" />\r\n";
            s += t(5) + "<Edge From=\"caught.OnAcquire\" To=\"SetFalse\" />\r\n";
            s += t(5) + "<Edge From=\"holder.handover\" To=\"SetFalse\" />\r\n";
            if (quest.has_bedside()) s += t(5) + "<Edge From=\"holder.denounced\" To=\"SetFalse\" />\r\n";
            if (!quest.log_target_dead.empty()) s += t(5) + "<Edge From=\"targetDeath.OnDeath\" To=\"SetFalse\" />\r\n";
            s += t(4) + "</State>\r\n";
            s += t(4) + "<Function Name=\"markShownClass\" MethodName=\"wh::entitymodule::CreateItemClassDescriptor\" "
                        "DeclaringType=\"wh::entitymodule\">\r\n";
            s += t(5) + "<Constant Name=\"ItemClass\" Value=\"" + xml_escape(mark_shown_class(quest)) + "\" />\r\n";
            s += t(5) + "<Constant Name=\"Amount\" Value=\"1\" />\r\n";
            s += t(4) + "</Function>\r\n";
            s += t(4) + "<ItemDescriptorTrigger Name=\"markShown\">\r\n";
            s += t(5) + "<Edge From=\"markShownClass.ItemDescriptor\" To=\"ItemDescriptor\" />\r\n";
            s += t(5) + "<Edge From=\"playerSoul.Inventory\" To=\"Inventory\" />\r\n";
            s += t(5) + "<Edge From=\"marking.State\" To=\"IsActive\" />\r\n";
            s += t(4) + "</ItemDescriptorTrigger>\r\n";
            s += t(4) + "<Function Name=\"markHiddenClass\" MethodName=\"wh::entitymodule::CreateItemClassDescriptor\" "
                        "DeclaringType=\"wh::entitymodule\">\r\n";
            s += t(5) + "<Constant Name=\"ItemClass\" Value=\"" + xml_escape(mark_hidden_class(quest)) + "\" />\r\n";
            s += t(5) + "<Constant Name=\"Amount\" Value=\"1\" />\r\n";
            s += t(4) + "</Function>\r\n";
            s += t(4) + "<ItemDescriptorTrigger Name=\"markHidden\">\r\n";
            s += t(5) + "<Edge From=\"markHiddenClass.ItemDescriptor\" To=\"ItemDescriptor\" />\r\n";
            s += t(5) + "<Edge From=\"playerSoul.Inventory\" To=\"Inventory\" />\r\n";
            s += t(5) + "<Edge From=\"marking.State\" To=\"IsActive\" />\r\n";
            s += t(4) + "</ItemDescriptorTrigger>\r\n";
        }
        if (written) {
            // Live from the accept until the token arrives, so a second writing (none is
            // possible, but a save from mid-dictation is) does not reopen the line.
            s += t(4) + "<State Name=\"writing\" TypeT=\"bool\">\r\n";
            s += t(5) + "<Edge From=\"" + on_active + "\" To=\"SetTrue\" />\r\n";
            s += t(5) + "<Edge From=\"written.OnAcquire\" To=\"SetFalse\" />\r\n";
            s += t(4) + "</State>\r\n";
            s += t(4) + "<Function Name=\"writtenClass\" MethodName=\"wh::entitymodule::CreateItemClassDescriptor\" "
                        "DeclaringType=\"wh::entitymodule\">\r\n";
            s += t(5) + "<Constant Name=\"ItemClass\" Value=\"" + xml_escape(token_class(quest)) + "\" />\r\n";
            s += t(5) + "<Constant Name=\"Amount\" Value=\"1\" />\r\n";
            s += t(4) + "</Function>\r\n";
            s += t(4) + "<ItemDescriptorTrigger Name=\"written\">\r\n";
            s += t(5) + "<Edge From=\"writtenClass.ItemDescriptor\" To=\"ItemDescriptor\" />\r\n";
            s += t(5) + "<Edge From=\"playerSoul.Inventory\" To=\"Inventory\" />\r\n";
            s += t(5) + "<Edge From=\"writing.State\" To=\"IsActive\" />\r\n";
            s += t(4) + "</ItemDescriptorTrigger>\r\n";
        }
        if (quest.has_chest()) {
            s += t(4) + "<State Name=\"taking\" TypeT=\"bool\">\r\n";
            s += t(5) + "<Edge From=\"" + on_active + "\" To=\"SetTrue\" />\r\n";
            s += t(5) + "<Edge From=\"taken.OnAcquire\" To=\"SetFalse\" />\r\n";
            s += t(5) + "<Edge From=\"holder.handover\" To=\"SetFalse\" />\r\n";
            s += t(4) + "</State>\r\n";
            s += t(4) + "<Function Name=\"takenClass\" MethodName=\"wh::entitymodule::CreateItemClassDescriptor\" "
                        "DeclaringType=\"wh::entitymodule\">\r\n";
            s += t(5) + "<Constant Name=\"ItemClass\" Value=\"" + xml_escape(taken_class(quest)) + "\" />\r\n";
            s += t(5) + "<Constant Name=\"Amount\" Value=\"1\" />\r\n";
            s += t(4) + "</Function>\r\n";
            s += t(4) + "<ItemDescriptorTrigger Name=\"taken\">\r\n";
            s += t(5) + "<Edge From=\"takenClass.ItemDescriptor\" To=\"ItemDescriptor\" />\r\n";
            s += t(5) + "<Edge From=\"playerSoul.Inventory\" To=\"Inventory\" />\r\n";
            s += t(5) + "<Edge From=\"taking.State\" To=\"IsActive\" />\r\n";
            s += t(4) + "</ItemDescriptorTrigger>\r\n";
        }
        // The giver's hail in his own mouth (notes.md 14 Oct 2026): the watch says when
        // (WatchHail) by a token of the hail's class, the pray's channel, and the trigger on
        // it -- live while the quest is unasked, the hail's whole season -- requests the
        // one-line monologue q_<id>_hail.xml on him, the game's bark with our words: the
        // subtitle over his head, the shape ambient-design/plan.md section 1.1 proved.
        // OverrideContextSuppress, since a routine's gossip context suppresses barks
        // otherwise; CanBeDelayed, so a soul mid-sentence finishes first.
        if (!quest.hail.empty()) {
            // The dialogue is a node of the graph, as the shipped shouts have it (the
            // DialogWrapper of mlynaruvUcen's pacholek_krici_u_mlyna lists the Dialog's
            // definition and stands an instance of it beside the RequestMonologue); listed
            // and not stood, its alias is nobody's and the request plays nothing.
            s += t(4) + "<" + hail_node(quest.id) + " Name=\"hailTalk\" />\r\n";
            s += t(4) + "<ObjectProperties Name=\"playerSoulHail\" DeclaringType=\"wh::rpgmodule::I_Soul\">\r\n";
            s += t(5) + "<Asset Name=\"I_Soul\" Alias=\"player\" />\r\n";
            s += t(4) + "</ObjectProperties>\r\n";
            s += t(4) + "<Function Name=\"hailClass\" MethodName=\"wh::entitymodule::CreateItemClassDescriptor\" "
                        "DeclaringType=\"wh::entitymodule\">\r\n";
            s += t(5) + "<Constant Name=\"ItemClass\" Value=\"" + xml_escape(hail_class(quest)) + "\" />\r\n";
            s += t(5) + "<Constant Name=\"Amount\" Value=\"1\" />\r\n";
            s += t(4) + "</Function>\r\n";
            s += t(4) + "<ItemDescriptorTrigger Name=\"hailed\">\r\n";
            s += t(5) + "<Edge From=\"hailClass.ItemDescriptor\" To=\"ItemDescriptor\" />\r\n";
            s += t(5) + "<Edge From=\"playerSoulHail.Inventory\" To=\"Inventory\" />\r\n";
            s += t(5) + "<Edge From=\"questState.None\" To=\"IsActive\" />\r\n";
            s += t(4) + "</ItemDescriptorTrigger>\r\n";
            s += t(4) + "<Function Name=\"hailBark\" MethodName=\"wh::xgenaimodule::RequestMonologue\" "
                        "DeclaringType=\"wh::xgenaimodule\">\r\n";
            s += t(5) + "<Asset Name=\"Souls\" Alias=\"giver\" />\r\n";
            s += t(5) + "<Constant Name=\"DecisionAlias\" Value=\"" + xml_escape(hail_alias(modid, quest.id)) + "\" />\r\n";
            s += t(5) + "<Constant Name=\"CanBeDelayed\" Value=\"true\" />\r\n";
            s += t(5) + "<Constant Name=\"ForceSubtitles\" Value=\"true\" />\r\n";
            s += t(5) + "<Constant Name=\"OverrideContextSuppress\" Value=\"true\" />\r\n";
            s += t(5) + "<Edge From=\"hailed.OnAcquire\" To=\"Exec\" />\r\n";
            s += t(4) + "</Function>\r\n";
        }
        if (named) {
            s += t(4) + "<State Name=\"naming\" TypeT=\"bool\">\r\n";
            s += t(5) + "<Edge From=\"" + on_active + "\" To=\"SetTrue\" />\r\n";
            s += t(5) + "<Edge From=\"named.OnAcquire\" To=\"SetFalse\" />\r\n";
            s += t(4) + "</State>\r\n";
            s += t(4) + "<Function Name=\"tokenClass\" MethodName=\"wh::entitymodule::CreateItemClassDescriptor\" "
                        "DeclaringType=\"wh::entitymodule\">\r\n";
            s += t(5) + "<Constant Name=\"ItemClass\" Value=\"" + xml_escape(token_class(quest)) + "\" />\r\n";
            s += t(5) + "<Constant Name=\"Amount\" Value=\"1\" />\r\n";
            s += t(4) + "</Function>\r\n";
            s += t(4) + "<ItemDescriptorTrigger Name=\"named\">\r\n";
            s += t(5) + "<Edge From=\"tokenClass.ItemDescriptor\" To=\"ItemDescriptor\" />\r\n";
            s += t(5) + "<Edge From=\"playerSoul.Inventory\" To=\"Inventory\" />\r\n";
            s += t(5) + "<Edge From=\"naming.State\" To=\"IsActive\" />\r\n";
            s += t(4) + "</ItemDescriptorTrigger>\r\n";
        }
    }
    // Paid on the transition to Done, once. The money tier is the row the generator
    // priced the favour at in money_change.xml -- when it priced it at a tier at all:
    // the smallest row is fifty groschen, a townsman's sum, and a varlet's five or ten
    // has no row, so the tier is empty and the Lua pays the groschen at the hand-in
    // (QuestCommand "complete") instead. The experience and reputation are the
    // smallest steps the shipped micro quests hand out, since this is an afternoon's
    // errand and not a chapter.
    s += t(4) + "<AddReward Name=\"reward\">\r\n";
    for (const std::string& port : on_done) s += t(5) + "<Edge From=\"" + port + "\" To=\"AddReward\" />\r\n";
    s += t(5) + "<Rewards>\r\n";
    if (!quest.reward_tier.empty()) {
        s += t(6) + "<MoneyReward Amount=\"" + xml_escape(quest.reward_tier) + "\" />\r\n";
    }
    // A "training" reward is the one spoken reward the game can make good on: a
    // SkillReward in the skill the words name, at the step the shipped sides pay a
    // lesson at (base-game-quests.md: SkillReward 1-micro to 4-normal).
    if (quest.reward_kind == "training") {
        s += t(6) + "<SkillReward Type=\"" + skill_taught(quest.reward_text) +
             "\" ExperienceChange=\"3-small\" />\r\n";
    }
    s += t(6) + "<StatReward Type=\"storyProgress\" ExperienceChange=\"2-minor\" />\r\n";
    s += t(6) + "<SoulReputationReward ReputationChange=\"quest_reward_2_minor\" "
                "Target=\"giver\" />\r\n";
    s += t(5) + "</Rewards>\r\n";
    s += t(4) + "</AddReward>\r\n";
    s += t(4) + "<SaveGame Name=\"save\">\r\n";
    s += t(5) + "<Constant Name=\"Objective\" Value=\"fetch\" />\r\n";
    for (const std::string& port : on_done) s += t(5) + "<Edge From=\"" + port + "\" To=\"EnqueueSave\" />\r\n";
    s += t(4) + "</SaveGame>\r\n";
    // What the interrogation's dodges cost (notes.md 9 Oct 2026, In the Stocks): the
    // question talked or frightened round, the receiver thinks the less of Henry and so does
    // the town (evaded); the check lost, the receiver has him for a thief's friend and says
    // the bailiff's men will hear of it -- the game has no node that makes Henry wanted on a
    // quest's say-so (nothing in the shipped Lua or the quest XML raises a crime on the
    // player), so the nearest thing is the soldiers' regard, which is what the guard's
    // manner towards him is made of, taken down a step with the townsfolk's (caught). The
    // steps are the shipped quest_decrease rows, aimed at the settlement's factions as the
    // shipped Kuttenberg quests aim them; a soul whose town has no such factions in the data
    // loses the receiver's regard alone. Paid once, on the complete port for that how. A tale
    // the receiver saw through (fibbed) costs his regard alone: he said so to Henry's face
    // and no further.
    if (quest.has_interrogation()) {
        const std::string town = quest.giver_soul.compare(0, 5, "kkut_") == 0 ? "kutnohorsko_settlements_kutnaHora" : "";
        for (const std::string& how : ways) {
            if (how != "evaded" && how != "caught" && how != "fibbed") continue;
            const bool caught = how == "caught";
            s += t(4) + "<AddReward Name=\"cost_" + how + "\">\r\n";
            s += t(5) + "<Edge From=\"" + P("complete_" + how) + "\" To=\"AddReward\" />\r\n";
            s += t(5) + "<Rewards>\r\n";
            s += t(6) + "<SoulReputationReward ReputationChange=\"" + (caught ? "quest_decrease_4_normal" : "quest_decrease_2_minor") +
                 "\" Target=\"target\" />\r\n";
            if (!town.empty() && how != "fibbed") {
                s += t(6) + "<FactionReputationReward ReputationChange=\"" + (caught ? "quest_decrease_3_small" : "quest_decrease_2_minor") +
                     "\" Target=\"" + town + "_commonFolk\" />\r\n";
                if (caught) {
                    s += t(6) + "<FactionReputationReward ReputationChange=\"quest_decrease_4_normal\" Target=\"" + town +
                         "_soldiers\" />\r\n";
                }
            }
            s += t(5) + "</Rewards>\r\n";
            s += t(4) + "</AddReward>\r\n";
        }
    }
    // The holder's side of the errand is the game's own menu, not a conversation: a
    // dialogue that is offered while the quest is Active and the thing has not yet
    // changed hands, and stops being offered the moment it has. The handover port is
    // fired by whichever of its branches wins the thing, and the Lua on that branch's
    // exit moves it; the state here is what closes the menu afterwards.
    if (holder) {
        s += t(4) + "<State Name=\"handed\" TypeT=\"bool\">\r\n";
        s += t(5) + "<Edge From=\"holder.handover\" To=\"SetTrue\" />\r\n";
        // The thing out of the chest closes the holder's menu as his giving it would.
        if (quest.has_chest()) s += t(5) + "<Edge From=\"taken.OnAcquire\" To=\"SetTrue\" />\r\n";
        // A beat's menu also closes when the man goes down: there is nothing left to talk
        // him out of.
        if (beaten) s += t(5) + "<Edge From=\"beaten.OnUnconscious\" To=\"SetTrue\" />\r\n";
        s += t(4) + "</State>\r\n";
        // A receiver with a word for a broken seal is told whether the page was opened:
        // the trigger's OnOpened held in a bool, read by his dialogue's entry conditions
        // (notes.md, 9 Oct 2026 -- Holy Jest's monk, remarking on it and no more).
        const bool seal_word = opened && !quest.receiver_hand_opened.empty();
        if (seal_word) {
            s += t(4) + "<State Name=\"wasOpened\" TypeT=\"bool\">\r\n";
            s += t(5) + "<Edge From=\"opened.OnOpened\" To=\"SetTrue\" />\r\n";
            s += t(4) + "</State>\r\n";
        }
        s += t(4) + "<" + holder_node(quest.id) + " Name=\"holder\">\r\n";
        s += t(5) + "<Edge From=\"" + active_bool + "\" To=\"active\" />\r\n";
        s += t(5) + "<Edge From=\"handed.State\" To=\"handed\" />\r\n";
        if (seal_word) s += t(5) + "<Edge From=\"wasOpened.State\" To=\"opened\" />\r\n";
        s += t(4) + "</" + holder_node(quest.id) + ">\r\n";
    }
    // The one who can let Henry in (In the Stocks' Elsbeth): her optional objective opens
    // off the naming token and closes with the quest (the return's ProgressFailable shape;
    // IsOptional on the objective below), and her menu is offered while it is open. Two
    // bools hold what she has done -- the door bought (`letin`), the key got (`keyed`) --
    // so the rows are gone once taken and a save keeps them gone. The door is the giver's
    // welcome over the holder's household: everyone who sleeps under his roof, and her,
    // carry the game's "the player is expected" contexts from then on (notes.md 13 Oct
    // 2026: Henry let into the house by day, no trespass), for good -- she said she never
    // saw him, and nobody takes that back -- unless the errand fails.
    if (named) {
        // Her objective is done the first time Henry has spoken to her (the menu's `spoken`,
        // off every row that ends the talk), or with the quest; the menu itself stays
        // offered from the naming to the end on its own bool, so he can come back for the
        // key after the door (notes.md 14 Oct 2026: the objective had closed only with the
        // quest, and the menu rode the objective's state).
        s += t(4) + "<State Name=\"helperState\"" + rfo + " TypeT=\"ProgressFailable\">\r\n";
        s += t(5) + "<Edge From=\"named.OnAcquire\" To=\"SetActive\" />\r\n";
        s += t(5) + "<Edge From=\"helperTalk.spoken\" To=\"SetDone\" />\r\n";
        s += t(5) + "<Edge From=\"" + P("complete") + "\" To=\"SetDone\" />\r\n";
        for (const std::string& how : ways) {
            s += t(5) + "<Edge From=\"" + P("complete_" + how) + "\" To=\"SetDone\" />\r\n";
        }
        s += t(5) + "<Edge From=\"questState.OnFailed\" To=\"SetCanceled\" />\r\n";
        s += t(4) + "</State>\r\n";
        s += t(4) + "<" + std::string(kHelperObjective) + " Name=\"objectiveVisual4\"" + rfo + ">\r\n";
        s += t(5) + "<Edge From=\"helperState.State\" To=\"Progress\" />\r\n";
        s += t(4) + "</" + std::string(kHelperObjective) + ">\r\n";
        s += t(4) + "<State Name=\"helperOpen\" TypeT=\"bool\">\r\n";
        s += t(5) + "<Edge From=\"named.OnAcquire\" To=\"SetTrue\" />\r\n";
        s += t(5) + "<Edge From=\"" + P("complete") + "\" To=\"SetFalse\" />\r\n";
        for (const std::string& how : ways) {
            s += t(5) + "<Edge From=\"" + P("complete_" + how) + "\" To=\"SetFalse\" />\r\n";
        }
        s += t(5) + "<Edge From=\"questState.OnFailed\" To=\"SetFalse\" />\r\n";
        s += t(4) + "</State>\r\n";
        s += t(4) + "<State Name=\"letin\" TypeT=\"bool\">\r\n";
        s += t(5) + "<Edge From=\"helperTalk.door\" To=\"SetTrue\" />\r\n";
        s += t(5) + "<Edge From=\"questState.OnFailed\" To=\"SetFalse\" />\r\n";
        s += t(4) + "</State>\r\n";
        if (keyed) {
            s += t(4) + "<State Name=\"keyed\" TypeT=\"bool\">\r\n";
            s += t(5) + "<Edge From=\"helperTalk.key\" To=\"SetTrue\" />\r\n";
            s += t(4) + "</State>\r\n";
        }
        s += t(4) + "<" + helper_node(quest.id) + " Name=\"helperTalk\">\r\n";
        s += t(5) + "<Edge From=\"helperOpen.State\" To=\"active\" />\r\n";
        s += t(5) + "<Edge From=\"letin.State\" To=\"letin\" />\r\n";
        if (keyed) s += t(5) + "<Edge From=\"keyed.State\" To=\"keyed\" />\r\n";
        s += t(4) + "</" + helper_node(quest.id) + ">\r\n";
        s += t(4) + "<SetEntityContextPreset Name=\"letinPreset\">\r\n";
        s += t(5) + "<Constant Name=\"Preset\" Value=\"" + std::string(kWelcomePreset) + "\" />\r\n";
        s += t(5) + "<Asset Name=\"Souls\" Alias=\"holderHousehold\" />\r\n";
        s += t(5) + "<Edge From=\"letin.State\" To=\"IsActive\" />\r\n";
        s += t(4) + "</SetEntityContextPreset>\r\n";
        s += t(4) + "<SetEntityContext Name=\"letinExtended\">\r\n";
        s += t(5) + "<Constant Name=\"Context\" Value=\"" + std::string(kWelcomeExtended) + "\" />\r\n";
        s += t(5) + "<Asset Name=\"Souls\" Alias=\"holderHousehold\" />\r\n";
        s += t(5) + "<Edge From=\"letin.State\" To=\"IsActive\" />\r\n";
        s += t(4) + "</SetEntityContext>\r\n";
    }
    // An exposure (Words Must Not Fail): the house the holder is caught in -- the witness's
    // household, and him -- expects Henry from the catching on, the giver's welcome shape,
    // so that once he has seen them he can stand there, wake the man and come and go
    // without a crime. Not before (notes.md 22 Oct 2026): the house is private until he
    // has seen them, and getting in at night is his own affair, as it would be. For good
    // once done, as a giver's is, and off if the errand fails.
    if (caught) {
        s += t(4) + "<State Name=\"exposeWelcome\" TypeT=\"bool\">\r\n";
        s += t(5) + "<Edge From=\"caught.OnAcquire\" To=\"SetTrue\" />\r\n";
        s += t(5) + "<Edge From=\"questState.OnFailed\" To=\"SetFalse\" />\r\n";
        s += t(4) + "</State>\r\n";
        s += t(4) + "<SetEntityContextPreset Name=\"exposePreset\">\r\n";
        s += t(5) + "<Constant Name=\"Preset\" Value=\"" + std::string(kWelcomePreset) + "\" />\r\n";
        s += t(5) + "<Asset Name=\"Souls\" Alias=\"holderHousehold\" />\r\n";
        s += t(5) + "<Edge From=\"exposeWelcome.State\" To=\"IsActive\" />\r\n";
        s += t(4) + "</SetEntityContextPreset>\r\n";
        s += t(4) + "<SetEntityContext Name=\"exposeExtended\">\r\n";
        s += t(5) + "<Constant Name=\"Context\" Value=\"" + std::string(kWelcomeExtended) + "\" />\r\n";
        s += t(5) + "<Asset Name=\"Souls\" Alias=\"holderHousehold\" />\r\n";
        s += t(5) + "<Edge From=\"exposeWelcome.State\" To=\"IsActive\" />\r\n";
        s += t(4) + "</SetEntityContext>\r\n";
    }
    // A run's leg keeps its own "on" and "done" (the run's questState is shared): on from
    // its accept to its hand-in or the run's failure; done from its hand-in.
    if (ctx.multi) {
        s += t(4) + "<State Name=\"legActive\" TypeT=\"bool\">\r\n";
        s += t(5) + "<Edge From=\"" + P("accept") + "\" To=\"SetTrue\" />\r\n";
        for (const std::string& port : out.complete_ports) s += t(5) + "<Edge From=\"" + port + "\" To=\"SetFalse\" />\r\n";
        s += t(5) + "<Edge From=\"questState.OnFailed\" To=\"SetFalse\" />\r\n";
        s += t(4) + "</State>\r\n";
        s += t(4) + "<State Name=\"legDone\" TypeT=\"bool\">\r\n";
        for (const std::string& port : out.complete_ports) s += t(5) + "<Edge From=\"" + port + "\" To=\"SetTrue\" />\r\n";
        s += t(4) + "</State>\r\n";
    }
    out.after = s;
    s.clear();
    // How this one ended, for the link after it: done, and done each untrue way (the
    // objective's own Completed state for it), and done no untrue way.
    for (const std::string& how : untrue_ways) {
        s += t(4) + "<Function Name=\"isDone_" + how + "\" MethodName=\"math::boolean::And\" DeclaringType=\"math::boolean\">\r\n";
        s += t(5) + "<Edge From=\"" + done_bool + "\" To=\"A\" />\r\n";
        s += t(5) + "<Edge From=\"objState." + resolution_state(how) + "\" To=\"B\" />\r\n";
        s += t(4) + "</Function>\r\n";
    }
    if (!untrue_ways.empty()) {
        // Or of the untrue ways, two at a time, then Done and not that.
        std::string any = "objState." + resolution_state(untrue_ways[0]);
        for (std::size_t i = 1; i < untrue_ways.size(); ++i) {
            const std::string name = "untrue_" + std::to_string(i);
            s += t(4) + "<Function Name=\"" + name + "\" MethodName=\"math::boolean::Or\" DeclaringType=\"math::boolean\">\r\n";
            s += t(5) + "<Edge From=\"" + any + "\" To=\"A\" />\r\n";
            s += t(5) + "<Edge From=\"objState." + resolution_state(untrue_ways[i]) + "\" To=\"B\" />\r\n";
            s += t(4) + "</Function>\r\n";
            any = name + ".bool";
        }
        s += t(4) + "<Function Name=\"not_untrue\" MethodName=\"math::boolean::Not\" DeclaringType=\"math::boolean\">\r\n";
        s += t(5) + "<Edge From=\"" + any + "\" To=\"Value\" />\r\n";
        s += t(4) + "</Function>\r\n";
        s += t(4) + "<Function Name=\"isDoneTruth\" MethodName=\"math::boolean::And\" DeclaringType=\"math::boolean\">\r\n";
        s += t(5) + "<Edge From=\"" + done_bool + "\" To=\"A\" />\r\n";
        s += t(5) + "<Edge From=\"not_untrue.bool\" To=\"B\" />\r\n";
        s += t(4) + "</Function>\r\n";
    }
    out.done_logic = s;
    // How this one ended, for the link after it (the Out ports): done, and done each
    // untrue way (the objective's own Completed state for it), and done no untrue way.
    const std::string done_port = ctx.multi ? quest_node(quest.id) + "_done" : "done";
    out.out_ports.push_back(done_port);
    out.out_ports.push_back(done_port + "_truth");
    for (const std::string& how : untrue_ways) out.out_ports.push_back(done_port + "_" + how);
    out.done_edges.push_back(done_bool + " " + done_port);
    out.done_edges.push_back(std::string(untrue_ways.empty() ? done_bool : "isDoneTruth.bool") + " " + done_port + "_truth");
    for (const std::string& how : untrue_ways) {
        out.done_edges.push_back("isDone_" + how + ".bool " + done_port + "_" + how);
    }
    s.clear();
    auto state = [&](const char* name, const char* value) {
        return t(5) + "<StateTypeEnumeration Name=\"" + std::string(name) +
               "\" ObjectiveValueType=\"" + value + "\" />\r\n";
    };
    s += state("None", "None");
    s += state("Active", "Started");
    if (!quest.log_target_dead.empty()) s += state("TargetDead", "Updated");
    if (opened) s += state("Opened", "Updated");
    // The moment that opens the return objective ticks this one off, not merely notes it:
    // the gather's "5/5" first (7 Oct 2026), and now the ask's truth got, the retrieve's
    // thing handed over, the travel's look, the beat, the prayer, the thing put down, the
    // mind's last visit the same -- "Find out from Matilda" stood open on the map beside
    // "Return to Anna" after Matilda had talked (notes.md, 8 Oct 2026). The shipped types
    // write Updated after Completed too (drak.xml's standoff), so an ask carried on still
    // gets its Onward line on the ticked objective, and the last leg is its own objective.
    const char* const mid = return_from.empty() ? "Updated" : "Completed";
    if (seen) s += state("Seen", mid);
    if (beaten) s += state("Beaten", mid);
    if (prayed) s += state("Prayed", mid);
    if (left) s += state("Left", mid);
    if (directed) s += state("Directed", "Updated");
    if (ambushed) s += state("Ambushed", "Updated");
    if (told) s += state("Told", mid);
    if (handed) s += state("Handed", mid);
    if (gathered) s += state("Gathered", "Completed");
    if (onward) s += state("Onward", "Updated");
    if (rebuffed) s += state("Rebuffed", "Updated");
    if (named) s += state("Named", "Updated");
    if (named) s += state("Letin", "Updated");
    if (keyed) s += state("Keyed", "Updated");
    if (written) s += state("Written", "Updated");
    if (caught) s += state("Caught", "Updated");
    if (followed) s += state("Followed", "Updated");
    if (followed) s += state("Shown", "Updated");
    if (followed) s += state("Hidden", "Updated");
    if (bedside) s += state("Denouncing", "Updated");
    for (int i = 1; i <= checks; ++i) {
        s += state(("Check" + std::to_string(i)).c_str(), i == checks ? mid : "Updated");
    }
    s += state("Done", "Completed");
    for (const std::string& how : ways) s += state(resolution_state(how).c_str(), "Completed");
    s += state("Abandoned", "Canceled");
    s += state("Failed", "Canceled");
    out.types = s;
    s.clear();
    if (!find && !pray && !spot) {
        s += t(4) + "<SoulAsset Name=\"target\" SharedSoulGuids=\"" + xml_escape(quest.target_guid) +
             "\" />\r\n";
    }
    // The one an ask's word is carried on to: the last objective's marker sits on them.
    if (onward) {
        s += t(4) + "<SoulAsset Name=\"onward\" SharedSoulGuids=\"" + xml_escape(quest.onward_guid) +
             "\" />\r\n";
    }
    // A find's keepsake, lying in the grass: the marker binds to any item of the class on
    // the ground, as *Sacks* (socky.xml) binds to the sacks the player dropped.
    if (keepsake) {
        s += t(4) + "<ItemOnGroundAsset Name=\"keepsake\" ItemClassGuids=\"" + xml_escape(quest.keepsake_class) +
             "\" />\r\n";
    }
    // The one who can let Henry in, and the holder's household her door opens (the holder,
    // everyone who sleeps under his roof, and her).
    // An exposure's is the witness's household and the holder in it (npcdb read_helper).
    if (named) {
        s += t(4) + "<SoulAsset Name=\"helper\" SharedSoulGuids=\"" + xml_escape(quest.helper_guid) + "\" />\r\n";
    }
    if (named || caught) {
        std::string guids;
        for (const std::string& g : quest.holder_household_guids) {
            if (!guids.empty()) guids += ' ';
            guids += g;
        }
        s += t(4) + "<SoulAsset Name=\"holderHousehold\" SharedSoulGuids=\"" + xml_escape(guids) + "\" />\r\n";
    }
    out.assets_a = s;
    out.wants_player = seen || prayed || left || ambushed || gathered || named || written || caught;
    s.clear();
    if (quest.has_brawl() && (seen || ambushed)) {
        std::string guids;
        for (const std::string& g : quest.brawl_guids) {
            if (!guids.empty()) guids += ' ';
            guids += g;
        }
        s += t(4) + "<SoulAsset Name=\"brawlers\" SharedSoulGuids=\"" + xml_escape(guids) + "\" />\r\n";
    }
    out.assets_b = s;
    s.clear();
    s += t(4) + "<Objective TypeT=\"" + xml_escape(objective_type(quest.id)) + "\" Name=\"fetch\">\r\n";
    s += t(5) + "<LocalizedName StringName=\"" + key("objective") + "\" Text=\"" +
         xml_escape(quest.objective) + "\" />\r\n";
    s += t(5) + "<Logs>\r\n";
    s += t(6) + "<EnumLog Type=\"None\" Name=\"None\" />\r\n";
    // A street night's (has_follow) Started carries no marker: the holder's is the Lua's to
    // put on and take off with his hours (Shown/Hidden below; emit.hpp mark_shown_class).
    s += t(6) + "<EnumLog Type=\"Started\" Name=\"Active\" IsTracked=\"true\"" +
         std::string(followed ? "" : " Marker=\"" + std::string(marker) + "\"") + ">\r\n";
    s += t(7) + "<Log StringName=\"" + key("log_started") + "\" Text=\"" +
         xml_escape(quest.log_started) + "\" />\r\n";
    s += t(6) + "</EnumLog>\r\n";
    if (!quest.log_target_dead.empty()) {
        s += t(6) + "<EnumLog Type=\"Updated\" Name=\"TargetDead\" Marker=\"target\">\r\n";
        s += t(7) + "<Log StringName=\"" + key("log_target_dead") + "\" Text=\"" +
             xml_escape(quest.log_target_dead) + "\" />\r\n";
        s += t(6) + "</EnumLog>\r\n";
    }
    if (opened) {
        s += t(6) + "<EnumLog Type=\"Updated\" Name=\"Opened\" Marker=\"target\">\r\n";
        s += t(7) + "<Log StringName=\"" + key("log_opened") + "\" Text=\"" +
             xml_escape(quest.log_opened) + "\" />\r\n";
        s += t(6) + "</EnumLog>\r\n";
    }
    // The mid-moment's line: Completed when it opens the return objective (the type above
    // says the same), with no marker of its own since the return's Started line carries
    // the giver's from the same moment; Updated with the giver's marker where there is no
    // return to open.
    const auto mid_log = [&](const char* name) {
        return return_from.empty()
                   ? "<EnumLog Type=\"Updated\" Name=\"" + std::string(name) + "\" Marker=\"giver\">\r\n"
                   : "<EnumLog Type=\"Completed\" Name=\"" + std::string(name) + "\">\r\n";
    };
    if (seen) {
        s += t(6) + mid_log("Seen");
        s += t(7) + "<Log StringName=\"" + key("log_seen") + "\" Text=\"" +
             xml_escape(quest.log_seen) + "\" />\r\n";
        s += t(6) + "</EnumLog>\r\n";
    }
    if (beaten) {
        s += t(6) + mid_log("Beaten");
        s += t(7) + "<Log StringName=\"" + key("log_beaten") + "\" Text=\"" +
             xml_escape(quest.log_beaten) + "\" />\r\n";
        s += t(6) + "</EnumLog>\r\n";
    }
    if (prayed) {
        s += t(6) + mid_log("Prayed");
        s += t(7) + "<Log StringName=\"" + key("log_prayed") + "\" Text=\"" +
             xml_escape(quest.log_prayed) + "\" />\r\n";
        s += t(6) + "</EnumLog>\r\n";
    }
    if (left) {
        s += t(6) + mid_log("Left");
        s += t(7) + "<Log StringName=\"" + key("log_left") + "\" Text=\"" +
             xml_escape(quest.log_left) + "\" />\r\n";
        s += t(6) + "</EnumLog>\r\n";
    }
    // An ambush's directions keep the marker on the one who gave them (the place is named
    // in words, and Henry comes back to him), and so does the trap sprung.
    if (directed) {
        s += t(6) + "<EnumLog Type=\"Updated\" Name=\"Directed\" Marker=\"target\">\r\n";
        s += t(7) + "<Log StringName=\"" + key("log_directed") + "\" Text=\"" +
             xml_escape(quest.log_directed) + "\" />\r\n";
        s += t(6) + "</EnumLog>\r\n";
    }
    if (ambushed) {
        s += t(6) + "<EnumLog Type=\"Updated\" Name=\"Ambushed\" Marker=\"target\">\r\n";
        s += t(7) + "<Log StringName=\"" + key("log_ambushed") + "\" Text=\"" +
             xml_escape(quest.log_ambushed) + "\" />\r\n";
        s += t(6) + "</EnumLog>\r\n";
    }
    // The dictated letter written and sealed: the marker stays on the one it goes to.
    if (written) {
        s += t(6) + "<EnumLog Type=\"Updated\" Name=\"Written\" Marker=\"target\">\r\n";
        s += t(7) + "<Log StringName=\"" + key("log_written") + "\" Text=\"" +
             xml_escape(quest.log_written) + "\" />\r\n";
        s += t(6) + "</EnumLog>\r\n";
    }
    // The holder caught where he should not be: the marker stays on him, since he is the
    // one to be faced with it.
    if (caught) {
        s += t(6) + "<EnumLog Type=\"Updated\" Name=\"Caught\" Marker=\"target\">\r\n";
        s += t(7) + "<Log StringName=\"" + key("log_caught") + "\" Text=\"" +
             xml_escape(quest.log_caught) + "\" />\r\n";
        s += t(6) + "</EnumLog>\r\n";
    }
    // The holder has caught Henry following him: he sleeps at home tonight, and the marker
    // comes off him for the night (bugs.txt 28 Oct 2026) -- the Lua takes `marked` down
    // with the sighting, and this state carries none of its own. Shown and Hidden are the
    // marker's own states, written by the Lua's tokens as his hours come and go: marker-only
    // Updated lines without journal text, as 21 shipped objectives write theirs.
    if (followed) {
        s += t(6) + "<EnumLog Type=\"Updated\" Name=\"Followed\">\r\n";
        s += t(7) + "<Log StringName=\"" + key("log_followed") + "\" Text=\"" +
             xml_escape(quest.log_followed) + "\" />\r\n";
        s += t(6) + "</EnumLog>\r\n";
        s += t(6) + "<EnumLog Type=\"Updated\" Name=\"Shown\" Marker=\"target\" />\r\n";
        s += t(6) + "<EnumLog Type=\"Updated\" Name=\"Hidden\" />\r\n";
    }
    // Denounced at the bedside: the thing stays his, and the giver is to be told.
    if (bedside) {
        s += t(6) + "<EnumLog Type=\"Updated\" Name=\"Denouncing\" Marker=\"giver\">\r\n";
        s += t(7) + "<Log StringName=\"" + key("log_denounced") + "\" Text=\"" +
             xml_escape(quest.log_denounced) + "\" />\r\n";
        s += t(6) + "</EnumLog>\r\n";
    }
    if (told) {
        s += t(6) + mid_log("Told");
        s += t(7) + "<Log StringName=\"" + key("log_told") + "\" Text=\"" +
             xml_escape(quest.log_told) + "\" />\r\n";
        s += t(6) + "</EnumLog>\r\n";
    }
    if (handed) {
        s += t(6) + mid_log("Handed");
        s += t(7) + "<Log StringName=\"" + key("log_handed") + "\" Text=\"" +
             xml_escape(handed_log(quest)) + "\" />\r\n";
        s += t(6) + "</EnumLog>\r\n";
    }
    if (gathered) {
        s += t(6) + "<EnumLog Type=\"Completed\" Name=\"Gathered\">\r\n";
        s += t(7) + "<Log StringName=\"" + key("log_gathered") + "\" Text=\"" +
             xml_escape(gathered_log(quest)) + "\" />\r\n";
        s += t(6) + "</EnumLog>\r\n";
    }
    // The word sent on: the giver has been told and paid, and the marker moves to the one
    // Henry is to carry it to (the last objective below is tracked the same way).
    if (onward) {
        s += t(6) + "<EnumLog Type=\"Updated\" Name=\"Onward\" Marker=\"onward\">\r\n";
        s += t(7) + "<Log StringName=\"" + key("log_onward") + "\" Text=\"" +
             xml_escape(quest.log_onward) + "\" />\r\n";
        s += t(6) + "</EnumLog>\r\n";
    }
    // The unyielding holder's refusal sends Henry back to the giver (marker on him); the
    // giver's naming, the door and the key keep this objective's marker on the holder --
    // the thing is still his, in his chest -- while the optional objective below marks the
    // one who can let Henry in.
    if (rebuffed) {
        s += t(6) + "<EnumLog Type=\"Updated\" Name=\"Rebuffed\" Marker=\"giver\">\r\n";
        s += t(7) + "<Log StringName=\"" + key("log_rebuffed") + "\" Text=\"" +
             xml_escape(filled.log_rebuffed) + "\" />\r\n";
        s += t(6) + "</EnumLog>\r\n";
    }
    if (named) {
        s += t(6) + "<EnumLog Type=\"Updated\" Name=\"Named\" Marker=\"target\">\r\n";
        s += t(7) + "<Log StringName=\"" + key("log_named") + "\" Text=\"" +
             xml_escape(filled.log_named) + "\" />\r\n";
        s += t(6) + "</EnumLog>\r\n";
        s += t(6) + "<EnumLog Type=\"Updated\" Name=\"Letin\" Marker=\"target\">\r\n";
        s += t(7) + "<Log StringName=\"" + key("log_letin") + "\" Text=\"" +
             xml_escape(filled.log_letin) + "\" />\r\n";
        s += t(6) + "</EnumLog>\r\n";
    }
    if (keyed) {
        s += t(6) + "<EnumLog Type=\"Updated\" Name=\"Keyed\" Marker=\"target\">\r\n";
        s += t(7) + "<Log StringName=\"" + key("log_keyed") + "\" Text=\"" +
             xml_escape(filled.log_keyed) + "\" />\r\n";
        s += t(6) + "</EnumLog>\r\n";
    }
    // A mind's visits keep the marker on the one looked in on until the last, which sends
    // Henry back to the giver.
    for (int i = 1; i <= checks; ++i) {
        const std::string field = "log_check_" + std::to_string(i);
        s += t(6) + (i == checks ? mid_log(("Check" + std::to_string(i)).c_str())
                                 : "<EnumLog Type=\"Updated\" Name=\"Check" + std::to_string(i) +
                                       "\" Marker=\"target\">\r\n");
        s += t(7) + "<Log StringName=\"" + key(field.c_str()) + "\" Text=\"" +
             xml_escape(quest.log_check[i - 1]) + "\" />\r\n";
        s += t(6) + "</EnumLog>\r\n";
    }
    s += t(6) + "<EnumLog Type=\"Completed\" Name=\"Done\">\r\n";
    s += t(7) + "<Log StringName=\"" + key("log_completed") + "\" Text=\"" +
         xml_escape(quest.log_completed) + "\" />\r\n";
    s += t(6) + "</EnumLog>\r\n";
    for (const std::string& how : ways) {
        const std::string field = "log_completed_" + how;
        s += t(6) + "<EnumLog Type=\"Completed\" Name=\"" + resolution_state(how) + "\">\r\n";
        s += t(7) + "<Log StringName=\"" + key(field.c_str()) + "\" Text=\"" +
             xml_escape(*resolution_log(quest, how)) + "\" />\r\n";
        s += t(6) + "</EnumLog>\r\n";
    }
    s += t(6) + "<EnumLog Type=\"Canceled\" Name=\"Abandoned\">\r\n";
    s += t(7) + "<Log StringName=\"" + key("log_abandoned") + "\" Text=\"" +
         xml_escape(quest.abandoned_log()) + "\" />\r\n";
    s += t(6) + "</EnumLog>\r\n";
    s += t(6) + "<EnumLog Type=\"Canceled\" Name=\"Failed\">\r\n";
    s += t(7) + "<Log StringName=\"" + key("log_giver_dead") + "\" Text=\"" +
         xml_escape(quest.log_giver_dead) + "\" />\r\n";
    s += t(6) + "</EnumLog>\r\n";
    s += t(5) + "</Logs>\r\n";
    s += t(4) + "</Objective>\r\n";
    if (!return_from.empty()) {
        s += t(4) + "<Objective TypeT=\"ProgressFailable\" Name=\"" + std::string(kReturnObjective) + "\">\r\n";
        s += t(5) + "<LocalizedName StringName=\"" + key("objective_return") + "\" Text=\"" +
             xml_escape(return_objective(quest)) + "\" />\r\n";
        s += t(5) + "<Logs>\r\n";
        s += t(6) + "<EnumLog Type=\"None\" Name=\"None\" />\r\n";
        s += t(6) + "<EnumLog Type=\"Started\" Name=\"Active\" IsTracked=\"true\" Marker=\"giver\" />\r\n";
        s += t(6) + "<EnumLog Type=\"Completed\" Name=\"Done\" />\r\n";
        s += t(6) + "<EnumLog Type=\"Canceled\" Name=\"Canceled\" />\r\n";
        s += t(5) + "</Logs>\r\n";
        s += t(4) + "</Objective>\r\n";
    }
    if (onward) {
        s += t(4) + "<Objective TypeT=\"ProgressFailable\" Name=\"" + std::string(kOnwardObjective) + "\">\r\n";
        s += t(5) + "<LocalizedName StringName=\"" + key("objective_onward") + "\" Text=\"" +
             xml_escape(quest.objective_onward) + "\" />\r\n";
        s += t(5) + "<Logs>\r\n";
        s += t(6) + "<EnumLog Type=\"None\" Name=\"None\" />\r\n";
        s += t(6) + "<EnumLog Type=\"Started\" Name=\"Active\" IsTracked=\"true\" Marker=\"onward\" />\r\n";
        s += t(6) + "<EnumLog Type=\"Completed\" Name=\"Done\" />\r\n";
        s += t(6) + "<EnumLog Type=\"Canceled\" Name=\"Canceled\" />\r\n";
        s += t(5) + "</Logs>\r\n";
        s += t(4) + "</Objective>\r\n";
    }
    // The optional objective, "Speak to <helper>": IsOptional as the shipped side steps
    // are written, tracked with the marker on her from the naming until the quest closes.
    if (named) {
        s += t(4) + "<Objective TypeT=\"ProgressFailable\" Name=\"" + std::string(kHelperObjective) + "\" IsOptional=\"true\">\r\n";
        s += t(5) + "<LocalizedName StringName=\"" + key("objective_helper") + "\" Text=\"" +
             xml_escape(filled.objective_helper) + "\" />\r\n";
        s += t(5) + "<Logs>\r\n";
        s += t(6) + "<EnumLog Type=\"None\" Name=\"None\" />\r\n";
        s += t(6) + "<EnumLog Type=\"Started\" Name=\"Active\" IsTracked=\"true\" Marker=\"helper\" />\r\n";
        s += t(6) + "<EnumLog Type=\"Completed\" Name=\"Done\" />\r\n";
        s += t(6) + "<EnumLog Type=\"Canceled\" Name=\"Canceled\" />\r\n";
        s += t(5) + "</Logs>\r\n";
        s += t(4) + "</Objective>\r\n";
    }
    out.objectives = s;
    return out;
}

// A run of several scenes: every name a leg owns gets the leg's suffix, so two legs' nodes,
// assets, objectives and types sit in one file without a collision. The names are the
// leg's own node-level elements (four tabs in), its assets and its objectives; the run's
// shared names (questState, giver, household, player...) are never among them. Edges
// From a leg's node, Alias= of a leg's asset, the objective element tags and the save's
// Objective constant are rewritten to match.
std::string suffix_leg(LegOut& leg, const std::string& sfx) {
    if (sfx.empty()) return {};
    std::set<std::string> names;
    static const std::regex node_re("\n\t\t\t\t<([A-Za-z_0-9]+) Name=\"([A-Za-z_0-9]+)\"");
    for (const std::string* part : {&leg.before, &leg.after, &leg.done_logic}) {
        const std::string text = "\n" + *part;
        for (std::sregex_iterator it(text.begin(), text.end(), node_re), end; it != end; ++it) {
            names.insert((*it)[2].str());
        }
    }
    for (const char* alias : {"target", "onward", "keepsake", "brawlers", "helper", "holderHousehold"}) names.insert(alias);
    const std::vector<std::string> objectives = {"fetch", kReturnObjective, kOnwardObjective, kHelperObjective};
    auto rewrite = [&](std::string& text) {
        for (const std::string& name : names) {
            text = replace_all(text, "Name=\"" + name + "\"", "Name=\"" + name + sfx + "\"");
            text = replace_all(text, "Alias=\"" + name + "\"", "Alias=\"" + name + sfx + "\"");
            // The EnumLog's Marker names an asset alias too: left bare, a follower's
            // "Deliver the letter" marker sat on the head's holder (notes.md, 9 Oct
            // 2026 -- In the Stocks, Father Adam's letter marked on Paul Schmidt).
            text = replace_all(text, "Marker=\"" + name + "\"", "Marker=\"" + name + sfx + "\"");
            text = replace_all(text, "From=\"" + name + ".", "From=\"" + name + sfx + ".");
        }
        for (const std::string& name : objectives) {
            text = replace_all(text, "<" + name + " Name=\"", "<" + name + sfx + " Name=\"");
            text = replace_all(text, "</" + name + ">", "</" + name + sfx + ">");
            text = replace_all(text, "Name=\"" + name + "\">", "Name=\"" + name + sfx + "\">");
            text = replace_all(text, "Value=\"" + name + "\"", "Value=\"" + name + sfx + "\"");
        }
    };
    for (std::string* part : {&leg.before, &leg.after, &leg.done_logic, &leg.assets_a, &leg.assets_b, &leg.objectives}) {
        rewrite(*part);
    }
    for (std::string& edge : leg.done_edges) {
        for (const std::string& name : names) {
            if (edge.compare(0, name.size() + 1, name + ".") == 0) edge = name + sfx + edge.substr(name.size());
        }
    }
    return sfx;
}

}  // namespace

std::string emit_quest(const std::string& modid, const Quest& quest) {
    return emit_quest(modid, QuestRun{{&quest}});
}

std::string emit_quest(const std::string& modid, const QuestRun& run) {
    if (run.legs.empty()) throw std::runtime_error("a run with no scenes");
    const Quest& head = *run.legs.front();
    const bool multi = run.legs.size() > 1;
    const std::string node = quest_node(head.id);
    auto t = [](int n) { return std::string(static_cast<std::size_t>(n), '\t'); };
    auto key = [&](const char* field) { return xml_escape(quest_key(modid, head.id, field)); };

    // Each scene's share, then its names suffixed for its place in the run (the head's
    // are bare, so a save with the head under way reads on).
    std::vector<LegOut> legs;
    for (std::size_t i = 0; i < run.legs.size(); ++i) {
        const Quest& leg = *run.legs[i];
        LegCtx ctx;
        ctx.multi = multi;
        std::vector<const Quest*> followers;
        for (const Quest* other : run.legs) {
            if (other->after_quest_id == leg.id) followers.push_back(other);
        }
        ctx.leads_on = [followers](const std::string& how) {
            for (const Quest* f : followers) {
                if (port_leads_on(how, f->after_how)) return true;
            }
            return false;
        };
        for (const Quest* f : followers) ctx.follower_accepts.push_back(quest_port({f->id, "accept", 0}));
        legs.push_back(emit_leg(modid, leg, ctx));
        suffix_leg(legs.back(), i == 0 ? "" : "_" + std::to_string(i + 1));
    }

    std::string s = skald_header(modid);
    // Micro is the type the shipped fetch-and-return errands carry (U02 Hříšná duše,
    // U304 Starý mnich), and the journal groups them under the same heading. No
    // Difficulty or ProductionCode: both are editor bookkeeping.
    s += t(2) + "<Quest Name=\"" + node + "\" Type=\"Micro\">\r\n";
    // The moments the dialogue reports: the errand taken up, and it paid off -- once
    // plainly, and once for each way the journal has its own line for. Everything the
    // quest does follows from one state driven by them, which is the shape of
    // poustevnik_a_vdova.xml with the middle taken out. A run of several scenes has each
    // scene's ports, named as the talk names them.
    s += t(3) + "<Ports>\r\n";
    for (const LegOut& leg : legs) {
        for (const std::string& port : leg.in_ports) {
            s += t(4) + "<Port Name=\"" + port + "\" Direction=\"In\" Type=\"trigger\" />\r\n";
        }
    }
    // The giver's map marker (below) is up while the errand can be taken: for a scene's
    // link that is only once the favour before it is done, and done a way the link
    // follows, which the favour before says on these Out ports and the project wires to
    // this In port. `done_truth` is the Lua's "truth": any ending that is not one of the
    // four the giver was lied to, kept from, left quiet about or outlived.
    if (head.is_link()) {
        s += t(4) + "<Port Name=\"before_open\" Direction=\"In\" Type=\"bool\" />\r\n";
    }
    for (const LegOut& leg : legs) {
        for (const std::string& port : leg.out_ports) {
            s += t(4) + "<Port Name=\"" + port + "\" Direction=\"Out\" Type=\"bool\" />\r\n";
        }
    }
    s += t(3) + "</Ports>\r\n";
    // The holder's dialogue is this quest's own definition, listed here the way a
    // shipped quest lists the dialogues under it, and resolved against this file's
    // folder.
    std::string definitions;
    for (const LegOut& leg : legs) definitions += leg.definitions;
    if (!definitions.empty()) {
        s += t(3) + "<Definitions>\r\n" + definitions + t(3) + "</Definitions>\r\n";
    }
    s += t(3) + "<Nodes>\r\n";
    // wh::questmodule::QuestProgress is the state the journal reads. SetActive puts the
    // quest in the book, SetDone completes it, SetFailed fails it -- the three ends the
    // shipped micro quests wire, in the same names.
    // RequiredForOutput on the quest's state and objective nodes as the shipped files have
    // it (sedmStatecnych.xml's questProgress, poustevnik_a_vdova.xml's questState and
    // objectiveVisual5): the Level the quest sits in.
    const std::string rfo = std::string(" RequiredForOutput=\"") + kQuestLevel + "\"";
    s += t(4) + "<State Name=\"questState\"" + rfo + " TypeT=\"wh::questmodule::QuestProgress\">\r\n";
    s += t(5) + "<Edge From=\"" + legs.front().in_ports.front() + "\" To=\"SetActive\" />\r\n";
    // `onward` is a beat in the middle (the giver told and paid, Henry sent on), not an
    // end: the quest stays Active until the word is said where it is going. In a run, a
    // hand-in whose ending leads on to another scene is not an end either: the run is
    // done at the hand-in no scene follows (stages.md).
    for (std::size_t i = 0; i < legs.size(); ++i) {
        const Quest& leg = *run.legs[i];
        std::vector<const Quest*> followers;
        for (const Quest* other : run.legs) {
            if (other->after_quest_id == leg.id) followers.push_back(other);
        }
        auto leads_on = [&](const std::string& how) {
            for (const Quest* f : followers) {
                if (port_leads_on(how, f->after_how)) return true;
            }
            return false;
        };
        for (const std::string& command : quest_commands(leg)) {
            if (command == "accept" || command == "onward" || command == "abandon") continue;
            const std::string how = command == "complete" ? "" : command.substr(std::string("complete_").size());
            if (leads_on(how)) continue;
            s += t(5) + "<Edge From=\"" + (multi ? quest_port({leg.id, command, 0}) : command) + "\" To=\"SetDone\" />\r\n";
        }
    }
    // Given up to any giver's face: the quest (a run's whole story) fails in the book, and
    // everything that follows OnFailed (the welcome, a leg's "on", the return) closes.
    for (std::size_t i = 0; i < legs.size(); ++i) {
        s += t(5) + "<Edge From=\"" + (multi ? quest_port({run.legs[i]->id, "abandon", 0}) : std::string("abandon")) +
             "\" To=\"SetFailed\" />\r\n";
    }
    s += t(5) + "<Edge From=\"giverDeath.OnDeath\" To=\"SetFailed\" />\r\n";
    s += t(4) + "</State>\r\n";
    if (multi) {
        for (const LegOut& leg : legs) s += leg.before + leg.after;
    } else {
        s += legs.front().before;
    }
    // Only watched while the quest is Active: a giver who dies before Henry ever
    // heard of the errand fails nothing, and one who dies after it is paid is not a
    // failure either.
    s += t(4) + "<SoulDeathTrigger Name=\"giverDeath\">\r\n";
    s += t(5) + "<Asset Name=\"Souls\" Alias=\"giver\" />\r\n";
    s += t(5) + "<Edge From=\"questState.Active\" To=\"IsActive\" />\r\n";
    s += t(4) + "</SoulDeathTrigger>\r\n";
    // The giver's house open to Henry (head.welcome): from the moment the errand is taken
    // up, and for good after it is done -- a man who carried her husband's flowers is not
    // a stranger at her door the next day -- the household's souls carry the game's own
    // "the player is expected" context, which is how the shipped quests open a house to
    // him (utils/crime/trespass/expectedeffect: the preset, and the extended context that
    // keeps it until he has left the private area; 248 uses in the Final graphs). Only
    // the household, not the rooms: a TriggerArea reaches a quest through the level's own
    // Link rows, which a mod cannot write, so an AreaLabelEffect "public" is not open to
    // us. Off again if the errand fails -- the giver dead, the house someone else's.
    const bool welcome = head.welcome && !head.welcome_guids.empty();
    if (welcome) {
        s += t(4) + "<State Name=\"welcome\" TypeT=\"bool\">\r\n";
        s += t(5) + "<Edge From=\"questState.OnActive\" To=\"SetTrue\" />\r\n";
        s += t(5) + "<Edge From=\"questState.OnFailed\" To=\"SetFalse\" />\r\n";
        s += t(4) + "</State>\r\n";
        s += t(4) + "<SetEntityContextPreset Name=\"welcomePreset\">\r\n";
        s += t(5) + "<Constant Name=\"Preset\" Value=\"" + std::string(kWelcomePreset) + "\" />\r\n";
        s += t(5) + "<Asset Name=\"Souls\" Alias=\"household\" />\r\n";
        s += t(5) + "<Edge From=\"welcome.State\" To=\"IsActive\" />\r\n";
        s += t(4) + "</SetEntityContextPreset>\r\n";
        s += t(4) + "<SetEntityContext Name=\"welcomeExtended\">\r\n";
        s += t(5) + "<Constant Name=\"Context\" Value=\"" + std::string(kWelcomeExtended) + "\" />\r\n";
        s += t(5) + "<Asset Name=\"Souls\" Alias=\"household\" />\r\n";
        s += t(5) + "<Edge From=\"welcome.State\" To=\"IsActive\" />\r\n";
        s += t(4) + "</SetEntityContext>\r\n";
    }
    // The giver's day before the errand (head.routine; behaviors.md §4.6): until Henry
    // takes it up, the scheduler tag is on, which in the level's scheduler override
    // (scripts/build_scheduler.py) turns her work link into the one to the wrapper
    // hub with that behaviour for her work hours -- Joan weeping on the farm bench from
    // ten to seven instead of the yard -- and moves her way to work to a home-hub link
    // dead under the tag. The accept is what gives her hope: the tag goes off with it
    // (questState leaves None), the bench link is HaltOnInvalid and drops her at once,
    // and she is at her chores again; done or failed keep it off. The shape is svatyne's
    // `questProgress_svatyne.None -> IsActive` (painterForcedStanding). Beside it the
    // game's own "don't comment on the player" preset on her over the same span, so the one
    // on the bench does not look up with "At your service, sir" -- the double tried before
    // this did exactly that (§8.7) -- nor "What a good doggy" at Mutt (notes.md 8 Oct 2026;
    // the greeting context alone left the dog bark, which is its own reaction). Note that
    // only the graph moves questState: a QuestCommand from Lua alone moves the store, so a
    // routine is tested through her menu.
    if (!head.routine.empty()) {
        s += t(4) + "<SchedulerLinkActivation Name=\"routineLink\">\r\n";
        s += t(5) + "<Constant Name=\"LinkTag\" Value=\"" + xml_escape(routine_tag(modid, head.id)) + "\" />\r\n";
        s += t(5) + "<Edge From=\"questState.None\" To=\"IsActive\" />\r\n";
        s += t(4) + "</SchedulerLinkActivation>\r\n";
        s += t(4) + "<SetEntityContextPreset Name=\"routineQuiet\">\r\n";
        s += t(5) + "<Constant Name=\"Preset\" Value=\"" + std::string(kRoutineQuiet) + "\" />\r\n";
        s += t(5) + "<Asset Name=\"Souls\" Alias=\"giver\" />\r\n";
        s += t(5) + "<Edge From=\"questState.None\" To=\"IsActive\" />\r\n";
        s += t(4) + "</SetEntityContextPreset>\r\n";
    }
    if (!multi) s += legs.front().after;
    // The giver on the map while the errand can be taken up, with the marker the shipped
    // quests put on theirs (ShowMapMarker, MarkerType QuestGiver, as kovar.xml and
    // certiNaTroskach.xml do; the utils QuestProgressExpanded module is the same node
    // behind its SetAvailable). Up while the quest is None -- not yet taken, not failed --
    // and, for a scene's link, while the favour before it is done the way the link wants;
    // gone the moment the errand is accepted, when the objective's own marker takes over.
    // Asked for in the notes of 4 Oct 2026: every giver on the map like the game's own.
    // And only in her day where one is written (hail_hours; notes.md 25 Oct 2026: "only
    // active when she's in her daytime routine, not sleeping or eating"): a
    // TimeOfDayInterval (klaster_hibernate_mode.xml's shape, Constants `8h`/`20h`, live
    // while the quest is None) sets and clears a bool State the marker's And reads, as the
    // Lua's WatchHail reads the same hours (HourIn) before shouting.
    std::string marker_on = head.is_link() ? "giverMarkerOn.bool" : "questState.None";
    if (head.is_link()) {
        s += t(4) + "<Function Name=\"giverMarkerOn\" MethodName=\"math::boolean::And\" DeclaringType=\"math::boolean\">\r\n";
        s += t(5) + "<Edge From=\"questState.None\" To=\"A\" />\r\n";
        s += t(5) + "<Edge From=\"before_open\" To=\"B\" />\r\n";
        s += t(4) + "</Function>\r\n";
    }
    if (head.has_hail_hours()) {
        s += t(4) + "<TimeOfDayInterval Name=\"giverDayInterval\">\r\n";
        s += t(5) + "<Constant Name=\"TimeOfDayStart\" Value=\"" + std::to_string(head.hail_hour_from) + "h\" />\r\n";
        s += t(5) + "<Constant Name=\"TimeOfDayEnd\" Value=\"" + std::to_string(head.hail_hour_to) + "h\" />\r\n";
        s += t(5) + "<Edge From=\"questState.None\" To=\"IsActive\" />\r\n";
        s += t(4) + "</TimeOfDayInterval>\r\n";
        s += t(4) + "<State Name=\"giverDay\" TypeT=\"bool\">\r\n";
        s += t(5) + "<Edge From=\"giverDayInterval.OnIntervalStarted\" To=\"SetTrue\" />\r\n";
        s += t(5) + "<Edge From=\"giverDayInterval.OnIntervalEnded\" To=\"SetFalse\" />\r\n";
        s += t(4) + "</State>\r\n";
        s += t(4) + "<Function Name=\"giverMarkerDay\" MethodName=\"math::boolean::And\" DeclaringType=\"math::boolean\">\r\n";
        s += t(5) + "<Edge From=\"" + marker_on + "\" To=\"A\" />\r\n";
        s += t(5) + "<Edge From=\"giverDay.State\" To=\"B\" />\r\n";
        s += t(4) + "</Function>\r\n";
        marker_on = "giverMarkerDay.bool";
    }
    s += t(4) + "<ShowMapMarker Name=\"giverMarker\">\r\n";
    s += t(5) + "<Asset Name=\"MarkerObject\" Alias=\"giver\" />\r\n";
    s += t(5) + "<Constant Name=\"MarkerType\" Value=\"QuestGiver\" />\r\n";
    s += t(5) + "<Edge From=\"" + marker_on + "\" To=\"IsActive\" />\r\n";
    s += t(4) + "</ShowMapMarker>\r\n";
    for (const LegOut& leg : legs) s += leg.done_logic;
    s += t(4) + "<Output Name=\"Output\">\r\n";
    s += t(5) + "<Edge From=\"questState.State\" To=\"Progress\" />\r\n";
    for (const LegOut& leg : legs) {
        for (const std::string& edge : leg.done_edges) {
            const std::size_t space = edge.find(' ');
            s += t(5) + "<Edge From=\"" + edge.substr(0, space) + "\" To=\"" + edge.substr(space + 1) + "\" />\r\n";
        }
    }
    s += t(4) + "</Output>\r\n";
    s += t(3) + "</Nodes>\r\n";
    // The objective's own state type, one enumeration per journal line above. The
    // shipped quests declare theirs the same way, between the nodes and the assets.
    s += t(3) + "<Types>\r\n";
    for (std::size_t i = 0; i < legs.size(); ++i) {
        s += t(4) + "<Type TypeName=\"" + xml_escape(objective_type(run.legs[i]->id)) + "\">\r\n";
        s += legs[i].types;
        s += t(4) + "</Type>\r\n";
    }
    s += t(3) + "</Types>\r\n";
    // SharedSoulGuids is soul.xml's soul_id, which is the key npcs.sqlite is built on.
    s += t(3) + "<Assets>\r\n";
    s += t(4) + "<SoulAsset Name=\"giver\" SharedSoulGuids=\"" + xml_escape(head.giver_guid) +
         "\" />\r\n";
    bool wants_player = false;
    for (const LegOut& leg : legs) {
        s += leg.assets_a;
        wants_player = wants_player || leg.wants_player;
    }
    // The player's soul, as every shipped graph names it (Haste.xml, Barbora.xml). Every
    // ItemDescriptorTrigger on Henry's pocket needs it: the find's went without for a day
    // (6 Oct 2026) and its playerSoul.Inventory watched nothing -- no "n/5", no Gathered,
    // no "Return to Joan" -- while the same trigger on the pray and ambush tokens, whose
    // kinds name the asset, fired. Four in-game passes bisected everything else first.
    if (wants_player) {
        s += t(4) + "<SoulAsset Name=\"player\" SharedSoulGuids=\"" + std::string(kPlayerSoulGuid) + "\" />\r\n";
    }
    for (const LegOut& leg : legs) s += leg.assets_b;
    // The giver's household (welcome, above): the giver and everyone who sleeps under the
    // same roof, several guids to the one asset as the brawlers are.
    if (welcome) {
        std::string guids;
        for (const std::string& g : head.welcome_guids) {
            if (!guids.empty()) guids += ' ';
            guids += g;
        }
        s += t(4) + "<SoulAsset Name=\"household\" SharedSoulGuids=\"" + xml_escape(guids) + "\" />\r\n";
    }
    s += t(3) + "</Assets>\r\n";
    // Text is the designer's name for the graph; QuestName and QuestDescription are
    // what the journal shows. All three carry the words too, since every shipped file
    // does, but it is the StringName that the game reads.
    s += t(3) + "<Text StringName=\"" + key("title") + "\" Text=\"" + xml_escape(head.name) +
         "\" />\r\n";
    s += t(3) + "<QuestName StringName=\"" + key("name") + "\" Text=\"" + xml_escape(head.name) +
         "\" />\r\n";
    s += t(3) + "<QuestDescription StringName=\"" + key("description") + "\" Text=\"" +
         xml_escape(head.description) + "\" />\r\n";
    // Marker on the holder while the objective is open, which is the map pin that
    s += t(3) + "<Objectives>\r\n";
    for (const LegOut& leg : legs) s += leg.objectives;
    s += t(3) + "</Objectives>\r\n";
    s += t(2) + "</Quest>\r\n";
    s += "\t</Skald>\r\n";
    s += "</Database>\r\n";
    return s;
}


std::vector<std::pair<std::string, std::string>> quest_localization(const std::string& modid,
                                                                    const Quest& quest) {
    std::vector<std::pair<std::string, std::string>> rows = {
        {quest_key(modid, quest.id, "title"), quest.name},
        {quest_key(modid, quest.id, "name"), quest.name},
        {quest_key(modid, quest.id, "description"), quest.description},
        {quest_key(modid, quest.id, "objective"), quest.objective},
        {quest_key(modid, quest.id, "log_started"), quest.log_started},
        {quest_key(modid, quest.id, "log_completed"), quest.log_completed},
        {quest_key(modid, quest.id, "log_giver_dead"), quest.log_giver_dead},
        {quest_key(modid, quest.id, "log_abandoned"), quest.abandoned_log()},
    };
    if (quest_returns(quest)) {
        rows.emplace_back(quest_key(modid, quest.id, "objective_return"), return_objective(quest));
    }
    if (!quest.log_target_dead.empty()) {
        rows.emplace_back(quest_key(modid, quest.id, "log_target_dead"), quest.log_target_dead);
    }
    if (quest.is_deliver() && !quest.log_opened.empty()) {
        rows.emplace_back(quest_key(modid, quest.id, "log_opened"), quest.log_opened);
    }
    if (quest.is_travel() && !quest.log_seen.empty()) {
        rows.emplace_back(quest_key(modid, quest.id, "log_seen"), quest.log_seen);
    }
    if (quest.is_beat() && !quest.log_beaten.empty()) {
        rows.emplace_back(quest_key(modid, quest.id, "log_beaten"), quest.log_beaten);
    }
    if (quest.is_pray() && !quest.log_prayed.empty()) {
        rows.emplace_back(quest_key(modid, quest.id, "log_prayed"), quest.log_prayed);
    }
    if (quest.is_spot() && !quest.log_left.empty()) {
        rows.emplace_back(quest_key(modid, quest.id, "log_left"), quest.log_left);
    }
    if (quest.is_mind()) {
        for (int i = 0; i < 3 && i < quest.count && !quest.log_check[i].empty(); ++i) {
            rows.emplace_back(quest_key(modid, quest.id, "log_check_" + std::to_string(i + 1)), quest.log_check[i]);
        }
    }
    if ((quest.is_ask() || quest.is_ambush()) && !quest.log_told.empty()) {
        rows.emplace_back(quest_key(modid, quest.id, "log_told"), quest.log_told);
    }
    if ((quest.is_retrieve() || quest.is_buy() || quest.is_deliver()) && quest.has_holder()) {
        rows.emplace_back(quest_key(modid, quest.id, "log_handed"), handed_log(quest));
    }
    if (quest.is_find() && quest.count > 0) {
        rows.emplace_back(quest_key(modid, quest.id, "log_gathered"), gathered_log(quest));
    }
    if (quest.carries_on()) {
        const Quest filled = with_holder_lines(quest);
        rows.emplace_back(quest_key(modid, quest.id, "log_onward"), filled.log_onward);
        rows.emplace_back(quest_key(modid, quest.id, "objective_onward"), filled.objective_onward);
    }
    // The unyielding holder's journal (In the Stocks): the rebuff, and with a helper the
    // naming, the optional objective, the door and the key.
    if (quest.is_unyielding()) {
        const Quest filled = with_holder_lines(quest);
        rows.emplace_back(quest_key(modid, quest.id, "log_rebuffed"), filled.log_rebuffed);
        if (quest.has_helper()) {
            rows.emplace_back(quest_key(modid, quest.id, "log_named"), filled.log_named);
            rows.emplace_back(quest_key(modid, quest.id, "objective_helper"), filled.objective_helper);
            rows.emplace_back(quest_key(modid, quest.id, "log_letin"), filled.log_letin);
            if (quest.has_key()) rows.emplace_back(quest_key(modid, quest.id, "log_keyed"), filled.log_keyed);
        }
    }
    if (quest.is_ambush() && !quest.log_directed.empty()) {
        rows.emplace_back(quest_key(modid, quest.id, "log_directed"), quest.log_directed);
    }
    // The hail's words, for the bark's subtitle (emit_hail_dialogue).
    if (!quest.hail.empty()) {
        rows.emplace_back(quest_key(modid, quest.id, "hail"), quest.hail);
    }
    if (quest.is_ambush() && !quest.log_ambushed.empty()) {
        rows.emplace_back(quest_key(modid, quest.id, "log_ambushed"), quest.log_ambushed);
    }
    if (quest.has_written_log()) {
        rows.emplace_back(quest_key(modid, quest.id, "log_written"), quest.log_written);
    }
    if (quest.has_expose()) {
        rows.emplace_back(quest_key(modid, quest.id, "log_caught"), quest.log_caught);
    }
    if (quest.has_follow()) {
        rows.emplace_back(quest_key(modid, quest.id, "log_followed"), quest.log_followed);
    }
    if (quest.has_bedside()) {
        rows.emplace_back(quest_key(modid, quest.id, "log_denounced"), quest.log_denounced);
    }
    for (const std::string& how : quest.resolutions()) {
        rows.emplace_back(quest_key(modid, quest.id, "log_completed_" + how),
                          *resolution_log(quest, how));
    }
    return rows;
}

namespace {

// The game's spelling of a tier, from the generator's. The shipped range runs from
// RidiculouslyEasy to RidiculouslyHard; the generator deals only the middle three.
std::string skill_tier(const std::string& difficulty) {
    if (difficulty == "easy") return "Easy";
    if (difficulty == "hard") return "Hard";
    return "Medium";
}

// The check behind the reasonable option. persuade is what the generator deals and
// what 177 shipped sequences use; the other speech checks are honoured if a row ever
// names one, and anything else falls back to persuade rather than to a check the
// engine would not know.
std::string speech_check(const std::string& skill) {
    for (const char* known : {"persuade", "impress", "coerce", "dominate", "scholarship"}) {
        if (skill == known) return skill;
    }
    return "persuade";
}

}  // namespace

std::string emit_holder_dialogue(const std::string& modid, const Quest& raw) {
    const Quest quest = with_holder_lines(raw);
    // The other party in this graph is the holder, and target_gender is theirs. A
    // letter's receiver is not, and is spoken with it anyway until a gender is kept
    // for them.
    auto finish = [&](std::string xml) { return voiced_dialogue(std::move(xml), quest.target_gender, quest.target_soul); };
    const std::string node = holder_node(quest.id);
    const std::string role = xml_escape(holder_role(quest.id));
    const std::string hold = xml_escape(dialogue_hold_key(modid, "talk"));
    const std::string tier = skill_tier(quest.difficulty);
    auto key = [&](const char* field) {
        return xml_escape(quest_key(modid, quest.id, std::string("holder_") + field));
    };
    auto t = [](int n) { return std::string(static_cast<std::size_t>(n), '\t'); };
    // One spoken line. Text is the author's copy and StringName is what the player
    // reads; both carry the words, as every shipped file does. The camera is authored
    // per response (README, "The camera is authored, one shot at a time"): without the
    // CameraCommand the view stayed where it was for the holder's first reply (notes.md
    // 5 Oct 2026, Father Nicholas), so each line is framed on whoever says it, as the
    // held conversation's beats are.
    auto line = [&](int depth, const std::string& who, const char* field, const std::string& words) {
        return t(depth) + "<Response Role=\"" + who + "\">\r\n" +
               t(depth + 1) + "<Text StringName=\"" + key(field) + "\" Text=\"" + xml_escape(words) +
               "\" />\r\n" +
               t(depth + 1) + "<Commands><CameraCommand Role=\"" + who + "\" CameraType=\"CloseUp\" /></Commands>\r\n" +
               t(depth) + "</Response>\r\n";
    };
    auto prompt = [&](int depth, const char* field, const std::string& words) {
        return t(depth) + "<UiPrompt StringName=\"" + key(field) + "\" Text=\"" + xml_escape(words) +
               "\" />\r\n";
    };
    // Lua on the exit of a winning branch, which is where the item actually moves. The
    // sequence has ended by then, so the line has been said before the axe appears. The
    // handover port fires from the same branch, in the place the shipped sequences put
    // their Triggers: after the prompt, before the elements.
    auto gave = [&](const char* how) {
        return std::string("Moonshine.QuestHolderGave('") + quest.id + "', '" + how + "')";
    };
    // Lua on the exit of a sequence that moves nothing: a sync point. The door's exit is
    // the menu about to be drawn, a check's exit is the row taken, a lost check's branch
    // is the refusal, the way out is the way out. Each logs `BRIDGE quest stage <id> <what>`
    // (QuestStage), so a runner keys off the log where it used to key off a clock; the
    // branches that do move something log theirs from inside the function they call.
    auto stage = [&](const std::string& what) {
        return "Moonshine.QuestStage('" + quest.id + "', '" + what + "')";
    };
    // An ask with a bribe in it (gift_classes) does not end on the winning line: the holder
    // has pressed the hat and gloves on Henry with the truth, and Henry takes them or does
    // not. So each winning sequence ends in a Decision of two instead of EndDialogue, the
    // open sequence's shape, and its exit script (gave, the handover) has already run by the
    // time the menu is drawn -- the truth is told either way, as it is in the story. The
    // rows' exits are QuestGift: 'took' makes the classes in Henry's pocket, 'refused' is the
    // bucket of guts. Named by the branch they hang off, since a Decision's name is what a
    // GoTo finds it by and the menu has several winning branches.
    const bool bribed = quest.is_ask() && !quest.gift_classes.empty();
    const std::string won_end = bribed ? "Decision" : "EndDialogue";
    auto gift_nest = [&](int depth, const std::string& branch) {
        if (!bribed) return std::string();
        auto took = [&](const char* how) {
            return std::string("Moonshine.QuestGift('") + quest.id + "', '" + how + "')";
        };
        std::string c;
        c += t(depth) + "<Decision Name=\"gift_" + branch + "\" Priority=\"General\">\r\n";
        c += t(depth + 1) + "<Sequences>\r\n";
        c += t(depth + 2) + "<Sequence EndType=\"EndDialogue\" Name=\"gift_" + branch + "_take\" ExitScript=\"" +
             took("took") + "\">\r\n";
        c += prompt(depth + 3, "gift_take", quest.holder_gift_take);
        c += t(depth + 3) + "<Elements>\r\n";
        c += line(depth + 4, "HENRY", "gift_take", quest.holder_gift_take);
        c += line(depth + 4, role, "gift_take_reply", quest.holder_gift_take_reply);
        c += t(depth + 3) + "</Elements>\r\n";
        c += t(depth + 2) + "</Sequence>\r\n";
        c += t(depth + 2) + "<Sequence EndType=\"EndDialogue\" Name=\"gift_" + branch + "_refuse\" ExitScript=\"" +
             took("refused") + "\">\r\n";
        c += prompt(depth + 3, "gift_refuse", quest.holder_gift_refuse);
        c += t(depth + 3) + "<Elements>\r\n";
        c += line(depth + 4, "HENRY", "gift_refuse", quest.holder_gift_refuse);
        c += line(depth + 4, role, "gift_refuse_reply", quest.holder_gift_refuse_reply);
        c += t(depth + 3) + "</Elements>\r\n";
        c += t(depth + 2) + "</Sequence>\r\n";
        c += t(depth + 1) + "</Sequences>\r\n";
        c += t(depth) + "</Decision>\r\n";
        return c;
    };

    std::string s = skald_header(modid);
    s += t(2) + "<FaderDialog Name=\"" + node + "\">\r\n";
    // In: whether the quest is Active and whether the thing has already gone. Out: that
    // it just went. Both Ins have to hold for the dialogue to be offered at all, which
    // is what makes the game's Talk prompt appear on the holder for exactly the stretch
    // of the errand and nothing either side of it.
    s += t(3) + "<Ports>\r\n";
    s += t(4) + "<Port Name=\"active\" Direction=\"In\" Type=\"bool\" />\r\n";
    s += t(4) + "<Port Name=\"handed\" Direction=\"In\" Type=\"bool\" />\r\n";
    s += t(4) + "<Port Name=\"handover\" Direction=\"Out\" Type=\"trigger\" />\r\n";
    const bool seal_word = quest.is_deliver() && !quest.log_opened.empty() && !quest.receiver_hand_opened.empty();
    if (seal_word) s += t(4) + "<Port Name=\"opened\" Direction=\"In\" Type=\"bool\" />\r\n";
    if (quest.is_mind()) {
        for (int i = 1; i <= std::min(3, std::max(1, quest.count)); ++i) {
            s += t(4) + "<Port Name=\"check" + std::to_string(i) + "\" Direction=\"Out\" Type=\"trigger\" />\r\n";
        }
    }
    if (quest.is_ambush()) {
        s += t(4) + "<Port Name=\"directed\" Direction=\"Out\" Type=\"trigger\" />\r\n";
    }
    // An unyielding holder's refusal: the row taken and Henry shown the door (the
    // objective's Updated line, the marker back on the giver).
    if (quest.is_unyielding()) {
        s += t(4) + "<Port Name=\"rebuffed\" Direction=\"Out\" Type=\"trigger\" />\r\n";
    }
    // The bedside's denouncing: Henry walked out with nothing (the objective's Updated line,
    // the marker on the giver, who is to be told).
    if (quest.has_bedside()) {
        s += t(4) + "<Port Name=\"denounced\" Direction=\"Out\" Type=\"trigger\" />\r\n";
    }
    s += t(3) + "</Ports>\r\n";
    s += t(3) + "<Text Text=\"" + xml_escape(quest.name + " (holder)") + "\" />\r\n";
    // A FaderDialog, so it is a conversation with a camera and a menu and not a bark,
    // and player-initiated, so the game puts a prompt on the soul for it. Greetings and
    // farewells off, as the held loop's are: with them on the game said "Farewell, good
    // sir" over the end of the menu in the soul's shipped voice, a different voice from the
    // one that had just been talking (notes.md 14 Oct 2026, Paul), so no bookends on any of
    // our menus (this one, the onward receiver's and the helper's).
    s += t(3) + kMenuDialogue;
    if (quest.has_follow()) {
        // The street written (has_follow): the root decision picks instead of asking. A
        // player-initiated dialogue presents its first decision as a menu of the entry
        // sequences whose conditions hold, with each sequence's prompt as the row -- one
        // row, Henry's opening line, which is the day door, the bedside and the denounced
        // door as the player has always seen them. On the night the holder has caught
        // Henry behind him the first line is the holder's, and the player saw a one-row
        // menu of Henry's answer before the holder had said a word (notes.md 25 Oct 2026).
        // So the root is Autoselected: the street's own sequence when the followed global
        // is up (and the catching not made), and otherwise one bare response and a GoTo to
        // the presented decision below, which holds the doors as before. The game's Talk
        // prompt reads the same conditions off the root, so it is offered exactly as it was.
        const std::string door_when = "Port('active') AND !Port('handed') AND gvar('" +
                                      xml_escape(talking_variable(modid)) + "')!=1";
        const std::string seen_var = xml_escape(quest_flag_variable(modid, quest.id, "seen"));
        const std::string followed_var = xml_escape(quest_flag_variable(modid, quest.id, "followed"));
        s += t(4) + "<Decision Name=\"dec1\" Autoselect=\"true\">\r\n";
        s += t(5) + "<Sequences>\r\n";
        s += t(6) + "<Sequence EntryCondition=\"" + door_when + " AND gvar('" + followed_var +
             "')==1 AND gvar('" + seen_var + "')!=1\" EndType=\"EndDialogue\" Name=\"open_followed\" ExitScript=\"" +
             stage("open followed") + "\">\r\n";
        s += prompt(7, "followed_reply", quest.holder_followed_reply);
        s += t(7) + "<Elements>\r\n";
        s += line(8, role, "followed", quest.holder_followed);
        s += line(8, "HENRY", "followed_reply", quest.holder_followed_reply);
        s += t(7) + "</Elements>\r\n";
        s += t(6) + "</Sequence>\r\n";
        s += t(6) + "<Sequence EntryCondition=\"" + door_when + " AND (gvar('" + followed_var +
             "')!=1 OR gvar('" + seen_var + "')==1)\" EndType=\"GoTo\" GoToDecision=\"doors\" Name=\"to_doors\">\r\n";
        s += prompt(7, "open", quest.holder_open);
        s += t(7) + "<Elements>\r\n";
        s += t(8) + "<Response Role=\"HENRY\" />\r\n";
        s += t(7) + "</Elements>\r\n";
        s += t(6) + "</Sequence>\r\n";
        s += t(6) + "<Sequence EntryCondition=\"FALSE\" EndType=\"Decision\" Name=\"doors__c\">\r\n";
        s += t(7) + "<Decision Name=\"doors\" Priority=\"General\">\r\n";
        s += t(8) + "<Sequences>\r\n";
    } else {
        s += t(4) + "<Decision Name=\"dec1\" Priority=\"General\">\r\n";
        s += t(5) + "<Sequences>\r\n";
    }
    if (quest.is_deliver()) {
        // The receiver of a letter has nothing to be talked out of, so the game's Talk on
        // him is the handover itself: Henry says who it is from, the receiver takes it,
        // the port fires and the exit script moves the letter from Henry's pocket to his.
        // One sequence and no menu -- the shipped menus are a nested Decision under an
        // opening exchange, and several sequences at the top are the quick-chat wheel,
        // not a menu -- so the leave lines the generator writes are not spoken here.
        // Under the same condition as the holder's door: the errand is on, the letter has
        // not gone, and our own held conversation is not up.
        // With a word written for a broken seal, the same exchange twice over: the one
        // offered when the page was opened on the way has the receiver's remark on it
        // between taking the letter and answering it. Same prompt, same port, same exit.
        auto hand_sequence = [&](const char* name, const std::string& when, bool remark) {
            s += t(6) + "<Sequence EntryCondition=\"Port('active') AND !Port('handed')" + when + " AND gvar('" +
                 xml_escape(talking_variable(modid)) + "')!=1\" EndType=\"EndDialogue\" Name=\"" + name + "\" "
                 "ExitScript=\"Moonshine.QuestDelivered('" + quest.id + "')\">\r\n";
            s += prompt(7, "hand", quest.receiver_hand);
            s += t(7) + "<Triggers>\r\n" + t(8) + "<Port Name=\"handover\" />\r\n" + t(7) + "</Triggers>\r\n";
            s += t(7) + "<Elements>\r\n";
            s += line(8, "HENRY", "hand", quest.receiver_hand);
            s += line(8, role, "hand_reply", quest.receiver_hand_reply);
            if (remark) s += line(8, role, "hand_opened", quest.receiver_hand_opened);
            // A receiver who reads it there and answers (a story's abbot laughing at the
            // question) has a third line; most have none and the sequence is the two.
            if (!quest.receiver_hand_read.empty()) {
                s += line(8, role, "hand_read", quest.receiver_hand_read);
            }
            s += t(7) + "</Elements>\r\n";
            s += t(6) + "</Sequence>\r\n";
        };
        // The interrogation (In the Stocks' chalice, notes.md 9 Oct 2026): the door is the
        // same two lines, but the receiver's reply is a question and the sequence ends in a
        // menu rather than the handover. Every row of the menu is an answer that hands the
        // thing over and ends the talk -- the port fires and the exit script moves it, as
        // the plain hand row's does, with the how beside it (QuestDelivered's second
        // argument, kept in the store for the giver's answer and the journal's log). The
        // cover story is a question in its turn, with a nested menu of two; the dodges are
        // the holder menu's own checks (SkillCheckType, the tier), each branch an answer
        // that still hands over, since the thing is already in the receiver's sight. No
        // way out: Henry came to give it back, and he has.
        auto interrogation = [&]() {
            auto delivered = [&](const char* how) {
                return std::string("Moonshine.QuestDelivered('") + quest.id + "', '" + how + "')";
            };
            // One answer: Henry's row, the receiver's reply, the handover and the end.
            auto answer = [&](int depth, const char* tag, const char* henry, const std::string& henry_words,
                              const char* reply, const std::string& reply_words, const char* how) {
                std::string c;
                c += t(depth) + "<Sequence EndType=\"EndDialogue\" Name=\"" + std::string(tag) + "\" ExitScript=\"" +
                     delivered(how) + "\">\r\n";
                c += prompt(depth + 1, henry, henry_words);
                c += t(depth + 1) + "<Triggers>\r\n" + t(depth + 2) + "<Port Name=\"handover\" />\r\n" + t(depth + 1) +
                     "</Triggers>\r\n";
                c += t(depth + 1) + "<Elements>\r\n";
                c += line(depth + 2, "HENRY", henry, henry_words);
                c += line(depth + 2, role, reply, reply_words);
                c += t(depth + 1) + "</Elements>\r\n";
                c += t(depth) + "</Sequence>\r\n";
                return c;
            };
            // A dodge: a check whose two branches both hand over, with their own hows.
            auto dodge = [&](int depth, const char* tag, const std::string& type, const char* henry,
                             const std::string& henry_words, const char* yes, const std::string& yes_words,
                             const char* no, const std::string& no_words) {
                std::string c;
                c += t(depth) + "<Sequence EndType=\"Decision\" SkillCheckType=\"" + type +
                     "\" Type=\"Skillcheck\" SkillCheck=\"" + tier + "\" Name=\"" + tag + "\" ExitScript=\"" +
                     stage(std::string("row ") + tag) + "\">\r\n";
                c += prompt(depth + 1, henry, henry_words);
                c += t(depth + 1) + "<Elements>\r\n";
                c += line(depth + 2, "HENRY", henry, henry_words);
                c += t(depth + 1) + "</Elements>\r\n";
                c += t(depth + 1) + "<Decision Name=\"" + std::string(tag) + "_out\" Priority=\"General\">\r\n";
                c += t(depth + 2) + "<Sequences>\r\n";
                for (const char* branch : {"yes", "no"}) {
                    const bool won = std::string(branch) == "yes";
                    c += t(depth + 3) + "<Sequence EndType=\"EndDialogue\" SkillcheckCondition=\"" +
                         (won ? "Success" : "Fail") + "\" Name=\"" + std::string(tag) + "_" + branch +
                         "\" ExitScript=\"" + delivered(won ? "evaded" : "caught") + "\">\r\n";
                    c += t(depth + 4) + "<UiPrompt StringName=\"" + hold + "\" Text=\"" + xml_escape(kHoldPrompt) +
                         "\" />\r\n";
                    c += t(depth + 4) + "<Triggers>\r\n" + t(depth + 5) + "<Port Name=\"handover\" />\r\n" +
                         t(depth + 4) + "</Triggers>\r\n";
                    c += t(depth + 4) + "<Elements>\r\n";
                    c += line(depth + 5, role, won ? yes : no, won ? yes_words : no_words);
                    c += t(depth + 4) + "</Elements>\r\n";
                    c += t(depth + 3) + "</Sequence>\r\n";
                }
                c += t(depth + 2) + "</Sequences>\r\n";
                c += t(depth + 1) + "</Decision>\r\n";
                c += t(depth) + "</Sequence>\r\n";
                return c;
            };
            s += t(6) + "<Sequence EntryCondition=\"Port('active') AND !Port('handed') AND gvar('" +
                 xml_escape(talking_variable(modid)) + "')!=1\" EndType=\"Decision\" Name=\"hand\" ExitScript=\"" +
                 stage("open") + "\">\r\n";
            s += prompt(7, "hand", quest.receiver_hand);
            s += t(7) + "<Elements>\r\n";
            s += line(8, "HENRY", "hand", quest.receiver_hand);
            s += line(8, role, "hand_reply", quest.receiver_hand_reply);
            s += t(7) + "</Elements>\r\n";
            s += t(7) + "<Decision Name=\"menu\" Priority=\"General\">\r\n";
            s += t(8) + "<Sequences>\r\n";
            s += answer(9, "truth", "tell_truth", quest.tell_truth, "tell_truth_reply", quest.tell_truth_reply, "truth");
            const bool cover = !quest.tell_cover.empty() && !quest.tell_cover_reply.empty() &&
                               !quest.tell_named.empty() && !quest.tell_named_reply.empty() &&
                               !quest.tell_heard.empty() && !quest.tell_heard_reply.empty();
            if (cover) {
                s += t(9) + "<Sequence EndType=\"Decision\" Name=\"cover\" ExitScript=\"" + stage("row cover") + "\">\r\n";
                s += prompt(10, "tell_cover", quest.tell_cover);
                s += t(10) + "<Elements>\r\n";
                s += line(11, "HENRY", "tell_cover", quest.tell_cover);
                s += line(11, role, "tell_cover_reply", quest.tell_cover_reply);
                s += t(10) + "</Elements>\r\n";
                s += t(10) + "<Decision Name=\"cover_out\" Priority=\"General\">\r\n";
                s += t(11) + "<Sequences>\r\n";
                s += answer(12, "named", "tell_named", quest.tell_named, "tell_named_reply", quest.tell_named_reply, "pinned");
                s += answer(12, "heard", "tell_heard", quest.tell_heard, "tell_heard_reply", quest.tell_heard_reply, "covered");
                // The tales (notes.md 14 Oct 2026): a vision, and a story that sounds made
                // up. The receiver sees through both, takes the thing and ends the talk on a
                // word about lying to a priest; one how for the two, since the giver's
                // answer and the journal's line are the same for either.
                if (!quest.tell_vision.empty() && !quest.tell_vision_reply.empty()) {
                    s += answer(12, "vision", "tell_vision", quest.tell_vision, "tell_vision_reply",
                                quest.tell_vision_reply, "fibbed");
                }
                if (!quest.tell_tale.empty() && !quest.tell_tale_reply.empty()) {
                    s += answer(12, "tale", "tell_tale", quest.tell_tale, "tell_tale_reply", quest.tell_tale_reply,
                                "fibbed");
                }
                s += t(11) + "</Sequences>\r\n";
                s += t(10) + "</Decision>\r\n";
                s += t(9) + "</Sequence>\r\n";
            }
            if (!quest.tell_evade.empty() && !quest.tell_evade_yes.empty() && !quest.tell_evade_no.empty()) {
                s += dodge(9, "evade", speech_check(quest.skill), "tell_evade", quest.tell_evade, "tell_evade_yes",
                           quest.tell_evade_yes, "tell_evade_no", quest.tell_evade_no);
            }
            if (!quest.tell_threat.empty() && !quest.tell_threat_yes.empty() && !quest.tell_threat_no.empty() &&
                quest.target_gender != "female") {
                s += dodge(9, "threat", "dread", "tell_threat", quest.tell_threat, "tell_threat_yes",
                           quest.tell_threat_yes, "tell_threat_no", quest.tell_threat_no);
            }
            s += t(8) + "</Sequences>\r\n";
            s += t(7) + "</Decision>\r\n";
            s += t(6) + "</Sequence>\r\n";
        };
        // A dictated letter (npcdb.hpp, dictate; Words Must Not Fail): the receiver takes it
        // and reads it part by part, a remark on each as Henry wrote it, then his verdict.
        // Which remark and which verdict are the dialogue globals the Lua keeps from the
        // store's digits (QuestFlags: p1..p3 the outcomes, verdict 0 prayer / 1 cursed / 2
        // mocked), read by the entry conditions of Autoselected decisions the way the
        // mind's visits read its count. The door is the plain hand sequence, ending on a
        // GoTo into the first reading; each reading is a decision of one sequence per
        // outcome, each a GoTo on to the next, a part left out passing through on one bare
        // response (an empty Elements crashes the engine at the GoTo, as the hold loop's
        // comment records); the verdict's sequences fire the handover port and end the
        // talk through QuestDelivered with the how. The readings live in sequences the
        // root can never offer (EntryCondition="FALSE"), reached by name alone, as the
        // voiced bodies do. The voicing then splits every spoken sequence into its row and
        // body as usual; the conditions stay on the rows, the GoTos on the bodies.
        auto dictated = [&]() {
            auto var = [&](const char* flag) { return xml_escape(quest_flag_variable(modid, quest.id, flag)); };
            auto delivered = [&](const char* how) {
                return std::string("Moonshine.QuestDelivered('") + quest.id + "'" +
                       (how == nullptr ? std::string() : std::string(", '") + how + "'") + ")";
            };
            s += t(6) + "<Sequence EntryCondition=\"Port('active') AND !Port('handed') AND gvar('" +
                 xml_escape(talking_variable(modid)) + "')!=1\" EndType=\"GoTo\" GoToDecision=\"read1\" Name=\"hand\" "
                 "ExitScript=\"" + stage("open") + "\">\r\n";
            s += prompt(7, "hand", quest.receiver_hand);
            s += t(7) + "<Elements>\r\n";
            s += line(8, "HENRY", "hand", quest.receiver_hand);
            s += line(8, role, "hand_reply", quest.receiver_hand_reply);
            s += t(7) + "</Elements>\r\n";
            s += t(6) + "</Sequence>\r\n";
            for (int k = 0; k < Quest::kDictationParts; ++k) {
                const std::string n = std::to_string(k + 1);
                const std::string next = k + 1 < Quest::kDictationParts ? "read" + std::to_string(k + 2) : "verdict";
                s += t(6) + "<Sequence EntryCondition=\"FALSE\" EndType=\"Decision\" Name=\"read" + n + "_c\">\r\n";
                s += t(7) + "<Decision Name=\"read" + n + "\" Autoselect=\"true\">\r\n";
                s += t(8) + "<Sequences>\r\n";
                for (int outcome : quest.dictation_outcomes(k)) {
                    const std::string& words = quest.read_part[k][outcome];
                    const std::string field = "read_" + n + (outcome == 2 ? "_soft" : outcome == 3 ? "_over" : "");
                    s += t(9) + "<Sequence EntryCondition=\"gvar('" + var(("p" + n).c_str()) + "')==" +
                         std::to_string(outcome) + "\" EndType=\"GoTo\" GoToDecision=\"" + next + "\" Name=\"read" + n +
                         "_" + std::to_string(outcome) + "\">\r\n";
                    s += t(10) + "<Elements>\r\n";
                    if (outcome == 1 || words.empty()) {
                        s += t(11) + "<Response Role=\"" + role + "\" />\r\n";
                    } else {
                        s += line(11, role, field.c_str(), words);
                    }
                    s += t(10) + "</Elements>\r\n";
                    s += t(9) + "</Sequence>\r\n";
                }
                s += t(8) + "</Sequences>\r\n";
                s += t(7) + "</Decision>\r\n";
                s += t(6) + "</Sequence>\r\n";
            }
            s += t(6) + "<Sequence EntryCondition=\"FALSE\" EndType=\"Decision\" Name=\"verdict_c\">\r\n";
            s += t(7) + "<Decision Name=\"verdict\" Autoselect=\"true\">\r\n";
            s += t(8) + "<Sequences>\r\n";
            struct Verdict { int code; const char* name; const char* field; const std::string* words; const char* how; };
            const Verdict verdicts[] = {
                {0, "verdict_prayer", "verdict_prayer", &quest.verdict_prayer, nullptr},
                {1, "verdict_cursed", "verdict_cursed", &quest.verdict_cursed, "cursed"},
                {2, "verdict_mocked", "verdict_mocked", &quest.verdict_mocked, "mocked"},
            };
            for (const Verdict& v : verdicts) {
                s += t(9) + "<Sequence EntryCondition=\"gvar('" + var("verdict") + "')==" + std::to_string(v.code) +
                     "\" EndType=\"EndDialogue\" Name=\"" + v.name + "\" ExitScript=\"" + delivered(v.how) + "\">\r\n";
                s += t(10) + "<Triggers>\r\n" + t(11) + "<Port Name=\"handover\" />\r\n" + t(10) + "</Triggers>\r\n";
                s += t(10) + "<Elements>\r\n";
                s += line(11, role, v.field, *v.words);
                s += t(10) + "</Elements>\r\n";
                s += t(9) + "</Sequence>\r\n";
            }
            s += t(8) + "</Sequences>\r\n";
            s += t(7) + "</Decision>\r\n";
            s += t(6) + "</Sequence>\r\n";
        };
        if (quest.has_dictation()) {
            dictated();
        } else if (quest.has_interrogation()) {
            interrogation();
        } else if (seal_word) {
            hand_sequence("hand", " AND !Port('opened')", false);
            hand_sequence("hand_opened", " AND Port('opened')", true);
        } else {
            hand_sequence("hand", "", false);
        }
        s += t(5) + "</Sequences>\r\n";
        s += t(4) + "</Decision>\r\n";
        s += t(3) + "</Dialogue>\r\n";
        s += t(2) + "</FaderDialog>\r\n";
        s += "\t</Skald>\r\n";
        s += "</Database>\r\n";
        return finish(s);
    }
    if (quest.is_mind()) {
        // The one looked in on. No door and no menu: each visit is one sequence, offered
        // while the errand is on, the count is not done (handed) and the visit is due -- the
        // Lua keeps two globals from the store and the game's clock: how many visits are
        // made (checksVar) and whether the next is due (dueVar). Each fires its own Out port,
        // which the quest graph wires to the objective's Updated log for that visit; the last
        // fires handover too, which closes this dialogue for good. Too soon after the last,
        // the early sequence is the one on offer, and it counts for nothing. Each visit's
        // exit script tells the store (QuestChecked), which is what moves the count on.
        const std::string checks = xml_escape(quest_flag_variable(modid, quest.id, "checks"));
        const std::string due = xml_escape(quest_flag_variable(modid, quest.id, "due"));
        const std::string door = "Port('active') AND !Port('handed') AND gvar('" +
                                 xml_escape(talking_variable(modid)) + "')!=1";
        const int visits = std::min(3, std::max(1, quest.count));
        for (int i = 0; i < visits; ++i) {
            const std::string n = std::to_string(i + 1);
            const std::string reply = ("check_reply_" + n);
            s += t(6) + "<Sequence EntryCondition=\"" + door + " AND gvar('" + checks + "')==" +
                 std::to_string(i) + " AND gvar('" + due + "')==1\" EndType=\"EndDialogue\" Name=\"check" + n +
                 "\" ExitScript=\"Moonshine.QuestChecked('" + quest.id + "')\">\r\n";
            s += prompt(7, "check", quest.target_check);
            s += t(7) + "<Triggers>\r\n" + t(8) + "<Port Name=\"check" + n + "\" />\r\n";
            if (i + 1 == visits) s += t(8) + "<Port Name=\"handover\" />\r\n";
            s += t(7) + "</Triggers>\r\n";
            s += t(7) + "<Elements>\r\n";
            s += line(8, "HENRY", "check", quest.target_check);
            s += line(8, role, reply.c_str(), quest.target_check_reply[i]);
            s += t(7) + "</Elements>\r\n";
            s += t(6) + "</Sequence>\r\n";
        }
        s += t(6) + "<Sequence EntryCondition=\"" + door + " AND gvar('" + due + "')!=1\" "
                    "EndType=\"EndDialogue\" Name=\"early\" ExitScript=\"" + stage("early") + "\">\r\n";
        s += prompt(7, "early", quest.target_early);
        s += t(7) + "<Elements>\r\n";
        s += line(8, "HENRY", "early", quest.target_early);
        s += line(8, role, "early_reply", quest.target_early_reply);
        s += t(7) + "</Elements>\r\n";
        s += t(6) + "</Sequence>\r\n";
        s += t(5) + "</Sequences>\r\n";
        s += t(4) + "</Decision>\r\n";
        s += t(3) + "</Dialogue>\r\n";
        s += t(2) + "</FaderDialog>\r\n";
        s += "\t</Skald>\r\n";
        s += "</Database>\r\n";
        return finish(s);
    }
    if (quest.is_ambush()) {
        // The one who knows the way, in three stages, the mind's shape: one sequence each,
        // gated on a global the Lua keeps from the store (0 before, 1 directed, 2 ambushed).
        // The first is the false way: its Out port writes the journal, and its exit script
        // (QuestDirected) makes the directions in Henry's pocket and moves the store on. The
        // second is Henry back too soon, and counts for nothing. The third, after the fight,
        // is the true way: the handover port (the journal's Told, the marker back on the
        // giver), which closes this dialogue for good.
        const std::string stage_var = xml_escape(quest_flag_variable(modid, quest.id, "stage"));
        const std::string door = "Port('active') AND !Port('handed') AND gvar('" +
                                 xml_escape(talking_variable(modid)) + "')!=1";
        s += t(6) + "<Sequence EntryCondition=\"" + door + " AND gvar('" + stage_var + "')==0\" "
                    "EndType=\"EndDialogue\" Name=\"direct\" ExitScript=\"Moonshine.QuestDirected('" + quest.id + "')\">\r\n";
        s += prompt(7, "direct", quest.target_direct);
        s += t(7) + "<Triggers>\r\n" + t(8) + "<Port Name=\"directed\" />\r\n" + t(7) + "</Triggers>\r\n";
        s += t(7) + "<Elements>\r\n";
        s += line(8, "HENRY", "direct", quest.target_direct);
        s += line(8, role, "direct_reply", quest.target_direct_reply);
        s += t(7) + "</Elements>\r\n";
        s += t(6) + "</Sequence>\r\n";
        s += t(6) + "<Sequence EntryCondition=\"" + door + " AND gvar('" + stage_var + "')==1\" "
                    "EndType=\"EndDialogue\" Name=\"early\" ExitScript=\"" + stage("early") + "\">\r\n";
        s += prompt(7, "early", quest.target_early);
        s += t(7) + "<Elements>\r\n";
        s += line(8, "HENRY", "early", quest.target_early);
        s += line(8, role, "early_reply", quest.target_early_reply);
        s += t(7) + "</Elements>\r\n";
        s += t(6) + "</Sequence>\r\n";
        s += t(6) + "<Sequence EntryCondition=\"" + door + " AND gvar('" + stage_var + "')==2\" "
                    "EndType=\"EndDialogue\" Name=\"truth\" ExitScript=\"" + gave("truth") + "\">\r\n";
        s += prompt(7, "truth", quest.target_truth);
        s += t(7) + "<Triggers>\r\n" + t(8) + "<Port Name=\"handover\" />\r\n" + t(7) + "</Triggers>\r\n";
        s += t(7) + "<Elements>\r\n";
        s += line(8, "HENRY", "truth", quest.target_truth);
        s += line(8, role, "truth_reply", quest.target_truth_reply);
        s += t(7) + "</Elements>\r\n";
        s += t(6) + "</Sequence>\r\n";
        s += t(5) + "</Sequences>\r\n";
        s += t(4) + "</Decision>\r\n";
        s += t(3) + "</Dialogue>\r\n";
        s += t(2) + "</FaderDialog>\r\n";
        s += "\t</Skald>\r\n";
        s += "</Database>\r\n";
        return finish(s);
    }
    if (quest.is_buy()) {
        // The one with the price: the door (Henry says who sent him, the holder names the
        // terms) and a menu of two -- the pay row, offered only while Henry has both the
        // giver's thing (a global the Lua keeps from his inventory) and the sum, and the way
        // out. No argument and no threat: the terms are the terms. The pay row's exit script
        // moves the thing and the money to the holder and the charm to Henry (QuestHolderGave,
        // 'pay', which the Lua reads with the buy's extra step).
        const std::string carry = xml_escape(quest_flag_variable(modid, quest.id, "carry"));
        s += t(6) + "<Sequence EntryCondition=\"Port('active') AND !Port('handed') AND gvar('" +
             xml_escape(talking_variable(modid)) + "')!=1\" EndType=\"Decision\" Name=\"open\" ExitScript=\"" +
             stage("open") + "\">\r\n";
        s += prompt(7, "open", quest.holder_open);
        s += t(7) + "<Elements>\r\n";
        s += line(8, "HENRY", "open", quest.holder_open);
        s += line(8, role, "refuse", quest.holder_refuse);
        s += t(7) + "</Elements>\r\n";
        s += t(7) + "<Decision Name=\"menu\" Priority=\"General\">\r\n";
        s += t(8) + "<Sequences>\r\n";
        s += t(9) + "<Sequence EntryCondition=\"gvar('" + carry + "')==1 AND GetActorMoney()[0] &gt;= " +
             std::to_string(quest.pay_groschen) + "\" EndType=\"EndDialogue\" Name=\"pay\" ExitScript=\"" +
             gave("pay") + "\">\r\n";
        s += prompt(10, "pay", quest.holder_pay);
        s += t(10) + "<Triggers>\r\n" + t(11) + "<Port Name=\"handover\" />\r\n" + t(10) + "</Triggers>\r\n";
        s += t(10) + "<Elements>\r\n";
        s += line(11, "HENRY", "pay", quest.holder_pay);
        s += line(11, role, "pay_yes", quest.holder_pay_yes);
        s += t(10) + "</Elements>\r\n";
        s += t(9) + "</Sequence>\r\n";
        s += t(9) + "<Sequence EndType=\"EndDialogue\" Name=\"leave\" ExitScript=\"" + stage("row leave") + "\">\r\n";
        s += prompt(10, "leave", quest.holder_leave);
        s += t(10) + "<Elements>\r\n";
        s += line(11, "HENRY", "leave", quest.holder_leave);
        s += line(11, role, "leave_reply", quest.holder_leave_reply);
        s += t(10) + "</Elements>\r\n";
        s += t(9) + "</Sequence>\r\n";
        s += t(8) + "</Sequences>\r\n";
        s += t(7) + "</Decision>\r\n";
        s += t(6) + "</Sequence>\r\n";
        s += t(5) + "</Sequences>\r\n";
        s += t(4) + "</Decision>\r\n";
        s += t(3) + "</Dialogue>\r\n";
        s += t(2) + "</FaderDialog>\r\n";
        s += "\t</Skald>\r\n";
        s += "</Database>\r\n";
        return finish(s);
    }
    // The one way in. Henry says what he came about and the holder makes an excuse,
    // and then the menu. No ThisSequenceUsed here: the player can come back and try
    // again, and it is the branches below that are spent, not the door.
    //
    // Shut while the held conversation is up. The first live run had the engine offer
    // our Start a conversation on the holder (CanTalk had not yet caught up with a role
    // granted the same frame), and RequestDialog then had two enterable dialogues on the
    // soul and started this one, with the model's words streaming over a menu the app
    // knew nothing about. With the talking global in the condition, this dialogue is
    // never a candidate for a request made on our account; the base menu reads CanTalk
    // with the global down, so the prompt it puts up is unaffected.
    // With the bedside written (has_bedside), this day door is only his until he is caught:
    // from the catching, the Talk opens on the bedside below instead, and after a
    // denouncing on a word of his own.
    const std::string door_when = "Port('active') AND !Port('handed') AND gvar('" +
                                  xml_escape(talking_variable(modid)) + "')!=1";
    const std::string seen_var = xml_escape(quest_flag_variable(modid, quest.id, "seen"));
    const std::string denounced_var = xml_escape(quest_flag_variable(modid, quest.id, "denounced"));
    const std::string followed_var = xml_escape(quest_flag_variable(modid, quest.id, "followed"));
    // With the street written (has_follow), the day door is shut for the night he has
    // caught Henry behind him: his Talk opens on his own line instead (open_followed).
    s += t(6) + "<Sequence EntryCondition=\"" + door_when +
         (quest.has_bedside() ? " AND gvar('" + seen_var + "')!=1" : "") +
         (quest.has_follow() ? " AND gvar('" + followed_var + "')!=1" : "") +
         "\" EndType=\"Decision\" Name=\"open\" ExitScript=\"" + stage("open") + "\">\r\n";
    s += prompt(7, "open", quest.holder_open);
    s += t(7) + "<Elements>\r\n";
    s += line(8, "HENRY", "open", quest.holder_open);
    s += line(8, role, "refuse", quest.holder_refuse);
    s += t(7) + "</Elements>\r\n";
    s += t(7) + "<Decision Name=\"menu\" Priority=\"General\">\r\n";
    s += t(8) + "<Sequences>\r\n";

    // A row that wins the thing without a check, offered while a global the Lua keeps
    // says the condition for it holds: Henry says his piece, the holder gives in, the
    // port fires and the dialogue ends. First on the menu when it is there at all,
    // since it is the row the errand was leading to.
    auto plain = [&](const char* tag, const std::string& condition, const char* henry,
                     const std::string& henry_words, const char* yes, const std::string& yes_words) {
        std::string c;
        c += t(9) + "<Sequence EntryCondition=\"" + condition + "\" EndType=\"" + won_end + "\" Name=\"" +
             std::string(tag) + "\" ExitScript=\"" + gave(tag) + "\">\r\n";
        c += prompt(10, henry, henry_words);
        c += t(10) + "<Triggers>\r\n" + t(11) + "<Port Name=\"handover\" />\r\n" + t(10) +
             "</Triggers>\r\n";
        c += t(10) + "<Elements>\r\n";
        c += line(11, "HENRY", henry, henry_words);
        c += line(11, role, yes, yes_words);
        c += t(10) + "</Elements>\r\n";
        c += gift_nest(10, tag);
        c += t(9) + "</Sequence>\r\n";
        return c;
    };
    // A nest: the holder's own errand (requires_quest_id) is done, which the Lua marks
    // in the quest's inner global from the store, and he keeps his word without a check.
    if (!quest.requires_quest_id.empty()) {
        s += plain("nested", "gvar('" + xml_escape(quest_flag_variable(modid, quest.id, "inner")) + "')==1",
                   "nested", quest.holder_nested, "nested_yes", quest.holder_nested_yes);
    }
    // A dispute, the second time round: Henry offered to carry the grievance (the broker
    // global), and now says the giver owned to it. Whether the giver did -- the app's
    // apologise directive -- or Henry is lying is the Lua's to know at the handover; the
    // holder believes him either way, which is the bible's lie recorded as a completion.
    if (quest.dispute) {
        s += plain("brokered", "gvar('" + xml_escape(quest_flag_variable(modid, quest.id, "broker")) + "')==1",
                   "brokered", quest.holder_brokered, "brokered_yes", quest.holder_brokered_yes);
    }
    // An exposure (Words Must Not Fail): Henry has caught the holder where he should not be
    // (the store's seen, written to the quest's seen global by QuestFlags), and says so; the
    // holder gives the thing up for his silence, and the errand ends `exposed`.
    if (quest.has_expose() && !quest.has_bedside()) {
        s += plain("expose", "gvar('" + seen_var + "')==1", "expose", quest.holder_expose, "expose_yes",
                   quest.holder_expose_yes);
    }

    // A check is a sequence with SkillCheckType and a tier, whose nested decision holds
    // one branch for each outcome; the engine weighs the player's stats against the tier
    // when the option is picked and takes the branch whose SkillcheckCondition matches.
    // Each is offered once (!ThisSequenceUsed), which is the shipped convention and what
    // stops a failed check being picked again until it passes. Henry's line is said in
    // the check sequence itself, the holder's answer in the branch.
    auto check = [&](const char* tag, const std::string& type, const char* henry,
                     const std::string& henry_words, const char* yes, const std::string& yes_words,
                     const char* no, const std::string& no_words) {
        std::string c;
        c += t(9) + "<Sequence EntryCondition=\"!ThisSequenceUsed()\" EndType=\"Decision\" "
                    "SkillCheckType=\"" + type + "\" Type=\"Skillcheck\" SkillCheck=\"" + tier +
             "\" Name=\"" + tag + "\" ExitScript=\"" + stage(std::string("row ") + tag) + "\">\r\n";
        c += prompt(10, henry, henry_words);
        c += t(10) + "<Elements>\r\n";
        c += line(11, "HENRY", henry, henry_words);
        c += t(10) + "</Elements>\r\n";
        c += t(10) + "<Decision Name=\"" + std::string(tag) + "_out\" Priority=\"General\">\r\n";
        c += t(11) + "<Sequences>\r\n";
        // Won: the holder gives in, the port fires, the Lua moves the thing, and the
        // dialogue ends -- there is nothing left to talk about.
        c += t(12) + "<Sequence EndType=\"" + won_end + "\" SkillcheckCondition=\"Success\" Name=\"" +
             std::string(tag) + "_yes\" ExitScript=\"" + gave(tag) + "\">\r\n";
        c += t(13) + "<UiPrompt StringName=\"" + hold + "\" Text=\"" + xml_escape(kHoldPrompt) +
             "\" />\r\n";
        c += t(13) + "<Triggers>\r\n" + t(14) + "<Port Name=\"handover\" />\r\n" + t(13) +
             "</Triggers>\r\n";
        c += t(13) + "<Elements>\r\n";
        c += line(14, role, yes, yes_words);
        c += t(13) + "</Elements>\r\n";
        c += gift_nest(13, tag);
        c += t(12) + "</Sequence>\r\n";
        // Lost: the holder holds out and the menu comes back with this option gone.
        c += t(12) + "<Sequence EndType=\"GoTo\" GoToDecision=\"menu\" SkillcheckCondition=\"Fail\" "
                     "Name=\"" + std::string(tag) + "_no\" ExitScript=\"" +
             stage(std::string("refused ") + tag) + "\">\r\n";
        c += t(13) + "<UiPrompt StringName=\"" + hold + "\" Text=\"" + xml_escape(kHoldPrompt) +
             "\" />\r\n";
        c += t(13) + "<Elements>\r\n";
        c += line(14, role, no, no_words);
        c += t(13) + "</Elements>\r\n";
        c += t(12) + "</Sequence>\r\n";
        c += t(11) + "</Sequences>\r\n";
        c += t(10) + "</Decision>\r\n";
        c += t(9) + "</Sequence>\r\n";
        return c;
    };
    // An unyielding holder (In the Stocks' sacristan, notes.md 13 Oct 2026): the same two
    // rows with no check behind them and the refusal the only answer, however high Henry's
    // Speech or his menace. Each ends the talk -- he is shown the door -- fires the menu's
    // `rebuffed` port and the Lua's QuestRebuffed (the store's `rebuffed`, which the app
    // reads to have the giver send Henry at the chest anyway), and is offered every time:
    // a man who takes offence takes it again. No pay row: coin is the worst insult of all.
    auto rebuff = [&](const char* tag, const char* henry, const std::string& henry_words, const char* no,
                      const std::string& no_words) {
        std::string c;
        c += t(9) + "<Sequence EndType=\"EndDialogue\" Name=\"" + std::string(tag) + "\" ExitScript=\"Moonshine.QuestRebuffed('" +
             quest.id + "', '" + tag + "')\">\r\n";
        c += prompt(10, henry, henry_words);
        c += t(10) + "<Triggers>\r\n" + t(11) + "<Port Name=\"rebuffed\" />\r\n" + t(10) + "</Triggers>\r\n";
        c += t(10) + "<Elements>\r\n";
        c += line(11, "HENRY", henry, henry_words);
        c += line(11, role, no, no_words);
        c += t(10) + "</Elements>\r\n";
        c += t(9) + "</Sequence>\r\n";
        return c;
    };
    if (quest.is_unyielding()) {
        s += rebuff("persuade", "persuade", quest.holder_persuade, "persuade_no", quest.holder_persuade_no);
        if (quest.target_gender != "female") {
            s += rebuff("threaten", "threaten", quest.holder_threaten, "threaten_no", quest.holder_threaten_no);
        }
    } else {
        s += check("persuade", speech_check(quest.skill), "persuade", quest.holder_persuade,
                   "persuade_yes", quest.holder_persuade_yes, "persuade_no", quest.holder_persuade_no);
        // Nobody frightens a woman for a kettle: a female holder gets no dread option, so
        // Henry talks, pays, brokers or steals (data/kcd2/quest-design/survey-followups.md §1).
        if (quest.target_gender != "female") {
            s += check("threaten", "dread", "threaten", quest.holder_threaten, "threaten_yes",
                       quest.holder_threaten_yes, "threaten_no", quest.holder_threaten_no);
        }
    }

    // Money is not a check: it works if Henry has it, which is the way the shipped
    // teachers and horse traders sell -- GetActorMoney()[0] in the entry condition
    // against a figure in groschen, and the transfer on exit. Once, like the checks, and
    // only offered to a player who can afford it, so the menu never shows an option that
    // would fail for want of coin. Left off altogether when the errand has no price
    // (pay_groschen 0): the giver is paying money, and a man who pays Henry to fetch a
    // thing would sooner have paid the holder himself. And off for an unyielding holder.
    if (quest.pay_groschen > 0 && !quest.is_unyielding()) {
        s += t(9) + "<Sequence EntryCondition=\"!ThisSequenceUsed() AND GetActorMoney()[0] &gt;= " +
             std::to_string(quest.pay_groschen) + "\" EndType=\"" + won_end + "\" Name=\"pay\" ExitScript=\"" +
             gave("pay") + "\">\r\n";
        s += prompt(10, "pay", quest.holder_pay);
        s += t(10) + "<Triggers>\r\n" + t(11) + "<Port Name=\"handover\" />\r\n" + t(10) +
             "</Triggers>\r\n";
        s += t(10) + "<Elements>\r\n";
        s += line(11, "HENRY", "pay", quest.holder_pay);
        s += line(11, role, "pay_yes", quest.holder_pay_yes);
        s += t(10) + "</Elements>\r\n";
        s += gift_nest(10, "pay");
        s += t(9) + "</Sequence>\r\n";
    }

    // A dispute's fifth line: Henry offers to carry the grievance to the giver. Nothing
    // changes hands; the exit script raises the broker flag in the store (and the global
    // the brokered row above reads), the giver's conversation then has the apology to
    // put to him, and the dialogue ends so Henry can go and do it. Once: the offer made
    // stands, and the row is gone while the flag is up either way.
    if (quest.dispute) {
        s += t(9) + "<Sequence EntryCondition=\"!ThisSequenceUsed() AND gvar('" +
             xml_escape(quest_flag_variable(modid, quest.id, "broker")) +
             "')!=1\" EndType=\"EndDialogue\" Name=\"broker\" ExitScript=\"Moonshine.QuestBroker('" +
             quest.id + "')\">\r\n";
        s += prompt(10, "broker", quest.holder_broker);
        s += t(10) + "<Elements>\r\n";
        s += line(11, "HENRY", "broker", quest.holder_broker);
        s += line(11, role, "broker_reply", quest.holder_broker_reply);
        s += t(10) + "</Elements>\r\n";
        s += t(9) + "</Sequence>\r\n";
    }

    // And the way out, always there, so a player who has spent every check and has no
    // money is never held in a menu with nothing to pick.
    s += t(9) + "<Sequence EndType=\"EndDialogue\" Name=\"leave\" ExitScript=\"" + stage("row leave") + "\">\r\n";
    s += prompt(10, "leave", quest.holder_leave);
    s += t(10) + "<Elements>\r\n";
    s += line(11, "HENRY", "leave", quest.holder_leave);
    s += line(11, role, "leave_reply", quest.holder_leave_reply);
    s += t(10) + "</Elements>\r\n";
    s += t(9) + "</Sequence>\r\n";

    s += t(8) + "</Sequences>\r\n";
    s += t(7) + "</Decision>\r\n";
    s += t(6) + "</Sequence>\r\n";

    // The bedside (npcdb.hpp has_bedside; notes.md 22 Oct 2026). From the catching, the
    // holder's Talk opens on him and not on Henry: Henry's waking line has been said over
    // the bed already, in his own voice, by the Lua (WatchExpose, `wake`), and the Lua asks
    // the engine for this Talk in the same breath; whether that or Henry's own press of
    // Talk opens it, the holder speaks first and asks what it will take (`wake_reply`).
    // Three rows and the way out: the thing for silence (expose, as above, the plain
    // `exposed`); the thing and coin besides on a Speech check at the hardest tier whatever
    // the errand's (blackmail; won, `exposed_paid`, QuestHolderGave pays blackmail_groschen
    // out of nothing, as a reward is paid; lost, the thing alone, `exposed` -- a man caught
    // in another's bed does not win the argument, he only keeps his purse); and nothing at
    // all, the parish to hear of it (denounce: the `denounced` port writes the journal,
    // QuestDenounced the store, and the errand stays open for the giver to be told). After
    // a denouncing he has one sullen line for Henry (open_denounced) and no menu.
    if (quest.has_bedside()) {
        const std::string bedside_when = door_when + " AND gvar('" + seen_var + "')==1 AND gvar('" + denounced_var + "')!=1";
        s += t(6) + "<Sequence EntryCondition=\"" + bedside_when + "\" EndType=\"Decision\" Name=\"open_caught\" ExitScript=\"" +
             stage("open caught") + "\">\r\n";
        s += prompt(7, "wake", quest.holder_wake);
        s += t(7) + "<Elements>\r\n";
        s += line(8, role, "wake_reply", quest.holder_wake_reply);
        s += t(7) + "</Elements>\r\n";
        s += t(7) + "<Decision Name=\"menu_caught\" Priority=\"General\">\r\n";
        s += t(8) + "<Sequences>\r\n";
        s += plain("expose", "gvar('" + seen_var + "')==1", "expose", quest.holder_expose, "expose_yes",
                   quest.holder_expose_yes);
        // The blackmail: a check whose two branches both hand over, with their own hows
        // (the interrogation's dodge shape).
        s += t(9) + "<Sequence EntryCondition=\"!ThisSequenceUsed()\" EndType=\"Decision\" SkillCheckType=\"coerce\" "
                    "Type=\"Skillcheck\" SkillCheck=\"Hard\" Name=\"blackmail\" ExitScript=\"" + stage("row blackmail") + "\">\r\n";
        s += prompt(10, "blackmail", quest.holder_blackmail);
        s += t(10) + "<Elements>\r\n";
        s += line(11, "HENRY", "blackmail", quest.holder_blackmail);
        s += t(10) + "</Elements>\r\n";
        s += t(10) + "<Decision Name=\"blackmail_out\" Priority=\"General\">\r\n";
        s += t(11) + "<Sequences>\r\n";
        for (const char* branch : {"yes", "no"}) {
            const bool won = std::string(branch) == "yes";
            s += t(12) + "<Sequence EndType=\"EndDialogue\" SkillcheckCondition=\"" + (won ? "Success" : "Fail") +
                 "\" Name=\"blackmail_" + branch + "\" ExitScript=\"" + gave(won ? "blackmail" : "blackmail_no") + "\">\r\n";
            s += t(13) + "<UiPrompt StringName=\"" + hold + "\" Text=\"" + xml_escape(kHoldPrompt) + "\" />\r\n";
            s += t(13) + "<Triggers>\r\n" + t(14) + "<Port Name=\"handover\" />\r\n" + t(13) + "</Triggers>\r\n";
            s += t(13) + "<Elements>\r\n";
            s += line(14, role, won ? "blackmail_yes" : "blackmail_no", won ? quest.holder_blackmail_yes : quest.holder_blackmail_no);
            s += t(13) + "</Elements>\r\n";
            s += t(12) + "</Sequence>\r\n";
        }
        s += t(11) + "</Sequences>\r\n";
        s += t(10) + "</Decision>\r\n";
        s += t(9) + "</Sequence>\r\n";
        s += t(9) + "<Sequence EndType=\"EndDialogue\" Name=\"denounce\" ExitScript=\"Moonshine.QuestDenounced('" + quest.id +
             "')\">\r\n";
        s += prompt(10, "denounce", quest.holder_denounce);
        s += t(10) + "<Triggers>\r\n" + t(11) + "<Port Name=\"denounced\" />\r\n" + t(10) + "</Triggers>\r\n";
        s += t(10) + "<Elements>\r\n";
        s += line(11, "HENRY", "denounce", quest.holder_denounce);
        s += line(11, role, "denounce_reply", quest.holder_denounce_reply);
        s += t(10) + "</Elements>\r\n";
        s += t(9) + "</Sequence>\r\n";
        s += t(9) + "<Sequence EndType=\"EndDialogue\" Name=\"leave_caught\" ExitScript=\"" + stage("row leave") + "\">\r\n";
        s += prompt(10, "leave", quest.holder_leave);
        s += t(10) + "<Elements>\r\n";
        s += line(11, "HENRY", "leave", quest.holder_leave);
        s += line(11, role, "leave_reply", quest.holder_leave_reply);
        s += t(10) + "</Elements>\r\n";
        s += t(9) + "</Sequence>\r\n";
        s += t(8) + "</Sequences>\r\n";
        s += t(7) + "</Decision>\r\n";
        s += t(6) + "</Sequence>\r\n";
        s += t(6) + "<Sequence EntryCondition=\"" + door_when + " AND gvar('" + denounced_var +
             "')==1\" EndType=\"EndDialogue\" Name=\"open_denounced\" ExitScript=\"" + stage("open denounced") + "\">\r\n";
        s += prompt(7, "open_denounced", quest.holder_open_denounced);
        s += t(7) + "<Elements>\r\n";
        s += line(8, role, "open_denounced", quest.holder_open_denounced);
        s += t(7) + "</Elements>\r\n";
        s += t(6) + "</Sequence>\r\n";
    }
    // The street (npcdb.hpp has_follow; notes.md 23 Oct 2026, n8): the night he has caught
    // Henry behind him on his way to the witness, his Talk opens on him -- the Lua asks the
    // engine for it as he turns (WatchFollow, the bedside's Attempt shape), or Henry presses
    // Talk -- with his one line and Henry's one answer, and ends. That sequence is the
    // root's, above (open_followed, Autoselected so his line comes first); the followed
    // global is up from the Lua's verdict until the morning (QuestFlags), and down again
    // for the next night's day door. Not after the catching, when the bedside is his Talk.
    if (quest.has_follow()) {
        s += t(8) + "</Sequences>\r\n";
        s += t(7) + "</Decision>\r\n";
        s += t(6) + "</Sequence>\r\n";
    }
    s += t(5) + "</Sequences>\r\n";
    s += t(4) + "</Decision>\r\n";
    s += t(3) + "</Dialogue>\r\n";
    s += t(2) + "</FaderDialog>\r\n";
    s += "\t</Skald>\r\n";
    s += "</Database>\r\n";
    return finish(s);
}

std::vector<std::pair<std::string, std::string>> holder_localization(const std::string& modid,
                                                                     const Quest& raw) {
    const Quest quest = with_holder_lines(raw);
    auto key = [&](const char* field) {
        return quest_key(modid, quest.id, std::string("holder_") + field);
    };
    if (quest.is_deliver()) {
        std::vector<std::pair<std::string, std::string>> rows = {
            {key("hand"), quest.receiver_hand}, {key("hand_reply"), quest.receiver_hand_reply}};
        if (!quest.receiver_hand_read.empty()) {
            rows.emplace_back(key("hand_read"), quest.receiver_hand_read);
        }
        if (!quest.log_opened.empty() && !quest.receiver_hand_opened.empty()) {
            rows.emplace_back(key("hand_opened"), quest.receiver_hand_opened);
        }
        if (quest.has_interrogation()) {
            const std::pair<const char*, const std::string*> tells[] = {
                {"tell_truth", &quest.tell_truth}, {"tell_truth_reply", &quest.tell_truth_reply},
                {"tell_cover", &quest.tell_cover}, {"tell_cover_reply", &quest.tell_cover_reply},
                {"tell_named", &quest.tell_named}, {"tell_named_reply", &quest.tell_named_reply},
                {"tell_heard", &quest.tell_heard}, {"tell_heard_reply", &quest.tell_heard_reply},
                {"tell_vision", &quest.tell_vision}, {"tell_vision_reply", &quest.tell_vision_reply},
                {"tell_tale", &quest.tell_tale}, {"tell_tale_reply", &quest.tell_tale_reply},
                {"tell_evade", &quest.tell_evade}, {"tell_evade_yes", &quest.tell_evade_yes},
                {"tell_evade_no", &quest.tell_evade_no}, {"tell_threat", &quest.tell_threat},
                {"tell_threat_yes", &quest.tell_threat_yes}, {"tell_threat_no", &quest.tell_threat_no},
            };
            for (const auto& [field, words] : tells) {
                if (!words->empty()) rows.emplace_back(key(field), *words);
            }
        }
        // A dictated letter's reading: a remark per part per outcome, and the verdicts.
        if (quest.has_dictation()) {
            for (int k = 0; k < Quest::kDictationParts; ++k) {
                const std::string n = std::to_string(k + 1);
                for (int outcome : {0, 2, 3}) {
                    const std::string& words = quest.read_part[k][outcome];
                    if (words.empty()) continue;
                    const std::string field = "read_" + n + (outcome == 2 ? "_soft" : outcome == 3 ? "_over" : "");
                    rows.emplace_back(key(field.c_str()), words);
                }
            }
            rows.emplace_back(key("verdict_prayer"), quest.verdict_prayer);
            rows.emplace_back(key("verdict_cursed"), quest.verdict_cursed);
            rows.emplace_back(key("verdict_mocked"), quest.verdict_mocked);
        }
        return rows;
    }
    if (quest.is_mind()) {
        std::vector<std::pair<std::string, std::string>> rows = {
            {key("check"), quest.target_check},
            {key("early"), quest.target_early},
            {key("early_reply"), quest.target_early_reply},
        };
        for (int i = 0; i < std::min(3, std::max(1, quest.count)); ++i) {
            rows.emplace_back(key(("check_reply_" + std::to_string(i + 1)).c_str()), quest.target_check_reply[i]);
        }
        return rows;
    }
    if (quest.is_ambush()) {
        return {
            {key("direct"), quest.target_direct},
            {key("direct_reply"), quest.target_direct_reply},
            {key("early"), quest.target_early},
            {key("early_reply"), quest.target_early_reply},
            {key("truth"), quest.target_truth},
            {key("truth_reply"), quest.target_truth_reply},
        };
    }
    if (quest.is_buy()) {
        return {
            {key("open"), quest.holder_open},
            {key("refuse"), quest.holder_refuse},
            {key("pay"), quest.holder_pay},
            {key("pay_yes"), quest.holder_pay_yes},
            {key("leave"), quest.holder_leave},
            {key("leave_reply"), quest.holder_leave_reply},
        };
    }
    if (quest.is_beat() || quest.is_ask()) {
        std::vector<std::pair<std::string, std::string>> rows = {
            {key("open"), quest.holder_open},
            {key("refuse"), quest.holder_refuse},
            {key("persuade"), quest.holder_persuade},
            {key("persuade_yes"), quest.holder_persuade_yes},
            {key("persuade_no"), quest.holder_persuade_no},
            {key("threaten"), quest.holder_threaten},
            {key("threaten_yes"), quest.holder_threaten_yes},
            {key("threaten_no"), quest.holder_threaten_no},
            {key("pay"), quest.holder_pay},
            {key("pay_yes"), quest.holder_pay_yes},
            {key("leave"), quest.holder_leave},
            {key("leave_reply"), quest.holder_leave_reply},
        };
        if (quest.is_ask() && !quest.gift_classes.empty()) {
            rows.emplace_back(key("gift_take"), quest.holder_gift_take);
            rows.emplace_back(key("gift_take_reply"), quest.holder_gift_take_reply);
            rows.emplace_back(key("gift_refuse"), quest.holder_gift_refuse);
            rows.emplace_back(key("gift_refuse_reply"), quest.holder_gift_refuse_reply);
        }
        return rows;
    }
    std::vector<std::pair<std::string, std::string>> rows = {
        {key("open"), quest.holder_open},
        {key("refuse"), quest.holder_refuse},
        {key("persuade"), quest.holder_persuade},
        {key("persuade_yes"), quest.holder_persuade_yes},
        {key("persuade_no"), quest.holder_persuade_no},
        {key("threaten"), quest.holder_threaten},
        {key("threaten_yes"), quest.holder_threaten_yes},
        {key("threaten_no"), quest.holder_threaten_no},
        {key("pay"), quest.holder_pay},
        {key("pay_yes"), quest.holder_pay_yes},
        {key("leave"), quest.holder_leave},
        {key("leave_reply"), quest.holder_leave_reply},
    };
    if (quest.dispute) {
        rows.emplace_back(key("broker"), quest.holder_broker);
        rows.emplace_back(key("broker_reply"), quest.holder_broker_reply);
        rows.emplace_back(key("brokered"), quest.holder_brokered);
        rows.emplace_back(key("brokered_yes"), quest.holder_brokered_yes);
    }
    if (!quest.requires_quest_id.empty()) {
        rows.emplace_back(key("nested"), quest.holder_nested);
        rows.emplace_back(key("nested_yes"), quest.holder_nested_yes);
    }
    if (quest.has_expose()) {
        rows.emplace_back(key("expose"), quest.holder_expose);
        rows.emplace_back(key("expose_yes"), quest.holder_expose_yes);
    }
    if (quest.has_follow()) {
        rows.emplace_back(key("followed"), quest.holder_followed);
        rows.emplace_back(key("followed_reply"), quest.holder_followed_reply);
    }
    if (quest.has_bedside()) {
        rows.emplace_back(key("wake"), quest.holder_wake);
        rows.emplace_back(key("wake_reply"), quest.holder_wake_reply);
        rows.emplace_back(key("denounce"), quest.holder_denounce);
        rows.emplace_back(key("denounce_reply"), quest.holder_denounce_reply);
        rows.emplace_back(key("blackmail"), quest.holder_blackmail);
        rows.emplace_back(key("blackmail_yes"), quest.holder_blackmail_yes);
        rows.emplace_back(key("blackmail_no"), quest.holder_blackmail_no);
        rows.emplace_back(key("open_denounced"), quest.holder_open_denounced);
    }
    return rows;
}

std::string onward_metarole(const std::string& quest_id) { return holder_metarole(quest_id) + "_ONWARD"; }
std::string onward_role(const std::string& quest_id) { return onward_metarole(quest_id) + "_TOLD"; }
std::string onward_node(const std::string& quest_id) { return quest_node(quest_id) + "_onward"; }

std::string emit_onward_dialogue(const std::string& modid, const Quest& raw) {
    // The one an ask's word is carried on to has nothing to be talked out of, so, as a
    // letter's receiver, the game's Talk on them is the saying itself: Henry says who sent
    // him and what the matter is, they answer, and say what they make of it; the port fires
    // and the exit script completes the quest (QuestCarried: the plain complete, nothing
    // paid again -- the giver paid at the telling). One sequence and no menu, under the
    // holder door's condition: the last leg is open, the word not yet said, and our own
    // held conversation not up.
    if (!raw.carries_on()) {
        throw std::runtime_error("quest " + raw.id + " does not carry its word on");
    }
    const Quest quest = with_holder_lines(raw);
    const std::string node = onward_node(quest.id);
    const std::string role = xml_escape(onward_role(quest.id));
    auto key = [&](const char* field) {
        return xml_escape(quest_key(modid, quest.id, std::string("onward_") + field));
    };
    auto t = [](int n) { return std::string(static_cast<std::size_t>(n), '\t'); };
    // Framed on the speaker, as the holder's lines are (the camera is authored per response).
    auto line = [&](int depth, const std::string& who, const char* field, const std::string& words) {
        return t(depth) + "<Response Role=\"" + who + "\">\r\n" +
               t(depth + 1) + "<Text StringName=\"" + key(field) + "\" Text=\"" + xml_escape(words) +
               "\" />\r\n" +
               t(depth + 1) + "<Commands><CameraCommand Role=\"" + who + "\" CameraType=\"CloseUp\" /></Commands>\r\n" +
               t(depth) + "</Response>\r\n";
    };
    std::string s = skald_header(modid);
    s += t(2) + "<FaderDialog Name=\"" + node + "\">\r\n";
    s += t(3) + "<Ports>\r\n";
    s += t(4) + "<Port Name=\"active\" Direction=\"In\" Type=\"bool\" />\r\n";
    s += t(4) + "<Port Name=\"handed\" Direction=\"In\" Type=\"bool\" />\r\n";
    s += t(4) + "<Port Name=\"handover\" Direction=\"Out\" Type=\"trigger\" />\r\n";
    s += t(3) + "</Ports>\r\n";
    s += t(3) + "<Text Text=\"" + xml_escape(quest.name + " (onward)") + "\" />\r\n";
    s += t(3) + kMenuDialogue;
    s += t(4) + "<Decision Name=\"dec1\" Priority=\"General\">\r\n";
    s += t(5) + "<Sequences>\r\n";
    s += t(6) + "<Sequence EntryCondition=\"Port('active') AND !Port('handed') AND gvar('" +
         xml_escape(talking_variable(modid)) + "')!=1\" EndType=\"EndDialogue\" Name=\"hand\" "
         "ExitScript=\"Moonshine.QuestCarried('" + quest.id + "')\">\r\n";
    s += t(7) + "<UiPrompt StringName=\"" + key("hand") + "\" Text=\"" + xml_escape(quest.onward_hand) +
         "\" />\r\n";
    s += t(7) + "<Triggers>\r\n" + t(8) + "<Port Name=\"handover\" />\r\n" + t(7) + "</Triggers>\r\n";
    s += t(7) + "<Elements>\r\n";
    s += line(8, "HENRY", "hand", quest.onward_hand);
    s += line(8, role, "hand_reply", quest.onward_hand_reply);
    if (!quest.onward_hand_read.empty()) {
        s += line(8, role, "hand_read", quest.onward_hand_read);
    }
    s += t(7) + "</Elements>\r\n";
    s += t(6) + "</Sequence>\r\n";
    s += t(5) + "</Sequences>\r\n";
    s += t(4) + "</Decision>\r\n";
    s += t(3) + "</Dialogue>\r\n";
    s += t(2) + "</FaderDialog>\r\n";
    s += "\t</Skald>\r\n";
    s += "</Database>\r\n";
    // onward_soul's gender is not on the quest. The holder's is the nearest one kept.
    return voiced_dialogue(s, quest.target_gender, quest.onward_soul);
}

std::vector<std::pair<std::string, std::string>> onward_localization(const std::string& modid,
                                                                     const Quest& raw) {
    if (!raw.carries_on()) return {};
    const Quest quest = with_holder_lines(raw);
    auto key = [&](const char* field) {
        return quest_key(modid, quest.id, std::string("onward_") + field);
    };
    std::vector<std::pair<std::string, std::string>> rows = {
        {key("hand"), quest.onward_hand}, {key("hand_reply"), quest.onward_hand_reply}};
    if (!quest.onward_hand_read.empty()) {
        rows.emplace_back(key("hand_read"), quest.onward_hand_read);
    }
    return rows;
}

std::string emit_helper_dialogue(const std::string& modid, const Quest& raw) {
    // The one who can let Henry in (In the Stocks' Elsbeth, notes.md 13 Oct 2026): the
    // holder menu's shape -- a door and a menu -- under a role of her own, offered while
    // her optional objective is open (Port active) and our held conversation is not up.
    // The door row is money and no check (the shipped teachers' GetActorMoney condition),
    // offered until taken (Port letin, a graph bool off its own Out port, so a save keeps
    // it gone); its exit script takes the groschen (QuestHelper 'door') and the port opens
    // the holder's house. The key row is a Speech check at helper_skill and more money,
    // offered until won (Port keyed) and once a visit (!ThisSequenceUsed, the holder
    // menu's convention, so a lost check is not picked again until Henry comes back): won,
    // the port fires and the exit makes the key in his pocket (QuestHelper 'key'); lost,
    // the menu comes back without it. Then the way out.
    if (!raw.has_helper()) {
        throw std::runtime_error("quest " + raw.id + " has no helper");
    }
    const Quest quest = with_holder_lines(raw);
    const std::string node = helper_node(quest.id);
    const std::string role = xml_escape(helper_role(quest.id));
    const std::string hold = xml_escape(dialogue_hold_key(modid, "talk"));
    const std::string tier = skill_tier(quest.helper_skill.empty() ? "hard" : quest.helper_skill);
    auto key = [&](const char* field) {
        return xml_escape(quest_key(modid, quest.id, std::string("helper_") + field));
    };
    auto t = [](int n) { return std::string(static_cast<std::size_t>(n), '\t'); };
    auto line = [&](int depth, const std::string& who, const char* field, const std::string& words) {
        return t(depth) + "<Response Role=\"" + who + "\">\r\n" +
               t(depth + 1) + "<Text StringName=\"" + key(field) + "\" Text=\"" + xml_escape(words) +
               "\" />\r\n" +
               t(depth + 1) + "<Commands><CameraCommand Role=\"" + who + "\" CameraType=\"CloseUp\" /></Commands>\r\n" +
               t(depth) + "</Response>\r\n";
    };
    auto prompt = [&](int depth, const char* field, const std::string& words) {
        return t(depth) + "<UiPrompt StringName=\"" + key(field) + "\" Text=\"" + xml_escape(words) +
               "\" />\r\n";
    };
    auto helped = [&](const char* what) {
        return std::string("Moonshine.QuestHelper('") + quest.id + "', '" + what + "')";
    };
    auto stage = [&](const std::string& what) {
        return "Moonshine.QuestStage('" + quest.id + "', 'helper " + what + "')";
    };
    std::string s = skald_header(modid);
    s += t(2) + "<FaderDialog Name=\"" + node + "\">\r\n";
    s += t(3) + "<Ports>\r\n";
    s += t(4) + "<Port Name=\"active\" Direction=\"In\" Type=\"bool\" />\r\n";
    s += t(4) + "<Port Name=\"letin\" Direction=\"In\" Type=\"bool\" />\r\n";
    if (quest.has_key()) s += t(4) + "<Port Name=\"keyed\" Direction=\"In\" Type=\"bool\" />\r\n";
    s += t(4) + "<Port Name=\"door\" Direction=\"Out\" Type=\"trigger\" />\r\n";
    if (quest.has_key()) s += t(4) + "<Port Name=\"key\" Direction=\"Out\" Type=\"trigger\" />\r\n";
    // Fired by every row that ends the talk, paid or not: Henry has spoken to her, which is
    // what her objective asks (notes.md 14 Oct 2026: it stayed open after the door was
    // bought, since it closed only with the quest).
    s += t(4) + "<Port Name=\"spoken\" Direction=\"Out\" Type=\"trigger\" />\r\n";
    s += t(3) + "</Ports>\r\n";
    s += t(3) + "<Text Text=\"" + xml_escape(quest.name + " (helper)") + "\" />\r\n";
    s += t(3) + kMenuDialogue;
    s += t(4) + "<Decision Name=\"dec1\" Priority=\"General\">\r\n";
    s += t(5) + "<Sequences>\r\n";
    s += t(6) + "<Sequence EntryCondition=\"Port('active') AND gvar('" + xml_escape(talking_variable(modid)) +
         "')!=1\" EndType=\"Decision\" Name=\"open\" ExitScript=\"" + stage("open") + "\">\r\n";
    s += prompt(7, "open", quest.helper_open);
    s += t(7) + "<Elements>\r\n";
    s += line(8, "HENRY", "open", quest.helper_open);
    s += line(8, role, "open_reply", quest.helper_open_reply);
    s += t(7) + "</Elements>\r\n";
    s += t(7) + "<Decision Name=\"menu\" Priority=\"General\">\r\n";
    s += t(8) + "<Sequences>\r\n";
    // The door: money, once.
    s += t(9) + "<Sequence EntryCondition=\"!Port('letin') AND GetActorMoney()[0] &gt;= " +
         std::to_string(quest.helper_door_price) + "\" EndType=\"EndDialogue\" Name=\"door\" ExitScript=\"" +
         helped("door") + "\">\r\n";
    s += prompt(10, "door", quest.helper_door);
    s += t(10) + "<Triggers>\r\n" + t(11) + "<Port Name=\"door\" />\r\n" + t(11) + "<Port Name=\"spoken\" />\r\n" +
         t(10) + "</Triggers>\r\n";
    s += t(10) + "<Elements>\r\n";
    s += line(11, "HENRY", "door", quest.helper_door);
    s += line(11, role, "door_yes", quest.helper_door_yes);
    s += t(10) + "</Elements>\r\n";
    s += t(9) + "</Sequence>\r\n";
    // The key: a check and more money, once a visit until won.
    if (quest.has_key()) {
        s += t(9) + "<Sequence EntryCondition=\"!Port('keyed') AND !ThisSequenceUsed() AND GetActorMoney()[0] &gt;= " +
             std::to_string(quest.helper_key_price) + "\" EndType=\"Decision\" SkillCheckType=\"" +
             speech_check(quest.skill) + "\" Type=\"Skillcheck\" SkillCheck=\"" + tier + "\" Name=\"key\" ExitScript=\"" +
             stage("row key") + "\">\r\n";
        s += prompt(10, "key", quest.helper_key);
        s += t(10) + "<Elements>\r\n";
        s += line(11, "HENRY", "key", quest.helper_key);
        s += t(10) + "</Elements>\r\n";
        s += t(10) + "<Decision Name=\"key_out\" Priority=\"General\">\r\n";
        s += t(11) + "<Sequences>\r\n";
        s += t(12) + "<Sequence EndType=\"EndDialogue\" SkillcheckCondition=\"Success\" Name=\"key_yes\" ExitScript=\"" +
             helped("key") + "\">\r\n";
        s += t(13) + "<UiPrompt StringName=\"" + hold + "\" Text=\"" + xml_escape(kHoldPrompt) + "\" />\r\n";
        s += t(13) + "<Triggers>\r\n" + t(14) + "<Port Name=\"key\" />\r\n" + t(14) + "<Port Name=\"spoken\" />\r\n" +
             t(13) + "</Triggers>\r\n";
        s += t(13) + "<Elements>\r\n";
        s += line(14, role, "key_yes", quest.helper_key_yes);
        s += t(13) + "</Elements>\r\n";
        s += t(12) + "</Sequence>\r\n";
        s += t(12) + "<Sequence EndType=\"GoTo\" GoToDecision=\"menu\" SkillcheckCondition=\"Fail\" Name=\"key_no\" ExitScript=\"" +
             stage("refused key") + "\">\r\n";
        s += t(13) + "<UiPrompt StringName=\"" + hold + "\" Text=\"" + xml_escape(kHoldPrompt) + "\" />\r\n";
        s += t(13) + "<Elements>\r\n";
        s += line(14, role, "key_no", quest.helper_key_no);
        s += t(13) + "</Elements>\r\n";
        s += t(12) + "</Sequence>\r\n";
        s += t(11) + "</Sequences>\r\n";
        s += t(10) + "</Decision>\r\n";
        s += t(9) + "</Sequence>\r\n";
    }
    s += t(9) + "<Sequence EndType=\"EndDialogue\" Name=\"leave\" ExitScript=\"" + stage("row leave") + "\">\r\n";
    s += prompt(10, "leave", quest.helper_leave);
    s += t(10) + "<Triggers>\r\n" + t(11) + "<Port Name=\"spoken\" />\r\n" + t(10) + "</Triggers>\r\n";
    s += t(10) + "<Elements>\r\n";
    s += line(11, "HENRY", "leave", quest.helper_leave);
    s += line(11, role, "leave_reply", quest.helper_leave_reply);
    s += t(10) + "</Elements>\r\n";
    s += t(9) + "</Sequence>\r\n";
    s += t(8) + "</Sequences>\r\n";
    s += t(7) + "</Decision>\r\n";
    s += t(6) + "</Sequence>\r\n";
    s += t(5) + "</Sequences>\r\n";
    s += t(4) + "</Decision>\r\n";
    s += t(3) + "</Dialogue>\r\n";
    s += t(2) + "</FaderDialog>\r\n";
    s += "\t</Skald>\r\n";
    s += "</Database>\r\n";
    return voiced_dialogue(s, quest.helper_gender.empty() ? "female" : quest.helper_gender, quest.helper_soul);
}

std::vector<std::pair<std::string, std::string>> helper_localization(const std::string& modid,
                                                                     const Quest& raw) {
    if (!raw.has_helper()) return {};
    const Quest quest = with_holder_lines(raw);
    auto key = [&](const char* field) {
        return quest_key(modid, quest.id, std::string("helper_") + field);
    };
    std::vector<std::pair<std::string, std::string>> rows = {
        {key("open"), quest.helper_open},         {key("open_reply"), quest.helper_open_reply},
        {key("door"), quest.helper_door},         {key("door_yes"), quest.helper_door_yes},
        {key("leave"), quest.helper_leave},       {key("leave_reply"), quest.helper_leave_reply},
    };
    if (quest.has_key()) {
        rows.emplace_back(key("key"), quest.helper_key);
        rows.emplace_back(key("key_yes"), quest.helper_key_yes);
        rows.emplace_back(key("key_no"), quest.helper_key_no);
    }
    return rows;
}

std::string hail_alias(const std::string& modid, const std::string& quest_id) {
    return quest_key(modid, quest_id, "hail");
}

std::string emit_hail_dialogue(const std::string& modid, const Quest& quest) {
    // The giver's shout as the game's own bark (emit.hpp, hail_class): one `ingame monolog`
    // dialogue with one line, the shape ambient-design/plan.md section 1.1 played on the Windows laptop
    // (Included and Autoselect on the decision, or nothing is chosen; Hearing 35, the
    // shipped ambient value; GesturesNotNeeded, since the body stays the routine's;
    // ClashPriority Crime, so it is not shouldered aside by a passer-by's remark). The
    // role is the dice-game one every ordinary soul of the save holds -- KOSTKAR_MUZ or
    // KOSTKAR_ZENA by the giver's gender -- so no role of the mod's own has to reach him.
    // The text row is the hail's own (quest_localization, `hail`), so the words on the
    // screen are the words the watch voices. No audio file is shipped under its name: the
    // app's voice says the line (WatchHail), and the engine shows the subtitle for the
    // words' length.
    if (quest.hail.empty()) {
        throw std::runtime_error("quest " + quest.id + " has no hail");
    }
    const std::string node = hail_node(quest.id);
    const std::string role = quest.giver_gender == "female" ? "KOSTKAR_ZENA" : "KOSTKAR_MUZ";
    auto t = [](int n) { return std::string(static_cast<std::size_t>(n), '\t'); };
    std::string s = skald_header(modid);
    s += t(2) + "<Dialog Name=\"" + node + "\">\r\n";
    s += t(3) + "<Text Text=\"" + xml_escape(quest.name + " (hail)") + "\" />\r\n";
    s += t(3) + "<Dialogue Type=\"ingame monolog\" TechnicalStatus=\"Enabled\" Hearing=\"35\" Initiator=\"NonPlayer\" "
                "ClashPriority=\"Crime\" GesturesNotNeeded=\"true\">\r\n";
    s += t(4) + "<Decision Name=\"dec1\" Priority=\"General\" Included=\"true\" Autoselect=\"true\" Alias=\"" +
         xml_escape(hail_alias(modid, quest.id)) + "\">\r\n";
    s += t(5) + "<Sequences>\r\n";
    s += t(6) + "<Sequence EndType=\"EndDialogue\" Name=\"hail\">\r\n";
    s += t(7) + "<Elements>\r\n";
    // ReferenceLength is the line's length in seconds, which the shipped responses carry
    // from their recordings; ours is the app's speaking rate over the words
    // (kCaptionSpeakRate), so the engine has a length for the line it has no clip of.
    const double seconds = std::max(1.5, static_cast<double>(quest.hail.size()) / kCaptionSpeakRate);
    char length[32];
    std::snprintf(length, sizeof length, "%.2f", seconds);
    s += t(8) + "<Response Role=\"" + role + "\" ReferenceLength=\"" + length + "\">\r\n";
    s += t(9) + "<Text StringName=\"" + xml_escape(quest_key(modid, quest.id, "hail")) + "\" Text=\"" +
         xml_escape(quest.hail) + "\" />\r\n";
    s += t(8) + "</Response>\r\n";
    s += t(7) + "</Elements>\r\n";
    s += t(6) + "</Sequence>\r\n";
    s += t(5) + "</Sequences>\r\n";
    s += t(4) + "</Decision>\r\n";
    s += t(3) + "</Dialogue>\r\n";
    s += t(2) + "</Dialog>\r\n";
    s += "\t</Skald>\r\n";
    s += "</Database>\r\n";
    return s;
}

// The mod's own item rows, by the class guid the database names them by. The generator
// derives the guid (uuid5 of "moonshine:item:<name>") and writes it into npc_slate and
// npc_quest as though it were a shipped class; this table is what makes it one. Model
// and icon are borrowed from shipped rows -- the saw's model is the carpenter's prop
// (an NPCTool with no name or icon, which is why it needs a row of its own to be carried)
// and its icon the rusty axe's, since the atlas has no saw. Type 5 / SubType 0 and the
// coefficients are the shipped misc tools' (rutina_rustyAxe).
struct CustomItem {
    const char* guid;
    const char* name;
    const char* ui_name;
    const char* ui_info;  // the line under the name in the inventory
    const char* model;
    const char* icon;
    const char* weight;
    const char* price;
};

const CustomItem kCustomItems[] = {
    {"fef6cd95-2256-534e-8ef1-747924e2581b", "moonshine_saw", "Cross-cut saw",
     "A carpenter's saw with a wooden frame, the teeth kept sharp. A man who hires out by the day "
     "is turned away without one.",
     "manmade/task_specific_props/wood_industry/carpentry/saw.cgf", "rutina_rustyAxe", "2", "42"},
    // The story favours' things (scripts/kcd2_stories.py STORY_ITEMS): the lost purse and
    // the kettle exist in the tables only as other quests' items, so each gets a row of its
    // own, with a shipped model and icon.
    {"4bd373af-31e7-5def-a343-04017ace78b4", "moonshine_purse", "Purse",
     "A drawstring purse of worn leather, with coin in it. Whose it is depends on who is asked.",
     "manmade/common_furniture/sacks/sack_wearable.cgf", "ztratyANalezy_purse", "0.3", "20"},
    {"f1c7428b-7de4-5067-8d07-e42320397005", "moonshine_copper_groschen", "Copper groschen",
     "Four groschen struck in copper and washed with silver, from no mint the king knows of.",
     "manmade/common_furniture/sacks/sack_items/sack_items.cgf", "taborCertovka_exoticItem_romanCoins", "0.1", "1"},
    {"f8485116-2fb8-5498-b5c3-1f0d97c0fbd8", "moonshine_ledger", "Ledger",
     "A book of accounts in a small, careful hand, the sums not matching the shares.",
     "characters/assets/book/book_01.cdf", "lorebook_simple_03", "0.8", "15"},
    {"e03d6419-5130-515b-bf9d-977ae3a27e00", "moonshine_fables_book", "Book of fables",
     "A little book of fables, much thumbed, the fox and the wolf and the honest miller among them.",
     "characters/assets/book/book_01.cdf", "lorebook_simple_03", "0.6", "25"},
    {"ac5e3fab-7909-5f0f-b4d1-33c60b325d48", "moonshine_sealed_letter", "Sealed letter",
     "A letter under a family's seal, the wax unbroken. Whoever holds it holds the family.",
     "characters/assets/parchment_folded/parchment_folded.cdf", "letter_fancy", "0.1", "5"},
    {"eb1037c6-c59b-51f5-b814-ad88d199e4a8", "moonshine_hood", "Good hood",
     "A woman's good hood, folded round whatever else she owned.",
     "manmade/task_specific_props/clothing_industry/tailoring/cloth_folded_a.cgf", "budovaniLazni_hood", "0.4", "12"},
    {"480c8833-5e17-5cb3-a33e-721eeb12e402", "moonshine_kettle", "Copper kettle",
     "A copper kettle, the bottom blackened, the handle mended once. Promised with a dowry.",
     "manmade/task_specific_props/household/cooking_eating/jugs/jug_pewter.cgf", "loot_pewterJug", "2", "30"},
    // The set-aside stories' things (quest-design/set-aside-stories.md): a leave's jug and
    // a return's grave goods, made in Henry's pocket at accept and put down at the place.
    {"7202ff02-6d46-5aea-87c1-ad23e9e98148", "moonshine_jug_of_milk", "Jug of milk",
     "A stoneware jug of this morning's milk, a cloth tied over the mouth. For somebody who is not a person.",
     "manmade/task_specific_props/household/cooking_eating/jugs/jug_g_milk.cgf", "milk", "1.5", "2"},
    {"090599a7-2d8c-53ca-a469-379744fb98d4", "moonshine_grave_goods", "Grave goods",
     "A ring, a gilt pin and a handful of old coin, wrapped in a rag that smells of the ground they were taken from.",
     "manmade/common_furniture/sacks/sack_items/sack_items.cgf", "damaVNesnazich_goldenRing", "0.2", "60"},
    {"07ba89c1-c328-52b5-8f55-e87680fc3401", "moonshine_rules_book", "Book of rules",
     "The Quaestiunculae: a little book of questions for parish priests, what is permitted and what is "
     "not, in a small Latin hand. Laughter is not in it.",
     "characters/assets/book/book_01.cdf", "lorebook_simple_03", "0.6", "25"},
    // A buy's carried thing (set-aside-stories.md §5): the townsman's own hood, which the
    // magician wants for the sympathy of it. uuid5 of "moonshine:item:townsman_hood".
    {"28a82f49-af59-5e76-82c2-6cae124194ca", "moonshine_townsman_hood", "Townsman's hood",
     "A man's hood of good brown wool, worn at the edge where it has rubbed his neck for years. "
     "It smells of him.",
     "manmade/task_specific_props/clothing_industry/tailoring/cloth_folded_a.cgf", "budovaniLazni_hood", "0.4", "15"},
    // The 3 Oct stories' things (STORY_ITEMS): the wildmen's horns and tow (*Bal Des
    // Ardents*) and the abbey's plan of the pipes (*Where does the water come from?*).
    // uuid5 of "moonshine:item:ram_horns", "flax_and_straw", "pipe_plan".
    {"65184b80-b2a8-58b9-b726-447de5a8ac1d", "moonshine_ram_horns", "Pair of ram's horns",
     "Two ram's horns sawn from the skull, curled and ridged, still smelling of the sheep. For a mask.",
     "manmade/task_specific_props/alchemy/special/horn", "horn", "1.2", "10"},
    {"44d0efd4-007c-5ff8-9fca-71684cc8f31d", "moonshine_flax_and_straw", "Bundle of flax and straw",
     "An armful of hackled flax and clean straw tied with a cord: the hair of a wildman, before the pitch.",
     "manmade/common_furniture/sacks/sack_wearable.cgf", "loot_cloth", "1.5", "4"},
    {"9c8eb033-9391-5770-a0ed-d217d60530cb", "moonshine_pipe_plan", "Plan of the pipes",
     "The abbey's plan of the wooden pipes from the Bylany springs to the town, drawn on vellum, the "
     "leaks of twenty years marked in another hand.",
     "characters/assets/parchment_folded/parchment_folded.cdf", "letter_fancy", "0.1", "20"},
    // *Widow's View*'s keepsake (notes.md 5 Oct 2026): laid in the grass among the marigolds
    // by the Lua, the objective's marker on it (keepsake_class). The tables have no carved
    // heart; the carved duck is the one small whittled thing the game ships, and the name
    // says what it is. uuid5 of "moonshine:item:wooden_heart".
    {"874897b3-25c3-5a55-8857-aa400e4399b6", "moonshine_wooden_heart", "Carved wooden heart",
     "A small heart whittled from limewood, smooth with handling, a hole bored for a cord that has "
     "rotted away. Somebody's summer's work, lost in the grass.",
     "manmade/task_specific_props/entertainment/art/carved_figure_duck.cgf", "loot_dogToy_duck", "0.1", "2"},
};

namespace {

// A shipped item whose price a story wants otherwise: the row as item.xml has it, with the
// price changed. The loader's patch replaces a row whose Id it already knows (the log counts
// them: "lines added: 0, modified: 1"), so this is the way to change one attribute of the
// game's own thing without a second item for it. In the Stocks' chalice (notes.md 14 Oct
// 2026): 500 groschen, where the tables' loot_silverChalice is 65. The table's Price is in
// tenths of a groschen (the shipped Radzig's sword is Price 55877 and the inventory shows
// 5587.7; the chalice's own 650 shows 65), so the figure here is ten times the sum wanted:
// a Price of 500 showed as 50 (notes.md 14 Oct 2026, the second note).
struct RepricedItem {
    const char* guid;
    const char* row;  // the whole element, Id and all
};

const RepricedItem kRepricedItems[] = {
    {"6dc80a04-dad0-4259-a854-e085caa74cc1",
     "<MiscItem Type=\"5\" SubType=\"2\" IconId=\"loot_silverChalice\" UIInfo=\"ui_in_loot_silverChalice\" "
     "UIName=\"ui_nm_loot_silverChalice\" DisplayInShop=\"false\" "
     "Model=\"manmade/task_specific_props/religious/chalice_silver_a.cgf\" Weight=\"0.6\" Price=\"5000\" "
     "FadeCoef=\"1\" VisibilityCoef=\"0\" Id=\"6dc80a04-dad0-4259-a854-e085caa74cc1\" Name=\"loot_silverChalice\" />"},
};

std::vector<const RepricedItem*> repriced_items_used(const std::vector<Quest>& quests) {
    std::vector<const RepricedItem*> out;
    for (const RepricedItem& item : kRepricedItems) {
        for (const Quest& q : quests) {
            if (q.item_class == item.guid || q.carry_class == item.guid || q.keepsake_class == item.guid) {
                out.push_back(&item);
                break;
            }
        }
    }
    return out;
}

std::vector<const CustomItem*> custom_items_used(const std::vector<Quest>& quests) {
    std::vector<const CustomItem*> out;
    for (const CustomItem& item : kCustomItems) {
        for (const Quest& q : quests) {
            if (q.item_class == item.guid || q.carry_class == item.guid || q.keepsake_class == item.guid) {
                out.push_back(&item);
                break;
            }
        }
    }
    return out;
}

}  // namespace

std::string item_ui_key(const std::string& modid, const std::string& name) {
    return "ui_nm_" + modid + "_" + name;
}

std::string item_info_key(const std::string& modid, const std::string& name) {
    return "ui_in_" + modid + "_" + name;
}

std::string letter_name(const Quest& quest) { return "letter_" + quest_node(quest.id); }

std::string letter_page_key(const std::string& modid, const Quest& quest) {
    return modid + "_" + letter_name(quest) + "_page";
}

std::string letter_class_prefix() { return "6d6f6f6e-7368-4c00-8"; }

std::string letter_class_suffix(const Quest& quest) {
    require_identifier(quest.id, "quest id");
    std::string tail = quest.id;
    while (tail.size() < 12) tail = "0" + tail;
    return "-" + tail.substr(tail.size() - 12);
}

std::string letter_class(const Quest& quest, const std::string& wrote) {
    // A dictated letter's Document class for one combination of outcomes: the token's
    // shape under 4c (the letter L), with the three outcome digits in the fourth group
    // after the 8 -- "8023" is a hex group, since the digits are 0..3 -- so the Lua can
    // spell the class from the store's digits without a table (letterPrefix .. wrote ..
    // letterSuffix), and collides with nothing shipped or of ours.
    if (wrote.size() != static_cast<std::size_t>(Quest::kDictationParts)) {
        throw std::runtime_error("quest " + quest.id + " letter variant '" + wrote + "' is not " +
                                 std::to_string(Quest::kDictationParts) + " digits");
    }
    return letter_class_prefix() + wrote + letter_class_suffix(quest);
}

std::string letter_variant_name(const Quest& quest, const std::string& wrote) {
    return letter_name(quest) + "_" + wrote;
}

std::string letter_variant_page_key(const std::string& modid, const Quest& quest, const std::string& wrote) {
    return modid + "_" + letter_variant_name(quest, wrote) + "_page";
}

namespace {

// The shipped letters' page markup: one <paragraph> per paragraph, the text's newlines
// being the breaks (the whole is XML-escaped on the way into the table, as
// text_ui_items.xml carries it).
std::string page_markup(const std::string& text) {
    std::string out;
    std::string paragraph;
    auto flush = [&]() {
        std::size_t a = paragraph.find_first_not_of(" \t\r");
        if (a == std::string::npos) { paragraph.clear(); return; }
        std::size_t b = paragraph.find_last_not_of(" \t\r");
        if (!out.empty()) out += "\n";
        out += "<paragraph>" + paragraph.substr(a, b - a + 1) + "</paragraph>";
        paragraph.clear();
    };
    for (char c : text) {
        if (c == '\n') flush();
        else paragraph += c;
    }
    flush();
    return out;
}

}  // namespace

std::string letter_page(const Quest& quest) {
    return page_markup(with_holder_lines(quest).letter);
}

std::string emit_item_table(const std::string& modid, const std::vector<Quest>& quests) {
    const std::vector<const CustomItem*> used = custom_items_used(quests);
    const std::vector<const RepricedItem*> repriced = repriced_items_used(quests);
    std::vector<const Quest*> letters, tokens, keys, papers, hails, follows;
    for (const Quest& q : quests) {
        if (q.has_document()) letters.push_back(&q);
        if (q.has_token()) tokens.push_back(&q);
        if (q.has_chest()) keys.push_back(&q);
        if (q.has_paper()) papers.push_back(&q);
        if (!q.hail.empty()) hails.push_back(&q);
        if (q.has_follow()) follows.push_back(&q);
    }
    if (used.empty() && repriced.empty() && letters.empty() && tokens.empty() && keys.empty() && hails.empty() &&
        follows.empty()) {
        return {};
    }
    // The shape of item__aux.xml and the other shipped fragments: the same root as
    // item.xml, and the engine merges the rows in ("Table 'item' is patched by ...").
    std::string s;
    s += "<?xml version=\"1.0\" encoding=\"us-ascii\"?>\r\n";
    s += "<database xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" name=\"barbora\" "
         "xsi:noNamespaceSchemaLocation=\"item.xsd\">\r\n";
    s += "\t<ItemClasses version=\"8\">\r\n";
    for (const CustomItem* item : used) {
        s += "\t\t<MiscItem Type=\"5\" SubType=\"0\" IconId=\"" + xml_escape(item->icon) +
             "\" UIInfo=\"" + xml_escape(item_info_key(modid, item->name)) + "\" UIName=\"" +
             xml_escape(item_ui_key(modid, item->name)) + "\" PickpocketInPouch=\"true\" Model=\"" +
             xml_escape(item->model) + "\" Weight=\"" + item->weight + "\" Price=\"" + item->price +
             "\" FadeCoef=\"3.174603\" VisibilityCoef=\"1\" Id=\"" + xml_escape(item->guid) +
             "\" Name=\"" + xml_escape(std::string(modid) + "_" + item->name) + "\" />\r\n";
    }
    for (const RepricedItem* item : repriced) s += "\t\t" + std::string(item->row) + "\r\n";
    // One Document per letter, in the shape of the shipped ones (letter_huntsmanRenes,
    // zikmunduv_dopis): the folded-parchment model and the plain letter icon, Book as the
    // script so it opens as a page, and DocumentContent naming the localisation row that
    // is the page itself. Not IsQuestItem: the Lua's inventory:CreateItem makes nothing of
    // a class so marked (measured on the Windows laptop -- the same row with and without the attribute,
    // count 0 -> 0 and 0 -> 1; the shipped quest letters that carry it are put in the
    // inventory by the quest graph, not by script), so the letter is an ordinary document
    // that can be sold or dropped, and the writer's keep row is what a lost letter costs.
    // The Id is the generator's letter_class guid, from npc_quest.item_class. An ambush's
    // directions and a buy's charm are the same Document under another page: a scrap of
    // directions takes the shipped map icon (the hand-drawn ways the game's own quests
    // give out carry map_book), a charm the page-of-rhyme one (pocestny_bookPageRhyme).
    // A dictated letter (npcdb.hpp, dictate) is one Document per combination of what Henry
    // wrote, under letter_class, each with its own page; the generator's class is not used,
    // since no letter of that text is ever made.
    auto document = [&](const Quest& q, const char* icon, const std::string& name, const std::string& id,
                        const std::string& page_key) {
        s += "\t\t<Document Type=\"5\" IconId=\"" + std::string(icon) + "\" UIInfo=\"" +
             xml_escape(item_info_key(modid, name)) + "\" UIName=\"" +
             xml_escape(item_ui_key(modid, name)) + "\" PickpocketInPouch=\"true\" "
             "Model=\"characters/assets/parchment_folded/parchment_folded.cdf\" EntityScript=\"Book\" "
             "Weight=\"0\" Price=\"5\" FadeCoef=\"1.333333\" VisibilityCoef=\"1\" Id=\"" +
             xml_escape(id) + "\" Name=\"" + xml_escape(modid + "_" + name) + "\">\r\n";
        s += "\t\t\t<DocumentContent Parts=\"" + xml_escape(page_key) + "\" />\r\n";
        s += "\t\t</Document>\r\n";
        (void)q;
    };
    for (const Quest* q : letters) {
        const char* icon = q->is_ambush() ? "map_book" : q->is_buy() ? "pocestny_bookPageRhyme" : "letter_simple";
        if (q->has_dictation()) {
            for (const std::string& wrote : q->dictation_variants()) {
                document(*q, icon, letter_variant_name(*q, wrote), letter_class(*q, wrote),
                         letter_variant_page_key(modid, *q, wrote));
            }
            continue;
        }
        document(*q, icon, letter_name(*q), q->item_class, letter_page_key(modid, *q));
    }
    // One token per pray: a weightless, priceless misc item in the saw's shape, made in the
    // player's pocket by the Lua when Henry has knelt and deleted a moment later, so it is
    // never seen; the graph's ItemDescriptorTrigger is what it is for.
    for (const Quest* q : tokens) {
        const std::string name = token_name(*q);
        s += "\t\t<MiscItem Type=\"5\" SubType=\"0\" IconId=\"rutina_rustyAxe\" UIInfo=\"" +
             xml_escape(item_info_key(modid, name)) + "\" UIName=\"" + xml_escape(item_ui_key(modid, name)) +
             "\" PickpocketInPouch=\"true\" Model=\"manmade/task_specific_props/wood_industry/carpentry/saw.cgf\" "
             "Weight=\"0\" Price=\"0\" FadeCoef=\"3.174603\" VisibilityCoef=\"1\" Id=\"" +
             xml_escape(token_class(*q)) + "\" Name=\"" + xml_escape(modid + "_" + name) + "\" />\r\n";
    }
    // One key per chest: the shipped chest keys' row (poi_vuzVJezirku_key: Type 4, the
    // general key icon and model), under the quest's own key class, which the Lua's Stash
    // is keyed to (behaviors.md §6.3). Made in Henry's pocket by the helper's key row.
    for (const Quest* q : keys) {
        const std::string name = key_name(*q);
        s += "\t\t<MiscItem Type=\"4\" IconId=\"key_general\" UIInfo=\"" + xml_escape(item_info_key(modid, name)) +
             "\" UIName=\"" + xml_escape(item_ui_key(modid, name)) +
             "\" PickpocketInPouch=\"true\" Model=\"manmade/common_tools/keys_lockpicks/key_a\" "
             "Weight=\"0\" Price=\"0\" FadeCoef=\"1\" VisibilityCoef=\"1\" Id=\"" + xml_escape(key_class(*q)) +
             "\" Name=\"" + xml_escape(modid + "_" + name) + "\" />\r\n";
        // And the chest's taken token, the pray token's row under its own class: made in
        // Henry's pocket by the watch when the thing has left the chest, and gone a moment
        // later.
        const std::string taken = taken_name(*q);
        s += "\t\t<MiscItem Type=\"5\" SubType=\"0\" IconId=\"rutina_rustyAxe\" UIInfo=\"" +
             xml_escape(item_info_key(modid, taken)) + "\" UIName=\"" + xml_escape(item_ui_key(modid, taken)) +
             "\" PickpocketInPouch=\"true\" Model=\"manmade/task_specific_props/wood_industry/carpentry/saw.cgf\" "
             "Weight=\"0\" Price=\"0\" FadeCoef=\"3.174603\" VisibilityCoef=\"1\" Id=\"" +
             xml_escape(taken_class(*q)) + "\" Name=\"" + xml_escape(modid + "_" + taken) + "\" />\r\n";
    }
    // A hail's token: the pray token's row under the hail's class, made in Henry's pocket
    // by the watch as the giver shouts and gone a moment later (emit.hpp, hail_class).
    for (const Quest* q : hails) {
        const std::string name = hail_name(*q);
        s += "\t\t<MiscItem Type=\"5\" SubType=\"0\" IconId=\"rutina_rustyAxe\" UIInfo=\"" +
             xml_escape(item_info_key(modid, name)) + "\" UIName=\"" + xml_escape(item_ui_key(modid, name)) +
             "\" PickpocketInPouch=\"true\" Model=\"manmade/task_specific_props/wood_industry/carpentry/saw.cgf\" "
             "Weight=\"0\" Price=\"0\" FadeCoef=\"3.174603\" VisibilityCoef=\"1\" Id=\"" +
             xml_escape(hail_class(*q)) + "\" Name=\"" + xml_escape(modid + "_" + name) + "\" />\r\n";
    }
    // A followed night's token: the same row under the follow class, made by the Lua when
    // it judges the holder's sighting of Henry to be the walk home (GraphSaid) and gone a
    // moment later (emit.hpp, follow_class).
    for (const Quest* q : follows) {
        const std::string name = follow_name(*q);
        s += "\t\t<MiscItem Type=\"5\" SubType=\"0\" IconId=\"rutina_rustyAxe\" UIInfo=\"" +
             xml_escape(item_info_key(modid, name)) + "\" UIName=\"" + xml_escape(item_ui_key(modid, name)) +
             "\" PickpocketInPouch=\"true\" Model=\"manmade/task_specific_props/wood_industry/carpentry/saw.cgf\" "
             "Weight=\"0\" Price=\"0\" FadeCoef=\"3.174603\" VisibilityCoef=\"1\" Id=\"" +
             xml_escape(follow_class(*q)) + "\" Name=\"" + xml_escape(modid + "_" + name) + "\" />\r\n";
        // And the night's marker on him and off again (emit.hpp, mark_shown_class): two more
        // of the same row, made by the Lua as his hours come and go.
        for (int i = 0; i < 2; ++i) {
            const std::string mname = i == 0 ? mark_shown_name(*q) : mark_hidden_name(*q);
            const std::string mclass = i == 0 ? mark_shown_class(*q) : mark_hidden_class(*q);
            s += "\t\t<MiscItem Type=\"5\" SubType=\"0\" IconId=\"rutina_rustyAxe\" UIInfo=\"" +
                 xml_escape(item_info_key(modid, mname)) + "\" UIName=\"" + xml_escape(item_ui_key(modid, mname)) +
                 "\" PickpocketInPouch=\"true\" Model=\"manmade/task_specific_props/wood_industry/carpentry/saw.cgf\" "
                 "Weight=\"0\" Price=\"0\" FadeCoef=\"3.174603\" VisibilityCoef=\"1\" Id=\"" +
                 xml_escape(mclass) + "\" Name=\"" + xml_escape(modid + "_" + mname) + "\" />\r\n";
        }
    }
    // A chest's paper: a Document of the letters' shape (a loose sheet rather than a folded
    // one), laid in the chest beside the thing by the watch, and worth nothing.
    for (const Quest* q : papers) {
        const std::string name = paper_name(*q);
        s += "\t\t<Document Type=\"5\" IconId=\"letter_simple\" UIInfo=\"" + xml_escape(item_info_key(modid, name)) +
             "\" UIName=\"" + xml_escape(item_ui_key(modid, name)) + "\" PickpocketInPouch=\"true\" "
             "Model=\"characters/assets/parchment_folded/parchment_folded.cdf\" EntityScript=\"Book\" "
             "Weight=\"0\" Price=\"0\" FadeCoef=\"1.333333\" VisibilityCoef=\"1\" Id=\"" +
             xml_escape(paper_class(*q)) + "\" Name=\"" + xml_escape(modid + "_" + name) + "\">\r\n";
        s += "\t\t\t<DocumentContent Parts=\"" + xml_escape(paper_page_key(modid, *q)) + "\" />\r\n";
        s += "\t\t</Document>\r\n";
    }
    s += "\t</ItemClasses>\r\n";
    s += "</database>\r\n";
    return s;
}

std::vector<std::pair<std::string, std::string>> item_localization(const std::string& modid,
                                                                   const std::vector<Quest>& quests) {
    std::vector<std::pair<std::string, std::string>> rows;
    for (const CustomItem* item : custom_items_used(quests)) {
        rows.emplace_back(item_ui_key(modid, item->name), item->ui_name);
        rows.emplace_back(item_info_key(modid, item->name), item->ui_info);
    }
    for (const Quest& q : quests) {
        if (!q.has_document()) continue;
        const std::string name = letter_name(q);
        if (q.is_ambush()) {
            rows.emplace_back(item_ui_key(modid, name), "Directions");
            rows.emplace_back(item_info_key(modid, name),
                              "A way to somebody, scrawled by " + q.target_name + " for " + q.giver_name + "'s sake.");
        } else if (q.is_buy()) {
            rows.emplace_back(item_ui_key(modid, name), "Charm");
            rows.emplace_back(item_info_key(modid, name),
                              "A scrap of writing from " + q.target_name + ", to be carried against a curse. "
                              "Whether it does anything is not written on it.");
        } else if (q.has_dictation()) {
            // One row set per combination; the words are Henry's hand, and the info says so.
            for (const std::string& wrote : q.dictation_variants()) {
                const std::string variant = letter_variant_name(q, wrote);
                rows.emplace_back(item_ui_key(modid, variant), "Letter to " + q.target_name);
                rows.emplace_back(item_info_key(modid, variant),
                                  "A folded letter, sealed, from " + q.giver_name + " to " + q.target_name +
                                      ", written in Henry's hand at " + q.giver_name + "'s dictation.");
                rows.emplace_back(letter_variant_page_key(modid, q, wrote), page_markup(q.dictated_letter(wrote)));
            }
            continue;
        } else {
            rows.emplace_back(item_ui_key(modid, name), "Letter to " + q.target_name);
            rows.emplace_back(item_info_key(modid, name),
                              "A folded letter, sealed, from " + q.giver_name + " to " + q.target_name + ".");
        }
        rows.emplace_back(letter_page_key(modid, q), letter_page(q));
    }
    for (const Quest& q : quests) {
        if (!q.has_token()) continue;
        if (q.is_pray()) {
            rows.emplace_back(item_ui_key(modid, token_name(q)), "A prayer said");
            rows.emplace_back(item_info_key(modid, token_name(q)), "A prayer said for " + q.giver_name + ".");
        } else if (q.is_return()) {
            rows.emplace_back(item_ui_key(modid, token_name(q)), "A thing put back");
            rows.emplace_back(item_info_key(modid, token_name(q)), "A thing put back for " + q.giver_name + ".");
        } else if (q.is_ambush()) {
            rows.emplace_back(item_ui_key(modid, token_name(q)), "A trap sprung");
            rows.emplace_back(item_info_key(modid, token_name(q)), "A trap walked into for " + q.giver_name + ".");
        } else if (q.has_helper()) {
            rows.emplace_back(item_ui_key(modid, token_name(q)), "A name given");
            rows.emplace_back(item_info_key(modid, token_name(q)), q.giver_name + " named " + q.helper_name + ".");
        } else if (q.has_written_log()) {
            rows.emplace_back(item_ui_key(modid, token_name(q)), "A letter written");
            rows.emplace_back(item_info_key(modid, token_name(q)), q.giver_name + "'s words, in Henry's hand.");
        } else {
            rows.emplace_back(item_ui_key(modid, token_name(q)), "A thing left");
            rows.emplace_back(item_info_key(modid, token_name(q)), "A thing left for " + q.giver_name + ".");
        }
    }
    for (const Quest& q : quests) {
        if (q.hail.empty()) continue;
        rows.emplace_back(item_ui_key(modid, hail_name(q)), "A shout");
        rows.emplace_back(item_info_key(modid, hail_name(q)), q.giver_name + " called Henry over.");
    }
    for (const Quest& q : quests) {
        if (!q.has_follow()) continue;
        rows.emplace_back(item_ui_key(modid, follow_name(q)), "A glance back");
        rows.emplace_back(item_info_key(modid, follow_name(q)), q.target_name + " turned and saw Henry behind him.");
        rows.emplace_back(item_ui_key(modid, mark_shown_name(q)), "A night's watch");
        rows.emplace_back(item_info_key(modid, mark_shown_name(q)), q.target_name + " is on his way for the night.");
        rows.emplace_back(item_ui_key(modid, mark_hidden_name(q)), "A night's end");
        rows.emplace_back(item_info_key(modid, mark_hidden_name(q)), q.target_name + "'s night is not to be watched.");
    }
    for (const Quest& q : quests) {
        if (!q.has_chest()) continue;
        rows.emplace_back(item_ui_key(modid, key_name(q)), "Key to " + q.target_name + "'s chest");
        rows.emplace_back(item_info_key(modid, key_name(q)),
                          "A small iron key on a cord, to the heavy chest in " + q.target_name + "'s room.");
        rows.emplace_back(item_ui_key(modid, taken_name(q)), "A thing taken");
        rows.emplace_back(item_info_key(modid, taken_name(q)),
                          "The " + q.item_name + " out of " + q.target_name + "'s chest.");
        if (q.has_paper()) {
            const std::string name = q.chest_paper_name.empty() ? "A sheet of names" : q.chest_paper_name;
            rows.emplace_back(item_ui_key(modid, paper_name(q)), name);
            rows.emplace_back(item_info_key(modid, paper_name(q)),
                              "A loose sheet from the bottom of " + q.target_name + "'s chest, closely written.");
            rows.emplace_back(paper_page_key(modid, q), page_markup(q.chest_paper));
        }
    }
    return rows;
}

std::string talking_variable(const std::string& modid) { return modid + "_talking"; }

std::string face_variable(const std::string& modid) { return modid + "_face"; }

// One Autoselect sequence per value of gvar('<modid>_face'). Commands on a response
// are static, so switching expression means switching sequence, the same way the
// hold loop already switches on gvar('<modid>_talking'). Codes are the index in
// this table; Lua's Face() / Speak() write that integer and nothing else.
//
// Idle is camera only. Speak is the OpenMouth/Hmm stack that already read as a
// talking mouth on a wordless beat. Moods are FacialMoodCommand, which need no
// guid. Head clips are AnimationHeadCommand; guid and variant are copied from a
// shipped line and name the clip, not the character.
//
// customLipSync is omitted on purpose: shipped lines use it to suppress ordinary
// lipsync (laugh, drink, silence), not to start talking.
struct FaceDef {
    const char* name;
    enum Kind { Idle, Speak, Mood, Head } kind;
    const char* mood;
    const char* fragment;
    const char* guid;
    const char* variant;
};

const FaceDef kFaces[] = {
    {"idle", FaceDef::Idle, nullptr, nullptr, nullptr, nullptr},
    {"speak", FaceDef::Speak, nullptr, nullptr, nullptr, nullptr},

    {"thinking1", FaceDef::Mood, "thinking1", nullptr, nullptr, nullptr},
    {"thinking2", FaceDef::Mood, "thinking2", nullptr, nullptr, nullptr},
    {"thinking3", FaceDef::Mood, "thinking3", nullptr, nullptr, nullptr},
    {"happy1", FaceDef::Mood, "happy1", nullptr, nullptr, nullptr},
    {"happy2", FaceDef::Mood, "happy2", nullptr, nullptr, nullptr},
    {"happy3", FaceDef::Mood, "happy3", nullptr, nullptr, nullptr},
    {"angry1", FaceDef::Mood, "angry1", nullptr, nullptr, nullptr},
    {"angry2", FaceDef::Mood, "angry2", nullptr, nullptr, nullptr},
    {"angry3", FaceDef::Mood, "angry3", nullptr, nullptr, nullptr},
    {"nervous1", FaceDef::Mood, "nervous1", nullptr, nullptr, nullptr},
    {"nervous2", FaceDef::Mood, "nervous2", nullptr, nullptr, nullptr},
    {"nervous3", FaceDef::Mood, "nervous3", nullptr, nullptr, nullptr},
    {"sad1", FaceDef::Mood, "sad1", nullptr, nullptr, nullptr},
    {"sad2", FaceDef::Mood, "sad2", nullptr, nullptr, nullptr},
    {"sad3", FaceDef::Mood, "sad3", nullptr, nullptr, nullptr},
    {"arrogant1", FaceDef::Mood, "arrogant1", nullptr, nullptr, nullptr},
    {"arrogant2", FaceDef::Mood, "arrogant2", nullptr, nullptr, nullptr},
    {"arrogant3", FaceDef::Mood, "arrogant3", nullptr, nullptr, nullptr},
    {"confusion1", FaceDef::Mood, "confusion1", nullptr, nullptr, nullptr},
    {"confusion2", FaceDef::Mood, "confusion2", nullptr, nullptr, nullptr},
    {"confusion3", FaceDef::Mood, "confusion3", nullptr, nullptr, nullptr},
    {"neutral", FaceDef::Mood, "neutral", nullptr, nullptr, nullptr},
    {"yell", FaceDef::Mood, "yell", nullptr, nullptr, nullptr},
    {"whisper", FaceDef::Mood, "whisper", nullptr, nullptr, nullptr},

    {"smile", FaceDef::Head, nullptr, "ADLG_FA_Smile",
     "3213f1a5-7cef-42a5-a0ab-01b102460ffc", "1"},
    {"smile_teeth", FaceDef::Head, nullptr, "ADLG_FA_SmileTeeth",
     "c82caf95-869f-4844-b809-a31d53223f1b", "3"},
    {"smile_forced", FaceDef::Head, nullptr, "ADLG_FA_SmileForced",
     "1a0c3261-1c40-41f6-b231-282929ff99a4", "1"},
    {"laugh", FaceDef::Head, nullptr, "ADLG_FA_Laugh",
     "9612b042-252b-4f80-b303-5a4cdf0d3adb", "1"},
    {"haha", FaceDef::Head, nullptr, "ADLG_FA_Haha",
     "feebdd52-d36a-4b63-a107-c97e942887a7", "1"},
    {"haha_evil", FaceDef::Head, nullptr, "ADLG_FA_HahaEvil",
     "380b99fa-060b-432e-846a-0694d9645854", "0"},
    {"sigh", FaceDef::Head, nullptr, "ADLG_FA_Sigh",
     "549cfbd4-f1db-4883-a27d-a482ba56fe7b", "0"},
    {"surprise", FaceDef::Head, nullptr, "ADLG_FA_Surprise",
     "aa9827dc-8a8d-4799-a681-11bf3b0e8c73", "7"},
    {"annoyed", FaceDef::Head, nullptr, "ADLG_FA_Annoyed",
     "58af5ebf-08bc-4e4b-8e0e-e0d88d2135ae", "2"},
    {"suspicious", FaceDef::Head, nullptr, "ADLG_FA_Suspicious",
     "a451da38-f7ed-453e-9508-cae8f8692b3a", "0"},
    {"disgust", FaceDef::Head, nullptr, "ADLG_FA_Disgust",
     "94596c54-c8c8-4d47-b90d-5185dd8db46a", "0"},
    {"disgust_taste", FaceDef::Head, nullptr, "ADLG_FA_DisgustTaste",
     "a294381c-6288-4e5a-ab94-11881c5bca0e", "2"},
    {"wink", FaceDef::Head, nullptr, "ADLG_FA_Wink",
     "1fe4c3eb-c034-4b2c-a31b-cb1aee640dd8", "0"},
    {"wink_surprised", FaceDef::Head, nullptr, "ADLG_FA_WinkSurprised",
     "e662c989-944b-4be3-82b2-228c925506b1", "1"},
    {"eyebrows", FaceDef::Head, nullptr, "ADLG_FA_EyebrowsRaise",
     "6e30f870-e652-4054-abc7-46ae86863652", "0"},
    {"eyebrow_left", FaceDef::Head, nullptr, "ADLG_FA_EyebrowRaise_Left",
     "58bccf36-fb2d-4a50-8616-916837cc774d", "0"},
    {"eyebrow_right", FaceDef::Head, nullptr, "ADLG_FA_EyebrowRaise_Right",
     "af7eb8d8-db3a-404a-9b2e-312c0f795776", "0"},
    {"hmm", FaceDef::Head, nullptr, "ADLG_FA_Hmm",
     "bac33065-1c6c-4def-969b-86d9bf436af1", "1"},
    {"hmm_suspicious", FaceDef::Head, nullptr, "ADLG_FA_HmmSuspicious",
     "d9ab3809-c4ba-4bef-82b3-1e2500904988", "1"},
    {"open_mouth", FaceDef::Head, nullptr, "ADLG_FA_OpenMouth",
     "79fbefd0-5b94-4a4e-9efa-650bcdddd59d", "0"},
    {"shrug", FaceDef::Head, nullptr, "ADLG_FA_IDontKnow",
     "d227ac07-3fc2-4fe1-892a-712f70fd76a1", "1"},
    {"awkward", FaceDef::Head, nullptr, "ADLG_FA_Awkward",
     "8410c1c8-cfc7-41ef-bd65-24c4d7026e52", "0"},
    {"cough", FaceDef::Head, nullptr, "ADLG_FA_Cough",
     "deaabd21-879f-410e-8842-636e2b789de6", "0"},
    {"chew", FaceDef::Head, nullptr, "ADLG_FA_Chew",
     "66c129fa-b544-4f72-ba30-84ebb8442520", "1"},
    {"give_up", FaceDef::Head, nullptr, "ADLG_FA_GiveUpTalk",
     "63fe3fcf-3a8d-4fc3-94c0-0ef90454bc4b", "0"},
};

constexpr int kFaceCount = static_cast<int>(sizeof(kFaces) / sizeof(kFaces[0]));

std::string dialogue_hold_key(const std::string& modid, const std::string& name) {
    return modid + "_" + name + "_hold";
}

std::string menu_row_key(const std::string& modid, int row) {
    return modid + "_menu_" + std::to_string(row);
}

std::string emit_dialogue(const std::string& modid,
                          const std::string& name,
                          const std::string& role,
                          double beat_seconds,
                          const std::vector<QuestBeat>& beats) {
    if (beat_seconds <= 0) throw std::runtime_error("a beat needs a length in seconds");
    require_identifier(role, "dialogue role");

    std::string s = skald_header(modid);
    // FaderDialog, not Dialog. The element name is what decides whether this is a
    // conversation or a remark: of the shipped player-initiated dialogues that
    // offer a prompt and move the camera, 173 are FaderDialog against 11 Dialog,
    // while 254 plain Dialogs offer neither. Every real conversation read while
    // working this out, from sitting on a bench to talking round Benedikt, is a
    // FaderDialog.
    s += "\t\t<FaderDialog Name=\"" + xml_escape(name) + "\">\r\n";
    // One Out trigger per quest beat, fired by the sequence of the same name below
    // and wired into the quest by the project. Ports come before Text in every
    // shipped dialogue that has them.
    if (!beats.empty()) {
        s += "\t\t\t<Ports>\r\n";
        for (const QuestBeat& b : beats) {
            s += "\t\t\t\t<Port Name=\"" + xml_escape(quest_port(b)) +
                 "\" Direction=\"Out\" Type=\"trigger\" />\r\n";
        }
        s += "\t\t\t</Ports>\r\n";
    }
    s += "\t\t\t<Text Text=\"" + xml_escape(name) + "\" />\r\n";
    // Player as Initiator is what separates this from a bark, which the NPC
    // starts by itself. TechnicalStatus has to say Enabled or it is skipped.
    //
    // AllowGreeting is what stops "Greetings, what do you need?" landing on top of
    // the first thing the other party has to say. 1747 shipped dialogues set it and
    // not one sets it true, so false is the only value it is ever given; the
    // farewell beside it is the same at the other end.
    //
    // Nothing else belongs here. Type and ClashPriority are what an ambient line
    // carries, and carrying them is what got this played as one: ClashPriority
    // "Flavor" appears on 201 shipped dialogues that offer no prompt and exactly
    // one that does, while conversations leave both attributes off entirely.
    s += "\t\t\t<Dialogue TechnicalStatus=\"Enabled\" Initiator=\"Player\""
         " AllowGreeting=\"false\" AllowFarewell=\"false\">\r\n";
    // The camera is authored, not implied: a response with no CameraCommand leaves
    // the view exactly where it was, which is why a dialogue can run to completion
    // and still look like an overheard remark. CloseUp and CloseShot are the two
    // the shipped dialogues reach for most, 36405 uses between them.
    //
    // Role is who the camera looks at, which is not the same as who is talking --
    // shipped commands point it away from the speaker 2702 times against 13 --
    // because the usual shot is the other party reacting. Left off, a greeting
    // opens on the back of the player's head, so each line here is framed on
    // whoever says it.
    //
    // Faces are a bank, not a probe. The camera is on every beat; the mouth and
    // expression sit beside it and only fire when Lua has asked for that sequence.
    // Role stays the other party on both halves of the beat, so a face does not
    // drop when the graph steps to Henry.
    //
    // Speak is suggestive talking, not visemes. The engine's real mouth is driven
    // by voiceover, which this wordless beat does not have. OpenMouth and Hmm are
    // one-shot jaw clips; stacked with delays they read as the mouth working. Guids
    // are copied from shipped lines and name the clip, not the character.
    auto commands_for = [&](const FaceDef& face, const std::string& subject) {
        const std::string who = xml_escape(role);
        std::string c = "\t\t\t\t\t\t\t\t\t<Commands>";
        if (face.kind == FaceDef::Speak) {
            c += "<AnimationHeadCommand Role=\"" + who +
                 "\" Delay=\"0.15\" FragmentId=\"ADLG_FA_OpenMouth\" "
                 "Guid=\"79fbefd0-5b94-4a4e-9efa-650bcdddd59d\" Variant=\"0\" />"
                 "<AnimationHeadCommand Role=\"" + who +
                 "\" Delay=\"1.1\" FragmentId=\"ADLG_FA_Hmm\" "
                 "Guid=\"bac33065-1c6c-4def-969b-86d9bf436af1\" Variant=\"1\" />"
                 "<AnimationHeadCommand Role=\"" + who +
                 "\" Delay=\"2.0\" FragmentId=\"ADLG_FA_OpenMouth\" "
                 "Guid=\"79fbefd0-5b94-4a4e-9efa-650bcdddd59d\" Variant=\"0\" />";
        } else if (face.kind == FaceDef::Mood) {
            c += "<FacialMoodCommand Role=\"" + who + "\" FacialMood=\"" +
                 xml_escape(face.mood) + "\" />";
        } else if (face.kind == FaceDef::Head) {
            c += "<AnimationHeadCommand Role=\"" + who +
                 "\" Delay=\"0.15\" FragmentId=\"" + xml_escape(face.fragment) +
                 "\" Guid=\"" + xml_escape(face.guid) + "\" Variant=\"" +
                 xml_escape(face.variant) + "\" />";
        }
        c += "<CameraCommand Role=\"" + xml_escape(subject) +
             "\" CameraType=\"CloseUp\" /></Commands>\r\n";
        return c;
    };

    // One line, said over and over, and the same id behind it every time. That is
    // what makes it free: an id keeps the first words it is given, which only hurts
    // when each turn needs different ones, and this one says the same thing on the
    // thousandth beat as on the first.
    //
    // It is a blank rather than nothing at all. A response with no Text child is a shape
    // the game ships -- hracovo_modleni.xml loops on nothing else -- and a dialogue built
    // that way was measured not to open: the request was accepted, the camera never
    // moved, and the beat's exit script never ran, which is what a session opening and
    // closing inside a frame looks like. The shipped examples that do it are monologues
    // and forced dialogues; a player-initiated one presents its root decision as a menu,
    // and neither a promptless sequence nor a wordless line can be presented.
    //
    // The presentable part is the prompt, and only the prompt. The responses say nothing at
    // all -- no Text child, no id, no row -- because whatever they say is a caption, and the
    // caption is where our own words are going.
    //
    // Three attempts were made at wording that line away first, and all three showed
    // something: three dots showed three dots, a non-breaking space showed a small box
    // (a font meeting a character it has no glyph for), and an empty string showed
    // `@moonshine_names_talk_line`, since an id resolving to nothing is an id the engine
    // reports as missing. Then the widget was hidden, then written over eight times a
    // second, and the caption still blinked wherever a response began -- because the
    // dialogue was not setting that field, it was clearing and re-playing it.
    //
    // A wordless response is ordinary rather than daring: 7104 of the shipped
    // FaderDialogs' responses carry no Text child, against 72526 that do, and 167 of
    // those carry ReferenceLength, which is the one thing this shape needs to keep.
    //
    // What paces a silent beat is StartDelay, not ReferenceLength.
    //
    // ReferenceLength is how long a line takes when no recorded audio decides it, and
    // with a line to say that is what held a beat to six seconds. With nothing to say it
    // is ignored: the first wordless run came round every 0.06s, 1032 beats in 69
    // seconds, which is the loop turning at the frame rate. Nothing looked wrong on
    // screen, because the words run on their own clock, but every beat re-evaluates two
    // decisions, runs an exit script and issues a camera command.
    //
    // StartDelay is a wait rather than a duration, so it does not need a line to measure.
    // It is used 17505 times in the shipped graphs, in seconds, and 72 of those are on
    // responses with no Text -- some carrying both, exactly as here:
    // <Response Role="HENRY" StartDelay="1.81" ReferenceLength="5">. The length is kept
    // beside it because it says what the beat is meant to be worth, and costs nothing if
    // the engine goes on ignoring it.
    //
    // All of the wait goes on the second response and none on the first, because the fade
    // to black that opens a session waits for the first response to begin: a delay there
    // is a delay before the player sees anything, and three seconds of it was noticeable
    // as a longer fade than the game's own.
    const std::string hold_key = dialogue_hold_key(modid, name);
    auto beat = [&](const std::string& speaker, double wait, double length,
                    const std::string& cmds) {
        std::string e;
        e += "\t\t\t\t\t\t\t\t<Response Role=\"" + xml_escape(speaker) + "\"";
        if (wait > 0) e += " StartDelay=\"" + trim_number(wait) + "\"";
        e += " ReferenceLength=\"" + trim_number(length) + "\">\r\n";
        e += cmds;
        e += "\t\t\t\t\t\t\t\t</Response>\r\n";
        return e;
    };

    // The loop that holds the camera. Two responses in the beat rather than one,
    // because a sequence holding a single response finishes the instant it starts:
    // the session opens and closes inside a frame, the fade and the camera never
    // land, and the request that asked for it still returns true, which reads
    // exactly like a refusal. Both are framed on the other party, so a beat is one
    // held shot rather than a cut back and forth.
    //
    // EndType="Decision" nests the branch inside the sequence, the way the shipped
    // tipster and praying dialogues do, and 123 player-initiated FaderDialogs use it.
    //
    // Autoselect is what makes a decision pick instead of ask, and leaving it off is
    // what held a session open for a minute with nothing happening: the root played
    // its beat, the graph reached the nested decision, and it waited for a choice from
    // a menu whose entries carry no prompt and so cannot be presented. The log said
    // indialog=true throughout and the exit script never ran, since a sequence ending
    // on a decision has not ended until the decision resolves.
    //
    // A dialogue the player starts presents every decision it reaches, which is the
    // difference from the shipped wordless loops: those are all NonPlayer-initiated,
    // so nothing in them is ever offered. 6143 shipped decisions set this, always
    // true, and 1932 of them are nested one sequence deep exactly as the hold
    // branch is.
    //
    // One sequence per face, Autoselected by gvar. Exclusive equalities so exactly
    // one entry is available, which is what keeps this from becoming a menu. The
    // prompt is on every sequence because any of them can be the one the session
    // opens on, and a sequence with no prompt cannot be presented.
    const std::string talk_var = talking_variable(modid);
    const std::string face_var = face_variable(modid);
    const std::string quest_var = quest_variable(modid);
    // Speak retriggers the jaw stack about as often as the stacked delays last.
    // Idle and moods use the same wait, so a Speak() or Face() lands on the next
    // 2.5s boundary rather than waiting out a 6-second camera hold. --beat-seconds
    // still wins when it is shorter.
    const double wait = std::min(beat_seconds, 2.5);

    // One beat of the loop: the prompt, the two wordless responses, and the nested
    // hold-or-leave decision. Faces and quest beats share it; they differ only in the
    // entry condition, the commands, the triggers and what the exit script is told.
    auto sequence = [&](const std::string& tag, const std::string& condition,
                        const std::string& triggers, const std::string& cmds,
                        const std::string& exit) {
        std::string e;
        e += "\t\t\t\t\t\t<Sequence EntryCondition=\"" + condition +
             "\" EndType=\"Decision\" Name=\"" + tag + "\" ExitScript=\"" + exit + "\">\r\n";
        e += "\t\t\t\t\t\t\t<UiPrompt StringName=\"" + xml_escape(hold_key) + "\" Text=\"" +
             xml_escape(kHoldPrompt) + "\" />\r\n";
        e += triggers;
        e += "\t\t\t\t\t\t\t<Elements>\r\n";
        e += beat(role, 0, wait / 2, cmds);
        e += beat("HENRY", wait, wait / 2, cmds);
        e += "\t\t\t\t\t\t\t</Elements>\r\n";
        e += "\t\t\t\t\t\t\t<Decision Name=\"hold_" + tag + "\" Autoselect=\"true\">\r\n";
        e += "\t\t\t\t\t\t\t\t<Sequences>\r\n";
        e += "\t\t\t\t\t\t\t\t\t<Sequence EntryCondition=\"gvar('" + xml_escape(talk_var) +
             "')==1\" EndType=\"GoTo\" GoToDecision=\"dec1\" Name=\"stay_" + tag + "\">\r\n";
        e += "\t\t\t\t\t\t\t\t\t\t<Elements>\r\n";
        e += "\t\t\t\t\t\t\t\t\t\t\t<Response Role=\"" + xml_escape(role) + "\" />\r\n";
        e += "\t\t\t\t\t\t\t\t\t\t</Elements>\r\n";
        e += "\t\t\t\t\t\t\t\t\t</Sequence>\r\n";
        // One bare response each, and the bareness is the point. A sequence with an
        // empty <Elements /> and no response at all crashes the game -- not when it
        // loads and not when the dialogue opens, but at the moment a GoTo arrives at
        // the decision holding it, which puts the crash several seconds after its
        // cause. One response is safe, which hracovo_modleni.xml demonstrates on
        // both of its branches.
        e += "\t\t\t\t\t\t\t\t\t<Sequence EntryCondition=\"gvar('" + xml_escape(talk_var) +
             "')!=1\" EndType=\"EndDialogue\" Name=\"go_" + tag + "\" ExitScript=\"" +
             std::string("Moonshine") + ".OnClosed()\">\r\n";
        e += "\t\t\t\t\t\t\t\t\t\t<Elements>\r\n";
        e += "\t\t\t\t\t\t\t\t\t\t\t<Response Role=\"" + xml_escape(role) + "\" />\r\n";
        e += "\t\t\t\t\t\t\t\t\t\t</Elements>\r\n";
        e += "\t\t\t\t\t\t\t\t\t</Sequence>\r\n";
        e += "\t\t\t\t\t\t\t\t</Sequences>\r\n";
        e += "\t\t\t\t\t\t\t</Decision>\r\n";
        e += "\t\t\t\t\t\t</Sequence>\r\n";
        return e;
    };

    // The quest gate on every face, so that while a quest beat is asked for no face
    // is available and exactly one sequence still is. Only written when there are
    // quests, so a mod without any is byte-for-byte what it was.
    const std::string no_quest =
        beats.empty() ? std::string() : " AND gvar('" + xml_escape(quest_var) + "')==0";
    // And the talking gate on everything, which is what keeps this dialogue out of the
    // base game's menu. The game offers its own Talk prompt for any soul with a dialogue
    // it could start, and it decides that by looking for an entry sequence whose
    // condition holds -- so a loop that was always enterable made every one of these
    // souls look like a quest character, and the wrapper had to tell our prompt from a
    // real one by the metarole, which stopped telling them apart the moment a soul had
    // both. Lua raises this global just before it asks for the session and lowers it
    // when the conversation ends, so between conversations nothing here can be entered
    // and a Talk prompt on one of these souls is always the game's own.
    const std::string talking = "gvar('" + xml_escape(talk_var) + "')==1 AND ";

    s += "\t\t\t\t<Decision Name=\"dec1\" Autoselect=\"true\">\r\n";
    s += "\t\t\t\t\t<Sequences>\r\n";
    for (int i = 0; i < kFaceCount; ++i) {
        const FaceDef& face = kFaces[i];
        s += sequence("face_" + xml_escape(face.name),
                      talking + "gvar('" + xml_escape(face_var) + "')==" + std::to_string(i) +
                          no_quest,
                      "", commands_for(face, role), "Moonshine.Beat()");
    }
    // Henry's beat: the one shot in the loop that frames him, for while his own line is
    // being spoken. The menu turns the camera on him to pick, and the row picked sent the
    // graph straight back to a face beat framed on the other party, so with a voice his
    // words were heard over a shot of the person listening (noted 12 Oct 2026). Two past
    // the bank (the menu is one past), camera only, held for as long as Lua keeps the code
    // up: until the app says the voice is done with his words, or the reading time plus a
    // grace has gone by.
    s += sequence("face_henry",
                  talking + "gvar('" + xml_escape(face_var) + "')==" + std::to_string(kFaceCount + 1) +
                      no_quest,
                  "", commands_for(kFaces[0], "HENRY"), "Moonshine.Beat()");
    // A quest beat is a camera-only beat that also fires a port. The Triggers element
    // is what fires it, in the place the shipped sequences put it (after the prompt,
    // before Elements); the exit script hands the code back so the next loop is a
    // face again, and the Lua also times the code out in case the exit never runs.
    for (const QuestBeat& b : beats) {
        const std::string port = xml_escape(quest_port(b));
        const std::string triggers =
            "\t\t\t\t\t\t\t<Triggers>\r\n\t\t\t\t\t\t\t\t<Port Name=\"" + port +
            "\" />\r\n\t\t\t\t\t\t\t</Triggers>\r\n";
        s += sequence(port,
                      talking + "gvar('" + xml_escape(quest_var) + "')==" + std::to_string(b.code),
                      triggers, commands_for(kFaces[0], role),
                      "Moonshine.QuestBeat(" + std::to_string(b.code) + ")");
    }

    // The menu: the face code one past the bank plays this instead of a beat. Its own
    // responses only turn the camera on Henry; what holds the session is the decision
    // under them, which is not Autoselected and so is presented and waited on. A menu
    // the engine is waiting on is the one state of this graph with no beat boundary,
    // and a boundary is what clears the HUD's list (measured: a list drawn from Lua
    // survives both responses of a beat and is gone the frame after the exit script,
    // so the clearing is in the step from one sequence to the next, not in a response
    // starting). Drawn over from Lua after it is presented, the list stays up for as
    // long as the player takes.
    //
    // The rows' prompts are placeholders, dots, since the words they will carry are
    // decided while the game is running. Every row but the last goes back to the top
    // of the loop; the last is the way out. Each row's exit script says which it was,
    // which is how the pick reaches Lua without any input handling of our own: the
    // engine's own dialogue navigation moves the highlight and confirms.
    {
        const std::string henry_camera =
            "\t\t\t\t\t\t\t\t\t<Commands><CameraCommand Role=\"HENRY\" CameraType=\"CloseUp\" />"
            "</Commands>\r\n";
        s += "\t\t\t\t\t\t<Sequence EntryCondition=\"" + talking + "gvar('" + xml_escape(face_var) +
             "')==" + std::to_string(kFaceCount) + no_quest +
             "\" EndType=\"Decision\" Name=\"menu\">\r\n";
        s += "\t\t\t\t\t\t\t<UiPrompt StringName=\"" + xml_escape(hold_key) + "\" Text=\"" +
             xml_escape(kHoldPrompt) + "\" />\r\n";
        s += "\t\t\t\t\t\t\t<Elements>\r\n";
        // Two responses, as in a beat, since one finishes inside a frame; short, since the
        // camera is all they are for and the menu should be up as soon as it has turned.
        s += beat(role, 0, 0.25, henry_camera);
        s += beat("HENRY", 0.25, 0.25, henry_camera);
        s += "\t\t\t\t\t\t\t</Elements>\r\n";
        s += "\t\t\t\t\t\t\t<Decision Name=\"pick\">\r\n";
        s += "\t\t\t\t\t\t\t\t<Sequences>\r\n";
        for (int row = 1; row <= kMenuRows; ++row) {
            const bool last = row == kMenuRows;
            s += "\t\t\t\t\t\t\t\t\t<Sequence EndType=\"" +
                 std::string(last ? "EndDialogue" : "GoTo\" GoToDecision=\"dec1") +
                 "\" Name=\"row_" + std::to_string(row) + "\" ExitScript=\"Moonshine.Picked(" +
                 std::to_string(row) + ")\">\r\n";
            s += "\t\t\t\t\t\t\t\t\t\t<UiPrompt StringName=\"" +
                 xml_escape(menu_row_key(modid, row)) + "\" Text=\"" + xml_escape(kHoldPrompt) +
                 "\" />\r\n";
            s += "\t\t\t\t\t\t\t\t\t\t<Elements>\r\n";
            s += "\t\t\t\t\t\t\t\t\t\t\t<Response Role=\"" + xml_escape(role) + "\" />\r\n";
            s += "\t\t\t\t\t\t\t\t\t\t</Elements>\r\n";
            s += "\t\t\t\t\t\t\t\t\t</Sequence>\r\n";
        }
        s += "\t\t\t\t\t\t\t\t</Sequences>\r\n";
        s += "\t\t\t\t\t\t\t</Decision>\r\n";
        s += "\t\t\t\t\t\t</Sequence>\r\n";
    }
    s += "\t\t\t\t\t</Sequences>\r\n";
    s += "\t\t\t\t</Decision>\r\n";

    s += "\t\t\t</Dialogue>\r\n";
    s += "\t\t</FaderDialog>\r\n";
    s += "\t</Skald>\r\n";
    s += "</Database>\r\n";
    return s;
}

std::string emit_storm_rules(const std::string& modid,
                             const std::vector<Persona>& personas,
                             int rule_order) {
    std::string s;
    s += "<?xml version=\"1.0\"?>\r\n";
    s += "<!DOCTYPE storm SYSTEM \"..\\storm.dtd\">\r\n";
    s += "<storm>\r\n";
    s += "\t<rules>\r\n";
    for (const Persona& p : personas) {
        // A game-named soul cast by exception keeps the name the game gave them.
        if (p.game_named) continue;
        const std::string soul = xml_escape(p.soul_name);
        s += "\t\t<rule name=\"" + modid + "_" + soul + "\"";
        if (rule_order != 0) {
            s += " order=\"" + std::to_string(rule_order) + "\"";
        }
        s += ">\r\n";
        s += "\t\t\t<selectors><hasName name=\"" + soul + "\" /></selectors>\r\n";
        s += "\t\t\t<operations><setUiName name=\"" +
             xml_escape(ui_name_key(modid, p.soul_name)) + "\" /></operations>\r\n";
        s += "\t\t</rule>\r\n";
    }
    s += "\t</rules>\r\n";
    s += "</storm>\r\n";
    return s;
}

std::string emit_localization(
    const std::string& modid,
    const std::vector<Persona>& personas,
    const std::vector<std::pair<std::string, std::string>>& extras) {
    // The shipped text_ui_*.xml files have no XML declaration and no BOM. The
    // middle cell is ignored by the game; the third is what players read.
    auto row = [](const std::string& key, const std::string& text) {
        return "<Row><Cell>" + key + "</Cell><Cell>" + text + "</Cell><Cell>" + text +
               "</Cell></Row>\r\n";
    };

    std::string s;
    s += "<Table>\r\n";
    for (const Persona& p : personas) {
        if (p.game_named) continue;   // no rename rule, so no name row (emit_storm_rules)
        s += row(xml_escape(ui_name_key(modid, p.soul_name)), xml_escape(p.full_name));
    }
    for (const auto& e : extras) {
        s += row(xml_escape(e.first), xml_escape(e.second));
    }
    s += "</Table>\r\n";
    return s;
}

std::string emit_font_xml(const std::string& face) {
    if (face.empty()) throw std::runtime_error("a font definition needs a face");
    std::string s;
    s += "<fontshader>\r\n";
    // The width and height are the glyph cache the face is rasterised into, and 512 is
    // what shipped subtitles.xml gives AlexanderQuill. VeraMono is given 288x416 there,
    // so this is per-face rather than a constant, and too small a cache is how glyphs
    // come out clipped.
    s += "\t<font path=\"" + xml_escape(face) + "\" w=\"512\" h=\"512\"/>\r\n";
    s += "\t<effect name=\"default\">\r\n";
    s += "\t\t<pass>\r\n";
    s += "\t\t</pass>\r\n";
    s += "\t\t<pass>\r\n";
    s += "\t\t\t<color r=\"0\" g=\"0\" b=\"0\" a=\"1\"/>\r\n";
    s += "\t\t\t<pos x=\"1\" y=\"1\"/>\r\n";
    s += "\t\t</pass>\r\n";
    s += "\t</effect>\r\n";
    // Named in shipped default.xml and subtitles.xml both, and something else may ask for
    // it by name, so it is kept rather than dropped.
    s += "\t<effect name=\"simple\">\r\n";
    s += "\t\t<pass>\r\n";
    s += "\t\t</pass>\r\n";
    s += "\t</effect>\r\n";
    s += "</fontshader>\r\n";
    return s;
}

namespace {

// The store that rides in the save, the hooks that carry it there and back, and the
// autopilot that lets a run of the game be driven from a file with nobody at the keyboard.
// See data/kcd2/state-design/plan.md; this is its section 2 (the spike) and the skeleton of
// its section 3 (the store).
std::string state_lua(const std::string& g) {
    std::string s;
    s += "-- ---------------------------------------------------------------------------\n";
    s += "-- The store: what the save remembers on this mod's account.\n";
    s += "--\n";
    s += "-- One versioned table under one key of the player's save table. The player entity\n";
    s += "-- is the host because it is always there; an NPC's script table is streamed out\n";
    s += "-- with the NPC. Everything that changes during play and that the app or the mod\n";
    s += "-- wants back after a load goes in here and nowhere else -- never a file beside the\n";
    s += "-- saves, which is keyed to the machine and not to the world the player is in.\n";
    s += g + ".stateVersion = 1\n";
    s += "function " + g + ".FreshState()\n";
    // known: who Henry has heard of without meeting -- the people an NPC named in his
    // hearing, by the token the app sends on @heard (a soul id, or name:<name> for somebody
    // not in the game). The app reads it back off the dump with the souls met, and offers a
    // question about a person only once he could know of them (notes.md 7 Oct 2026).
    s += "\treturn { version = " + g + ".stateVersion, souls = {}, quests = {}, known = {} }\n";
    s += "end\n";
    s += g + ".state = " + g + ".FreshState()\n";
    s += "\n";
    // A clock for measuring our own part of a save or load. Neither spelling is certain
    // to be bound, so the first that answers is used and a silent zero is the fallback.
    s += "function " + g + ".Now()\n";
    s += "\tlocal ok, t = pcall(function() return System.GetCurrAsyncTime() end)\n";
    s += "\tif ok and type(t) == \"number\" then return t end\n";
    s += "\tok, t = pcall(function() return os.clock() end)\n";
    s += "\tif ok and type(t) == \"number\" then return t end\n";
    s += "\treturn 0\n";
    s += "end\n";
    s += "\n";
    // Text bytes in a table, which is the number the budget is set in. Keys count too,
    // since the serializer writes them.
    s += "function " + g + ".Bytes(v, seen)\n";
    s += "\tseen = seen or {}\n";
    s += "\tlocal t = type(v)\n";
    s += "\tif t == \"string\" then return #v end\n";
    s += "\tif t ~= \"table\" or seen[v] then return 0 end\n";
    s += "\tseen[v] = true\n";
    s += "\tlocal n = 0\n";
    s += "\tfor k, x in pairs(v) do\n";
    s += "\t\tn = n + " + g + ".Bytes(k, seen) + " + g + ".Bytes(x, seen)\n";
    s += "\tend\n";
    s += "\treturn n\n";
    s += "end\n";
    s += "\n";
    s += "function " + g + ".Tally(t)\n";
    s += "\tlocal n = 0\n";
    s += "\tif type(t) == \"table\" then for _ in pairs(t) do n = n + 1 end end\n";
    s += "\treturn n\n";
    s += "end\n";
    s += "\n";
    // Called from the wrapped Player.OnSave (the hook the shipped build actually calls;
    // OnSaveAI never fires, see the state-design plan). The whole store goes on the table
    // by reference; the engine serializes it after this returns, so the milliseconds
    // logged here are ours alone (0.3 ms at 100 MB) and the engine's part is measured
    // from outside, by the autopilot, as the gap to the .whs appearing (under 2 s).
    // Two rules the engine imposes on what goes on the table, both measured on the Windows laptop: no
    // string of 65,536 bytes or more (the save writes and then will not load), which is
    // what stateChunk and Chunk() below are for; and numbers are 32-bit floats, exact
    // only below 2^24.
    s += "function " + g + ".OnSave(save, via)\n";
    s += "\tlocal t0 = " + g + ".Now()\n";
    s += "\tsave.moonshine = " + g + ".state\n";
    s += "\tlocal bytes = string.format(\"%d\", " + g + ".Bytes(" + g + ".state))\n";
    s += "\t" + g + ".Log(\"BRIDGE state saved via \" .. tostring(via) .. \" version=\" .. tostring(" + g + ".state.version) ..\n";
    s += "\t\t\" souls=\" .. " + g + ".Tally(" + g + ".state.souls) .. \" bytes=\" .. bytes ..\n";
    s += "\t\t\" ms=\" .. string.format(\"%.1f\", (" + g + ".Now() - t0) * 1000))\n";
    s += "end\n";
    s += "\n";
    // Called from the wrapped Player.OnLoad. A load is the player changing worlds, so the
    // store is replaced, not merged: whatever was in memory belonged to the save before.
    // Nothing under the key -- a save from before the mod, or a new game -- is a fresh
    // store; but note the engine does not call OnLoad at all for a save with nothing of
    // ours in it, so the reset for that case has to happen when the world arrives
    // (OnInit, which fires before OnLoad on every load measured). A store from a newer
    // mod is kept as it is and said so; rewriting it would be the older mod destroying
    // what the newer one knew.
    s += g + ".worldHooks = {}\n";
    s += "function " + g + ".OnWorld(via, entity)\n";
    // Every time, not once: a load is the player changing worlds, and the engine does not
    // call OnLoad for a save that has nothing of ours, so this is the only reset that
    // save gets. OnLoad, when it comes, replaces the fresh store with what was saved.
    s += "\tif via == \"OnInit\" then\n";
    s += "\t\t" + g + ".state = " + g + ".FreshState()\n";
    s += "\t\t" + g + ".stateSoul = nil\n";
    s += "\t\t" + g + ".Log(\"BRIDGE state reset, a world is arriving\")\n";
    s += "\tend\n";
    s += "\tif not " + g + ".worldHooks[via] then\n";
    s += "\t\t" + g + ".worldHooks[via] = true\n";
    s += "\t\t" + g + ".Log(\"BRIDGE state hook \" .. tostring(via) .. \" fired\")\n";
    s += "\t\tif via == \"OnInit\" then\n";
    s += "\t\t\tpcall(" + g + ".PatchRules)\n";
    s += "\t\t\tlocal ok, err = pcall(function() entity:SetTimer(31337, 5000) end)\n";
    s += "\t\t\t" + g + ".Log(\"BRIDGE state hook entity:SetTimer -> \" .. tostring(ok) .. \" \" .. tostring(err))\n";
    s += "\t\tend\n";
    s += "\tend\n";
    s += "\t" + g + ".AutoHeartbeat(via)\n";
    s += "\tif " + g + ".auto ~= nil and " + g + ".auto.awaitLoad then\n";
    s += "\t\t" + g + ".auto.awaitLoad = false\n";
    s += "\t\t" + g + ".Log(\"BRIDGE auto \" .. tostring(via) .. \" fired, a world is arriving\")\n";
    s += "\t\t" + g + ".AutoAfter(2000)\n";
    s += "\tend\n";
    s += "\tpcall(" + g + ".AutoArmNudge, entity)\n";
    // The loop armed here is in doubt until it ticks (WatchArm): a load wipes it.
    s += "\tif via == \"OnInit\" or via == \"OnReset\" or via == \"OnLoad\" then " + g + ".watchDoubt = true end\n";
    s += "\tpcall(" + g + ".WatchArm, via)\n";
    s += "end\n";
    s += "\n";
    // After a load the engine gives the autopilot no heartbeat of its own until the player
    // does something: no OnUpdate, no GetActions with nobody in view, and the timers armed
    // during loading are wiped. The runs that went on had a Player:OnAction thirty seconds
    // in, which turned out to be a gamepad Steam kept re-reading; when it went to sleep,
    // every run sat at waitloaded until the runner's idle kill. Plain keys do not reach
    // OnAction outside the mod's own actionmaps, so while a script is waiting for a world
    // the F9 map (the typing toggle, which does nothing between conversations) is pointed
    // at the player and switched on, and the runner presses F9 -- one OnAction, one
    // heartbeat -- until the world has been seen; then the map goes back off.
    s += "function " + g + ".AutoArmNudge(entity)\n";
    s += "\tlocal a = " + g + ".auto\n";
    s += "\tif a == nil or a.steps == nil or #a.steps == 0 or a.finished or a.nudgeArmed then return end\n";
    s += "\tif not " + g + ".typingLoaded then return end\n";
    s += "\tlocal who = entity\n";
    s += "\tif who == nil or who.id == nil then who = rawget(_G, \"player\") end\n";
    s += "\tif who == nil or who.id == nil then return end\n";
    s += "\tif ActionMapManager.SetActionListener ~= nil then\n";
    s += "\t\tpcall(function() ActionMapManager.SetActionListener(" + g + ".typingToggleMap, who.id) end)\n";
    s += "\tend\n";
    s += "\tif " + g + ".TypingMap(" + g + ".typingToggleMap, true) then\n";
    s += "\t\ta.nudgeArmed = true\n";
    s += "\t\t" + g + ".Log(\"BRIDGE auto F9 armed for the runner's nudge\")\n";
    s += "\tend\n";
    s += "end\n";
    s += "\n";
    s += "function " + g + ".AutoDisarmNudge()\n";
    s += "\tlocal a = " + g + ".auto\n";
    s += "\tif a == nil or not a.nudgeArmed then return end\n";
    s += "\ta.nudgeArmed = false\n";
    s += "\tif not " + g + ".active then " + g + ".TypingMap(" + g + ".typingToggleMap, false) end\n";
    s += "end\n";
    s += "\n";
    s += "function " + g + ".OnLoad(saved, via)\n";
    s += "\t" + g + ".OnWorld(via)\n";
    s += "\tlocal t0 = " + g + ".Now()\n";
    s += "\tlocal got = nil\n";
    s += "\tif type(saved) == \"table\" then got = saved.moonshine end\n";
    s += "\tif type(got) ~= \"table\" then\n";
    s += "\t\t" + g + ".state = " + g + ".FreshState()\n";
    s += "\t\t" + g + ".Log(\"BRIDGE state loaded via \" .. tostring(via) .. \" none, fresh store\")\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    s += "\tif type(got.version) ~= \"number\" then got.version = 0 end\n";
    s += "\tif type(got.souls) ~= \"table\" then got.souls = {} end\n";
    s += "\tif type(got.quests) ~= \"table\" then got.quests = {} end\n";
    s += "\tif type(got.known) ~= \"table\" then got.known = {} end\n";
    s += "\t" + g + ".state = got\n";
    s += "\t" + g + ".Log(\"BRIDGE state loaded via \" .. tostring(via) .. \" version=\" .. got.version ..\n";
    s += "\t\t\" souls=\" .. " + g + ".Tally(got.souls) .. \" bytes=\" .. string.format(\"%d\", " + g + ".Bytes(got)) ..\n";
    s += "\t\t\" ms=\" .. string.format(\"%.1f\", (" + g + ".Now() - t0) * 1000))\n";
    s += "\tif got.version > " + g + ".stateVersion then\n";
    s += "\t\t" + g + ".Log(\"BRIDGE state is from a newer mod (\" .. got.version .. \" > \" ..\n";
    s += "\t\t\t" + g + ".stateVersion .. \"), kept as it is\")\n";
    s += "\tend\n";
    // The holder menus' globals follow the store, not the save's own copy of them.
    s += "\t" + g + ".flagsSynced = false\n";
    s += "\tpcall(" + g + ".QuestFlags)\n";
    // A find's placed keepsake may not survive the load (behaviors.md §6.1a). `laid`
    // would otherwise stay true and WatchKeepsake would never try again. Cleared here;
    // the watch re-lays when Henry is near and the heart is not in his pocket.
    s += "\tfor id, f in pairs(" + g + ".state.quests or {}) do\n";
    s += "\t\tlocal q = " + g + ".quests[id]\n";
    s += "\t\tif type(f) == \"table\" and q ~= nil and q.kind == \"find\" and q.keepsake ~= nil then\n";
    s += "\t\t\tf.laid = nil\n";
    s += "\t\tend\n";
    s += "\tend\n";
    s += "end\n";
    s += "\n";
    // The store's contents (plan.md section 3). Per soul: met, first, last, rev, memory,
    // stats; quest facts at the top level by quest id, because both parties to a quest
    // are told the same facts and the app looks them up by the quest, not the soul.
    // Everything here is Lua the app never sees; what it sees is Dump(), below.
    s += "-- The soul's record, made on first sight. Fields a later version adds are filled in\n";
    s += "-- here too, so a record from an older save is never missing one.\n";
    s += "function " + g + ".Soul(id)\n";
    s += "\tlocal souls = " + g + ".state.souls\n";
    s += "\tlocal soul = souls[id]\n";
    s += "\tif type(soul) ~= \"table\" then\n";
    s += "\t\tsoul = { met = 0, rev = 0, memory = {}, stats = {} }\n";
    s += "\t\tsouls[id] = soul\n";
    s += "\tend\n";
    s += "\tif type(soul.met) ~= \"number\" then soul.met = 0 end\n";
    s += "\tif type(soul.rev) ~= \"number\" then soul.rev = 0 end\n";
    s += "\tif type(soul.memory) ~= \"table\" then soul.memory = {} end\n";
    s += "\tif type(soul.stats) ~= \"table\" then soul.stats = {} end\n";
    s += "\treturn soul\n";
    s += "end\n";
    s += "\n";
    // Calendar.GetWorldTime() is seconds since the game's epoch. Nil when the calendar is
    // not there to ask, which the app reads as an unknown gap rather than a zero one.
    s += "function " + g + ".WorldTime()\n";
    s += "\tlocal ok, t = pcall(function() return Calendar.GetWorldTime() end)\n";
    s += "\tif ok and type(t) == \"number\" then return t end\n";
    s += "\treturn nil\n";
    s += "end\n";
    s += "\n";
    // A number for the log: whole seconds, or nil spelled out. %d and not .., since the
    // game's Lua prints large numbers in exponent form.
    s += "function " + g + ".Whole(n)\n";
    s += "\tif type(n) ~= \"number\" then return \"nil\" end\n";
    s += "\treturn string.format(\"%d\", n)\n";
    s += "end\n";
    s += "\n";
    // One entry appended. The 65,535-byte rule is enforced here, on the one road into the
    // memory: a spoken line is never near it and a summary is bounded by the app, so a cut
    // is a fault being contained rather than a thing that happens.
    s += "function " + g + ".StateRemember(id, entry)\n";
    s += "\tif type(entry.s) == \"string\" and #entry.s > " + g + ".stateChunk then\n";
    s += "\t\tentry.s = string.sub(entry.s, 1, " + g + ".stateChunk)\n";
    s += "\tend\n";
    s += "\tlocal soul = " + g + ".Soul(id)\n";
    s += "\tsoul.memory[#soul.memory + 1] = entry\n";
    s += "end\n";
    s += "\n";
    // The facts of a quest, set by the same directives that drive the game's graph, so
    // the two agree. handover is what the holder's own dialogue reports through
    // QuestHolderGave; accept and complete come from the app's directives.
    // Three more are a dispute's: broker, when Henry offers the holder to carry his
    // grievance (the holder menu's own exit script); apologise and refuse_apology, the
    // giver's answer when Henry puts it to him (the app's directives). And how, set at
    // the handover, is which branch won the thing, for the completed log.
    s += "function " + g + ".StateQuest(id, command)\n";
    s += "\tlocal quests = " + g + ".state.quests\n";
    s += "\tlocal facts = quests[id]\n";
    s += "\tif type(facts) ~= \"table\" then facts = {}; quests[id] = facts end\n";
    // The accept's clock reading stays with the first accept: a letter's writer withholds
    // "it is delivered" until the trip could have been made (the app's kLieHours).
    s += "\tif command == \"accept\" then\n";
    s += "\t\tfacts.accepted = true\n";
    s += "\t\tif facts.acceptedAt == nil then facts.acceptedAt = " + g + ".WorldTime() end\n";
    s += "\tend\n";
    s += "\tif command == \"handover\" then facts.handed = true end\n";
    s += "\tif command == \"complete\" or command == \"lie\" or command == \"keep\" or command == \"quiet\"\n";
    s += "\t\t\tor command == \"confess\" then\n";
    s += "\t\tfacts.accepted = true; facts.done = true\n";
    s += "\tend\n";
    // Given up to the giver's face (the app's give-up row): over, and the dump says how
    // (:d/abandoned), so the app plans the giver as somebody Henry let down.
    s += "\tif command == \"abandon\" then facts.accepted = true; facts.done = true; facts.how = \"abandoned\" end\n";
    s += "\tif command == \"seen\" then facts.seen = true end\n";
    // A beat's man down, a beat's or a mind's target dead, a pray knelt, a mind's visit made
    // (with the game's clock at the moment, for the next to be due against).
    s += "\tif command == \"beaten\" then facts.beaten = true end\n";
    s += "\tif command == \"target_dead\" then facts.dead = true end\n";
    s += "\tif command == \"prayed\" then facts.prayed = true end\n";
    // A leave's or a return's thing put down at the place, its cue heard from the dark; a
    // return's grave robbed (looted) and its curse laid (cursed, with the clock) and lifted.
    s += "\tif command == \"near\" then facts.near = true end\n";
    s += "\tif command == \"left\" then facts.left = true end\n";
    s += "\tif command == \"heard\" then facts.heard = true end\n";
    s += "\tif command == \"looted\" then facts.looted = true end\n";
    s += "\tif command == \"cursed\" then facts.cursed = true; facts.cursedAt = " + g + ".WorldTime() end\n";
    s += "\tif command == \"cured\" then facts.cursed = false; facts.cursedAt = nil; facts.cured = true end\n";
    // An ambush's directions given and its trap sprung.
    s += "\tif command == \"directed\" then facts.directed = true end\n";
    s += "\tif command == \"ambushed\" then facts.ambushed = true end\n";
    s += "\tif command == \"checked\" then\n";
    s += "\t\tfacts.checks = (facts.checks or 0) + 1\n";
    s += "\t\tfacts.checkedAt = " + g + ".WorldTime()\n";
    s += "\tend\n";
    s += "\tif command == \"broker\" then facts.broker = true end\n";
    s += "\tif command == \"apologise\" then facts.apologised = true end\n";
    s += "\tif command == \"refuse_apology\" then facts.refused = true end\n";
    // An ask's bribe taken or thrown back (QuestGift); the letters t and f.
    s += "\tif command == \"gift_took\" then facts.gift = \"took\" end\n";
    s += "\tif command == \"gift_refused\" then facts.gift = \"refused\" end\n";
    // An ask's word carried on: the giver told and paid, Henry sent on (the app's `onward`
    // directive; the letter w). The quest is not done until the one sent to is told.
    s += "\tif command == \"onward\" then facts.accepted = true; facts.onward = true end\n";
    // A find's keepsake in Henry's pocket, or out of it again (WatchKeepsake; the letter e).
    s += "\tif command == \"keepsake\" then facts.keepsake = true end\n";
    s += "\tif command == \"keepsake_gone\" then facts.keepsake = false end\n";
    // An unyielding holder's refusal (QuestRebuffed; the letter n), the giver's naming of
    // the one who can let Henry in (the app's `named` directive; a), the door bought (i)
    // and the key got (q) from her (QuestHelper); and the chest spawned and the thing taken
    // out of it (WatchChest, no letters: the app reads Henry's pocket).
    s += "\tif command == \"rebuffed\" then facts.rebuffed = true end\n";
    // The bedside's denouncing (QuestDenounced; the letter d): Henry walked out with nothing,
    // and the how is settled there, since the giver's report is the only complete left.
    s += "\tif command == \"denounced\" then facts.denounced = true; facts.how = \"denounced\" end\n";
    // The street's night (WatchFollow): the holder has caught Henry behind him, until the
    // morning clears it (no letter: the app has no part in it, and it is gone by day). The
    // clock of the sighting goes with it, for FollowMorning to measure the night by.
    // The night's marker comes off him with the sighting (the graph's Followed state carries
    // none), so the store says so without a token (WatchMarker); the row said by the mod
    // itself when his Talk never opened (FollowSaid) takes the global down.
    s += "\tif command == \"followed\" then facts.followed = true; facts.followedAt = " + g + ".WorldTime(); facts.marked = false end\n";
    s += "\tif command == \"followed_said\" then facts.followedSaid = true end\n";
    s += "\tif command == \"unfollowed\" then facts.followed = nil; facts.followedAt = nil; facts.followedSaid = nil end\n";
    // The street night's marker on him or off (WatchMarker; bugs.txt 28 Oct 2026).
    s += "\tif command == \"marked\" then facts.marked = true end\n";
    s += "\tif command == \"unmarked\" then facts.marked = false end\n";
    s += "\tif command == \"named\" then facts.accepted = true; facts.named = true end\n";
    s += "\tif command == \"letin\" then facts.letin = true end\n";
    s += "\tif command == \"keyed\" then facts.keyed = true end\n";
    s += "\tif command == \"chest\" then facts.chest = true end\n";
    s += "\tif command == \"taken\" then facts.taken = true end\n";
    // A dictated letter written: the three outcome digits (the flag W<digits>).
    s += "\tlocal wrote = string.match(command, \"^wrote_(%d+)$\")\n";
    s += "\tif wrote ~= nil then facts.accepted = true; facts.wrote = wrote end\n";
    s += "\t" + g + ".Log(\"BRIDGE state quest \" .. tostring(id) .. \" \" .. tostring(command) ..\n";
    s += "\t\t\" accepted=\" .. tostring(facts.accepted == true) .. \" handed=\" .. tostring(facts.handed == true) ..\n";
    s += "\t\t\" done=\" .. tostring(facts.done == true) .. \" flags=\" .. " + g + ".QuestFlagLetters(facts))\n";
    s += "\tpcall(" + g + ".QuestFlags)\n";
    s += "end\n";
    s += "\n";
    // The flags after a quest's stage letter on the dump, and how the app reads them:
    // b, Henry offered to broker; p, the giver apologised; r, he refused to; s, a travel's
    // one looked at has been seen; k, a beat's man is down; x, the target is dead; y, a pray
    // is knelt; one c per visit a mind has made; l, a leave's or a return's thing is put
    // down, h, its cue was heard; o, a return's grave was robbed, u, its curse is on him now;
    // g, an ambush's directions are given, m, its trap is sprung; w, an ask's giver has
    // been told and sent Henry on with the word; e, a find's keepsake is in Henry's pocket;
    // n, an unyielding holder has rebuffed Henry; a, the giver has named the one who can
    // let him in; i, she has let him in; q, she has got him the key; d, the bedside's holder
    // was denounced and Henry walked out with nothing; W followed by three
    // digits, a dictated letter is written, each digit a part's outcome (0 as said, 1 left
    // out, 2 softened, 3 made larger), last so the digits run to the end of the letters.
    s += "function " + g + ".QuestFlagLetters(facts)\n";
    s += "\treturn (facts.broker and \"b\" or \"\") .. (facts.apologised and \"p\" or \"\") .. (facts.refused and \"r\" or \"\")\n";
    s += "\t\t.. (facts.seen and \"s\" or \"\") .. (facts.beaten and \"k\" or \"\") .. (facts.dead and \"x\" or \"\")\n";
    s += "\t\t.. (facts.prayed and \"y\" or \"\") .. (facts.left and \"l\" or \"\") .. (facts.heard and \"h\" or \"\")\n";
    s += "\t\t.. (facts.looted and \"o\" or \"\") .. (facts.cursed and \"u\" or \"\") .. (facts.directed and \"g\" or \"\")\n";
    s += "\t\t.. (facts.ambushed and \"m\" or \"\") .. string.rep(\"c\", facts.checks or 0)\n";
    s += "\t\t.. (facts.gift == \"took\" and \"t\" or \"\") .. (facts.gift == \"refused\" and \"f\" or \"\")\n";
    s += "\t\t.. (facts.onward and \"w\" or \"\") .. (facts.keepsake and \"e\" or \"\")\n";
    s += "\t\t.. (facts.rebuffed and \"n\" or \"\") .. (facts.named and \"a\" or \"\")\n";
    s += "\t\t.. (facts.letin and \"i\" or \"\") .. (facts.keyed and \"q\" or \"\")\n";
    s += "\t\t.. (facts.denounced and \"d\" or \"\")\n";
    s += "\t\t.. (facts.wrote and (\"W\" .. facts.wrote) or \"\")\n";
    s += "end\n";
    s += "\n";
    // A dictated letter's Document class for what was written: the digits spelt into the
    // class (emit.cpp letter_class), so the Lua needs no table of the combinations.
    s += "function " + g + ".LetterClass(q, wrote)\n";
    s += "\tif q.letterPrefix == nil or wrote == nil then return q.letterBase or q.item end\n";
    s += "\treturn q.letterPrefix .. wrote .. q.letterSuffix\n";
    s += "end\n";
    s += "\n";
    // The receiver's verdict on a dictated letter, as the reading's global has it: 2 (mocked)
    // where any part was made larger, 1 (cursed) where a curse part went down as said, 0
    // (the prayer taken) otherwise. Nothing written reads as 0, which no verdict sequence
    // is reached with.
    s += "function " + g + ".DictationVerdict(q, wrote)\n";
    s += "\tif wrote == nil then return 0 end\n";
    s += "\tif string.find(wrote, \"3\", 1, true) then return 2 end\n";
    s += "\tfor _, k in ipairs(q.curseParts or {}) do\n";
    s += "\t\tif string.sub(wrote, k, k) == \"0\" then return 1 end\n";
    s += "\tend\n";
    s += "\treturn 0\n";
    s += "end\n";
    s += "\n";
    // The game's clock in seconds of world time, for a mind's hours between visits; the
    // real clock when the Calendar is not there to ask (a test harness), so nothing waits
    // for ever.
    s += "function " + g + ".WorldTime()\n";
    s += "\tlocal t = nil\n";
    s += "\tpcall(function() t = Calendar.GetWorldTime() end)\n";
    s += "\tif type(t) ~= \"number\" then return " + g + ".Now() end\n";
    s += "\treturn t\n";
    s += "end\n";
    s += "\n";
    // Whether a mind's next visit is due: the first at once, each after by the gap in
    // game hours since the last; none once the count is made.
    s += "function " + g + ".MindDue(id)\n";
    s += "\tlocal q = " + g + ".quests[id]\n";
    s += "\tlocal f = (" + g + ".state.quests or {})[id] or {}\n";
    s += "\tlocal made = f.checks or 0\n";
    s += "\tif q == nil or made >= (q.count or 1) then return false end\n";
    s += "\tif made == 0 or f.checkedAt == nil then return true end\n";
    s += "\treturn " + g + ".WorldTime() - f.checkedAt >= (q.gap or " + std::to_string(kMindGapHours) + ") * 3600\n";
    s += "end\n";
    s += "\n";
    // The holder's menu cannot read the store, only dialogue globals, so the two facts it
    // turns on are written out as globals whenever the store changes and after a load:
    // the broker flag, and whether the favour this one requires is done. From the store
    // rather than the other way round, so a save from before the flag existed reads as it
    // should, and so the app and the menu can never disagree about it.
    s += "function " + g + ".QuestFlags()\n";
    s += "\tlocal quests = " + g + ".state.quests or {}\n";
    s += "\tfor id, q in pairs(" + g + ".quests) do\n";
    s += "\t\tlocal f = quests[id] or {}\n";
    s += "\t\tif q.dispute then\n";
    s += "\t\t\tpcall(function() Variables.SetGlobal(q.brokerVar, f.broker and 1 or 0) end)\n";
    s += "\t\tend\n";
    s += "\t\tif q.requires ~= nil and q.requires ~= \"\" then\n";
    s += "\t\t\tlocal inner = quests[q.requires] or {}\n";
    s += "\t\t\tpcall(function() Variables.SetGlobal(q.innerVar, inner.done and 1 or 0) end)\n";
    s += "\t\tend\n";
    // A mind's menu reads the visits made and whether the next is due; the Watch keeps
    // the second fresh as the clock moves.
    s += "\t\tif q.kind == \"mind\" then\n";
    s += "\t\t\tpcall(function() Variables.SetGlobal(q.checksVar, f.checks or 0) end)\n";
    s += "\t\t\tpcall(function() Variables.SetGlobal(q.dueVar, " + g + ".MindDue(id) and 1 or 0) end)\n";
    s += "\t\tend\n";
    // An ambush's menu reads its stage: 0 before the directions, 1 after, 2 after the trap.
    s += "\t\tif q.kind == \"ambush\" then\n";
    s += "\t\t\tpcall(function() Variables.SetGlobal(q.stageVar, f.ambushed and 2 or f.directed and 1 or 0) end)\n";
    s += "\t\tend\n";
    // A buy's menu reads whether the giver's thing is on Henry; the Watch keeps it fresh.
    s += "\t\tif q.kind == \"buy\" then\n";
    s += "\t\t\tpcall(function() Variables.SetGlobal(q.carryVar, " + g + ".Count(" + g + ".Player(), q.carry) > 0 and 1 or 0) end)\n";
    s += "\t\tend\n";
    // A dictated letter: the receiver's reading reads each part's outcome and the verdict,
    // and the quest's item is the Document actually written, so the begin line's count and
    // the handover move the right class (nothing of the base class is ever made).
    s += "\t\tif q.dictated then\n";
    s += "\t\t\tfor k, var in ipairs(q.partVars or {}) do\n";
    s += "\t\t\t\tlocal digit = f.wrote and tonumber(string.sub(f.wrote, k, k)) or 0\n";
    s += "\t\t\t\tpcall(function() Variables.SetGlobal(var, digit) end)\n";
    s += "\t\t\tend\n";
    s += "\t\t\tpcall(function() Variables.SetGlobal(q.verdictVar, " + g + ".DictationVerdict(q, f.wrote)) end)\n";
    s += "\t\t\tq.item = " + g + ".LetterClass(q, f.wrote)\n";
    s += "\t\tend\n";
    // An exposure's menu reads whether the holder has been caught (the expose row); and a
    // retrieve of the letter an earlier dictation wrote takes its class from that quest's
    // digits, so the handover moves the very Document the holder was given.
    s += "\t\tif q.seenVar ~= nil then\n";
    s += "\t\t\tpcall(function() Variables.SetGlobal(q.seenVar, f.seen and 1 or 0) end)\n";
    s += "\t\tend\n";
    s += "\t\tif q.denouncedVar ~= nil then\n";
    s += "\t\t\tpcall(function() Variables.SetGlobal(q.denouncedVar, f.denounced and 1 or 0) end)\n";
    s += "\t\tend\n";
    s += "\t\tif q.followedVar ~= nil then\n";
    s += "\t\t\tpcall(function() Variables.SetGlobal(q.followedVar, (f.followed and not f.followedSaid) and 1 or 0) end)\n";
    s += "\t\tend\n";
    s += "\t\tif q.letterOf ~= nil then\n";
    s += "\t\t\tlocal of, off = " + g + ".quests[q.letterOf], quests[q.letterOf] or {}\n";
    s += "\t\t\tif of ~= nil and of.dictated and off.wrote ~= nil then q.item = " + g + ".LetterClass(of, off.wrote) end\n";
    s += "\t\tend\n";
    s += "\tend\n";
    s += "\t" + g + ".flagsSynced = true\n";
    s += "end\n";
    s += g + ".flagsSynced = false\n";
    s += "\n";
    // Whether a giver's favour can be taken up now: neither accepted nor done, and, on a
    // chain, the link before it done. What the hint and the app's director both mean by
    // "offerable"; the app decides it for itself from the dump, this is the mod's copy.
    // Whether the player can pray at a shrine at all: the game gates its praying spots on
    // Basic Theology, and a pray is not asked of somebody who could not do it (the app is
    // told on the begin line, pray=0|1, and withholds the offer; the hint stays off here).
    s += "function " + g + ".CanPray()\n";
    s += "\tlocal can = false\n";
    s += "\tpcall(function() can = " + g + ".Player().soul:HasPerk(\"" + std::string(kPrayPerk) + "\", false) == true end)\n";
    s += "\treturn can\n";
    s += "end\n";
    s += "\n";
    s += "function " + g + ".QuestOfferable(id)\n";
    s += "\tlocal q = " + g + ".quests[id]\n";
    s += "\tif q == nil then return false end\n";
    s += "\tlocal f = " + g + ".state.quests[id] or {}\n";
    s += "\tif f.accepted or f.done then return false end\n";
    s += "\tif q.kind == \"pray\" and not " + g + ".CanPray() then return false end\n";
    s += "\tif q.chain ~= nil and q.chain ~= \"\" and q.order > 1 then\n";
    s += "\t\tfor other, oq in pairs(" + g + ".quests) do\n";
    s += "\t\t\tif oq.chain == q.chain and oq.order == q.order - 1 then\n";
    s += "\t\t\t\tlocal of = " + g + ".state.quests[other] or {}\n";
    s += "\t\t\t\tif not of.done then return false end\n";
    s += "\t\t\tend\n";
    s += "\t\tend\n";
    s += "\tend\n";
    s += "\tif q.after ~= nil and q.after ~= \"\" and " + g + ".QuestLinkState(id) ~= \"open\" then return false end\n";
    s += "\treturn true\n";
    s += "end\n";
    s += "\n";
    // A scene's link: locked while the favour before it is not done; void when it is done
    // but ended a way the link does not follow (the scene is never offered); open otherwise.
    // How a favour ended is its how in the store -- the holder menu's branch, or the giver's
    // lie/kept/quiet/dead ending, or the receiver's interrogation dodged (pinned, covered,
    // fibbed, evaded, caught) -- and "truth" is any ending that is none of those (npcdb.hpp
    // untrue_how): the plain complete, persuaded, threatened, paid, stolen, brokered,
    // nested, fought.
    s += "function " + g + ".QuestHowMatches(how, wanted)\n";
    s += "\tif wanted == nil or wanted == \"\" then return true end\n";
    s += "\thow = how or \"\"\n";
    s += "\tlocal untrue = { lied = true, quiet = true, kept = true, dead = true,\n";
    s += "\t\tpinned = true, covered = true, fibbed = true, evaded = true, caught = true }\n";
    s += "\tfor token in string.gmatch(wanted, \"[^|]+\") do\n";
    s += "\t\tif token == how then return true end\n";
    s += "\t\tif token == \"truth\" and not untrue[how] then\n";
    s += "\t\t\treturn true\n";
    s += "\t\tend\n";
    s += "\tend\n";
    s += "\treturn false\n";
    s += "end\n";
    s += "\n";
    s += "function " + g + ".QuestLinkState(id)\n";
    s += "\tlocal q = " + g + ".quests[id]\n";
    s += "\tif q == nil or q.after == nil or q.after == \"\" then return \"open\" end\n";
    s += "\tlocal before = " + g + ".state.quests[q.after] or {}\n";
    s += "\tif not before.done then return \"locked\" end\n";
    s += "\tif " + g + ".QuestHowMatches(before.how, q.afterHow) then return \"open\" end\n";
    s += "\treturn \"void\"\n";
    s += "end\n";
    s += "\n";
    // Text on a log line. Backslash first, then the line breaks a log line cannot carry,
    // then the marker itself, broken so that nothing a player typed can read as one of
    // the mod's own lines to the app. The app undoes exactly these four.
    s += "function " + g + ".Escape(text)\n";
    s += "\ttext = string.gsub(tostring(text), \"\\\\\", \"\\\\\\\\\")\n";
    s += "\ttext = string.gsub(text, \"\\n\", \"\\\\n\")\n";
    s += "\ttext = string.gsub(text, \"\\r\", \"\\\\r\")\n";
    s += "\ttext = string.gsub(text, \"BRIDGE\", \"BR\\\\IDGE\")\n";
    s += "\treturn text\n";
    s += "end\n";
    s += "\n";
    // The soul's record to the app, before the begin line so that the app has it when
    // the conversation opens and has nothing to wait for. One line per entry rather than
    // a packed chunk: entries are short (a spoken line, a paragraph of summary), a line
    // carries 3,855 bytes, and one entry per line is one parser. The count is on the
    // header and the end line says the run is over; the app opens on either.
    // `ahead` is a gap entry not yet in the record: the one StateBegin would append if the
    // player pressed now. With it, the dump is the record as the begin's dump would be --
    // met counting the conversation, the entry closing the list -- and nothing is stored;
    // the app builds the same sheet from it that the begin will bring, which is what lets
    // the opening line be guessed on a look (Look below) and then said on the press.
    s += g + ".memoryLine = 3000\n";
    s += "function " + g + ".Dump(id, ahead)\n";
    // Peeked rather than made: a look at a soul is not a reason to open a record on them
    // in the store, so a soul not yet in it is dumped as the empty record they would be.
    s += "\tlocal soul = ahead ~= nil and (" + g + ".state.souls[id] or { met = 0, rev = 0, memory = {}, stats = {} })\n";
    s += "\t\tor " + g + ".Soul(id)\n";
    s += "\tif ahead ~= nil then\n";
    s += "\t\tlocal memory = {}\n";
    s += "\t\tfor i = 1, #(soul.memory or {}) do memory[i] = soul.memory[i] end\n";
    s += "\t\tmemory[#memory + 1] = ahead\n";
    s += "\t\tsoul = { met = (tonumber(soul.met) or 0) + 1, rev = soul.rev or 0, memory = memory,\n";
    s += "\t\t\tstats = soul.stats or {}, first = soul.first or ahead.t, last = ahead.t }\n";
    s += "\tend\n";
    s += "\tlocal facts = \"\"\n";
    s += "\tlocal questId = " + g + ".questBySoul ~= nil and " + g + ".questBySoul[id] or nil\n";
    s += "\tif questId ~= nil then\n";
    s += "\t\tlocal f = " + g + ".state.quests[questId] or {}\n";
    s += "\t\tfacts = \" quest=\" .. questId .. \" accepted=\" .. tostring(f.accepted == true) ..\n";
    s += "\t\t\t\" handed=\" .. tostring(f.handed == true) .. \" done=\" .. tostring(f.done == true)\n";
    s += "\tend\n";
    // Every quest the save has moved, by its furthest stage, so the app can tell whether a
    // favour this soul refers Henry to -- somebody else's -- is still there to be taken up.
    // Absent means untouched, which is what most of them are.
    // After the stage letter, the flags (QuestFlagLetters): a dispute's broker offer and
    // the giver's answer to it. The app reads the first letter as the stage and the rest
    // as flags, so a mod from before the flags reads the same. Then, after a slash, how
    // the favour ended when the store knows (/persuaded, /lied ...), which is what a
    // later scene's link turns on; the app reads it and gates its own offers by it. Last,
    // after a tilde, the game hours since the accept when the store has the clock reading
    // (memory.cpp reads it off first; a store from before it has none, and the app
    // withholds nothing for it).
    s += "\tlocal all = \"\"\n";
    s += "\tlocal now = " + g + ".WorldTime()\n";
    s += "\tfor qid, f in pairs(" + g + ".state.quests or {}) do\n";
    s += "\t\tif type(qid) == \"string\" and type(f) == \"table\" then\n";
    s += "\t\t\tlocal stage = (f.done and \"d\") or (f.handed and \"h\") or (f.accepted and \"a\") or nil\n";
    s += "\t\t\tif stage ~= nil then\n";
    s += "\t\t\t\tall = all .. (all == \"\" and \" quests=\" or \",\") .. qid .. \":\" .. stage .. " + g + ".QuestFlagLetters(f)\n";
    s += "\t\t\t\t\t.. (f.how ~= nil and (\"/\" .. tostring(f.how)) or \"\")\n";
    s += "\t\t\t\t\t.. ((type(f.acceptedAt) == \"number\" and type(now) == \"number\")\n";
    s += "\t\t\t\t\t\tand string.format(\"~%.1f\", math.max(0, now - f.acceptedAt) / 3600) or \"\")\n";
    s += "\t\t\tend\n";
    s += "\t\tend\n";
    s += "\tend\n";
    s += "\tfacts = facts .. all\n";
    s += "\tlocal stats = \"\"\n";
    s += "\tfor name, value in pairs(soul.stats) do\n";
    s += "\t\tif type(name) == \"string\" and type(value) == \"number\" then\n";
    s += "\t\t\tstats = stats .. \" stat:\" .. name .. \"=\" .. string.format(\"%.3f\", value)\n";
    s += "\t\tend\n";
    s += "\tend\n";
    s += "\tlocal n = #soul.memory\n";
    s += "\t" + g + ".Log(\"BRIDGE state npc=\" .. id .. \" met=\" .. " + g + ".Whole(soul.met) ..\n";
    s += "\t\t\" first=\" .. " + g + ".Whole(soul.first) .. \" last=\" .. " + g + ".Whole(soul.last) ..\n";
    s += "\t\t\" rev=\" .. " + g + ".Whole(soul.rev) .. \" entries=\" .. n .. facts .. stats)\n";
    // Who Henry knows of, for the questions the app may offer about people: every soul
    // the save has him meeting, by id, and everyone an NPC has named in his hearing
    // (state.known, written by @heard). On lines of their own after the header, as many
    // as it takes under the log's line length, since a long game knows hundreds.
    s += "\tlocal known = \"\"\n";
    s += "\tlocal function knownAdd(tok)\n";
    s += "\t\tif #known + #tok + 1 > " + g + ".memoryLine then\n";
    s += "\t\t\t" + g + ".Log(\"BRIDGE known npc=\" .. id .. known)\n";
    s += "\t\t\tknown = \"\"\n";
    s += "\t\tend\n";
    s += "\t\tknown = known .. \" \" .. tok\n";
    s += "\tend\n";
    s += "\tfor sid, rec in pairs(" + g + ".state.souls) do\n";
    s += "\t\tif type(sid) == \"string\" and type(rec) == \"table\" and (tonumber(rec.met) or 0) > 0 then knownAdd(sid) end\n";
    s += "\tend\n";
    s += "\tfor tok, on in pairs(" + g + ".state.known or {}) do\n";
    s += "\t\tif type(tok) == \"string\" and on and (" + g + ".state.souls[tok] == nil or (tonumber(" + g + ".state.souls[tok].met) or 0) == 0) then knownAdd(tok) end\n";
    s += "\tend\n";
    s += "\tif known ~= \"\" then " + g + ".Log(\"BRIDGE known npc=\" .. id .. known) end\n";
    s += "\tfor i = 1, n do\n";
    s += "\t\tlocal e = soul.memory[i]\n";
    s += "\t\t" + g + ".Log(\"BRIDGE memory npc=\" .. id .. \" \" .. i .. \"/\" .. n .. \" k=\" .. tostring(e.k) ..\n";
    s += "\t\t\t\" t=\" .. " + g + ".Whole(e.t) .. \" s=\" .. " + g + ".Escape(string.sub(tostring(e.s or \"\"), 1, " + g + ".memoryLine)))\n";
    s += "\tend\n";
    s += "\t" + g + ".Log(\"BRIDGE memory npc=\" .. id .. \" end\")\n";
    s += "end\n";
    s += "\n";
    // A conversation opening: met, first and last, the gap entry that carries this
    // conversation's time and scene (the same key=value readings the begin line has, so
    // the app reads them with the one parser it already owns), then the dump. The words
    // that follow are appended as they are said, by StateSaid and StateReply, to whoever
    // this set as the open soul.
    s += g + ".stateSoul = nil\n";
    s += "function " + g + ".StateBegin(id, scene)\n";
    s += "\tlocal soul = " + g + ".Soul(id)\n";
    s += "\tsoul.met = soul.met + 1\n";
    s += "\tlocal now = " + g + ".WorldTime()\n";
    s += "\tif soul.first == nil then soul.first = now end\n";
    s += "\tsoul.last = now\n";
    s += "\t" + g + ".StateRemember(id, { k = \"gap\", t = now, s = scene or \"\" })\n";
    s += "\t" + g + ".stateSoul = id\n";
    s += "\t" + g + ".Dump(id)\n";
    s += "end\n";
    s += "\n";
    s += "function " + g + ".StateSaid(line)\n";
    s += "\tif " + g + ".stateSoul == nil or line == nil or line == \"\" then return end\n";
    s += "\t" + g + ".StateRemember(" + g + ".stateSoul, { k = \"p\", s = line })\n";
    s += "end\n";
    s += "\n";
    s += "function " + g + ".StateReply(words)\n";
    s += "\tif " + g + ".stateSoul == nil or words == nil or words == \"\" then return end\n";
    s += "\t" + g + ".StateRemember(" + g + ".stateSoul, { k = \"n\", s = words })\n";
    s += "end\n";
    s += "\n";
    s += "function " + g + ".StateEnd()\n";
    s += "\t" + g + ".stateSoul = nil\n";
    s += "end\n";
    s += "\n";
    // Compaction, asked for by the app (plan.md section 6): the first n entries become one
    // sum entry, dated from the first of them. rev is the soul's revision the app saw on
    // the dump; a request against another revision is a stale view and is refused, so a
    // race can only lose a compaction, never a line. The prefix is what is compacted, so
    // the request may arrive during a conversation with the same soul and still be right.
    s += "function " + g + ".Compact(id, rev, n, summary)\n";
    s += "\tlocal soul = " + g + ".state.souls[id]\n";
    s += "\tn = tonumber(n)\n";
    s += "\trev = tonumber(rev)\n";
    s += "\tif soul == nil or n == nil or rev == nil or rev ~= soul.rev or n < 1 or n > #soul.memory\n";
    s += "\t\t\tor type(summary) ~= \"string\" or summary == \"\" then\n";
    s += "\t\t" + g + ".Log(\"BRIDGE state compact refused npc=\" .. tostring(id) .. \" rev=\" .. tostring(rev) ..\n";
    s += "\t\t\t\" have=\" .. tostring(soul and soul.rev) .. \" n=\" .. tostring(n) .. \" entries=\" .. tostring(soul and #soul.memory))\n";
    s += "\t\treturn false\n";
    s += "\tend\n";
    s += "\tif #summary > " + g + ".stateChunk then summary = string.sub(summary, 1, " + g + ".stateChunk) end\n";
    s += "\tlocal kept = { { k = \"sum\", t = soul.memory[1].t, s = summary } }\n";
    s += "\tfor i = n + 1, #soul.memory do kept[#kept + 1] = soul.memory[i] end\n";
    s += "\tsoul.memory = kept\n";
    s += "\tsoul.rev = soul.rev + 1\n";
    s += "\t" + g + ".Log(\"BRIDGE state compacted npc=\" .. id .. \" n=\" .. n .. \" rev=\" .. soul.rev ..\n";
    s += "\t\t\" entries=\" .. #soul.memory .. \" bytes=\" .. string.format(\"%d\", " + g + ".Bytes(soul.memory)))\n";
    s += "\treturn true\n";
    s += "end\n";
    s += "\n";
    // A derived stat the app worked out after a conversation (plan.md section 8).
    s += "function " + g + ".StateStat(id, name, value)\n";
    s += "\tvalue = tonumber(value)\n";
    s += "\tif value == nil or name == nil or name == \"\" then return false end\n";
    s += "\t" + g + ".Soul(id).stats[name] = value\n";
    s += "\t" + g + ".Log(\"BRIDGE state stat npc=\" .. id .. \" \" .. name .. \"=\" .. string.format(\"%.3f\", value))\n";
    s += "\treturn true\n";
    s += "end\n";
    s += "\n";
    // The spike. A synthetic table with every shape the real store will have -- one long
    // string, many short ones, nesting, a number and a boolean -- put under the store so
    // it goes through the same door. The checksum is over samples rather than every byte,
    // so a hundred-megabyte blob is checked in the time a frame takes: what it catches is
    // truncation, a boundary gone wrong and a string swapped for another, which is what a
    // serializer that fails will do. The marker is whatever the run wants to see come back.
    s += "function " + g + ".Checksum(str)\n";
    s += "\tlocal n = #str\n";
    s += "\tif n == 0 then return \"0:0\" end\n";
    s += "\tlocal stride = math.max(1, math.floor(n / 65536))\n";
    s += "\tlocal sum = 0\n";
    s += "\tfor i = 1, n, stride do\n";
    s += "\t\tsum = (sum * 31 + string.byte(str, i)) % 2147483647\n";
    s += "\tend\n";
    s += "\treturn string.format(\"%d:%d\", n, sum)\n";
    s += "end\n";
    s += "\n";
    // The save's serializer writes a string's length in two bytes: a 64,512-byte string
    // came back whole and a 65,536-byte one made the whole save unloadable ("Game load
    // failed", nothing in kcd.log), measured on the Windows laptop with a probe script. So no string
    // that goes in the save may reach 65,536 bytes, and the store keeps long text as
    // a list of pieces of at most stateChunk bytes.
    s += g + ".stateChunk = 61440\n";
    s += "\n";
    s += "function " + g + ".Chunk(str)\n";
    s += "\tlocal pieces = {}\n";
    s += "\tlocal size = " + g + ".stateChunk\n";
    s += "\tfor i = 1, math.max(#str, 1), size do\n";
    s += "\t\tpieces[#pieces + 1] = string.sub(str, i, i + size - 1)\n";
    s += "\tend\n";
    s += "\treturn pieces\n";
    s += "end\n";
    s += "\n";
    // 1 KB is exactly 64 repeats of a 16-character run, so the length is easy to read.
    // Varied rather than one repeated character so a compressor cannot make it vanish
    // and hide the real cost. The blob is made a chunk at a time and never joined: a
    // 32 MB string came out of Chunk() 272 bytes longer than it went in, which is what
    // string.sub does when its positions pass 2^24 and the number type cannot hold
    // them exactly; so nothing here indexes past a chunk, and the store will not either.
    s += "function " + g + ".SpikeKilo()\n";
    s += "\tlocal unit = \"0123456789abcdef\"\n";
    s += "\tlocal parts = {}\n";
    s += "\tfor i = 1, 64 do\n";
    s += "\t\tparts[i] = string.sub(unit, (i % 16) + 1) .. string.sub(unit, 1, i % 16)\n";
    s += "\tend\n";
    s += "\treturn table.concat(parts)\n";
    s += "end\n";
    s += "\n";
    // kb kilobytes as pieces of stateChunk bytes (60 KB each), the last one shorter.
    s += "function " + g + ".SpikeChunks(kb)\n";
    s += "\tlocal kilo = " + g + ".SpikeKilo()\n";
    s += "\tlocal perChunk = math.floor(" + g + ".stateChunk / 1024)\n";
    s += "\tlocal whole = {}\n";
    s += "\tfor i = 1, perChunk do whole[i] = kilo end\n";
    s += "\tlocal full = table.concat(whole)\n";
    s += "\tlocal chunks = {}\n";
    s += "\tlocal left = kb or 0\n";
    s += "\twhile left > 0 do\n";
    s += "\t\tif left >= perChunk then\n";
    s += "\t\t\tchunks[#chunks + 1] = full\n";
    s += "\t\t\tleft = left - perChunk\n";
    s += "\t\telse\n";
    s += "\t\t\tchunks[#chunks + 1] = string.sub(full, 1, left * 1024)\n";
    s += "\t\t\tleft = 0\n";
    s += "\t\tend\n";
    s += "\tend\n";
    s += "\treturn chunks\n";
    s += "end\n";
    s += "\n";
    // One checksum over a list of pieces: each piece's own, folded together, with the
    // piece count and the summed length in front. Every number stays under 2^24 except
    // the total, which is only reported.
    s += "function " + g + ".ChunksSum(chunks)\n";
    s += "\tlocal total, fold = 0, 0\n";
    s += "\tfor i = 1, #chunks do\n";
    s += "\t\tlocal piece = chunks[i]\n";
    s += "\t\ttotal = total + #piece\n";
    s += "\t\tlocal _, sum = string.match(" + g + ".Checksum(piece), \"^(%d+):(%d+)$\")\n";
    s += "\t\tfold = (fold * 7 + (tonumber(sum) or 0) % 1000003 + #piece) % 16777213\n";
    s += "\tend\n";
    s += "\treturn string.format(\"%d:%d:%d\", #chunks, total, fold)\n";
    s += "end\n";
    s += "\n";
    s += "function " + g + ".SpikeFill(kb, count, marker)\n";
    s += "\tlocal t0 = " + g + ".Now()\n";
    s += "\tlocal chunks = " + g + ".SpikeChunks(kb)\n";
    s += "\tlocal many = {}\n";
    s += "\tfor i = 1, (count or 0) do many[i] = \"line \" .. i .. \" of the spike\" end\n";
    s += "\t" + g + ".state.spike = {\n";
    s += "\t\tmarker = tostring(marker or \"none\"),\n";
    s += "\t\tchunks = chunks,\n";
    s += "\t\tsum = " + g + ".ChunksSum(chunks),\n";
    s += "\t\tmany = many,\n";
    s += "\t\tcount = #many,\n";
    s += "\t\tnested = { a = { b = { c = { deep = \"deep\", n = 42, yes = true } } } },\n";
    s += "\t}\n";
    s += "\t" + g + ".Log(\"BRIDGE spike filled marker=\" .. tostring(marker) .. \" kb=\" .. tostring(kb) ..\n";
    s += "\t\t\" sum=\" .. " + g + ".state.spike.sum .. \" count=\" .. #many .. \" bytes=\" .. string.format(\"%d\", " + g + ".Bytes(" + g + ".state.spike)) ..\n";
    s += "\t\t\" ms=\" .. string.format(\"%.1f\", (" + g + ".Now() - t0) * 1000))\n";
    s += "end\n";
    s += "\n";
    s += "function " + g + ".SpikeCheck()\n";
    s += "\tlocal t0 = " + g + ".Now()\n";
    s += "\tlocal sp = " + g + ".state.spike\n";
    s += "\tif type(sp) ~= \"table\" then\n";
    s += "\t\t" + g + ".Log(\"BRIDGE spike check: no spike in the store\")\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    s += "\tlocal sum = " + g + ".ChunksSum(type(sp.chunks) == \"table\" and sp.chunks or {})\n";
    s += "\tlocal deep = nil\n";
    s += "\tpcall(function() deep = sp.nested.a.b.c end)\n";
    s += "\t" + g + ".Log(\"BRIDGE spike check marker=\" .. tostring(sp.marker) ..\n";
    s += "\t\t\" sum=\" .. sum .. \" stored=\" .. tostring(sp.sum) ..\n";
    s += "\t\t\" intact=\" .. tostring(sum == sp.sum) ..\n";
    s += "\t\t\" many=\" .. " + g + ".Tally(sp.many) .. \"/\" .. tostring(sp.count) ..\n";
    s += "\t\t\" last=\" .. tostring(type(sp.many) == \"table\" and sp.many[sp.count or 0] or nil) ..\n";
    s += "\t\t\" deep=\" .. tostring(deep and deep.deep) .. \"/\" .. tostring(deep and deep.n) ..\n";
    s += "\t\t\"/\" .. tostring(deep and deep.yes) .. \" ms=\" .. string.format(\"%.1f\", (" + g + ".Now() - t0) * 1000))\n";
    s += "end\n";
    s += "\n";
    // How long a log line survives. The chunk size for shipping the store to the app is
    // set from the answer, so this says its own length up front and ends in a word the
    // harness can look for.
    s += "function " + g + ".LongLine(n)\n";
    s += "\t" + g + ".Log(\"BRIDGE longline n=\" .. n .. \" \" .. string.rep(\"x\", n) .. \" END\")\n";
    s += "end\n";
    s += "\n";
    s += "-- ---------------------------------------------------------------------------\n";
    s += "-- The autopilot: a run of the game driven from a file, for tests that need a save\n";
    s += "-- written and read back with nobody at the keyboard.\n";
    s += "--\n";
    s += "-- autopilot.txt beside the turn files is Lua, read once when the mod is installed,\n";
    s += "-- which is while the main menu is up. It usually calls " + g + ".Run with a list of\n";
    s += "-- steps; each runs when the one before has finished, and a step that raises is\n";
    s += "-- logged and skipped so a run always reaches its quit. The file is the player's own,\n";
    s += "-- under their saves, the same standing as the hook files.\n";
    s += g + ".auto = { steps = {}, at = 0, waited = 0 }\n";
    s += "function " + g + ".Run(steps)\n";
    s += "\t" + g + ".auto.steps = steps or {}\n";
    s += "\t" + g + ".auto.at = 0\n";
    s += "\t" + g + ".AutoNext()\n";
    s += "end\n";
    s += "\n";
    s += "function " + g + ".AutoNext()\n";
    s += "\tlocal a = " + g + ".auto\n";
    s += "\ta.at = a.at + 1\n";
    s += "\tlocal step = a.steps[a.at]\n";
    s += "\tif step == nil then\n";
    s += "\t\ta.finished = true\n";
    s += "\t\t" + g + ".Log(\"BRIDGE auto done\")\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    s += "\t" + g + ".Log(\"BRIDGE auto step \" .. a.at .. \" \" .. tostring(step[1]))\n";
    // The nudge map, if a waitloaded armed it, comes down here rather than at the moment
    // the world was seen (see AutoPoll); a load step arms it again.
    s += "\tpcall(" + g + ".AutoDisarmNudge)\n";
    s += "\tlocal ok, err = pcall(" + g + ".AutoStep, step)\n";
    s += "\tif not ok then\n";
    s += "\t\t" + g + ".Log(\"BRIDGE auto step \" .. a.at .. \" raised \" .. tostring(err))\n";
    s += "\t\t" + g + ".AutoAfter(100)\n";
    s += "\tend\n";
    s += "end\n";
    s += "\n";
    // Time in the autopilot is a due moment, not a timer. Script timers armed before a
    // world or while one is loading are wiped when the loading ends (measured: none of
    // them ever came back), so a step sets when it is due and the heartbeat moves the
    // run on when that moment has passed. Before a world there is no heartbeat either,
    // and nothing to wait for, so the run goes straight on -- unless it has just asked
    // for a load, when the world arriving is what carries it.
    // The autopilot's clock is its own pulse (below): a half-second timer chain, so a due
    // moment is a tick count. Two clocks were tried before it and both lied. Now()
    // (System.GetCurrAsyncTime) reads as seconds since the level loaded inside a
    // Script.SetTimer callback and as seconds since the process started elsewhere
    // (measured: dueAt=190.4 against now=84.2, fourteen seconds apart in real time), so a
    // due moment set in one place never came in the other. os.time() then, whole seconds
    // from one source -- except that this engine's Lua keeps its numbers in single
    // precision, and at 1.79e9 a float has a step of 128: os.time() held one value for up
    // to two minutes and then jumped (logged as 1.79054e+09). A poll gated on "a second
    // since the last" waited for the jump, which is where the twenty to ninety seconds of
    // Henry sitting on his horse after every load went; and a settle of twenty seconds,
    // now + 20 rounded back to now, took none.
    //
    // So: due in ticks of the pulse; and a timer as well, for when timers run, which is
    // the finer clock -- it marks the moment due itself rather than reading any clock.
    s += "function " + g + ".AutoAfter(ms)\n";
    s += "\tlocal a = " + g + ".auto\n";
    s += "\tlocal loaded, why = " + g + ".AutoLoaded()\n";
    s += "\tif not loaded and not a.awaitLoad then\n";
    s += "\t\t" + g + ".Log(\"BRIDGE auto no heartbeat before a world (\" .. tostring(why) .. \"), going straight on\")\n";
    s += "\t\t" + g + ".AutoNext()\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    s += "\ta.dueAt = (a.pulseN or 0) + math.max(1, math.ceil(ms / 500))\n";
    s += "\ta.dueNow = false\n";
    s += "\ta.token = (a.token or 0) + 1\n";
    s += "\tlocal token = a.token\n";
    s += "\tpcall(function() Script.SetTimer(ms, function() if a.token == token then a.dueNow = true " + g + ".AutoHeartbeat(\"timer\") end end, nil, true) end)\n";
    s += "end\n";
    s += "\n";
    // The heartbeat: called from wherever the engine gives the mod control in a world --
    // the player's per-frame OnUpdate, OnAction, GetActions, a timer. Cheap when there is
    // nothing due. The first call from each source is logged, so the log says which of
    // them the engine actually makes.
    s += g + ".heartbeatSources = {}\n";
    s += "function " + g + ".AutoHeartbeat(source)\n";
    s += "\tlocal a = " + g + ".auto\n";
    s += "\tif a == nil then return end\n";
    s += "\tif source ~= nil and not " + g + ".heartbeatSources[source] then\n";
    s += "\t\t" + g + ".heartbeatSources[source] = true\n";
    s += "\t\tif a.steps ~= nil and #a.steps > 0 then " + g + ".Log(\"BRIDGE auto heartbeat from \" .. source) end\n";
    s += "\tend\n";
    // A pulse of its own once there is a world, since the engine's own calls are not
    // to be relied on for one: GetActions comes only while the player is looking at
    // something usable, OnUpdate never came at all, and a run whose player faced a wall
    // sat at its settle step for three minutes until the runner killed the game. Timers
    // armed after the world has arrived survive (the conversation pump runs on them),
    // so the first heartbeat from a loaded world arms a half-second chain that re-arms
    // itself until the run is done.
    //
    // Armed once, and armed again when it has died: the first heartbeat of a loaded world
    // comes while the save is still streaming in (OnLoad, the player entity up and the
    // engine not yet saying it is loading), and a timer armed then is wiped with the rest
    // when the loading ends (measured: "pulse dead at tick 0" on every load). Whether it is
    // alive is read off its own tick count: a heartbeat from anywhere else that finds the
    // count where the last such heartbeat left it, two seconds or more later on the async
    // clock (both readings from outside a timer, so from the one clock), with the world
    // loaded, has caught a dead pulse. Those other heartbeats are the runner's F9 nudges,
    // so a wiped pulse costs one nudge's wait, not a run's. Each chain carries its
    // generation and a chain of an older one stops itself: without that, a heartbeat that
    // came thirty times in one frame (a key's OnAction) armed thirty chains, and the tick
    // count ran to 36000 in a three-minute run.
    s += "\tif source ~= \"pulse\" and a.pulsing and " + g + ".AutoLoaded() then\n";
    s += "\t\tlocal t = " + g + ".Now()\n";
    s += "\t\tif a.pulseSeenAt == nil or t - a.pulseSeenAt >= 2 then\n";
    s += "\t\t\tif a.pulseSeenAt ~= nil and a.pulseSeen == a.pulseN then\n";
    s += "\t\t\t\ta.pulsing = false\n";
    s += "\t\t\t\t" + g + ".Log(\"BRIDGE auto pulse dead at tick \" .. tostring(a.pulseN) .. \" (\" .. tostring(source) .. \"), arming again\")\n";
    s += "\t\t\tend\n";
    s += "\t\t\ta.pulseSeen = a.pulseN\n";
    s += "\t\t\ta.pulseSeenAt = t\n";
    s += "\t\tend\n";
    s += "\tend\n";
    s += "\tif not a.pulsing and a.steps ~= nil and #a.steps > 0 and not a.finished and " + g + ".AutoLoaded() then\n";
    s += "\t\ta.pulsing = true\n";
    s += "\t\ta.pulseN = a.pulseN or 0\n";
    s += "\t\ta.pulseSeen = a.pulseN\n";
    s += "\t\ta.pulseSeenAt = nil\n";
    s += "\t\ta.pulseGen = (a.pulseGen or 0) + 1\n";
    s += "\t\tlocal gen = a.pulseGen\n";
    s += "\t\tlocal function pulse()\n";
    s += "\t\t\tif a.pulseGen ~= gen then return end\n";
    s += "\t\t\tif a.finished then a.pulsing = false return end\n";
    s += "\t\t\ta.pulseN = a.pulseN + 1\n";
    s += "\t\t\t" + g + ".AutoHeartbeat(\"pulse\")\n";
    s += "\t\t\tpcall(function() Script.SetTimer(500, pulse) end)\n";
    s += "\t\tend\n";
    s += "\t\tlocal armed = pcall(function() Script.SetTimer(500, pulse) end)\n";
    s += "\t\tif not armed then a.pulsing = false end\n";
    s += "\tend\n";
    s += "\tif a.dueAt ~= nil and (a.dueNow or (a.pulseN or 0) >= a.dueAt) then\n";
    s += "\t\ta.dueAt = nil\n";
    s += "\t\ta.dueNow = false\n";
    s += "\t\ta.token = (a.token or 0) + 1\n";
    s += "\t\t" + g + ".AutoNext()\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    // The poll for a world rides the pulse -- every other tick, a second -- and any
    // heartbeat from elsewhere polls too, since those are few (the world's own hooks and
    // the runner's nudges). It was gated on a second of os.time since the last poll, and
    // with os.time stepping by 128 (see AutoAfter) a run's polls were two: the arriving
    // world's, and whichever heartbeat came after the step, twenty to ninety seconds on
    // (measured over eleven passes: "loaded after 2s" every time).
    s += "\tif a.polling and (source ~= \"pulse\" or a.pulseN % 2 == 0) then\n";
    s += "\t\t" + g + ".AutoPoll(source)\n";
    s += "\tend\n";
    // An until step's wait is counted on the pulse alone, a second every other tick: the
    // other heartbeats come as often as the engine likes (GetActions every frame with a
    // usable thing in view) and counted a 900 s wait out in seconds on the first sweep.
    // They still look at the flag, so a road's end is seen at once.
    s += "\tif a.untilFlag ~= nil then\n";
    s += "\t\tif source == \"pulse\" then\n";
    s += "\t\t\tif a.pulseN % 2 == 0 then " + g + ".AutoUntilPoll(true) end\n";
    s += "\t\telse\n";
    s += "\t\t\t" + g + ".AutoUntilPoll(false)\n";
    s += "\t\tend\n";
    s += "\tend\n";
    // Spaced on the async clock, not os.time (see AutoAfter); before a world these heartbeats
    // are the arriving world's own hooks, so the spacing hardly matters.
    s += "\tif a.awaitLoad and a.lastTry ~= nil and " + g + ".Now() - a.lastTry >= 3 then\n";
    s += "\t\t" + g + ".AutoTryLoad(source or \"heartbeat\")\n";
    s += "\tend\n";
    s += "end\n";
    s += "\n";
    // Loaded is: a player entity exists and the engine is not mid-load. Both are read
    // through pcall since neither is certain at the menu. A settle after that, because
    // the first seconds in a level are the streaming and the AI waking, and a save asked
    // for then is a save of a world still arriving.
    s += "function " + g + ".AutoLoaded()\n";
    s += "\tlocal p = rawget(_G, \"player\")\n";
    s += "\tif p == nil then return false, \"player=nil\" end\n";
    s += "\tif p.soul == nil then return false, \"player.soul=nil\" end\n";
    s += "\tlocal ok, loading = pcall(function() return Game.IsLoadingEngineSaveGame() end)\n";
    s += "\tif ok and loading == true then return false, \"IsLoadingEngineSaveGame=true\" end\n";
    s += "\treturn true, \"\"\n";
    s += "end\n";
    s += "\n";
    // Each poll's line says where it came from and which pulse tick it was on; the first
    // few and then every tenth, since a poll is a second.
    s += "function " + g + ".AutoPoll(source)\n";
    s += "\tlocal a = " + g + ".auto\n";
    s += "\ta.waited = a.waited + 1\n";
    s += "\tif not a.polling then return end\n";
    s += "\tlocal loaded, why = " + g + ".AutoLoaded()\n";
    s += "\tif a.waited <= 3 or a.waited % 10 == 1 then\n";
    s += "\t\t" + g + ".Log(\"BRIDGE auto poll \" .. a.waited .. \" from \" .. tostring(source) .. \" \" .. tostring(why) .. \" async=\" .. string.format(\"%.1f\", " + g + ".Now()) .. \" pulse=\" .. tostring(a.pulseN))\n";
    s += "\tend\n";
    s += "\tif loaded then\n";
    s += "\t\ta.polling = false\n";
    // The F9 map stays armed through the settle (AutoNext takes it down): the settle's
    // timer and the pulse are both wiped when the save finishes streaming, which is often
    // after this line, and a run whose Henry faced a wall (no GetActions) then sat at the
    // settle until the idle kill (26 Oct 2026). The runner nudges until the next step.
    s += "\t\t" + g + ".Log(\"BRIDGE auto loaded after \" .. a.waited .. \"s, settling \" .. a.settle .. \"s\")\n";
    s += "\t\t" + g + ".AutoAfter(a.settle * 1000)\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    s += "\tif a.waited >= a.timeout then\n";
    s += "\t\ta.polling = false\n";
    s += "\t\t" + g + ".Log(\"BRIDGE auto: no loaded world after \" .. a.waited .. \"s\")\n";
    s += "\t\t" + g + ".AutoAfter(100)\n";
    s += "\tend\n";
    s += "end\n";
    s += "\n";
    // The script runs before the startup video, when QuickLoad says false; the heartbeat
    // asks again every few seconds until a world arrives or the tries run out. A world
    // arriving (OnWorld) ends the retries by clearing awaitLoad.
    s += "function " + g + ".AutoTryLoad(from)\n";
    s += "\tlocal a = " + g + ".auto\n";
    s += "\tif not a.awaitLoad then return end\n";
    s += "\tif a.loadTries >= 40 then\n";
    s += "\t\t" + g + ".Log(\"BRIDGE auto no world after \" .. a.loadTries .. \" load tries, giving up\")\n";
    s += "\t\ta.awaitLoad = false\n";
    s += "\t\t" + g + ".AutoNext()\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    s += "\ta.loadTries = a.loadTries + 1\n";
    s += "\ta.lastTry = " + g + ".Now()\n";
    s += "\tlocal quick = nil\n";
    s += "\t" + g + ".AutoCall(\"Game.QuickLoad try \" .. a.loadTries .. \" from \" .. from, function() quick = Game.QuickLoad(); return quick end)\n";
    s += "end\n";
    s += "\n";
    // Flags are how a lua step that works on its own timers tells the run it is finished:
    // the step's code raises one (AutoFlag) and an `until` step waits for it. A flag is
    // one-shot, taken by the until that consumes it, so a sweep that raises the same name
    // once a road waits for a fresh one each time. Raised before anything waits on it, it
    // is kept until something does.
    s += g + ".auto.flags = {}\n";
    s += "function " + g + ".AutoFlag(name)\n";
    s += "\tlocal a = " + g + ".auto\n";
    s += "\tif a.flags == nil then a.flags = {} end\n";
    s += "\ta.flags[tostring(name)] = true\n";
    s += "\t" + g + ".Log(\"BRIDGE auto flag \" .. tostring(name))\n";
    s += "\tif a.untilFlag == tostring(name) then " + g + ".AutoUntilPoll(false) end\n";
    s += "end\n";
    s += "\n";
    s += "function " + g + ".AutoUntilPoll(count)\n";
    s += "\tlocal a = " + g + ".auto\n";
    s += "\tlocal name = a.untilFlag\n";
    s += "\tif name == nil then return end\n";
    s += "\tif count then a.untilWaited = (a.untilWaited or 0) + 1 end\n";
    s += "\tif a.flags ~= nil and a.flags[name] then\n";
    s += "\t\ta.flags[name] = nil\n";
    s += "\t\ta.untilFlag = nil\n";
    s += "\t\t" + g + ".Log(\"BRIDGE auto until \" .. name .. \" after \" .. a.untilWaited .. \"s\")\n";
    s += "\t\t" + g + ".AutoAfter(100)\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    s += "\tif a.untilWaited >= a.untilTimeout then\n";
    s += "\t\ta.untilFlag = nil\n";
    s += "\t\t" + g + ".Log(\"BRIDGE auto until \" .. name .. \" timed out after \" .. a.untilWaited .. \"s\")\n";
    s += "\t\t" + g + ".AutoAfter(100)\n";
    s += "\tend\n";
    s += "end\n";
    s += "\n";
    // The rebaseline step's answer: the runner, having written the baseline back over the
    // saves, presses F9 (the nudge key, armed for it), and the first press while one is
    // awaited is the acknowledgement. Without the runner the step times out and the load
    // after it takes whatever save is newest, which the report will show.
    s += "function " + g + ".AutoAck(from)\n";
    s += "\tlocal a = " + g + ".auto\n";
    s += "\tif a == nil or not a.awaitAck then return false end\n";
    s += "\ta.awaitAck = false\n";
    s += "\t" + g + ".Log(\"BRIDGE auto rebaseline acknowledged (\" .. tostring(from) .. \")\")\n";
    s += "\tpcall(" + g + ".AutoDisarmNudge)\n";
    s += "\t" + g + ".AutoFlag(\"rebaselined\")\n";
    s += "\treturn true\n";
    s += "end\n";
    s += "\n";
    s += "function " + g + ".AutoCall(label, fn)\n";
    s += "\tlocal t0 = " + g + ".Now()\n";
    s += "\tlocal ok, ret = pcall(fn)\n";
    s += "\t" + g + ".Log(\"BRIDGE auto \" .. label .. \" -> \" ..\n";
    s += "\t\t(ok and tostring(ret) or (\"raised \" .. tostring(ret))) ..\n";
    s += "\t\t\" ms=\" .. string.format(\"%.1f\", (" + g + ".Now() - t0) * 1000))\n";
    s += "\treturn ok\n";
    s += "end\n";
    s += "\n";
    // The steps. Each either arms AutoAfter itself or hands over to a poll that will.
    //   wait S               sit for S seconds
    //   load                 await a world: Game.QuickLoad() is asked (it says false at
    //                        the menu) and the game's own autoload, from the user.cfg
    //                        the runner writes, is what actually brings the save
    //   waitloaded T S       poll up to T seconds for a world, then settle S seconds
    //   fill KB N MARKER     put the spike in the store
    //   check                say what the spike in the store looks like now
    //   save                 Game.QuickSave()
    //   rest                 Game.SaveGameViaResting(), the other road to a save
    //   savecmd              the console's wh_sys_TestSaveGame, the third
    //   cmd STRING           any console command, through System.ExecuteCommand
    //   cvar NAME VALUE      System.SetCVar, then GetCVar read back into the log
    //   tokenfill KB MARKER  the same blob through GameToken.SetToken, the other road
    //                        the engine is known to carry through a save
    //   tokencheck           what GameToken.GetToken gives back for it
    //   longline N           a log line of N x's
    //   lua CODE             anything else
    //   until NAME T         wait up to T seconds (600) for Moonshine.AutoFlag(NAME), which
    //                        a lua step's own timers raise when their work is done
    //   rebaseline T         ask the runner to write the baseline back over the saves
    //                        ("BRIDGE auto rebaseline N"; it answers with F9) and wait up
    //                        to T seconds (120) for the answer; a `load` follows
    //   quit                 System.Quit()
    s += "function " + g + ".AutoStep(step)\n";
    s += "\tlocal name = step[1]\n";
    s += "\tif name == \"wait\" then\n";
    s += "\t\t" + g + ".AutoAfter((tonumber(step[2]) or 1) * 1000)\n";
    s += "\telseif name == \"load\" then\n";
    // The next step is reached from OnInit when the world arrives, or from the timer
    // if timers happen to run here; whichever comes first takes the token.
    // A load asked for from inside a world (a sweep going back to its baseline between
    // roads) is said so, since the runner's nudges after a load start from that line.
    s += "\t\tif " + g + ".AutoLoaded() then " + g + ".Log(\"BRIDGE auto reload asked from a loaded world\") end\n";
    s += "\t\t" + g + ".auto.awaitLoad = true\n";
    s += "\t\t" + g + ".auto.loadTries = 0\n";
    s += "\t\t" + g + ".AutoTryLoad(\"step\")\n";
    s += "\telseif name == \"waitloaded\" then\n";
    s += "\t\t" + g + ".auto.polling = true\n";
    s += "\t\t" + g + ".auto.waited = 0\n";
    s += "\t\t" + g + ".auto.timeout = tonumber(step[2]) or 180\n";
    s += "\t\t" + g + ".auto.settle = tonumber(step[3]) or 10\n";
    s += "\t\tpcall(function() Script.SetTimer(1000, function() " + g + ".AutoHeartbeat(\"timer\") end, nil, true) end)\n";
    s += "\telseif name == \"fill\" then\n";
    s += "\t\t" + g + ".SpikeFill(tonumber(step[2]) or 64, tonumber(step[3]) or 1000, step[4])\n";
    s += "\t\t" + g + ".AutoAfter(100)\n";
    s += "\telseif name == \"check\" then\n";
    s += "\t\t" + g + ".SpikeCheck()\n";
    s += "\t\t" + g + ".AutoAfter(100)\n";
    s += "\telseif name == \"save\" then\n";
    s += "\t\t" + g + ".AutoCall(\"Game.QuickSave\", function() return Game.QuickSave() end)\n";
    s += "\t\t" + g + ".AutoAfter(1000)\n";
    s += "\telseif name == \"rest\" then\n";
    s += "\t\t" + g + ".AutoCall(\"Game.SaveGameViaResting\", function() return Game.SaveGameViaResting() end)\n";
    s += "\t\t" + g + ".AutoAfter(1000)\n";
    s += "\telseif name == \"savecmd\" then\n";
    s += "\t\t" + g + ".AutoCall(\"wh_sys_TestSaveGame\", function() return System.ExecuteCommand(\"wh_sys_TestSaveGame\") end)\n";
    s += "\t\t" + g + ".AutoAfter(1000)\n";
    s += "\telseif name == \"cvar\" then\n";
    s += "\t\tlocal cname = tostring(step[2])\n";
    s += "\t\tlocal before = nil\n";
    s += "\t\tpcall(function() before = System.GetCVar(cname) end)\n";
    s += "\t\tlocal ok, err = pcall(function() System.SetCVar(cname, step[3]) end)\n";
    s += "\t\tlocal after = nil\n";
    s += "\t\tpcall(function() after = System.GetCVar(cname) end)\n";
    s += "\t\t" + g + ".Log(\"BRIDGE auto cvar \" .. cname .. \" was \" .. tostring(before) .. \", set \" .. tostring(step[3]) .. \" -> \" .. tostring(after) .. (ok and \"\" or (\" (raised \" .. tostring(err) .. \")\")))\n";
    s += "\t\t" + g + ".AutoAfter(100)\n";
    s += "\telseif name == \"cmd\" then\n";
    s += "\t\t" + g + ".AutoCall(tostring(step[2]), function() return System.ExecuteCommand(tostring(step[2])) end)\n";
    s += "\t\t" + g + ".AutoAfter(1000)\n";
    s += "\telseif name == \"tokenfill\" then\n";
    s += "\t\tlocal kb = tonumber(step[2]) or 64\n";
    s += "\t\tlocal blob = table.concat(" + g + ".SpikeChunks(kb))\n";
    s += "\t\t" + g + ".AutoCall(\"GameToken.SetToken moonshine.marker\", function() return GameToken.SetToken(\"moonshine.marker\", tostring(step[3])) end)\n";
    s += "\t\t" + g + ".AutoCall(\"GameToken.SetToken moonshine.blob \" .. #blob .. \" bytes\", function() return GameToken.SetToken(\"moonshine.blob\", blob) end)\n";
    s += "\t\t" + g + ".AutoCall(\"GameToken.SetToken moonshine.sum\", function() return GameToken.SetToken(\"moonshine.sum\", " + g + ".Checksum(blob)) end)\n";
    s += "\t\t" + g + ".AutoAfter(100)\n";
    s += "\telseif name == \"tokencheck\" then\n";
    s += "\t\tlocal marker, blob, sum\n";
    s += "\t\tpcall(function() marker = GameToken.GetToken(\"moonshine.marker\") end)\n";
    s += "\t\tpcall(function() blob = GameToken.GetToken(\"moonshine.blob\") end)\n";
    s += "\t\tpcall(function() sum = GameToken.GetToken(\"moonshine.sum\") end)\n";
    s += "\t\tlocal now = type(blob) == \"string\" and " + g + ".Checksum(blob) or \"-\"\n";
    s += "\t\t" + g + ".Log(\"BRIDGE spike token marker=\" .. tostring(marker) .. \" blob=\" .. tostring(type(blob) == \"string\" and #blob or type(blob)) ..\n";
    s += "\t\t\t\" sum=\" .. now .. \" stored=\" .. tostring(sum) .. \" intact=\" .. tostring(sum ~= nil and now == sum))\n";
    s += "\t\t" + g + ".AutoAfter(100)\n";
    s += "\telseif name == \"longline\" then\n";
    s += "\t\t" + g + ".LongLine(tonumber(step[2]) or 8000)\n";
    s += "\t\t" + g + ".AutoAfter(100)\n";
    s += "\telseif name == \"lua\" then\n";
    s += "\t\tlocal loadstring = rawget(_G, \"loadstring\") or rawget(_G, \"load\")\n";
    s += "\t\tlocal chunk, err = loadstring(tostring(step[2]), \"auto\")\n";
    s += "\t\tif chunk == nil then error(err) end\n";
    s += "\t\t" + g + ".AutoCall(\"lua\", chunk)\n";
    s += "\t\t" + g + ".AutoAfter(100)\n";
    s += "\telseif name == \"until\" then\n";
    s += "\t\t" + g + ".auto.untilFlag = tostring(step[2])\n";
    s += "\t\t" + g + ".auto.untilWaited = 0\n";
    s += "\t\t" + g + ".auto.untilTimeout = tonumber(step[3]) or 600\n";
    s += "\t\t" + g + ".AutoUntilPoll(false)\n";
    s += "\telseif name == \"rebaseline\" then\n";
    s += "\t\tlocal a = " + g + ".auto\n";
    s += "\t\ta.rebaselines = (a.rebaselines or 0) + 1\n";
    s += "\t\ta.awaitAck = true\n";
    s += "\t\ta.nudgeArmed = false\n";
    s += "\t\tpcall(" + g + ".AutoArmNudge, rawget(_G, \"player\"))\n";
    s += "\t\ta.untilFlag = \"rebaselined\"\n";
    s += "\t\ta.untilWaited = 0\n";
    s += "\t\ta.untilTimeout = tonumber(step[2]) or 120\n";
    s += "\t\t" + g + ".Log(\"BRIDGE auto rebaseline \" .. a.rebaselines)\n";
    s += "\telseif name == \"quit\" then\n";
    s += "\t\t" + g + ".Log(\"BRIDGE auto quitting\")\n";
    s += "\t\t" + g + ".AutoCall(\"System.Quit\", function() return System.Quit() end)\n";
    s += "\telse\n";
    s += "\t\terror(\"unknown step \" .. tostring(name))\n";
    s += "\tend\n";
    s += "end\n";
    s += "\n";
    // Found the way turns are: under whichever inbox pattern answers, with autopilot.txt
    // in place of turn_#.txt. Read once, at install, which is the main menu -- and at the
    // main menu a loose file under %USER% reads as nil (measured), so the runner ships the
    // script inside a small pak in the mod's Data folder, where "moonshine/autopilot.txt"
    // reads the way the mod's own Lua does. A game is one process and the engine's cache
    // of a path read is the process's, so the next launch reads the file afresh.
    s += "function " + g + ".Autopilot()\n";
    s += "\tlocal loadstring = rawget(_G, \"loadstring\") or rawget(_G, \"load\")\n";
    s += "\tif System.LoadTextFile == nil or loadstring == nil then return end\n";
    s += "\tlocal looked = {}\n";
    s += "\tfor i = 1, #" + g + ".inboxes do\n";
    s += "\t\tlocal path = (string.gsub(" + g + ".inboxes[i], \"turn_#%.txt\", \"autopilot.txt\"))\n";
    s += "\t\tlocal ok, got = pcall(function() return System.LoadTextFile(path) end)\n";
    s += "\t\tif ok and type(got) == \"string\" and got ~= \"\" then\n";
    s += "\t\t\t" + g + ".Log(\"BRIDGE auto script from \" .. path .. \", \" .. #got .. \" bytes\")\n";
    s += "\t\t\tlocal chunk, err = loadstring(got, \"autopilot\")\n";
    s += "\t\t\tif chunk == nil then\n";
    s += "\t\t\t\t" + g + ".Log(\"BRIDGE auto script did not parse: \" .. tostring(err))\n";
    s += "\t\t\t\treturn\n";
    s += "\t\t\tend\n";
    s += "\t\t\tlocal ran, why = pcall(chunk)\n";
    s += "\t\t\tif not ran then " + g + ".Log(\"BRIDGE auto script raised \" .. tostring(why)) end\n";
    s += "\t\t\treturn\n";
    s += "\t\tend\n";
    s += "\t\tlooked[#looked + 1] = path .. \"=\" .. tostring(ok and type(got) or got)\n";
    s += "\tend\n";
    s += "\t" + g + ".Log(\"BRIDGE auto no script: \" .. table.concat(looked, \" \"))\n";
    s += "end\n";
    s += "\n";
    return s;
}

}  // namespace

std::string emit_conversation_lua(const std::string& modid,
                                  const std::vector<Persona>& personas,
                                  const std::string& role,
                                  const std::string& metarole,
                                  const std::string& leave_hint,
                                  const ScreenText& screen,
                                  const std::vector<std::string>& inboxes,
                                  const std::vector<Quest>& quests) {
    if (inboxes.empty()) throw std::runtime_error("nowhere to read a turn's words from");
    if (screen.mode != "draw" && screen.mode != "info" && screen.mode != "caption") {
        throw std::runtime_error(
            "words reach the screen by 'draw', 'info' or 'caption', not '" + screen.mode + "'");
    }
    if (screen.repaint_millis <= 0) throw std::runtime_error("a repaint needs an interval");
    // Lua identifiers cannot hold a modid with underscores in every position we
    // want, so everything hangs off one global table named after the mod.
    const std::string g = "Moonshine";

    std::string s;
    s += "-- " + modid + " -- adds one interactor option to the NPCs this mod renamed.\n";
    s += "-- Generated by kcd2modgen. Edit the generator, not this file.\n";
    s += "\n";
    s += g + " = " + g + " or {}\n";
    s += "\n";
    s += "-- The souls this mod gave a personal name to, keyed by the entity name that\n";
    s += "-- STORM matched with <hasName>. Story NPCs are absent, so they keep the menu\n";
    s += "-- they shipped with.\n";
    s += g + ".namedSouls = {\n";
    for (const Persona& p : personas) {
        s += "[\"" + lua_escape(p.soul_name) + "\"]=true,\n";
    }
    s += "}\n";
    s += g + ".soulCount = " + std::to_string(personas.size()) + "\n";
    s += "-- And what each was named, as the nameplate reads it, so a line in the caption can\n";
    s += "-- say who is speaking. The speaker is being looked at, but a line that arrives a\n";
    s += "-- word at a time under a face reads better with a name in front of it.\n";
    s += g + ".soulNames = {\n";
    for (const Persona& p : personas) {
        s += "[\"" + lua_escape(p.soul_name) + "\"]=\"" + lua_escape(p.full_name) + "\",\n";
    }
    s += "}\n";
    // The beggars among them (npc.occupation), who take alms off the app's rows: the begin
    // line carries alms=<letters> for these and nobody else (AlmsLine).
    s += g + ".beggars = {\n";
    for (const Persona& p : personas) {
        if (p.beggar) s += "[\"" + lua_escape(p.soul_name) + "\"]=true,\n";
    }
    s += "}\n";
    s += g + ".npcName = \"\"\n";
    s += "\n";
    s += g + ".hint = \"@" + lua_escape(conversation_hint_key(modid)) + "\"\n";
    s += "-- The prompt on a giver whose favour can be taken up now: neither accepted nor\n";
    s += "-- done in the save. It means offerable, not \"has a quest in the database\", so a\n";
    s += "-- giver whose errand is finished reads like anyone else. Its own words rather\n";
    s += "-- than the game's Talk, which a player scans a street for to find the game's\n";
    s += "-- quests and should go on finding them under.\n";
    s += g + ".favourHint = \"@" + lua_escape(favour_hint_key(modid)) + "\"\n";
    s += "-- Shown on its own as a conversation starts, because the way out of a focused\n";
    s += "-- conversation is not one a player can guess: the prompt that started it is gone,\n";
    s += "-- movement is gone, and there is no menu. Only then: it used to ride beside every\n";
    s += "-- line as well, and a line that ends in the same parenthesis every time is a line\n";
    s += "-- the eye learns to stop before the end of. Literal text, as the words are.\n";
    s += g + ".leaveHint = \"" + lua_escape(leave_hint) + "\"\n";
    s += g + ".role = \"" + lua_escape(role) + "\"\n";
    s += g + ".metarole = \"" + lua_escape(metarole) + "\"\n";
    s += "\n";
    s += "-- The global the dialogue reads at the end of every beat to decide whether to\n";
    s += "-- come round again. Setting it is how a session is held open, and clearing it\n";
    s += "-- is how we are let out.\n";
    s += g + ".talkVar = \"" + lua_escape(talking_variable(modid)) + "\"\n";
    s += "-- Which face the next beat plays. The graph Autoselects a sequence per value,\n";
    s += "-- so this is the only way Lua can ask for a mouth or an expression. 0 is idle\n";
    s += "-- (camera only). Speak() writes the speak code for a stretch of ticks and then\n";
    s += "-- puts back restFace, which is whatever Face() last named that was not speak.\n";
    s += g + ".faceVar = \"" + lua_escape(face_variable(modid)) + "\"\n";
    s += g + ".faces = {\n";
    for (int i = 0; i < kFaceCount; ++i) {
        s += "\t" + std::string(kFaces[i].name) + " = " + std::to_string(i) + ",\n";
    }
    s += "}\n";
    s += g + ".face = " + g + ".faces.idle\n";
    s += g + ".restFace = " + g + ".faces.idle\n";
    s += g + ".speakUntil = 0\n";
    s += "-- One past the bank: the code that plays the reply menu instead of a beat. The\n";
    s += "-- graph turns the camera on Henry and presents a decision of menuRows rows, whose\n";
    s += "-- placeholder prompts Lua draws over; the row picked comes back through Picked().\n";
    s += g + ".chooseCode = " + std::to_string(kFaceCount) + "\n";
    s += g + ".menuRows = " + std::to_string(kMenuRows) + "\n";
    s += "-- How many of them carry the app's rows; the two after the last with words are\n";
    s += "-- the say row and the way out.\n";
    s += g + ".menuOptionRows = " + std::to_string(kMenuOptionRows) + "\n";
    s += g + ".menuLeave = \"(End dialog)\"\n";
    s += "-- The row above the way out: picked, the menu goes and the keyboard opens for a\n";
    s += "-- line of Henry's own, typed on the mod's keyboard or spoken into the app's\n";
    s += "-- microphone (say-mode, SayOpen).\n";
    s += g + ".menuSay = \"(Say something else)\"\n";
    s += "-- Say-mode: the keyboard open from the menu rather than from F9. The line so far is\n";
    s += "-- `typed`, as it is for the keyboard -- keys append, the app's transcript (SayHeard)\n";
    s += "-- replaces -- and what differs is Enter (the words, or nothing on an empty line),\n";
    s += "-- escape and B (the line cleared, or with nothing in it the menu brought back).\n";
    s += g + ".saying = false\n";
    s += "-- Where the say row was drawn at the last draw (SayRow), like menuLeaveRow.\n";
    s += g + ".menuSayRow = " + std::to_string(kMenuRows - 1) + "\n";
    s += "-- Off, and Henry answers on the keyboard as before: for a writer that sends no\n";
    s += "-- options, and for the harness, which drives the keyboard.\n";
    s += g + ".menuOn = true\n";
    s += g + ".choosing = false\n";
    s += "-- The rows the app has sent for this round, by row; menuWait is a menu asked for\n";
    s += "-- and not yet entered, menuUp one the engine has presented, menuDrawn how many rows\n";
    s += "-- of words were on it at the last draw (0 before the first). menuToken changes on\n";
    s += "-- every Choose and Picked so a timer armed for an earlier menu does nothing.\n";
    s += g + ".options = {}\n";
    s += g + ".menuWait = false\n";
    s += g + ".menuUp = false\n";
    s += g + ".menuDrawn = 0\n";
    s += g + ".menuToken = 0\n";
    s += "-- Where the way out was drawn at the last draw: straight after the last row with\n";
    s += "-- words, so a short list is short rather than padded with dots.\n";
    s += g + ".menuLeaveRow = " + std::to_string(kMenuRows) + "\n";
    s += "-- How many rows the app said the list has (@rows), or nil when it has not said.\n";
    s += g + ".optionsTotal = nil\n";
    s += "-- The app said the list is closed (@rows <n> closed): its rows alone are drawn, with\n";
    s += "-- no say row and no way out under them. Cleared when the menu is next asked for.\n";
    s += g + ".menuClosed = false\n";
    s += "-- Set by a turn flagged @end: the conversation closes once that turn is read.\n";
    s += g + ".ending = false\n";
    s += "-- @voiced in front of a line: the app is speaking what it writes, and sends a hook\n";
    s += "-- (Spoken) when a line's audio has been heard to the end. Two lines are paced by\n";
    s += "-- that rather than by the reading time guessed from the words: Henry's, which the\n";
    s += "-- camera stays on (henryCode, the beat framed on him) until Spoken('henry') or the\n";
    s += "-- reading plus a grace, so his reply is not shown over him mid-sentence; and the\n";
    s += "-- closing one, which the conversation stays open for until Spoken('npc') or the\n";
    s += "-- same grace, so the camera does not cut the last line off. The grace is in ticks,\n";
    s += "-- for an app that said it would speak and then did not.\n";
    s += g + ".voiced = false\n";
    s += g + ".spoken = false\n";
    s += g + ".spokenGraceTicks = 30\n";
    s += g + ".henryCode = " + std::to_string(kFaceCount + 1) + "\n";
    s += g + ".henryUntil = 0\n";
    s += "-- The mouth. As the app starts a line's audio it writes lip_<key>_<n>.txt beside the\n";
    s += "-- turn files, <key> being a sum over the voice marker's words that both sides compute\n";
    s += "-- (LipKey), naming the game's own facial clips to play on the speaker and the speed\n";
    s += "-- to play them at, with rests where the speech pauses; later files revise the plan\n";
    s += "-- from a second on, as the app finds the pauses, and the last says stop. LipPoll\n";
    s += "-- looks for the next one ten times a second while a voiced line is up. lipsync goes\n";
    s += "-- true at the first cue of the run, and from then on Say() leaves the OpenMouth/Hmm\n";
    s += "-- stack alone.\n";
    s += g + ".lipKey = nil\n";
    s += g + ".lipSeq = 1\n";
    s += g + ".lipWho = nil\n";
    s += g + ".lipGen = 0\n";
    s += g + ".lipPollMillis = 100\n";
    s += g + ".lipPollMax = 600\n";
    s += g + ".lipLayer = 13\n";
    s += g + ".lipBlend = 0.15\n";
    s += "-- The mouth let go softly (notes.md 14 Oct 2026): stopping the layer put the engine's\n";
    s += "-- face back in a frame, a snap at every rest and line's end. The app names a shut clip\n";
    s += "-- for the face ('shut <clip>' in the cue: a shipped clip whose mouth never opens) and\n";
    s += "-- the mod blends into it over lipRestBlend where it used to stop, stopping the layer\n";
    s += "-- only lipLetGoMillis after the line, once the lips have settled shut. lipOn is the\n";
    s += "-- clip on the layer now, so a clip already playing is not re-issued (refused) or\n";
    s += "-- stopped under itself.\n";
    s += g + ".lipRestBlend = 0.25\n";
    s += g + ".lipLetGoMillis = 1500\n";
    s += g + ".lipShut = nil\n";
    s += g + ".lipOn = nil\n";
    s += g + ".lipOnEnt = nil\n";
    s += g + ".lipEnt = nil\n";
    s += g + ".lipPlan = nil\n";
    s += g + ".lipPending = nil\n";
    s += g + ".lipClip = 0\n";
    s += g + ".lipsync = false\n";
    s += "-- The mouth shaped word by word. Once the app knows where the words fall (it reads\n";
    s += "-- them off the voice model as it speaks) its cues say which shape to show when,\n";
    s += "-- 'mouth <seconds> <viseme>', seconds from the start of the line's audio; LipMouthTick\n";
    s += "-- shows each as its second comes, counting from the first cue of the line (lipT0: the\n";
    s += "-- app writes it as the sound starts, and it is seen within a poll). A shape is a\n";
    s += "-- short clip of the mod's own holding the mouth in it (MoonshineMouth, from\n";
    s += "-- Scripts/mods/moonshine_mouth.lua: scripts/facial_corpus.py bank), or, where the\n";
    s += "-- game did not load those, a frame of one of its own facial clips held. The\n";
    s += "-- borrowed-clip plan is dropped at the first shape.\n";
    s += g + ".lipMouth = {}\n";
    s += g + ".lipMouthAt = 1\n";
    s += g + ".lipT0 = nil\n";
    s += g + ".lipMouthMillis = 40\n";
    s += g + ".lipMouthLead = 0.05\n";
    s += g + ".lipMouthBlend = 0.08\n";
    s += g + ".lipMouthBlendMax = 0.25\n";
    s += g + ".lipMouthSpeed = 0.01\n";
    s += g + ".lipShape = nil\n";
    s += g + ".lipHoldAt = 0\n";
    s += g + ".lipHoldLeft = 0\n";
    s += g + ".lipShapes = 0\n";
    s += g + ".lipRefused = 0\n";
    s += g + ".lipLate = 0\n";
    s += "-- The menu waits for the voice: a voiced line's menu (Heard -> Choose) would turn the\n";
    s += "-- camera on Henry two seconds in, over the other party's mouth still working, so\n";
    s += "-- while that line's cues are being looked for the menu is held (MenuHold) and let go\n";
    s += "-- by the stop cue, by a tap of the skip button (Skip), or by the reading time plus\n";
    s += "-- the grace (MenuHeldExpire).\n";
    s += g + ".menuHeld = false\n";
    s += g + ".menuHeldAt = 0\n";
    s += "-- After the boundary the menu sequence is entered on, how long before the engine's\n";
    s += "-- own rows are surely up (measured: about 0.3s; a draw at 0.25s was overwritten),\n";
    s += "-- and after that how long to wait for the last row before drawing without it.\n";
    s += g + ".menuDelayMillis = 600\n";
    s += g + ".menuGraceMillis = 1500\n";
    // The engine draws its placeholder dots about 0.3s after the menu sequence starts
    // (measured; a draw at 0.25s was overwritten). When the rows are already whole --
    // written ahead while Henry looked (session.cpp, preview_opening_list) -- ours covers
    // them at this mark, so the player never sits on "..." for the rest of a second
    // (notes.md, 5 Oct 2026). The longer delay stays for a list that is still arriving.
    s += g + ".menuCoverMillis = 350\n";
    // Both marks were measured at 60 fps, and the engine's draw is not a time alone: a
    // fixed delay in its graph and then some frames of its UI, so at 30 fps -- which is
    // where Frames holds the game for the voice's sake while a conversation is up -- it
    // lands later, and a cover drawn at 0.35s was under it (11 Oct 2026: a menu of five
    // "..." with the rows all in; a redraw by hand put them up). So each millisecond a
    // frame runs over 60 fps's is counted this many times over on top of either mark:
    // at 30 fps the cover comes at 0.65s and the delay at 0.9s, at 60 fps nothing changes.
    s += "-- How many frames' worth of the frame's excess over 60 fps is added to the two\n";
    s += "-- marks above (MenuMillis), the engine's own draw being partly a count of frames.\n";
    s += g + ".menuCoverFrames = 18\n";
    s += "-- Before the menu is asked of the engine at all, how long to wait for the rows to\n";
    s += "-- arrive from the app, so the engine's dots are drawn over at the first chance.\n";
    s += g + ".menuEnterGraceMillis = 2000\n";
    s += g + ".chooseWaited = 0\n";
    s += "-- After the code is set, how long without a beat boundary before the graph is taken\n";
    s += "-- to have entered the menu already, the code having changed between a beat's exit\n";
    s += "-- script and its decision (MenuMissed): a beat is 2.5s and a quarter, with room.\n";
    s += g + ".menuMissedMillis = 3500\n";
    s += "-- Which quest beat the next loop plays, or 0 for none. A quest beat is a face\n";
    s += "-- beat that also fires one of the dialogue's ports, and the graph Autoselects it\n";
    s += "-- the same way: while this is non-zero no face sequence is available, so the\n";
    s += "-- code is put back to 0 by the beat's own exit script, or by the pump if that\n";
    s += "-- never runs. Codes are dealt in the order below, two per quest.\n";
    s += g + ".questVar = \"" + lua_escape(quest_variable(modid)) + "\"\n";
    // Which deal this build was made from, for the app to check against its own database
    // on the begin line (qset=): see quest_set_fingerprint.
    {
        std::vector<std::string> ids;
        for (const Quest& q : quests) ids.push_back(q.id);
        s += g + ".questSet = \"" + quest_set_fingerprint(ids) + "\"\n";
    }
    s += g + ".questCodes = {\n";
    for (const QuestBeat& b : quest_beats(quests)) {
        s += "\t[\"" + lua_escape(b.quest_id + " " + b.command) + "\"] = " +
             std::to_string(b.code) + ",\n";
    }
    s += "}\n";
    s += "-- The quests themselves, by id: who asks, who has the thing, and what the thing\n";
    s += "-- is, since moving it is done here rather than in the graph. The item is a class\n";
    s += "-- guid, which is what every inventory call takes. holderMeta is the metarole the\n";
    s += "-- holder's own menu speaks through, granted on sight to a holder an old save made\n";
    s += "-- before the mod; pay is what the menu's money option costs, in groschen, and\n";
    s += "-- reward is what the giver hands over at the hand-in when the graph has no\n";
    s += "-- money tier for so small a sum (0 when the graph pays, or nothing is paid).\n";
    s += "-- chain/order and requires are the slate's (a chain link is offered once the link\n";
    s += "-- before is done; a nest's holder hands over once the favour he gives is done);\n";
    s += "-- dispute says the holder menu has the broker row; brokerVar and innerVar are\n";
    s += "-- the dialogue globals that menu reads, kept from the store by QuestFlags.\n";
    s += "-- after/afterHow are a scene's link: the favour is offered only once `after` is done,\n";
    s += "-- and only if it ended one of the afterHow ways (QuestOfferable; empty is any way).\n";
    s += "-- kind is retrieve, deliver, find, travel, beat, pray, mind or ask: a deliver's item is a\n";
    s += "-- letter made in the player's pocket at accept and moved to the target by the target's\n";
    s += "-- own hand row; a find has no target, and count is how many of the item the giver\n";
    s += "-- wants and the hand-in takes; a travel has no item, and its target is only looked at\n";
    s += "-- (Watch); a beat has no item, and its target is watched for going down or dying; a\n";
    s += "-- pray has neither, and spot is where Henry kneels (level units) and token the item\n";
    s += "-- class the Watch makes for a moment when he has; a mind has no item, count is the\n";
    s += "-- visits, gap the game hours between them, and checksVar/dueVar the globals its\n";
    s += "-- target's menu reads; an ask has no item, its target has a beat's menu, and the\n";
    s += "-- handover is the truth told (nothing changes hands, unless gift names the classes the\n";
    s += "-- one asked presses on Henry with it, made in his pocket then); a leave has no target, its item\n";
    s += "-- is made in the player's pocket at accept (count of them) and put down at spot, within\n";
    s += "-- hours (from, to; nil is any hour), after which cue (an audio trigger) is played from\n";
    s += "-- the dark; a return is a leave with no hours, loot naming the level's own pieces at\n";
    s += "-- the spot that a thief would take. An ambush's target is spoken to in three stages\n";
    s += "-- (stageVar, the global its menu reads: 0, 1 directed, 2 ambushed); its item is the\n";
    s += "-- directions, made in the player's pocket on the first; spot and hours are where and\n";
    s += "-- when the Watch springs the trap with its token. A buy's holder wants carry (the\n";
    s += "-- giver's thing, made in the player's pocket at accept with pay in groschen) and\n";
    s += "-- carryVar is the global its menu reads for whether Henry still has it.\n";
    s += g + ".quests = {\n";
    for (const Quest& q : quests) {
        s += "\t[\"" + lua_escape(q.id) + "\"] = { kind = \"" + lua_escape(q.kind) + "\", giver = \"" +
             lua_escape(q.giver_soul) +
             "\", target = \"" + lua_escape(q.target_soul) + "\", item = \"" +
             lua_escape(q.item_class) + "\", name = \"" + lua_escape(q.name) +
             "\", holderMeta = \"" + lua_escape(holder_metarole(q.id)) +
             "\", pay = " + std::to_string(q.pay_groschen) +
             ", reward = " + std::to_string(q.reward_tier.empty() ? q.reward_groschen : 0) +
             ", chain = \"" + lua_escape(q.chain_id) + "\", order = " + std::to_string(q.chain_order) +
             ", requires = \"" + lua_escape(q.requires_quest_id) + "\", after = \"" +
             lua_escape(q.after_quest_id) + "\", afterHow = \"" + lua_escape(q.after_how) + "\", count = " +
             std::to_string(q.count) + ", minHealth = " + health_text(q.min_health) + ", dispute = " +
             (q.dispute ? "true" : "false") + ", brokerVar = \"" +
             lua_escape(quest_flag_variable(modid, q.id, "broker")) + "\", innerVar = \"" +
             lua_escape(quest_flag_variable(modid, q.id, "inner")) + "\"";
        if (q.is_pray() || q.is_spot() || q.is_ambush() || q.has_keepsake()) {
            // "x,y,z" -> { x, y, z }; a spot the generator did not write is nil, and the
            // Watch has nothing to measure against. (A find with a keepsake has the spot
            // the keepsake is laid at, and no token.)
            std::string spot;
            std::string part;
            int parts = 0;
            for (char c : q.spot_pos + ",") {
                if (c == ',') {
                    if (!part.empty() && std::strspn(part.c_str(), "0123456789.-+eE ") == part.size()) {
                        spot += (parts ? ", " : "") + part;
                        ++parts;
                    }
                    part.clear();
                } else {
                    part += c;
                }
            }
            if (parts == 3) s += ", spot = { " + spot + " }";
            if (!q.has_keepsake()) s += ", token = \"" + lua_escape(token_class(q)) + "\"";
        }
        if (q.has_keepsake()) {
            // The keepsake laid at spot when the errand is taken up (LayKeepsake), and
            // what Henry's pocket is watched for (WatchKeepsake).
            s += ", keepsake = \"" + lua_escape(q.keepsake_class) + "\", keepsakeName = \"" +
                 lua_escape(q.keepsake_name) + "\"";
        }
        if (q.is_spot() || q.is_ambush()) {
            if (q.hour_from >= 0 && q.hour_to >= 0 && q.hour_from != q.hour_to) {
                s += ", hours = { " + std::to_string(q.hour_from) + ", " + std::to_string(q.hour_to) + " }";
            }
        }
        if (q.is_ambush()) {
            s += ", stageVar = \"" + lua_escape(quest_flag_variable(modid, q.id, "stage")) + "\"";
        }
        if (q.is_buy()) {
            s += ", carry = \"" + lua_escape(q.carry_class) + "\", carryName = \"" + lua_escape(q.carry_name) +
                 "\", carryVar = \"" + lua_escape(quest_flag_variable(modid, q.id, "carry")) + "\"";
        }
        if (q.is_spot()) {
            s += ", thing = \"" + lua_escape(q.item_name) + "\"";
            if (!q.cue_sound.empty()) s += ", cue = \"" + lua_escape(q.cue_sound) + "\"";
            const std::string loot = lua_class_list(q.loot_classes);
            if (!loot.empty()) s += ", loot = { " + loot + " }";
        }
        if (q.is_ask()) {
            const std::string gift = lua_class_list(q.gift_classes);
            if (!gift.empty()) s += ", gift = { " + gift + " }";
        }
        if (q.carries_on()) {
            // The word carried on: who the giver sends Henry to once told (the `onward`
            // beat pays and sends; the one sent to's own hand row completes, paying nothing
            // again), and the metarole their menu speaks through.
            s += ", onward = \"" + lua_escape(q.onward_soul) + "\", onwardMeta = \"" +
                 lua_escape(onward_metarole(q.id)) + "\"";
        }
        if (!q.reward_class.empty()) {
            s += ", rewardClass = \"" + lua_escape(q.reward_class) + "\"";
        }
        if (!q.hail.empty()) {
            // The giver's shout when Henry comes near with the errand unasked (WatchHail):
            // the line, the name over it, whose stand-in voice the app gives it, and the
            // token that asks the graph to put it in his mouth (hail_class).
            s += ", hail = \"" + lua_escape(q.hail) + "\", hailName = \"" + lua_escape(q.giver_name) +
                 "\", hailVoice = \"" + (q.giver_gender == "female" ? "female" : "male") +
                 "\", hailToken = \"" + lua_escape(hail_class(q)) + "\"";
            // The giver's day (hail_hours): no shout outside it, as her marker is off the
            // map outside it (the graph's giverDay).
            if (q.has_hail_hours()) {
                s += ", hailHours = { " + std::to_string(q.hail_hour_from) + ", " + std::to_string(q.hail_hour_to) + " }";
            }
        }
        if (!q.dress_target.empty()) {
            // A clothing preset put on the holder once a session (WatchDress).
            s += ", dress = \"" + lua_escape(q.dress_target) + "\"";
        }
        // An unyielding holder (In the Stocks): the menu's rows are refusals, and the store
        // notes the rebuff (QuestRebuffed) for the app's giver to send Henry at the chest.
        if (q.is_unyielding()) s += ", unyielding = true";
        if (q.takes_letter_of()) {
            // The thing is the Document an earlier quest's dictation wrote, whichever variant:
            // QuestFlags names the class from that quest's digits in the store (letterOf).
            s += ", letterOf = \"" + lua_escape(q.item_of) + "\"";
        }
        if (q.has_expose()) {
            // An exposure (WatchExpose): the one the holder is caught with, the night window,
            // the global the menu's expose row reads, and the token for the graph's caught
            // trigger.
            s += ", seenWith = \"" + lua_escape(q.seen_with) + "\", seenVar = \"" +
                 lua_escape(quest_flag_variable(modid, q.id, "seen")) + "\", hours = { " +
                 std::to_string(q.hour_from) + ", " + std::to_string(q.hour_to) + " }, token = \"" +
                 lua_escape(token_class(q)) + "\"";
            // Henry's aside as the holder goes in to the witness (WatchExpose, enters).
            if (!q.holder_enters.empty()) s += ", enters = \"" + lua_escape(q.holder_enters) + "\"";
            // Where the bed is, for the door and the street to be anchored on (NearHome).
            if (q.has_bed_spot()) {
                char spot[96];
                std::snprintf(spot, sizeof spot, ", bedPos = { x = %.2f, y = %.2f }", q.bed_x, q.bed_y);
                s += spot;
            }
            // The holder turning on Henry in the street (GraphSaid): the walk's hours about
            // the night window, the global his Talk reads to open on him, and the token for
            // the graph's followed trigger.
            if (q.has_follow()) {
                double from = q.hour_from - kFollowHoursBefore;
                if (from < 0) from += 24;   // HourIn wraps a window that crosses midnight
                s += ", followHours = { " + health_text(from) + ", " +
                     health_text(std::fmod(q.hour_from + kFollowHoursAfter, 24.0)) + " }, followedVar = \"" +
                     lua_escape(quest_flag_variable(modid, q.id, "followed")) + "\", followToken = \"" +
                     lua_escape(follow_class(q)) + "\"";
                // Henry to himself: the holder near on his walk, and the night lost.
                if (!q.holder_follow_warn.empty()) s += ", followWarn = \"" + lua_escape(q.holder_follow_warn) + "\"";
                if (!q.holder_follow_over.empty()) s += ", followOver = \"" + lua_escape(q.holder_follow_over) + "\"";
                // His own row for when his Talk never opens on the request (FollowSaid): his
                // line and Henry's answer, with his name and voice for the narrative line.
                s += ", followedLine = \"" + lua_escape(q.holder_followed) + "\", followedReply = \"" +
                     lua_escape(q.holder_followed_reply) + "\", holderName = \"" + lua_escape(q.target_name) +
                     "\", holderVoice = \"" + (q.target_gender == "female" ? "female" : "male") + "\"";
                // The night's marker (WatchMarker): on him from his homecoming (routine_home_at,
                // else the street watch's start) to the night window's end, off otherwise and
                // for a `followed` night; the two tokens for the graph's markShown/markHidden.
                const double home = q.home_at >= 0 ? q.home_at : from;
                s += ", markHours = { " + health_text(home) + ", " + std::to_string(q.hour_to) + " }, markShown = \"" +
                     lua_escape(mark_shown_class(q)) + "\", markHidden = \"" + lua_escape(mark_hidden_class(q)) + "\"";
            }
        }
        if (q.has_fast_travel()) {
            // The giver's carrying (FastTravelGo): the spot and the hour of the day.
            double x = 0, y = 0, z = 0;
            std::sscanf(q.fast_travel_to.c_str(), "%lf,%lf,%lf", &x, &y, &z);
            char spot[128];
            std::snprintf(spot, sizeof spot, ", fastTravel = { x = %.2f, y = %.2f, z = %.2f, at = %.2f }", x, y, z,
                          q.fast_travel_at);
            s += spot;
        }
        if (q.has_bedside()) {
            // The bedside (WatchExpose): Henry's waking line, said in his voice over the bed
            // at the catching, the global the Talk reads for a denouncing, and the
            // blackmail's price.
            s += ", wake = \"" + lua_escape(q.holder_wake) + "\", denouncedVar = \"" +
                 lua_escape(quest_flag_variable(modid, q.id, "denounced")) + "\", blackmail = " +
                 std::to_string(q.blackmail_groschen);
        }
        if (q.has_chest()) {
            // The chest the thing lies in (WatchChest spawns it at the spot when Henry comes
            // near, locked at chestLock and keyed to keyClass, and moves the thing into it).
            std::string spot;
            std::string part;
            int parts = 0;
            for (char c : q.chest_pos + ",") {
                if (c == ',') {
                    if (!part.empty() && std::strspn(part.c_str(), "0123456789.-+eE ") == part.size()) {
                        spot += (parts ? ", " : "") + part;
                        ++parts;
                    }
                    part.clear();
                } else {
                    part += c;
                }
            }
            // Three figures are the spot; a fourth is the chest's yaw in degrees.
            if (parts == 3 || parts == 4) s += ", chest = { " + spot + " }";
            s += ", chestLock = " + health_text(q.chest_lock) + ", keyClass = \"" + lua_escape(key_class(q)) + "\"";
            // The token the watch makes when the thing has left the chest (the graph's taken
            // trigger), and the paper laid beside the thing, where one is written.
            s += ", taken = \"" + lua_escape(taken_class(q)) + "\"";
            if (q.has_paper()) s += ", paper = \"" + lua_escape(paper_class(q)) + "\"";
        }
        if (q.has_helper()) {
            // The one who can let Henry in: her soul and the metarole her menu speaks through
            // (GrantHolder), her name for the log, what she asks for the door and the key
            // (QuestHelper takes it), and the naming token (Token, on the app's `named`).
            s += ", helper = \"" + lua_escape(q.helper_soul) + "\", helperMeta = \"" +
                 lua_escape(helper_metarole(q.id)) + "\", helperName = \"" + lua_escape(q.helper_name) +
                 "\", doorPrice = " + std::to_string(q.helper_door_price) +
                 ", token = \"" + lua_escape(token_class(q)) + "\"";
            if (q.has_key()) s += ", keyPrice = " + std::to_string(q.helper_key_price);
        }
        if (q.is_travel() && !q.seen_mood.empty() && q.seen_mood != "none") {
            // The fa_idle_<mood>_01 clip; "arrogant" is spelt the game's way in the clip
            // name. The Lua checks the clip exists (GetAnimationLength > 0) before playing.
            const std::string mood = q.seen_mood == "arrogant" ? "arogant" : q.seen_mood;
            s += ", mood = \"fa_idle_" + lua_escape(mood) + "_01\"";
        }
        if (q.is_mind()) {
            s += ", gap = " + std::to_string(kMindGapHours) + ", checksVar = \"" +
                 lua_escape(quest_flag_variable(modid, q.id, "checks")) + "\", dueVar = \"" +
                 lua_escape(quest_flag_variable(modid, q.id, "due")) + "\"";
        }
        if (q.has_dictation()) {
            // A letter Henry writes at dictation (npcdb.hpp, dictate): no letter at accept;
            // the app's `wrote_<digits>` makes the Document of that combination
            // (letterPrefix .. digits .. letterSuffix is its class, LetterClass) and the
            // store's digits set the receiver's reading globals (QuestFlags: partVars by
            // part, verdictVar by DictationVerdict, cursed where a curseParts part is as
            // said, mocked where any is made larger). The token tells the graph it is
            // written (log_written), where the journal has the line.
            s += ", dictated = true, letterBase = \"" + lua_escape(q.item_class) + "\", letterPrefix = \"" +
                 lua_escape(letter_class_prefix()) + "\", letterSuffix = \"" + lua_escape(letter_class_suffix(q)) +
                 "\", partVars = { ";
            for (int k = 0; k < Quest::kDictationParts; ++k) {
                s += std::string(k ? ", " : "") + "\"" +
                     lua_escape(quest_flag_variable(modid, q.id, "p" + std::to_string(k + 1))) + "\"";
            }
            s += " }, verdictVar = \"" + lua_escape(quest_flag_variable(modid, q.id, "verdict")) + "\", curseParts = { ";
            for (std::size_t i = 0; i < q.curse_parts.size(); ++i) {
                s += std::string(i ? ", " : "") + std::to_string(q.curse_parts[i]);
            }
            s += " }";
            if (q.has_written_log()) s += ", token = \"" + lua_escape(token_class(q)) + "\"";
        }
        s += " },\n";
    }
    s += "}\n";
    s += "-- Soul name to every quest it is party to, either side, in the order the quests\n";
    s += "-- are dealt: a chain's giver is on each of its links, a nest's holder gives the\n";
    s += "-- inner favour. questBySoul is the first of them, for the older readers of it;\n";
    s += "-- the begin line reports how many of each quest's item the player is carrying,\n";
    s += "-- which is what tells the app a giver is being approached by someone holding\n";
    s += "-- what they asked for.\n";
    s += g + ".questsBySoul = {}\n";
    for (const Quest& q : quests) {
        for (const std::string* soul : {&q.giver_soul, &q.target_soul, &q.onward_soul, &q.helper_soul}) {
            if (soul->empty()) continue;   // a find's empty target; a word not carried on; no helper
            s += g + ".questsBySoul[\"" + lua_escape(*soul) + "\"] = " + g + ".questsBySoul[\"" +
                 lua_escape(*soul) + "\"] or {}\n";
            s += "table.insert(" + g + ".questsBySoul[\"" + lua_escape(*soul) + "\"], \"" +
                 lua_escape(q.id) + "\")\n";
        }
    }
    s += g + ".questBySoul = {}\n";
    s += "for soul, ids in pairs(" + g + ".questsBySoul) do " + g + ".questBySoul[soul] = ids[1] end\n";
    s += g + ".questQueue = {}\n";
    s += g + ".questCode = 0\n";
    s += g + ".questArmedAt = 0\n";
    s += "-- Ticks a code is left up before the pump concludes the graph is not going to\n";
    s += "-- play it and moves on. A beat is 2.5s and a tick 0.4s, so this is several beats.\n";
    s += g + ".questPatience = 25\n";
    s += "-- Lines said so far, counted across the whole run of the game rather than per\n";
    s += "-- conversation, because that is how the writer numbers them. There is no\n";
    s += "-- budget any more: nothing a line touches is spent, since the words go through\n";
    s += "-- Game.SendInfoText, which takes the text itself and not an id that would keep\n";
    s += "-- the first wording it was given.\n";
    s += g + ".turn = 0\n";
    s += "-- The last hook_N.txt run, counted the same way. See Hook.\n";
    s += g + ".hooked = 0\n";
    s += "-- The console pump (Console): which chain of ticks is the live one, and when it\n";
    s += "-- last ticked, so a chain the engine wiped is told from one still running.\n";
    s += g + ".consoleGen = 0\n";
    s += g + ".consoleAt = -1\n";
    s += g + ".words = nil\n";
    s += "-- Whether the turn being said was flagged @more -- the reply is still being written\n";
    s += "-- and this is the text so far -- and whether the last one was, which is when the\n";
    s += "-- line is looked for again at streamMillis rather than waiting on the pump.\n";
    s += g + ".more = false\n";
    s += "-- Whether the turn being said was flagged @beat: a written line told in two beats,\n";
    s += "-- the next turn being the speaker's second, so Henry's turn waits for that one.\n";
    s += g + ".beat = false\n";
    s += g + ".streaming = false\n";
    s += g + ".streamArmed = false\n";
    s += g + ".streamMillis = 100\n";
    s += "-- The tick the line on screen has had its time, so the next does not replace it\n";
    s += "-- before it has been read.\n";
    s += g + ".hold = 0\n";
    s += "\n";
    s += "-- Which road the words take to the screen. \"draw\" is System.DrawText, which puts\n";
    s += "-- them where it is told at the size it is given; \"info\" is Game.SendInfoText, the\n";
    s += "-- widget the game shows its own notices in, which decides all of that itself and\n";
    s += "-- queues what it is handed behind anything it was already saying.\n";
    s += "--\n";
    s += "-- Drawing costs a frame timer, since it lasts exactly one frame and has to be\n";
    s += "-- asked for again, and is set back to info text for the rest of a conversation if\n";
    s += "-- the call is refused.\n";
    s += g + ".mode = \"" + lua_escape(screen.mode) + "\"\n";
    s += "-- Fractions of the viewport, not pixels: x of the middle of the line and y of\n";
    s += "-- its top. DrawText itself wants pixels, and the screen this was written on is\n";
    s += "-- 3840x1600, so the same pixels elsewhere would be somewhere else.\n";
    s += g + ".drawX = " + trim_number(screen.x) + "\n";
    s += g + ".drawY = " + trim_number(screen.y) + "\n";
    s += g + ".drawSize = " + trim_number(screen.size) + "\n";
    s += "-- The only name the engine is known to answer to. Of the four definitions in\n";
    s += "-- Engine.pak/Fonts, three are the monospaced VeraMono that debug text uses and\n";
    s += "-- subtitles is the period AlexanderQuill, but asking DrawText for that name drew\n";
    s += "-- the monospace anyway and warned on every paint.\n";
    s += g + ".drawFont = \"" + lua_escape(screen.font) + "\"\n";
    s += "-- Pixels a character stands per unit of size, by eye, because nothing in the\n";
    s += "-- bindings measures a string and a line has to be measured to be centred.\n";
    s += g + ".drawPixels = " + trim_number(screen.pixels) + "\n";
    s += g + ".repaintMillis = " + std::to_string(screen.repaint_millis) + "\n";
    s += "-- The words on screen now, where they were worked out to go, and whether the\n";
    s += "-- timer that keeps them there is running. All three only mean anything while\n";
    s += "-- drawing.\n";
    s += g + ".shown = \"\"\n";
    s += "-- And, in the caption, the line over them: the widget has a second field, the one\n";
    s += "-- the game puts a speaker's name in, drawn in gold on its own row above the text.\n";
    s += "-- It takes a whole sentence and wraps it, which is what puts one line above another\n";
    s += "-- here -- a newline in the text field breaks the line but leaves the upper one\n";
    s += "-- blank, measured. Empty means the row is not drawn.\n";
    s += g + ".above = \"\"\n";
    s += g + ".shownX = 0\n";
    s += g + ".shownY = 0\n";
    s += g + ".painting = false\n";
    s += "-- Said once a run: whether the game's own caption has been asked to get out of\n";
    s += "-- the way. It is asked on every conversation, since a reload rebuilds the UI, but\n";
    s += "-- there is nothing to learn from hearing about it twice.\n";
    s += g + ".captionTold = false\n";
    s += g + ".wroteTold = false\n";
    // The last thing the caption was told, so it is only told again when the words change.
    // nil rather than "" so that clearing an empty one still goes out once.
    s += g + ".wrote = nil\n";
    // The instances every UI call here is aimed at, -1 meaning all of them.
    s += g + ".hudIds = { -1, 0 }\n";
    s += "-- Paints between one assertion of our line in the game's caption and the next, or\n";
    s += "-- zero for no assertion at all.\n";
    s += "--\n";
    s += "-- The caption takes itself down about a second after it is set, and saying the same\n";
    s += "-- thing again resets that: written once a line, the words showed for a second and\n";
    s += "-- then left three seconds of nothing until the next one. So the assertion is not\n";
    s += "-- there to win the field back from the dialogue, it is there to keep the widget's\n";
    s += "-- own timer from running out, and the rate only has to beat a second. Six paints at\n";
    s += "-- the 49 a second the pump runs was about eight a second, and looked steady, but a\n";
    s += "-- long line held through a few beats of the camera loop still lost a frame or two\n";
    s += "-- now and then (Joan's answer to what is wrong; notes.md, 8 Oct 2026): a wordless\n";
    s += "-- response beginning still empties the field, and whatever passes between that\n";
    s += "-- and our next write is a blink. Every paint, then, which is a frame: the widget\n";
    s += "-- was already seen not to mind being told the same thing 49 times a second.\n";
    s += g + ".captionEvery = 1\n";
    s += "-- Said once each: the size of the screen we are drawing on, and whether the font\n";
    s += "-- has been asked for. LoadFont is what makes a font available to be selected, and\n";
    s += "-- one that could not be loaded is not one to keep asking DrawText for.\n";
    s += g + ".viewTold = false\n";
    s += g + ".fontTried = false\n";
    s += "-- Which of DrawText's call shapes the engine took, once it is known: 0 for not\n";
    s += "-- tried yet, 1 for all ten arguments, 2 for position and words alone. drawTold\n";
    s += "-- keeps a refusal from being logged on every frame of the conversation that\n";
    s += "-- follows it.\n";
    s += g + ".drawShape = 0\n";
    s += g + ".drawTold = false\n";
    s += "-- Lines drawn this conversation and the pump tick it started counting from,\n";
    s += "-- which together are how often the words are really going back up.\n";
    s += g + ".paints = 0\n";
    s += g + ".paintFrom = 0\n";
    s += g + ".rateTold = false\n";
    s += "\n";
    s += "-- Where a turn's words are looked for. All of these were measured readable: the\n";
    s += "-- engine resolves a path through its own archive layer and a loose file came\n";
    s += "-- back under every one of them, which is the finding this whole design rests on,\n";
    s += "-- since it is how words decided outside the game get inside it. The writer drops\n";
    s += "-- a copy under all of them and the first that reads is remembered.\n";
    s += g + ".inboxes = {\n";
    for (const std::string& inbox : inboxes) {
        s += "\t\"" + lua_escape(inbox) + "\",\n";
    }
    s += "}\n";
    s += g + ".inbox = nil\n";
    s += "\n";
    s += "-- A conversation in progress: who is in it, and whether the pump should keep\n";
    s += "-- looking for the next thing to say.\n";
    s += g + ".active = false\n";
    s += g + ".npc = nil\n";
    s += g + ".user = nil\n";
    s += g + ".ticks = 0\n";
    s += "-- How far the player can drift before the conversation is taken as over,\n";
    s += "-- in metres. Generous, because ending one by mistake is worse than a line\n";
    s += "-- carrying a little further than it should.\n";
    s += g + ".range = 6\n";
    s += g + ".gap = 0\n";
    s += "-- Whether the session has been asked for. Once, per conversation: asking again\n";
    s += "-- per line is what replayed the greeting every turn and left the pacing to a\n";
    s += "-- behaviour tree that honoured some requests half a minute late and some never.\n";
    s += g + ".opened = false\n";
    s += g + ".openedAt = 0\n";
    s += g + ".pushed = false\n";
    s += "-- How long a session may stay open without a single beat before it is cut, in\n";
    s += "-- pump ticks of 400ms. A beat is a few seconds, so this is generous; it exists\n";
    s += "-- because a graph resting where it will not move leaves the player no way out\n";
    s += "-- at all, movement being the thing a focused dialogue takes away first.\n";
    s += g + ".stuckTicks = 37\n";
    s += "-- The app's heartbeat: it writes alive_<bucket>.txt beside the turn files, the bucket\n";
    s += "-- being the Unix time in whole aliveBucket seconds, and takes the old ones away. A\n";
    s += "-- name per bucket because the engine caches a path once read, so one file rewritten\n";
    s += "-- would read as alive for the rest of the run. Looked for once when a conversation\n";
    s += "-- opens (AppAlive); nothing readable for this bucket or its neighbours is an app that\n";
    s += "-- is not running, and the player is told so in the dialogue instead of being left\n";
    s += "-- with somebody who never answers (notes.md, 5 Oct 2026). The same figure is in\n";
    s += "-- app/src/turnfile.h (kAliveBucketSeconds).\n";
    s += g + ".aliveBucket = " + std::to_string(kAliveBucketSeconds) + "\n";
    s += "-- And the backstop for an app that was up when the conversation opened and has died\n";
    s += "-- since, or is up and not answering: no first line this many pump ticks of 400ms\n";
    s += "-- after the session opened, and the player is told the app did not answer (appBusyLine)\n";
    s += "-- rather than left standing in the camera. A written greeting is on disk before the\n";
    s += "-- press; a model's opening line takes a few seconds, a cold prefill ten or so; this is\n";
    s += "-- past those and short of the point where the player gives up and holds to leave,\n";
    s += "-- which on the Windows laptop was about five seconds (29 Oct 2026).\n";
    s += g + ".appTicks = 30\n";
    s += g + ".appChecked = false\n";
    s += g + ".appDown = false\n";
    s += g + ".turnAtOpen = 0\n";
    s += g + ".appDownLine = \"" + lua_escape(kAppDownLine) + "\"\n";
    s += g + ".appBusyLine = \"" + lua_escape(kAppBusyLine) + "\"\n";
    s += "-- Set for a line that is the mod's own and not the other party's: shown bare, with\n";
    s += "-- no speaker's name in front (Named).\n";
    s += g + ".bare = false\n";
    s += "-- How long Leave waits for the graph to close itself before forcing it, in ms.\n";
    s += "-- It has to outlast a beat: the ending condition is only read where one beat\n";
    s += "-- hands over to the next, so a shorter wait always fires while a session that is\n";
    s += "-- about to end on its own is still up. Measured closes land 2 to 3.3s after the\n";
    s += "-- button, against a beat of six.\n";
    s += g + ".cutDelay = 8000\n";
    s += "-- Beats the shell has reported. The only sign the loop is still turning, since\n";
    s += "-- it says nothing out loud.\n";
    s += g + ".beats = 0\n";
    s += g + ".beatLimit = 2000\n";
    s += g + ".missing = {}\n";
    s += "\n";
    s += "-- The same action the base game starts quest dialogue with: A on an Xbox pad,\n";
    s += "-- cross on PlayStation, the use key on a keyboard. It reaches the interactor\n";
    s += "-- from GetActions, and needs no explicit actionMap, exactly as the base call\n";
    s += "-- that offers it does.\n";
    s += g + ".action = \"talk\"\n";
    s += "\n";
    s += "-- Each of these merged its own copy of BasicAIActions.GetActions via\n";
    s += "-- table.Merge, so wrapping BasicAIActions now would not reach any of them.\n";
    s += g + ".classNames = { \"NPC\", \"NPC_Female\", \"NPC_NAI\" }\n";
    s += "\n";
    s += "-- The one button a conversation answers to: tapped it moves the words on, held it\n";
    s += "-- leaves. Holding is the way out because the focused camera takes movement away,\n";
    s += "-- so walking off -- the exit this was built around -- cannot happen at all.\n";
    s += "--\n";
    s += "-- Player:OnAction is the only input the engine hands to Lua. The names come from\n";
    s += "-- the dialog actionmap in Libs/Config/defaultProfile.xml, the map that holds\n";
    s += "-- while a dialogue is up, where B is bound to both of them. Not ui_back, which\n";
    s += "-- the comment in the shipped player.lua names and which no longer exists anywhere\n";
    s += "-- in the profile.\n";
    s += "--\n";
    s += "-- Both are listened to because one press of B delivers both, and because\n";
    s += "-- keybindSuperactions.xml splits them for a keyboard: dialog_skip answers to space\n";
    s += "-- and right mouse, while dialog_cancel sits with escape, which a dialogue spends\n";
    s += "-- on the pause menu instead. Only dialog_skip covers both devices.\n";
    s += g + ".buttonActions = { [\"dialog_cancel\"] = true, [\"dialog_skip\"] = true }\n";
    s += "-- How long the button has to be down to mean leaving rather than skipping, in ms.\n";
    s += "--\n";
    s += "-- A quarter second, which is the game's own answer rather than a taste: both of the\n";
    s += "-- actions above declare holdTriggerDelay=\"0.25\" in Libs/Config/defaultProfile.xml,\n";
    s += "-- and 48 of the 72 hold delays in that file say the same. A hold that took longer\n";
    s += "-- than the game's would feel like one that had not registered.\n";
    s += "--\n";
    s += "-- Still timed here rather than taken from the engine: dialog_skip declares no\n";
    s += "-- onHold at all, so a keyboard would have nothing to go on, and one clock for both\n";
    s += "-- devices is one behaviour to reason about.\n";
    s += g + ".holdMillis = 250\n";
    s += g + ".down = false\n";
    s += "-- Hold timers armed and not yet fired. What it is for is the timer left over from\n";
    s += "-- an earlier press landing inside a later one, which would leave a conversation a\n";
    s += "-- quarter second into a press the player had only just begun. Only the last timer\n";
    s += "-- outstanding is allowed to act, and since every press arms one, the last one to\n";
    s += "-- fire is the one whose press has actually lasted.\n";
    s += "--\n";
    s += "-- Counted rather than timed because the previous guard was timed and broke: it\n";
    s += "-- asked whether the 400ms pump had ticked since the press, which a 500ms hold\n";
    s += "-- always outlasted and a 250ms hold does not.\n";
    s += g + ".pending = 0\n";
    s += "\n";
    s += "-- Typing a reply, for testing a conversation without a microphone. The keyboard is\n";
    s += "-- two actionmaps in the XML named below, loaded at start-up: one that holds F9 and\n";
    s += "-- is on for the length of a conversation, and an exclusive one that binds every\n";
    s += "-- key a line could need and is on only while typing, so the game sees none of\n";
    s += "-- them. Each key arrives at Player:OnAction as mtype_<key>.\n";
    s += "--\n";
    s += "-- It is Henry's turn once a line of the other party's has been played and nothing\n";
    s += "-- is owed: typing opens by itself then, Enter prints the line as BRIDGE said for\n";
    s += "-- the app to answer, and the reply's arrival is what opens it again. Esc leaves\n";
    s += "-- the conversation; F9 puts the keyboard away until the next turn, or brings it\n";
    s += "-- back, by hand.\n";
    s += g + ".typingXml = \"" + lua_escape(typing_actionmaps_path(modid)) + "\"\n";
    s += g + ".typingToggleMap = \"" + lua_escape(modid) + "_typing_toggle\"\n";
    s += g + ".typingKeysMap = \"" + lua_escape(modid) + "_typing\"\n";
    s += g + ".typingLoaded = false\n";
    s += "-- Whether the pump opens the keyboard by itself after a line (TypingCheck). Off: the\n";
    s += "-- \"Henry: _\" prompt was the early prototype's, and free-form speech is to come in\n";
    s += "-- another way. F9 still opens the keyboard by hand.\n";
    s += g + ".typingAuto = false\n";
    s += "-- Whether the exclusive map is up and keys are being collected.\n";
    s += g + ".typing = false\n";
    s += g + ".typed = \"\"\n";
    s += g + ".shift = false\n";
    s += "-- A line has been sent and its answer has not arrived. Nothing opens typing while\n";
    s += "-- this holds, or the player would be asked to speak over a reply being written.\n";
    s += g + ".awaiting = false\n";
    s += "-- The other party has said something this conversation, so there is a line to\n";
    s += "-- answer. Typing never opens before the greeting.\n";
    s += g + ".heard = false\n";
    s += "-- Esc was pressed: typing stays down until the next turn, or F9.\n";
    s += g + ".typingDismissed = false\n";
    s += "-- The cursor under the typed line, and the timer that blinks it. Blinking is what\n";
    s += "-- says a field is live: an underscore that never moves reads as punctuation.\n";
    s += g + ".cursorOn = true\n";
    s += g + ".blinkArmed = false\n";
    s += g + ".blinkMillis = 500\n";
    s += "-- What stands in for the cursor while it is off: two no-break spaces, which the\n";
    s += "-- field keeps where it would trim a plain one, and which together measure the\n";
    s += "-- underscore's width, so the plate under the line does not breathe with the\n";
    s += "-- blink. Not a coloured underscore: the field strips a <font> tag rather than\n";
    s += "-- honouring it, measured.\n";
    s += g + ".cursorOff = \"\\194\\160\\194\\160\"\n";
    s += "-- Who the typed line belongs to, in front of it and over the reply it waits for.\n";
    s += "-- The game has one protagonist, so this is not looked up.\n";
    s += g + ".playerName = \"Henry\"\n";
    s += "-- The line the player last sent, kept up over the reply's placeholder.\n";
    s += g + ".said = nil\n";
    s += "-- A US layout, by hand, since an action carries a key name and not a character.\n";
    s += g + ".typedShifted = {\n";
    s += "\t[\"1\"] = \"!\", [\"2\"] = \"@\", [\"3\"] = \"#\", [\"4\"] = \"$\", [\"5\"] = \"%\",\n";
    s += "\t[\"6\"] = \"^\", [\"7\"] = \"&\", [\"8\"] = \"*\", [\"9\"] = \"(\", [\"0\"] = \")\",\n";
    s += "\tcomma = \"<\", period = \">\", apostrophe = \"\\\"\", minus = \"_\", slash = \"?\",\n";
    s += "\tsemicolon = \":\",\n";
    s += "}\n";
    s += g + ".typedPlain = {\n";
    s += "\tcomma = \",\", period = \".\", apostrophe = \"'\", minus = \"-\", slash = \"/\",\n";
    s += "\tsemicolon = \";\", space = \" \",\n";
    s += "}\n";
    s += "\n";
    s += "-- System.Log is filtered out of kcd.log at the default verbosity, which makes a\n";
    s += "-- working mod look like one that never loaded, so prefer LogAlways.\n";
    s += "function " + g + ".Log(text)\n";
    s += "\tlocal write = System.LogAlways or System.Log\n";
    s += "\twrite(\"[" + modid + "] \" .. text)\n";
    s += "end\n";
    s += "\n";
    s += "-- GetActions runs while an NPC is merely being looked at, so anything logged\n";
    s += "-- from it has to be said once per subject rather than once per frame.\n";
    s += g + ".logged = {}\n";
    s += "function " + g + ".LogOnce(key, text)\n";
    s += "\tif " + g + ".logged[key] then return end\n";
    s += "\t" + g + ".logged[key] = true\n";
    s += "\t" + g + ".Log(text)\n";
    s += "end\n";
    s += "\n";
    s += "function " + g + ".HasMeta(npc, name)\n";
    s += "\tif npc.soul == nil or npc.soul.GetMetaRoles == nil then return false end\n";
    s += "\tlocal ok, got = pcall(function() return npc.soul:GetMetaRoles() end)\n";
    s += "\tif not ok then return false end\n";
    s += "\tif type(got) == \"table\" then\n";
    s += "\t\tfor k, v in pairs(got) do\n";
    s += "\t\t\tif tostring(v) == name or tostring(k) == name then return true end\n";
    s += "\t\tend\n";
    s += "\t\treturn false\n";
    s += "\tend\n";
    s += "\treturn tostring(got) == name\n";
    s += "end\n";
    s += "\n";
    // What a soul currently carries, for the log. GetMetaRoles is the only way to
    // see any of this from Lua, and whether it hands back a table or a string is
    // not documented, so cope with both.
    s += "function " + g + ".MetaRoles(npc)\n";
    s += "\tif npc.soul == nil or npc.soul.GetMetaRoles == nil then return \"<unavailable>\" end\n";
    s += "\tlocal ok, got = pcall(function() return npc.soul:GetMetaRoles() end)\n";
    s += "\tif not ok then return \"<raised \" .. tostring(got) .. \">\" end\n";
    s += "\tif type(got) == \"table\" then\n";
    s += "\t\tlocal names = {}\n";
    s += "\t\tfor k, v in pairs(got) do names[#names + 1] = tostring(v) end\n";
    s += "\t\ttable.sort(names)\n";
    s += "\t\treturn \"(\" .. #names .. \") \" .. table.concat(names, \" \")\n";
    s += "\tend\n";
    s += "\treturn tostring(got)\n";
    s += "end\n";
    s += "\n";
    // How the player looks to the soul they walked up to, reported raw and turned into
    // prose for the model by app/src/approach.cpp. An NPC who remarks on the blood down the
    // player's front is worth more than one who says the same thing to everybody.
    //
    // Every read is wrapped, because an unarmed or soulless entity has to come out unread
    // rather than take the conversation down with it.
    s += "function " + g + ".Stat(user, name)\n";
    s += "\tif user == nil or user.soul == nil or user.soul.GetDerivedStat == nil then\n";
    s += "\t\treturn nil\n";
    s += "\tend\n";
    s += "\tlocal ok, got = pcall(function() return user.soul:GetDerivedStat(name) end)\n";
    s += "\tif not ok or type(got) ~= \"number\" then return nil end\n";
    s += "\treturn got\n";
    s += "end\n";
    s += "\n";
    // A different call with a different vocabulary: health, hunger, stamina, exhaust and
    // alcoholism are the five ids the shipped scripts ask for, health in 202 places.
    s += "function " + g + ".State(user, name)\n";
    s += "\tif user == nil or user.soul == nil or user.soul.GetState == nil then\n";
    s += "\t\treturn nil\n";
    s += "\tend\n";
    s += "\tlocal ok, got = pcall(function() return user.soul:GetState(name) end)\n";
    s += "\tif not ok or type(got) ~= \"number\" then return nil end\n";
    s += "\treturn got\n";
    s += "end\n";
    s += "\n";
    // DO NOT sweep GetDerivedStat over a list of guessed ids. An id the engine does not
    // know does not return nil and does not raise a Lua error -- it takes the process down
    // in native code, where pcall cannot reach it, and the player loses their session.
    //
    // This was learned by doing it. 110 three-letter strings were lifted from a contiguous
    // block in WHGame.dll and swept here, on the grounds that bld and drt were both in the
    // block. They were, but the block is not the derived-stat list: the strings immediately
    // around it are wh.rpgmodule.C_BuffPerk, C_CombatTechniquePerk and C_MetaPerk, so it is
    // buff and perk ids that bld and drt happen to share a neighbourhood with. The game
    // crashed on the first conversation after the sweep shipped, with kcd.log ending on the
    // look line directly above the sweep.
    //
    // Only ids confirmed by a source that knows belong here, and the source that knows is
    // the game's own Skald quest modules. Each one that reads a stat spells the id out:
    //
    //     <MemberFunction MethodName="GetDerivedStat" DeclaringType="wh::rpgmodule::I_Soul">
    //         <Constant Name="DerivedStat" Value="rch" />
    //
    // which is the same method on the same interface the mod calls. Seventeen ids are
    // confirmed that way, and five of them are the "relative" family, each normalised in
    // utils/rpg/getrelative*ininterval.xml against its own ScriptParams bounds:
    //
    //     rch  relative charisma   rpg_charisma_low/medium/high
    //     bld  bloodiness          rpg_bld_*
    //     drt  dirtiness           rpg_drt_*
    //     sml  smell               rpg_sml_*
    //     bad  badassness          rpg_bad_*
    //
    // So charisma is rch, not cha. cha survives in two places -- Negotiation.lua under a
    // KCD1 folder, and DialogUtils.ImpressByPlayer, which nothing in the 290 shipped Lua
    // files or any quest ever calls -- so it is a KCD1 id left in dead code, which is
    // exactly why it returned nil through a call that works for everything else.
    //
    // The dual read stays because it costs nothing and it is what proved the object was
    // never at fault: cha came back nil from both.
    s += "function " + g + ".PlayerStat(user, name)\n";
    s += "\tlocal got = " + g + ".Stat(user, name)\n";
    s += "\tif got ~= nil then return got, \"user\" end\n";
    s += "\tif g_localActor ~= nil then\n";
    s += "\t\tgot = " + g + ".Stat(g_localActor, name)\n";
    s += "\t\tif got ~= nil then return got, \"local\" end\n";
    s += "\tend\n";
    s += "\treturn nil, \"none\"\n";
    s += "end\n";
    s += "\n";
    // What the player looks like and what state they are in, reported as the raw numbers
    // the game gave up and nothing more. The judgement -- which of these is worth remarking
    // on, and how it reads in a prompt -- is made in app/src/approach.cpp.
    //
    // It used to be made here, and that was the mistake. The bands lived in this function
    // with copies in personadb.h and public/lib/look.js that had to be edited together, and
    // twice were not: "filthy" sat two bands off the game's own value for a day because only
    // two of the three copies were corrected. Numbers on the wire, judgement in one place.
    s += "function " + g + ".SpeechStat(who)\n";
    s += "\tif who == nil or who.soul == nil or who.soul.GetStatLevel == nil then return nil end\n";
    s += "\tfor _, name in ipairs({\"speech\", \"spc\"}) do\n";
    s += "\t\tlocal ok, v = pcall(function() return who.soul:GetStatLevel(name) end)\n";
    s += "\t\tif ok and type(v) == \"number\" and v > 0 then return v end\n";
    s += "\tend\n";
    s += "\treturn nil\n";
    s += "end\n";
    s += "\n";
    s += "function " + g + ".Seen(user)\n";
    s += "\tlocal rch, rchFrom = " + g + ".PlayerStat(user, \"rch\")\n";
    s += "\tlocal bld = " + g + ".PlayerStat(user, \"bld\")\n";
    s += "\tlocal drt = " + g + ".PlayerStat(user, \"drt\")\n";
    // Confirmed valid by the same standard as the rest: GetDerivitedDrunkennessInInterval.xml
    // passes <Constant Name="DerivedStat" Value="dru" /> to GetDerivedStat on I_Soul. Worth
    // stating because dru is the one relative stat with no rpg_dru_* bands in
    // ScriptParams.xml -- that module takes interval_min and interval_max as plain float
    // ports instead -- and an unbanded stat is not an invalid one.
    s += "\tlocal dru = " + g + ".PlayerStat(user, \"dru\")\n";
    s += "\tlocal hp = " + g + ".State(user, \"health\")\n";
    // Read and logged, neither sent. bad was 0.695271 to 1 across four outfits on one save
    // and sml was 0 in all four, so on this character neither separates one outfit from
    // another. An early-game or deliberately shabby save may say otherwise, and the log is
    // where that would show.
    s += "\tlocal bad = " + g + ".PlayerStat(user, \"bad\")\n";
    s += "\tlocal sml = " + g + ".PlayerStat(user, \"sml\")\n";
    s += "\tlocal drawn = false\n";
    s += "\tif user ~= nil and user.human ~= nil and user.human.IsWeaponDrawn ~= nil then\n";
    s += "\t\tlocal ok, got = pcall(function() return user.human:IsWeaponDrawn() end)\n";
    s += "\t\tdrawn = ok and got == true\n";
    s += "\tend\n";
    // rchFrom, bad and sml go to the log but not to the app, which has no use for them.
    s += "\t" + g + ".Log(\"  read rchFrom=\" .. rchFrom\n";
    s += "\t\t.. \" bad=\" .. tostring(bad) .. \" sml=\" .. tostring(sml))\n";
    // Nakedness has no stat. The game computes it two ways, neither reachable from Lua: a
    // Skald module calling soul:CheckEquipment with an array of EquipmentSlotIds
    // (body_cloth, body_chainmail, leg_trousers and so on), and an AI behaviour tree walking
    // the player's inventory subgraph for equipped items matching armor.TunicShort.* and
    // twenty-two sibling categories. No .lua in the game calls CheckEquipment and no .lua
    // names a slot id, so the Lua spelling of that enum is unknown. This says only whether
    // the function is reachable at all, and deliberately does not call it.
    s += "\tlocal soul = nil\n";
    s += "\tif user ~= nil then soul = user.soul end\n";
    s += "\t" + g + ".Log(\"  naked \" .. " + g + ".Exists(\"checkequip\", soul, \"CheckEquipment\")\n";
    s += "\t\t.. \" \" .. " + g + ".Exists(\"clothlink\", XGenAIModule, \"HasClothingLink\"))\n";
    // tostring on a nil reads "nil", which the app parses as unread -- and unread is a
    // different thing from zero, since zero dirt is a clean player.
    // bad joins the line after all: it never separated one outfit from another, which is why
    // it was left out of the greeting, but it is the game's own reading of how dangerous the
    // player looks, and that is most of what a dread check weighs.
    // And the player's Speech stat, for the one check the app weighs itself: Henry asking
    // a letter's writer what is in it (approach.h). soul:GetStatLevel takes the stat's
    // full name in this game ("speech", as CombatDebug.AddStat spells them; the "spc" the
    // KCD1-era DialogUtils.PersuadeByPlayer asks for came back nil on the quest probe), so
    // the long name first and the short one as a fallback. Left off when both fail.
    s += "\tlocal spc = " + g + ".SpeechStat(user ~= nil and user.soul ~= nil and user or g_localActor)\n";
    s += "\tif spc ~= nil then spc = string.format(\" spc=%d\", spc) else spc = \"\" end\n";
    s += "\treturn \" rch=\" .. tostring(rch) .. \" bld=\" .. tostring(bld)\n";
    s += "\t\t.. \" drt=\" .. tostring(drt) .. \" dru=\" .. tostring(dru)\n";
    s += "\t\t.. \" hp=\" .. tostring(hp) .. \" drawn=\" .. tostring(drawn)\n";
    s += "\t\t.. \" bad=\" .. tostring(bad) .. spc\n";
    s += "end\n";
    s += "\n";
    // One read, named, with whatever came back rendered for the log. Reporting the type
    // alongside the value is what turned "stance returns 3" into knowing 3 was a constant
    // rather than an activity.
    s += "function " + g + ".Try(name, call)\n";
    s += "\tlocal ok, got = pcall(call)\n";
    s += "\tif not ok then return name .. \"=<raised \" .. tostring(got) .. \">\" end\n";
    s += "\tif got == nil then return name .. \"=nil\" end\n";
    s += "\tif type(got) == \"table\" then\n";
    s += "\t\tlocal fields = {}\n";
    s += "\t\tfor k, v in pairs(got) do fields[#fields + 1] = tostring(k) end\n";
    s += "\t\ttable.sort(fields)\n";
    s += "\t\treturn name .. \"=table{\" .. table.concat(fields, \",\") .. \"}\"\n";
    s += "\tend\n";
    s += "\treturn name .. \"=\" .. type(got) .. \":\" .. tostring(got)\n";
    s += "end\n";
    s += "\n";
    // What the soul is busy with. GetCurrentAnimationState hands back the Mannequin
    // fragment by name -- LumberJackSawVAR, StonemasonChisel, CampWineDrink,
    // Bartender_TakeAndTapStein, SittingIdle -- which is the finest grain the engine
    // has, and every name it returned is one of the 2,147 in actor_anim_action.xml, so
    // the vocabulary is enumerable offline rather than only discoverable by playing.
    //
    // Sampled from GetActions, not from OnConversation. By the time a conversation
    // opens the NPC has stood up, put the saw down and turned to face the player, so
    // the activity worth naming is already over. Merely being looked at does not
    // interrupt it, which makes the look the only honest moment to ask.
    //
    // Five other calls were tried over one session and are not worth trying again:
    // actor:GetLinkedEntity, entity:GetCurAnimation(0), AI.GetPreviousBehaviorName and
    // AI.GetLastUsedSmartObject all returned nil for every NPC, soul:GetSchedule an
    // empty table, and AI.GetStance a constant 3 -- CryEngine's STANCE_RELAXED -- even
    // for souls the fragment name showed were sitting.
    // The fragment on its own, for the app. Kept separate from the log line because the
    // app wants the name and nothing else, and because it has to be remembered: asking
    // again at OnConversation returns whatever the NPC is doing about being talked to.
    s += g + ".lastAnim = {}\n";
    s += "function " + g + ".Anim(npc)\n";
    s += "\tif npc.actor == nil or npc.actor.GetCurrentAnimationState == nil then\n";
    s += "\t\treturn nil\n";
    s += "\tend\n";
    s += "\tlocal ok, got = pcall(function()\n";
    s += "\t\treturn npc.actor:GetCurrentAnimationState()\n";
    s += "\tend)\n";
    s += "\tif not ok or type(got) ~= \"string\" or got == \"\" then return nil end\n";
    s += "\treturn got\n";
    s += "end\n";
    s += "\n";
    // MotionMovement means walking, which is to say on the way somewhere rather than at
    // anything, and it is what an NPC plays while turning to face the player. Keeping it
    // would overwrite the saw the player actually walked up on.
    s += "function " + g + ".Remember(npc)\n";
    s += "\tlocal anim = " + g + ".Anim(npc)\n";
    s += "\tif anim == nil or anim == \"MotionMovement\" then return end\n";
    s += "\t" + g + ".lastAnim[npc:GetName()] = anim\n";
    s += "end\n";
    s += "\n";
    // What the hour is, which is the one reading that would separate walking to work from
    // walking home. Nothing in the engine says where an NPC is headed -- that was settled
    // by GetLastUsedSmartObject, GetSchedule and GetPreviousBehaviorName all coming back
    // empty, and it follows from how the AI is built: the scheduler moves a soul to its
    // next smart object, so the destination IS the smart object and those were the reads
    // that would have named it. The clock plus SchedulerAlias.xml's own hours -- villager
    // work 7-19, morning 5-7, freetime 18-22 -- is what is left, and it is inference
    // rather than a read.
    //
    // GetTimeOfDayHour is registered but its object is not settled. Its neighbours in the
    // binary are PHYSICPARAM_*, MTL_LAYER_*, NULL_ENTITY and GetGUID, which is System and
    // Entity globals mixed together, so both are tried along with the two names KCD uses
    // for its own clock elsewhere. Logged on its own and not per soul, because the answer
    // is the same for everybody and changes once an hour.
    // All four spellings raised, and the engine's error text is the same useless sentence
    // every time -- "Lua error. Please run with -lua_storedebug 1" -- so calling blind says
    // only that something went wrong, not whether the function is there. Asking the type
    // first separates the two cases without needing the debug flag: "nil" means not
    // registered on that object, "function" means registered and the call itself failed.
    // Existence without invocation, for a function whose arguments are not known. Reach
    // calls what it finds, which is right when the call takes nothing, and wrong for
    // something like CheckEquipment(soul, freeSlots) where the second argument is an array
    // of an engine enum with no Lua spelling anywhere in the shipped scripts. Guessing the
    // representation is how the derived-stat sweep crashed the game, so this reports whether
    // the door exists and stops there.
    s += "function " + g + ".Exists(label, holder, name)\n";
    s += "\tif holder == nil then return label .. \"=<no holder>\" end\n";
    s += "\tlocal ok, kind = pcall(function() return type(holder[name]) end)\n";
    s += "\tif not ok then return label .. \"=<holder raised>\" end\n";
    s += "\treturn label .. \"=\" .. kind\n";
    s += "end\n";
    s += "\n";
    s += "function " + g + ".Reach(label, holder, name)\n";
    s += "\tif holder == nil then return label .. \"=<no holder>\" end\n";
    s += "\tlocal ok, kind = pcall(function() return type(holder[name]) end)\n";
    s += "\tif not ok then return label .. \"=<holder raised>\" end\n";
    s += "\tif kind ~= \"function\" then return label .. \"=<\" .. kind .. \">\" end\n";
    s += "\tlocal fine, got = pcall(function() return holder[name](holder) end)\n";
    s += "\tif not fine then\n";
    // Dot-call as well as method-call: a global like System takes no self, and passing one
    // is enough on its own to raise.
    s += "\t\tlocal bare, plain = pcall(function() return holder[name]() end)\n";
    s += "\t\tif bare then return label .. \"=dot:\" .. tostring(plain) end\n";
    s += "\t\treturn label .. \"=<function, both calls raised>\"\n";
    s += "\tend\n";
    s += "\treturn label .. \"=self:\" .. tostring(got)\n";
    s += "end\n";
    s += "\n";
    s += "function " + g + ".Clock()\n";
    s += "\tlocal parts = {}\n";
    s += "\tlocal function add(text) parts[#parts + 1] = text end\n";
    s += "\tadd(" + g + ".Reach(\"sys.hour\", System, \"GetTimeOfDayHour\"))\n";
    s += "\tadd(" + g + ".Reach(\"sys.tod\", System, \"GetTimeOfDay\"))\n";
    s += "\tadd(" + g + ".Reach(\"cal.hour\", Calendar, \"GetTimeOfDayHour\"))\n";
    s += "\tadd(" + g + ".Reach(\"game.hour\", Game, \"GetTimeOfDayHour\"))\n";
    // Where the game's own scripts would keep a clock if it is not on System at all.
    s += "\tadd(" + g + ".Reach(\"script.hour\", Script, \"GetTimeOfDayHour\"))\n";
    s += "\tadd(" + g + ".Reach(\"xgen.hour\", XGenAIModule, \"GetTimeOfDayHour\"))\n";
    s += "\treturn table.concat(parts, \" \")\n";
    s += "end\n";
    s += "\n";
    // The two soul contexts the app acts on, as fields for the begin marker. Both are also
    // in the Doing probe above, which logs everything it can reach; this reports only the two
    // that change what is said. A sleeper has not seen the player at all, and one already
    // mid-conversation is being interrupted rather than approached.
    s += "function " + g + ".SoulState(npc)\n";
    s += "\tlocal asleep, chatting = false, false\n";
    s += "\tif npc ~= nil and npc.soul ~= nil and npc.soul.HasScriptContext ~= nil then\n";
    s += "\t\tlocal ok, got = pcall(function()\n";
    s += "\t\t\treturn npc.soul:HasScriptContext(\"speech_sleeping\")\n";
    s += "\t\tend)\n";
    s += "\t\tasleep = ok and got == true\n";
    s += "\t\tok, got = pcall(function()\n";
    s += "\t\t\treturn npc.soul:HasScriptContext(\"speech_npcToNpcDialogActive\")\n";
    s += "\t\tend)\n";
    s += "\t\tchatting = ok and got == true\n";
    s += "\tend\n";
    // And where the soul is standing, in world metres, to the metre: the app sets it against
    // the places their routine takes them (npc_routine) to tell a woman at her cellar door
    // from the same woman met on the open road, and keeps Henry's remarks on the cellar for
    // the door (noted 4 Oct 2026: "pre-written options to discuss a cellar" on the road).
    // Left off when the read fails, as the World fields are, so unread and "at the origin"
    // cannot be confused.
    s += "\tlocal at = \"\"\n";
    s += "\tpcall(function()\n";
    s += "\t\tlocal p = npc:GetWorldPos()\n";
    s += "\t\tif type(p.x) == \"number\" and type(p.y) == \"number\" then\n";
    s += "\t\t\tat = string.format(\" at=%.0f,%.0f\", p.x, p.y)\n";
    s += "\t\tend\n";
    s += "\tend)\n";
    // The soul's own Speech stat, the other half of the app's pry check (see Seen).
    s += "\tlocal nspc = " + g + ".SpeechStat(npc)\n";
    s += "\tif nspc ~= nil then nspc = string.format(\" nspc=%d\", nspc) else nspc = \"\" end\n";
    s += "\treturn \" asleep=\" .. tostring(asleep) .. \" chatting=\" .. tostring(chatting) .. at .. nspc\n";
    s += "end\n";
    s += "\n";
    // The world both of them are standing in, which belongs to neither. Two reads, each
    // confirmed by shipped Lua or a shipped quest graph calling it -- see
    // mods/kcd2/time-and-weather.md -- and each pcall-guarded anyway, because a read that
    // raises on a save we have not tried would take the whole conversation down with it and
    // the NPC would simply never answer.
    //
    // Raw numbers, as with the player's stats: which part of the day 16.78 is, and whether
    // 0.24 of rain is a drizzle or a downpour, is decided in app/src/approach.cpp. That is not
    // fastidiousness -- the bands used to be judged here, and the copies in the app and the
    // playground fell out of step twice.
    //
    // A field is LEFT OFF when its read fails rather than sent as a zero, because the app
    // tells those apart and they mean opposite things: no hour at all is passed over in
    // silence, and an hour of 0 is midnight.
    s += "function " + g + ".World()\n";
    s += "\tlocal parts = {}\n";
    s += "\tlocal function add(name, fn)\n";
    s += "\t\tlocal ok, got = pcall(fn)\n";
    s += "\t\tif ok and type(got) == \"number\" then\n";
    s += "\t\t\tparts[#parts + 1] = \" \" .. name .. \"=\" .. tostring(got)\n";
    s += "\t\tend\n";
    s += "\tend\n";
    // GetWorldHourOfDay, not GetTimeOfDayHour: the latter is registered on Entity only, and
    // on none of Calendar, System, Game, Script or XGenAIModule -- all six were probed and
    // came back nil.
    s += "\tif Calendar ~= nil and Calendar.GetWorldHourOfDay ~= nil then\n";
    s += "\t\tadd(\"hour\", function() return Calendar.GetWorldHourOfDay() end)\n";
    s += "\tend\n";
    // 0 is dry and 1 is heavy rain. Cloud cover and wind have no reachable getter, so rain
    // intensity is the whole of what the sky can be asked.
    s += "\tif EnvironmentModule ~= nil and EnvironmentModule.GetRainIntensity ~= nil then\n";
    s += "\t\tadd(\"rain\", function() return EnvironmentModule.GetRainIntensity() end)\n";
    s += "\tend\n";
    s += "\treturn table.concat(parts)\n";
    s += "end\n";
    s += "\n";
    s += "function " + g + ".Doing(npc)\n";
    s += "\tlocal parts = {}\n";
    s += "\tlocal function add(text) parts[#parts + 1] = text end\n";
    s += "\tadd(\"animstate=\" .. tostring(" + g + ".Anim(npc)))\n";
    // Both answer, both were false for everyone awake and alone, so they are kept for
    // the two cases the fragment name alone would not settle: a sleeper should be woken
    // rather than greeted, and one already mid-conversation is being interrupted.
    s += "\tif npc.soul ~= nil then\n";
    s += "\t\tadd(" + g + ".Try(\"asleep\", function()\n";
    s += "\t\t\treturn npc.soul:HasScriptContext(\"speech_sleeping\")\n";
    s += "\t\tend))\n";
    s += "\t\tadd(" + g + ".Try(\"npc2npc\", function()\n";
    s += "\t\t\treturn npc.soul:HasScriptContext(\"speech_npcToNpcDialogActive\")\n";
    s += "\t\tend))\n";
    // Contexts are the one soul read already proven to answer, and these are the few of
    // the 776 that say something a conversation could use. None of them says where an NPC
    // is headed -- there is no such context, which is itself the answer to that question.
    s += "\t\tfor _, name in ipairs({\"situation_isDrunk\", \"crime_sleeping\",\n";
    s += "\t\t\t\t\"activity_seller\", \"activity_cityWalk_standing\",\n";
    s += "\t\t\t\t\"behavior_meditation\", \"situation_beggar\"}) do\n";
    s += "\t\t\tlocal ok, got = pcall(function()\n";
    s += "\t\t\t\treturn npc.soul:HasScriptContext(name)\n";
    s += "\t\t\tend)\n";
    // Only the true ones. Six false readings on every line would bury the animation.
    s += "\t\t\tif ok and got == true then add(\"ctx:\" .. name) end\n";
    s += "\t\tend\n";
    s += "\tend\n";
    // The last read in WHGame.dll that might name what a soul is busy with. It sits
    // directly beside the literal string "XGen AI function" and HasSoulTag, so it is an
    // XGenAIModule call, but no tag vocabulary exists in any table -- so the point of
    // trying it with a plausible tag is the error it raises, which says the arity and
    // whether the tag argument is even a string.
    s += "\tadd(" + g + ".Reach(\"tag\", XGenAIModule, \"HasBehaviorTag\"))\n";
    s += "\treturn table.concat(parts, \" \")\n";
    s += "end\n";
    s += "\n";
    // Whether Lua can read Henry's journal. The binds exist on paper
    // (QuestSystem.IsQuestCompleted and friends on C_ScriptBindQuest) and are
    // rumoured not to be a global until a save is in the world -- the same late
    // arrival as HUD. One look at anyone, plus one pass at install, is enough to
    // settle both moments. Names are guesses from soul prefixes and appearance
    // folders; a raised call still tells us the global was there.
    //
    // The global may be a table or userdata, so it is probed as long as it is not
    // nil. Dot and colon are both tried on the first name: the docs write a dot
    // and CryEngine userdata often wants a colon.
    s += "function " + g + ".QuestProbe(tag, npc, user)\n";
    s += "\tlocal key = \"questprobe-\" .. tag\n";
    s += "\tif " + g + ".logged[key] then return end\n";
    s += "\t" + g + ".logged[key] = true\n";
    s += "\tlocal function dump(label, call) " + g +
        ".Log(\"BRIDGE quest \" .. tag .. \" \" .. " + g + ".Try(label, call)) end\n";
    s += "\t" + g + ".Log(\"BRIDGE quest \" .. tag .. \" begin\")\n";
    s += "\tlocal found = {}\n";
    s += "\tfor k, v in pairs(_G) do\n";
    s += "\t\tlocal n = tostring(k)\n";
    s += "\t\tif string.find(string.lower(n), \"quest\") then\n";
    s += "\t\t\tfound[#found + 1] = n .. \":\" .. type(v)\n";
    s += "\t\tend\n";
    s += "\tend\n";
    s += "\ttable.sort(found)\n";
    s += "\t" + g + ".Log(\"BRIDGE quest \" .. tag .. \" globals \" ..\n";
    s += "\t\t((#found > 0) and table.concat(found, \" \") or \"none\"))\n";
    s += "\tdump(\"QuestSystem\", function() return QuestSystem end)\n";
    s += "\tdump(\"Quest\", function() return Quest end)\n";
    s += "\tdump(\"player\", function() return player end)\n";
    s += "\tdump(\"HUD\", function() return HUD end)\n";
    s += "\tdump(\"Game\", function() return Game end)\n";
    s += "\tdump(\"System\", function() return System end)\n";
    s += "\tdump(\"XGenAIModule\", function() return XGenAIModule end)\n";
    s += "\tdump(\"UIAction\", function() return UIAction end)\n";
    s += "\tdump(\"ItemManager\", function() return ItemManager end)\n";
    s += "\tdump(\"DialogUtils\", function() return DialogUtils end)\n";
    s += "\tdump(\"Variables\", function() return Variables end)\n";
    s += "\tdump(\"GameToken\", function() return GameToken end)\n";
    s += "\tdump(\"GameUtils\", function() return GameUtils end)\n";
    s += "\tdump(\"Database\", function() return Database end)\n";
    s += "\tdump(\"DialogModule\", function() return DialogModule end)\n";
    s += "\tdump(\"GetGlobal(talk)\", function() return Variables.GetGlobal(" + g + ".talkVar) end)\n";
    s += "\tdump(\"questTable\", function()\n";
    s += "\t\tDatabase.LoadTable(\"quest\")\n";
    s += "\t\tlocal info = Database.GetTableInfo(\"quest\")\n";
    s += "\t\treturn info and info.LineCount\n";
    s += "\tend)\n";
    s += "\tdump(\"GameToken.GetToken(quest)\", function() return GameToken.GetToken(\"quest\") end)\n";
    s += "\tlocal qs = rawget(_G, \"QuestSystem\")\n";
    s += "\tif qs ~= nil then\n";
    s += "\t\tdump(\"IsQuestCompleted.dot\", function()\n";
    s += "\t\t\treturn qs.IsQuestCompleted(\"kutnohorskyTurnaj\")\n";
    s += "\t\tend)\n";
    s += "\t\tdump(\"IsQuestCompleted.colon\", function()\n";
    s += "\t\t\treturn qs:IsQuestCompleted(\"kutnohorskyTurnaj\")\n";
    s += "\t\tend)\n";
    s += "\t\tlocal names = {\n";
    s += "\t\t\t\"kutnohorskyTurnaj\", \"KuttenbergTournament\", \"q_kutnohorskyTurnaj\",\n";
    s += "\t\t\t\"storm\", \"Storm\", \"bourka\", \"drak\", \"finale\",\n";
    s += "\t\t\t\"kralovskeStribro\", \"hromovyKamen\", \"duels\",\n";
    s += "\t\t}\n";
    s += "\t\tfor _, name in ipairs(names) do\n";
    s += "\t\t\tdump(\"IsQuestCompleted(\" .. name .. \")\", function()\n";
    s += "\t\t\t\treturn qs.IsQuestCompleted(name)\n";
    s += "\t\t\tend)\n";
    s += "\t\t\tdump(\"IsQuestStarted(\" .. name .. \")\", function()\n";
    s += "\t\t\t\treturn qs.IsQuestStarted(name)\n";
    s += "\t\t\tend)\n";
    s += "\t\t\tdump(\"GetActiveObjectives(\" .. name .. \")\", function()\n";
    s += "\t\t\t\treturn qs.GetActiveObjectives(name)\n";
    s += "\t\t\tend)\n";
    s += "\t\tend\n";
    s += "\t\tdump(\"IsObjectiveCompleted_1arg\", function()\n";
    s += "\t\t\treturn qs.IsObjectiveCompleted(\"kutnohorskyTurnaj\")\n";
    s += "\t\tend)\n";
    s += "\t\tdump(\"IsObjectiveCompleted_2arg\", function()\n";
    s += "\t\t\treturn qs.IsObjectiveCompleted(\"kutnohorskyTurnaj\", \"win\")\n";
    s += "\t\tend)\n";
    s += "\tend\n";
    s += "\tlocal p = user\n";
    s += "\tif p == nil then p = rawget(_G, \"player\") end\n";
    s += "\tif p ~= nil and p.soul ~= nil then\n";
    s += "\t\tdump(\"player.GetId\", function() return p.soul:GetId() end)\n";
    s += "\t\tdump(\"player.GetStatLevel(spc)\", function() return p.soul:GetStatLevel(\"spc\") end)\n";
    s += "\t\tdump(\"player.GetSocialClass\", function() return p.soul:GetSocialClass() end)\n";
    s += "\t\tlocal oksc, sc = pcall(function() return p.soul:GetSocialClass() end)\n";
    s += "\t\tif oksc and type(sc) == \"table\" then\n";
    s += "\t\t\tfor _, field in ipairs({\"Id\", \"Name\", \"Wealth\", \"Paleness\", \"Roughness\", \"SoulCrimeRoleId\"}) do\n";
    s += "\t\t\t\tdump(\"social.\" .. field, function() return sc[field] end)\n";
    s += "\t\t\tend\n";
    s += "\t\tend\n";
    s += "\t\tdump(\"soul.HasPerk\", function() return p.soul.HasPerk end)\n";
    s += "\t\tdump(\"soul.HasSoulTag\", function() return p.soul.HasSoulTag end)\n";
    s += "\t\tdump(\"soul.GetPerks\", function() return p.soul.GetPerks end)\n";
    s += "\t\tdump(\"soul.HasScriptContext\", function() return p.soul.HasScriptContext end)\n";
    s += "\t\tdump(\"HasPerk(dummy)\", function() return p.soul:HasPerk(\"00000000-0000-0000-0000-000000000000\") end)\n";
    s += "\t\tdump(\"HasSoulTag(dummy)\", function() return p.soul:HasSoulTag(\"moonshine\") end)\n";
    s += "\t\tlocal mt = getmetatable(p.soul)\n";
    s += "\t\tdump(\"soul.metatable\", function() return mt end)\n";
    s += "\t\tif type(mt) == \"table\" then\n";
    s += "\t\t\tlocal keys = {}\n";
    s += "\t\t\tfor k, v in pairs(mt) do keys[#keys + 1] = tostring(k) .. \":\" .. type(v) end\n";
    s += "\t\t\ttable.sort(keys)\n";
    s += "\t\t\t" + g + ".Log(\"BRIDGE quest \" .. tag .. \" soul.mt \" ..\n";
    s += "\t\t\t\t((#keys > 0) and table.concat(keys, \" \") or \"empty\"))\n";
    s += "\t\t\tlocal idx = mt.__index\n";
    s += "\t\t\tif type(idx) == \"table\" then\n";
    s += "\t\t\t\tlocal ikeys = {}\n";
    s += "\t\t\t\tfor k, v in pairs(idx) do\n";
    s += "\t\t\t\t\tlocal n = tostring(k)\n";
    s += "\t\t\t\t\tif string.find(string.lower(n), \"perk\") or string.find(string.lower(n), \"quest\")\n";
    s += "\t\t\t\t\t\tor string.find(string.lower(n), \"tag\") or string.find(string.lower(n), \"relat\")\n";
    s += "\t\t\t\t\t\tor string.find(string.lower(n), \"social\") then\n";
    s += "\t\t\t\t\t\tikeys[#ikeys + 1] = n .. \":\" .. type(v)\n";
    s += "\t\t\t\t\tend\n";
    s += "\t\t\t\tend\n";
    s += "\t\t\t\ttable.sort(ikeys)\n";
    s += "\t\t\t\t" + g + ".Log(\"BRIDGE quest \" .. tag .. \" soul.index \" ..\n";
    s += "\t\t\t\t\t((#ikeys > 0) and table.concat(ikeys, \" \") or \"none\"))\n";
    s += "\t\t\tend\n";
    s += "\t\tend\n";
    s += "\t\tif npc ~= nil and npc.soul ~= nil then\n";
    s += "\t\t\tdump(\"GetRelationship\", function()\n";
    s += "\t\t\t\treturn npc.soul:GetRelationship(p.soul:GetId())\n";
    s += "\t\t\tend)\n";
    s += "\t\tend\n";
    s += "\tend\n";
    s += "\t" + g + ".Log(\"BRIDGE quest \" .. tag .. \" end\")\n";
    s += "end\n";
    s += "\n";
	// STORM assigns roles when a soul is created, so a save made before this mod
    // was enabled has souls our rules never touched, and the dialogue has nobody
    // to speak it. Granting the metarole is what reaches those souls, and it is
    // what has to happen before the request rather than after a failure: asking
    // first and granting later cost several seconds of a player pressing a button
    // and watching nothing happen.
    //
    // It has to be the metarole and not the role, because AddMetaRoleByName is the
    // only runtime grant there is, and granting a metarole is what makes the roles
    // beneath it speakable. Passing the metarole as RequestDialog's roles override
    // instead would avoid a lasting change to the soul, and was measured not to
    // work, so the lasting change is the price of reaching an old save.
    //
    // A grant used to be remembered per soul so the wrapper could tell our Talk prompt
    // from a real one. It no longer needs to: the held dialogue is gated shut between
    // conversations, so the base game never offers a prompt on our account.
    s += "function " + g + ".GrantName(npc, name)\n";
    s += "\tlocal ok, ret = pcall(function() return npc.soul:AddMetaRoleByName(name) end)\n";
    s += "\t" + g + ".Log(\"  AddMetaRoleByName(\" .. name .. \") ok=\" .. tostring(ok) ..\n";
    s += "\t\t\" ret=\" .. tostring(ret))\n";
    s += "\treturn ok\n";
    s += "end\n";
    s += "\n";
    s += "function " + g + ".Grant(npc)\n";
    s += "\tif npc.soul == nil or npc.soul.AddMetaRoleByName == nil then\n";
    s += "\t\t" + g + ".Log(\"  no soul:AddMetaRoleByName on this entity\")\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    s += "\t" + g + ".GrantName(npc, " + g + ".metarole)\n";
    s += "\tif not " + g + ".HasMeta(npc, " + g + ".metarole) then\n";
    s += "\t\t" + g + ".GrantName(npc, " + g + ".role)\n";
    s += "\t\t" + g + ".GrantName(npc, " + g + ".role .. \"_MUZ\")\n";
    s += "\t\t" + g + ".GrantName(npc, " + g + ".role .. \"_ZENA\")\n";
    s += "\tend\n";
    s += "end\n";
    s += "\n";
    // RequestDialog, which is what opened the focused session with the camera the
    // one time this worked. monologRequest by alias replaced it for a while and
    // never spoke, including for a line the game ships, so the request was not the
    // thing that needed changing.
    //
    // The fifth argument is bRequestChat: false asks for the focused session rather
    // than an ambient chat bubble.
    s += "function " + g + ".Attempt(npc, user)\n";
    s += "\tlocal ok, ret = pcall(function()\n";
    s += "\t\treturn npc.actor:RequestDialog(user.id, \"\", false, true, false)\n";
    s += "\tend)\n";
    s += "\t" + g + ".Log(\"  RequestDialog -> \" ..\n";
    s += "\t\t(ok and tostring(ret) or (\"raised \" .. tostring(ret))))\n";
    s += "\treturn ok\n";
    s += "end\n";
    s += "\n";
    // RequestDialog only queues the request. The NPC's own behaviour tree is what
    // runs it, via an unconditional loop in AI/npc/basic/switch/switch.xml feeding
    // dialogMailbox into handleDialog, and that fails quietly when no dialogue
    // matches the soul's roles. So the return value only says the request was
    // accepted, and IsInDialog is the one honest signal, which has to be read a
    // little later rather than off the call.
    //
    // ForceDialog is deliberately not the last resort here. It does start a
    // dialogue, but it starts whichever one the engine resolves rather than ours,
    // so a success would say nothing about our content.
    // Plain distance, not CanInteractWith. That call is the menu's question -- can
    // this be interacted with right now -- and it goes false as soon as the player
    // looks away, which ended conversations two seconds in while the player was
    // still standing there.
    //
    // Failing to measure is not the same as having left, so an unanswerable
    // position check keeps the conversation and leaves the slot budget as the
    // backstop.
    s += "function " + g + ".Near()\n";
    s += "\tlocal npc = " + g + ".npc\n";
    s += "\tif npc == nil then return false end\n";
    s += "\tlocal ok, gap = pcall(function()\n";
    s += "\t\tlocal a = npc:GetWorldPos()\n";
    s += "\t\tlocal b = player:GetWorldPos()\n";
    s += "\t\tlocal dx, dy, dz = a.x - b.x, a.y - b.y, a.z - b.z\n";
    s += "\t\treturn math.sqrt(dx * dx + dy * dy + dz * dz)\n";
    s += "\tend)\n";
    s += "\tif not ok or type(gap) ~= \"number\" then return true end\n";
    s += "\t" + g + ".gap = gap\n";
    s += "\treturn gap <= " + g + ".range\n";
    s += "end\n";
    s += "\n";
    s += "function " + g + ".InDialog()\n";
    s += "\tlocal ok, got = pcall(function() return player.human:IsInDialog() end)\n";
    s += "\treturn ok and got == true\n";
    s += "end\n";
    s += "\n";
    s += "function " + g + ".Call(label, fn)\n";
    s += "\tlocal ok, ret = pcall(fn)\n";
    s += "\t" + g + ".Log(\"  \" .. label .. \" -> \" ..\n";
    s += "\t\t(ok and tostring(ret) or (\"raised \" .. tostring(ret))))\n";
    s += "\treturn ok and ret\n";
    s += "end\n";
    s += "\n";
    // Words can reach the caption without a session. That is the failure a player
    // reports as "Foo N but no camera": RequestDialog returned true, the overlay
    // wrote, and IsInDialog stayed false. Give the behaviour tree a beat to pick
    // the request up, then try the other doors that still name our own dialogue.
    // ForceDialog and a SMLOUVANI grant are not among them: both start whichever
    // prompt the engine already has for this soul, which is how a kostkar's choice
    // menu appeared instead of the focused camera.
    s += "function " + g + ".PushCamera()\n";
    s += "\tif " + g + ".pushed then return end\n";
    s += "\t" + g + ".pushed = true\n";
    s += "\tlocal npc, user = " + g + ".npc, " + g + ".user\n";
    s += "\tif npc == nil or user == nil then return end\n";
    s += "\t" + g + ".Log(\"BRIDGE camera retry, still not in dialog\")\n";
    s += "\t" + g + ".Call(\"RequestDialog override\", function()\n";
    s += "\t\treturn npc.actor:RequestDialog(user.id, " + g + ".metarole, false, true, false)\n";
    s += "\tend)\n";
    s += "\tif " + g + ".InDialog() then return end\n";
    s += "\tlocal human = player.human\n";
    s += "\tif human ~= nil and human.ReadyForDialogWithRoles ~= nil then\n";
    s += "\t\t" + g + ".Call(\"ReadyForDialogWithRoles(npc, metarole)\", function()\n";
    s += "\t\t\treturn human:ReadyForDialogWithRoles(npc.id, " + g + ".metarole)\n";
    s += "\t\tend)\n";
    s += "\t\tif " + g + ".InDialog() then return end\n";
    s += "\t\t" + g + ".Call(\"ReadyForDialogWithRoles(metarole)\", function()\n";
    s += "\t\t\treturn human:ReadyForDialogWithRoles(" + g + ".metarole)\n";
    s += "\t\tend)\n";
    s += "\tend\n";
    s += "end\n";
    s += "\n";
    // Reading a turn is also how we learn it exists, since there is no way to ask
    // whether a path is there. A file that has not been written yet either hands
    // back nothing or raises, and both mean the same thing: not ready.
    // Said once each, at the first conversation, because a turn that cannot be read
    // is otherwise indistinguishable from one that has not been written: both leave
    // the pump quietly looking. Two ways in are tried, since which of them a retail
    // build exposes is not something the shipped files settle -- the game's own
    // LoadTextFile call sits inside a block its master build comments out, and
    // LoadScript is the one that mods are themselves loaded through.
    // The turn number is substituted rather than formatted: one of the paths is
    // %USER%, which the engine expands itself and string.format would read as a
    // format specifier it does not have.
    // Where the engine is willing to look for a file, which is the mod's to manage rather
    // than the player's to configure, because leaving it configured has a price.
    //
    // sys_PakPriority is 2 in this build: pak only, disk never consulted. That is the real
    // reason a loose file looked unreadable, and it is also why level loading is as quick
    // as it is. Setting it to 0 in a cfg bought the reading and cost about seven seconds of
    // every load; 1, which is pak first and disk second, still cost three and a half, since
    // the load asks for thousands of files no pak holds and each miss then goes to disk.
    //
    // A conversation opens a handful of files and a load opens tens of thousands, so 1 is
    // held for the length of a conversation and given back after it.
    s += g + ".diskRead = 1\n";
    s += g + ".diskRest = 2\n";
    s += g + ".diskWas = nil\n";
    // Whether the setting is ours to move at all -- see DiskStart.
    s += g + ".diskHold = false\n";
    // Whether a conversation has opened it for reading yet (Begin, Step).
    s += g + ".diskOn = false\n";
    s += "\n";
    // `quiet` is for the console pump, which moves it and back once a second (Console) and
    // would otherwise fill the log with the two lines.
    s += "function " + g + ".Disk(value, quiet)\n";
    s += "\tif not " + g + ".diskHold or System.SetCVar == nil then return end\n";
    s += "\tpcall(function() System.SetCVar(\"sys_PakPriority\", value) end)\n";
    s += "\tlocal got = nil\n";
    s += "\tif System.GetCVar ~= nil then\n";
    s += "\t\tpcall(function() got = System.GetCVar(\"sys_PakPriority\") end)\n";
    s += "\tend\n";
    // A second road, because a cvar the engine guards is set from a console line and not
    // from Lua, and which of the two this one answers to is not something the shipped
    // files settle. Only tried when the first did not take, and the log says what stuck
    // either way -- a value that will not move is the difference between a conversation
    // with words in it and one without.
    s += "\tif tostring(got) ~= tostring(value) and System.ExecuteCommand ~= nil then\n";
    s += "\t\tpcall(function() System.ExecuteCommand(\"sys_PakPriority \" .. value) end)\n";
    s += "\t\tif System.GetCVar ~= nil then\n";
    s += "\t\t\tpcall(function() got = System.GetCVar(\"sys_PakPriority\") end)\n";
    s += "\t\tend\n";
    s += "\tend\n";
    s += "\tif quiet and tostring(got) == tostring(value) then return end\n";
    s += "\t" + g + ".Log(\"BRIDGE asked for sys_PakPriority \" .. value ..\n";
    s += "\t\t\", it reads \" .. tostring(got))\n";
    s += "end\n";
    s += "\n";
    // Run once at load, before the first level load, which is the whole point of doing it
    // here rather than in a cfg the engine reads before either.
    //
    // 0 is left alone wherever it is found: disk first is what a loose file needs to
    // override a pak that holds the same path, so a player who set it wanted it, and
    // lowering it would break whatever wanted it -- the mod's own font override included.
    s += "function " + g + ".DiskStart()\n";
    s += "\tif System.GetCVar ~= nil then\n";
    s += "\t\tpcall(function() " + g + ".diskWas = System.GetCVar(\"sys_PakPriority\") end)\n";
    s += "\tend\n";
    s += "\tif " + g + ".diskWas == nil or tostring(" + g + ".diskWas) == \"0\" then\n";
    s += "\t\t" + g + ".Log(\"sys_PakPriority reads \" .. tostring(" + g + ".diskWas) ..\n";
    s += "\t\t\t\", leaving it where it is\")\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    s += "\t" + g + ".diskHold = true\n";
    s += "\t" + g + ".Disk(" + g + ".diskRest)\n";
    s += "end\n";
    s += "\n";
    // The frame rate while a conversation is up. The app's voice engine runs on the same
    // cores and the same power budget as the game, and the game at 60 fps leaves it half
    // of real time: 11 Oct 2026 on the Windows laptop, a 6 s line took 11.6 s to make and the first word
    // came after 4.5 s, while with the game held to 30 fps the same line took 6.3 s and
    // the first word came after 2.2 s -- the game's share fell from five cores to under
    // two and the GPU from 100 W to 60, and both are what the engine was short of. A
    // dialogue is a standing camera, so the trade is made for its length and given back
    // on the way out, like Disk. The player's own cap is read first: one already at or
    // under ours is left alone, and 0 (no cap) is moved like any higher number. Whatever
    // was there is what comes back.
    s += g + ".framesTalk = 30\n";
    s += g + ".framesWas = nil\n";
    s += "\n";
    // The same two roads as Disk: Lua's SetCVar, then the console line when that did not
    // take, and a log line saying what stuck.
    s += "function " + g + ".Frames(value)\n";
    s += "\tif System.SetCVar == nil then return end\n";
    s += "\tpcall(function() System.SetCVar(\"sys_MaxFPS\", value) end)\n";
    s += "\tlocal got = nil\n";
    s += "\tif System.GetCVar ~= nil then\n";
    s += "\t\tpcall(function() got = System.GetCVar(\"sys_MaxFPS\") end)\n";
    s += "\tend\n";
    s += "\tif tostring(got) ~= tostring(value) and System.ExecuteCommand ~= nil then\n";
    s += "\t\tpcall(function() System.ExecuteCommand(\"sys_MaxFPS \" .. value) end)\n";
    s += "\t\tif System.GetCVar ~= nil then\n";
    s += "\t\t\tpcall(function() got = System.GetCVar(\"sys_MaxFPS\") end)\n";
    s += "\t\tend\n";
    s += "\tend\n";
    s += "\t" + g + ".Log(\"BRIDGE asked for sys_MaxFPS \" .. value .. \", it reads \" .. tostring(got))\n";
    s += "end\n";
    s += "\n";
    // At Begin. framesWas is what FramesRest gives back, and nil when nothing was moved.
    // A hold already in place (a voiced dialogue of the game's own, VoicedFrames) is kept
    // as it is: what it read first is what the player had, and FramesRest gives it back.
    s += "function " + g + ".FramesHold()\n";
    s += "\tif " + g + ".framesWas ~= nil then return end\n";
    s += "\tif System.GetCVar == nil then return end\n";
    s += "\tlocal was = nil\n";
    s += "\tpcall(function() was = System.GetCVar(\"sys_MaxFPS\") end)\n";
    s += "\tlocal n = tonumber(was)\n";
    s += "\tif n == nil then return end\n";
    s += "\tif n > 0 and n <= " + g + ".framesTalk then return end\n";
    s += "\t" + g + ".framesWas = n\n";
    s += "\t" + g + ".Frames(" + g + ".framesTalk)\n";
    s += "end\n";
    s += "\n";
    // At Leave, the only way out of a conversation, so the cap never outlives one.
    s += "function " + g + ".FramesRest()\n";
    s += "\tlocal was = " + g + ".framesWas\n";
    s += "\tif was == nil then return end\n";
    s += "\t" + g + ".framesWas = nil\n";
    s += "\t" + g + ".Frames(was)\n";
    s += "end\n";
    s += "\n";
    // The same trade for a dialogue of the game's own that the app is voicing (the rows
    // emit.cpp rewrites to call Voice): the engine runs on the same GPU as the game, and
    // uncapped the game had enough of it that a line came out in pieces -- the stutter
    // through Paul Schmidt's lines, 12 Oct 2026, where the Begin road's cap never applied.
    // Held from the first Voice call, and given back once the dialogue has closed and a
    // few seconds have passed since the last line was asked for, so the last line is made
    // under the cap too. One chain of timers at a time, as Console does it; a Moonshine
    // conversation taking over mid-way leaves the hold to its own Leave.
    // The disk too (Disk): the mouth's cues are loose files beside the turns, and with
    // sys_PakPriority at its pak-only rest the engine never sees one -- Paul Schmidt's
    // Talk (notes.md 14 Oct 2026) armed every line and found no cue in 600 polls while
    // the app had written each within a second. Opened with the first line and given back
    // with the frames; a conversation of ours taking over mid-way owns it from there, and
    // its Leave gives it back. The console's once-a-second poll (Console), which opens and
    // shuts the disk round its own read between conversations, leaves it alone while this
    // holds it: the first build of this had the hold undone within a second.
    s += g + ".voicedGen = 0\n";
    s += g + ".voicedAt = -1\n";
    s += g + ".voicedDisk = false\n";
    s += "function " + g + ".VoicedFrames()\n";
    s += "\t" + g + ".voicedAt = " + g + ".Now()\n";
    s += "\tif " + g + ".framesWas == nil then " + g + ".FramesHold() end\n";
    s += "\tif not " + g + ".voicedDisk then\n";
    s += "\t\t" + g + ".voicedDisk = true\n";
    s += "\t\t" + g + ".Disk(" + g + ".diskRead)\n";
    s += "\tend\n";
    s += "\t" + g + ".voicedGen = " + g + ".voicedGen + 1\n";
    s += "\tlocal gen = " + g + ".voicedGen\n";
    s += "\tlocal function tick()\n";
    s += "\t\tif gen ~= " + g + ".voicedGen then return end\n";
    s += "\t\tif " + g + ".active then " + g + ".voicedDisk = false return end\n";
    s += "\t\tif " + g + ".Now() - " + g + ".voicedAt < 4 or " + g + ".InDialog() then\n";
    s += "\t\t\tpcall(function() Script.SetTimer(1000, tick) end)\n";
    s += "\t\t\treturn\n";
    s += "\t\tend\n";
    s += "\t\t" + g + ".Log(\"BRIDGE voiced dialogue over, frames given back\")\n";
    s += "\t\t" + g + ".FramesRest()\n";
    s += "\t\tif " + g + ".voicedDisk then " + g + ".voicedDisk = false; " + g + ".Disk(" + g + ".diskRest) end\n";
    s += "\t\t" + g + ".LipStop(\"voiced dialogue over\")\n";
    s += "\t\t" + g + ".lipKey = nil\n";
    s += "\t\t" + g + ".lipQueue = {}\n";
    s += "\t\t" + g + ".voicedSoul = nil\n";
    s += "\t\t" + g + ".voicedTalk = false\n";
    s += "\tend\n";
    s += "\tpcall(function() Script.SetTimer(1000, tick) end)\n";
    s += "end\n";
    s += "\n";
    s += "function " + g + ".Path(pattern, turn)\n";
    s += "\treturn (string.gsub(pattern, \"#\", tostring(turn)))\n";
    s += "end\n";
    s += "\n";
    // Reading a turn is also how we learn it exists: the words are the file, so a
    // file that is there is a turn that is ready and one that is not raises or hands
    // back nothing. Nothing is spent finding out, which is the difference that
    // matters -- polling for a turn used to mean loading a localization id, and an id
    // loaded is an id used up.
    // quiet is for a caller that looks often and expects to miss: a turn that is not
    // there yet is the usual case while a line is being streamed, not something to log.
    s += "function " + g + ".Fetch(turn, quiet)\n";
    s += "\tif System.LoadTextFile == nil then\n";
    s += "\t\t" + g + ".Log(\"System.LoadTextFile is not bound, nothing can be said\")\n";
    s += "\t\treturn nil\n";
    s += "\tend\n";
    s += "\tlocal patterns = " + g + ".inboxes\n";
    s += "\tif " + g + ".inbox ~= nil then patterns = { " + g + ".inbox } end\n";
    s += "\tfor i = 1, #patterns do\n";
    s += "\t\tlocal path = " + g + ".Path(patterns[i], turn)\n";
    s += "\t\tlocal ok, got = pcall(function() return System.LoadTextFile(path) end)\n";
    s += "\t\tif ok and type(got) == \"string\" and got ~= \"\" then\n";
    s += "\t\t\tif " + g + ".inbox == nil then\n";
    s += "\t\t\t\t" + g + ".inbox = patterns[i]\n";
    s += "\t\t\t\t" + g + ".Log(\"reading turns from \" .. patterns[i])\n";
    s += "\t\t\tend\n";
    // The writer ends the file with a newline and the reader hands back whatever is
    // in it, so the words are trimmed here rather than shown with it.
    s += "\t\t\tlocal words = (string.gsub(got, \"^%s*(.-)%s*$\", \"%1\"))\n";
    s += "\t\t\tif words ~= \"\" then return words end\n";
    s += "\t\tend\n";
    s += "\tend\n";
    // Once per turn rather than once per poll, which is four times a second.
    s += "\tif not quiet and " + g + ".missing[turn] == nil then\n";
    s += "\t\t" + g + ".missing[turn] = true\n";
    s += "\t\t" + g + ".Log(\"turn \" .. turn .. \" is not readable under any inbox\")\n";
    s += "\tend\n";
    s += "\treturn nil\n";
    s += "end\n";
    s += "\n";
    // Whether the app is running: its heartbeat file for this bucket of the clock, or the
    // one either side (the engine's clock steps by 128 seconds, so the edge of a bucket is
    // soft). Looked for under the inbox patterns as a turn is. True when there is no way
    // to tell -- no file reading, no clock -- since a check that cannot be made must not
    // close every conversation.
    // Each way out says why in the log, once per conversation: the first the Windows laptop run (5 Oct
    // 2026, appdown_probe.lua) had the check come back alive from the pump while the same
    // call from a probe step had just said missing, and nothing in the log told which
    // road it took.
    s += "function " + g + ".AppAlive()\n";
    s += "\tlocal function unchecked(why)\n";
    s += "\t\t" + g + ".Log(\"BRIDGE app heartbeat not checked -- \" .. why)\n";
    s += "\t\treturn true\n";
    s += "\tend\n";
    s += "\tif System.LoadTextFile == nil then return unchecked(\"no System.LoadTextFile\") end\n";
    s += "\tlocal clock = rawget(_G, \"os\")\n";
    s += "\tif clock == nil or clock.time == nil then return unchecked(\"no os.time\") end\n";
    s += "\tlocal ok, now = pcall(clock.time)\n";
    s += "\tif not ok or type(now) ~= \"number\" then return unchecked(\"os.time gave \" .. tostring(now)) end\n";
    s += "\tlocal bucket = math.floor(now / " + g + ".aliveBucket)\n";
    s += "\tlocal patterns = " + g + ".inboxes\n";
    s += "\tif " + g + ".inbox ~= nil then patterns = { " + g + ".inbox } end\n";
    s += "\tfor _, d in ipairs({ 0, -1, 1 }) do\n";
    s += "\t\tlocal name = string.format(\"alive_%d.txt\", bucket + d)\n";
    s += "\t\tfor i = 1, #patterns do\n";
    s += "\t\t\tlocal path = (string.gsub(patterns[i], \"turn_#%.txt\", name))\n";
    s += "\t\t\tlocal read, got = pcall(function() return System.LoadTextFile(path) end)\n";
    s += "\t\t\tif read and type(got) == \"string\" and got ~= \"\" then\n";
    s += "\t\t\t\t" + g + ".Log(\"BRIDGE app heartbeat \" .. name .. \" under \" .. patterns[i])\n";
    s += "\t\t\t\treturn true\n";
    s += "\t\t\tend\n";
    s += "\t\tend\n";
    s += "\tend\n";
    s += "\t" + g + ".Log(\"BRIDGE app heartbeat missing, looked for alive_\" .. string.format(\"%d\", bucket) .. \".txt and its neighbours\")\n";
    s += "\treturn false\n";
    s += "end\n";
    s += "\n";
    // Nobody is going to answer: say so in the other party's place, bare, and close the
    // conversation once the line has been read (@end, through Heard and EndWhenRead). The
    // turn counter moves as for any line, so an app started afterwards, which reads the
    // count off the log, carries on from the right number.
    // `line` is what the player reads; appDownLine unless the caller knows better (the
    // backstop, with the heartbeat seen, says appBusyLine).
    s += "function " + g + ".AppDown(why, line)\n";
    s += "\tif " + g + ".appDown then return end\n";
    s += "\t" + g + ".appDown = true\n";
    s += "\t" + g + ".Log(\"BRIDGE app down -- \" .. tostring(why))\n";
    s += "\t" + g + ".Say(" + g + ".turn + 1, \"@end \" .. (line or " + g + ".appDownLine), true)\n";
    s += "end\n";
    s += "\n";
    // A development hook: Lua run from hook_N.txt beside the turn files, N counting up
    // from one across the run of the game, looked for once a tick while a conversation is
    // up and once a second otherwise (Console). It is how the caption's behaviour is tried
    // without restarting the game -- what
    // parts two lines, whether the speaker field draws above or beside -- since each such
    // question used to cost a rebuild, a redeploy and a walk back to the NPC. The same
    // numbering as turns, for the same reason: the engine caches a path once read. The
    // folder is the player's own, under their saves, so this runs nothing they did not
    // put there themselves. Errors are logged, and the number moves on regardless, so one
    // bad file cannot jam the next.
    s += "function " + g + ".Hook()\n";
    // 5.1's name for it, which is the engine's Lua, with 5.2's as the fallback.
    s += "\tlocal loadstring = rawget(_G, \"loadstring\") or rawget(_G, \"load\")\n";
    s += "\tif System.LoadTextFile == nil or loadstring == nil then return end\n";
    s += "\tlocal patterns = " + g + ".inboxes\n";
    s += "\tif " + g + ".inbox ~= nil then patterns = { " + g + ".inbox } end\n";
    s += "\tlocal n = " + g + ".hooked + 1\n";
    s += "\tfor i = 1, #patterns do\n";
    s += "\t\tlocal path = " + g + ".Path((string.gsub(patterns[i], \"turn_\", \"hook_\")), n)\n";
    s += "\t\tlocal ok, got = pcall(function() return System.LoadTextFile(path) end)\n";
    s += "\t\tif ok and type(got) == \"string\" and got ~= \"\" then\n";
    s += "\t\t\t" + g + ".hooked = n\n";
    s += "\t\t\tlocal chunk, err = loadstring(got, \"hook_\" .. n)\n";
    s += "\t\t\tif chunk == nil then\n";
    s += "\t\t\t\t" + g + ".Log(\"BRIDGE hook \" .. n .. \" did not parse: \" .. tostring(err))\n";
    s += "\t\t\t\treturn\n";
    s += "\t\t\tend\n";
    s += "\t\t\tlocal ran, why = pcall(chunk)\n";
    s += "\t\t\tif ran then\n";
    s += "\t\t\t\t" + g + ".Log(\"BRIDGE hook \" .. n .. \" ran\" ..\n";
    s += "\t\t\t\t\t(why ~= nil and (\" -- \" .. tostring(why)) or \"\"))\n";
    s += "\t\t\telse\n";
    s += "\t\t\t\t" + g + ".Log(\"BRIDGE hook \" .. n .. \" raised \" .. tostring(why))\n";
    s += "\t\t\tend\n";
    s += "\t\t\treturn\n";
    s += "\t\tend\n";
    s += "\tend\n";
    s += "end\n";
    s += "\n";
    // The console: the hook looked for once a second whether or not anybody is being talked
    // to, so that a command typed at the app -- "/goto 27", which writes a hook calling
    // Teleport -- lands while Henry is walking about. A chain of Script.SetTimer ticks, one
    // live chain at a time (consoleGen), armed from the player's OnAction (every key), from
    // GetActions and from Client.OnUpdate (which never came on the Windows laptop, but is kept for a build
    // where it does), and from a command typed at the game's console (ConsoleSaid); re-armed
    // from the same places when the last tick is more than a
    // few seconds old: a timer armed while a save is loading is wiped when the loading ends,
    // and the engine's clock going backwards (a harness reset, a new run) reads as the same.
    // Nothing is done while a conversation is up, since Step pumps the hook itself then.
    //
    // The disk is only consulted while a conversation is up (Disk: sys_PakPriority is pak-only
    // between them, for the sake of load times), so each poll moves it to pak-first for the
    // read and back. Measured (console_probe.lua, 5 Oct 2026): without that, nothing under
    // %USER%/moonshine read at all outside a conversation -- not a hook, not a turn file
    // that had been there all along.
    //
    // Not armed from the world hooks (OnInit, OnLoad), and not polling until the world is
    // loaded: a chain armed at OnInit is wiped when the loading ends, and polling the disk
    // while a save streams in is what the pak-only setting is there to avoid.
    s += "function " + g + ".Console(why)\n";
    s += "\tlocal now = " + g + ".Now()\n";
    s += "\tlocal at = " + g + ".consoleAt\n";
    s += "\tif at >= 0 and now >= at and now - at < 4 then return false end\n";
    s += "\tif not " + g + ".AutoLoaded() then return false end\n";
    s += "\t" + g + ".consoleGen = " + g + ".consoleGen + 1\n";
    s += "\tlocal gen = " + g + ".consoleGen\n";
    s += "\t" + g + ".consoleAt = now\n";
    s += "\tlocal function tick()\n";
    s += "\t\tif gen ~= " + g + ".consoleGen then return end\n";
    s += "\t\t" + g + ".consoleAt = " + g + ".Now()\n";
    s += "\t\tif not " + g + ".active and " + g + ".AutoLoaded() then\n";
    // Not moved while a dialogue of the game's own holds it open for the mouth's cues
    // (VoicedFrames): put back to pak-only here, between the mouth's polls, it undid
    // that hold within a second of its taking, and Paul Schmidt's Talk went on finding
    // no cue with the disk nominally open (14 Oct 2026).
    s += "\t\t\tlocal held = " + g + ".voicedDisk\n";
    s += "\t\t\tif not held then " + g + ".Disk(" + g + ".diskRead, true) end\n";
    s += "\t\t\tpcall(" + g + ".Hook)\n";
    s += "\t\t\tif not held then " + g + ".Disk(" + g + ".diskRest, true) end\n";
    s += "\t\tend\n";
    s += "\t\tpcall(function() Script.SetTimer(1000, tick) end)\n";
    s += "\tend\n";
    s += "\tlocal armed = pcall(function() Script.SetTimer(1000, tick) end)\n";
    s += "\t" + g + ".Log(\"BRIDGE console \" .. (armed and \"armed\" or \"could not arm\") .. \" (\" .. tostring(why) .. \") chain \" .. gen)\n";
    s += "\treturn armed\n";
    s += "end\n";
    s += "\n";
    // The same commands from the game's own console (~), which is where somebody testing
    // by hand is already typing. System.AddCCommand registers a console command whose body
    // is Lua, with %% standing for every word typed after the name, each quoted (the typing
    // probe measured it, mods/kcd2/typing/README.md: con_restricted is 0, so a command with
    // no flags runs). The mod has no quest list with givers' names and routines to hand, and
    // cannot write a file, so the line is put in the log for the app, which answers through
    // a hook (Console::game_lua): the list written back into the log, which the console
    // shows, or the teleport. Nothing is answered when the app is not running, so that is
    // said at once, and nothing is answered before a save is loaded, since the hook pump
    // waits for the world; that is said too.
    s += g + ".consoleCommands = {\n";
    s += "\t{ \"/quests\", \"quests\", \"/quests [word] -- the quests dealt, numbered; with a word, only those that mention it\" },\n";
    s += "\t{ \"/goto\", \"goto\", \"/goto N [target] -- put Henry beside the giver of quest N (needs -devmode); target: the other party\" },\n";
    s += "\t{ \"/help\", \"help\", \"/help -- the moonshine commands\" },\n";
    s += "}\n";
    s += "function " + g + ".ConsoleSaid(cmd, ...)\n";
    s += "\tlocal text = \"/\" .. cmd\n";
    s += "\tlocal words = { ... }\n";
    s += "\tfor i = 1, #words do text = text .. \" \" .. tostring(words[i]) end\n";
    s += "\tif not " + g + ".AutoLoaded() then\n";
    s += "\t\t" + g + ".Log(\"[Moonshine] \" .. text .. \" is answered once a save is loaded\")\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    s += "\t" + g + ".Disk(" + g + ".diskRead, true)\n";
    s += "\tlocal alive = " + g + ".AppAlive()\n";
    s += "\t" + g + ".Disk(" + g + ".diskRest, true)\n";
    s += "\tif not alive then\n";
    s += "\t\t" + g + ".Log(\"[Moonshine] the conversation app is not running, so nobody can answer \" .. text .. \"; start moonshine_app\")\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    s += "\t" + g + ".Log(\"BRIDGE console line \" .. text)\n";
    s += "\tpcall(" + g + ".Console, \"console\")\n";
    s += "end\n";
    s += "\n";
    s += "function " + g + ".ConsoleRegister()\n";
    s += "\tif System.AddCCommand == nil then\n";
    s += "\t\t" + g + ".Log(\"no System.AddCCommand, so the game's console has no /quests\")\n";
    s += "\t\treturn 0\n";
    s += "\tend\n";
    s += "\tlocal n = 0\n";
    s += "\tfor _, c in ipairs(" + g + ".consoleCommands) do\n";
    s += "\t\tlocal name, cmd, help = c[1], c[2], c[3]\n";
    s += "\t\t" + g + "[\"Console_\" .. cmd] = function(...) return " + g + ".ConsoleSaid(cmd, ...) end\n";
    s += "\t\tlocal ok, err = pcall(function() System.AddCCommand(name, \"" + g + ".Console_\" .. cmd .. \"(%%)\", help) end)\n";
    s += "\t\tif ok then n = n + 1 else " + g + ".Log(\"AddCCommand \" .. name .. \" raised \" .. tostring(err)) end\n";
    s += "\tend\n";
    // The quest graph's line to the mod (GraphSaid): a console command of the mod's meant
    // for the graph's ExecuteConsoleString nodes, the way the shipped
    // utils/debug/DelayedConsoleExecution runs its. The node turned out inert in the
    // release build (24 Oct 2026; no graph of ours uses it now), but the command works from
    // Lua and the probes use it. Not the player's: it is not listed in /help and goes
    // nowhere near the app.
    s += "\tpcall(function() System.AddCCommand(\"" + std::string(kGraphCommand) + "\", \"" + g +
         ".GraphSaid(%%)\", \"the quest graph's word to the mod\") end)\n";
    s += "\t" + g + ".Log(\"BRIDGE console registered \" .. n .. \" of \" .. #" + g + ".consoleCommands .. \" commands\")\n";
    s += "\treturn n\n";
    s += "end\n";
    s += "\n";
    // Henry put beside a soul, or at one of the soul's routine spots when the soul is not
    // loaded here: the app's /goto, through a hook. `spots` is a list of {startMinute,
    // endMinute, x, y, z} off npc_routine, the window in minutes of the day; the one whose
    // window has the game's hour is taken, else the one with no window (the bed), else the
    // first. Beside means two metres off on the side Henry is already on, as the pass's
    // beside() does it. The devmode goto first and SetWorldPos when that did not take; a
    // check a second on says in the log whether either held, since without -devmode
    // neither may. A mounted Henry is dismounted first and moved four seconds later.
    // Returns a line for the hook's log entry, which the app prints.
    s += "function " + g + ".Teleport(soul, label, spots)\n";
    s += "\tlocal me = " + g + ".Player()\n";
    s += "\tif me == nil or me.GetWorldPos == nil then return \"no player to move\" end\n";
    s += "\tlabel = label or soul\n";
    s += "\tlocal e = nil\n";
    s += "\tpcall(function() e = System.GetEntityByName(soul) end)\n";
    s += "\tlocal to, how\n";
    s += "\tif e ~= nil and e.GetWorldPos ~= nil then\n";
    s += "\t\tlocal tp, pp = e:GetWorldPos(), me:GetWorldPos()\n";
    s += "\t\tlocal dx, dy = pp.x - tp.x, pp.y - tp.y\n";
    s += "\t\tlocal d = math.sqrt(dx * dx + dy * dy)\n";
    s += "\t\tif d < 0.01 then dx, dy, d = 1, 0, 1 end\n";
    s += "\t\tto = { x = tp.x + dx / d * 2, y = tp.y + dy / d * 2, z = tp.z }\n";
    s += "\t\thow = string.format(\"beside %s, who was %.0fm away\", label, d)\n";
    s += "\telse\n";
    s += "\t\tif type(spots) ~= \"table\" or #spots == 0 then\n";
    s += "\t\t\treturn label .. \" (\" .. soul .. \") is not loaded here and has no routine spot to go to\"\n";
    s += "\t\tend\n";
    s += "\t\tlocal hour = nil\n";
    s += "\t\tpcall(function() hour = Calendar.GetWorldHourOfDay() end)\n";
    s += "\t\tlocal minute = type(hour) == \"number\" and hour * 60 or nil\n";
    s += "\t\tlocal pick = nil\n";
    s += "\t\tif minute ~= nil then\n";
    s += "\t\t\tfor _, sp in ipairs(spots) do\n";
    s += "\t\t\t\tif sp[1] < sp[2] and minute >= sp[1] and minute < sp[2] then pick = sp; break end\n";
    s += "\t\t\tend\n";
    s += "\t\tend\n";
    s += "\t\tif pick == nil then\n";
    s += "\t\t\tfor _, sp in ipairs(spots) do if sp[1] == sp[2] then pick = sp; break end end\n";
    s += "\t\tend\n";
    s += "\t\tif pick == nil then pick = spots[1] end\n";
    s += "\t\tto = { x = pick[3] + 2, y = pick[4], z = pick[5] }\n";
    s += "\t\thow = string.format(\"at %s's spot for %s (%s is not loaded here)\", label,\n";
    s += "\t\t\tminute ~= nil and string.format(\"%d:00\", math.floor(hour)) or \"the hour\", soul)\n";
    s += "\tend\n";
    s += "\tlocal function move()\n";
    s += "\t\tpcall(function() System.ExecuteCommand(string.format(\"goto %.2f %.2f %.2f\", to.x, to.y, to.z)) end)\n";
    s += "\t\tlocal function gap()\n";
    s += "\t\t\tlocal pp = me:GetWorldPos()\n";
    s += "\t\t\treturn math.sqrt((pp.x - to.x) ^ 2 + (pp.y - to.y) ^ 2)\n";
    s += "\t\tend\n";
    s += "\t\tif gap() > 1 then pcall(function() me:SetWorldPos(to) end) end\n";
    s += "\t\tpcall(function() Script.SetTimer(1000, function()\n";
    s += "\t\t\tlocal left = gap()\n";
    s += "\t\t\tif left <= 3 then\n";
    s += "\t\t\t\t" + g + ".Log(string.format(\"BRIDGE console Henry is %s\", how))\n";
    s += "\t\t\telse\n";
    s += "\t\t\t\t" + g + ".Log(string.format(\"BRIDGE console the teleport did not hold, Henry is %.0fm off; is the game in -devmode?\", left))\n";
    s += "\t\t\tend\n";
    s += "\t\tend) end)\n";
    s += "\tend\n";
    s += "\tlocal mounted = false\n";
    s += "\tpcall(function() mounted = me.human:IsMounted() end)\n";
    s += "\tif mounted then\n";
    s += "\t\tpcall(function() me.human:Dismount() end)\n";
    s += "\t\tpcall(function() Script.SetTimer(4000, move) end)\n";
    s += "\t\treturn string.format(\"dismounting, then %s (%.1f %.1f %.1f)\", how, to.x, to.y, to.z)\n";
    s += "\tend\n";
    s += "\tmove()\n";
    s += "\treturn string.format(\"going %s (%.1f %.1f %.1f)\", how, to.x, to.y, to.z)\n";
    s += "end\n";
    s += "\n";
    // The giver's carrying (the app's `fast_travel` directive; npcdb.hpp has_fast_travel;
    // notes.md 25 Oct 2026): Henry taken to the quest's spot and the clock wound on to its
    // hour, as the game's own carriage does for a widow's champion. One road, the same with
    // and without -devmode (26 Oct 2026, kept so for maintenance and testing): the clock is
    // wound first, Calendar.SetWorldTime as player.lua's own wait does, and only ever
    // forward, the hour already past being tomorrow's; then me:SetWorldPos to the spot,
    // which holds for a dismounted Henry without devmode (carry_probe_nodev, 25 Oct), the
    // terrain under the spot standing in for a z written lower than it (a spot measured on
    // different ground, within reason: stand on it rather than in it; lower is paving over
    // terrain). Dismounted first, and sent once the dismount has played (mounted, no
    // teleport holds). Armed as the directive arrives, it waits for the conversation to be
    // gone (the giver's answer is still being said), a second at a time, and gives up after
    // two minutes. The log a second after he lands says where, how far off, and what the
    // clock reads. The game's own carriage (Last Will's CutsceneHandler on a level-bound
    // CutsceneHolder) and the console's `wh_pl_FakeFastTravelTo` were both tried and set
    // aside: the first wants level data the mod does not ship, the second -devmode (README,
    // "The quest's own carriage").
    s += "function " + g + ".FastTravelArm(id, q, tries)\n";
    s += "\ttries = tries or 0\n";
    s += "\tif not " + g + ".InDialog() and not " + g + ".active then\n";
    s += "\t\t" + g + ".FastTravelGo(id, q)\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    s += "\tif tries >= 120 then\n";
    s += "\t\t" + g + ".Log(\"BRIDGE quest \" .. id .. \" carrying given up, the conversation never ended\")\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    s += "\tpcall(function() Script.SetTimer(1000, function() " + g + ".FastTravelArm(id, q, tries + 1) end) end)\n";
    s += "end\n";
    s += "function " + g + ".FastTravelGo(id, q)\n";
    s += "\tlocal me = " + g + ".Player()\n";
    s += "\tlocal ft = q.fastTravel\n";
    s += "\tif me == nil or me.GetWorldPos == nil or ft == nil then return end\n";
    s += "\tlocal to = { x = ft.x, y = ft.y, z = ft.z }\n";
    s += "\tlocal function go()\n";
    s += "\t\tlocal ahead = nil\n";
    s += "\t\tlocal hour, now = nil, " + g + ".WorldTime()\n";
    s += "\t\tpcall(function() hour = Calendar.GetWorldHourOfDay() end)\n";
    s += "\t\tif type(hour) == \"number\" and now ~= nil then\n";
    s += "\t\t\tahead = (ft.at - hour) % 24\n";
    s += "\t\t\tif ahead > 0.01 then pcall(function() Calendar.SetWorldTime(now + ahead * 3600) end) end\n";
    s += "\t\tend\n";
    s += "\t\tpcall(function()\n";
    s += "\t\t\tlocal ground = System.GetTerrainElevation(to.x, to.y)\n";
    s += "\t\t\tif type(ground) == \"number\" and ground > to.z and ground - to.z <= 5 then to.z = ground end\n";
    s += "\t\tend)\n";
    s += "\t\tpcall(function() me:SetWorldPos(to) end)\n";
    // The street watch is told (WatchFollow's grace): the holder is re-placed by the clock's
    // winding and stands wherever the engine put him, often beside the gate.
    s += "\t\t" + g + ".followGraceAt = " + g + ".Now()\n";
    s += "\t\tpcall(function() Script.SetTimer(1000, function()\n";
    s += "\t\t\tlocal pp = me:GetWorldPos()\n";
    s += "\t\t\tlocal read = nil\n";
    s += "\t\t\tpcall(function() read = Calendar.GetWorldHourOfDay() end)\n";
    s += "\t\t\t" + g + ".Log(string.format(\"BRIDGE quest %s carried Henry to %.1f %.1f %.1f, %.1fm off, %s on, the clock reads %s\",\n";
    s += "\t\t\t\tid, to.x, to.y, to.z, math.sqrt((pp.x - to.x) ^ 2 + (pp.y - to.y) ^ 2),\n";
    s += "\t\t\t\tahead ~= nil and string.format(\"%.1fh\", ahead) or \"no hours\",\n";
    s += "\t\t\t\ttype(read) == \"number\" and string.format(\"%.2f\", read) or \"nil\"))\n";
    s += "\t\tend) end)\n";
    s += "\tend\n";
    s += "\tlocal mounted = false\n";
    s += "\tpcall(function() mounted = me.human:IsMounted() end)\n";
    s += "\tif mounted then\n";
    s += "\t\tpcall(function() me.human:Dismount() end)\n";
    s += "\t\tpcall(function() Script.SetTimer(4000, go) end)\n";
    s += "\t\t" + g + ".Log(\"BRIDGE quest \" .. id .. \" carrying Henry once he is off the horse\")\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    s += "\tgo()\n";
    s += "end\n";
    s += "\n";
    // Long enough to read, and later long enough to hear: once the words are being
    // spoken aloud this is however long that takes. The engine wants whole seconds --
    // the reference says so of the fourth argument, and a fraction is not worth
    // finding out about -- so it is floored rather than rounded.
    s += "function " + g + ".Reading(words)\n";
    s += "\tlocal seconds = math.floor(2 + #words / 12)\n";
    s += "\tif seconds < 2 then seconds = 2 end\n";
    s += "\tif seconds > 12 then seconds = 12 end\n";
    s += "\treturn seconds\n";
    s += "end\n";
    s += "\n";
    // The font, re-read once per run of the game.
    //
    // Not selected -- DrawText has no font parameter, which a probe of seven call shapes
    // settled -- but replaced. LoadFont(name) reads fonts/<name>.xml, which the engine
    // told us itself when a name with a folder in it came back as "Can't open file
    // 'fonts/Fonts/subtitles.xml'". The mod ships its own fonts/default.xml, so asking for
    // "default" again is what makes the engine read ours over the one it loaded at
    // startup, and the only way the face these words are drawn in can be chosen at all.
    s += "function " + g + ".Font()\n";
    s += "\tif " + g + ".fontTried then return end\n";
    s += "\t" + g + ".fontTried = true\n";
    s += "\tif System.LoadFont == nil then\n";
    s += "\t\t" + g + ".Log(\"System.LoadFont is not bound, drawing in whatever is loaded\")\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    s += "\tlocal ok, err = pcall(function() System.LoadFont(" + g + ".drawFont) end)\n";
    s += "\tif ok then\n";
    s += "\t\t" + g + ".Log(\"BRIDGE font \" .. " + g + ".drawFont)\n";
    s += "\telse\n";
    s += "\t\t" + g + ".Log(\"LoadFont refused \" .. " + g + ".drawFont .. \" -- \" ..\n";
    s += "\t\t\ttostring(err) .. \", drawing in the default font\")\n";
    s += "\t\t" + g + ".drawFont = \"default\"\n";
    s += "\tend\n";
    s += "end\n";
    s += "\n";
    s += "function " + g + ".CVar(name)\n";
    s += "\tlocal ok, got = pcall(function() return System.GetCVar(name) end)\n";
    s += "\tif ok then return tonumber(got) end\n";
    s += "\treturn nil\n";
    s += "end\n";
    s += "\n";
    // The size of the screen DrawText draws on, which is not the one GetViewport
    // reports.
    //
    // GetViewport gives the render resolution. Measured on a 3840x1600 screen it came
    // back 2560x1067, because the game renders smaller and upscales -- the log calls
    // them "Render resolution" and "Post/Overlay resolution" and they differ by half
    // again. Text goes in the overlay, so fractions of the viewport landed every line a
    // third of the way up and left of where it was asked to be: the middle of the screen
    // instead of the bottom of it.
    //
    // r_Width and r_Height are the display size, and are taken over the viewport
    // whenever they are at least as large. Neither is trusted blindly, which is what
    // the sanity check and the log line are for.
    s += "function " + g + ".Screen()\n";
    s += "\tlocal wide, high, from = 1920, 1080, \"nothing, so a guess\"\n";
    s += "\tlocal ok, view = pcall(function() return System.GetViewport() end)\n";
    s += "\tif ok and type(view) == \"table\" and type(view.width) == \"number\" and\n";
    s += "\t\t\tview.width > 0 then\n";
    s += "\t\twide, high, from = view.width, view.height, \"GetViewport\"\n";
    s += "\tend\n";
    s += "\tlocal cw, ch = " + g + ".CVar(\"r_Width\"), " + g + ".CVar(\"r_Height\")\n";
    s += "\tif cw ~= nil and ch ~= nil and cw >= wide and ch >= high then\n";
    s += "\t\twide, high, from = cw, ch, \"r_Width and r_Height\"\n";
    s += "\tend\n";
    s += "\tif not " + g + ".viewTold then\n";
    s += "\t\t" + g + ".viewTold = true\n";
    s += "\t\t" + g + ".Log(\"BRIDGE screen \" .. wide .. \"x\" .. high .. \" from \" .. from ..\n";
    s += "\t\t\t\", viewport said \" .. tostring(ok and view.width) .. \"x\" ..\n";
    s += "\t\t\ttostring(ok and view.height) .. \", cvars said \" .. tostring(cw) .. \"x\" ..\n";
    s += "\t\t\ttostring(ch))\n";
    s += "\tend\n";
    s += "\treturn wide, high\n";
    s += "end\n";
    s += "\n";
    // Where a line goes, in the pixels DrawText wants, worked out once per line rather
    // than once per frame.
    //
    // Centring is an estimate and cannot be anything else. Of the 139 calls on System
    // not one measures a string, so the width of this one is guessed from how many
    // characters it has, and a guess that is out moves the line off centre and does
    // nothing worse.
    s += "function " + g + ".Where(text)\n";
    s += "\tlocal wide, high = " + g + ".Screen()\n";
    s += "\tlocal across = wide * " + g + ".drawX\n";
    s += "\tlocal down = high * " + g + ".drawY\n";
    // Half the characters' width back from the middle. The half again is because a
    // character is taller than it is wide in every face here.
    s += "\tacross = across - (#text * " + g + ".drawPixels * " + g + ".drawSize * 0.5) / 2\n";
    s += "\tif across < 0 then across = 0 end\n";
    s += "\treturn across, down\n";
    s += "end\n";
    s += "\n";
    // One line of the conversation, put where it was asked to go.
    //
    // (x, y, text, size, r, g, b, alpha), which is the reference's list with the font
    // taken out of it, and every part of that was measured rather than read. Seven call
    // shapes were tried once each: the engine took every numeric one without a word and
    // warned about both that carried a font name, on all 16295 paints of the session that
    // tried it, so there is no font here to choose. Then two arrangements of a colour were
    // drawn in red, one with a second size between the size and the red and one without,
    // and the one without is the one that came out red.
    //
    // The three-argument form is kept as the answer if even this is refused, and which
    // shape worked is remembered rather than rediscovered every frame.
    s += "function " + g + ".Stroke(text)\n";
    s += "\tif " + g + ".drawShape ~= 2 then\n";
    s += "\t\tlocal full = pcall(function()\n";
    s += "\t\t\tSystem.DrawText(" + g + ".shownX, " + g + ".shownY, text, " + g +
         ".drawSize, 1, 1, 1, 1)\n";
    s += "\t\tend)\n";
    s += "\t\tif full then\n";
    s += "\t\t\tif " + g + ".drawShape == 0 then\n";
    s += "\t\t\t\t" + g + ".drawShape = 1\n";
    s += "\t\t\t\t" + g + ".Log(\"BRIDGE DrawText takes a position, words, a size and\" ..\n";
    s += "\t\t\t\t\t\" a colour\")\n";
    s += "\t\t\tend\n";
    s += "\t\t\treturn\n";
    s += "\t\tend\n";
    s += "\tend\n";
    s += "\tlocal short, err = pcall(function()\n";
    s += "\t\tSystem.DrawText(" + g + ".shownX, " + g + ".shownY, text)\n";
    s += "\tend)\n";
    s += "\tif short then\n";
    s += "\t\tif " + g + ".drawShape ~= 2 then\n";
    s += "\t\t\t" + g + ".drawShape = 2\n";
    s += "\t\t\t" + g + ".Log(\"BRIDGE DrawText takes three arguments, not ten\")\n";
    s += "\t\tend\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    // Said once, then never again: this runs every frame, and a call that raises on
    // one frame will raise on all of them. The words carry on through the widget the
    // game shows its own notices in, which is plainer to look at but has been carrying
    // them all along.
    //
    // For the rest of the run of the game, not just this conversation. Trying again on
    // the next one would only find the same missing call, and would find drawTold
    // already true and so no longer arrange a way round it.
    s += "\tif not " + g + ".drawTold then\n";
    s += "\t\t" + g + ".drawTold = true\n";
    s += "\t\t" + g + ".mode = \"info\"\n";
    s += "\t\t" + g + ".Log(\"DrawText raised \" .. tostring(err) ..\n";
    s += "\t\t\t\", the words go through info text for the rest of this run\")\n";
    s += "\t\t" + g + ".Show(text, " + g + ".Reading(text))\n";
    s += "\tend\n";
    s += "end\n";
    s += "\n";
    // Drawn text lasts one frame, so a line stays up only as long as something keeps
    // asking for it. This is that asking: a timer that re-arms itself for as long as a
    // conversation is open, and lets go of the words when one ends.
    //
    // Nothing here decides when a line has been up long enough. It holds the last thing
    // said until the next thing is said, which is what the pump is already timing, and
    // means a player who looks away comes back to words still there rather than an
    // empty screen.
    s += "function " + g + ".Paint()\n";
    // Info text needs no pump because the widget keeps what it is given; the other two
    // roads both expire, one after a frame and one when the dialogue writes its own line
    // over ours.
    s += "\tif not " + g + ".active or " + g + ".mode == \"info\" then\n";
    s += "\t\t" + g + ".shown = \"\"\n";
    s += "\t\t" + g + ".painting = false\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    // Put back rather than set, because the dialogue owns this widget and empties it as
    // each response begins -- twice a beat, and the beats are wordless now, so what it
    // leaves is nothing. Whatever time passes between its clear and our next write is a
    // gap with nothing in it, which is exactly what a caption blinking out for a frame or
    // two is, so the interval here is the size of that blink and there is no reason for it
    // to be more than one paint (captionEvery).
    s += "\tif " + g + ".mode == \"caption\" then\n";
    // What is put back is both rows: see Display.
    s += "\t\tlocal text, above = " + g + ".Display()\n";
    s += "\t\tif " + g + ".captionEvery > 0 and text ~= \"\" and\n";
    s += "\t\t\t\t" + g + ".paints % " + g + ".captionEvery == 0 then\n";
    // Straight past the dedupe, since the whole point of putting it back is saying again
    // what the widget was already told.
    s += "\t\t\t" + g + ".wrote = nil\n";
    s += "\t\t\t" + g + ".Write(text, above)\n";
    s += "\t\tend\n";
    s += "\telseif " + g + ".shown ~= \"\" then\n";
    s += "\t\t" + g + ".Stroke(" + g + ".shown)\n";
    s += "\tend\n";
    // How often the line actually goes back up, measured once a conversation and said
    // in the log. It is the number the whole approach rests on: asking for 10ms is not
    // the same as getting it, no shipped script asks Script.SetTimer for anything
    // faster than a second, and text that goes up less often than the game draws is
    // text that flickers. Counted against the pump's own ticks rather than a clock,
    // since the pump is the one timer here already known to keep time.
    s += "\t" + g + ".paints = " + g + ".paints + 1\n";
    s += "\tif not " + g + ".rateTold then\n";
    s += "\t\tlocal spent = " + g + ".ticks - " + g + ".paintFrom\n";
    s += "\t\tif spent >= 5 then\n";
    s += "\t\t\t" + g + ".rateTold = true\n";
    s += "\t\t\t" + g + ".Log(\"BRIDGE drew \" .. " + g + ".paints .. \" times in \" ..\n";
    s += "\t\t\t\tstring.format(\"%.1f\", spent * 0.4) .. \"s, about \" ..\n";
    s += "\t\t\t\tmath.floor(" + g + ".paints / (spent * 0.4)) .. \" a second\")\n";
    s += "\t\tend\n";
    s += "\tend\n";
    s += "\tlocal armed = pcall(function() Script.SetTimer(" + g + ".repaintMillis, " + g +
         ".Paint) end)\n";
    s += "\tif not armed then\n";
    s += "\t\t" + g + ".painting = false\n";
    s += "\t\t" + g + ".Log(\"Script.SetTimer refused, nothing will be drawn\")\n";
    s += "\tend\n";
    s += "end\n";
    s += "\n";
    // Words in front of a player, by whichever of the two roads is being taken.
    //
    // Both take the text itself rather than a name for one, which is the whole reason a
    // conversation is no longer finite: nothing is looked up, nothing is defined, and
    // nothing is kept from one line to the next.
    //
    // Info text is a queue that owns what it is given -- clearing it is what makes each
    // line replace the one before rather than stack under it, and seconds is how long it
    // stays. Drawing owns nothing and expires on its own, so there the seconds are
    // ignored and the line is simply handed to the painter, which is started here the
    // first time anything is said.
    // The game's own caption, out of the way for as long as a conversation lasts.
    //
    // The dialogue has a line to say on every beat and we have nothing for it to say: the
    // words the player reads are drawn separately, so anything the caption carries is a
    // second one underneath. Three tries at making that line invisible in the string all
    // showed something -- dots, then a box for a glyph the font lacks, then the id itself,
    // which is what the engine puts up when a stringId resolves to nothing.
    //
    // So the widget goes instead of the words in it. hud.gfx declares the caption as a
    // movie clip named Subtitles at bc.subtitles, and UIAction can hide a clip by name,
    // which is two calls a conversation rather than a fight with a string the dialogue
    // rewrites twice a beat.
    //
    // Both instance ids are tried because nothing here says which one a live element
    // answers to, -1 meaning every instance. Neither raises on a name the UI does not
    // know, so there is no reading back which of them worked: the log says what was
    // asked for and the screen says whether it landed.
    s += "function " + g + ".Caption(show)\n";
    s += "\tif UIAction == nil or UIAction.SetVisible == nil then\n";
    s += "\t\tif not " + g + ".captionTold then\n";
    s += "\t\t\t" + g + ".captionTold = true\n";
    s += "\t\t\t" + g + ".Log(\"UIAction.SetVisible is not there, the dialogue's caption\" ..\n";
    s += "\t\t\t\t\" stays on screen\")\n";
    s += "\t\tend\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    s += "\tlocal ids = " + g + ".hudIds\n";
    s += "\tfor i = 1, #ids do\n";
    s += "\t\tpcall(function()\n";
    s += "\t\t\tUIAction.SetVisible(\"hud\", ids[i], \"" + std::string(kCaptionClip) +
         "\", show)\n";
    s += "\t\tend)\n";
    // And empty as well as hidden, because a clip that is shown again should not bring
    // back the line it was hiding. ClearSubtitles is declared by the same element and
    // takes nothing.
    s += "\t\tif not show then\n";
    s += "\t\t\tpcall(function()\n";
    s += "\t\t\t\tUIAction.CallFunction(\"hud\", ids[i], \"ClearSubtitles\")\n";
    s += "\t\t\tend)\n";
    s += "\t\tend\n";
    s += "\tend\n";
    s += "\tif not " + g + ".captionTold then\n";
    s += "\t\t" + g + ".captionTold = true\n";
    // Which way round, because this says "hide" whatever it was asked for otherwise, and a
    // log line that reports the opposite of what happened costs an hour the next time
    // something is wrong here.
    s += "\t\t" + g + ".Log(\"BRIDGE asked UIAction to \" ..\n";
    s += "\t\t\t(show and \"show \" or \"hide \") .. \"" + std::string(kCaptionClip) +
         "\")\n";
    s += "\tend\n";
    s += "end\n";
    s += "\n";
    // Our line in the game's own caption, which the same element that clears one declares a
    // setter for: SetSubtitles(Text), documented as "Subtitles text to be set. (can be
    // HTML)". A raw string, so it costs no stringId and keeps none -- which is the whole
    // difference between this and everything the dialogue can be made to say.
    //
    // What it would buy is everything the drawn words had to be talked into: the caption's
    // own face, which is the Scaleform one no font definition can reach, its own background,
    // its own place on the screen, and wrapping. What it costs is ownership -- the dialogue
    // writes its own line here at the start of every response, so ours has to be put back
    // rather than simply set.
    //
    // It is HTML, and it means it: a bare > was measured to vanish, taking the character
    // after it, so both rows are escaped here and nowhere else. above is the speaker
    // field, the gold row over the text; empty leaves that row undrawn. Text empty hides
    // the whole widget whatever above says, which is why the line goes in the text field
    // and the row over it is the one that comes and goes.
    s += "function " + g + ".Html(text)\n";
    s += "\ttext = string.gsub(text, \"&\", \"&amp;\")\n";
    s += "\ttext = string.gsub(text, \"<\", \"&lt;\")\n";
    s += "\ttext = string.gsub(text, \">\", \"&gt;\")\n";
    s += "\treturn text\n";
    s += "end\n";
    s += "\n";
    s += "function " + g + ".Write(text, above)\n";
    s += "\tabove = above or \"\"\n";
    // Only when the words change, now that nothing else is writing them. The first road put
    // the line back on every frame to fight the dialogue for the field; asking a widget to
    // say what it is already saying, 49 times a second, is at best wasted and at worst a
    // fade that keeps starting over.
    s += "\tlocal key = above .. \"\\n\" .. text\n";
    s += "\tif key == " + g + ".wrote then return end\n";
    s += "\t" + g + ".wrote = key\n";
    s += "\tif UIAction == nil or UIAction.CallFunction == nil then\n";
    s += "\t\tif not " + g + ".wroteTold then\n";
    s += "\t\t\t" + g + ".wroteTold = true\n";
    s += "\t\t\t" + g + ".Log(\"UIAction.CallFunction is not there, the caption stays the\" ..\n";
    s += "\t\t\t\t\" dialogue's\")\n";
    s += "\t\tend\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    s += "\tlocal ids = " + g + ".hudIds\n";
    s += "\tfor i = 1, #ids do\n";
    // Emptying goes through the function made for it rather than through a blank string,
    // since a caption told to say nothing still has a plate to draw around it.
    s += "\t\tif text == \"\" then\n";
    s += "\t\t\tpcall(function()\n";
    s += "\t\t\t\tUIAction.CallFunction(\"hud\", ids[i], \"ClearSubtitles\")\n";
    s += "\t\t\tend)\n";
    s += "\t\telse\n";
    // All three parameters, which HUD.xml names as Text, SpeakerName and IsPlayer. The
    // first call here passed only the first, and a Flash function reading two undefineds
    // may well take a different branch than one reading an empty name and a false.
    s += "\t\t\tpcall(function()\n";
    s += "\t\t\t\tUIAction.CallFunction(\"hud\", ids[i], \"SetSubtitles\", " + g +
         ".Html(text),\n";
    s += "\t\t\t\t\t" + g + ".Html(above), false)\n";
    s += "\t\t\tend)\n";
    s += "\t\tend\n";
    s += "\tend\n";
    s += "\tif not " + g + ".wroteTold then\n";
    s += "\t\t" + g + ".wroteTold = true\n";
    s += "\t\t" + g + ".Log(\"BRIDGE asked the game's caption to say the line through\" ..\n";
    s += "\t\t\t\" SetSubtitles\")\n";
    s += "\tend\n";
    s += "end\n";
    s += "\n";
    // The caption's background and its speaker row, asked for once as a conversation
    // opens. The background is the plate a drawn line never got -- auxiliary geometry is
    // not rendered in a master build, so the only fill available is one the UI already
    // draws. The speaker row is on because it is the second line: Write fills it with
    // whatever should sit over the text, and an empty speaker draws no row at all, so
    // leaving it enabled costs nothing on a line with nothing above it.
    s += "function " + g + ".Plate()\n";
    s += "\tif UIAction == nil or UIAction.CallFunction == nil then return end\n";
    s += "\tlocal ids = " + g + ".hudIds\n";
    s += "\tfor i = 1, #ids do\n";
    s += "\t\tpcall(function()\n";
    s += "\t\t\tUIAction.CallFunction(\"hud\", ids[i], \"SetSubtitleBgEnable\", true)\n";
    s += "\t\tend)\n";
    s += "\t\tpcall(function()\n";
    s += "\t\t\tUIAction.CallFunction(\"hud\", ids[i], \"SetSubtitleSpeakerNameEnable\",\n";
    s += "\t\t\t\ttrue)\n";
    s += "\t\tend)\n";
    s += "\tend\n";
    s += "end\n";
    s += "\n";
    s += "function " + g + ".Show(text, seconds)\n";
    s += "\tif " + g + ".mode == \"info\" then\n";
    s += "\t\tlocal ok, err = pcall(function()\n";
    s += "\t\t\tGame.SendInfoText(text, true, nil, seconds)\n";
    s += "\t\tend)\n";
    s += "\t\tif not ok then " + g + ".Log(\"  SendInfoText raised \" .. tostring(err)) end\n";
    s += "\t\treturn ok\n";
    s += "\tend\n";
    // The caption road stops here: the words are handed to the widget and the pump is
    // started to keep putting them back, and nothing is drawn, since a line the game itself
    // is captioning does not want a second copy of itself over the top in another face.
    s += "\tif " + g + ".mode == \"caption\" then\n";
    s += "\t\t" + g + ".shown = text\n";
    s += "\t\t" + g + ".PageSet(text, " + g + ".more)\n";
    s += "\t\t" + g + ".Write(" + g + ".Display())\n";
    // No frame timer unless the line is being put back, which it is not: one write holds
    // until the next line, so there is nothing for a pump to do between them.
    s += "\t\tif " + g + ".captionEvery > 0 and not " + g + ".painting then\n";
    s += "\t\t\t" + g + ".painting = true\n";
    s += "\t\t\t" + g + ".Paint()\n";
    s += "\t\tend\n";
    s += "\t\treturn true\n";
    s += "\tend\n";
    s += "\t" + g + ".Font()\n";
    s += "\t" + g + ".shown = text\n";
    s += "\t" + g + ".shownX, " + g + ".shownY = " + g + ".Where(text)\n";
    s += "\tif not " + g + ".painting then\n";
    s += "\t\t" + g + ".painting = true\n";
    s += "\t\t" + g + ".Paint()\n";
    s += "\tend\n";
    s += "\treturn true\n";
    s += "end\n";
    s += "\n";
    // A reply arrives as it is written now: every hundred milliseconds or so the app sends
    // the text so far as a turn of its own, flagged @more, and the last turn is the whole
    // reply unflagged. A flagged turn replaces the line on screen with the longer one and
    // holds nothing, since nothing has been taken away; while one is expected the line is
    // looked for again on a fast timer of its own, because the pump's 400ms would show the
    // words in lumps. Only the unflagged turn holds the line for reading and hands the turn
    // to the player.
    // bare is for a line of the mod's own (AppDown): shown without the other party's name
    // in front, since they did not say it.
    // A line has been put on the screen. The app speaks it: henry for him, npc for
    // whoever the conversation is open with, male or female where the graph knows.
    // The actors' own clones stay unused until their rights are cleared.
    // From a dialogue of the game's own (the voiced rows), the frame cap a conversation
    // gets at Begin is taken here instead, for the engine's sake (VoicedFrames).
    // Outside a session (the game's own Talk on a holder, a receiver, a helper) the
    // third argument is the speaker's entity name, which is where the mouth goes; the
    // lines of one row arrive together (Henry's, then theirs) and the app speaks them in
    // turn, so they queue, each armed as the one before it stops (notes.md 13 Oct 2026).
    // The soul rides on the marker (`soul=<name>` before the words; gamelog.cpp strips it
    // and the line's key is of the words alone) so the app can borrow the mouth from *that*
    // actor's clips: before (23 Oct 2026) it took the face of whatever session was last
    // open, and Father Ernest read Christine's letter with the housekeeper's clips on his
    // head, which moved nothing a player could see.
    s += "function " + g + ".Voice(who, text, soul)\n";
    s += "\tif text == nil or text == \"\" or text == \"...\" then return end\n";
    s += "\tif not " + g + ".active and soul ~= nil and soul ~= \"\" then\n";
    s += "\t\t" + g + ".Log(\"BRIDGE voice \" .. who .. \" soul=\" .. soul .. \" \" .. text)\n";
    s += "\telse\n";
    s += "\t\t" + g + ".Log(\"BRIDGE voice \" .. who .. \" \" .. text)\n";
    s += "\tend\n";
    s += "\tif " + g + ".active then\n";
    // The mouth's cues looked for, from the same words the app will key them by.
    s += "\t\tpcall(" + g + ".LipArm, who, text)\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    s += "\tpcall(" + g + ".VoicedFrames)\n";
    s += "\t" + g + ".voicedTalk = true\n";
    s += "\tif soul ~= nil and soul ~= \"\" then " + g + ".voicedSoul = soul end\n";
    // One row's lines come in the same frame; a call a second or more after the last is
    // the next row picked, so whatever the last row still had armed or queued is over.
    s += "\tlocal now = " + g + ".Now()\n";
    s += "\tif now - (" + g + ".voiceAt or -10) > 1.0 and " + g + ".lipKey ~= nil then\n";
    s += "\t\t" + g + ".LipStop(\"next row\")\n";
    s += "\t\t" + g + ".lipKey = nil\n";
    s += "\t\t" + g + ".lipQueue = {}\n";
    s += "\tend\n";
    s += "\t" + g + ".voiceAt = now\n";
    s += "\tif " + g + ".lipKey == nil then\n";
    s += "\t\tpcall(" + g + ".LipArm, who, text)\n";
    s += "\telse\n";
    s += "\t\tlocal q = " + g + ".lipQueue\n";
    s += "\t\tq[#q + 1] = { who = who, text = text }\n";
    s += "\tend\n";
    s += "end\n";
    s += "\n";
    s += g + ".voicedSoul = nil\n";
    s += g + ".voicedTalk = false\n";
    s += g + ".lipQueue = {}\n";
    // Lips move in a session, or in a voiced Talk of the game's own.
    s += "function " + g + ".LipLive()\n";
    s += "\treturn " + g + ".active or " + g + ".voicedTalk\n";
    s += "end\n";
    s += "\n";
    // The next queued line of a voiced Talk, once the one before it has stopped.
    s += "function " + g + ".LipNext()\n";
    s += "\tif " + g + ".active then return end\n";
    s += "\tlocal q = " + g + ".lipQueue\n";
    s += "\tif #q == 0 then return end\n";
    s += "\tlocal nxt = table.remove(q, 1)\n";
    s += "\tpcall(" + g + ".LipArm, nxt.who, nxt.text)\n";
    s += "end\n";
    s += "\n";
    s += "function " + g + ".Say(turn, words, bare)\n";
    s += "\t" + g + ".turn = turn\n";
    s += "\t" + g + ".bare = (bare == true)\n";
    // Directives first, and off the front, so that what reaches the screen is only ever
    // words. A turn that was all directives has still been played: the turn counter has
    // moved on and the pump can look for the next.
    s += "\t" + g + ".more = false\n";
    s += "\t" + g + ".beat = false\n";
    s += "\t" + g + ".optTurn = false\n";
    s += "\twords = " + g + ".Directives(words)\n";
    s += "\tlocal wasStreaming = " + g + ".streaming\n";
    s += "\t" + g + ".streaming = " + g + ".more\n";
    s += "\tif words == \"\" then\n";
    s += "\t\t" + g + ".Log(\"BRIDGE played turn=\" .. turn .. \" -- (directives only)\")\n";
    // A turn of menu options is not the other party finishing: it is answered by the
    // menu, which is already asked for, and not by opening anything.
    s += "\t\tif " + g + ".more then " + g + ".StreamArm()\n";
    s += "\t\telseif " + g + ".optTurn then " + g + ".MenuMaybeDraw()\n";
    s += "\t\telse " + g + ".Heard() end\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    // What is new since the last update of the same line, for the log, so a streamed reply
    // reads down the log as it appeared. A turn that does not extend the last is a new line
    // and is logged whole.
    // Into the store once, when the reply is whole: the flagged updates are the same words
    // growing, and the unflagged turn is the one that was said.
    s += "\tif not " + g + ".more then pcall(" + g + ".StateReply, words) end\n";
    s += "\tlocal piece = words\n";
    s += "\tif wasStreaming and " + g + ".words ~= nil and\n";
    s += "\t\t\tstring.sub(words, 1, #" + g + ".words) == " + g + ".words then\n";
    s += "\t\tpiece = string.sub(words, #" + g + ".words + 1)\n";
    s += "\tend\n";
    s += "\t" + g + ".words = words\n";
    s += "\tlocal seconds = " + g + ".Reading(words)\n";
    // In the game's caption the speaker goes where the game puts its own -- the gold
    // row over the words -- and the words stand alone, as every shipped line reads
    // (notes.md 7 Oct 2026: the name in the text row, in the text's colour, was the
    // typing prototype's layout and read as a hack). The drawn and info roads have no
    // such row, so there the name stays in front of the words. Nothing after either
    // way: the leave hint used to ride here and now shows only as the conversation opens.
    s += "\tif " + g + ".mode == \"caption\" then\n";
    s += "\t\t" + g + ".above = " + g + ".Speaker()\n";
    s += "\t\tif not " + g + ".Show(words, seconds) then return end\n";
    s += "\telse\n";
    s += "\t\t" + g + ".above = \"\"\n";
    s += "\t\tif not " + g + ".Show(" + g + ".Named(words), seconds) then return end\n";
    s += "\tend\n";
    // The mouth moves from the first update and is not restarted on each, which would
    // say so in the log twenty times a line; the finished line sets it for its reading.
    // Not once the app's cues have moved a mouth this run (lipsync): the game's own clips
    // play on the voice then, and the jaw stack over them would be two mouths.
    s += "\tif not " + g + ".lipsync and (not " + g + ".more or " + g + ".speakUntil == 0) then " + g + ".Speak(seconds) end\n";
    // Ticks rather than seconds, since the pump is the only clock here and it runs
    // four times a second. No hold at all while more is coming: the next update takes
    // nothing off the screen, so there is nothing to protect from it.
    s += "\tif " + g + ".more then\n";
    s += "\t\t" + g + ".hold = 0\n";
    s += "\telse\n";
    s += "\t\t" + g + ".hold = " + g + ".ticks + math.ceil(seconds / 0.4)\n";
    s += "\tend\n";
    // The marker the writer answers. It says a line reached the screen, which is now
    // the same moment it was decided rather than something to be waited for: the
    // words no longer travel through the dialogue, so there is no request left to go
    // unheard.
    s += "\t" + g + ".Log(\"BRIDGE played turn=\" .. turn .. \" -- \" .. piece ..\n";
    s += "\t\t(" + g + ".more and \" ...\" or (" + g + ".beat and \" (beat)\" or \"\")))\n";
    // The first line's lateness, in the engine's clock, beside the time the pump has
    // been turning for: equal when the ticks ran on time, far apart when they stalled.
    s += "\tif turn == " + g + ".turnAtOpen + 1 then\n";
    s += "\t\t" + g + ".Log(\"BRIDGE first line \" .. string.format(\"%.1f\", " + g + ".Now() - (" + g +
         ".beganAt or 0)) .. \"s after begin, \" .. (" + g + ".ticks - " + g + ".openedAt) .. \" ticks in\")\n";
    s += "\tend\n";
    // The finished line, once. A streamed update is the same words growing, and
    // speaking each of them would queue a reading per piece. A bare line is the
    // mod's own notice, not anyone talking.
    s += "\tif not " + g + ".more and not " + g + ".bare then " + g + ".Voice(\"npc\", words) end\n";
    // A beat is not the end of their turn: the next line of theirs follows through the
    // pump once this one has had its reading, and Henry's turn comes after that one.
    s += "\tif " + g + ".more then " + g + ".StreamArm()\n";
    s += "\telseif not " + g + ".beat then " + g + ".Heard() end\n";
    s += "end\n";
    s += "\n";
    // The fast look for the next update of a line being written. Its own timer rather
    // than a faster pump, because everything the pump counts -- holds, the stuck-graph
    // watchdog, a quest's patience -- is in ticks of 400ms and would all have to change.
    // Armed once, re-armed after each look while more is expected, and gone the moment
    // the line finishes or the conversation ends.
    s += "function " + g + ".StreamArm()\n";
    s += "\tif " + g + ".streamArmed then return end\n";
    s += "\t" + g + ".streamArmed = true\n";
    s += "\tpcall(function() Script.SetTimer(" + g + ".streamMillis, " + g + ".StreamTick) end)\n";
    s += "end\n";
    s += "\n";
    // As many updates as have landed since the last look, so a line that grew several
    // times while the game was busy catches up in one go instead of trailing an update
    // behind for the rest of the reply. Bounded, since a runaway writer is not something
    // to keep up with.
    s += "function " + g + ".StreamTick()\n";
    s += "\t" + g + ".streamArmed = false\n";
    s += "\tif not " + g + ".active or not " + g + ".streaming then return end\n";
    s += "\tfor i = 1, 20 do\n";
    s += "\t\tif not " + g + ".streaming then break end\n";
    s += "\t\tlocal words = " + g + ".Fetch(" + g + ".turn + 1, true)\n";
    s += "\t\tif words == nil then break end\n";
    s += "\t\tlocal ok, err = pcall(" + g + ".Say, " + g + ".turn + 1, words)\n";
    s += "\t\tif not ok then " + g + ".Log(\"stream raised \" .. tostring(err)) end\n";
    s += "\tend\n";
    s += "\tif " + g + ".streaming then " + g + ".StreamArm() end\n";
    s += "end\n";
    s += "\n";
    // The other party has finished speaking, so it is Henry's turn: whatever was owed has
    // been paid, an Esc from the last turn no longer applies, and the keyboard can open at
    // once rather than at the next tick.
    s += "function " + g + ".Heard()\n";
    s += "\t" + g + ".heard = true\n";
    s += "\t" + g + ".awaiting = false\n";
    s += "\t" + g + ".typingDismissed = false\n";
    // A turn flagged @end is the last: the app has the errand taken up, and the game's own
    // quests close on the accepting line, so no menu and no keyboard -- the conversation
    // closes once the line has been read and the accept beat has played.
    s += "\tif " + g + ".ending then " + g + ".EndWhenRead() return end\n";
    // The menu is how Henry answers now; the keyboard is still there on F9 for a
    // machine with no options coming, and comes back as the (Say) row later.
    s += "\tif " + g + ".menuOn then " + g + ".Choose() else " + g + ".TypingCheck() end\n";
    s += "end\n";
    s += "\n";
    // Whether the menu's code should wait: the line is voiced and the mouth's cues for the
    // other party's line are still being looked for (LipArm), which is until the stop cue
    // says the audio has ended. The camera cut to Henry two seconds into a six-second
    // line was the first thing seen once the mouth moved (12 Oct 2026). Asked from
    // Choose(), after the round's bookkeeping, so the rows that land during the hold are
    // kept for it rather than cleared by a Choose() that came after them.
    s += "function " + g + ".MenuHold()\n";
    s += "\tif not " + g + ".voiced or " + g + ".lipKey == nil or " + g + ".lipWho ~= \"npc\" then return false end\n";
    s += "\tif not " + g + ".menuHeld then\n";
    s += "\t\t" + g + ".menuHeld = true\n";
    s += "\t\t" + g + ".menuHeldAt = " + g + ".ticks\n";
    s += "\t\t" + g + ".Log(\"BRIDGE menu held for the voice\")\n";
    s += "\tend\n";
    s += "\treturn true\n";
    s += "end\n";
    s += "\n";
    // The hold let go: by the stop cue (LipApply), by a tap of the skip button (Skip), or
    // by the grace below. A conversation
    // that ends under it is cleared by Leave, and a close the app asked for (ending) is
    // EndWhenRead's to pace, not the menu's.
    s += "function " + g + ".MenuRelease(why)\n";
    s += "\tif not " + g + ".menuHeld then return end\n";
    s += "\t" + g + ".menuHeld = false\n";
    s += "\t" + g + ".Log(\"BRIDGE menu released -- \" .. why .. \" after \" ..\n";
    s += "\t\tstring.format(\"%.1f\", (" + g + ".ticks - " + g + ".menuHeldAt) * 0.4) .. \"s\")\n";
    s += "\tif not " + g + ".active or " + g + ".ending or not " + g + ".choosing then return end\n";
    s += "\t" + g + ".ChooseGo()\n";
    s += "end\n";
    s += "\n";
    // Every tick: no cue at all for the line by the end of its reading means the app is
    // not writing them (no facials beside its voices), and the menu comes as it did; a
    // cue came, so the mouth is going, and the stop is waited for up to the reading plus
    // the grace, as EndWhenRead waits for the closing line.
    s += "function " + g + ".MenuHeldExpire()\n";
    s += "\tif not " + g + ".menuHeld then return end\n";
    s += "\tlocal cued = " + g + ".lipSeq > 1\n";
    s += "\tlocal limit = " + g + ".hold\n";
    s += "\tif cued then limit = limit + " + g + ".spokenGraceTicks end\n";
    s += "\tif " + g + ".ticks >= limit then\n";
    s += "\t\t" + g + ".MenuRelease(cued and \"grace ran out\" or \"no cue by the reading\")\n";
    s += "\tend\n";
    s += "end\n";
    s += "\n";
    // Leaves when the last line has had its reading time and no quest beat is still owed
    // to the graph: Leave() drops a queued beat, and the accept is the one beat that must
    // not be dropped. Polled on the pump's own period.
    //
    // And, when the app said it is speaking the words (@voiced), until it says they have
    // been heard (Spoken, a hook): the reading time is a guess from the words, and the voice
    // runs past it on a long line, which is the camera cutting the other party off
    // mid-sentence (noted 11 Oct 2026). Bounded, for an app that said so and then did not.
    s += "function " + g + ".EndWhenRead()\n";
    s += "\tif not " + g + ".active or not " + g + ".ending then return end\n";
    s += "\tlocal voiceWait = " + g + ".voiced and not " + g + ".spoken and\n";
    s += "\t\t" + g + ".ticks < " + g + ".hold + " + g + ".spokenGraceTicks\n";
    s += "\tif " + g + ".ticks < " + g + ".hold or voiceWait or " + g + ".questCode ~= 0 or #" + g + ".questQueue > 0 then\n";
    s += "\t\tpcall(function() Script.SetTimer(400, " + g + ".EndWhenRead) end)\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    s += "\tif " + g + ".voiced then\n";
    s += "\t\t" + g + ".Log(\"BRIDGE end waited for the voice: \" .. (" + g + ".spoken and \"spoken\" or \"grace ran out\") ..\n";
    s += "\t\t\t\" \" .. string.format(\"%.1f\", (" + g + ".ticks - " + g + ".hold) * 0.4) .. \"s past the reading\")\n";
    s += "\tend\n";
    s += "\t" + g + ".Log(\"BRIDGE picked 0 end -- (the app closed the conversation)\")\n";
    s += "\t" + g + ".Leave(\"ended by the app\")\n";
    s += "\t" + g + ".OnClosed()\n";
    s += "end\n";
    s += "\n";
    // The loop that carries a conversation. Nothing here decides what is said or
    // when: a turn appears when whoever is writing them decides it should, which is
    // what leaves room for a transcript of real speech and an answer to it.
    // The step is kept apart from the loop that re-arms it. Anything raised in here
    // used to travel out through the pcall that called the pump and take the next
    // timer with it, which stopped the conversation dead and said nothing about why
    // -- indistinguishable from a conversation that had simply ended.
    s += "function " + g + ".Step()\n";
    // Standing next to the other party is what says the conversation is still
    // wanted. Walking off is the way out of something that offers no menu to leave
    // from, and without this the pump would go on saying things to someone who had
    // gone.
    s += "\tif not " + g + ".Near() then\n";
    s += "\t\t" + g + ".Leave(\"walked off\")\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    s += "\tpcall(" + g + ".Hook)\n";
    s += "\n";
    // Asked for once and then held, which is the whole change. Re-asking per line
    // replayed the greeting over every turn, and left the pacing to a behaviour tree
    // that honoured some requests half a minute late and some not at all -- a turn
    // "going unheard" was the commonest thing in the log and each one cost a slot.
    // Now the session is opened once and the words arrive inside it.
    //
    // The global goes up first, because the loop reads it at the end of its first
    // beat and a session that opens with it clear would close again at once.
    s += "\tif not " + g + ".opened then\n";
    s += "\t\t" + g + ".opened = true\n";
    s += "\t\t" + g + ".openedAt = " + g + ".ticks\n";
    s += "\t\t" + g + ".turnAtOpen = " + g + ".turn\n";
    s += "\t\t" + g + ".appChecked = false\n";
    s += "\t\t" + g + ".appDown = false\n";
    s += "\t\t" + g + ".Talking(true)\n";
    s += "\t\t" + g + ".Attempt(" + g + ".npc, " + g + ".user)\n";
    s += "\tend\n";
    s += "\n";
    // The disk, opened for reading once the dialogue is up (Begin says why not sooner),
    // or after three ticks regardless, which is when the camera is pushed for a dialogue
    // that has not come up by itself. Nothing below can read a turn or the heartbeat
    // before this, so the step ends here until it has happened.
    s += "\tif not " + g + ".diskOn then\n";
    s += "\t\tif not " + g + ".InDialog() and " + g + ".ticks - " + g + ".openedAt < 3 then return end\n";
    s += "\t\t" + g + ".diskOn = true\n";
    s += "\t\t" + g + ".Disk(" + g + ".diskRead)\n";
    s += "\t\t" + g + ".Log(\"BRIDGE dialogue up \" .. string.format(\"%.1f\", " + g + ".Now() - (" + g +
         ".beganAt or 0)) .. \"s after begin, \" .. (" + g + ".ticks - " + g + ".openedAt) ..\n";
    s += "\t\t\t\" ticks in, in dialog \" .. tostring(" + g + ".InDialog()))\n";
    s += "\tend\n";
    s += "\n";
    // Whether anybody is going to answer. The app's heartbeat is looked for once, a
    // couple of ticks in so the caption road is up to carry the answer; and if it was
    // there and no first line comes all the same, the backstop says the app did not answer
    // in time. Either way the mod's own line is said in the other party's place and closes
    // the conversation when read (@end), so the player is never held by somebody silent.
    s += "\tif " + g + ".appDown then return end\n";
    s += "\tif not " + g + ".appChecked and " + g + ".ticks - " + g + ".openedAt >= 2 then\n";
    s += "\t\t" + g + ".appChecked = true\n";
    s += "\t\tif not " + g + ".AppAlive() then\n";
    s += "\t\t\t" + g + ".AppDown(\"no heartbeat beside the turn files\")\n";
    s += "\t\t\treturn\n";
    s += "\t\tend\n";
    s += "\tend\n";
    s += "\tif " + g + ".turn == " + g + ".turnAtOpen and not " + g + ".streaming and\n";
    s += "\t\t\t" + g + ".ticks - " + g + ".openedAt > " + g + ".appTicks then\n";
    s += "\t\t" + g + ".AppDown(\"no first line in \" .. (" + g + ".appTicks * 0.4) .. \"s\", " + g + ".appBusyLine)\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    s += "\n";
    s += "\tif " + g + ".opened and not " + g + ".InDialog() and\n";
    s += "\t\t\t" + g + ".ticks - " + g + ".openedAt >= 3 then\n";
    s += "\t\t" + g + ".PushCamera()\n";
    s += "\tend\n";
    s += "\n";
    // A session that opens and never beats is the one failure that traps the player:
    // the graph rests somewhere it will not leave, and because a focused dialogue takes
    // movement away, walking off -- the only exit the player has -- cannot happen
    // either. It was measured: a nested decision with no Autoselect waited two and a
    // half minutes for a choice out of a menu with no entries.
    //
    // A beat is a few seconds, so nothing at all after this long is not slowness. The
    // way out has to come from here, since every other exit waits on a beat.
    s += "\tif " + g + ".beats == 0 and " + g + ".InDialog() and\n";
    s += "\t\t\t" + g + ".ticks - " + g + ".openedAt > " + g + ".stuckTicks then\n";
    s += "\t\t" + g + ".Log(\"BRIDGE open but never beat, the graph is stuck\")\n";
    s += "\t\t" + g + ".Leave(\"graph never beat\")\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    s += "\n";
    // Nothing replaces a line until it has had its time on screen. This is the whole
    // of the pacing now: the writer holds the next turn back until it sees this one
    // reach the screen, and this holds the screen until the words have been read.
    s += "\t" + g + ".QuestExpire()\n";
    // Henry's line, held for his voice: let go when the app has not said in time.
    s += "\t" + g + ".HenryExpire()\n";
    // The menu, held for the other party's voice: the same.
    s += "\t" + g + ".MenuHeldExpire()\n";
    // A long line's next page, when the voice should have reached it.
    s += "\tpcall(" + g + ".PageTick)\n";
    // Every tick and not only when a line lands, so a keyboard that was refused -- no
    // player entity yet, a map still loading -- is offered again rather than never.
    s += "\t" + g + ".TypingCheck()\n";
    // Looked for before the hold is checked rather than after, so that a turn of menu
    // options -- which is not a line and takes nothing off the screen -- is taken the
    // tick it lands and not when the player has finished reading. A spoken line that
    // arrives early waits where it is: a turn file is written once, so reading it again
    // next tick costs a file read and nothing else.
    s += "\tlocal words = " + g + ".Fetch(" + g + ".turn + 1)\n";
    s += "\tif words == nil then return end\n";
    s += "\tif " + g + ".ticks < " + g + ".hold and not string.match(words, \"^@opt%s\") then return end\n";
    s += "\t" + g + ".Say(" + g + ".turn + 1, words)\n";
    // The rows that follow a line are a turn file each, and the app writes them in the
    // same breath as the line (0.2s from the begin line to the fourth row, measured with
    // the fake game); taken one a tick, a written greeting's four rows took four ticks to
    // land, and the menu drew "..." for the rows still to come -- a second or two on every
    // first word with a soul (noted 5 Oct 2026). Rows are not lines and take nothing off
    // the screen, so as many as have already landed are taken in this tick. Bounded, and
    // only rows: a line behind them waits for its tick and its hold as before.
    s += "\tfor i = 1, " + g + ".menuRows do\n";
    s += "\t\tlocal rows = " + g + ".Fetch(" + g + ".turn + 1, true)\n";
    s += "\t\tif rows == nil or not string.match(rows, \"^@opt%s\") then break end\n";
    s += "\t\t" + g + ".Say(" + g + ".turn + 1, rows)\n";
    s += "\tend\n";
    s += "end\n";
    s += "\n";
    // Held on the table rather than built at each arming. A timer set on a closure
    // made on the spot keeps the only reference to it inside the engine's timer, and
    // a pump armed that way was measured to run for a second or so and then stop
    // without a word -- which reads exactly like a conversation that ended by
    // itself. A field of a global table cannot be collected out from under us.
    s += "function " + g + ".Tick()\n";
    s += "\tpcall(" + g + ".Pump)\n";
    s += "\tpcall(" + g + ".WatchArm, \"pump\")\n";
    s += "end\n";
    s += "\n";
    s += "function " + g + ".Pump()\n";
    s += "\tif not " + g + ".active then return end\n";
    s += "\t" + g + ".ticks = " + g + ".ticks + 1\n";
    // Says the loop is alive while it has nothing else to report, so a stop can be
    // told apart from a wait.
    // And every tick for the first few after a conversation opens, with the engine's
    // clock, since kcd.log has no times of its own and the opening is where a first
    // line's wait is. These are what showed the ticks running on time (+0.4, +0.8, +1.2 s)
    // and the first line taken at the third while the app spoke it 6 s later: the wait
    // was the app's, reading the soul's sheet into the model before it looked at the log
    // again (Session::greet), not a timer firing late (12 Oct 2026).
    s += "\tif " + g + ".ticks % 10 == 0 or (" + g + ".opened and " + g + ".ticks - " + g +
         ".openedAt <= 5) then\n";
    s += "\t\t" + g + ".Log(\"pump tick \" .. " + g + ".ticks .. \" turn=\" .. " + g +
         ".turn ..\n";
    s += "\t\t\t\" +\" .. string.format(\"%.1f\", " + g + ".Now() - (" + g +
         ".beganAt or 0)) .. \"s\" ..\n";
    // Whether a dialogue is running at all, which nothing else here can answer. The
    // beat count says the loop is turning and this says the session is up, and a run
    // where they disagree is worth being able to see.
    s += "\t\t\t\" indialog=\" .. tostring(" + g + ".InDialog()) ..\n";
    s += "\t\t\t\" gap=\" .. string.format(\"%.1f\", " + g + ".gap) ..\n";
    s += "\t\t\t\" face=\" .. " + g + ".face)\n";
    s += "\tend\n";
    s += "\tlocal ok, err = pcall(" + g + ".Step)\n";
    s += "\tif not ok then " + g + ".Log(\"pump raised \" .. tostring(err)) end\n";
    // Re-armed even when the step raised. A single bad turn should cost that turn
    // and not the conversation.
    s += "\tif " + g + ".active then\n";
    s += "\t\tlocal armed = pcall(function() Script.SetTimer(400, " + g + ".Tick) end)\n";
    s += "\t\tif not armed then " + g + ".Log(\"Script.SetTimer refused, pump stops here\") end\n";
    s += "\tend\n";
    s += "end\n";
    s += "\n";
    // Named in the beat's ExitScript, so it runs each time round the loop. It decides
    // nothing about what is said -- the words are on their own clock now -- and exists
    // to say the loop is still turning and to notice a player who has gone while it
    // was turning.
    //
    // It is also the only thing that will report a beat that finishes instantly. A
    // wordless response is supposed to take its ReferenceLength; if that turns out to
    // be ignored, the beat count in the log climbs at the frame rate and says so.
    s += "function " + g + ".Beat()\n";
    s += "\t" + g + ".beats = " + g + ".beats + 1\n";
    // A session outliving its conversation has nothing to hold it open for. Clearing
    // the global again is cheap and covers the case where the pump stopped without
    // getting to it.
    s += "\tif not " + g + ".active then\n";
    s += "\t\t" + g + ".Talking(false)\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    s += "\tif not " + g + ".Near() then\n";
    s += "\t\t" + g + ".Leave(\"walked off\")\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    // The one thing that could go badly wrong here is a beat that takes no time, and it
    // has happened: a wordless response ignores ReferenceLength, so before StartDelay
    // was put on it the loop came round every 0.06s and this counter reached 1032 in a
    // 69-second conversation. Nothing on screen said so. A graph spinning at the frame
    // rate is also how an earlier design hung the process hard enough to need killing.
    // At a beat every few seconds the limit below is hours of talking, and at the frame
    // rate it is a couple of minutes.
    s += "\tif " + g + ".beats > " + g + ".beatLimit then\n";
    s += "\t\t" + g + ".Leave(\"beats ran away, \" .. " + g + ".beats .. \" of them\")\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    // Every one of them, not every fifth. A beat lasts seconds, so this is not noise,
    // and a run where the first three went unlogged is indistinguishable from a
    // session that never opened -- which cost a whole test run to work out.
    s += "\t" + g + ".Log(\"BRIDGE beat \" .. " + g + ".beats .. \" turn=\" .. " + g +
         ".turn)\n";
    // This boundary is the one the menu sequence is entered on when the code is up and
    // no quest beat is in the way: the last Beat() until the player picks. Whether or not
    // the menu is still waited for: a draw the watchdog below made before the engine had
    // the menu up (MenuMissed) was thrown away with the step into this sequence, and this
    // boundary is the one to draw over.
    s += "\tif " + g + ".choosing and " + g + ".face == " + g + ".chooseCode and " + g +
         ".questCode == 0 then\n";
    s += "\t\tif not " + g + ".menuWait then " + g + ".Log(\"BRIDGE menu boundary after the early draw, drawn again\") end\n";
    s += "\t\t" + g + ".menuDrawn = 0\n";
    s += "\t\t" + g + ".MenuEntered()\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    s += "\t" + g + ".ExpireSpeak()\n";
    s += "end\n";
    s += "\n";
    // The far branch of the same decision, so this is the session actually closing
    // rather than us asking it to. Worth its own line in the log: the gap between
    // asking and this is how long the graph took to notice, and a conversation that
    // ends any other way -- crash, interrupt, the loop stopping on its own -- is one
    // where this never appears.
    s += "function " + g + ".OnClosed()\n";
    s += "\t" + g + ".opened = false\n";
    s += "\t" + g + ".pushed = false\n";
    s += "\t" + g + ".Log(\"BRIDGE session closed after \" .. " + g + ".beats .. \" beats\")\n";
    s += "end\n";
    s += "\n";
    // Everything that ends a conversation comes through here, so there is one place
    // that both stops the pump and lets the session go.
    s += "function " + g + ".Leave(why)\n";
    s += "\tif not " + g + ".active then return end\n";
    s += "\t" + g + ".active = false\n";
    s += "\tpcall(" + g + ".StateEnd)\n";
    // A line still being written is not going to be finished for anybody.
    s += "\t" + g + ".streaming = false\n";
    // And a close the app asked for is spent, whichever way the conversation ended.
    s += "\t" + g + ".ending = false\n";
    s += "\t" + g + ".voiced = false\n";
    s += "\t" + g + ".spoken = false\n";
    s += "\t" + g + ".henryUntil = 0\n";
    // Nor is a menu going to be drawn, or a pick answered. A say slot still live is told
    // to the app (say stop), which stops listening.
    s += "\tif " + g + ".saying then " + g + ".SayStop(\"conversation over\") end\n";
    s += "\t" + g + ".SayReset()\n";
    s += "\t" + g + ".choosing = false\n";
    s += "\t" + g + ".menuWait = false\n";
    s += "\t" + g + ".menuUp = false\n";
    s += "\t" + g + ".menuToken = " + g + ".menuToken + 1\n";
    // Before the caption is emptied below, so the typed line goes with it, and before
    // anything else so that a keyboard is never left owning w and space after the camera
    // has given movement back.
    s += "\t" + g + ".TypingClose()\n";
    s += "\t" + g + ".Talking(false)\n";
    s += "\t" + g + ".Face(\"idle\")\n";
    // The borrowed mouth too, or the face stays on the clip's last frame after the talk.
    s += "\tpcall(" + g + ".LipStop, \"conversation over\")\n";
    s += "\t" + g + ".lipKey = nil\n";
    s += "\t" + g + ".menuHeld = false\n";
    // A hail's line still on the narrative subtitle (put up in the breath before the
    // conversation opened) goes with it (HailShow, 28 Oct 2026).
    s += "\tpcall(" + g + ".HailHide)\n";
    // A beat still waiting to play is not going to now, and a code left up would gate
    // every face of the next conversation.
    s += "\tif " + g + ".questCode ~= 0 or #" + g + ".questQueue > 0 then\n";
    s += "\t\t" + g + ".Log(\"BRIDGE quest left with code=\" .. " + g + ".questCode ..\n";
    s += "\t\t\t\" queued=\" .. #" + g + ".questQueue)\n";
    s += "\tend\n";
    s += "\t" + g + ".questQueue = {}\n";
    s += "\t" + g + ".SetQuestCode(0)\n";
    // Given back here for the same reason the caption is: this is the only way out, so the
    // setting is never raised for longer than a conversation, and the next level load is
    // never the one that pays for it.
    s += "\t" + g + ".Disk(" + g + ".diskRest)\n";
    s += "\tpcall(" + g + ".FramesRest)\n";
    // Given back on the way out, and this is the only way out, so the game's captions are
    // never hidden for longer than a conversation. A run that ends without passing through
    // here leaves them hidden until the next launch rebuilds the UI, which is a great deal
    // better than the reverse bargain.
    //
    // When the caption is where our words were going it is emptied instead of shown, since
    // it was never hidden and would otherwise keep the last line of the conversation up
    // afterwards, waiting for whatever the game next has to say to overwrite it.
    s += "\tif " + g + ".mode == \"caption\" then\n";
    s += "\t\t" + g + ".shown = \"\"\n";
    s += "\t\t" + g + ".above = \"\"\n";
    s += "\t\t" + g + ".Write(\"\")\n";
    s += "\telse\n";
    s += "\t\t" + g + ".Caption(true)\n";
    s += "\tend\n";
    s += "\t" + g + ".Log(\"BRIDGE left, \" .. why .. \", \" ..\n";
    s += "\t\tstring.format(\"%.1f\", " + g + ".gap) .. \"m away\")\n";
    // The loop only reads the global at the end of a beat, so a player who has
    // already turned away would stand in the camera until this one finishes. This is
    // the watchdog for that, and for a beat that never finishes at all.
    s += "\tpcall(function() Script.SetTimer(" + g + ".cutDelay, " + g + ".Cut) end)\n";
    s += "end\n";
    s += "\n";
    // Everything the engine passes to Lua while a conversation is open, said out loud in
    // the log, since it is the only measurement of what input survives a dialogue that
    // owns the HUD.
    //
    // Returns true for an action that was ours and is not to reach the shipped handler:
    // the keys of the typing maps, which exist only in this mod's XML and mean nothing
    // to the game.
    s += "function " + g + ".OnPlayerAction(action, activation)\n";
    // First and unconditionally, since the maps are ours by name whether or not a
    // conversation is up, and a stray F9 between conversations should do nothing at all
    // rather than reach the game as an action it has never heard of.
    s += "\tif string.sub(tostring(action), 1, 6) == \"mtype_\" then\n";
    s += "\t\treturn " + g + ".Typed(action, activation)\n";
    s += "\tend\n";
    s += "\tif not " + g + ".active then return false end\n";
    s += "\t" + g + ".Log(\"action \" .. tostring(action) .. \" \" .. tostring(activation))\n";
    // The pad's A (or the mouse button) with the keyboard open from the menu: the line
    // goes, as Enter sends it. The typing map binds it under the engine's own name, since
    // the engine's map is silenced while ours is up; outside say-mode it is nobody's.
    s += "\tif action == \"dialog_confirm\" then\n";
    s += "\t\tif " + g + ".saying and activation == \"press\" then pcall(" + g + ".SaySend) end\n";
    s += "\t\treturn false\n";
    s += "\tend\n";
    // On the way down rather than the way up, so the button answers while it is still
    // held. A hold repeats, and release follows every press, but Leave only does its
    // work once.
    s += "\tif " + g + ".buttonActions[action] ~= true then return false end\n";
    // The engine's own hold is ignored; a press starts a clock of our own and a release
    // stops it. One press of B arrives twice over, under both names, so everything here
    // has to survive being run again for the same button: the second press finds the
    // clock already started, and the second release finds the tap already spent.
    s += "\tif activation == \"press\" then\n";
    s += "\t\t" + g + ".down = true\n";
    s += "\t\t" + g + ".pending = " + g + ".pending + 1\n";
    s += "\t\tpcall(function() Script.SetTimer(" + g + ".holdMillis, " + g + ".Held) end)\n";
    s += "\telseif activation == \"release\" and " + g + ".down then\n";
    s += "\t\t" + g + ".down = false\n";
    // A tap in say-mode is the line's: cleared, or with nothing in it the menu brought
    // back. Nothing to skip then anyway, with no line of the other party's playing.
    s += "\t\tif " + g + ".saying then pcall(" + g + ".SayBack) else " + g + ".Skip() end\n";
    s += "\tend\n";
    // Watched, not taken: the game's own dialogue still wants its skip.
    s += "\treturn false\n";
    s += "end\n";
    s += "\n";
    // Fires for a press that has not been let go of yet.
    s += "function " + g + ".Held()\n";
    // Counted down first and unconditionally, so a timer arriving while nothing is going
    // on cannot leave the count standing high enough to swallow a real hold later.
    s += "\tif " + g + ".pending > 0 then " + g + ".pending = " + g + ".pending - 1 end\n";
    // Not the last timer outstanding, so a later press armed one after this and this one
    // has no business ending anything -- see pending above.
    s += "\tif " + g + ".pending > 0 then return end\n";
    s += "\tif not " + g + ".active then return end\n";
    s += "\tif not " + g + ".down then return end\n";
    s += "\t" + g + ".down = false\n";
    s += "\t" + g + ".Leave(\"asked to go\")\n";
    s += "end\n";
    s += "\n";
    // Where stopping the voice will go, once there is one to stop: a skipped line should
    // fall silent, not talk over the line after it.
    s += "function " + g + ".Skip()\n";
    s += "\t" + g + ".Log(\"BRIDGE skipped turn=\" .. " + g + ".turn)\n";
    s += "\t" + g + ".hold = 0\n";
    s += "\t" + g + ".Speak(0)\n";
    // The borrowed mouth comes off and the menu it was holding comes up, as the game's
    // own skip ends a line: the player has heard enough of it. The audio itself runs on
    // in the app for now (the same as a skipped line's words staying on the caption).
    s += "\tif " + g + ".lipEnt ~= nil or " + g + ".menuHeld then\n";
    s += "\t\tpcall(" + g + ".LipStop, \"skipped\")\n";
    s += "\t\t" + g + ".lipKey = nil\n";
    s += "\t\tpcall(" + g + ".MenuRelease, \"skipped\")\n";
    s += "\tend\n";
    s += "end\n";
    s += "\n";
    // ---- Typing a reply -------------------------------------------------------------
    //
    // Once, at start-up. LoadFromXML is additive -- it creates each <actionmap> it finds
    // and leaves defaultProfile.xml alone -- and was measured to succeed before any level
    // is loaded. Both maps start off; a conversation turns the toggle on and typing turns
    // the keys on.
    s += "function " + g + ".LoadTyping()\n";
    s += "\tif " + g + ".typingLoaded then return end\n";
    s += "\tif ActionMapManager == nil or ActionMapManager.LoadFromXML == nil or\n";
    s += "\t\t\tActionMapManager.EnableActionMap == nil then\n";
    s += "\t\t" + g + ".Log(\"ActionMapManager is not bound, replies cannot be typed\")\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    s += "\tlocal ok, err = pcall(function() ActionMapManager.LoadFromXML(" + g + ".typingXml) end)\n";
    s += "\tif not ok then\n";
    s += "\t\t" + g + ".Log(\"LoadFromXML \" .. " + g + ".typingXml .. \" raised \" .. tostring(err))\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    s += "\t" + g + ".typingLoaded = true\n";
    s += "\t" + g + ".TypingMap(" + g + ".typingToggleMap, false)\n";
    s += "\t" + g + ".TypingMap(" + g + ".typingKeysMap, false)\n";
    s += "\t" + g + ".Log(\"typing actionmaps loaded from \" .. " + g + ".typingXml)\n";
    s += "end\n";
    s += "\n";
    s += "function " + g + ".TypingMap(name, on)\n";
    s += "\tlocal ok, err = pcall(function() ActionMapManager.EnableActionMap(name, on) end)\n";
    s += "\tif not ok then\n";
    s += "\t\t" + g + ".Log(\"  EnableActionMap \" .. name .. \" raised \" .. tostring(err))\n";
    s += "\tend\n";
    s += "\treturn ok\n";
    s += "end\n";
    s += "\n";
    // As a conversation opens. A map made before a level has no listener entity, so
    // both are pointed at the player here, where one certainly exists -- the Warhorse
    // bind for that takes an entity id. Then F9 is armed for the conversation.
    s += "function " + g + ".TypingOpen(user)\n";
    s += "\t" + g + ".typing = false\n";
    s += "\t" + g + ".typed = \"\"\n";
    s += "\t" + g + ".words = nil\n";
    s += "\t" + g + ".said = nil\n";
    s += "\t" + g + ".cursorOn = true\n";
    s += "\t" + g + ".more = false\n";
    s += "\t" + g + ".beat = false\n";
    s += "\t" + g + ".streaming = false\n";
    s += "\t" + g + ".ending = false\n";
    s += "\t" + g + ".shift = false\n";
    s += "\t" + g + ".awaiting = false\n";
    s += "\t" + g + ".heard = false\n";
    s += "\t" + g + ".typingDismissed = false\n";
    s += "\t" + g + ".SayReset()\n";
    s += "\tif not " + g + ".typingLoaded then return end\n";
    s += "\tlocal who = user\n";
    s += "\tif who == nil or who.id == nil then who = " + g + ".Player() end\n";
    s += "\tif who ~= nil and who.id ~= nil and ActionMapManager.SetActionListener ~= nil then\n";
    s += "\t\tfor _, name in ipairs({ " + g + ".typingToggleMap, " + g + ".typingKeysMap }) do\n";
    s += "\t\t\tlocal ok, err = pcall(function() ActionMapManager.SetActionListener(name, who.id) end)\n";
    s += "\t\t\tif not ok then\n";
    s += "\t\t\t\t" + g + ".Log(\"  SetActionListener \" .. name .. \" raised \" .. tostring(err))\n";
    s += "\t\t\tend\n";
    s += "\t\tend\n";
    s += "\tend\n";
    s += "\t" + g + ".TypingMap(" + g + ".typingToggleMap, true)\n";
    s += "end\n";
    s += "\n";
    s += "function " + g + ".TypingClose()\n";
    s += "\t" + g + ".SetTyping(false)\n";
    s += "\t" + g + ".awaiting = false\n";
    s += "\tif " + g + ".typingLoaded and " + g + ".spotArmed == nil then " + g + ".TypingMap(" + g + ".typingToggleMap, false) end\n";
    s += "end\n";
    s += "\n";
    // The exclusive map up or down, and the caption told either way, since the typed
    // line is part of what it shows. Nothing else announces it: a hint used to go down
    // the notice widget the first time the keyboard opened, and landed as a line of
    // debug text in the middle of the screen at the one moment a player is being asked
    // to look at a face. The prompt and its blinking cursor are the announcement.
    s += "function " + g + ".SetTyping(on)\n";
    s += "\tif " + g + ".typing == on then return end\n";
    s += "\t" + g + ".typing = on\n";
    s += "\t" + g + ".typed = \"\"\n";
    s += "\t" + g + ".shift = false\n";
    s += "\tif " + g + ".typingLoaded then " + g + ".TypingMap(" + g + ".typingKeysMap, on) end\n";
    s += "\t" + g + ".Log(\"BRIDGE typing \" .. (on and \"on\" or \"off\"))\n";
    // Shown, not blank, whenever the field opens or a key lands, so the cursor is there
    // to be seen the moment there is something to see it against.
    s += "\t" + g + ".cursorOn = true\n";
    s += "\tif on then " + g + ".BlinkArm() end\n";
    s += "\t" + g + ".Refresh()\n";
    s += "end\n";
    s += "\n";
    // The cursor's own timer, on for as long as typing is. Armed once and re-armed after
    // each toggle; a tick that finds typing over does nothing and arms nothing, which is
    // how it stops.
    s += "function " + g + ".BlinkArm()\n";
    s += "\tif " + g + ".blinkArmed then return end\n";
    s += "\t" + g + ".blinkArmed = true\n";
    s += "\tpcall(function() Script.SetTimer(" + g + ".blinkMillis, " + g + ".Blink) end)\n";
    s += "end\n";
    s += "\n";
    s += "function " + g + ".Blink()\n";
    s += "\t" + g + ".blinkArmed = false\n";
    s += "\tif not " + g + ".typing or not " + g + ".active then return end\n";
    s += "\t" + g + ".cursorOn = not " + g + ".cursorOn\n";
    s += "\t" + g + ".Refresh()\n";
    s += "\t" + g + ".BlinkArm()\n";
    s += "end\n";
    s += "\n";
    // The speaker's name in front of their words, as the nameplate reads it. Bare words
    // for a soul this mod did not name, which should not happen, since the option that
    // starts a conversation is only offered to the ones it did.
    s += "function " + g + ".Named(words)\n";
    s += "\tif " + g + ".bare or " + g + ".npcName == \"\" then return words end\n";
    s += "\treturn " + g + ".npcName .. \": \" .. words\n";
    s += "end\n";
    s += "\n";
    // The same name for the caption's speaker row, and nothing when the words are nobody's
    // (a line of the mod's own), which leaves the row undrawn.
    s += "function " + g + ".Speaker()\n";
    s += "\tif " + g + ".bare or " + g + ".npcName == \"\" then return \"\" end\n";
    s += "\treturn " + g + ".npcName\n";
    s += "end\n";
    s += "\n";
    // Whether it is Henry's turn, and the keyboard if so. Called from Say and from every
    // tick of the pump, so the conditions live here and nowhere else: a conversation is
    // up, the other party has said something, no reply is owed, and the player has not
    // asked for quiet.
    //
    // Off by default (typingAuto). The "Henry: _" prompt that opened itself after every
    // line was the early prototype's way of taking a typed reply, and it kept surfacing in
    // focused conversations where the menu is the way to answer (notes.md, 5 Oct 2026).
    // Free-form speech is to come in by another road, so the pump no longer opens the
    // keyboard on its own; F9 (Typed, mtype_toggle) still opens it by hand, and that road
    // is unchanged. The flag is left so a harness or a probe can turn the old behaviour on.
    s += "function " + g + ".TypingCheck()\n";
    s += "\tif not " + g + ".typingAuto then return end\n";
    s += "\tif not " + g + ".active or not " + g + ".typingLoaded then return end\n";
    s += "\tif " + g + ".typing or " + g + ".awaiting or " + g + ".typingDismissed then return end\n";
    s += "\tif not " + g + ".heard then return end\n";
    // Never over a menu. Choose() puts the keyboard down as it asks for the rows, but a
    // line that lands while the menu is asked for or up -- the app's canned turn after a
    // greeting, a reply that outran its rows -- comes through Heard(), which clears the
    // dismissal and finds Choose() already choosing; the next tick then opened the
    // exclusive map under the drawn rows and took the arrows and Enter before the
    // dialogue saw them (measured 5 Oct 2026: two of three roads of the first sweep lost
    // their holder row to "Henry: _").
    s += "\tif " + g + ".menuOn and " + g + ".choosing then return end\n";
    s += "\t" + g + ".SetTyping(true)\n";
    s += "end\n";
    s += "\n";
    // What the caption shows, as two rows: the text and the row over it. Not typing, that
    // is the line and whatever Said left above it. Typing, the entry takes the text row --
    // the player's name, the reply so far, and a cursor that is an underscore or its
    // width in blank by the blink timer -- and the other party's line moves up into the
    // row over it, so the player answers with the question still in view.
    // A long line in pages (notes.md 14 Oct 2026, Benesh's speech on Henry's return): the
    // caption shows roughly four lines, and a reply longer than that ran off the plate. A
    // line over pageChars is cut into pages at its sentence ends (or, failing one in reach,
    // at a word), and the pages are turned on the pump as the voice should be reaching them:
    // each page's share of a speaking time of speakRate characters a second, counted from
    // the moment the words went up, and again from the voice's first cue (PageAnchor: the
    // audio begins a second or two after the words, once the app has made it); the audio's
    // end turns to the last page whatever the clock says (PageLast). The voice itself is
    // untouched, it is one piece of audio; only the words under it move. A line being
    // streamed shows its last page, the newest words, until it is whole. Pages belong to
    // the line they were cut from: a line put up another way (Henry's own, the leave hint)
    // shows as it is.
    s += g + ".pageChars = " + std::to_string(kCaptionPageChars) + "\n";
    s += g + ".speakRate = " + std::to_string(kCaptionSpeakRate) + "\n";
    s += g + ".pages = {}\n";
    s += g + ".pagesOf = nil\n";
    s += g + ".page = 1\n";
    s += g + ".pageAt = 0\n";
    s += g + ".pageDur = {}\n";
    s += "function " + g + ".Pages(text)\n";
    s += "\tlocal pages = {}\n";
    s += "\tlocal rest = text\n";
    s += "\tlocal limit = " + g + ".pageChars\n";
    s += "\twhile #rest > limit do\n";
    s += "\t\tlocal cut = nil\n";
    s += "\t\tlocal i = 1\n";
    s += "\t\twhile true do\n";
    s += "\t\t\tlocal a, b = string.find(rest, \"[%.!%?;:]['\\\"]?%s+\", i)\n";
    s += "\t\t\tif a == nil or a > limit then break end\n";
    s += "\t\t\tcut = b\n";
    s += "\t\t\ti = b + 1\n";
    s += "\t\tend\n";
    s += "\t\tif cut == nil or cut < limit / 3 then\n";
    s += "\t\t\tlocal j = limit\n";
    s += "\t\t\twhile j > limit / 2 and string.sub(rest, j, j) ~= \" \" do j = j - 1 end\n";
    s += "\t\t\tcut = (j > limit / 2) and j or limit\n";
    s += "\t\tend\n";
    s += "\t\tlocal head = string.gsub(string.sub(rest, 1, cut), \"%s+$\", \"\")\n";
    s += "\t\tif head ~= \"\" then pages[#pages + 1] = head end\n";
    s += "\t\trest = string.gsub(string.sub(rest, cut + 1), \"^%s+\", \"\")\n";
    s += "\tend\n";
    s += "\trest = string.gsub(rest, \"%s+$\", \"\")\n";
    s += "\tif rest ~= \"\" or #pages == 0 then pages[#pages + 1] = rest end\n";
    s += "\treturn pages\n";
    s += "end\n";
    s += "\n";
    s += "function " + g + ".PageSet(text, tail)\n";
    s += "\t" + g + ".pages = " + g + ".Pages(text)\n";
    s += "\t" + g + ".pagesOf = text\n";
    s += "\t" + g + ".page = tail and #" + g + ".pages or 1\n";
    s += "\t" + g + ".pageAt = " + g + ".Now()\n";
    s += "\t" + g + ".pageDur = {}\n";
    s += "\tlocal secs = #text / " + g + ".speakRate\n";
    s += "\tfor i, p in ipairs(" + g + ".pages) do " + g + ".pageDur[i] = secs * #p / math.max(#text, 1) end\n";
    s += "\tif #" + g + ".pages > 1 and not tail then\n";
    s += "\t\t" + g + ".Log(\"BRIDGE caption in \" .. #" + g + ".pages .. \" pages (\" .. #text .. \" chars)\")\n";
    s += "\tend\n";
    s += "end\n";
    s += "\n";
    s += "function " + g + ".Page()\n";
    s += "\tif " + g + ".pagesOf ~= " + g + ".shown then return " + g + ".shown end\n";
    s += "\treturn " + g + ".pages[" + g + ".page] or " + g + ".shown\n";
    s += "end\n";
    s += "\n";
    s += "function " + g + ".PageTick()\n";
    s += "\tif " + g + ".pagesOf ~= " + g + ".shown or " + g + ".page >= #" + g + ".pages then return end\n";
    s += "\tif " + g + ".Now() - " + g + ".pageAt < (" + g + ".pageDur[" + g + ".page] or 0) then return end\n";
    s += "\t" + g + ".page = " + g + ".page + 1\n";
    s += "\t" + g + ".pageAt = " + g + ".Now()\n";
    s += "\t" + g + ".Log(\"BRIDGE caption page \" .. " + g + ".page .. \" of \" .. #" + g + ".pages)\n";
    s += "\t" + g + ".Refresh()\n";
    s += "end\n";
    s += "\n";
    s += "function " + g + ".PageAnchor()\n";
    s += "\tif " + g + ".pagesOf ~= " + g + ".shown or #" + g + ".pages < 2 or " + g + ".more then return end\n";
    s += "\t" + g + ".page = 1\n";
    s += "\t" + g + ".pageAt = " + g + ".Now()\n";
    s += "\t" + g + ".Refresh()\n";
    s += "end\n";
    s += "\n";
    s += "function " + g + ".PageLast()\n";
    s += "\tif " + g + ".pagesOf ~= " + g + ".shown or " + g + ".page >= #" + g + ".pages then return end\n";
    s += "\t" + g + ".page = #" + g + ".pages\n";
    s += "\t" + g + ".Log(\"BRIDGE caption last page with the audio's end\")\n";
    s += "\t" + g + ".Refresh()\n";
    s += "end\n";
    s += "\n";
    s += "function " + g + ".Display()\n";
    s += "\tif not " + g + ".typing then return " + g + ".Page(), " + g + ".above end\n";
    s += "\tlocal over = \"\"\n";
    s += "\tif " + g + ".words ~= nil then over = " + g + ".Named(" + g + ".words) end\n";
    s += "\treturn " + g + ".playerName .. \": \" .. " + g + ".typed ..\n";
    s += "\t\t(" + g + ".cursorOn and \"_\" or " + g + ".cursorOff), over\n";
    s += "end\n";
    s += "\n";
    // The caption brought up to date with the buffer. Only the caption road shows the
    // reply as it is typed; the others get the line when Enter is pressed.
    s += "function " + g + ".Refresh()\n";
    s += "\tif " + g + ".mode ~= \"caption\" then return end\n";
    s += "\t" + g + ".Write(" + g + ".Display())\n";
    s += "end\n";
    s += "\n";
    // Enter, or a row picked. The line goes out on the log for the app, which has no other
    // way of hearing it, and the caption keeps it up as the player's own until the first
    // of the reply replaces it: in the game's caption as the game shows Henry's own lines,
    // his name on the speaker row and the words below; on the drawn and info roads as
    // "Henry: line" over the other party's place held by an ellipsis, since those have
    // one row. The hold on the previous line is released, because a player who has
    // answered it has read it.
    s += "function " + g + ".Said(line)\n";
    s += "\t" + g + ".Log(\"BRIDGE said \" .. line)\n";
    s += "\t" + g + ".Voice(\"henry\", line)\n";
    s += "\tpcall(" + g + ".StateSaid, line)\n";
    s += "\t" + g + ".awaiting = true\n";
    s += "\t" + g + ".said = line\n";
    s += "\t" + g + ".SetTyping(false)\n";
    // Written, Henry's line gives way to the reply the moment it lands. Spoken, it is held
    // -- the reply waits where it is, and the camera is his -- until the app says the
    // voice is done with it (Spoken), or the reading and a grace have passed (HenryExpire).
    // A speak stretch still running is ended first, since its expiry would write the rest
    // face over his code before the graph had read it.
    s += "\tif " + g + ".voiced and line ~= \"...\" then\n";
    s += "\t\t" + g + ".speakUntil = 0\n";
    s += "\t\t" + g + ".henryUntil = " + g + ".ticks + math.ceil(" + g + ".Reading(line) / 0.4) + " + g + ".spokenGraceTicks\n";
    s += "\t\t" + g + ".hold = " + g + ".henryUntil\n";
    s += "\t\t" + g + ".SetFaceCode(" + g + ".henryCode)\n";
    s += "\telse\n";
    s += "\t\t" + g + ".hold = 0\n";
    s += "\tend\n";
    s += "\tif " + g + ".mode == \"caption\" then\n";
    s += "\t\t" + g + ".above = " + g + ".playerName\n";
    s += "\t\t" + g + ".shown = line\n";
    s += "\t\t" + g + ".Refresh()\n";
    s += "\telse\n";
    s += "\t\t" + g + ".above = " + g + ".playerName .. \": \" .. line\n";
    s += "\t\t" + g + ".shown = " + g + ".Named(\"...\")\n";
    s += "\t\t" + g + ".Show(" + g + ".shown, 3)\n";
    s += "\tend\n";
    s += "end\n";
    s += "\n";
    // One key of the typing maps. Always true: these actions are ours by name and the
    // game has no handler for them.
    s += "function " + g + ".Typed(action, activation)\n";
    s += "\tif action == \"mtype_toggle\" then\n";
    // The runner's answer to a rebaseline step (see AutoAck) comes down this key.
    s += "\t\tif activation == \"press\" and " + g + ".auto ~= nil and " + g + ".auto.awaitAck then\n";
    s += "\t\t\tpcall(" + g + ".AutoAck, \"F9\")\n";
    s += "\t\t\treturn true\n";
    s += "\t\tend\n";
    // Out of a conversation the toggle is the leave key, live only while Henry stands at a
    // leave's or a return's place with the thing on him (SpotArm).
    s += "\t\tif activation == \"press\" and not " + g + ".active and " + g + ".spotArmed ~= nil then\n";
    s += "\t\t\tpcall(" + g + ".SpotKey)\n";
    s += "\t\t\treturn true\n";
    s += "\t\tend\n";
    s += "\t\tif activation ~= \"press\" or not " + g + ".active then return true end\n";
    // From the menu's say row, F9 is the way back to the menu, not a dismissal that
    // would leave the conversation with neither.
    s += "\t\tif " + g + ".saying then " + g + ".SayLeave(\"toggle\") return true end\n";
    // Not while a menu is up: the keyboard's map would take the arrows and Enter from
    // it, and a line typed then has no row to be picked through. The say row is the
    // road for that.
    s += "\t\tif " + g + ".choosing then return true end\n";
    s += "\t\tif " + g + ".typing then\n";
    s += "\t\t\t" + g + ".typingDismissed = true\n";
    s += "\t\t\t" + g + ".SetTyping(false)\n";
    s += "\t\telse\n";
    // By hand, so it also serves as the way back in when a reply never came.
    s += "\t\t\t" + g + ".typingDismissed = false\n";
    s += "\t\t\t" + g + ".awaiting = false\n";
    s += "\t\t\t" + g + ".SetTyping(true)\n";
    s += "\t\tend\n";
    s += "\t\treturn true\n";
    s += "\tend\n";
    s += "\tlocal key = string.sub(action, 7)\n";
    s += "\tif key == \"lshift\" or key == \"rshift\" then\n";
    s += "\t\t" + g + ".shift = (activation == \"press\")\n";
    s += "\t\treturn true\n";
    s += "\tend\n";
    s += "\tif activation ~= \"press\" and not (key == \"backspace\" and activation == \"hold\") then\n";
    s += "\t\treturn true\n";
    s += "\tend\n";
    // A key arriving after the map went down, which the engine can do for a release.
    s += "\tif not " + g + ".typing then return true end\n";
    // Enter sends. From the menu's say row (say-mode) an empty line is nothing rather
    // than nothing said.
    s += "\tif key == \"enter\" then\n";
    s += "\t\tif " + g + ".saying then " + g + ".SaySend()\n";
    s += "\t\telseif " + g + ".typed ~= \"\" then " + g + ".Said(" + g + ".typed) end\n";
    s += "\t\treturn true\n";
    s += "\tend\n";
    // The way out while the keyboard has every key: the same exit as holding the button,
    // through the one function that ends a conversation. F9 is what puts the keyboard
    // away without leaving. In say-mode escape is the pad's B: the line cleared, or with
    // nothing in it the menu back.
    s += "\tif key == \"escape\" then\n";
    s += "\t\tif " + g + ".saying then " + g + ".SayBack() else " + g + ".Leave(\"escape\") end\n";
    s += "\t\treturn true\n";
    s += "\tend\n";
    s += "\tif key == \"backspace\" then\n";
    s += "\t\t" + g + ".typed = string.sub(" + g + ".typed, 1, -2)\n";
    s += "\telse\n";
    s += "\t\tlocal ch = " + g + ".TypedChar(key)\n";
    s += "\t\tif ch ~= nil then " + g + ".typed = " + g + ".typed .. ch end\n";
    s += "\tend\n";
    s += "\t" + g + ".cursorOn = true\n";
    s += "\t" + g + ".Refresh()\n";
    s += "\treturn true\n";
    s += "end\n";
    s += "\n";
    // The character a key name stands for under the shift state, or nil for a key that is
    // not one (a US layout, by hand; see typedPlain and typedShifted).
    s += "function " + g + ".TypedChar(key)\n";
    s += "\tif " + g + ".shift and " + g + ".typedShifted[key] then return " + g + ".typedShifted[key] end\n";
    s += "\tif " + g + ".typedPlain[key] then return " + g + ".typedPlain[key] end\n";
    s += "\tif #key == 1 then return " + g + ".shift and string.upper(key) or key end\n";
    s += "\treturn nil\n";
    s += "end\n";
    s += "\n";
    s += "function " + g + ".Cut()\n";
    // A conversation started since this was armed is not the one it was armed for.
    s += "\tif " + g + ".active then return end\n";
    // Stood down by the graph reporting its own close, which is the ordinary way out and
    // the one that leaves nothing to force. IsInDialog can still be true behind it,
    // through the fade.
    s += "\tif not " + g + ".opened then return end\n";
    s += "\tif not " + g + ".InDialog() then return end\n";
    // Tried in turn, because human carries both and only the plural is known to take no
    // arguments -- the singular raises when called bare, which is all this ever did. A
    // call that returns without ending the session is not a call that worked, so the
    // session itself is the test rather than the absence of an error.
    s += "\tlocal tries = { \"InterruptDialogs\", \"InterruptDialog\" }\n";
    s += "\tfor i = 1, #tries do\n";
    s += "\t\tlocal name = tries[i]\n";
    s += "\t\tlocal ok, err = pcall(function() player.human[name](player.human) end)\n";
    s += "\t\tif not ok then\n";
    s += "\t\t\t" + g + ".Log(\"  \" .. name .. \" raised \" .. tostring(err))\n";
    s += "\t\telseif " + g + ".InDialog() then\n";
    s += "\t\t\t" + g + ".Log(\"  \" .. name .. \" returned but the session is still up\")\n";
    s += "\t\telse\n";
    s += "\t\t\t" + g + ".Log(\"  \" .. name .. \" cut a session that outlived the conversation\")\n";
    s += "\t\t\treturn\n";
    s += "\t\tend\n";
    s += "\tend\n";
    s += "end\n";
    s += "\n";
    s += "function " + g + ".Talking(on)\n";
    s += "\tlocal ok, err = pcall(function()\n";
    s += "\t\tVariables.SetGlobal(" + g + ".talkVar, on and 1 or 0)\n";
    s += "\tend)\n";
    s += "\tif not ok then " + g + ".Log(\"  SetGlobal raised \" .. tostring(err)) end\n";
    s += "end\n";
    s += "\n";
    // The graph only reads the face global when Autoselecting the next beat, so a
    // call here is a request for the following loop, not an instant clip. Speak is
    // the exception that also arms a tick deadline; Face() is the held expression.
    s += "function " + g + ".SetFaceCode(code)\n";
    s += "\t" + g + ".face = code\n";
    s += "\tlocal ok, err = pcall(function()\n";
    s += "\t\tVariables.SetGlobal(" + g + ".faceVar, code)\n";
    s += "\tend)\n";
    s += "\tif not ok then " + g + ".Log(\"  SetGlobal face raised \" .. tostring(err)) end\n";
    s += "end\n";
    s += "\n";
    s += "function " + g + ".Face(name)\n";
    s += "\tlocal code = " + g + ".faces[name]\n";
    s += "\tif code == nil then\n";
    s += "\t\t" + g + ".Log(\"BRIDGE face unknown \" .. tostring(name))\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    s += "\tif name ~= \"speak\" then\n";
    s += "\t\t" + g + ".restFace = code\n";
    s += "\t\t" + g + ".speakUntil = 0\n";
    s += "\tend\n";
    s += "\t" + g + ".SetFaceCode(code)\n";
    s += "\t" + g + ".Log(\"BRIDGE face \" .. name)\n";
    s += "end\n";
    s += "\n";
    // Rough duration: the mouth sequence is 2.5s, so this lasts until the next beat
    // after the deadline rather than cutting mid-clip. Say() uses the same seconds
    // it holds the caption for. Speak(0) puts back the last Face() that was not
    // speak, which is idle unless something else was asked for.
    s += "function " + g + ".Speak(seconds)\n";
    s += "\tseconds = tonumber(seconds)\n";
    s += "\tif seconds == nil or seconds <= 0 then\n";
    s += "\t\tif " + g + ".speakUntil > 0 or " + g + ".face == " + g + ".faces.speak then\n";
    s += "\t\t\t" + g + ".speakUntil = 0\n";
    s += "\t\t\t" + g + ".SetFaceCode(" + g + ".restFace)\n";
    s += "\t\t\t" + g + ".Log(\"BRIDGE speak ended\")\n";
    s += "\t\telse\n";
    s += "\t\t\t" + g + ".speakUntil = 0\n";
    s += "\t\tend\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    s += "\t" + g + ".SetFaceCode(" + g + ".faces.speak)\n";
    s += "\t" + g + ".speakUntil = " + g + ".ticks + math.max(1, math.ceil(seconds / 0.4))\n";
    s += "\t" + g + ".Log(\"BRIDGE speak \" .. tostring(seconds) .. \"s\")\n";
    s += "end\n";
    s += "\n";
    s += "function " + g + ".ExpireSpeak()\n";
    s += "\tif " + g + ".speakUntil > 0 and " + g + ".ticks >= " + g + ".speakUntil then\n";
    s += "\t\t" + g + ".Speak(0)\n";
    s += "\tend\n";
    s += "end\n";
    s += "\n";
    // The mouth that goes with the voice (app/src/lipsync.h). Every line the game ships has
    // a baked facial clip in every human's animation set, named by its stem; the app picks
    // one of about the spoken line's length in the speaker's actor's face and the mod
    // plays it here with ent:StartAnimation on a layer the dialogue does not use, at the
    // speed that fits (lipsync_probe.lua, 12 Oct 2026: a 10.5 s line's clip opened the
    // mouth through the words on layer 13; 12, the engine's own lipsync layer, took the
    // clip and moved nothing; speeds of 0.5 to 1.6 took; a clip left to end holds its last
    // frame until the layer is stopped, so every line ends in StopAnimation; and a clip
    // re-issued on its layer while still running is refused, which is why the app never
    // plans the same clip twice running).
    //
    // The key is h = h * 31 + byte over "<who> <words>", the words with whitespace runs made
    // one space and the ends trimmed, mod 524287 (2^19 - 1) -- a multiply-add rather than a
    // proper hash because this Lua has no bit operations, and that small a modulus because
    // the game's Lua keeps its numbers in 32-bit floats: mod 2^32 the two sides disagreed on
    // the first greeting (12 Oct 2026, lipsync_pass.lua, 1465712640 here against 842313093
    // in the app, which is the exact sum rounded to 24 bits of mantissa at every step).
    // Below 2^19 the step's product stays under 2^24 and is exact. The app's lip_key
    // (app/src/lipsync.cpp) is the same sum.
    s += "function " + g + ".LipKey(who, text)\n";
    s += "\tlocal flat = (string.gsub(tostring(text), \"%s+\", \" \"))\n";
    s += "\tflat = (string.gsub(flat, \"^ \", \"\"))\n";
    s += "\tflat = (string.gsub(flat, \" $\", \"\"))\n";
    s += "\tlocal s = tostring(who) .. \" \" .. flat\n";
    s += "\tlocal h = 0\n";
    s += "\tfor i = 1, #s do h = (h * 31 + string.byte(s, i)) % 524287 end\n";
    s += "\treturn h\n";
    s += "end\n";
    s += "\n";
    // Whose mouth: Henry's for his line, the other party's for anything else -- and not
    // the entity itself but its dialogue twin. For a conversation the game spawns an NPC
    // named DialogTwin_<name> on each party's spot (DialogTwin_Dude for Henry) and draws
    // that; the entity proper stands frozen underneath, its layers' times not moving, a
    // clip started on it going nowhere (lipsync_pass.lua, 12 Oct 2026: layer 13 "running"
    // at time 0.00 for four seconds, every joint still, while the twin's head moved). The
    // entity is the fallback, for a dialogue that has no twin.
    s += "function " + g + ".LipEntity(who)\n";
    s += "\tlocal base = " + g + ".npc\n";
    s += "\tif who == \"henry\" then base = " + g + ".Player()\n";
    s += "\telseif not " + g + ".active and " + g + ".voicedSoul ~= nil then\n";
    s += "\t\tbase = nil\n";
    s += "\t\tpcall(function() base = System.GetEntityByName(" + g + ".voicedSoul) end)\n";
    s += "\tend\n";
    s += "\tif base == nil then return nil, false end\n";
    s += "\tlocal name = nil\n";
    s += "\tpcall(function() name = EntityUtils.GetName(base) end)\n";
    s += "\tif name ~= nil then\n";
    s += "\t\tlocal twin = nil\n";
    s += "\t\tpcall(function() twin = System.GetEntityByName(\"DialogTwin_\" .. name) end)\n";
    s += "\t\tif twin ~= nil and twin.StartAnimation ~= nil then return twin, true end\n";
    s += "\tend\n";
    s += "\treturn base, false\n";
    s += "end\n";
    s += "\n";
    // A voiced line is on screen: look for its cues until the next line or the stop.
    s += "function " + g + ".LipArm(who, text)\n";
    s += "\tif System.LoadTextFile == nil then return end\n";
    // Outside a session the arming is said in the log, since nothing else of the mouth is
    // until a cue is found: a Talk whose lips never moved (14 Oct 2026) left no trace.
    s += "\tif not " + g + ".LipLive() then\n";
    s += "\t\tif not " + g + ".active then " + g + ".Log(\"BRIDGE lip not armed for \" .. tostring(who) .. \": no session and no voiced talk\") end\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    s += "\t" + g + ".LipStop(\"next line\")\n";
    s += "\t" + g + ".lipKey = " + g + ".LipKey(who, text)\n";
    s += "\t" + g + ".lipSeq = 1\n";
    s += "\t" + g + ".lipWho = who\n";
    s += "\t" + g + ".lipGen = " + g + ".lipGen + 1\n";
    s += "\tlocal gen = " + g + ".lipGen\n";
    s += "\tif not " + g + ".active then " + g + ".Log(string.format(\"BRIDGE lip armed %s key=%.0f in the voiced talk\", tostring(who), " + g + ".lipKey)) end\n";
    s += "\tlocal polls = 0\n";
    s += "\tlocal function tick()\n";
    s += "\t\tif gen ~= " + g + ".lipGen then return end\n";
    s += "\t\tif not " + g + ".LipLive() then " + g + ".Log(\"BRIDGE lip \" .. tostring(who) .. \" let go: no session and no voiced talk\") return end\n";
    s += "\t\tpolls = polls + 1\n";
    s += "\t\tif polls > " + g + ".lipPollMax then\n";
    s += "\t\t\t" + g + ".Log(string.format(\"BRIDGE lip %s gave up: no cue lip_%.0f_%d.txt in %d polls\", tostring(who), " + g + ".lipKey or -1, " + g + ".lipSeq, polls - 1))\n";
    s += "\t\t\t" + g + ".lipKey = nil; " + g + ".LipNext() return\n";
    s += "\t\tend\n";
    s += "\t\tlocal ok, err = pcall(" + g + ".LipPoll)\n";
    s += "\t\tif not ok then " + g + ".Log(\"BRIDGE lip poll raised \" .. tostring(err)) return end\n";
    s += "\t\tif " + g + ".lipKey == nil then return end\n";
    s += "\t\tpcall(function() Script.SetTimer(" + g + ".lipPollMillis, tick) end)\n";
    s += "\tend\n";
    s += "\tpcall(function() Script.SetTimer(" + g + ".lipPollMillis, tick) end)\n";
    s += "end\n";
    s += "\n";
    // One look for the next cue file of the current line. Found, it is applied and the
    // sequence moves on; a stop ends the looking.
    s += "function " + g + ".LipPoll()\n";
    s += "\tlocal key = " + g + ".lipKey\n";
    s += "\tif key == nil then return end\n";
    s += "\tlocal name = string.format(\"lip_%.0f_%d.txt\", key, " + g + ".lipSeq)\n";
    s += "\tlocal patterns = " + g + ".inboxes\n";
    s += "\tif " + g + ".inbox ~= nil then patterns = { " + g + ".inbox } end\n";
    s += "\tfor i = 1, #patterns do\n";
    s += "\t\tlocal path = (string.gsub(patterns[i], \"turn_#%.txt\", name))\n";
    s += "\t\tlocal ok, got = pcall(function() return System.LoadTextFile(path) end)\n";
    s += "\t\tif ok and type(got) == \"string\" and got ~= \"\" then\n";
    s += "\t\t\tlocal seq = " + g + ".lipSeq\n";
    s += "\t\t\t" + g + ".lipSeq = seq + 1\n";
    s += "\t\t\t" + g + ".LipApply(got, seq)\n";
    s += "\t\t\treturn\n";
    s += "\t\tend\n";
    s += "\tend\n";
    s += "end\n";
    s += "\n";
    // The cue: "lip <who> <key>" then "play <clip> <speed> <seconds>" lines, each clip
    // following the last after its seconds, and "rest <seconds>" where the speech pauses
    // (the layer stopped, so the lips close); under a "from <seconds>" line the same is a
    // revision, replacing the plan from that second of the line on; or "mouth <seconds>
    // <viseme>" lines, shapes to show at their second of the audio; or "stop". `seq` is
    // which cue of the line this is: the first comes as the sound starts, and is the
    // line's clock.
    s += "function " + g + ".LipApply(body, seq)\n";
    s += "\tlocal plan = {}\n";
    s += "\tlocal mouth = {}\n";
    s += "\tlocal stop = false\n";
    s += "\tlocal from = 0\n";
    s += "\tfor line in string.gmatch(body, \"[^\\r\\n]+\") do\n";
    s += "\t\tlocal clip, speed, seconds = string.match(line, \"^play%s+(%S+)%s+(%S+)%s+(%S+)\")\n";
    s += "\t\tlocal rest = string.match(line, \"^rest%s+(%S+)\")\n";
    s += "\t\tlocal start = string.match(line, \"^from%s+(%S+)\")\n";
    s += "\t\tlocal at, shape = string.match(line, \"^mouth%s+(%S+)%s+(%S+)\")\n";
    s += "\t\tlocal shut = string.match(line, \"^shut%s+(%S+)\")\n";
    s += "\t\tif clip ~= nil then\n";
    s += "\t\t\tplan[#plan + 1] = { clip = clip, speed = tonumber(speed) or 1, seconds = tonumber(seconds) or 0 }\n";
    s += "\t\telseif shut ~= nil then\n";
    s += "\t\t\t" + g + ".lipShut = shut\n";
    s += "\t\telseif rest ~= nil then\n";
    s += "\t\t\tplan[#plan + 1] = { rest = true, seconds = tonumber(rest) or 0 }\n";
    s += "\t\telseif start ~= nil then\n";
    s += "\t\t\tfrom = tonumber(start) or 0\n";
    s += "\t\telseif at ~= nil then\n";
    s += "\t\t\tmouth[#mouth + 1] = { at = tonumber(at) or 0, shape = shape }\n";
    s += "\t\telseif string.match(line, \"^stop\") then\n";
    s += "\t\t\tstop = true\n";
    s += "\t\tend\n";
    s += "\tend\n";
    s += "\tif seq == 1 then " + g + ".lipT0 = " + g + ".Now() end\n";
    // The audio has begun: a paged caption counts its pages from here.
    s += "\tif seq == 1 and " + g + ".lipWho == \"npc\" then pcall(" + g + ".PageAnchor) end\n";
    s += "\tif stop then\n";
    s += "\t\t" + g + ".LipStop(\"audio ended\")\n";
    s += "\t\t" + g + ".lipKey = nil\n";
    // The other party's line heard out: the menu that waited on it may come, and a paged
    // caption shows its last page.
    s += "\t\tif " + g + ".lipWho == \"npc\" then pcall(" + g + ".MenuRelease, \"audio ended\") end\n";
    s += "\t\tif " + g + ".lipWho == \"npc\" then pcall(" + g + ".PageLast) end\n";
    s += "\t\t" + g + ".LipNext()\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    s += "\tif #plan == 0 and #mouth == 0 then return end\n";
    s += "\tlocal ent, twin = " + g + ".LipEntity(" + g + ".lipWho)\n";
    s += "\tif ent == nil or ent.StartAnimation == nil then\n";
    s += "\t\t" + g + ".Log(\"BRIDGE lip no entity for \" .. tostring(" + g + ".lipWho))\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    // The first cue of the run retires the OpenMouth/Hmm stack: the app has a mouth now.
    s += "\tif not " + g + ".lipsync then\n";
    s += "\t\t" + g + ".lipsync = true\n";
    s += "\t\t" + g + ".Speak(0)\n";
    s += "\tend\n";
    s += "\tif " + g + ".lipOnEnt ~= ent then " + g + ".lipOn = nil end\n";
    s += "\t" + g + ".lipOnEnt = ent\n";
    s += "\t" + g + ".lipEnt = ent\n";
    s += "\tif #mouth > 0 then\n";
    s += "\t\t" + g + ".LipMouth(mouth, ent, twin)\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    // Shapes already showing: the plan is the clips' guess at the mouth, and the shapes know.
    s += "\tif #" + g + ".lipMouth > 0 then return end\n";
    s += "\tlocal gen = " + g + ".lipGen\n";
    s += "\tlocal layer = " + g + ".lipLayer\n";
    s += "\tlocal total, clips, rests = 0, 0, 0\n";
    s += "\tfor i = 1, #plan do\n";
    s += "\t\ttotal = total + plan[i].seconds\n";
    s += "\t\tif plan[i].rest then rests = rests + 1 else clips = clips + 1 end\n";
    s += "\tend\n";
    // Each entry in its turn: a clip started on the layer at its speed (blended from
    // whatever the layer held; one already there plays on), a rest the mouth shut -- the
    // face's shut clip blended in, or the layer stopped where the app named none -- the
    // next after its seconds less `past`, the part of it already over when a revision is
    // joined partway.
    s += "\tlocal function play(i, past)\n";
    s += "\t\tif gen ~= " + g + ".lipGen or plan ~= " + g + ".lipPlan or i > #plan then return end\n";
    s += "\t\tlocal p = plan[i]\n";
    s += "\t\t" + g + ".lipClip = i\n";
    s += "\t\tif p.rest then\n";
    s += "\t\t\t" + g + ".LipRest(ent)\n";
    s += "\t\telseif p.clip ~= " + g + ".lipOn then\n";
    s += "\t\t\tlocal ok, took = pcall(function() return ent:StartAnimation(0, p.clip, layer, " + g + ".lipBlend, p.speed, false) end)\n";
    s += "\t\t\tif not ok or took ~= true then\n";
    s += "\t\t\t\t" + g + ".Log(\"BRIDGE lip clip \" .. i .. \" refused: \" .. p.clip .. \" -> \" .. tostring(took))\n";
    s += "\t\t\telse\n";
    s += "\t\t\t\t" + g + ".lipOn = p.clip\n";
    s += "\t\t\tend\n";
    s += "\t\tend\n";
    s += "\t\tif i < #plan then\n";
    s += "\t\t\tlocal left = p.seconds - (past or 0)\n";
    s += "\t\t\tpcall(function() Script.SetTimer(math.max(50, math.floor(left * 1000)), function() play(i + 1) end) end)\n";
    s += "\t\tend\n";
    s += "\tend\n";
    // Taking the plan up at its second against the line's clock: whatever the layer held
    // -- the last line's shut clip, or the clip the plan before it was playing -- is
    // blended from, not stopped under (the stop was the snap). A plan whose second has
    // passed (the opening one, or a revision that came late) skips the entries already
    // over and joins the current one partway.
    s += "\tlocal function begin()\n";
    s += "\t\tif gen ~= " + g + ".lipGen or " + g + ".lipPending ~= plan then return end\n";
    s += "\t\t" + g + ".lipPending = nil\n";
    s += "\t\t" + g + ".lipPlan = plan\n";
    s += "\t\tlocal past = (" + g + ".Now() - (" + g + ".lipT0 or " + g + ".Now())) - from\n";
    s += "\t\tif past < 0 then past = 0 end\n";
    s += "\t\tlocal i = 1\n";
    s += "\t\twhile i < #plan and past >= plan[i].seconds do\n";
    s += "\t\t\tpast = past - plan[i].seconds\n";
    s += "\t\t\ti = i + 1\n";
    s += "\t\tend\n";
    s += "\t\tplay(i, past)\n";
    s += "\tend\n";
    // A revision for a second still to come waits for it, the plan before it playing on
    // until then; a newer revision in the meantime takes its place.
    s += "\t" + g + ".lipPending = plan\n";
    s += "\tlocal wait = from - (" + g + ".Now() - (" + g + ".lipT0 or " + g + ".Now()))\n";
    s += "\tif wait > 0.05 then\n";
    s += "\t\tpcall(function() Script.SetTimer(math.floor(wait * 1000), begin) end)\n";
    s += "\telse\n";
    s += "\t\tbegin()\n";
    s += "\tend\n";
    s += "\tlocal first = plan[1].rest and string.format(\"rest %.2fs\", plan[1].seconds) or string.format(\"%s x%.2f\", plan[1].clip, plan[1].speed)\n";
    s += "\tlocal start = from > 0 and string.format(\" from %.2fs\", from) or \"\"\n";
    s += "\tlocal shut = rests > 0 and string.format(\", %d rest%s\", rests, rests == 1 and \"\" or \"s\") or \"\"\n";
    s += "\t" + g + ".Log(string.format(\"BRIDGE lip %s%s %d clip%s%s over %.1fs on the %s: %s\", tostring(" + g + ".lipWho), start, clips,\n";
    s += "\t\tclips == 1 and \"\" or \"s\", shut, total, twin and \"twin\" or \"entity\", first))\n";
    s += "end\n";
    s += "\n";
    // The bank of shapes: MoonshineMouth.shapes[<viseme>] = { clips = { <the mod's own
    // clips of the shape alone, copies> }, clip = <a shipped facial clip>, at = <0..1 of
    // its length where its actor's mouth is in the shape> }, found over the shipped lines
    // (scripts/facial_corpus.py bank). Loaded once, from the data pak, as the alms table is.
    s += g + ".mouthLoaded = false\n";
    s += g + ".mouthMissing = false\n";
    s += "function " + g + ".MouthBank()\n";
    s += "\tif not " + g + ".mouthLoaded then\n";
    s += "\t\t" + g + ".mouthLoaded = true\n";
    s += "\t\tif rawget(_G, \"MoonshineMouth\") == nil then\n";
    s += "\t\t\tpcall(function() Script.LoadScript(\"Scripts/mods/moonshine_mouth.lua\") end)\n";
    s += "\t\tend\n";
    s += "\tend\n";
    s += "\tlocal t = rawget(_G, \"MoonshineMouth\")\n";
    s += "\tif type(t) ~= \"table\" or type(t.shapes) ~= \"table\" then return nil end\n";
    s += "\treturn t.shapes\n";
    s += "end\n";
    s += "\n";
    // Shapes for the line, from a mouth cue: queued by their second, and the ticker started
    // at the first. The plan's clips, if any were playing, give way here.
    s += "function " + g + ".LipMouth(mouth, ent, twin)\n";
    s += "\tlocal bank = " + g + ".MouthBank()\n";
    s += "\tif bank == nil then\n";
    s += "\t\tif not " + g + ".mouthMissing then " + g + ".Log(\"BRIDGE lip no bank of shapes (moonshine_mouth.lua)\") end\n";
    s += "\t\t" + g + ".mouthMissing = true\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    s += "\tlocal first = #" + g + ".lipMouth == 0\n";
    s += "\tfor i = 1, #mouth do " + g + ".lipMouth[#" + g + ".lipMouth + 1] = mouth[i] end\n";
    s += "\tif not first then return end\n";
    s += "\t" + g + ".lipPending = nil\n";
    s += "\tif " + g + ".lipPlan ~= nil then\n";
    s += "\t\t" + g + ".lipPlan = nil\n";
    s += "\t\t" + g + ".lipClip = 0\n";
    s += "\t\tpcall(function() ent:StopAnimation(0, " + g + ".lipLayer) end)\n";
    s += "\t\t" + g + ".lipOn = nil\n";
    s += "\tend\n";
    s += "\tif " + g + ".lipT0 == nil then " + g + ".lipT0 = " + g + ".Now() end\n";
    s += "\t" + g + ".Log(string.format(\"BRIDGE lip %s shapes from %.2fs on the %s, first %s at %.2fs\", tostring(" + g + ".lipWho),\n";
    s += "\t\tmouth[1].at, twin and \"twin\" or \"entity\", tostring(mouth[1].shape), " + g + ".Now() - " + g + ".lipT0))\n";
    s += "\t" + g + ".LipMouthTick(" + g + ".lipGen)\n";
    s += "end\n";
    s += "\n";
    // Every few frames while a line's shapes are up: whatever shape's second has come is
    // shown, the newest when several have (the timer runs about every 65 ms, and a stop
    // consonant may be shorter), and nothing when none has. Ends with the line (LipStop
    // moves lipGen on) or when the cues stopped being looked for.
    s += "function " + g + ".LipMouthTick(gen)\n";
    s += "\tif gen ~= " + g + ".lipGen or not " + g + ".LipLive() or " + g + ".lipKey == nil then return end\n";
    s += "\tlocal t = " + g + ".Now() - (" + g + ".lipT0 or " + g + ".Now()) + " + g + ".lipMouthLead\n";
    s += "\tlocal q = " + g + ".lipMouth\n";
    s += "\tlocal pick, blend = nil, nil\n";
    s += "\tlocal skipped = -1\n";
    // A shape is blended in over the time since the one before it (within bounds), and
    // the blend starts half its length early, so the mouth is moving between the shapes
    // for the whole of the speech and arrives at each about as its sound starts; a fixed
    // short blend made every shape a snap (13 Oct 2026, in play).
    s += "\twhile " + g + ".lipMouthAt <= #q do\n";
    s += "\t\tlocal i = " + g + ".lipMouthAt\n";
    s += "\t\tlocal b = " + g + ".LipBlend(q, i)\n";
    s += "\t\tif q[i].at - b * 0.5 > t then break end\n";
    s += "\t\tpick, blend = q[i], b\n";
    s += "\t\t" + g + ".lipMouthAt = i + 1\n";
    s += "\t\tskipped = skipped + 1\n";
    s += "\tend\n";
    s += "\tif skipped > 0 then " + g + ".lipLate = " + g + ".lipLate + skipped end\n";
    s += "\tif pick ~= nil and pick.shape ~= " + g + ".lipShape then\n";
    s += "\t\t" + g + ".LipShape(pick.shape, blend)\n";
    s += "\telseif " + g + ".lipHoldLeft > 0 and " + g + ".lipEnt ~= nil then\n";
    // A borrowed clip's frame, asked for on the ticks after its start: SetAnimationTime
    // and SetAnimationSpeed act on the layer's oldest clip, so they only reach the new one
    // once the old has blended out, a tick or two on (lipsync_probe3.lua, 13 Oct 2026).
    s += "\t\t" + g + ".lipHoldLeft = " + g + ".lipHoldLeft - 1\n";
    s += "\t\tlocal ent, at = " + g + ".lipEnt, " + g + ".lipHoldAt\n";
    s += "\t\tpcall(function() ent:SetAnimationTime(0, " + g + ".lipLayer, at) end)\n";
    s += "\t\tpcall(function() ent:SetAnimationSpeed(0, " + g + ".lipLayer, " + g + ".lipMouthSpeed) end)\n";
    s += "\tend\n";
    s += "\tpcall(function() Script.SetTimer(" + g + ".lipMouthMillis, function() " + g + ".LipMouthTick(gen) end) end)\n";
    s += "end\n";
    s += "\n";
    // How long the i-th shape of the line blends in: the gap since the shape before it,
    // no shorter than lipMouthBlend (a stop consonant still closes the lips) and no longer
    // than lipMouthBlendMax (the first word after a pause does not ooze in).
    s += "function " + g + ".LipBlend(q, i)\n";
    s += "\tlocal gap = " + g + ".lipMouthBlend\n";
    s += "\tif i > 1 then gap = q[i].at - q[i - 1].at end\n";
    s += "\tif gap < " + g + ".lipMouthBlend then return " + g + ".lipMouthBlend end\n";
    s += "\tif gap > " + g + ".lipMouthBlendMax then return " + g + ".lipMouthBlendMax end\n";
    s += "\treturn gap\n";
    s += "end\n";
    s += "\n";
    // Whether the game loaded the mod's own shape clips (Animations/humans/facials/moonshine,
    // taken in by male.chrparams' wildcard), found out once from the first shape asked for:
    // a clip the animation set knows has a length.
    s += g + ".mouthOwn = nil\n";
    s += "function " + g + ".MouthOwn(ent, pose)\n";
    s += "\tif " + g + ".mouthOwn == nil and pose.clips ~= nil and pose.clips[1] ~= nil then\n";
    s += "\t\tlocal len = 0\n";
    s += "\t\tpcall(function() len = ent:GetAnimationLength(0, pose.clips[1]) or 0 end)\n";
    s += "\t\t" + g + ".mouthOwn = len > 0\n";
    s += "\t\t" + g + ".Log(string.format(\"BRIDGE lip own shape clips %s (%s %.2fs)\", len > 0 and \"loaded\" or \"not loaded, borrowing frames\", pose.clips[1], len))\n";
    s += "\tend\n";
    s += "\treturn " + g + ".mouthOwn == true and pose.clips ~= nil\n";
    s += "end\n";
    s += "\n";
    // One shape on the speaker's face. With the mod's own clips: the shape's clip (a few
    // frames of the mouth held in it) started on the mouth's layer at full speed, blended in
    // over `blend` (LipBlend: the gap since the last shape) so one flows into the next, and
    // left at its last frame; the
    // engine refuses a clip re-issued on a layer it is still on, so each shape has copies
    // taken in turn. Without them: a frame of a shipped facial clip, started at full speed
    // (a slow clip blends in slowly) and then held by LipMouthTick setting its time and
    // speed on the ticks after. A shape the bank lacks shows as AX, the neutral open mouth.
    s += "function " + g + ".LipShape(shape, blend)\n";
    s += "\tblend = blend or " + g + ".lipMouthBlend\n";
    s += "\tlocal ent = " + g + ".lipEnt\n";
    s += "\tlocal bank = " + g + ".MouthBank()\n";
    s += "\tif ent == nil or bank == nil then return end\n";
    s += "\tlocal pose = bank[shape] or bank.AX\n";
    s += "\tif pose == nil then return end\n";
    s += "\t" + g + ".lipShape = shape\n";
    s += "\tlocal layer = " + g + ".lipLayer\n";
    s += "\tlocal clip, own = pose.clip, " + g + ".MouthOwn(ent, pose)\n";
    s += "\tif own then\n";
    s += "\t\tpose.turn = (pose.turn or 0) % #pose.clips + 1\n";
    s += "\t\tclip = pose.clips[pose.turn]\n";
    s += "\tend\n";
    s += "\tif clip == nil then return end\n";
    s += "\tlocal ok, took = pcall(function() return ent:StartAnimation(0, clip, layer, blend, pose.speed or 1, false) end)\n";
    s += "\tif not ok or took ~= true then\n";
    s += "\t\t" + g + ".lipRefused = " + g + ".lipRefused + 1\n";
    s += "\t\tif " + g + ".lipRefused <= 3 then " + g + ".Log(\"BRIDGE lip shape \" .. shape .. \" refused: \" .. clip .. \" -> \" .. tostring(took)) end\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    s += "\t" + g + ".lipHoldLeft = 0\n";
    s += "\tif not own and pose.at ~= nil and ent.SetAnimationTime ~= nil then\n";
    s += "\t\t" + g + ".lipHoldAt = pose.at\n";
    s += "\t\t" + g + ".lipHoldLeft = 3\n";
    s += "\tend\n";
    s += "\t" + g + ".lipShapes = " + g + ".lipShapes + 1\n";
    s += "end\n";
    s += "\n";
    // The mouth shut: the face's shut clip blended in over lipRestBlend, so the lips close
    // as the voice pauses rather than the face snapping back; the layer stopped outright
    // where the app named no shut clip. Nothing when the shut clip is already on.
    s += "function " + g + ".LipRest(ent)\n";
    s += "\tlocal shut = " + g + ".lipShut\n";
    s += "\tif shut == nil then\n";
    s += "\t\tpcall(function() ent:StopAnimation(0, " + g + ".lipLayer) end)\n";
    s += "\t\t" + g + ".lipOn = nil\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    s += "\tif " + g + ".lipOn == shut then return end\n";
    s += "\tlocal ok, took = pcall(function() return ent:StartAnimation(0, shut, " + g + ".lipLayer, " + g + ".lipRestBlend, 1, false) end)\n";
    s += "\tif ok and took == true then\n";
    s += "\t\t" + g + ".lipOn = shut\n";
    s += "\telse\n";
    s += "\t\t" + g + ".Log(\"BRIDGE lip shut clip refused: \" .. tostring(shut) .. \" -> \" .. tostring(took) .. \", the layer stopped instead\")\n";
    s += "\t\tpcall(function() ent:StopAnimation(0, " + g + ".lipLayer) end)\n";
    s += "\t\t" + g + ".lipOn = nil\n";
    s += "\tend\n";
    s += "end\n";
    s += "\n";
    // The mouth let go: the lips blended shut, and the layer stopped lipLetGoMillis on if
    // no line has taken it up by then (a next line's LipArm moves lipGen, and its clips
    // blend from the shut mouth), so the face is the engine's again without the snap. No
    // queued clip or shape starts. From the stop cue, the next line, and the end of a
    // conversation.
    s += "function " + g + ".LipStop(why)\n";
    s += "\t" + g + ".lipGen = " + g + ".lipGen + 1\n";
    s += "\tlocal ent = " + g + ".lipEnt\n";
    s += "\t" + g + ".lipEnt = nil\n";
    s += "\t" + g + ".lipClip = 0\n";
    s += "\t" + g + ".lipPlan = nil\n";
    s += "\t" + g + ".lipPending = nil\n";
    s += "\tlocal shapes, late, refused = " + g + ".lipShapes, " + g + ".lipLate, " + g + ".lipRefused\n";
    s += "\t" + g + ".lipMouth = {}\n";
    s += "\t" + g + ".lipMouthAt = 1\n";
    s += "\t" + g + ".lipT0 = nil\n";
    s += "\t" + g + ".lipShape = nil\n";
    s += "\t" + g + ".lipHoldLeft = 0\n";
    s += "\t" + g + ".lipShapes = 0\n";
    s += "\t" + g + ".lipRefused = 0\n";
    s += "\t" + g + ".lipLate = 0\n";
    s += "\tif ent ~= nil then\n";
    s += "\t\tif " + g + ".lipShut ~= nil and " + g + ".lipOn ~= nil then\n";
    s += "\t\t\t" + g + ".LipRest(ent)\n";
    s += "\t\t\tlocal gen = " + g + ".lipGen\n";
    s += "\t\t\tpcall(function() Script.SetTimer(" + g + ".lipLetGoMillis, function()\n";
    s += "\t\t\t\tif gen ~= " + g + ".lipGen or " + g + ".lipEnt ~= nil then return end\n";
    s += "\t\t\t\tpcall(function() ent:StopAnimation(0, " + g + ".lipLayer) end)\n";
    s += "\t\t\t\tif " + g + ".lipOnEnt == ent then " + g + ".lipOn = nil end\n";
    s += "\t\t\t\t" + g + ".Log(\"BRIDGE lip layer let go\")\n";
    s += "\t\t\tend) end)\n";
    s += "\t\telse\n";
    s += "\t\t\tpcall(function() ent:StopAnimation(0, " + g + ".lipLayer) end)\n";
    s += "\t\t\t" + g + ".lipOn = nil\n";
    s += "\t\tend\n";
    s += "\t\t" + g + ".lipShut = nil\n";
    s += "\t\tlocal said = \"BRIDGE lip stopped -- \" .. tostring(why)\n";
    s += "\t\tif shapes > 0 or late > 0 or refused > 0 then\n";
    s += "\t\t\tsaid = said .. string.format(\" after %d shapes, %d skipped, %d refused\", shapes, late, refused)\n";
    s += "\t\tend\n";
    s += "\t\t" + g + ".Log(said)\n";
    s += "\tend\n";
    s += "end\n";
    s += "\n";
    // The app's word that the voice is done with a line, run as a hook: 'henry' for his
    // line, 'npc' for the closing one. Each lets go of what was waiting on it -- the reply
    // held behind Henry's line, with the camera back on the other party; the close that
    // EndWhenRead is polling for -- and nothing else, so a word arriving late, after the
    // hold it was for has run out or the conversation has moved on, does nothing.
    s += "function " + g + ".Spoken(who)\n";
    s += "\tif not " + g + ".active then return end\n";
    s += "\tif who == \"henry\" then\n";
    s += "\t\t" + g + ".HenryDone(\"spoken\")\n";
    s += "\telseif who == \"npc\" then\n";
    s += "\t\tif " + g + ".ending then " + g + ".spoken = true end\n";
    s += "\telse\n";
    s += "\t\t" + g + ".Log(\"BRIDGE spoken by nobody known: \" .. tostring(who))\n";
    s += "\tend\n";
    s += "end\n";
    s += "\n";
    // The end of Henry's hold (Said): the reply may land now, and the camera goes back to
    // whatever face was last asked for. The hold is cut to this tick rather than cleared,
    // since zero is also what a reply that is all directives leaves it at.
    s += "function " + g + ".HenryDone(why)\n";
    s += "\tif " + g + ".henryUntil == 0 then return end\n";
    s += "\tlocal early = (" + g + ".henryUntil - " + g + ".spokenGraceTicks - " + g + ".ticks) * 0.4\n";
    s += "\t" + g + ".henryUntil = 0\n";
    s += "\tif " + g + ".hold > " + g + ".ticks then " + g + ".hold = " + g + ".ticks end\n";
    s += "\tif " + g + ".face == " + g + ".henryCode then " + g + ".SetFaceCode(" + g + ".restFace) end\n";
    s += "\t" + g + ".Log(\"BRIDGE henry heard: \" .. why .. \" \" .. string.format(\"%.1f\", -early) .. \"s past the reading\")\n";
    s += "end\n";
    s += "\n";
    // Every tick: an app that said it would speak and then did not is not waited on past
    // the reading and the grace.
    s += "function " + g + ".HenryExpire()\n";
    s += "\tif " + g + ".henryUntil > 0 and " + g + ".ticks >= " + g + ".henryUntil then\n";
    s += "\t\t" + g + ".HenryDone(\"grace ran out\")\n";
    s += "\tend\n";
    s += "end\n";
    s += "\n";
    // Asks for the menu on the next boundary. A speak stretch still running is ended
    // first, since its expiry would otherwise write the rest face over this code before
    // the graph had read it. The graph reads the code at the end of the current beat,
    // so the menu is up within one beat of the call, and the Beat() of that boundary is
    // the last one until the player picks.
    s += "function " + g + ".Choose()\n";
    s += "\tif not " + g + ".active or " + g + ".choosing then return end\n";
    s += "\t" + g + ".speakUntil = 0\n";
    s += "\t" + g + ".choosing = true\n";
    s += "\t" + g + ".options = {}\n";
    s += "\t" + g + ".menuWait = true\n";
    s += "\t" + g + ".menuUp = false\n";
    s += "\t" + g + ".menuDrawn = 0\n";
    s += "\t" + g + ".menuLeaveRow = " + g + ".menuRows\n";
    s += "\t" + g + ".menuSayRow = " + g + ".menuRows - 1\n";
    s += "\t" + g + ".optionsTotal = nil\n";
    s += "\t" + g + ".menuClosed = false\n";
    s += "\t" + g + ".menuToken = " + g + ".menuToken + 1\n";
    // A new round starts with nothing in the say row.
    s += "\t" + g + ".SayReset()\n";
    // The keyboard's exclusive map would take the arrows and Enter before the dialogue
    // saw them, and the caption's cursor would sit under a menu. Down, and not offered
    // again by the pump while the menu is the way to answer.
    s += "\t" + g + ".TypingClose()\n";
    s += "\t" + g + ".typingDismissed = true\n";
    // The code is not set until the rows are in, or a grace has passed: set at once, the
    // engine drew its dots a third of a second later and ours a second after that, and the
    // player read four "..." rows before the words came (noted 4 Oct 2026). The rows follow
    // the reply by a pump tick or two, so the wait is usually shorter than the engine's own.
    //
    // And not at all while the other party's voice is still going (MenuHold): the round
    // is set up -- the keyboard down, the rows collected as they land -- and the code
    // follows when the stop cue or the grace lets go (MenuRelease -> ChooseGo).
    s += "\tif " + g + ".MenuHold() then return end\n";
    s += "\t" + g + ".ChooseGo()\n";
    s += "end\n";
    s += "\n";
    s += "function " + g + ".ChooseGo()\n";
    s += "\t" + g + ".chooseWaited = 0\n";
    s += "\t" + g + ".ChooseEnter(" + g + ".menuToken)\n";
    s += "end\n";
    s += "\n";
    s += "function " + g + ".ChooseEnter(token)\n";
    s += "\tif token ~= " + g + ".menuToken or not " + g + ".choosing or not " + g + ".active then return end\n";
    s += "\tif " + g + ".OptionsComplete() or " + g + ".chooseWaited >= " + g + ".menuEnterGraceMillis then\n";
    s += "\t\t" + g + ".SetFaceCode(" + g + ".chooseCode)\n";
    s += "\t\t" + g + ".Log(\"BRIDGE choose\" .. (" + g + ".OptionsComplete() and \"\" or \" (rows still short)\"))\n";
    s += "\t\tpcall(function() Script.SetTimer(" + g + ".menuMissedMillis, function() " + g + ".MenuMissed(token) end) end)\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    s += "\t" + g + ".chooseWaited = " + g + ".chooseWaited + 100\n";
    s += "\tpcall(function() Script.SetTimer(100, function() " + g + ".ChooseEnter(token) end) end)\n";
    s += "end\n";
    s += "\n";
    // The boundary the code was set for has not come in a beat's length and more: the
    // graph is already in the menu, having read the code between a beat's exit script and
    // its decision (notes.md 25 Oct 2026: Christine's offer of the second errand had the
    // engine's dots up and no words, the Beat() before it having seen Henry's code and the
    // same pump tick then cutting his hold, letting the menu go and setting the code, all
    // before the decision looked). The engine's list is up, so it is drawn over now; a
    // boundary that does come after all draws again (Beat).
    s += "function " + g + ".MenuMissed(token)\n";
    s += "\tif token ~= " + g + ".menuToken or not " + g + ".choosing or not " + g + ".menuWait or not " + g +
         ".active then return end\n";
    s += "\tif " + g + ".face ~= " + g + ".chooseCode or " + g + ".questCode ~= 0 then return end\n";
    s += "\t" + g + ".Log(\"BRIDGE menu boundary missed, drawn over the engine's list\")\n";
    s += "\t" + g + ".MenuEntered()\n";
    s += "end\n";
    s += "\n";
    // The rows as they arrive from the app, 1-based, with the intent each was written to
    // carry. Kept across the draw so a pick can say what it said; cleared when the menu
    // is next asked for, since the next round's rows come after the next reply.
    s += "function " + g + ".Option(row, intent, text)\n";
    s += "\tif row == nil or row < 1 or row > " + g + ".menuOptionRows then\n";
    s += "\t\t" + g + ".Log(\"BRIDGE opt row out of range: \" .. tostring(row))\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    // Frozen once drawn: the highlight may be on it.
    s += "\tif " + g + ".menuDrawn > 0 and " + g + ".options[row] ~= nil then return end\n";
    s += "\t" + g + ".options[row] = { text = text, intent = intent }\n";
    s += "\t" + g + ".Log(\"BRIDGE opt \" .. row .. \" \" .. intent .. \" -- \" .. text)\n";
    s += "end\n";
    s += "\n";
    // Whole when the app has said how many rows there are and they are all in (a count of
    // none is whole at once), or, from an app that does not say, when every row is.
    s += "function " + g + ".OptionsComplete()\n";
    s += "\tlocal total = " + g + ".optionsTotal or " + g + ".menuOptionRows\n";
    s += "\tfor row = 1, total do\n";
    s += "\t\tif " + g + ".options[row] == nil then return false end\n";
    s += "\tend\n";
    s += "\treturn true\n";
    s += "end\n";
    s += "\n";
    // Called from Beat() at the boundary the menu sequence is entered on. The engine
    // draws its placeholder rows about a third of a second after that (its two responses
    // are a quarter second apart), and a draw of ours before it is thrown away, so ours
    // waits a full second and then goes on when the rows are all in or a grace has
    // passed -- once, since every draw replays the widget's fade-in.
    // A mark for the menu, stretched for a frame rate under 60 (menuCoverFrames). The frame
    // time is the engine's own figure for the last frame; one it cannot give, or a wild one
    // (a load's stall), is read as 60 fps and the mark stands as measured.
    s += "function " + g + ".MenuMillis(base)\n";
    s += "\tlocal sixty = 1000 / 60\n";
    s += "\tlocal frame = sixty\n";
    s += "\tpcall(function() frame = System.GetFrameTime() * 1000 end)\n";
    s += "\tif type(frame) ~= \"number\" or frame <= 0 or frame > 200 then frame = sixty end\n";
    s += "\treturn math.floor(base + math.max(0, frame - sixty) * " + g + ".menuCoverFrames)\n";
    s += "end\n";
    s += "\n";
    s += "function " + g + ".MenuEntered()\n";
    s += "\t" + g + ".menuWait = false\n";
    s += "\tlocal token = " + g + ".menuToken\n";
    s += "\tlocal delay = " + g + ".MenuMillis(" + g + ".OptionsComplete() and " + g + ".menuCoverMillis or " + g + ".menuDelayMillis)\n";
    s += "\tpcall(function() Script.SetTimer(delay, function()\n";
    s += "\t\tif token ~= " + g + ".menuToken or not " + g + ".choosing then return end\n";
    s += "\t\t" + g + ".menuUp = true\n";
    s += "\t\t" + g + ".MenuMaybeDraw()\n";
    s += "\t\tif " + g + ".menuDrawn == 0 then\n";
    s += "\t\t\tpcall(function() Script.SetTimer(" + g + ".menuGraceMillis, function()\n";
    s += "\t\t\t\tif token ~= " + g + ".menuToken or not " + g + ".choosing then return end\n";
    s += "\t\t\t\tif " + g + ".menuDrawn == 0 then " + g + ".MenuDraw() end\n";
    s += "\t\t\tend) end)\n";
    s += "\t\tend\n";
    s += "\tend) end)\n";
    s += "end\n";
    s += "\n";
    // Draws when there is a menu to draw over and something new to put on it: all the
    // rows before the first draw, or a row that came after it (one more fade, which is
    // better than a row that never shows).
    s += "function " + g + ".MenuMaybeDraw()\n";
    s += "\tif not " + g + ".choosing or not " + g + ".menuUp then return end\n";
    s += "\tlocal have = 0\n";
    s += "\tfor row = 1, " + g + ".menuOptionRows do\n";
    s += "\t\tif " + g + ".options[row] ~= nil then have = have + 1 end\n";
    s += "\tend\n";
    s += "\tif " + g + ".menuDrawn == 0 then\n";
    s += "\t\tif " + g + ".OptionsComplete() then " + g + ".MenuDraw() end\n";
    s += "\telseif have > " + g + ".menuDrawn then\n";
    s += "\t\t" + g + ".MenuDraw()\n";
    s += "\tend\n";
    s += "end\n";
    s += "\n";
    // The HUD's list: five values a row -- text, used, skill check name and difficulty,
    // value -- then SetDialogSequences with false for the left, where the design puts
    // it. A row with no words yet keeps the engine's dots and is dimmed, since picking
    // it says nothing; the last row is the way out.
    // A list shorter than the graph's rows is drawn short (noted 4 Oct 2026: a round of
    // three showed a dimmed "..." fourth, and a round of none four of them): the rows
    // after the last with words are left off and the way out drawn straight after it, so
    // the widget's entries stay the graph's responses by index -- entry n is still row n's
    // exit script -- and a pick on the way out, wherever it sits, is `menuLeaveRow`. A row
    // with no words before the last that has them keeps its dots, which is rare: the app
    // numbers its rows in order.
    //
    // After the last row with words come two of the mod's own: the say row, "(Say
    // something else)", and the way out. Unless the app closed the list (@rows <n> closed,
    // a dictated letter's part): then its rows are the whole menu, and the say row and
    // the way out are not drawn -- their rows are 0, which no pick is -- so the choice
    // has to be one of the ways written for the part (notes.md 20 Oct 2026). A closed
    // list with no rows at all is drawn open, since a menu with nothing on it needs its
    // way out.
    s += "function " + g + ".MenuDraw()\n";
    s += "\tif not " + g + ".choosing then return end\n";
    s += "\tlocal values = {}\n";
    s += "\tlocal have = 0\n";
    s += "\tlocal empty = {}\n";
    s += "\tlocal last = 0\n";
    s += "\tfor row = 1, " + g + ".menuOptionRows do\n";
    s += "\t\tif " + g + ".options[row] ~= nil then last = row end\n";
    s += "\tend\n";
    s += "\tlocal closed = " + g + ".menuClosed and last > 0\n";
    s += "\tlocal extra = closed and 0 or 2\n";
    s += "\t" + g + ".menuSayRow = closed and 0 or last + 1\n";
    s += "\t" + g + ".menuLeaveRow = closed and 0 or last + 2\n";
    s += "\tfor row = 1, last + extra do\n";
    s += "\t\tlocal text = " + g + ".menuLeave\n";
    s += "\t\tif row <= last then\n";
    s += "\t\t\tlocal opt = " + g + ".options[row]\n";
    s += "\t\t\tif opt ~= nil then text = opt.text; have = have + 1 else text = \"\"; empty[#empty + 1] = row end\n";
    s += "\t\telseif row == last + 1 then\n";
    s += "\t\t\ttext = " + g + ".menuSay\n";
    s += "\t\tend\n";
    s += "\t\tvalues[#values + 1] = text\n";
    s += "\t\tvalues[#values + 1] = false\n";
    s += "\t\tvalues[#values + 1] = \"\"\n";
    s += "\t\tvalues[#values + 1] = 0\n";
    s += "\t\tvalues[#values + 1] = 0\n";
    s += "\tend\n";
    s += "\t" + g + ".menuDrawn = math.max(have, 1)\n";
    s += "\tlocal ok, err = pcall(function()\n";
    s += "\t\tfor _, id in ipairs(" + g + ".hudIds) do\n";
    s += "\t\t\tUIAction.SetArray(\"hud\", id, \"SequenceList\", values)\n";
    s += "\t\t\tUIAction.CallFunction(\"hud\", id, \"SetDialogSequences\", false)\n";
    s += "\t\t\tfor _, row in ipairs(empty) do\n";
    s += "\t\t\t\tUIAction.CallFunction(\"hud\", id, \"SetEnableSequenceChanged\", row - 1, false)\n";
    s += "\t\t\tend\n";
    s += "\t\tend\n";
    s += "\tend)\n";
    s += "\tif ok then\n";
    s += "\t\t" + g + ".Log(\"BRIDGE menu drawn rows=\" .. have .. \" empty=\" .. #empty .. \" say=\" .. " + g +
         ".menuSayRow .. \" leave=\" .. " + g + ".menuLeaveRow .. (closed and \" closed\" or \"\"))\n";
    s += "\telse\n";
    s += "\t\t" + g + ".Log(\"BRIDGE menu draw raised \" .. tostring(err))\n";
    s += "\tend\n";
    s += "end\n";
    s += "\n";
    // The exit script of the menu row the player confirmed, 1-based. The last row is the
    // way out and the graph is already ending the session when this runs; every other
    // row sends the graph back to the top, where it reads the face code again, so the
    // rest face goes back before that happens. A row with words is a line Henry said:
    // it goes down the log twice, as the pick with its intent for the app that knows
    // menus and as a said line for the one that does not, and into the caption and the
    // store exactly as a typed line does.
    //
    // The say row resolves like any other -- the menu goes, the rest face comes back --
    // and opens the keyboard in its place (SayOpen; Say-mode below). Its pick is logged
    // with the intent `say` and no words, so the app knows to take the said line that
    // follows as Henry's own rather than a row's.
    s += "function " + g + ".Picked(row)\n";
    s += "\tlocal opt = " + g + ".options[row]\n";
    // The way out is the last row of the graph, or wherever the short list drew it.
    s += "\tlocal leaving = row == " + g + ".menuRows or (opt == nil and row == " + g + ".menuLeaveRow)\n";
    s += "\tlocal saying = not leaving and opt == nil and row == " + g + ".menuSayRow\n";
    s += "\t" + g + ".choosing = false\n";
    s += "\t" + g + ".menuUp = false\n";
    s += "\t" + g + ".menuWait = false\n";
    s += "\t" + g + ".menuToken = " + g + ".menuToken + 1\n";
    s += "\tif leaving then\n";
    s += "\t\t" + g + ".Log(\"BRIDGE picked \" .. row .. \" leave -- \" .. " + g + ".menuLeave)\n";
    s += "\t\t" + g + ".Leave(\"ended\")\n";
    s += "\t\t" + g + ".OnClosed()\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    s += "\tif " + g + ".face == " + g + ".chooseCode then " + g + ".SetFaceCode(" + g + ".restFace) end\n";
    s += "\tif saying then\n";
    s += "\t\t" + g + ".Log(\"BRIDGE picked \" .. row .. \" say -- \")\n";
    s += "\t\t" + g + ".SayOpen()\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    s += "\tif opt == nil then\n";
    s += "\t\t" + g + ".Log(\"BRIDGE picked \" .. tostring(row) .. \" none -- \")\n";
    s += "\t\t" + g + ".Said(\"...\")\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    s += "\t" + g + ".Log(\"BRIDGE picked \" .. row .. \" \" .. opt.intent .. \" -- \" .. opt.text)\n";
    s += "\t" + g + ".Said(opt.text)\n";
    s += "end\n";
    s += "\n";
    // ---- Say-mode: a line of Henry's own, from the menu -------------------------------
    //
    // "(Say something else)" sits above the way out on every list. Confirmed, the decision
    // resolves as any row's does, and in the menu's place the mod's keyboard opens (the
    // same one F9 opens, with its exclusive map and its "Henry: _" caption), the app is
    // told to listen (BRIDGE say start) and sends the running transcript back as hooks
    // (SayHeard), which replace the line as the keys append to it. Enter, the pad's A or
    // the mouse button sends the line (SaySend); escape or B clears it, and with nothing
    // in it brings the menu back with the rows it had (SayBack -> ChooseAgain). F9 is
    // the same way back.
    //
    // Why not the row itself as the slot, the words replacing "(Say something else)"
    // under the menu as they are typed: that was built and measured first (14 Oct 2026).
    // The menu can be presented again after a pick with the choose code left up, and the
    // row redrawn per key, but only the engine resolves a decision, and it does so only
    // from its own `dialog` map -- a `dialog_confirm` fired from a map of the mod's own
    // reaches OnAction and resolves nothing. The keys for typing need an exclusive map
    // (under the dialog map, E confirms, W and S move the cursor, space skips and
    // backspace opens the pause menu), and an exclusive map silences the dialog map's
    // Enter and A with the rest. So there was no way to send. The menu-less keyboard is
    // the design the engine allows.
    s += "function " + g + ".SayReset()\n";
    s += "\t" + g + ".saying = false\n";
    s += "end\n";
    s += "\n";
    // The say row picked: the keyboard, in say-mode. The pick has already resolved the
    // menu and put the rest face back; the camera is the conversation's.
    s += "function " + g + ".SayOpen()\n";
    s += "\tif not " + g + ".active then return end\n";
    s += "\t" + g + ".saying = true\n";
    s += "\t" + g + ".typingDismissed = false\n";
    s += "\t" + g + ".Log(\"BRIDGE say start\")\n";
    s += "\t" + g + ".SetTyping(true)\n";
    s += "end\n";
    s += "\n";
    // Say-mode over, and the app told to stop listening. The keyboard is the caller's to
    // close or keep: Said closes it, SayLeave closes it, a conversation ending wipes it.
    s += "function " + g + ".SayStop(why)\n";
    s += "\tif not " + g + ".saying then return end\n";
    s += "\t" + g + ".saying = false\n";
    s += "\t" + g + ".Log(\"BRIDGE say stop -- \" .. why)\n";
    s += "end\n";
    s += "\n";
    // Enter, A or the mouse button: the line goes as a typed one does. Nothing in it,
    // nothing happens -- an empty line is not a thing to say.
    s += "function " + g + ".SaySend()\n";
    s += "\tif not " + g + ".saying then return end\n";
    s += "\tlocal line = " + g + ".typed\n";
    s += "\tif line == \"\" then\n";
    s += "\t\t" + g + ".Log(\"BRIDGE say enter on an empty line\")\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    s += "\t" + g + ".SayStop(\"sent\")\n";
    s += "\t" + g + ".Said(line)\n";
    s += "end\n";
    s += "\n";
    // Escape or B: the line cleared, or with nothing to clear the menu brought back.
    s += "function " + g + ".SayBack()\n";
    s += "\tif not " + g + ".saying then return end\n";
    s += "\tif " + g + ".typed ~= \"\" then\n";
    s += "\t\t" + g + ".typed = \"\"\n";
    s += "\t\t" + g + ".cursorOn = true\n";
    s += "\t\t" + g + ".Log(\"BRIDGE say cleared\")\n";
    s += "\t\t" + g + ".Refresh()\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    s += "\t" + g + ".SayLeave(\"back\")\n";
    s += "end\n";
    s += "\n";
    // Out of say-mode and back to the menu: the keyboard down, and the choose code up
    // again with the rows the round already has (ChooseAgain).
    s += "function " + g + ".SayLeave(why)\n";
    s += "\tif not " + g + ".saying then return end\n";
    s += "\t" + g + ".SayStop(why)\n";
    s += "\t" + g + ".SetTyping(false)\n";
    s += "\t" + g + ".typingDismissed = true\n";
    s += "\t" + g + ".ChooseAgain()\n";
    s += "end\n";
    s += "\n";
    // The app's running transcript of what the microphone hears while say-mode is on
    // (a hook, Moonshine.SayHeard("...")). It replaces the line, since each transcript is
    // the whole utterance so far, and the caption follows. Nothing is sent: the player
    // reads it and presses Enter, or clears it and tries again.
    s += "function " + g + ".SayHeard(text)\n";
    s += "\tif not " + g + ".saying or not " + g + ".typing or not " + g + ".active then return end\n";
    s += "\tif type(text) ~= \"string\" then return end\n";
    s += "\t" + g + ".typed = text\n";
    s += "\t" + g + ".cursorOn = true\n";
    s += "\t" + g + ".Log(\"BRIDGE say heard \" .. text)\n";
    s += "\t" + g + ".Refresh()\n";
    s += "end\n";
    s += "\n";
    // The menu asked for again with the rows the round already has: the say row picked
    // and then given up. Choose() would start a round and wait for rows that are not
    // coming again, so this is the smaller version of it -- the code up, the draw to
    // follow at the boundary the engine enters the menu on (MenuEntered), as before.
    s += "function " + g + ".ChooseAgain()\n";
    s += "\tif not " + g + ".active or " + g + ".choosing then return end\n";
    s += "\t" + g + ".speakUntil = 0\n";
    s += "\t" + g + ".choosing = true\n";
    s += "\t" + g + ".menuWait = true\n";
    s += "\t" + g + ".menuUp = false\n";
    s += "\t" + g + ".menuDrawn = 0\n";
    s += "\t" + g + ".menuToken = " + g + ".menuToken + 1\n";
    s += "\t" + g + ".SetFaceCode(" + g + ".chooseCode)\n";
    s += "\t" + g + ".Log(\"BRIDGE choose again\")\n";
    s += "end\n";
    s += "\n";
    // The quest side of a conversation. The app decides everything -- whether the
    // player agreed, whether the holder was talked round, whether the giver has been
    // paid -- and says so with "@quest <id> <command>" ahead of a turn's words. This
    // end only carries it out: inventories move here, because the graph has no way to
    // reach them that a mod's own project can name; the journal moves in the graph,
    // because a quest state is the one thing Lua cannot set.
    s += "function " + g + ".SetQuestCode(code)\n";
    s += "\t" + g + ".questCode = code\n";
    s += "\tlocal ok, err = pcall(function()\n";
    s += "\t\tVariables.SetGlobal(" + g + ".questVar, code)\n";
    s += "\tend)\n";
    s += "\tif not ok then " + g + ".Log(\"  SetGlobal quest raised \" .. tostring(err)) end\n";
    s += "end\n";
    s += "\n";
    // One code up at a time. The graph reads the global when it next Autoselects, which
    // is the end of the current beat, so a second code set before then would overwrite
    // the first unplayed; hence the queue.
    s += "function " + g + ".QuestArm()\n";
    s += "\tif " + g + ".questCode ~= 0 or #" + g + ".questQueue == 0 then return end\n";
    s += "\tlocal code = table.remove(" + g + ".questQueue, 1)\n";
    s += "\t" + g + ".questArmedAt = " + g + ".ticks\n";
    s += "\t" + g + ".SetQuestCode(code)\n";
    s += "\t" + g + ".Log(\"BRIDGE quest armed code=\" .. code)\n";
    s += "end\n";
    s += "\n";
    // Named in a quest sequence's ExitScript, so it runs once that beat has played and
    // its trigger has fired. Hands the code back, arms the next if any, and then does
    // what every beat does.
    s += "function " + g + ".QuestBeat(code)\n";
    s += "\t" + g + ".Log(\"BRIDGE quest beat code=\" .. tostring(code) .. \" played\")\n";
    s += "\tif " + g + ".questCode == code then " + g + ".SetQuestCode(0) end\n";
    s += "\t" + g + ".QuestArm()\n";
    s += "\t" + g + ".Beat()\n";
    s += "end\n";
    s += "\n";
    // The pump's side of the same bargain: a code that has been up for longer than a
    // few beats is one the graph is not going to play -- the session closed under it,
    // or the exit script never ran -- and holding it would hold every face too.
    s += "function " + g + ".QuestExpire()\n";
    s += "\tif " + g + ".questCode == 0 then return end\n";
    s += "\tif " + g + ".ticks - " + g + ".questArmedAt < " + g + ".questPatience then return end\n";
    s += "\t" + g + ".Log(\"BRIDGE quest code=\" .. " + g + ".questCode .. \" never played, dropped\")\n";
    s += "\t" + g + ".SetQuestCode(0)\n";
    s += "\t" + g + ".QuestArm()\n";
    s += "end\n";
    s += "\n";
    // Whoever is in the conversation: the other party as the mod knows them, and the
    // player entity, which is the user handed to OnConversation or the global.
    s += "function " + g + ".Player()\n";
    s += "\tlocal p = " + g + ".user\n";
    s += "\tif p == nil or p.inventory == nil then p = rawget(_G, \"player\") end\n";
    s += "\treturn p\n";
    s += "end\n";
    s += "\n";
    // How many of an item class someone carries, or -1 when it cannot be asked. No class
    // (a travel's) is none carried.
    s += "function " + g + ".Count(who, item)\n";
    s += "\tif item == nil or item == \"\" then return 0 end\n";
    s += "\tif who == nil or who.inventory == nil then return -1 end\n";
    s += "\tlocal ok, got = pcall(function() return who.inventory:GetCountOfClass(item) end)\n";
    s += "\tif ok and type(got) == \"number\" then return got end\n";
    s += "\treturn -1\n";
    s += "end\n";
    s += "\n";
    // Moves n of the item from one inventory to another, the way ItemUtils.GiveItem
    // does it, and says how many went. MoveItemOfClass takes the receiving inventory's
    // id, the class, the amount, and whether to say so on screen. MoveOne is the old
    // name for one.
    s += "function " + g + ".MoveN(from, to, item, n)\n";
    s += "\tif from == nil or to == nil or from.inventory == nil or to.inventory == nil then\n";
    s += "\t\treturn 0\n";
    s += "\tend\n";
    s += "\tlocal ok, got = pcall(function()\n";
    s += "\t\treturn from.inventory:MoveItemOfClass(to.inventory:GetId(), item, n, true)\n";
    s += "\tend)\n";
    s += "\tif not ok then\n";
    s += "\t\t" + g + ".Log(\"  MoveItemOfClass raised \" .. tostring(got))\n";
    s += "\t\treturn 0\n";
    s += "\tend\n";
    s += "\tif type(got) == \"number\" then return got end\n";
    s += "\treturn 0\n";
    s += "end\n";
    s += "function " + g + ".MoveOne(from, to, item)\n";
    s += "\treturn " + g + ".MoveN(from, to, item, 1)\n";
    s += "end\n";
    s += "\n";
    // The rows of an item class in someone's pocket, freshest first: { id, health, amount }
    // each, the way the game stacks them (one row per class and health; health on 0..1).
    // inventory:GetInventoryTable() lists the wuids and ItemManager.GetItem(wuid) is
    // { health, amount, id, class, entity } (scripts/autopilot/health_probe.lua, 6 Oct
    // 2026). nil when the pocket cannot be read that way.
    s += "function " + g + ".Rows(who, item)\n";
    s += "\tif who == nil or who.inventory == nil or item == nil or item == \"\" then return nil end\n";
    s += "\tlocal ok, list = pcall(function() return who.inventory:GetInventoryTable() end)\n";
    s += "\tif not ok or type(list) ~= \"table\" then return nil end\n";
    s += "\tlocal out = {}\n";
    s += "\tfor _, wuid in ipairs(list) do\n";
    s += "\t\tlocal got, it = pcall(function() return ItemManager.GetItem(wuid) end)\n";
    s += "\t\tif got and type(it) == \"table\" and it.class == item and (it.amount or 0) > 0 then\n";
    s += "\t\t\tout[#out + 1] = { id = wuid, health = tonumber(it.health) or 0, amount = it.amount }\n";
    s += "\t\tend\n";
    s += "\tend\n";
    s += "\ttable.sort(out, function(a, b) return a.health > b.health end)\n";
    s += "\treturn out\n";
    s += "end\n";
    s += "\n";
    // How many of the item at or above a freshness, for a find that wants them fresh
    // (quest.minHealth; Joan's marigolds from the meadow). No threshold is the plain
    // count, and so is a pocket that cannot be read by rows, rather than refusing a
    // hand-in over a reading the game would not give.
    s += "function " + g + ".CountFresh(who, item, min)\n";
    s += "\tif min == nil or min <= 0 then return " + g + ".Count(who, item) end\n";
    s += "\tlocal rows = " + g + ".Rows(who, item)\n";
    s += "\tif rows == nil then return " + g + ".Count(who, item) end\n";
    s += "\tlocal n = 0\n";
    s += "\tfor _, r in ipairs(rows) do if r.health >= min then n = n + r.amount end end\n";
    s += "\treturn n\n";
    s += "end\n";
    s += "\n";
    // Takes n of the item from one pocket to another, freshest first and none below the
    // threshold, and says how many went. The by-class movers (MoveItemOfClass,
    // DeleteItemOfClass) draw from the stalest row first, which is the opposite of what a
    // giver who asked for fresh ones should get; ItemManager.RemoveItem takes a whole row
    // (inventory:DeleteItem(wuid) answers and removes nothing), so a row bigger than the
    // need is removed and the rest made back at its own health. The receiver's are made
    // at the health they had.
    s += "function " + g + ".TakeFresh(from, to, item, min, n)\n";
    s += "\tlocal rows = " + g + ".Rows(from, item)\n";
    s += "\tif rows == nil then return 0 end\n";
    s += "\tlocal left = n\n";
    s += "\tfor _, r in ipairs(rows) do\n";
    s += "\t\tif left <= 0 then break end\n";
    s += "\t\tif r.health >= (min or 0) then\n";
    s += "\t\t\tlocal take = math.min(r.amount, left)\n";
    s += "\t\t\tlocal ok, err = pcall(function() ItemManager.RemoveItem(r.id) end)\n";
    s += "\t\t\tif not ok then\n";
    s += "\t\t\t\t" + g + ".Log(\"  RemoveItem raised \" .. tostring(err))\n";
    s += "\t\t\telse\n";
    s += "\t\t\t\tif r.amount > take then pcall(function() from.inventory:CreateItem(item, r.health, r.amount - take) end) end\n";
    s += "\t\t\t\tif to ~= nil and to.inventory ~= nil then pcall(function() to.inventory:CreateItem(item, r.health, take) end) end\n";
    s += "\t\t\t\tleft = left - take\n";
    s += "\t\t\tend\n";
    s += "\t\tend\n";
    s += "\tend\n";
    s += "\tif n - left > 0 then pcall(function() Game.ShowItemsTransfer(item, -(n - left)) end) end\n";
    s += "\treturn n - left\n";
    s += "end\n";
    s += "\n";
    // Alms (notes.md, 6 Oct 2026: beggars always accept food, clothing -- and wear it -- and
    // groschen; behaviors.md 6.4). The classes are the shipped tables' and ride as
    // Scripts/mods/moonshine_alms.lua in the Data pak (scripts/kcd2_alms_classes.py), a
    // table of food classes with prices and garments with price and group, loaded here
    // once; without it nothing is offered and nothing moves.
    //
    // AlmsHave reads Henry's pocket once at the begin line and says what he could give, as
    // letters: m for a purse of three groschen or more, f for any food, c for a spare
    // garment. The game has no bind that says what is worn, so a garment is spare only
    // when Henry carries two of a group (two tunics, two pairs of shoes) and the cheaper
    // of the pair goes -- a rule that never undresses him, at the price of saying nothing
    // when his one spare shirt is of a group he also wears. The app reads the letters off
    // the begin line (alms=mfc) and offers a row for each.
    s += g + ".almsLoaded = false\n";
    s += "function " + g + ".AlmsTable()\n";
    s += "\tif not " + g + ".almsLoaded then\n";
    s += "\t\t" + g + ".almsLoaded = true\n";
    s += "\t\tif rawget(_G, \"MoonshineAlms\") == nil then\n";
    s += "\t\t\tpcall(function() Script.LoadScript(\"Scripts/mods/moonshine_alms.lua\") end)\n";
    s += "\t\tend\n";
    s += "\tend\n";
    s += "\tlocal t = rawget(_G, \"MoonshineAlms\")\n";
    s += "\tif type(t) ~= \"table\" or type(t.food) ~= \"table\" or type(t.clothes) ~= \"table\" then return nil end\n";
    s += "\treturn t\n";
    s += "end\n";
    s += "\n";
    // Henry's rows that are food or garments: { class, price, amount, group } each.
    s += "function " + g + ".AlmsRows(who)\n";
    s += "\tlocal t = " + g + ".AlmsTable()\n";
    s += "\tif t == nil or who == nil or who.inventory == nil then return nil end\n";
    s += "\tlocal ok, list = pcall(function() return who.inventory:GetInventoryTable() end)\n";
    s += "\tif not ok or type(list) ~= \"table\" then return nil end\n";
    s += "\tlocal food, clothes = {}, {}\n";
    s += "\tfor _, wuid in ipairs(list) do\n";
    s += "\t\tlocal got, it = pcall(function() return ItemManager.GetItem(wuid) end)\n";
    s += "\t\tif got and type(it) == \"table\" and (it.amount or 0) > 0 then\n";
    s += "\t\t\tlocal price = t.food[it.class]\n";
    s += "\t\t\tif price ~= nil then food[#food + 1] = { class = it.class, price = price, amount = it.amount } end\n";
    s += "\t\t\tlocal g = t.clothes[it.class]\n";
    s += "\t\t\tif g ~= nil then clothes[#clothes + 1] = { class = it.class, price = g[1], amount = it.amount, group = g[2] } end\n";
    s += "\t\tend\n";
    s += "\tend\n";
    s += "\ttable.sort(food, function(a, b) return a.price < b.price end)\n";
    s += "\ttable.sort(clothes, function(a, b) return a.price < b.price end)\n";
    s += "\treturn food, clothes\n";
    s += "end\n";
    s += "\n";
    // The spare garment, if any: the cheapest row of a group Henry carries two of (two
    // rows, or one row of two).
    s += "function " + g + ".AlmsSpare(clothes)\n";
    s += "\tlocal seen = {}\n";
    s += "\tfor _, c in ipairs(clothes) do\n";
    s += "\t\tif c.amount >= 2 then return c end\n";
    s += "\t\tif seen[c.group] ~= nil then return seen[c.group] end\n";
    s += "\t\tseen[c.group] = c\n";
    s += "\tend\n";
    s += "\treturn nil\n";
    s += "end\n";
    s += "\n";
    s += g + ".almsGroschen = 3\n";
    s += "function " + g + ".AlmsHave(user)\n";
    s += "\tlocal p = user\n";
    s += "\tif p == nil or p.inventory == nil then p = " + g + ".Player() end\n";
    s += "\tlocal food, clothes = " + g + ".AlmsRows(p)\n";
    s += "\tif food == nil then return \"\" end\n";
    s += "\tlocal out = \"\"\n";
    s += "\tlocal ok, money = pcall(function() return p.inventory:GetMoney() end)\n";
    // inventory:GetMoney() reads in groschen, with a decimal (6380.7 on the Windows laptop's save).
    s += "\tif ok and type(money) == \"number\" and money >= " + g + ".almsGroschen then out = out .. \"m\" end\n";
    s += "\tif #food > 0 then out = out .. \"f\" end\n";
    s += "\tif " + g + ".AlmsSpare(clothes) ~= nil then out = out .. \"c\" end\n";
    s += "\treturn out\n";
    s += "end\n";
    s += "\n";
    // " alms=<letters>" for the begin line of a beggar; nothing for anyone else, and
    // nothing when the letters are empty, so the app offers no row it cannot keep.
    s += "function " + g + ".AlmsLine(npc, user)\n";
    s += "\tif npc == nil or not " + g + ".beggars[EntityUtils.GetName(npc)] then return \"\" end\n";
    s += "\tlocal have = " + g + ".AlmsHave(user)\n";
    s += "\tif have == \"\" then return \"\" end\n";
    s += "\treturn \" alms=\" .. have\n";
    s += "end\n";
    s += "\n";
    // @alms <money|food|clothes>: the app's row was picked, so the thing goes. Money by the
    // same transaction the holder's fee uses (deci-groschen, player to soul); food the
    // cheapest he carries, one of it; a garment the spare, moved and then put on him with
    // actor:EquipInventoryItem, the bind the game's own dressing uses (scripts/autopilot/
    // alms_probe.lua, 6 Oct 2026: moves and the equip call return without raising on a
    // sitting beggar). The log line is what the harness and the eye read.
    s += "function " + g + ".AlmsGive(kind)\n";
    s += "\tlocal npc, p = " + g + ".npc, " + g + ".Player()\n";
    s += "\tif npc == nil or p == nil then return end\n";
    s += "\tlocal name = EntityUtils.GetName(npc)\n";
    s += "\tif kind == \"money\" then\n";
    s += "\t\tlocal pence = " + g + ".almsGroschen * 10\n";
    s += "\t\tlocal ok, err = pcall(function() Shops.DoMoneyTransaction(npc.id, pence, 0) end)\n";
    s += "\t\t" + g + ".Log(\"BRIDGE alms money \" .. " + g + ".almsGroschen .. \" groschen to \" .. name .. (ok and \"\" or (\" raised \" .. tostring(err))))\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    s += "\tlocal food, clothes = " + g + ".AlmsRows(p)\n";
    s += "\tif food == nil then " + g + ".Log(\"BRIDGE alms \" .. kind .. \" nothing to give: no table\"); return end\n";
    s += "\tlocal pick = nil\n";
    s += "\tif kind == \"food\" then pick = food[1] end\n";
    s += "\tif kind == \"clothes\" then pick = " + g + ".AlmsSpare(clothes) end\n";
    s += "\tif pick == nil then " + g + ".Log(\"BRIDGE alms \" .. kind .. \" nothing to give\"); return end\n";
    s += "\tlocal moved = " + g + ".MoveOne(p, npc, pick.class)\n";
    s += "\tlocal worn = \"\"\n";
    s += "\tif moved > 0 and kind == \"clothes\" and npc.actor ~= nil and npc.actor.EquipInventoryItem ~= nil then\n";
    s += "\t\tlocal wuid = nil\n";
    s += "\t\tlocal rows = " + g + ".Rows(npc, pick.class)\n";
    s += "\t\tif rows ~= nil and rows[1] ~= nil then wuid = rows[1].id end\n";
    s += "\t\tif wuid ~= nil then\n";
    s += "\t\t\tlocal ok, err = pcall(function() npc.actor:EquipInventoryItem(wuid) end)\n";
    s += "\t\t\tworn = ok and \", worn\" or (\", equip raised \" .. tostring(err))\n";
    s += "\t\telse\n";
    s += "\t\t\tworn = \", no row to wear\"\n";
    s += "\t\tend\n";
    s += "\tend\n";
    s += "\t" + g + ".Log(\"BRIDGE alms \" .. kind .. \" \" .. pick.class .. \" moved \" .. moved .. \" to \" .. name .. worn)\n";
    s += "end\n";
    s += "\n";
    // The holder agreed, or was talked into it: the thing goes from their pockets to
    // Henry's. If they have not got it -- a save from before the default item reached
    // them, or one that was sold -- it is made rather than moved, since the agreement
    // has been reached and a promise with nothing behind it is worse than an axe from
    // nowhere. Game.ShowItemsTransfer is the game's own "received" notice. holder is
    // whoever has it: the soul in conversation when the app said so, the quest's target
    // by name when the game's own menu did.
    s += "function " + g + ".Handover(quest, holder)\n";
    s += "\tif holder == nil then holder = " + g + ".npc end\n";
    s += "\tlocal p = " + g + ".Player()\n";
    s += "\tlocal moved = " + g + ".MoveOne(holder, p, quest.item)\n";
    s += "\tif moved < 1 then\n";
    s += "\t\tlocal ok, err = pcall(function()\n";
    // inventory:CreateItem(class, health, amount), the shape player.lua uses; the
    // ItemManager.CreateItem + AddItem pair the utils use raised from mod Lua (money_probe).
    s += "\t\t\tp.inventory:CreateItem(quest.item, 1, 1)\n";
    s += "\t\tend)\n";
    s += "\t\tif ok then\n";
    s += "\t\t\tmoved = 1\n";
    s += "\t\t\tpcall(function() Game.ShowItemsTransfer(quest.item, 1) end)\n";
    s += "\t\t\t" + g + ".Log(\"BRIDGE quest handover made the item, the holder had none\")\n";
    s += "\t\telse\n";
    s += "\t\t\t" + g + ".Log(\"BRIDGE quest handover failed: \" .. tostring(err))\n";
    s += "\t\tend\n";
    s += "\telse\n";
    s += "\t\t" + g + ".Log(\"BRIDGE quest handover moved \" .. moved)\n";
    s += "\tend\n";
    s += "\tif " + g + ".Count(p, quest.item) < 1 and moved > 0 then\n";
    s += "\t\t" + g + ".Log(\"BRIDGE quest handover but the player still has none\")\n";
    s += "\tend\n";
    s += "end\n";
    s += "\n";
    // The giver is being paid off: the thing leaves Henry for them. Moved rather than
    // deleted so that it is where the story says it went, and deleted if the move is
    // refused, so that Henry cannot be paid twice for the one axe.
    // A travel's arrival, for the store. The graph's DistanceTrigger writes the journal on
    // its own, but it cannot call back into Lua, and the app reads the store; so the same
    // distance (watchMetres, the graph's constant) is measured here, every two seconds,
    // while any travel is accepted, unseen and not done, and not at all otherwise. Armed at
    // accept, when a world arrives (OnWorld, since a save can be loaded mid-errand) and
    // from the conversation pump; one loop at a time, and a loop that has not ticked for a
    // while is taken as wiped by a load and armed again. The tick is a field of the table,
    // not a closure, for the reason Tick gives.
    s += g + ".watchMetres = " + std::to_string(kTravelSeenMetres) + "\n";
    s += g + ".watching = false\n";
    s += g + ".watchAt = 0\n";
    // What there is to watch: a travel until seen, a beat until its man is down or dead,
    // a pray until knelt, a mind until its visits are made or its one is dead, an ask until
    // the truth is told or the one who knows is dead; all only while accepted and not done.
    s += "function " + g + ".WatchPending()\n";
    s += "\tlocal out = {}\n";
    s += "\tlocal quests = (" + g + ".state or {}).quests or {}\n";
    s += "\tfor id, q in pairs(" + g + ".quests) do\n";
    s += "\t\tlocal f = quests[id]\n";
    // Before anything is accepted: a giver with a hail written, until the errand is taken
    // up or over; a holder to dress, until he has been this session. Both ride the one
    // loop, so a town with a hail in it is watched from the world's arrival on.
    s += "\t\tlocal pre = (q.hail ~= nil and not (type(f) == \"table\" and (f.accepted or f.done)))\n";
    s += "\t\t\tor (q.dress ~= nil and " + g + ".dressed[id] == nil)\n";
    s += "\t\tif pre then out[#out + 1] = id end\n";
    s += "\t\tif type(f) == \"table\" and f.accepted and not f.done and not pre then\n";
    s += "\t\t\tif q.kind == \"travel\" and not f.seen then out[#out + 1] = id end\n";
    // A leave until its thing is down, and then until its cue has played; a return until done,
    // and past done while its curse is on Henry (the clock that lifts it runs here).
    s += "\t\t\tif q.kind == \"leave\" and q.spot ~= nil and (not f.left or " + g + ".cueDue[id] ~= nil) then out[#out + 1] = id end\n";
    s += "\t\t\tif q.kind == \"return\" and q.spot ~= nil then out[#out + 1] = id end\n";
    s += "\t\t\tif q.kind == \"beat\" and not f.beaten and not f.dead then out[#out + 1] = id end\n";
    s += "\t\t\tif q.kind == \"pray\" and not f.prayed and q.spot ~= nil then out[#out + 1] = id end\n";
    s += "\t\t\tif q.kind == \"mind\" and not f.dead and (f.checks or 0) < (q.count or 1) then out[#out + 1] = id end\n";
    s += "\t\t\tif q.kind == \"ask\" and not f.handed and not f.dead then out[#out + 1] = id end\n";
    // An ambush until the truth is told or the one who knows is dead (the trap in between
    // is this watch's to spring); a buy until the charm is handed over or the holder dead,
    // for the carry global and the death.
    s += "\t\t\tif q.kind == \"ambush\" and not f.handed and not f.dead then out[#out + 1] = id end\n";
    s += "\t\t\tif q.kind == \"buy\" and not f.handed and not f.dead then out[#out + 1] = id end\n";
    // A find with a keepsake: until it is laid, and then for Henry's pocket (the flag the
    // app reads for the giver's thanks).
    s += "\t\t\tif q.kind == \"find\" and q.keepsake ~= nil and q.spot ~= nil then out[#out + 1] = id end\n";
    // A retrieve whose thing lies in a chest: until the chest stands (WatchChest spawns it
    // when Henry comes near), and then until the thing has left it for Henry's pocket (the
    // taken token) or been handed in some other way.
    // An exposure's retrieve: until the holder is caught (WatchExpose) or the thing is handed
    // over some other way.
    s += "\t\t\tif q.kind == \"retrieve\" and not f.handed and ((q.chest ~= nil and not (f.chest and f.taken))\n";
    s += "\t\t\t\t\tor (q.seenWith ~= nil and not f.seen)) then out[#out + 1] = id end\n";
    s += "\t\telseif type(f) == \"table\" and f.done and q.kind == \"return\" and f.cursed and q.spot ~= nil then\n";
    s += "\t\t\tout[#out + 1] = id\n";
    s += "\t\tend\n";
    s += "\tend\n";
    s += "\treturn out\n";
    s += "end\n";
    s += "\n";
    // A world arriving puts the loop in doubt: the timers armed while a save streams in
    // are wiped when the loading ends (AutoHeartbeat), and the loop armed at OnInit, OnLoad
    // or OnReset is one of them -- a town with a hail in it was watched by nobody until
    // the player's first conversation re-armed it from the pump, which is why Benesh never
    // called Henry over before he was spoken to (notes.md 14 Oct 2026). In doubt, a loop
    // that has not ticked for two seconds is taken as wiped and armed again, from the
    // player's own keys (OnAction, the one call the engine makes in a loaded world
    // without fail); the first tick that lands clears the doubt. A second loop armed over
    // a live one dies at its first tick, half a second or less behind the other's.
    s += g + ".watchDoubt = false\n";
    s += g + ".watchTicked = nil\n";
    s += g + ".watchQuick = false\n";
    s += "function " + g + ".WatchArm(why)\n";
    s += "\tlocal now = " + g + ".Now()\n";
    s += "\tlocal stale = " + g + ".watchDoubt and 2 or 10\n";
    s += "\tif " + g + ".watching and now - " + g + ".watchAt < stale then return end\n";
    s += "\tlocal pending = " + g + ".WatchPending()\n";
    s += "\tif #pending == 0 then return end\n";
    s += "\t" + g + ".watching = true\n";
    s += "\t" + g + ".watchAt = now\n";
    s += "\t" + g + ".Log(\"BRIDGE watch armed (\" .. tostring(why) .. \") for \" .. table.concat(pending, \" \"))\n";
    s += "\tScript.SetTimer(2000, " + g + ".WatchTick)\n";
    s += "end\n";
    s += "\n";
    s += "function " + g + ".WatchTick()\n";
    s += "\tlocal now = " + g + ".Now()\n";
    s += "\tif " + g + ".watchTicked ~= nil and now - " + g + ".watchTicked < 0.5 and now >= " + g + ".watchTicked then return end\n";
    s += "\t" + g + ".watchTicked = now\n";
    s += "\t" + g + ".watchDoubt = false\n";
    s += "\t" + g + ".watchAt = now\n";
    s += "\tlocal pending = " + g + ".WatchPending()\n";
    s += "\tif #pending == 0 then\n";
    s += "\t\t" + g + ".watching = false\n";
    s += "\t\t" + g + ".Log(\"BRIDGE watch idle, nothing to look at\")\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    s += "\tlocal pp = nil\n";
    s += "\tpcall(function() pp = " + g + ".Player():GetWorldPos() end)\n";
    s += "\t" + g + ".watchQuick = false\n";
    s += "\tif pp ~= nil then\n";
    s += "\t\tfor _, id in ipairs(pending) do\n";
    s += "\t\t\tlocal q = " + g + ".quests[id]\n";
    s += "\t\t\tif q.hail ~= nil then pcall(" + g + ".WatchHail, id, q, pp) end\n";
    s += "\t\t\tif q.dress ~= nil then pcall(" + g + ".WatchDress, id, q, pp) end\n";
    s += "\t\t\tif q.kind == \"travel\" then " + g + ".WatchTravel(id, q, pp)\n";
    s += "\t\t\telseif q.kind == \"beat\" then " + g + ".WatchBeat(id, q)\n";
    s += "\t\t\telseif q.kind == \"pray\" then " + g + ".WatchPray(id, q, pp)\n";
    s += "\t\t\telseif q.kind == \"leave\" or q.kind == \"return\" then " + g + ".WatchSpot(id, q, pp)\n";
    s += "\t\t\telseif q.kind == \"mind\" then " + g + ".WatchMind(id, q)\n";
    s += "\t\t\telseif q.kind == \"ask\" then " + g + ".WatchAsk(id, q)\n";
    s += "\t\t\telseif q.kind == \"ambush\" then " + g + ".WatchAmbush(id, q, pp)\n";
    s += "\t\t\telseif q.kind == \"buy\" then " + g + ".WatchBuy(id, q)\n";
    s += "\t\t\telseif q.kind == \"find\" then " + g + ".WatchKeepsake(id, q, pp)\n";
    s += "\t\t\telseif q.kind == \"retrieve\" then\n";
    s += "\t\t\t\tif q.chest ~= nil then " + g + ".WatchChest(id, q, pp) end\n";
    s += "\t\t\t\tif q.seenWith ~= nil then " + g + ".WatchExpose(id, q, pp) end\n";
    s += "\t\t\tend\n";
    s += "\t\tend\n";
    s += "\tend\n";
    s += "\tScript.SetTimer(" + g + ".watchQuick and " + std::to_string(kWatchQuickMs) + " or 2000, " + g +
         ".WatchTick)\n";
    s += "end\n";
    s += "\n";
    // A giver's hail (notes.md 13 Oct 2026, In the Stocks): Benesh should not wait to be
    // spoken to but shout for Henry to come over. While the errand is unasked and no
    // conversation is open, the giver's distance to Henry is measured with the rest; within
    // hailMetres his line is said -- the app voices it in his stand-in's voice from the
    // `BRIDGE voice` line, as it does any line, a token of the hail's class goes in Henry's
    // pocket for a moment, which the graph's trigger turns into a RequestMonologue of
    // q_<id>_hail.xml on him: the game's own bark channel, which turns him to Henry and
    // holds him as speaking for the line's ReferenceLength. It does not caption the line:
    // a bark's words are drawn for as long as its voice clip plays, and a line the mod
    // writes has no clip the game will find (hail_probe2/3, 14 Oct 2026, twelve runs: the
    // monologue ran each time, IsInDialog rising within a frame of the token, and no words
    // came, with or without a silent ogg under every folder the shipped voice paks use and
    // with a shipped line's StringName in place of ours), so the narrative subtitle keeps
    // the words (HailShow), put up hailWordsMs after the token rather than with it: the
    // monologue's opening wipes whatever the narrative line held (hail_run28, 14 Oct 2026:
    // one put, the words up until the token landed and gone in the same burst), while a
    // put made once it is open draws and stays until it closes (runs 18, 20, 21: the words
    // in every shot while IsInDialog held, none after). Then not again until Henry has
    // gone off (twice hailMetres) and hailAgain seconds have passed, so a player who
    // stands about is hailed once, not every two seconds. Stopping what he is doing is not
    // here: the game's behaviour tree owns the body, and the probes
    // have no way to interrupt it that survives a frame. A load wipes `hailed`, which costs
    // one repeat at most.
    s += g + ".hailMetres = " + std::to_string(kHailMetres) + "\n";
    s += g + ".hailAgain = " + std::to_string(kHailAgainSeconds) + "\n";
    s += g + ".hailSeconds = " + std::to_string(kHailSeconds) + "\n";
    s += g + ".hailWordsMs = " + std::to_string(kHailWordsMs) + "\n";
    s += g + ".hailed = {}\n";
    s += g + ".hailGen = 0\n";
    s += "function " + g + ".WatchHail(id, q, pp)\n";
    s += "\tlocal f = ((" + g + ".state or {}).quests or {})[id]\n";
    s += "\tif type(f) == \"table\" and (f.accepted or f.done) then return end\n";
    s += "\tif " + g + ".active or " + g + ".InDialog() then return end\n";
    // Her day (hail_hours; notes.md 25 Oct 2026): asleep or at her supper she does not call
    // Henry over, and her marker is off the map (the graph's giverDay).
    s += "\tif q.hailHours ~= nil and not " + g + ".HourIn(q.hailHours) then return end\n";
    s += "\tlocal gp = nil\n";
    s += "\tpcall(function()\n";
    s += "\t\tlocal e = System.GetEntityByName(q.giver)\n";
    s += "\t\tif e == nil or (e.actor ~= nil and e.actor:IsDead()) then return end\n";
    s += "\t\tgp = e:GetWorldPos()\n";
    s += "\tend)\n";
    s += "\tif gp == nil then return end\n";
    s += "\tlocal d = math.sqrt((pp.x - gp.x) ^ 2 + (pp.y - gp.y) ^ 2)\n";
    s += "\tlocal was = " + g + ".hailed[id]\n";
    s += "\tif d > " + g + ".hailMetres then\n";
    s += "\t\tif was ~= nil and d > " + g + ".hailMetres * 2 then was.away = true end\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    s += "\tif was ~= nil and (not was.away or " + g + ".Now() - was.at < " + g + ".hailAgain) then return end\n";
    s += "\t" + g + ".hailed[id] = { at = " + g + ".Now(), away = false }\n";
    s += "\t" + g + ".Log(\"BRIDGE quest \" .. id .. \" hail \" .. q.giver .. \" at \" .. string.format(\"%.1f\", d) .. \"m\")\n";
    // The giver named as the third argument, so the shout's mouth is his (LipEntity): without
    // it the mouth was armed and found no entity (14 Oct 2026).
    s += "\t" + g + ".Voice(q.hailVoice or \"male\", q.hail, q.giver)\n";
    s += "\tif q.hailToken ~= nil then " + g + ".Token(q, q.hailToken) end\n";
    s += "\t" + g + ".HailShow(q.hail, q.hailName or \"\", q.hailToken ~= nil and " + g + ".hailWordsMs or 0)\n";
    s += "end\n";
    s += "\n";
    // The hail's words on the screen: the HUD's narrative subtitle (SetNarrativeSubtitles,
    // the big period-font line the game uses for a narrator outside a conversation), as
    // "<name>: <words>", layout 0 (a block low on the right; layout 2 is one wide line
    // that runs off the screen, 1 and 3 draw nothing). The conversation caption
    // (SetSubtitles on bc.subtitles) is not used: its plate only draws while a dialogue is
    // open (caption_probe, 14 Oct 2026: the call returned ok, every parent visible and at
    // alpha 100, and the shot carried nothing), and Game.SendInfoText reads each word as a
    // string id (`@You! @Yes, ...`).
    //
    // One put, `delay` ms from now, and a hide with a fade at hailSeconds after it. One put
    // is the whole of it because the widget keeps a line about five seconds from the first
    // put and nothing said again extends that (narr_probe5, 14 Oct 2026, three roads in one
    // run: a single put, gone between 4.5 and 5.5 s; the same words put back every 400 ms
    // for eight seconds, taken down or not first, gone at the same moment; the mod's
    // earlier loop, gone a little sooner), so hailSeconds is what the widget gives and the
    // hide is for tidiness. The delay is for the bark: its opening wipes the line (WatchHail
    // above). A UI without the function (an older HUD) is left alone: the pcall fails
    // quietly and the voice is all there is.
    //
    // The hide is owed whether or not a conversation is open by then: until 28 Oct 2026 it
    // stood down when one was (`active`, so as to leave the HUD to the dialogue), and
    // Christine's shout stayed on the screen through the whole talk that followed it and
    // for minutes after (bugs.txt), since the widget's own five seconds do not run while a
    // dialogue holds the HUD and nothing else ever took the line down. HideNarrativeSubtitles
    // touches the narrative line alone, not the dialogue's captions. hailUp notes a line on
    // the screen, and Leave takes it down too if the timer has not (HailHide), for a line put
    // up in the last breath before the conversation opened.
    s += g + ".hailUp = false\n";
    s += "function " + g + ".HailHide()\n";
    s += "\tif not " + g + ".hailUp then return end\n";
    s += "\t" + g + ".hailUp = false\n";
    s += "\tfor _, i in ipairs(" + g + ".hudIds) do\n";
    s += "\t\tpcall(function() UIAction.CallFunction(\"hud\", i, \"HideNarrativeSubtitles\", 0.3) end)\n";
    s += "\tend\n";
    s += "end\n";
    s += "\n";
    s += "function " + g + ".HailShow(text, above, delay)\n";
    s += "\t" + g + ".hailGen = " + g + ".hailGen + 1\n";
    s += "\tlocal gen = " + g + ".hailGen\n";
    s += "\tlocal line = (above ~= \"\" and (above .. \": \") or \"\") .. text\n";
    s += "\tlocal function show()\n";
    s += "\t\tif gen ~= " + g + ".hailGen or " + g + ".active then return end\n";
    s += "\t\t" + g + ".hailUp = true\n";
    s += "\t\tfor _, i in ipairs(" + g + ".hudIds) do\n";
    s += "\t\t\tpcall(function() UIAction.CallFunction(\"hud\", i, \"SetNarrativeSubtitles\", line, 0, 0.2) end)\n";
    s += "\t\tend\n";
    s += "\t\tpcall(function() Script.SetTimer(" + g + ".hailSeconds * 1000, function()\n";
    s += "\t\t\tif gen ~= " + g + ".hailGen then return end\n";
    s += "\t\t\t" + g + ".HailHide()\n";
    s += "\t\tend) end)\n";
    s += "\tend\n";
    s += "\tif (delay or 0) > 0 then\n";
    s += "\t\tpcall(function() Script.SetTimer(delay, show) end)\n";
    s += "\telse\n";
    s += "\t\tshow()\n";
    s += "\tend\n";
    s += "end\n";
    s += "\n";
    // A holder dressed for his part (notes.md 13 Oct 2026: Benesh says Paul Schmidt bought
    // a fine new coat, so Paul wears one). actor:EquipClothingPreset(guid) puts a shipped
    // clothing_preset on a soul -- proven on spawned NPCs (wildman-costume.md §8), tried
    // here on a level soul and said in the log either way. Once a session, when the
    // entity is loaded; a save keeps the clothes, as it keeps any inventory, and a session
    // that finds him already in it has put on the same again, which the preset tolerates.
    s += g + ".dressed = {}\n";
    s += "function " + g + ".WatchDress(id, q, pp)\n";
    s += "\tif " + g + ".dressed[id] ~= nil then return end\n";
    s += "\tlocal e = nil\n";
    s += "\tpcall(function() e = System.GetEntityByName(q.target) end)\n";
    s += "\tif e == nil or e.actor == nil or e.actor.EquipClothingPreset == nil then return end\n";
    s += "\tlocal ok, err = pcall(function() return e.actor:EquipClothingPreset(q.dress) end)\n";
    s += "\t" + g + ".dressed[id] = ok and \"on\" or \"failed\"\n";
    s += "\t" + g + ".Log(\"BRIDGE quest \" .. id .. \" dress \" .. q.target .. \" in \" .. q.dress .. \" -> \" .. tostring(ok) .. \" \" .. tostring(err))\n";
    s += "end\n";
    s += "\n";
    // A find's keepsake (*Widow's View*; behaviors.md §6.1a, §10.4). Laid when the errand is
    // taken up (QuestCommand accept -> LayKeepsake) and, if that failed -- the spot too far
    // out to place at, a load that wiped it -- tried again from the watch while Henry is
    // within keepsakeMetres of the spot and has not got it. The laying is the probes'
    // recipe: the class made in Henry's pocket, found, and put down on a spot entity at the
    // ground height of the spot, so an item of the class lies there for the objective's
    // ItemOnGroundAsset to mark. Then his pocket is watched: holding it is the store's
    // `keepsake` (the letter e), which the app reads to answer the hand-in with the giver's
    // thanks_keepsake; QuestDelivered takes it with the things.
    s += g + ".keepsakeMetres = " + std::to_string(kKeepsakeMetres) + "\n";
    s += "function " + g + ".LayKeepsake(id, why)\n";
    s += "\tlocal q = " + g + ".quests[id]\n";
    s += "\tif q == nil or q.keepsake == nil or q.spot == nil then return false end\n";
    s += "\tlocal f = " + g + ".state.quests[id]\n";
    s += "\tif type(f) ~= \"table\" then f = {}; " + g + ".state.quests[id] = f end\n";
    s += "\tif f.laid then return true end\n";
    s += "\tlocal p = " + g + ".Player()\n";
    s += "\tif p == nil then return false end\n";
    s += "\tlocal z = " + g + ".Ground(q.spot[1], q.spot[2], q.spot[3])\n";
    s += "\tlocal spot = " + g + ".SpotEntity(\"moonshine_keepsake_\" .. id, q.spot[1], q.spot[2], z)\n";
    s += "\tif spot == nil then\n";
    s += "\t\t" + g + ".Log(\"BRIDGE quest \" .. id .. \" keepsake not laid (\" .. tostring(why) .. \"): no spot entity\")\n";
    s += "\t\treturn false\n";
    s += "\tend\n";
    s += "\tlocal ok, err = pcall(function()\n";
    s += "\t\tif " + g + ".Count(p, q.keepsake) <= 0 then p.inventory:CreateItem(q.keepsake, 100, 1) end\n";
    s += "\t\tlocal wuid = p.inventory:FindItem(q.keepsake)\n";
    s += "\t\tif wuid == nil then error(\"the keepsake could not be made\") end\n";
    s += "\t\tp.human:PlaceItem(wuid, spot.id, false)\n";
    s += "\tend)\n";
    s += "\tif not ok or " + g + ".Count(p, q.keepsake) > 0 then\n";
    s += "\t\tpcall(function() p.inventory:DeleteItemOfClass(q.keepsake, " + g + ".Count(p, q.keepsake)) end)\n";
    s += "\t\t" + g + ".Log(\"BRIDGE quest \" .. id .. \" keepsake not laid (\" .. tostring(why) .. \"): \" .. tostring(err))\n";
    s += "\t\treturn false\n";
    s += "\tend\n";
    s += "\tf.laid = true\n";
    s += "\t" + g + ".Log(\"BRIDGE quest \" .. id .. \" keepsake laid at \" .. string.format(\"%.1f,%.1f,%.1f\", q.spot[1], q.spot[2], z) .. \" (\" .. tostring(why) .. \")\")\n";
    s += "\tpcall(" + g + ".QuestFlags)\n";
    s += "\treturn true\n";
    s += "end\n";
    s += "\n";
    s += "function " + g + ".WatchKeepsake(id, q, pp)\n";
    s += "\tlocal f = " + g + ".state.quests[id] or {}\n";
    s += "\tlocal p = " + g + ".Player()\n";
    s += "\tlocal has = " + g + ".Count(p, q.keepsake) > 0\n";
    s += "\tif has ~= (f.keepsake == true) then\n";
    s += "\t\t" + g + ".StateQuest(id, has and \"keepsake\" or \"keepsake_gone\")\n";
    s += "\tend\n";
    s += "\tif not f.laid and not has then\n";
    s += "\t\tlocal d = math.sqrt((pp.x - q.spot[1]) ^ 2 + (pp.y - q.spot[2]) ^ 2)\n";
    s += "\t\tif d <= " + g + ".keepsakeMetres then " + g + ".LayKeepsake(id, \"near\") end\n";
    s += "\tend\n";
    s += "end\n";
    s += "\n";
    // A retrieve's chest (In the Stocks' sacristan; behaviors.md §6.3, proven): the thing
    // lies in a Stash of the mod's own at q.chest rather than in the holder's pocket. Spawned
    // from the watch when Henry is within chestMetres of the spot and the errand is on --
    // once: the store's `chest` says it stands, and the entity's name is looked for first,
    // since a spawned chest and its contents survive a save and reload (§6.3) while the
    // store might not have. The shipped Prefabs/Chest/chest.xml shape: the rustic model
    // (a .cdf, which the lockpicking camera wants), the Chest category, nobody's (so
    // opening it is not the crime; the house is the holder's and his household's business,
    // which the helper's door settles), locked at chestLock on the engine's 0..1 with the
    // difficulty overridden -- the model's own would be the rustic chest's 11.2/20 -- and
    // keyed to the quest's key class, so the key the helper gets Henry offers "Unlock and
    // open" and the game's generated master keys do not. bSkipAngleCheck so it opens from
    // any side of a cramped room. The thing goes in by the proven route: made in Henry's
    // pocket, moved to the chest's inventory by class (and the paper after it, where one is
    // written). Its leaving the chest is the errand's mid-moment (notes.md 14 Oct 2026: the
    // fetch objective stood open after the chest was emptied): once the chest stands, the
    // watch looks for the thing in Henry's pocket, and the first time it is there makes the
    // quest's taken token for the graph's trigger (the fetch Completed, the return open, the
    // holder's menu closed) and notes it in the store. The chest's facing is the spot's
    // fourth figure, a yaw in degrees (0 faces +y, 90 faces +x), so it stands square to the
    // room's furniture; the pose at spawn is a forward vector.
    s += g + ".chestMetres = " + std::to_string(kChestMetres) + "\n";
    s += "function " + g + ".WatchChest(id, q, pp)\n";
    s += "\tlocal f = " + g + ".state.quests[id]\n";
    s += "\tif type(f) ~= \"table\" then f = {}; " + g + ".state.quests[id] = f end\n";
    s += "\tif q.chest == nil then return end\n";
    s += "\tif f.chest then\n";
    s += "\t\tif not f.taken and " + g + ".Count(" + g + ".Player(), q.item) > 0 then\n";
    s += "\t\t\t" + g + ".Log(\"BRIDGE quest \" .. id .. \" thing taken from the chest, have=\" .. " + g +
         ".Count(" + g + ".Player(), q.item))\n";
    s += "\t\t\tpcall(" + g + ".StateQuest, id, \"taken\")\n";
    s += "\t\t\t" + g + ".Token(q, q.taken)\n";
    s += "\t\tend\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    s += "\tlocal name = \"mn_chest_\" .. id\n";
    s += "\tlocal standing = nil\n";
    s += "\tpcall(function() standing = System.GetEntityByName(name) end)\n";
    s += "\tif standing ~= nil then\n";
    s += "\t\t" + g + ".Log(\"BRIDGE quest \" .. id .. \" chest already standing\")\n";
    s += "\t\tpcall(" + g + ".StateQuest, id, \"chest\")\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    s += "\tlocal d = math.sqrt((pp.x - q.chest[1]) ^ 2 + (pp.y - q.chest[2]) ^ 2)\n";
    s += "\tif d > " + g + ".chestMetres then return end\n";
    s += "\tlocal ok, chest = pcall(System.SpawnEntity, {\n";
    s += "\t\tclass = \"Stash\", name = name,\n";
    s += "\t\tposition = { x = q.chest[1], y = q.chest[2], z = q.chest[3] },\n";
    s += "\t\torientation = { x = math.sin(math.rad(q.chest[4] or 0)), y = math.cos(math.rad(q.chest[4] or 0)), z = 0 },\n";
    s += "\t\tproperties = {\n";
    s += "\t\t\tobject_Model = \"objects/characters/assets/chest/chest_rustic_a.cdf\",\n";
    s += "\t\t\tsWH_AI_EntityCategory = \"Chest\", bOwnedByHome = false, bSkipAngleCheck = true,\n";
    s += "\t\t\tLock = { bLocked = true, bCanLockPick = true, bCanUnlockWithDynamicKey = false,\n";
    s += "\t\t\t\tbLockDifficultyOverride = true, fLockDifficulty = q.chestLock or 0.9,\n";
    s += "\t\t\t\tguidItemClassId = q.keyClass },\n";
    s += "\t\t\tDatabase = { sInventoryPreset = \"inventory_empty\", nRestockPeriodDays = 0, nDestockPeriodDays = 0 },\n";
    s += "\t\t},\n";
    s += "\t})\n";
    s += "\tif not ok or chest == nil then\n";
    s += "\t\t" + g + ".Log(\"BRIDGE quest \" .. id .. \" chest could not be made: \" .. tostring(chest))\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    s += "\tlocal moved = 0\n";
    s += "\tlocal p = " + g + ".Player()\n";
    s += "\tlocal okm, err = pcall(function()\n";
    s += "\t\tlocal inv = chest:GetInventoryToOpen()\n";
    s += "\t\tif inv == nil or inv == 0 then error(\"the chest has no inventory\") end\n";
    s += "\t\tp.inventory:CreateItem(q.item, 100, 1)\n";
    s += "\t\tmoved = p.inventory:MoveItemOfClass(inv, q.item, 1, true) or 0\n";
    s += "\t\tif q.paper ~= nil then\n";
    s += "\t\t\tp.inventory:CreateItem(q.paper, 100, 1)\n";
    s += "\t\t\tp.inventory:MoveItemOfClass(inv, q.paper, 1, true)\n";
    s += "\t\tend\n";
    s += "\tend)\n";
    s += "\tif not okm then " + g + ".Log(\"BRIDGE quest \" .. id .. \" chest could not be filled: \" .. tostring(err)) end\n";
    s += "\t" + g + ".Log(\"BRIDGE quest \" .. id .. \" chest made at \" .. string.format(\"%.1f,%.1f,%.1f\", q.chest[1], q.chest[2], q.chest[3])\n";
    s += "\t\t.. \" lock=\" .. tostring(q.chestLock) .. \" moved=\" .. tostring(moved) .. \" have=\" .. " + g + ".Count(p, q.item))\n";
    s += "\tpcall(" + g + ".StateQuest, id, \"chest\")\n";
    s += "end\n";
    s += "\n";
    // A travel: the one looked at, within the graph's distance. Not a body: the graph's
    // trigger goes quiet at the death (see the watching state), and so does this, or the
    // store would say seen where the journal says dead.
    // The face (q.mood, a fa_idle_* clip) goes on when Henry is within moodMetres -- far
    // enough out that it is set before he can read it, close enough that the clip is not
    // running on half the town -- and is stopped once seen, so a later conversation with
    // the target has its lipsync back. A load wipes layer 13 with everything else, so the
    // start is retried while he is in range and the errand unseen; StartAnimation on a
    // clip already looping is harmless.
    s += g + ".moodMetres = " + std::to_string(kTravelMoodMetres) + "\n";
    s += g + ".moodOn = {}\n";
    s += "function " + g + ".TargetFace(e, clip, on)\n";
    s += "\tlocal ok = false\n";
    s += "\tpcall(function()\n";
    s += "\t\tif on then\n";
    s += "\t\t\tif (e:GetAnimationLength(0, clip) or 0) <= 0 then return end\n";
    s += "\t\t\te:StartAnimation(0, clip, 13, 0.2, 1.0, true)\n";
    s += "\t\telse\n";
    s += "\t\t\te:StopAnimation(0, 13)\n";
    s += "\t\tend\n";
    s += "\t\tok = true\n";
    s += "\tend)\n";
    s += "\treturn ok\n";
    s += "end\n";
    s += "\n";
    s += "function " + g + ".WatchTravel(id, q, pp)\n";
    s += "\tlocal tp, ent = nil, nil\n";
    s += "\tpcall(function()\n";
    s += "\t\tlocal e = System.GetEntityByName(q.target)\n";
    s += "\t\tif e.actor ~= nil and e.actor:IsDead() then return end\n";
    s += "\t\ttp = e:GetWorldPos()\n";
    s += "\t\tent = e\n";
    s += "\tend)\n";
    s += "\tif tp == nil then return end\n";
    s += "\tlocal d = math.sqrt((pp.x - tp.x) ^ 2 + (pp.y - tp.y) ^ 2)\n";
    s += "\tif q.mood ~= nil and d <= " + g + ".moodMetres and " + g + ".moodOn[id] == nil then\n";
    s += "\t\tif " + g + ".TargetFace(ent, q.mood, true) then\n";
    s += "\t\t\t" + g + ".moodOn[id] = " + g + ".Now()\n";
    s += "\t\t\t" + g + ".Log(\"BRIDGE quest \" .. id .. \" face \" .. q.mood .. \" on \" .. q.target)\n";
    s += "\t\tend\n";
    s += "\tend\n";
    s += "\tif d <= " + g + ".watchMetres then\n";
    s += "\t\t" + g + ".Log(\"BRIDGE quest \" .. id .. \" seen \" .. q.target .. \" at \" .. string.format(\"%.1f\", d) .. \"m\")\n";
    s += "\t\tpcall(" + g + ".StateQuest, id, \"seen\")\n";
    s += "\t\tif q.mood ~= nil then " + g + ".TargetFace(ent, q.mood, false); " + g + ".moodOn[id] = nil end\n";
    s += "\tend\n";
    s += "end\n";
    s += "\n";
    // An exposure (Words Must Not Fail, notes.md 21 Oct 2026): the holder caught in the
    // night where he should not be. Henry within exposeMetres of the holder -- the width of
    // a room, so the wall of the house is between them otherwise -- with the holder within
    // exposeWithMetres of the one he is caught with (the two sides of a double bed are a
    // pace apart) in the quest's night window. Neither dead. Then the store (seen, which
    // QuestFlags writes to the menu's global, so the expose row is there the next time
    // Henry speaks to him) and the token for the graph's caught trigger (log_caught).
    s += g + ".exposeMetres = " + health_text(kExposeMetres) + "\n";
    s += g + ".exposeWithMetres = " + health_text(kExposeWithMetres) + "\n";
    s += g + ".enterWithMetres = " + health_text(kEnterWithMetres) + "\n";
    s += g + ".enterNearMetres = " + health_text(kEnterNearMetres) + "\n";
    s += g + ".enterSeeMetres = " + health_text(kEnterSeeMetres) + "\n";
    s += g + ".enterHoursBefore = " + health_text(kEnterHoursBefore) + "\n";
    s += g + ".entered = {}\n";
    // Whether the level labels the point `interior`: true, false, or nil when it cannot say
    // (no XGenAIModule, as in the harness).
    s += "function " + g + ".Indoors(p)\n";
    s += "\tlocal ok, r = pcall(function() return XGenAIModule.IsPointInAreaWithLabel(p, \"interior\") end)\n";
    s += "\tif not ok then return nil end\n";
    s += "\treturn r == true\n";
    s += "end\n";
    // How far the holder at tp is from her home: on the flat from the bed (q.bedPos, off
    // routine_bed_target) where the quest has one, since she is about the house and yard of
    // an evening and is no fixed point; else from her in three dimensions.
    s += "function " + g + ".HomeGap(q, tp, wp)\n";
    s += "\tif q.bedPos ~= nil then return math.sqrt((q.bedPos.x - tp.x) ^ 2 + (q.bedPos.y - tp.y) ^ 2) end\n";
    s += "\treturn math.sqrt((wp.x - tp.x) ^ 2 + (wp.y - tp.y) ^ 2 + (wp.z - tp.z) ^ 2)\n";
    s += "end\n";
    // Henry's aside at the door (notes.md 23 Oct 2026, `enters`): the holder gone in to the
    // witness in the evening, Henry near enough to have seen the door. Once a night: the
    // mark clears when the hour leaves the window, not as either of them moves (notes.md
    // 25 Oct 2026, three in one evening). See emit.hpp kEnter*.
    s += "function " + g + ".WatchEnters(id, q, pp, tp, wp)\n";
    s += "\tif q.enters == nil then return end\n";
    s += "\tif not " + g + ".HourIn({ q.hours[1] - " + g + ".enterHoursBefore, q.hours[2] }) then\n";
    s += "\t\t" + g + ".entered[id] = nil\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    s += "\tif " + g + ".entered[id] then return end\n";
    s += "\tlocal home = " + g + ".HomeGap(q, tp, wp)\n";
    s += "\tif home > " + g + ".enterWithMetres then return end\n";
    s += "\tlocal inside = " + g + ".Indoors(tp)\n";
    s += "\tif inside ~= true and home > " + g + ".enterNearMetres then return end\n";
    s += "\tlocal d = math.sqrt((pp.x - tp.x) ^ 2 + (pp.y - tp.y) ^ 2)\n";
    s += "\tif d > " + g + ".enterSeeMetres then return end\n";
    s += "\t" + g + ".entered[id] = true\n";
    s += "\t" + g + ".Log(\"BRIDGE quest \" .. id .. \" enters \" .. q.target .. \" in to \" .. q.seenWith .. \" at \"\n";
    s += "\t\t.. string.format(\"%.1f\", d) .. \"m, \" .. string.format(\"%.1f\", home)\n";
    s += "\t\t.. (q.bedPos ~= nil and \"m from the bed\" or \"m from her\")\n";
    s += "\t\t.. (inside == nil and \", unlabelled\" or (inside and \"\" or \", at the door\")))\n";
    s += "\tif " + g + ".active or " + g + ".InDialog() then return end\n";
    s += "\t" + g + ".Voice(\"henry\", q.enters)\n";
    s += "\t" + g + ".HailShow(q.enters, \"Henry\", 0)\n";
    s += "end\n";
    s += "function " + g + ".WatchExpose(id, q, pp)\n";
    s += "\tlocal tp, wp = nil, nil\n";
    s += "\tpcall(function()\n";
    s += "\t\tlocal e = System.GetEntityByName(q.target)\n";
    s += "\t\tif e.actor ~= nil and e.actor:IsDead() then return end\n";
    s += "\t\tlocal w = System.GetEntityByName(q.seenWith)\n";
    s += "\t\tif w.actor ~= nil and w.actor:IsDead() then return end\n";
    s += "\t\ttp = e:GetWorldPos()\n";
    s += "\t\twp = w:GetWorldPos()\n";
    s += "\tend)\n";
    s += "\tif tp == nil or wp == nil then return end\n";
    // The quick tick (emit.hpp kWatchQuickMs): the holder near Henry in the hours of the
    // door aside or the street, when a second's delay shows.
    s += "\tif (q.enters ~= nil or q.followHours ~= nil)\n";
    s += "\t\tand math.sqrt((pp.x - tp.x) ^ 2 + (pp.y - tp.y) ^ 2) <= " + g + ".enterSeeMetres\n";
    s += "\t\tand " + g + ".HourIn({ q.hours[1] - math.max(" + g + ".enterHoursBefore, " + g +
         ".followHoursBefore), q.hours[2] }) then\n";
    s += "\t\t" + g + ".watchQuick = true\n";
    s += "\tend\n";
    s += "\tpcall(" + g + ".WatchEnters, id, q, pp, tp, wp)\n";
    s += "\tif q.followHours ~= nil then\n";
    s += "\t\tpcall(" + g + ".FollowMorning, id, q)\n";
    s += "\t\tpcall(" + g + ".WatchMarker, id, q)\n";
    s += "\t\tpcall(" + g + ".WatchFollow, id, q, pp, tp, wp)\n";
    s += "\tend\n";
    s += "\tif not " + g + ".HourIn(q.hours) then return end\n";
    s += "\tlocal d = math.sqrt((pp.x - tp.x) ^ 2 + (pp.y - tp.y) ^ 2 + (pp.z - tp.z) ^ 2)\n";
    s += "\tlocal together = math.sqrt((wp.x - tp.x) ^ 2 + (wp.y - tp.y) ^ 2 + (wp.z - tp.z) ^ 2)\n";
    s += "\tif d > " + g + ".exposeMetres or together > " + g + ".exposeWithMetres then return end\n";
    s += "\t" + g + ".Log(\"BRIDGE quest \" .. id .. \" caught \" .. q.target .. \" with \" .. q.seenWith .. \" at \"\n";
    s += "\t\t.. string.format(\"%.1f\", d) .. \"m, \" .. string.format(\"%.1f\", together) .. \"m apart\")\n";
    s += "\tpcall(" + g + ".StateQuest, id, \"seen\")\n";
    s += "\t" + g + ".Token(q)\n";
    // The bedside (notes.md 22 Oct 2026): Henry says what wakes the man, in his own voice
    // and on the narrative line as a hail is, and the engine is asked for the holder's Talk
    // (RequestDialog, the Attempt shape) a few seconds on, once the token has landed and the
    // seen global is up so that the Talk opens on the bedside sequence and not the day's.
    // Our own held conversation is never a candidate for it, since the holder has no role of
    // ours granted outside a session; the game's own Talk on him is the one match. The
    // request only queues (Attempt): if his behaviour tree sleeps through it, the player
    // presses Talk on him, and the sequence is the same.
    s += "\tif q.wake ~= nil then\n";
    s += "\t\t" + g + ".Voice(\"henry\", q.wake)\n";
    s += "\t\t" + g + ".HailShow(q.wake, \"Henry\", 0)\n";
    s += "\t\tpcall(function() Script.SetTimer(" + std::to_string(kWakeTalkMs) + ", function()\n";
    s += "\t\t\tif " + g + ".active or " + g + ".InDialog() then return end\n";
    s += "\t\t\tlocal e = nil\n";
    s += "\t\t\tpcall(function() e = System.GetEntityByName(q.target) end)\n";
    s += "\t\t\tif e == nil or e.actor == nil then return end\n";
    s += "\t\t\t" + g + ".Log(\"BRIDGE quest \" .. id .. \" asks for the holder's Talk at the bedside\")\n";
    s += "\t\t\t" + g + ".Attempt(e, " + g + ".Player())\n";
    s += "\t\tend) end)\n";
    s += "\tend\n";
    s += "end\n";
    s += "\n";
    // The street (notes.md 23 Oct 2026, n8; npcdb.hpp has_follow; emit.hpp kFollow*). The
    // watch looks for Henry through the holder's eyes every tick (WatchExpose): the errand
    // on and nobody caught, the walk's hours, the holder not indoors nor at the witness's
    // side, and Henry in his sight -- within followSeeMetres, inside the cone about the way
    // he faces or within followHearMetres from any side, and nothing between their eyes
    // (System.RayTraceCheck, true when the ray from one to the other meets nothing but the
    // two of them; where the engine has no such call the sight is clear). Then the night
    // is his: the store's `followed` (the global his Talk reads, QuestFlags), the token for
    // the graph (his bed swapped, the journal's line), and the engine asked for his Talk a
    // moment on, as at the bedside, so he turns on Henry with the line; if his behaviour
    // tree sleeps through it, Henry's own press of Talk opens the same. FollowMorning clears
    // the night once the hour is past the window's end (the graph's own rest Timer clears
    // its side). The game's own perception was the first design (the graph's
    // PerceptionTrigger reporting through the console command, GraphSaid): the trigger
    // fires but ExecuteConsoleString is inert in the release build, so no sighting ever
    // arrived (24 Oct 2026); the command stays registered and GraphSaid logs what it is
    // told, for the probes.
    s += g + ".followSeeMetres = " + health_text(kFollowSeeMetres) + "\n";
    s += g + ".followHearMetres = " + health_text(kFollowHearMetres) + "\n";
    s += g + ".followHoursBefore = " + health_text(kFollowHoursBefore) + "\n";
    s += g + ".followCosine = " + health_text(std::cos(kFollowConeDegrees / 2.0 * 3.14159265358979 / 180.0)) + "\n";
    s += g + ".followEyeMetres = " + health_text(kFollowEyeMetres) + "\n";
    s += g + ".followWarnMetres = " + health_text(kFollowWarnMetres) + "\n";
    s += g + ".followOverMillis = " + std::to_string(kFollowOverMs) + "\n";
    s += g + ".followClearSeconds = " + std::to_string(kFollowClearHours * 3600) + "\n";
    s += g + ".followWarned = {}\n";
    s += g + ".followNight = {}\n";
    // The walk (emit.hpp kFollowStepMetres, kFollowJumpMetres, kFollowGraceSeconds,
    // kClockJumpSeconds; bugs.txt 28 Oct 2026): where the holder stood last tick and the
    // world clock's reading then, per quest, and the real moment of the last carry or clock
    // jump, for the grace.
    s += g + ".followStepMetres = " + health_text(kFollowStepMetres) + "\n";
    s += g + ".followJumpMetres = " + health_text(kFollowJumpMetres) + "\n";
    s += g + ".followGraceSeconds = " + std::to_string(kFollowGraceSeconds) + "\n";
    s += g + ".clockJumpSeconds = " + std::to_string(kClockJumpSeconds) + "\n";
    s += g + ".followStillSeconds = " + std::to_string(kFollowStillSeconds) + "\n";
    s += g + ".followTalkWaitMs = " + std::to_string(kFollowTalkWaitMs) + "\n";
    s += g + ".followLast = {}\n";
    s += g + ".followHeld = {}\n";
    s += g + ".followClock = nil\n";
    s += g + ".followGraceAt = nil\n";
    s += "function " + g + ".GraphSaid(kind, id, word)\n";
    s += "\tkind, id, word = tostring(kind), tostring(id), tostring(word)\n";
    s += "\tlocal q = " + g + ".quests[id]\n";
    s += "\tif q == nil then\n";
    s += "\t\t" + g + ".Log(\"BRIDGE graph said \" .. kind .. \" of unknown quest \" .. id)\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    s += "\t" + g + ".Log(\"BRIDGE graph said \" .. kind .. \" \" .. id .. \" \" .. word)\n";
    s += "end\n";
    // Whether the holder at tp, facing the way his entity does, has Henry at pp in sight:
    // the cone or the close range, then the ray between their eyes. Returns the verdict and
    // a word for the log.
    s += "function " + g + ".Sees(e, tp, pp)\n";
    s += "\tlocal dx, dy, dz = pp.x - tp.x, pp.y - tp.y, pp.z - tp.z\n";
    s += "\tlocal d = math.sqrt(dx ^ 2 + dy ^ 2 + dz ^ 2)\n";
    s += "\tif d > " + g + ".followSeeMetres then return false, \"far\" end\n";
    s += "\tlocal how = \"close\"\n";
    s += "\tif d > " + g + ".followHearMetres then\n";
    s += "\t\tlocal fwd = nil\n";
    s += "\t\tpcall(function() fwd = e:GetDirectionVector() end)\n";
    s += "\t\tif fwd ~= nil and d > 0 then\n";
    s += "\t\t\tlocal flat = math.sqrt(fwd.x ^ 2 + fwd.y ^ 2)\n";
    s += "\t\t\tif flat > 0 and (fwd.x * dx + fwd.y * dy) / (flat * math.sqrt(dx ^ 2 + dy ^ 2)) < " + g +
         ".followCosine then return false, \"behind\" end\n";
    s += "\t\t\thow = \"ahead\"\n";
    s += "\t\telse\n";
    s += "\t\t\thow = \"near\"\n";
    s += "\t\tend\n";
    s += "\tend\n";
    s += "\tlocal clear = true\n";
    s += "\tpcall(function()\n";
    s += "\t\tlocal up = " + g + ".followEyeMetres\n";
    s += "\t\tclear = System.RayTraceCheck({ x = tp.x, y = tp.y, z = tp.z + up }, { x = pp.x, y = pp.y, z = pp.z + up },\n";
    s += "\t\t\te.id, " + g + ".Player().id) ~= false\n";
    s += "\tend)\n";
    s += "\tif not clear then return false, \"hidden\" end\n";
    s += "\treturn true, how\n";
    s += "end\n";
    // Whether Henry at pp has the holder at tp before his eyes: the holder's head in the
    // camera's frame (System.IsPointVisible, the frustum) and nothing between their eyes
    // (the ray, as Sees asks it the other way). Where the engine has neither call the view
    // is clear. Returns the verdict and a word for the log.
    s += "function " + g + ".InView(e, tp, pp)\n";
    s += "\tlocal up = " + g + ".followEyeMetres\n";
    s += "\tlocal head = { x = tp.x, y = tp.y, z = tp.z + up }\n";
    s += "\tlocal framed = true\n";
    s += "\tpcall(function() if System.IsPointVisible(head) == false then framed = false end end)\n";
    s += "\tif not framed then return false, \"out of frame\" end\n";
    s += "\tlocal clear = true\n";
    s += "\tpcall(function()\n";
    s += "\t\tclear = System.RayTraceCheck({ x = pp.x, y = pp.y, z = pp.z + up }, head, " + g + ".Player().id, e.id) ~= false\n";
    s += "\tend)\n";
    s += "\tif not clear then return false, \"hidden\" end\n";
    s += "\treturn true, \"in view\"\n";
    s += "end\n";
    // The walk (bugs.txt 28 Oct 2026; emit.hpp kFollowStepMetres and after). Where the
    // holder stood last tick, per quest, and when he last moved a step: a sighting wants a
    // man walking (moved within followStillSeconds), not one the engine has just set down
    // -- a step of followJumpMetres or more between ticks is a re-placing, and he must walk
    // again from there. The world clock between ticks too: a jump of clockJumpSeconds or
    // more (the game's wait or sleep, the giver's carrying) opens the grace, as the carry
    // itself does (FastTravelGo), and for followGraceSeconds of real time no sighting is
    // judged. Returns nil for a walk that counts, else the word for why not.
    s += "function " + g + ".FollowStill(id, tp)\n";
    s += "\tlocal now = " + g + ".Now()\n";
    s += "\tlocal clock = " + g + ".WorldTime()\n";
    s += "\tif clock ~= nil and " + g + ".followClock ~= nil and math.abs(clock - " + g + ".followClock) >= " + g +
         ".clockJumpSeconds then\n";
    s += "\t\t" + g + ".followGraceAt = now\n";
    s += "\t\t" + g + ".Log(string.format(\"BRIDGE quest follow grace, the clock jumped %.0f minutes\", (clock - " + g +
         ".followClock) / 60))\n";
    s += "\tend\n";
    s += "\tif clock ~= nil then " + g + ".followClock = clock end\n";
    s += "\tlocal last = " + g + ".followLast[id]\n";
    s += "\tlocal here = { x = tp.x, y = tp.y, movedAt = last ~= nil and last.movedAt or nil }\n";
    s += "\tlocal why = nil\n";
    s += "\tif last == nil then\n";
    s += "\t\twhy = \"first seen\"\n";
    s += "\telse\n";
    s += "\t\tlocal step = math.sqrt((tp.x - last.x) ^ 2 + (tp.y - last.y) ^ 2)\n";
    s += "\t\tif step >= " + g + ".followJumpMetres then\n";
    s += "\t\t\there.movedAt = nil\n";
    s += "\t\t\twhy = string.format(\"set down %.0fm off\", step)\n";
    s += "\t\telseif step >= " + g + ".followStepMetres then\n";
    s += "\t\t\there.movedAt = now\n";
    s += "\t\tend\n";
    s += "\tend\n";
    s += "\t" + g + ".followLast[id] = here\n";
    s += "\tif why == nil and (here.movedAt == nil or now - here.movedAt > " + g + ".followStillSeconds) then why = \"standing\" end\n";
    s += "\tif " + g + ".followGraceAt ~= nil and now - " + g + ".followGraceAt < " + g + ".followGraceSeconds then\n";
    s += "\t\twhy = \"in the grace\"\n";
    s += "\tend\n";
    s += "\treturn why\n";
    s += "end\n";
    s += "function " + g + ".WatchFollow(id, q, pp, tp, wp)\n";
    s += "\tlocal f = (" + g + ".state.quests or {})[id] or {}\n";
    s += "\tif not f.accepted or f.done or f.handed or f.seen or f.followed then return end\n";
    // His steps and the clock are kept every tick, indoors or out, so the first tick of his
    // walk out of a door is measured from where he was and not from nothing.
    s += "\tlocal still = " + g + ".FollowStill(id, tp)\n";
    s += "\tif not " + g + ".HourIn(q.followHours) then return end\n";
    s += "\tif " + g + ".entered[id] then return end\n";
    s += "\tif " + g + ".Indoors(tp) == true then return end\n";
    s += "\tif " + g + ".HomeGap(q, tp, wp) <= " + g + ".enterWithMetres then return end\n";
    s += "\tlocal e = nil\n";
    s += "\tpcall(function() e = System.GetEntityByName(q.target) end)\n";
    s += "\tif e == nil then return end\n";
    s += "\tlocal d = math.sqrt((pp.x - tp.x) ^ 2 + (pp.y - tp.y) ^ 2)\n";
    // Henry's warning to himself (follow_warn, notes.md 25 Oct 2026): the holder on his walk
    // within followWarnMetres and before Henry's eyes (InView: in the frame, nothing between
    // them), before any sighting; once a night (FollowMorning clears it), and not over a
    // conversation. "There he goes" with the man round a corner or behind Henry was the
    // first build (notes.md 25 Oct 2026, 15:33), so the words wait on the view; a holder
    // who sees Henry first forfeits the warning, the night being his.
    s += "\tlocal seen, how = " + g + ".Sees(e, tp, pp)\n";
    s += "\tif q.followWarn ~= nil and not " + g + ".followWarned[id] and d <= " + g + ".followWarnMetres then\n";
    s += "\t\tif seen then\n";
    s += "\t\t\t" + g + ".followWarned[id] = true\n";
    s += "\t\t\t" + g + ".followNight[id] = " + g + ".WorldTime()\n";
    s += "\t\telse\n";
    s += "\t\t\tlocal shown, why = " + g + ".InView(e, tp, pp)\n";
    s += "\t\t\tif shown then\n";
    s += "\t\t\t\t" + g + ".followWarned[id] = true\n";
    s += "\t\t\t\t" + g + ".followNight[id] = " + g + ".WorldTime()\n";
    s += "\t\t\t\t" + g + ".Log(\"BRIDGE quest \" .. id .. \" follow warn, \" .. q.target .. \" at \" .. string.format(\"%.1f\", d) .. \"m, \" .. why)\n";
    s += "\t\t\t\tif not " + g + ".active and not " + g + ".InDialog() then\n";
    s += "\t\t\t\t\t" + g + ".Voice(\"henry\", q.followWarn)\n";
    s += "\t\t\t\t\t" + g + ".HailShow(q.followWarn, \"Henry\", 0)\n";
    s += "\t\t\t\tend\n";
    s += "\t\t\tend\n";
    s += "\t\tend\n";
    s += "\tend\n";
    s += "\tif not seen then return end\n";
    // Henry in his sight, but the man not on his walk (FollowStill): no night lost. Said
    // once per reason, so the log tells why and does not fill with it.
    s += "\tif still ~= nil then\n";
    s += "\t\tif " + g + ".followHeld[id] ~= still then\n";
    s += "\t\t\t" + g + ".followHeld[id] = still\n";
    s += "\t\t\t" + g + ".Log(\"BRIDGE quest \" .. id .. \" follow held, \" .. q.target .. \" at \" .. string.format(\"%.1f\", d)\n";
    s += "\t\t\t\t.. \"m, \" .. how .. \", \" .. still)\n";
    s += "\t\tend\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    s += "\t" + g + ".followHeld[id] = nil\n";
    s += "\t" + g + ".Log(\"BRIDGE quest \" .. id .. \" followed \" .. q.target .. \" at \" .. string.format(\"%.1f\", d)\n";
    s += "\t\t.. \"m, \" .. how)\n";
    s += "\t" + g + ".followNight[id] = " + g + ".WorldTime()\n";
    s += "\tpcall(" + g + ".StateQuest, id, \"followed\")\n";
    s += "\t" + g + ".Token(q, q.followToken)\n";
    s += "\tif " + g + ".active or " + g + ".InDialog() then return end\n";
    s += "\tpcall(function() Script.SetTimer(" + std::to_string(kFollowTalkMs) + ", function()\n";
    s += "\t\tif " + g + ".active or " + g + ".InDialog() then return end\n";
    s += "\t\tlocal e = nil\n";
    s += "\t\tpcall(function() e = System.GetEntityByName(q.target) end)\n";
    s += "\t\tif e == nil or e.actor == nil then return end\n";
    s += "\t\t" + g + ".Log(\"BRIDGE quest \" .. id .. \" asks for the holder's Talk in the street\")\n";
    s += "\t\t" + g + ".Attempt(e, " + g + ".Player())\n";
    // The request only queues (emit.hpp kFollowTalkWaitMs): if no dialogue is up by then,
    // the mod says the row itself (FollowSaid).
    s += "\t\tpcall(function() Script.SetTimer(" + g + ".followTalkWaitMs, function()\n";
    s += "\t\t\tif " + g + ".active or " + g + ".InDialog() then return end\n";
    s += "\t\t\tlocal f = (" + g + ".state.quests or {})[id] or {}\n";
    s += "\t\t\tif not f.followed or f.followedSaid then return end\n";
    s += "\t\t\t" + g + ".FollowSaid(id, q)\n";
    s += "\t\tend) end)\n";
    s += "\tend) end)\n";
    s += "end\n";
    // The row said without his Talk (bugs.txt 28 Oct 2026: the sighting judged, the request
    // made and answered true, and no dialogue, no line, nothing on the screen; the night
    // lost in silence). His line in his voice and on the narrative line as a hail is, with
    // his name before it; Henry's answer once it has been read; then the night given up
    // (FollowOver), as his Talk's exit would have. The store notes the row said
    // (followed_said), which takes the global down (QuestFlags) so a later press of Talk on
    // him opens his day's menu and not the row again; the night itself stands until morning.
    s += "function " + g + ".FollowSaid(id, q)\n";
    s += "\tif q.followedLine == nil or q.followedLine == \"\" then return end\n";
    s += "\t" + g + ".Log(\"BRIDGE quest \" .. id .. \" holder's Talk never opened, the row said in the street\")\n";
    s += "\tpcall(" + g + ".StateQuest, id, \"followed_said\")\n";
    s += "\t" + g + ".Voice(q.holderVoice or \"male\", q.followedLine, q.target)\n";
    s += "\t" + g + ".HailShow(q.followedLine, q.holderName or \"\", 0)\n";
    s += "\tlocal wait = " + g + ".Reading(q.followedLine) * 1000\n";
    s += "\tpcall(function() Script.SetTimer(wait, function()\n";
    s += "\t\tif " + g + ".active or " + g + ".InDialog() then return end\n";
    s += "\t\tif q.followedReply ~= nil and q.followedReply ~= \"\" then\n";
    s += "\t\t\t" + g + ".Voice(\"henry\", q.followedReply)\n";
    s += "\t\t\t" + g + ".HailShow(q.followedReply, \"Henry\", 0)\n";
    s += "\t\tend\n";
    s += "\t\t" + g + ".FollowOver(id)\n";
    s += "\tend) end)\n";
    s += "end\n";
    // The night's marker on him (bugs.txt 28 Oct 2026; emit.hpp mark_shown_class): wanted
    // in markHours -- from his homecoming (routine_home_at) to the night window's end --
    // and not for a night already `followed`; the store's `marked` says what the map has,
    // and when the two differ the token for the graph's markShown or markHidden trigger is
    // made and the store told. Nothing while the night is still to be accepted or is over
    // (caught, handed, done): the objective's own states carry the marker then. A night
    // carried across a save keeps `marked` in the store, so a load in his hours puts no
    // second token where the map already has him.
    s += "function " + g + ".WatchMarker(id, q)\n";
    s += "\tif q.markHours == nil or q.markShown == nil or q.markHidden == nil then return end\n";
    s += "\tlocal f = (" + g + ".state.quests or {})[id] or {}\n";
    s += "\tif not f.accepted or f.done or f.handed or f.seen then return end\n";
    s += "\tlocal want = " + g + ".HourIn(q.markHours) and not f.followed\n";
    s += "\tif (f.marked == true) == want then return end\n";
    s += "\t" + g + ".Log(\"BRIDGE quest \" .. id .. \" marker \" .. (want and \"on\" or \"off\") .. \" \" .. q.target)\n";
    s += "\tpcall(" + g + ".StateQuest, id, want and \"marked\" or \"unmarked\")\n";
    s += "\t" + g + ".Token(q, want and q.markShown or q.markHidden)\n";
    s += "end\n";
    // The morning: the hour past the window's end, and the world clock kFollowClearHours
    // on from the night's last word (the warning or the sighting; the store's followedAt
    // for a night carried across a save). The hour alone was the first rule, and the clock
    // read 20.50 a second after the carry's winding and 20.36 two minutes later with the
    // night cleared and the warning said again in between (26 Oct 2026, the user's log): a
    // reading from the minutes after Calendar.SetWorldTime is not to be trusted on its own,
    // and the clock is asked as well. The window's end is at least six and a half hours
    // from the latest sighting, so a true morning is never kept waiting.
    s += "function " + g + ".FollowMorning(id, q)\n";
    s += "\tif " + g + ".HourIn({ q.followHours[1], q.hours[2] }) then return end\n";
    s += "\tlocal now = " + g + ".WorldTime()\n";
    s += "\tlocal function fresh(at)\n";
    s += "\t\treturn at ~= nil and now ~= nil and now - at < " + g + ".followClearSeconds\n";
    s += "\tend\n";
    s += "\tif fresh(" + g + ".followNight[id]) then return end\n";
    s += "\t" + g + ".followWarned[id] = nil\n";
    s += "\t" + g + ".followNight[id] = nil\n";
    s += "\tlocal f = (" + g + ".state.quests or {})[id]\n";
    s += "\tif f == nil or not f.followed then return end\n";
    s += "\tif fresh(f.followedAt) then return end\n";
    s += "\t" + g + ".Log(\"BRIDGE quest \" .. id .. \" followed night over\")\n";
    s += "\tpcall(" + g + ".StateQuest, id, \"unfollowed\")\n";
    s += "end\n";
    // The night lost (follow_over): the holder's Talk has closed on the `followed` row
    // (its exit script, QuestStage "open followed") and Henry, alone in the street again,
    // says so to himself a moment on.
    // The dialogue may still be fading when the timer first fires; it waits again, a few
    // times, and gives the line up if the player is still in some conversation.
    s += "function " + g + ".FollowOver(id, tries)\n";
    s += "\tlocal q = " + g + ".quests[id]\n";
    s += "\tif q == nil or q.followOver == nil then return end\n";
    s += "\ttries = tries or 4\n";
    s += "\tpcall(function() Script.SetTimer(" + g + ".followOverMillis, function()\n";
    s += "\t\tif " + g + ".active then return end\n";
    s += "\t\tif " + g + ".InDialog() then\n";
    s += "\t\t\tif tries > 1 then " + g + ".FollowOver(id, tries - 1) end\n";
    s += "\t\t\treturn\n";
    s += "\t\tend\n";
    s += "\t\t" + g + ".Log(\"BRIDGE quest \" .. id .. \" follow over\")\n";
    s += "\t\t" + g + ".Voice(\"henry\", q.followOver)\n";
    s += "\t\t" + g + ".HailShow(q.followOver, \"Henry\", 0)\n";
    s += "\tend) end)\n";
    s += "end\n";
    s += "\n";
    // A beat: the man down (actor:IsUnconscious, what the graph's SoulUnconsciousTrigger
    // fires on) or dead (which the graph's death state has too), whoever did it -- the
    // errand does not ask who. Dead first: a dead man may read as unconscious as well.
    s += "function " + g + ".WatchBeat(id, q)\n";
    s += "\tlocal down, dead = false, false\n";
    s += "\tpcall(function()\n";
    s += "\t\tlocal e = System.GetEntityByName(q.target)\n";
    s += "\t\tif e.actor == nil then return end\n";
    s += "\t\tdead = e.actor:IsDead()\n";
    s += "\t\tif not dead then down = e.actor:IsUnconscious() end\n";
    s += "\tend)\n";
    s += "\tif dead then\n";
    s += "\t\t" + g + ".Log(\"BRIDGE quest \" .. id .. \" target \" .. q.target .. \" dead\")\n";
    s += "\t\tpcall(" + g + ".StateQuest, id, \"target_dead\")\n";
    s += "\telseif down then\n";
    s += "\t\t" + g + ".Log(\"BRIDGE quest \" .. id .. \" beaten \" .. q.target)\n";
    s += "\t\tpcall(" + g + ".StateQuest, id, \"beaten\")\n";
    s += "\tend\n";
    s += "end\n";
    s += "\n";
    // A pray: the game's own praying context on the player soul (HasScriptContext of
    // kPrayContext, up while Henry kneels at a shrine, down when the prayer ends), seen up
    // with Henry within prayMetres of the spot -- the kneel, kept per quest -- and then
    // seen down after kPrayKneelSeconds at least: the prayer said, at this shrine and not
    // another. Standing at the cross is not praying; kneeling at a different cross is not
    // this errand's; kneeling and rising at once (the game's Stop) is not a prayer. Then
    // the store, and the token in his pocket for the graph to see.
    s += g + ".prayMetres = " + std::to_string(kPrayMetres) + "\n";
    s += g + ".prayKneelSeconds = " + std::to_string(kPrayKneelSeconds) + "\n";
    s += g + ".prayContext = \"" + std::string(kPrayContext) + "\"\n";
    s += g + ".prayKneel = {}\n";
    s += "function " + g + ".WatchPray(id, q, pp)\n";
    s += "\tlocal d = math.sqrt((pp.x - q.spot[1]) ^ 2 + (pp.y - q.spot[2]) ^ 2)\n";
    s += "\tlocal praying = false\n";
    s += "\tpcall(function() praying = " + g + ".Player().soul:HasScriptContext(" + g + ".prayContext) == true end)\n";
    s += "\tif praying then\n";
    s += "\t\tif d <= " + g + ".prayMetres and " + g + ".prayKneel[id] == nil then\n";
    s += "\t\t\t" + g + ".prayKneel[id] = " + g + ".Now()\n";
    s += "\t\t\t" + g + ".Log(\"BRIDGE quest \" .. id .. \" kneeling at \" .. string.format(\"%.1f\", d) .. \"m\")\n";
    s += "\t\tend\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    s += "\tif " + g + ".prayKneel[id] == nil then return end\n";
    s += "\tlocal held = " + g + ".Now() - " + g + ".prayKneel[id]\n";
    s += "\t" + g + ".prayKneel[id] = nil\n";
    s += "\tif held < " + g + ".prayKneelSeconds then\n";
    s += "\t\t" + g + ".Log(\"BRIDGE quest \" .. id .. \" rose after \" .. string.format(\"%.0f\", held) .. \"s, not a prayer\")\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    s += "\t" + g + ".Log(\"BRIDGE quest \" .. id .. \" prayed at \" .. string.format(\"%.1f\", d) .. \"m after \" .. string.format(\"%.0f\", held) .. \"s\")\n";
    s += "\tpcall(" + g + ".StateQuest, id, \"prayed\")\n";
    s += "\t" + g + ".Token(q)\n";
    s += "end\n";
    s += "\n";
    // The token: one of the quest's own item class (its token, or the class given: a
    // chest's taken token) made in the player's pocket (inventory:CreateItem, as the letter
    // and the reward are made) and deleted a moment later, after the graph's
    // ItemDescriptorTrigger has seen it arrive.
    s += "function " + g + ".Token(q, class)\n";
    s += "\tclass = class or q.token\n";
    s += "\tif class == nil then return end\n";
    s += "\tlocal p = " + g + ".Player()\n";
    s += "\tlocal ok, err = pcall(function() p.inventory:CreateItem(class, 100, 1) end)\n";
    s += "\tif not ok then\n";
    s += "\t\t" + g + ".Log(\"BRIDGE quest token could not be made: \" .. tostring(err))\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    s += "\t" + g + ".Log(\"BRIDGE quest token made, have=\" .. " + g + ".Count(p, class))\n";
    s += "\t" + g + ".tokenClass = class\n";
    // Each token's own hide, by class: two in the same breath (the street's followed and the
    // night's marker, 28 Oct 2026) would otherwise leave the first in his pocket for good.
    s += "\tScript.SetTimer(1500, function() " + g + ".TokenGone(class) end)\n";
    s += "end\n";
    s += "\n";
    s += "function " + g + ".TokenGone(class)\n";
    s += "\tclass = class or " + g + ".tokenClass\n";
    s += "\tif class == nil then return end\n";
    s += "\tif " + g + ".tokenClass == class then " + g + ".tokenClass = nil end\n";
    s += "\tlocal p = " + g + ".Player()\n";
    s += "\tlocal have = " + g + ".Count(p, class)\n";
    s += "\tif have > 0 then pcall(function() p.inventory:DeleteItemOfClass(class, have) end) end\n";
    s += "\t" + g + ".Log(\"BRIDGE quest token gone, have=\" .. " + g + ".Count(p, class))\n";
    s += "end\n";
    s += "\n";

    // A leave or a return (quest-design/set-aside-stories.md §2, §4). The place is a point
    // in the level and the hour a window; nothing is added to the map, so the objective
    // names the place in words and the marker stays on the giver. Within spotMetres of it,
    // in the window, with the thing on him, the leave key goes live -- the typing toggle's
    // own map (F9), pointed at the player and switched on only while he stands there, so a
    // stray press anywhere else reaches nobody -- and SpotKey puts every one of the thing
    // down at the spot through human:PlaceItem, which wants an entity to place at (a spawned
    // AudioTriggerSpot, removed after). A drop by hand -- the count gone from some to none
    // while he stands there -- is taken as the same. Then the store (left), the token for
    // the graph, and for a leave the cue: the sound from the dark, played from a spawned
    // spot cueMetres off a little later, heard if Henry is still near. Outside the window
    // the hint says so and nothing goes down. A return's grave has its own weather
    // (WatchGrave): the wind once as he comes, a cough now and then while he is there, the
    // robbing seen as the level's own loot rising in his pocket within lootMetres (dust and
    // bats), and the curse laid when he is curseMetres off with the goods still on him or the
    // grave robbed -- the game's plague buff, which the clock lifts after curseHours of game
    // time or putting the goods down lifts at once.
    s += g + ".spotMetres = " + std::to_string(kSpotMetres) + "\n";
    s += g + ".cueMetres = " + std::to_string(kCueMetres) + "\n";
    s += g + ".cueDelaySeconds = " + std::to_string(kCueDelaySeconds) + "\n";
    s += g + ".cueHeardMetres = " + std::to_string(kCueHeardMetres) + "\n";
    s += g + ".lootMetres = " + std::to_string(kLootMetres) + "\n";
    s += g + ".curseMetres = " + std::to_string(kCurseMetres) + "\n";
    s += g + ".curseHours = " + std::to_string(kCurseHours) + "\n";
    s += g + ".coughMetres = " + std::to_string(kCoughMetres) + "\n";
    s += g + ".coughSeconds = " + std::to_string(kCoughSeconds) + "\n";
    s += g + ".curseBuff = \"" + std::string(kCurseBuff) + "\"\n";
    s += g + ".smellyBuff = \"" + std::string(kSmellyBuff) + "\"\n";
    s += g + ".coughSound = \"" + std::string(kCoughSound) + "\"\n";
    s += g + ".windSound = \"" + std::string(kWindSound) + "\"\n";
    s += g + ".batsSound = \"" + std::string(kBatsSound) + "\"\n";
    s += g + ".batsEffect = \"" + std::string(kBatsEffect) + "\"\n";
    s += g + ".dustEffect = \"" + std::string(kDustEffect) + "\"\n";
    s += g + ".spotArmed = nil\n";
    s += g + ".spotHinted = {}\n";
    s += g + ".spotWas = {}\n";
    s += g + ".cueDue = {}\n";
    s += g + ".spotEnts = {}\n";
    s += g + ".lootSeen = {}\n";
    s += g + ".windAt = {}\n";
    s += g + ".coughAt = 0\n";
    s += g + ".curseInst = nil\n";
    // The hour window: { from, to } in game hours, to exclusive, across midnight when from
    // is the later (23, 1 is eleven at night to one in the morning). No Calendar is any hour.
    s += "function " + g + ".HourIn(hours)\n";
    s += "\tif hours == nil then return true end\n";
    s += "\tlocal h = nil\n";
    s += "\tpcall(function() h = Calendar.GetWorldHourOfDay() end)\n";
    s += "\tif type(h) ~= \"number\" then return true end\n";
    s += "\tlocal a, b = hours[1], hours[2]\n";
    s += "\tif a <= b then return h >= a and h < b end\n";
    s += "\treturn h >= a or h < b\n";
    s += "end\n";
    s += "\n";
    s += "function " + g + ".HourWords(hours)\n";
    s += "\tlocal function w(h)\n";
    s += "\t\tif h == 0 or h == 24 then return \"midnight\" end\n";
    s += "\t\tif h == 12 then return \"noon\" end\n";
    s += "\t\tif h < 12 then return tostring(h) .. \" in the morning\" end\n";
    s += "\t\tif h < 18 then return tostring(h - 12) .. \" in the afternoon\" end\n";
    s += "\t\treturn tostring(h - 12) .. \" at night\"\n";
    s += "\tend\n";
    s += "\treturn \"between \" .. w(hours[1]) .. \" and \" .. w(hours[2])\n";
    s += "end\n";
    s += "\n";
    // An on-screen word about the place, once in twenty seconds per errand, since the tick
    // would say it every two.
    s += "function " + g + ".SpotHint(id, text)\n";
    s += "\tlocal at = " + g + ".spotHinted[id]\n";
    s += "\tif at ~= nil and " + g + ".Now() - at < 20 then return end\n";
    s += "\t" + g + ".spotHinted[id] = " + g + ".Now()\n";
    s += "\tpcall(function() Game.SendInfoText(text, true, nil, 4) end)\n";
    s += "end\n";
    s += "\n";
    s += "function " + g + ".SpotArm(id)\n";
    s += "\tif " + g + ".spotArmed == id then return end\n";
    s += "\t" + g + ".spotArmed = id\n";
    s += "\tif not " + g + ".typingLoaded then return end\n";
    s += "\tlocal who = " + g + ".Player()\n";
    s += "\tif who ~= nil and who.id ~= nil and ActionMapManager.SetActionListener ~= nil then\n";
    s += "\t\tpcall(function() ActionMapManager.SetActionListener(" + g + ".typingToggleMap, who.id) end)\n";
    s += "\tend\n";
    s += "\tif not " + g + ".active then " + g + ".TypingMap(" + g + ".typingToggleMap, true) end\n";
    s += "\t" + g + ".Log(\"BRIDGE quest \" .. id .. \" leave key armed\")\n";
    s += "end\n";
    s += "\n";
    s += "function " + g + ".SpotDisarm(id)\n";
    s += "\tif " + g + ".spotArmed ~= id then return end\n";
    s += "\t" + g + ".spotArmed = nil\n";
    s += "\tif " + g + ".typingLoaded and not " + g + ".active then " + g + ".TypingMap(" + g + ".typingToggleMap, false) end\n";
    s += "\t" + g + ".Log(\"BRIDGE quest \" .. id .. \" leave key off\")\n";
    s += "end\n";
    s += "\n";
    // A spot entity at a point: the AudioTriggerSpot class, which has no shape and is what the
    // probes placed things against and played sounds from. Swept away after a while.
    s += "function " + g + ".SpotEntity(name, x, y, z)\n";
    s += "\tlocal ok, ent = pcall(System.SpawnEntity, {\n";
    s += "\t\tclass = \"AudioTriggerSpot\", name = name, position = { x = x, y = y, z = z },\n";
    s += "\t\torientation = { x = 0, y = 1, z = 0 },\n";
    s += "\t\tproperties = { audioTriggerPlayTriggerName = \"\", audioTriggerStopTriggerName = \"\", bEnabled = 1,\n";
    s += "\t\t\tbPlayRandom = 0, fRadiusRandom = 0, eiSoundObstructionType = 1 },\n";
    s += "\t})\n";
    s += "\tif not ok or ent == nil then\n";
    s += "\t\t" + g + ".Log(\"BRIDGE spot entity could not be made: \" .. tostring(ent))\n";
    s += "\t\treturn nil\n";
    s += "\tend\n";
    s += "\ttable.insert(" + g + ".spotEnts, { ent = ent, at = " + g + ".Now() })\n";
    s += "\tif #" + g + ".spotEnts == 1 then Script.SetTimer(30000, " + g + ".SpotSweep) end\n";
    s += "\treturn ent\n";
    s += "end\n";
    s += "\n";
    s += "function " + g + ".SpotSweep()\n";
    s += "\tlocal keep = {}\n";
    s += "\tfor _, e in ipairs(" + g + ".spotEnts) do\n";
    s += "\t\tif " + g + ".Now() - e.at >= 25 then\n";
    s += "\t\t\tpcall(function() e.ent:FreeSlot(1) end)\n";
    s += "\t\t\tpcall(function() System.RemoveEntity(e.ent.id) end)\n";
    s += "\t\telse\n";
    s += "\t\t\tkeep[#keep + 1] = e\n";
    s += "\t\tend\n";
    s += "\tend\n";
    s += "\t" + g + ".spotEnts = keep\n";
    s += "\tif #keep > 0 then Script.SetTimer(30000, " + g + ".SpotSweep) end\n";
    s += "end\n";
    s += "\n";
    // A sound at an entity: the audio trigger by name, on the entity's own proxy.
    s += "function " + g + ".Sound(ent, name)\n";
    s += "\tif ent == nil or name == nil or name == \"\" then return false end\n";
    s += "\tlocal ok, err = pcall(function()\n";
    s += "\t\tent:ExecuteAudioTrigger(Sound.GetAudioTriggerID(name), ent:GetDefaultAuxAudioProxyID())\n";
    s += "\tend)\n";
    s += "\tif not ok then " + g + ".Log(\"BRIDGE sound \" .. name .. \" raised \" .. tostring(err)) end\n";
    s += "\treturn ok\n";
    s += "end\n";
    s += "\n";
    // Ground height at a point when the engine will say, else the height given.
    s += "function " + g + ".Ground(x, y, z)\n";
    s += "\tlocal h = nil\n";
    s += "\tpcall(function() h = System.GetTerrainElevation({ x = x, y = y, z = z }) end)\n";
    s += "\tif type(h) ~= \"number\" or h <= 0 then return z end\n";
    s += "\tif math.abs(h - z) > 6 then return z end\n";
    s += "\treturn h\n";
    s += "end\n";
    s += "\n";
    s += "function " + g + ".WatchSpot(id, q, pp)\n";
    s += "\tlocal f = " + g + ".state.quests[id] or {}\n";
    s += "\tlocal d = math.sqrt((pp.x - q.spot[1]) ^ 2 + (pp.y - q.spot[2]) ^ 2)\n";
    s += "\tlocal p = " + g + ".Player()\n";
    s += "\tlocal have = " + g + ".Count(p, q.item)\n";
    s += "\tif not f.left and not f.done then\n";
    s += "\t\tif d <= " + g + ".spotMetres then\n";
    s += "\t\t\tif not " + g + ".HourIn(q.hours) then\n";
    s += "\t\t\t\t" + g + ".SpotDisarm(id)\n";
    s += "\t\t\t\t" + g + ".SpotHint(id, \"Not yet. This is to be done \" .. " + g + ".HourWords(q.hours) .. \".\")\n";
    s += "\t\t\telseif have <= 0 then\n";
    s += "\t\t\t\t" + g + ".SpotDisarm(id)\n";
    s += "\t\t\t\tif (" + g + ".spotWas[id] or 0) > 0 then\n";
    s += "\t\t\t\t\t" + g + ".SpotLeft(id, q, d, \"dropped\")\n";
    s += "\t\t\t\telse\n";
    s += "\t\t\t\t\t" + g + ".SpotHint(id, \"You have no \" .. q.thing .. \" to leave here.\")\n";
    s += "\t\t\t\tend\n";
    s += "\t\t\telse\n";
    s += "\t\t\t\t" + g + ".SpotArm(id)\n";
    s += "\t\t\t\t" + g + ".SpotHint(id, \"Press F9 to leave the \" .. q.thing .. \" here, or drop it.\")\n";
    s += "\t\t\tend\n";
    s += "\t\telse\n";
    s += "\t\t\t" + g + ".SpotDisarm(id)\n";
    s += "\t\t\t" + g + ".spotHinted[id] = nil\n";
    s += "\t\tend\n";
    s += "\t\t" + g + ".spotWas[id] = have\n";
    s += "\tend\n";
    // The cue, due so many seconds after the thing went down.
    s += "\tlocal due = " + g + ".cueDue[id]\n";
    s += "\tif due ~= nil and " + g + ".Now() >= due then\n";
    s += "\t\t" + g + ".cueDue[id] = nil\n";
    s += "\t\t" + g + ".Cue(id, q, pp, d)\n";
    s += "\tend\n";
    s += "\tif q.kind == \"return\" then " + g + ".WatchGrave(id, q, pp, f, d, have) end\n";
    s += "end\n";
    s += "\n";
    // The leave key: every one of the thing out of Henry's pocket and on to the ground at
    // the spot. A piece the engine will not place is deleted, so the errand is not stuck on
    // a thing that cannot be put down; the log says which it was.
    s += "function " + g + ".SpotKey()\n";
    s += "\tlocal id = " + g + ".spotArmed\n";
    s += "\tlocal q = id ~= nil and " + g + ".quests[id] or nil\n";
    s += "\tif q == nil then return end\n";
    s += "\tlocal p = " + g + ".Player()\n";
    s += "\tlocal have = " + g + ".Count(p, q.item)\n";
    s += "\tif have <= 0 then return end\n";
    s += "\tlocal z = " + g + ".Ground(q.spot[1], q.spot[2], q.spot[3])\n";
    s += "\tlocal spot = " + g + ".SpotEntity(\"moonshine_leave_\" .. id, q.spot[1], q.spot[2], z)\n";
    s += "\tlocal placed = 0\n";
    s += "\tif spot ~= nil then\n";
    s += "\t\tfor i = 1, have do\n";
    s += "\t\t\tlocal ok, err = pcall(function()\n";
    s += "\t\t\t\tlocal wuid = p.inventory:FindItem(q.item)\n";
    s += "\t\t\t\tif wuid == nil then error(\"no item found\") end\n";
    s += "\t\t\t\tp.human:PlaceItem(wuid, spot.id, false)\n";
    s += "\t\t\tend)\n";
    s += "\t\t\tif not ok then " + g + ".Log(\"BRIDGE quest \" .. id .. \" PlaceItem raised \" .. tostring(err)); break end\n";
    s += "\t\t\tplaced = placed + 1\n";
    s += "\t\tend\n";
    s += "\tend\n";
    s += "\tlocal rest = " + g + ".Count(p, q.item)\n";
    s += "\tif rest > 0 then\n";
    s += "\t\tpcall(function() p.inventory:DeleteItemOfClass(q.item, rest) end)\n";
    s += "\t\t" + g + ".Log(\"BRIDGE quest \" .. id .. \" placed \" .. placed .. \", \" .. rest .. \" would not place and were taken\")\n";
    s += "\tend\n";
    s += "\tlocal pp = p:GetWorldPos()\n";
    s += "\tlocal d = math.sqrt((pp.x - q.spot[1]) ^ 2 + (pp.y - q.spot[2]) ^ 2)\n";
    s += "\t" + g + ".SpotLeft(id, q, d, \"placed \" .. placed)\n";
    s += "end\n";
    s += "\n";
    s += "function " + g + ".SpotLeft(id, q, d, how)\n";
    s += "\t" + g + ".Log(\"BRIDGE quest \" .. id .. \" left \" .. q.thing .. \" at \" .. string.format(\"%.1f\", d) .. \"m (\" .. how .. \")\")\n";
    s += "\t" + g + ".SpotDisarm(id)\n";
    s += "\tpcall(" + g + ".StateQuest, id, \"left\")\n";
    s += "\t" + g + ".Token(q)\n";
    s += "\tif q.cue ~= nil then " + g + ".cueDue[id] = " + g + ".Now() + " + g + ".cueDelaySeconds end\n";
    // A return's curse is lifted by putting the goods back.
    s += "\tlocal f = " + g + ".state.quests[id] or {}\n";
    s += "\tif f.cursed then " + g + ".Cure(id, \"the goods are back\") end\n";
    s += "end\n";
    s += "\n";
    // The sound from the dark: a spot cueMetres off the place, on a side picked at random,
    // at ground height; heard if Henry is still within cueHeardMetres of the place.
    s += "function " + g + ".Cue(id, q, pp, d)\n";
    s += "\tlocal a = math.random() * 2 * math.pi\n";
    s += "\tlocal x = q.spot[1] + math.cos(a) * " + g + ".cueMetres\n";
    s += "\tlocal y = q.spot[2] + math.sin(a) * " + g + ".cueMetres\n";
    s += "\tlocal z = " + g + ".Ground(x, y, q.spot[3]) + 1\n";
    s += "\tlocal ent = " + g + ".SpotEntity(\"moonshine_cue_\" .. id, x, y, z)\n";
    s += "\tlocal played = " + g + ".Sound(ent, q.cue)\n";
    s += "\t" + g + ".Log(\"BRIDGE quest \" .. id .. \" cue \" .. q.cue .. (played and \" played\" or \" failed\") .. \" with Henry at \" .. string.format(\"%.1f\", d) .. \"m\")\n";
    s += "\tif played and d <= " + g + ".cueHeardMetres then pcall(" + g + ".StateQuest, id, \"heard\") end\n";
    s += "end\n";
    s += "\n";
    // A return's grave: see WatchSpot.
    s += "function " + g + ".WatchGrave(id, q, pp, f, d, have)\n";
    s += "\tlocal p = " + g + ".Player()\n";
    s += "\tlocal now = " + g + ".Now()\n";
    s += "\tif d <= " + g + ".coughMetres then\n";
    s += "\t\tif " + g + ".windAt[id] == nil then\n";
    s += "\t\t\t" + g + ".windAt[id] = now\n";
    s += "\t\t\tlocal ent = " + g + ".SpotEntity(\"moonshine_wind_\" .. id, q.spot[1], q.spot[2], q.spot[3] + 1)\n";
    s += "\t\t\t" + g + ".Sound(ent, " + g + ".windSound)\n";
    s += "\t\t\tif not f.near then pcall(" + g + ".StateQuest, id, \"near\") end\n";
    s += "\t\tend\n";
    s += "\t\tif now - " + g + ".coughAt >= " + g + ".coughSeconds then\n";
    s += "\t\t\t" + g + ".coughAt = now\n";
    s += "\t\t\t" + g + ".Sound(p, " + g + ".coughSound)\n";
    s += "\t\tend\n";
    s += "\telseif d > 2 * " + g + ".coughMetres then\n";
    s += "\t\t" + g + ".windAt[id] = nil\n";
    s += "\tend\n";
    // The robbing: the level's own pieces, counted in Henry's pocket only while he is at the
    // grave, against what he came with.
    s += "\tif q.loot ~= nil then\n";
    s += "\t\tif d <= " + g + ".lootMetres then\n";
    s += "\t\t\tlocal n = 0\n";
    s += "\t\t\tfor _, class in ipairs(q.loot) do n = n + " + g + ".Count(p, class) end\n";
    s += "\t\t\tlocal was = " + g + ".lootSeen[id]\n";
    s += "\t\t\tif was ~= nil and n > was and not f.looted then " + g + ".Looted(id, q) end\n";
    s += "\t\t\t" + g + ".lootSeen[id] = n\n";
    s += "\t\telse\n";
    s += "\t\t\t" + g + ".lootSeen[id] = nil\n";
    s += "\t\tend\n";
    s += "\tend\n";
    // The curse: laid on the way out, lifted by the clock.
    s += "\tif f.cursed then\n";
    s += "\t\tif f.cursedAt ~= nil and " + g + ".WorldTime() - f.cursedAt >= " + g + ".curseHours * 3600 then\n";
    s += "\t\t\t" + g + ".Cure(id, \"two days are gone\")\n";
    s += "\t\tend\n";
    s += "\telseif not f.cured and f.near and d > " + g + ".curseMetres and (f.looted or (have > 0 and not f.left)) then\n";
    s += "\t\t" + g + ".Curse(id, f.looted and \"the grave robbed\" or \"the goods carried off\")\n";
    s += "\tend\n";
    s += "end\n";
    s += "\n";
    s += "function " + g + ".Looted(id, q)\n";
    s += "\t" + g + ".Log(\"BRIDGE quest \" .. id .. \" grave robbed\")\n";
    s += "\tpcall(" + g + ".StateQuest, id, \"looted\")\n";
    s += "\tlocal p = " + g + ".Player()\n";
    s += "\tpcall(function() Particle.SpawnEffect(" + g + ".dustEffect, { x = q.spot[1], y = q.spot[2], z = q.spot[3] }, { x = 0, y = 0, z = 1 }, 1.0) end)\n";
    s += "\tlocal ent = " + g + ".SpotEntity(\"moonshine_bats_\" .. id, q.spot[1], q.spot[2], q.spot[3] + 1.5)\n";
    s += "\tif ent ~= nil then\n";
    s += "\t\tpcall(function() ent:LoadParticleEffect(1, " + g + ".batsEffect, { SpeedScale = 1.5 }) end)\n";
    s += "\t\t" + g + ".Sound(ent, " + g + ".batsSound)\n";
    s += "\tend\n";
    s += "\t" + g + ".Sound(p, " + g + ".coughSound)\n";
    s += "\t" + g + ".coughAt = " + g + ".Now()\n";
    s += "end\n";
    s += "\n";
    s += "function " + g + ".Curse(id, why)\n";
    s += "\tlocal p = " + g + ".Player()\n";
    s += "\tlocal ok, inst = pcall(function() return p.soul:AddBuff(" + g + ".curseBuff) end)\n";
    s += "\tif ok then " + g + ".curseInst = inst end\n";
    s += "\t" + g + ".Log(\"BRIDGE quest \" .. id .. \" cursed, \" .. why .. (ok and \"\" or (\" (buff raised \" .. tostring(inst) .. \")\")))\n";
    s += "\tpcall(" + g + ".StateQuest, id, \"cursed\")\n";
    s += "\t" + g + ".Sound(p, " + g + ".coughSound)\n";
    s += "\tpcall(function() Game.SendInfoText(\"Something has come out of that place with you.\", true, nil, 5) end)\n";
    s += "end\n";
    s += "\n";
    s += "function " + g + ".Cure(id, why)\n";
    s += "\tlocal p = " + g + ".Player()\n";
    s += "\tif " + g + ".curseInst ~= nil then pcall(function() p.soul:RemoveBuff(" + g + ".curseInst) end) end\n";
    s += "\t" + g + ".curseInst = nil\n";
    s += "\tpcall(function() p.soul:RemoveAllBuffsByGuid(" + g + ".curseBuff) end)\n";
    s += "\t" + g + ".Log(\"BRIDGE quest \" .. id .. \" curse lifted, \" .. why)\n";
    s += "\tpcall(" + g + ".StateQuest, id, \"cured\")\n";
    s += "end\n";
    s += "\n";
    // A mind: the one looked in on dead (the errand ends there), and the due global fresh
    // as the game's clock moves, so the target's menu offers the visit when it is time.
    s += "function " + g + ".WatchMind(id, q)\n";
    s += "\tlocal dead = false\n";
    s += "\tpcall(function()\n";
    s += "\t\tlocal e = System.GetEntityByName(q.target)\n";
    s += "\t\tif e.actor ~= nil then dead = e.actor:IsDead() end\n";
    s += "\tend)\n";
    s += "\tif dead then\n";
    s += "\t\t" + g + ".Log(\"BRIDGE quest \" .. id .. \" target \" .. q.target .. \" dead\")\n";
    s += "\t\tpcall(" + g + ".StateQuest, id, \"target_dead\")\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    s += "\tlocal due = " + g + ".MindDue(id) and 1 or 0\n";
    s += "\tif " + g + ".mindDue == nil then " + g + ".mindDue = {} end\n";
    s += "\tif " + g + ".mindDue[id] ~= due then\n";
    s += "\t\t" + g + ".mindDue[id] = due\n";
    s += "\t\tpcall(function() Variables.SetGlobal(q.dueVar, due) end)\n";
    s += "\t\t" + g + ".Log(\"BRIDGE quest \" .. id .. \" visit \" .. (due == 1 and \"due\" or \"not yet due\"))\n";
    s += "\tend\n";
    s += "end\n";
    s += "\n";
    // An ask: only the one who knows dead (the truth goes with them, and the errand ends
    // in the giver's dead_reply). Knocking them down is not asking, so nothing else is
    // watched; the telling is the menu's own handover.
    s += "function " + g + ".WatchAsk(id, q)\n";
    s += "\tlocal dead = false\n";
    s += "\tpcall(function()\n";
    s += "\t\tlocal e = System.GetEntityByName(q.target)\n";
    s += "\t\tif e.actor ~= nil then dead = e.actor:IsDead() end\n";
    s += "\tend)\n";
    s += "\tif dead then\n";
    s += "\t\t" + g + ".Log(\"BRIDGE quest \" .. id .. \" target \" .. q.target .. \" dead\")\n";
    s += "\t\tpcall(" + g + ".StateQuest, id, \"target_dead\")\n";
    s += "\tend\n";
    s += "end\n";
    s += "\n";
    // Whether a quest's target is dead, for the watches that only end there.
    s += "function " + g + ".TargetDead(q)\n";
    s += "\tlocal dead = false\n";
    s += "\tpcall(function()\n";
    s += "\t\tlocal e = System.GetEntityByName(q.target)\n";
    s += "\t\tif e.actor ~= nil then dead = e.actor:IsDead() end\n";
    s += "\tend)\n";
    s += "\treturn dead\n";
    s += "end\n";
    s += "\n";
    // An ambush (set-aside-stories.md §5): the one who knows dead ends it as an ask's does.
    // Otherwise, once the directions are given and until the trap is sprung, Henry within
    // ambushMetres of the place with the directions on him: in the hour, the store learns
    // it (ambushed) and the token goes into his pocket for the graph's trigger, which sets
    // the three on him; outside the hour, a word on screen that it is not the time, since a
    // place at midnight is the directions' whole point. Nothing more is watched: the fight
    // is the graph's, and the truth is the menu's own handover.
    s += g + ".ambushMetres = " + std::to_string(kAmbushMetres) + "\n";
    s += "function " + g + ".WatchAmbush(id, q, pp)\n";
    s += "\tif " + g + ".TargetDead(q) then\n";
    s += "\t\t" + g + ".Log(\"BRIDGE quest \" .. id .. \" target \" .. q.target .. \" dead\")\n";
    s += "\t\tpcall(" + g + ".StateQuest, id, \"target_dead\")\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    s += "\tlocal f = " + g + ".state.quests[id] or {}\n";
    s += "\tif not f.directed or f.ambushed or q.spot == nil then return end\n";
    s += "\tlocal d = math.sqrt((pp.x - q.spot[1]) ^ 2 + (pp.y - q.spot[2]) ^ 2)\n";
    s += "\tif d > " + g + ".ambushMetres then " + g + ".spotHinted[id] = nil; return end\n";
    s += "\tif not " + g + ".HourIn(q.hours) then\n";
    s += "\t\t" + g + ".SpotHint(id, \"Nobody here. The directions said \" .. " + g + ".HourWords(q.hours) .. \".\")\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    s += "\tif " + g + ".Count(" + g + ".Player(), q.item) < 1 then\n";
    s += "\t\t" + g + ".SpotHint(id, \"Nobody here. You have not got the directions on you.\")\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    s += "\t" + g + ".Log(\"BRIDGE quest \" .. id .. \" ambushed at \" .. string.format(\"%.1f\", d) .. \"m\")\n";
    s += "\tpcall(" + g + ".StateQuest, id, \"ambushed\")\n";
    s += "\t" + g + ".Token(q)\n";
    s += "end\n";
    s += "\n";
    // A buy: the holder dead (the charm is on the body, and bringing it is the retrieve's
    // stolen), and the carry global fresh as Henry's pockets change -- sold the hood, got
    // it back -- so the menu's pay row is there exactly when he can meet the terms.
    s += "function " + g + ".WatchBuy(id, q)\n";
    s += "\tif " + g + ".TargetDead(q) then\n";
    s += "\t\t" + g + ".Log(\"BRIDGE quest \" .. id .. \" target \" .. q.target .. \" dead\")\n";
    s += "\t\tpcall(" + g + ".StateQuest, id, \"target_dead\")\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    s += "\tlocal carrying = " + g + ".Count(" + g + ".Player(), q.carry) > 0 and 1 or 0\n";
    s += "\tif " + g + ".carryWas == nil then " + g + ".carryWas = {} end\n";
    s += "\tif " + g + ".carryWas[id] ~= carrying then\n";
    s += "\t\t" + g + ".carryWas[id] = carrying\n";
    s += "\t\tpcall(function() Variables.SetGlobal(q.carryVar, carrying) end)\n";
    s += "\t\t" + g + ".Log(\"BRIDGE quest \" .. id .. \" carrying the \" .. tostring(q.carryName) .. \": \" .. tostring(carrying == 1))\n";
    s += "\tend\n";
    s += "end\n";
    s += "\n";
    // A find takes its count (quest.count, 1 for everything else), and settles for what
    // Henry has when he has fewer: the app only says complete when he has enough, so a
    // shortfall here is a save that changed under it, and the errand still closes.
    // A find that wants them fresh (quest.minHealth) counts and takes by freshness, the
    // freshest first, so the giver gets the ones from the meadow and Henry keeps the rest.
    s += "function " + g + ".TakeBack(quest)\n";
    s += "\tlocal p = " + g + ".Player()\n";
    s += "\tlocal want = quest.count or 1\n";
    s += "\tlocal min = quest.minHealth or 0\n";
    s += "\tlocal have = " + g + ".CountFresh(p, quest.item, min)\n";
    s += "\tif have >= 0 and have < want then\n";
    s += "\t\t" + g + ".Log(\"BRIDGE quest complete wanted \" .. want .. \" but the player has \" .. have)\n";
    s += "\t\twant = have\n";
    s += "\tend\n";
    s += "\tif want < 1 then return end\n";
    s += "\tlocal moved = 0\n";
    s += "\tif min > 0 then\n";
    s += "\t\tmoved = " + g + ".TakeFresh(p, " + g + ".npc, quest.item, min, want)\n";
    s += "\t\tif moved > 0 then " + g + ".Log(\"BRIDGE quest complete moved \" .. moved .. \" fresh (at least \" .. min .. \") to the giver\") end\n";
    s += "\t\tif moved >= want then return end\n";
    s += "\t\twant = want - moved\n";
    s += "\t\tmoved = 0\n";
    s += "\tend\n";
    s += "\tmoved = moved + " + g + ".MoveN(p, " + g + ".npc, quest.item, want)\n";
    s += "\tif moved < want then\n";
    s += "\t\tlocal left = want - moved\n";
    s += "\t\tlocal ok, got = pcall(function()\n";
    s += "\t\t\treturn p.inventory:DeleteItemOfClass(quest.item, left)\n";
    s += "\t\tend)\n";
    s += "\t\tif ok then\n";
    s += "\t\t\tpcall(function() Game.ShowItemsTransfer(quest.item, -left) end)\n";
    s += "\t\t\t" + g + ".Log(\"BRIDGE quest complete took \" .. left .. \" back by deleting\")\n";
    s += "\t\telse\n";
    s += "\t\t\t" + g + ".Log(\"BRIDGE quest complete could not take the item: \" .. tostring(got))\n";
    s += "\t\tend\n";
    s += "\tend\n";
    s += "\tif moved > 0 then\n";
    s += "\t\t" + g + ".Log(\"BRIDGE quest complete moved \" .. moved .. \" to the giver\")\n";
    s += "\tend\n";
    s += "end\n";
    s += "\n";
    // The giver's groschen, for a sum too small for any money_change row. Money is an
    // item class counted in tenths of a groschen (DialogUtils.ShopMoneyTransaction's
    // deciGroschen = wholeGroschen * 10, which is why the pay branch multiplies too), made
    // with inventory:CreateItem(class, health, amount) as player.lua makes it: of the
    // eight calls scripts/autopilot/money_probe.lua tried, that one and a shop
    // transaction were the two that moved the count, and ItemManager.CreateItem raised.
    // Announced with the same "+N Groschen" notice the graph's MoneyReward shows.
    // A reward that is a thing (rewardClass, from npc_quest_lines reward_class) is made in
    // his pocket here too -- one, even if he has one already, since it is his pay and not
    // a quest's token.
    s += "function " + g + ".PayReward(quest)\n";
    s += "\tif quest.rewardClass ~= nil and quest.rewardClass ~= \"\" then\n";
    s += "\t\t" + g + ".GiveClass(quest.rewardClass, 1, \"reward\", true)\n";
    s += "\tend\n";
    s += "\tif quest.reward == nil or quest.reward <= 0 then return end\n";
    s += "\t" + g + ".PayMoney(quest.reward, \"complete paid\")\n";
    s += "end\n";
    s += "\n";
    // The same purse, for any sum the mod puts in it: the reward, a buy's fee at accept,
    // the lie's extra.
    s += "function " + g + ".PayMoney(groschen, why)\n";
    s += "\tif groschen == nil or groschen <= 0 then return end\n";
    s += "\tlocal p = " + g + ".Player()\n";
    s += "\tlocal money = ItemUtils.itemIDs.money\n";
    s += "\tlocal before = 0\n";
    s += "\tpcall(function() before = p.inventory:GetCountOfClass(money) end)\n";
    s += "\tlocal ok, err = pcall(function()\n";
    s += "\t\tp.inventory:CreateItem(money, 100, groschen * 10)\n";
    s += "\t\tGame.ShowItemsTransfer(money, groschen * 10)\n";
    s += "\tend)\n";
    s += "\tif ok then\n";
    s += "\t\tlocal after = before\n";
    s += "\t\tpcall(function() after = p.inventory:GetCountOfClass(money) end)\n";
    s += "\t\t" + g + ".Log(\"BRIDGE quest \" .. why .. \" \" .. groschen .. \" groschen, purse \"\n";
    s += "\t\t\t.. tostring(before / 10) .. \" -> \" .. tostring(after / 10))\n";
    s += "\telse\n";
    s += "\t\t" + g + ".Log(\"BRIDGE quest \" .. why .. \" could not pay: \" .. tostring(err))\n";
    s += "\tend\n";
    s += "end\n";
    s += "\n";
    // One directive. handover is all inventory and asks nothing of the graph; accept
    // and complete are journal moments, so they queue the beat that fires the port,
    // and complete also collects the item first.
    s += "function " + g + ".QuestCommand(id, command)\n";
    s += "\tlocal quest = " + g + ".quests[id]\n";
    s += "\tif quest == nil then\n";
    s += "\t\t" + g + ".Log(\"BRIDGE quest unknown id=\" .. tostring(id) .. \" \" .. tostring(command))\n";
    // Said on the screen as well as in the log: an app offering an errand this build has
    // never heard of is an app and a mod from different deals (notes.md, 5 Oct 2026 --
    // three errands accepted and never in the journal), and the player should know why
    // nothing happened rather than find out from the journal later.
    s += "\t\tpcall(function()\n";
    s += "\t\t\tif Game ~= nil and Game.SendInfoText ~= nil then\n";
    s += "\t\t\t\tGame.SendInfoText(\"[Moonshine] The game's mod does not know this errand (\" .. tostring(id)\n";
    s += "\t\t\t\t\t.. \"): the app and the mod were built from different data. Send the mod again (scripts/build_mod.sh).\", true, nil, 8)\n";
    s += "\t\t\tend\n";
    s += "\t\tend)\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    s += "\t" + g + ".Log(\"BRIDGE quest \" .. id .. \" \" .. command .. \" -- \" .. quest.name)\n";
    s += "\tpcall(" + g + ".StateQuest, id, command)\n";
    // The giver's carrying (FastTravelArm): nothing for the store or the book.
    s += "\tif command == \"fast_travel\" then\n";
    s += "\t\tif quest.fastTravel ~= nil then " + g + ".FastTravelArm(id, quest) end\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    s += "\tif command == \"handover\" then\n";
    s += "\t\t" + g + ".Handover(quest)\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    // The giver's word on a dispute is a fact for the store and the holder's menu, and no
    // journal moment: nothing plays.
    s += "\tif command == \"apologise\" or command == \"refuse_apology\" or command == \"broker\" then return end\n";
    // A retrieve's complete takes the thing back and pays; a deliver's letter is already
    // with the receiver, so it only pays. lie is a deliver's paid ending with the letter
    // still in Henry's pocket (the writer cannot tell), keep its unpaid one.
    // A travel's complete is the truth told and paid; lie is its paid ending too, and
    // quiet -- Henry went and will not say -- its unpaid one. Nothing changes hands.
    // A beat's, a pray's, a mind's and an ask's complete is the truth told and paid, their
    // lie the paid ending on a false word; confess -- the man is dead, and Henry says so --
    // is a beat's, a mind's or an ask's unpaid one. Nothing changes hands: they have no
    // item. A leave's and a return's are the travel's three, and their thing is already on
    // the ground where it was left, so nothing is taken back (a lie with it still in his
    // pocket leaves it there, as a deliver's does the letter).
    // An ambush's directions are Henry's to keep (the giver has no use for a false way), so
    // nothing is taken back; its three rows are the ask's. A buy's complete takes the charm
    // back and pays as a retrieve's does; its lie -- the price was more than it was -- is
    // paid, with the fee again on top, which is what the lie is for.
    // An ask whose word is carried on pays at the `onward` beat -- the giver told, paying,
    // and sending Henry on -- and its complete, fired by the one sent to's own hand row
    // (QuestCarried), pays nothing again. The one sent to is granted their menu's metarole
    // here too, for a soul an old save made before the mod.
    s += "\tif command == \"onward\" then\n";
    s += "\t\t" + g + ".PayReward(quest)\n";
    s += "\t\t" + g + ".GrantOnwardByName(quest)\n";
    s += "\tend\n";
    // Given up: nothing paid, nothing taken back (a letter or a thing stays where it is);
    // the beat alone, which fails the quest in the book with its Abandoned line.
    s += "\tif command == \"abandon\" then " + g + ".Log(\"BRIDGE quest \" .. id .. \" given up; nothing paid\") end\n";
    // The giver has named the one who can let Henry in (In the Stocks' Benesh naming
    // Elsbeth; the app's `named` directive): the token for the graph -- the journal's
    // Named line and the optional objective with her marker -- her menu's role, and the
    // watch for the chest. No beat: the graph hears it through the token.
    s += "\tif command == \"named\" then\n";
    s += "\t\tif quest.helper ~= nil then\n";
    s += "\t\t\t" + g + ".Token(quest)\n";
    s += "\t\t\t" + g + ".GrantHelperByName(quest)\n";
    s += "\t\t\t" + g + ".WatchArm(\"named\")\n";
    s += "\t\tend\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    // A dictated letter written (the app's `wrote_<digits>`, Words Must Not Fail): the store
    // has the digits (StateQuest, above), the reading's globals and the quest's item follow
    // them (QuestFlags), the Document of that combination goes into Henry's pocket as the
    // letter would have at accept, and the token tells the graph (log_written). No beat.
    s += "\tif string.match(command, \"^wrote_%d+$\") then\n";
    s += "\t\tif quest.dictated then\n";
    s += "\t\t\t" + g + ".QuestFlags()\n";
    s += "\t\t\t" + g + ".GiveLetter(quest)\n";
    s += "\t\t\tif quest.token ~= nil then " + g + ".Token(quest) end\n";
    s += "\t\telse\n";
    s += "\t\t\t" + g + ".Log(\"BRIDGE quest \" .. id .. \" is not dictated; \" .. command .. \" ignored\")\n";
    s += "\t\tend\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    s += "\tif command == \"complete\" then\n";
    s += "\t\tlocal f = " + g + ".state.quests[id] or {}\n";
    // A denounced bedside's complete is Henry's report with nothing in hand: nothing taken
    // back and nothing paid, and the graph's complete_denounced writes the journal.
    s += "\t\tif f.how == \"denounced\" then\n";
    s += "\t\t\t" + g + ".Log(\"BRIDGE quest \" .. id .. \" told as denounced; nothing came back, nothing paid\")\n";
    s += "\t\telse\n";
    s += "\t\tif quest.kind ~= \"deliver\" and quest.kind ~= \"leave\" and quest.kind ~= \"return\" and quest.kind ~= \"ambush\"\n";
    s += "\t\t\t\tand quest.item ~= \"\" then\n";
    s += "\t\t\t" + g + ".TakeBack(quest)\n";
    s += "\t\tend\n";
    // A find's keepsake, brought back with the things, goes to the giver too (the store's
    // `keepsake` stays set: it is what the thanks were for).
    s += "\t\tif quest.keepsake ~= nil and " + g + ".Count(" + g + ".Player(), quest.keepsake) > 0 then\n";
    s += "\t\t\tlocal moved = " + g + ".MoveOne(" + g + ".Player(), " + g + ".npc, quest.keepsake)\n";
    s += "\t\t\tif moved < 1 then pcall(function() " + g + ".Player().inventory:DeleteItemOfClass(quest.keepsake, 1) end) end\n";
    s += "\t\t\t" + g + ".Log(\"BRIDGE quest \" .. id .. \" keepsake handed back (moved \" .. moved .. \")\")\n";
    s += "\t\tend\n";
    s += "\t\tif quest.onward ~= nil and f.onward then\n";
    s += "\t\t\t" + g + ".Log(\"BRIDGE quest \" .. id .. \" word carried on; paid at the telling\")\n";
    s += "\t\telse\n";
    s += "\t\t\t" + g + ".PayReward(quest)\n";
    s += "\t\tend\n";
    s += "\t\tend\n";
    s += "\tend\n";
    // A retrieve's lie -- the thing is in Henry's coat and he says the holder never had it
    // (the giver's lie_have row; In the Stocks) -- is the one lie that pays nothing: the
    // giver has nothing to pay for, and the thing stays where it is.
    s += "\tif command == \"lie\" then\n";
    s += "\t\tif quest.kind == \"buy\" then\n";
    s += "\t\t\tif quest.item ~= \"\" then " + g + ".TakeBack(quest) end\n";
    s += "\t\t\t" + g + ".PayMoney(quest.pay, \"lie pocketed\")\n";
    s += "\t\tend\n";
    s += "\t\tif quest.kind == \"retrieve\" then\n";
    s += "\t\t\t" + g + ".Log(\"BRIDGE quest \" .. id .. \" lie with the thing kept; nothing paid\")\n";
    s += "\t\telse\n";
    s += "\t\t\t" + g + ".PayReward(quest)\n";
    s += "\t\tend\n";
    s += "\tend\n";
    // A buy's accept puts the giver's thing and the fee in Henry's hands: the carry item
    // (made, as a letter is) and pay groschen, which the holder's row takes off him again.
    s += "\tif command == \"accept\" then\n";
    s += "\t\tif quest.target ~= \"\" and quest.kind ~= \"travel\" then " + g + ".GrantHolderByName(quest) end\n";
    // A dictated letter is not written yet at accept (it comes with `wrote_<digits>`), so
    // Henry gets nothing; the reading's globals start at nothing written.
    s += "\t\tif quest.kind == \"deliver\" and not quest.dictated then " + g + ".GiveLetter(quest) end\n";
    s += "\t\tif quest.dictated then " + g + ".QuestFlags() end\n";
    s += "\t\tif quest.kind == \"leave\" or quest.kind == \"return\" then " + g + ".GiveThing(quest, quest.count or 1) end\n";
    s += "\t\tif quest.kind == \"buy\" then\n";
    s += "\t\t\t" + g + ".GiveClass(quest.carry, 1, quest.carryName)\n";
    s += "\t\t\t" + g + ".PayMoney(quest.pay, \"accept advanced\")\n";
    s += "\t\tend\n";
    s += "\t\tif quest.kind == \"mind\" or quest.kind == \"ambush\" or quest.kind == \"buy\" then " + g + ".QuestFlags() end\n";
    // A find's keepsake goes down where the things grow the moment the errand is taken up,
    // so the objective's marker has something to sit on; the watch tries again if not.
    s += "\t\tif quest.kind == \"find\" and quest.keepsake ~= nil then " + g + ".LayKeepsake(id, \"accept\") end\n";
    s += "\t\tif quest.kind == \"travel\" or quest.kind == \"beat\" or quest.kind == \"pray\" or quest.kind == \"mind\"\n";
    s += "\t\t\t\tor quest.kind == \"ask\" or quest.kind == \"leave\" or quest.kind == \"return\"\n";
    s += "\t\t\t\tor quest.kind == \"ambush\" or quest.kind == \"buy\"\n";
    s += "\t\t\t\tor (quest.kind == \"find\" and quest.keepsake ~= nil)\n";
    s += "\t\t\t\tor (quest.kind == \"retrieve\" and quest.chest ~= nil) then\n";
    s += "\t\t\t" + g + ".WatchArm(\"accept\")\n";
    s += "\t\tend\n";
    s += "\tend\n";
    s += "\tif command == \"seen\" or command == \"beaten\" or command == \"prayed\" or command == \"checked\"\n";
    s += "\t\t\tor command == \"target_dead\" or command == \"left\" or command == \"heard\" or command == \"looted\"\n";
    s += "\t\t\tor command == \"cursed\" or command == \"cured\" or command == \"near\" or command == \"directed\"\n";
    s += "\t\t\tor command == \"ambushed\" or command == \"keepsake\" or command == \"keepsake_gone\"\n";
    s += "\t\t\tor command == \"rebuffed\" or command == \"letin\" or command == \"keyed\" or command == \"chest\"\n";
    s += "\t\t\tor command == \"denounced\" then return end\n";
    // Which complete: the one whose journal line says how the thing was won, when the
    // graph has a port for it. how is set at the handover; a complete with no handover
    // behind it is the player having got the thing some other way, which is stolen. A
    // deliver's lie and keep are their own completes when the journal has the line, and
    // the plain one when it has not.
    s += "\tlocal beat = command\n";
    s += "\tif command == \"lie\" or command == \"keep\" or command == \"quiet\" or command == \"confess\" then\n";
    s += "\t\tlocal f = " + g + ".state.quests[id] or {}\n";
    s += "\t\tf.how = (command == \"lie\") and \"lied\" or command == \"keep\" and \"kept\"\n";
    s += "\t\t\tor command == \"confess\" and \"dead\" or \"quiet\"\n";
    s += "\t\t" + g + ".state.quests[id] = f\n";
    s += "\t\tbeat = \"complete\"\n";
    s += "\t\tif " + g + ".questCodes[id .. \" complete_\" .. f.how] ~= nil then beat = \"complete_\" .. f.how end\n";
    s += "\t\t" + g + ".Log(\"BRIDGE quest \" .. id .. \" completes as \" .. beat)\n";
    s += "\telseif command == \"complete\" then\n";
    s += "\t\tlocal f = " + g + ".state.quests[id] or {}\n";
    // A beat with no handover behind its complete is the man knocked down (fought); a
    // retrieve's is the thing got some other way (stolen).
    s += "\t\tlocal how = f.how\n";
    s += "\t\tif how == nil and quest.kind == \"beat\" then\n";
    s += "\t\t\thow = f.beaten and \"fought\" or nil\n";
    // Kept in the store as a handover's how is, so the facts line reads :dk/fought.
    s += "\t\t\tif how ~= nil then f.how = how; " + g + ".state.quests[id] = f end\n";
    // Only a retrieve can be stolen: an ask's complete is the truth carried back, and a
    // travel's, find's or mind's plain complete has no handover to miss (the headless
    // matrix caught every told travel ending :d/stolen when this said `~= "ask"`).
    s += "\t\telseif how == nil and not f.handed and quest.kind == \"retrieve\" then\n";
    // Kept in the store too, so the dump reads :d/stolen and the app's done paragraph
    // knows the thing was never asked for.
    s += "\t\t\thow = \"stolen\"; f.how = how; " + g + ".state.quests[id] = f\n";
    s += "\t\tend\n";
    s += "\t\tif how ~= nil and " + g + ".questCodes[id .. \" complete_\" .. how] ~= nil then\n";
    s += "\t\t\tbeat = \"complete_\" .. how\n";
    s += "\t\tend\n";
    s += "\t\t" + g + ".Log(\"BRIDGE quest \" .. id .. \" completes as \" .. beat)\n";
    s += "\tend\n";
    s += "\tlocal code = " + g + ".questCodes[id .. \" \" .. beat]\n";
    s += "\tif code == nil then\n";
    s += "\t\t" + g + ".Log(\"BRIDGE quest has no beat for \" .. command)\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    s += "\ttable.insert(" + g + ".questQueue, code)\n";
    s += "\t" + g + ".QuestArm()\n";
    s += "end\n";
    s += "\n";
    // Takes the directives off the front of a turn and hands back the words. The
    // writer puts them first, three tokens each, as many as it owes, so this is a loop
    // and not a match. Words with nothing left are a turn that only did things.
    //
    // One more carries no arguments and is about the turn itself: @more says the reply
    // is still being written and these words are the text so far, so the line is
    // replaced rather than started again and the keyboard stays shut. It leaves its mark
    // in Moonshine.more.
    //
    // @memory is the app writing to the store (plan.md sections 6 and 8):
    //   @memory <soul> compact <rev> <n> <len>:<summary>   the first n entries become one
    //   @memory <soul> stat <name> <value>                  a derived stat
    //   @said <len>:<words>                                 what the player said to the app
    // The last is how the player's own words reach the store when they were spoken to
    // the app (a microphone, its console) rather than typed here: the mod only hears
    // what goes through its keyboard. The summary and the words are length-prefixed
    // rather than delimited, because they are prose with spaces in it and the words of
    // the turn follow them on the same line.
    //   @opt <row> <intent> <len>:<words>                   one row of the reply menu
    //   @heard <token>                                      the words name somebody new to Henry
    // A turn of @opt lines is the menu so far, whole each time (the @more rule: each
    // file replaces the last), and is taken through the reading hold since it shows
    // nothing itself. Rows are frozen once drawn: a later file fills empty rows only.
    s += "function " + g + ".Directives(words)\n";
    s += "\twhile true do\n";
    s += "\t\tlocal id, command, rest = string.match(words, \"^@quest%s+(%S+)%s+(%S+)%s*(.*)$\")\n";
    s += "\t\tif id ~= nil then\n";
    s += "\t\t\tpcall(function() " + g + ".QuestCommand(id, command) end)\n";
    s += "\t\t\twords = rest\n";
    s += "\t\telseif string.match(words, \"^@opt%s\") then\n";
    s += "\t\t\tlocal row, intent, len, tail = string.match(words, \"^@opt%s+(%d+)%s+(%S+)%s+(%d+):(.*)$\")\n";
    s += "\t\t\tif row == nil then\n";
    s += "\t\t\t\t" + g + ".Log(\"BRIDGE opt not understood: \" .. string.sub(words, 1, 80))\n";
    s += "\t\t\t\treturn \"\"\n";
    s += "\t\t\tend\n";
    s += "\t\t\tlen = tonumber(len)\n";
    s += "\t\t\t" + g + ".optTurn = true\n";
    s += "\t\t\tpcall(function() " + g + ".Option(tonumber(row), intent, string.sub(tail, 1, len)) end)\n";
    s += "\t\t\twords = string.match(string.sub(tail, len + 1), \"^%s*(.*)$\")\n";
    s += "\t\telseif string.match(words, \"^@said%s\") then\n";
    s += "\t\t\tlocal len, tail = string.match(words, \"^@said%s+(%d+):(.*)$\")\n";
    s += "\t\t\tif len == nil then return string.match(words, \"^@said%s*(.*)$\") or \"\" end\n";
    s += "\t\t\tlen = tonumber(len)\n";
    s += "\t\t\tpcall(" + g + ".StateSaid, string.sub(tail, 1, len))\n";
    s += "\t\t\twords = string.match(string.sub(tail, len + 1), \"^%s*(.*)$\")\n";
    s += "\t\telseif string.match(words, \"^@memory%s\") then\n";
    s += "\t\t\tlocal soul, rev, n, len, tail = string.match(words, \"^@memory%s+(%S+)%s+compact%s+(%d+)%s+(%d+)%s+(%d+):(.*)$\")\n";
    s += "\t\t\tif soul ~= nil then\n";
    s += "\t\t\t\tlen = tonumber(len)\n";
    s += "\t\t\t\tpcall(function() " + g + ".Compact(soul, rev, n, string.sub(tail, 1, len)) end)\n";
    s += "\t\t\t\twords = string.match(string.sub(tail, len + 1), \"^%s*(.*)$\")\n";
    s += "\t\t\telse\n";
    s += "\t\t\t\tlocal who, name, value\n";
    s += "\t\t\t\twho, name, value, rest = string.match(words, \"^@memory%s+(%S+)%s+stat%s+(%S+)%s+(%S+)%s*(.*)$\")\n";
    s += "\t\t\t\tif who == nil then\n";
    s += "\t\t\t\t\t" + g + ".Log(\"BRIDGE state directive not understood: \" .. string.sub(words, 1, 80))\n";
    s += "\t\t\t\t\treturn string.match(words, \"^@memory%s+%S+%s+%S+%s*(.*)$\") or \"\"\n";
    s += "\t\t\t\tend\n";
    s += "\t\t\t\tpcall(function() " + g + ".StateStat(who, name, value) end)\n";
    s += "\t\t\t\twords = rest\n";
    s += "\t\t\tend\n";
    s += "\t\telseif string.match(words, \"^@rows%s+%d+\") then\n";
    // @rows <n>: the list is whole at n rows, so the draw need not wait for more. With
    // `closed` after the count, the list is those rows alone: no say row and no way out
    // (a dictated letter's part; MenuDraw).
    s += "\t\t\tlocal n, rest2 = string.match(words, \"^@rows%s+(%d+)%s+closed%s*(.*)$\")\n";
    s += "\t\t\tif n ~= nil then " + g + ".menuClosed = true\n";
    s += "\t\t\telse n, rest2 = string.match(words, \"^@rows%s+(%d+)%s*(.*)$\") end\n";
    s += "\t\t\t" + g + ".optTurn = true\n";
    s += "\t\t\t" + g + ".optionsTotal = tonumber(n)\n";
    s += "\t\t\twords = rest2\n";
    s += "\t\telseif string.match(words, \"^@alms%s\") then\n";
    // @alms <money|food|clothes>: a beggar's row was picked; the thing goes (AlmsGive).
    s += "\t\t\tlocal kind, rest3 = string.match(words, \"^@alms%s+(%S+)%s*(.*)$\")\n";
    s += "\t\t\tpcall(function() " + g + ".AlmsGive(kind) end)\n";
    s += "\t\t\twords = rest3\n";
    s += "\t\telseif string.match(words, \"^@check%s\") then\n";
    // @check <skill> <pass|fail>: the app weighed a check of its own (the pry at a letter's
    // writer, app/src/scripted.h) and says how it went; the game's own mark goes up, the one
    // its dialogue skill checks show (DialogUtils.PersuadeByPlayer: Game.ShowStatCheckResult).
    s += "\t\t\tlocal skill, went, rest5 = string.match(words, \"^@check%s+(%S+)%s+(%S+)%s*(.*)$\")\n";
    // The enum is the engine's, not any script's (no .lua defines it), and on this game it
    // is nil (9 Oct 2026: `kind=nil shown=true`, no mark on the screen), so with it nil the
    // other shipped helper's way is taken instead -- DialogUtils.SkillCheck's
    // Game.SendInfoText('@ui_hud_succes' / '@ui_hud_fail'). Neither is called by any shipped
    // quest, so the line logs which was tried and whether it raised.
    s += "\t\t\tlocal kind = eSequenceType_Speach\n";
    s += "\t\t\tif skill == \"strength\" then kind = eSequenceType_Strength\n";
    s += "\t\t\telseif skill == \"charisma\" then kind = eSequenceType_Charisma end\n";
    s += "\t\t\tlocal shown, err\n";
    s += "\t\t\tif kind ~= nil then\n";
    s += "\t\t\t\tshown, err = pcall(function() Game.ShowStatCheckResult(kind, went == \"pass\") end)\n";
    s += "\t\t\telse\n";
    s += "\t\t\t\tkind = \"infotext\"\n";
    s += "\t\t\t\tshown, err = pcall(function() Game.SendInfoText(went == \"pass\" and \"@ui_hud_succes\" or \"@ui_hud_fail\") end)\n";
    s += "\t\t\tend\n";
    s += "\t\t\t" + g + ".Log(\"BRIDGE quest check \" .. tostring(skill) .. \" \" .. tostring(went)\n";
    s += "\t\t\t\t.. \" kind=\" .. tostring(kind) .. \" shown=\" .. tostring(shown) .. (shown and \"\" or (\" \" .. tostring(err))))\n";
    s += "\t\t\twords = rest5\n";
    s += "\t\telseif string.match(words, \"^@heard%s\") then\n";
    // @heard <token>: the words of this turn name somebody Henry had not heard of, so the
    // save now has him knowing of them (state.known; read back on the dump as `known`).
    s += "\t\t\tlocal tok, rest4 = string.match(words, \"^@heard%s+(%S+)%s*(.*)$\")\n";
    s += "\t\t\tif type(" + g + ".state.known) ~= \"table\" then " + g + ".state.known = {} end\n";
    s += "\t\t\t" + g + ".state.known[tok] = true\n";
    s += "\t\t\twords = rest4\n";
    s += "\t\telseif words == \"@end\" or string.match(words, \"^@end%s\") then\n";
    // @end: these are the last words; the conversation closes once they are read (Heard).
    s += "\t\t\t" + g + ".ending = true\n";
    s += "\t\t\twords = string.match(words, \"^@end%s*(.*)$\")\n";
    s += "\t\telseif words == \"@voiced\" or string.match(words, \"^@voiced%s\") then\n";
    // @voiced: the app speaks what it writes and will say when a line has been heard
    // (Spoken, a hook), so Henry's line and the close wait for that. On every line, the
    // @more updates too; each new line is unspoken until the app says otherwise, so a
    // word about an earlier line cannot stand for this one.
    s += "\t\t\t" + g + ".voiced = true\n";
    s += "\t\t\t" + g + ".spoken = false\n";
    s += "\t\t\twords = string.match(words, \"^@voiced%s*(.*)$\")\n";
    s += "\t\telseif words == \"@beat\" or string.match(words, \"^@beat%s\") then\n";
    // @beat: the speaker has more to say -- the next turn is another line of theirs (a
    // written reply told in two beats, notes.md 7 Oct 2026). The line is shown and held
    // for its reading as any other, but the turn is not handed to Henry at the end of it:
    // the pump takes the next turn after the hold, and that one opens the menu.
    s += "\t\t\t" + g + ".beat = true\n";
    s += "\t\t\twords = string.match(words, \"^@beat%s*(.*)$\")\n";
    s += "\t\telse\n";
    s += "\t\t\trest = string.match(words, \"^@more%s*(.*)$\")\n";
    s += "\t\t\tif rest == nil then return words end\n";
    s += "\t\t\t" + g + ".more = true\n";
    s += "\t\t\twords = rest\n";
    s += "\t\tend\n";
    s += "\tend\n";
    s += "end\n";
    s += "\n";
    // What the begin line says about the quest this soul is party to: have= is how many
    // of its item the player carries, which the app reads to know a giver is being
    // approached by someone carrying what they asked for. Nothing at all for a soul with
    // no quest, so the line is what it was.
    // have= is the first quest's count, as it always was; holding=<id>:<n>,... is every
    // quest this soul is party to, for a soul on more than one (a chain's giver, a nest's
    // holder who gives the inner favour), so the app can tell which thing is in Henry's
    // hands.
    s += "function " + g + ".Holding(npc, user)\n";
    s += "\tlocal ids = " + g + ".questsBySoul[EntityUtils.GetName(npc)]\n";
    s += "\tif ids == nil then return \"\" end\n";
    s += "\tif user == nil or user.inventory == nil then user = rawget(_G, \"player\") end\n";
    s += "\tlocal out, all = \"\", \"\"\n";
    s += "\tfor i, id in ipairs(ids) do\n";
    s += "\t\tlocal quest = " + g + ".quests[id]\n";
    s += "\t\tlocal have = " + g + ".CountFresh(user, quest.item, quest.minHealth)\n";
    s += "\t\tlocal theirs = " + g + ".Count(npc, quest.item)\n";
    s += "\t\t" + g + ".Log(\"  quest \" .. id .. \" have=\" .. have .. \" theirs=\" .. theirs ..\n";
    s += "\t\t\t\" code=\" .. " + g + ".questCode .. \" queued=\" .. #" + g + ".questQueue)\n";
    s += "\t\tif have >= 0 then\n";
    s += "\t\t\tif i == 1 then out = \" have=\" .. have end\n";
    s += "\t\t\tall = all .. (all == \"\" and \" holding=\" or \",\") .. id .. \":\" .. have\n";
    s += "\t\tend\n";
    s += "\tend\n";
    // pray=0|1 on a pray's giver: whether the player holds the perk that lets him pray at a
    // shrine at all, which the app reads to withhold the offer from someone who could not.
    s += "\tlocal pray = \"\"\n";
    s += "\tfor _, id in ipairs(ids) do\n";
    s += "\t\tif " + g + ".quests[id].kind == \"pray\" then pray = \" pray=\" .. (" + g + ".CanPray() and 1 or 0) end\n";
    s += "\tend\n";
    s += "\treturn out .. all .. pray\n";
    s += "end\n";
    s += "\n";
    // The holder's menu speaks through a role only they hold. STORM grants it to a soul
    // as it is made, which misses every soul an older save already has, so it is granted
    // again here, on sight, the same way the talk metarole is granted on conversation.
    // Once per soul per run: the grant is written into the save and the check is a
    // metarole scan, which is not something to do every frame an NPC is looked at.
    //
    // Returns true only when a grant was actually made this call. The engine's CanTalk
    // does not see a new role on the same frame -- the first live run read false on the
    // holder straight after the grant and true only after a conversation had come and
    // gone -- so the caller that granted stands back for that frame rather than offer a
    // prompt the base menu is about to make redundant.
    // Every quest the soul holds the thing of, since a soul may hold for one favour and
    // give another. Granted together, and the frame is stood back from if any was.
    s += g + ".holderGranted = {}\n";
    s += "function " + g + ".GrantHolder(npc)\n";
    s += "\tlocal name = EntityUtils.GetName(npc)\n";
    s += "\tif " + g + ".holderGranted[name] then return false end\n";
    s += "\tlocal ids = " + g + ".questsBySoul[name]\n";
    s += "\tif ids == nil then return false end\n";
    s += "\t" + g + ".holderGranted[name] = true\n";
    s += "\tlocal granted = false\n";
    s += "\tfor _, id in ipairs(ids) do\n";
    s += "\t\tlocal quest = " + g + ".quests[id]\n";
    s += "\t\tif quest ~= nil and quest.target == name and not " + g + ".HasMeta(npc, quest.holderMeta)\n";
    s += "\t\t\t\tand npc.soul ~= nil and npc.soul.AddMetaRoleByName ~= nil then\n";
    s += "\t\t\t" + g + ".GrantName(npc, quest.holderMeta)\n";
    s += "\t\t\t" + g + ".Log(\"BRIDGE quest \" .. id .. \" holder role \" .. quest.holderMeta ..\n";
    s += "\t\t\t\t\" granted to \" .. name .. \" -> \" .. tostring(" + g + ".HasMeta(npc, quest.holderMeta)))\n";
    s += "\t\t\tgranted = true\n";
    s += "\t\tend\n";
    // The one an ask's word is carried on to speaks through a role of their own, the same
    // way, for the one hand row that completes the errand.
    s += "\t\tif quest ~= nil and quest.onward == name and not " + g + ".HasMeta(npc, quest.onwardMeta)\n";
    s += "\t\t\t\tand npc.soul ~= nil and npc.soul.AddMetaRoleByName ~= nil then\n";
    s += "\t\t\t" + g + ".GrantName(npc, quest.onwardMeta)\n";
    s += "\t\t\t" + g + ".Log(\"BRIDGE quest \" .. id .. \" onward role \" .. quest.onwardMeta ..\n";
    s += "\t\t\t\t\" granted to \" .. name .. \" -> \" .. tostring(" + g + ".HasMeta(npc, quest.onwardMeta)))\n";
    s += "\t\t\tgranted = true\n";
    s += "\t\tend\n";
    // And the one who can let Henry in, for her door, her key and her way out.
    s += "\t\tif quest ~= nil and quest.helper == name and not " + g + ".HasMeta(npc, quest.helperMeta)\n";
    s += "\t\t\t\tand npc.soul ~= nil and npc.soul.AddMetaRoleByName ~= nil then\n";
    s += "\t\t\t" + g + ".GrantName(npc, quest.helperMeta)\n";
    s += "\t\t\t" + g + ".Log(\"BRIDGE quest \" .. id .. \" helper role \" .. quest.helperMeta ..\n";
    s += "\t\t\t\t\" granted to \" .. name .. \" -> \" .. tostring(" + g + ".HasMeta(npc, quest.helperMeta)))\n";
    s += "\t\t\tgranted = true\n";
    s += "\t\tend\n";
    s += "\tend\n";
    s += "\treturn granted\n";
    s += "end\n";
    s += "\n";
    // The same for the one who can let Henry in, at the naming.
    s += "function " + g + ".GrantHelperByName(quest)\n";
    s += "\tif quest.helper == nil or quest.helper == \"\" then return end\n";
    s += "\tlocal soul = nil\n";
    s += "\tpcall(function() soul = System.GetEntityByName(quest.helper) end)\n";
    s += "\tif soul == nil then\n";
    s += "\t\t" + g + ".Log(\"BRIDGE quest helper \" .. quest.helper .. \" not loaded, role waits for sight\")\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    s += "\tpcall(function() " + g + ".GrantHolder(soul) end)\n";
    s += "end\n";
    s += "\n";
    // The same for the one an ask's word is carried on to, at the `onward` beat.
    s += "function " + g + ".GrantOnwardByName(quest)\n";
    s += "\tif quest.onward == nil or quest.onward == \"\" then return end\n";
    s += "\tlocal soul = nil\n";
    s += "\tpcall(function() soul = System.GetEntityByName(quest.onward) end)\n";
    s += "\tif soul == nil then\n";
    s += "\t\t" + g + ".Log(\"BRIDGE quest onward \" .. quest.onward .. \" not loaded, role waits for sight\")\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    s += "\tpcall(function() " + g + ".GrantHolder(soul) end)\n";
    s += "end\n";
    s += "\n";
    // The same grant at the moment the errand is taken up, by name, so the holder has
    // their role while the player is still walking over and the engine has all that
    // time to notice. On-sight granting alone left the grant and the first look at the
    // menu on the same frame. Quiet when the holder is not loaded: the on-sight grant
    // still covers them, and by then the errand has usually been accepted for a while.
    s += "function " + g + ".GrantHolderByName(quest)\n";
    s += "\tlocal holder = nil\n";
    s += "\tpcall(function() holder = System.GetEntityByName(quest.target) end)\n";
    s += "\tif holder == nil then\n";
    s += "\t\t" + g + ".Log(\"BRIDGE quest holder \" .. quest.target .. \" not loaded, role waits for sight\")\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    s += "\tpcall(function() " + g + ".GrantHolder(holder) end)\n";
    s += "end\n";
    s += "\n";
    // Named in the ExitScript of the holder menu's winning branches, so it runs once the
    // holder has said their piece and the port has fired. how is which branch won --
    // persuade, threaten or pay -- and pay is the one with money in it. The holder is
    // found by name rather than taken from the conversation, since there is no
    // conversation: this is the game's own dialogue, and Moonshine.npc is whoever was
    // last talked to by the other road.
    // Named in the ExitScript of a holder sequence that moves nothing -- the door (the menu
    // is about to be drawn), a check taken (`row persuade`), a check lost (`refused
    // threaten`), an early visit, the way out -- and called from the functions below for
    // the rows that do move something. A sync point: the runner reads the stage off the
    // log, or off holderStage from inside the game, where it used to count seconds.
    s += "function " + g + ".QuestStage(id, what)\n";
    s += "\t" + g + ".holderStage = { id = tostring(id), what = tostring(what), n = (" + g + ".holderStage and " + g + ".holderStage.n or 0) + 1 }\n";
    s += "\t" + g + ".Log(\"BRIDGE quest stage \" .. tostring(id) .. \" \" .. tostring(what))\n";
    // The street's Talk closing on the holder's turn (the followed sequence's exit script):
    // Henry's own word on the night (FollowOver).
    s += "\tif tostring(what) == \"open followed\" then pcall(" + g + ".FollowOver, tostring(id)) end\n";
    s += "end\n";
    s += "\n";
    s += "function " + g + ".QuestHolderGave(id, how)\n";
    s += "\tlocal quest = " + g + ".quests[id]\n";
    s += "\tif quest == nil then\n";
    s += "\t\t" + g + ".Log(\"BRIDGE quest unknown id=\" .. tostring(id) .. \" gave \" .. tostring(how))\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    s += "\t" + g + ".QuestStage(id, \"won \" .. tostring(how))\n";
    s += "\t" + g + ".Log(\"BRIDGE quest \" .. id .. \" handover -- \" .. quest.name .. \" (\" .. tostring(how) .. \")\")\n";
    // The branch, as the journal names it, kept for the complete beat. brokered is two
    // endings: the giver gave his word (the app's apologise directive is in the store),
    // or Henry said he did.
    // An ambush's truth row is its handover with nothing in hand and no how of its own:
    // told as it was is the plain completion.
    // An exposure's row: the holder caught and the thing given up for silence (exposed); the
    // bedside's blackmail won is the thing and coin (exposed_paid), lost the thing alone.
    s += "\tlocal hows = { persuade = \"persuaded\", threaten = \"threatened\", pay = \"paid\", nested = \"nested\",\n";
    s += "\t\texpose = \"exposed\", blackmail = \"exposed_paid\", blackmail_no = \"exposed\" }\n";
    s += "\tlocal f = " + g + ".state.quests[id] or {}\n";
    s += "\tif how == \"brokered\" then f.how = f.apologised and \"brokered\" or \"lied\"\n";
    s += "\telse f.how = hows[how] end\n";
    s += "\t" + g + ".state.quests[id] = f\n";
    s += "\tif f.how ~= nil then " + g + ".Log(\"BRIDGE quest \" .. id .. \" won by \" .. f.how) end\n";
    s += "\tpcall(" + g + ".StateQuest, id, \"handover\")\n";
    s += "\tlocal holder = nil\n";
    s += "\tpcall(function() holder = System.GetEntityByName(quest.target) end)\n";
    s += "\tif holder == nil then\n";
    s += "\t\t" + g + ".Log(\"  holder \" .. quest.target .. \" not found, the item will be made\")\n";
    s += "\tend\n";
    s += "\tif how == \"pay\" and quest.pay > 0 then\n";
    // Shops.DoMoneyTransaction is what the shipped menus pay a horse trader or a teacher
    // with: the receiving soul, the amount in decigroschen (hence the ten), and whether
    // to play a farewell bark. ShowItemsTransfer is the "-25 Groschen" notice.
    s += "\t\tlocal ok, err = pcall(function()\n";
    s += "\t\t\tShops.DoMoneyTransaction(holder.id, quest.pay * 10, 0)\n";
    s += "\t\t\tGame.ShowItemsTransfer(ItemUtils.itemIDs.money, -quest.pay * 10)\n";
    s += "\t\tend)\n";
    s += "\t\tif ok then\n";
    s += "\t\t\t" + g + ".Log(\"BRIDGE quest paid \" .. quest.pay .. \" groschen to the holder\")\n";
    s += "\t\telse\n";
    s += "\t\t\t" + g + ".Log(\"BRIDGE quest payment raised \" .. tostring(err))\n";
    s += "\t\tend\n";
    s += "\tend\n";
    // A buy's price has a second part: the giver's thing goes to the holder with the money
    // (moved; deleted if the move is refused, so the terms are met either way).
    s += "\tif how == \"pay\" and quest.kind == \"buy\" and quest.carry ~= nil and quest.carry ~= \"\" then\n";
    s += "\t\tlocal p = " + g + ".Player()\n";
    s += "\t\tlocal moved = holder ~= nil and " + g + ".MoveOne(p, holder, quest.carry) or 0\n";
    s += "\t\tif moved < 1 then\n";
    s += "\t\t\tpcall(function() p.inventory:DeleteItemOfClass(quest.carry, 1) end)\n";
    s += "\t\t\tpcall(function() Game.ShowItemsTransfer(quest.carry, -1) end)\n";
    s += "\t\tend\n";
    s += "\t\t" + g + ".Log(\"BRIDGE quest gave the \" .. tostring(quest.carryName) .. \" to the holder, moved \" .. moved\n";
    s += "\t\t\t.. \", have=\" .. " + g + ".Count(p, quest.carry))\n";
    s += "\t\tpcall(" + g + ".QuestFlags)\n";
    s += "\tend\n";
    // An ask's gift (Emmeram's fox-fur hat and gloves) is not made here: the winning branch
    // goes on to a menu of two, and QuestGift below has the taking or the refusing.
    // Nothing to hand over on a beat: the man's word is the whole of it. Nor on an ambush's
    // truth: the way is spoken, and the false directions stay where they are.
    s += "\tif quest.item ~= \"\" and quest.kind ~= \"ambush\" then " + g + ".Handover(quest, holder) end\n";
    // The bedside's blackmail won: his coin for Henry's silence, over the thing. Made in
    // Henry's purse as a reward is (PayMoney) rather than moved out of his: a priest's
    // pockets in the data hold nothing like it, and the shipped money transaction only runs
    // the other way.
    s += "\tif how == \"blackmail\" and (quest.blackmail or 0) > 0 then\n";
    s += "\t\t" + g + ".PayMoney(quest.blackmail, \"blackmail paid\")\n";
    s += "\tend\n";
    s += "end\n";
    s += "\n";
    // Named in the ExitScript of the bedside's denounce row: Henry walks out with nothing,
    // the parish to hear where its priest sleeps. The store notes it (denounced; the letter
    // d, and the how, since the giver's report is the only complete left to this errand);
    // the port on the same row writes the journal. The thing stays where it is.
    s += "function " + g + ".QuestDenounced(id)\n";
    s += "\tlocal quest = " + g + ".quests[id]\n";
    s += "\tif quest == nil then\n";
    s += "\t\t" + g + ".Log(\"BRIDGE quest unknown id=\" .. tostring(id) .. \" denounced\")\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    s += "\t" + g + ".QuestStage(id, \"denounced\")\n";
    s += "\t" + g + ".Log(\"BRIDGE quest \" .. id .. \" denounced -- \" .. quest.name)\n";
    s += "\tpcall(" + g + ".StateQuest, id, \"denounced\")\n";
    s += "end\n";
    s += "\n";
    // Named in the ExitScript of an unyielding holder's two rows (In the Stocks' sacristan):
    // the refusal is the only answer, Henry is shown the door, and the store notes it
    // (rebuffed; the letter n) for the app's giver, who sends him at the chest anyway and
    // names the one who can let him in. The port on the same row writes the journal.
    s += "function " + g + ".QuestRebuffed(id, row)\n";
    s += "\tlocal quest = " + g + ".quests[id]\n";
    s += "\tif quest == nil then\n";
    s += "\t\t" + g + ".Log(\"BRIDGE quest unknown id=\" .. tostring(id) .. \" rebuffed \" .. tostring(row))\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    s += "\t" + g + ".QuestStage(id, \"rebuffed \" .. tostring(row))\n";
    s += "\t" + g + ".Log(\"BRIDGE quest \" .. id .. \" rebuffed on \" .. tostring(row) .. \" -- \" .. quest.name)\n";
    s += "\tpcall(" + g + ".StateQuest, id, \"rebuffed\")\n";
    s += "end\n";
    s += "\n";
    // Named in the ExitScript of the helper menu's two winning rows. 'door': her price in
    // groschen goes to her (the holder's fee's own call), and the store notes she has let
    // Henry in (letin; the letter i) -- the port on the row opens the house. 'key': her
    // price goes to her, the chest's key is made in Henry's pocket (GiveClass, as a letter
    // is), and the store notes it (keyed; q).
    s += "function " + g + ".QuestHelper(id, what)\n";
    s += "\tlocal quest = " + g + ".quests[id]\n";
    s += "\tif quest == nil or quest.helper == nil then\n";
    s += "\t\t" + g + ".Log(\"BRIDGE quest unknown id=\" .. tostring(id) .. \" helper \" .. tostring(what))\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    s += "\t" + g + ".QuestStage(id, \"helper \" .. tostring(what))\n";
    s += "\tlocal price = (what == \"key\") and (quest.keyPrice or 0) or (quest.doorPrice or 0)\n";
    s += "\tlocal helper = nil\n";
    s += "\tpcall(function() helper = System.GetEntityByName(quest.helper) end)\n";
    s += "\tif price > 0 then\n";
    s += "\t\tlocal ok, err = pcall(function()\n";
    s += "\t\t\tif helper ~= nil then Shops.DoMoneyTransaction(helper.id, price * 10, 0)\n";
    s += "\t\t\telse " + g + ".Player().inventory:DeleteItemOfClass(ItemUtils.itemIDs.money, price * 10) end\n";
    s += "\t\t\tGame.ShowItemsTransfer(ItemUtils.itemIDs.money, -price * 10)\n";
    s += "\t\tend)\n";
    s += "\t\tif ok then\n";
    s += "\t\t\t" + g + ".Log(\"BRIDGE quest \" .. id .. \" paid \" .. price .. \" groschen to \" .. tostring(quest.helperName) .. \" for the \" .. tostring(what))\n";
    s += "\t\telse\n";
    s += "\t\t\t" + g + ".Log(\"BRIDGE quest \" .. id .. \" helper payment raised \" .. tostring(err))\n";
    s += "\t\tend\n";
    s += "\tend\n";
    s += "\tif what == \"key\" and quest.keyClass ~= nil then\n";
    s += "\t\t" + g + ".GiveClass(quest.keyClass, 1, \"the key to \" .. quest.target .. \"'s chest\")\n";
    s += "\t\tpcall(" + g + ".StateQuest, id, \"keyed\")\n";
    s += "\telseif what == \"door\" then\n";
    s += "\t\tpcall(" + g + ".StateQuest, id, \"letin\")\n";
    s += "\tend\n";
    s += "end\n";
    s += "\n";
    // Named in the ExitScript of the two rows under an ask's winning branch when the ask
    // has a bribe in it (gift_classes). 'took': the classes are made in Henry's pocket, one
    // at a time as any other thing is (GiveClass; they are Armor rows the mod ships as static
    // assets), and his to keep whatever he then tells the giver. 'refused': the holder's
    // answer is a bucket of guts -- every stitch Henry has on is fouled (AddDirt is the
    // engine's own dirt on all equipped items, AddBlood the stain of it) and he carries the
    // mod's Smelly debuff (smellyBuff, a day of the world's clock, charisma down), to the
    // sound of it landing. Either way the store keeps which (gift_took /
    // gift_refused; the letters t and f), so the giver's conversation can know.
    s += "function " + g + ".QuestGift(id, how)\n";
    s += "\tlocal quest = " + g + ".quests[id]\n";
    s += "\tif quest == nil then\n";
    s += "\t\t" + g + ".Log(\"BRIDGE quest unknown id=\" .. tostring(id) .. \" gift \" .. tostring(how))\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    s += "\t" + g + ".QuestStage(id, \"gift \" .. tostring(how))\n";
    s += "\tif how == \"took\" then\n";
    s += "\t\tif quest.gift ~= nil then\n";
    s += "\t\t\tfor _, class in ipairs(quest.gift) do " + g + ".GiveClass(class, 1, \"gift\") end\n";
    s += "\t\tend\n";
    s += "\t\t" + g + ".Log(\"BRIDGE quest \" .. id .. \" gift took\")\n";
    s += "\t\tpcall(" + g + ".StateQuest, id, \"gift_took\")\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    s += "\tlocal p = " + g + ".Player()\n";
    s += "\tlocal dirtied, err = pcall(function()\n";
    s += "\t\tp.actor:AddDirt(1)\n";
    s += "\t\tp.actor:AddBlood(0.5)\n";
    s += "\tend)\n";
    s += "\tif not dirtied then " + g + ".Log(\"  AddDirt raised \" .. tostring(err)) end\n";
    s += "\tlocal buffed, inst = pcall(function() return p.soul:AddBuff(" + g + ".smellyBuff) end)\n";
    s += "\tif not buffed then " + g + ".Log(\"  AddBuff raised \" .. tostring(inst)) end\n";
    s += "\t" + g + ".Log(\"BRIDGE quest \" .. id .. \" gift refused -- a bucket of guts, dirtied=\" .. tostring(dirtied)\n";
    s += "\t\t.. \" smelly=\" .. tostring(buffed and inst ~= nil))\n";
    // The sound of it, on Henry himself: the wet slap of the stuff landing and the slop
    // after. No line on the screen (noted 5 Oct 2026: it read as debug text); the Smelly
    // buff's own name and description in the stats say what happened and for how long.
    s += "\t" + g + ".Sound(p, \"" + std::string(kGutsSlapSound) + "\")\n";
    s += "\t" + g + ".Sound(p, \"" + std::string(kGutsSlopSound) + "\")\n";
    s += "\tpcall(" + g + ".StateQuest, id, \"gift_refused\")\n";
    s += "end\n";
    s += "\n";
    // Named in the ExitScript of each of a mind target's visit rows, once the words are
    // said and the port has fired: the store counts the visit and takes the clock, and the
    // globals move on so the next row is the one on offer when its time comes.
    s += "function " + g + ".QuestChecked(id)\n";
    s += "\tlocal quest = " + g + ".quests[id]\n";
    s += "\tif quest == nil then\n";
    s += "\t\t" + g + ".Log(\"BRIDGE quest unknown id=\" .. tostring(id) .. \" checked\")\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    s += "\tpcall(" + g + ".StateQuest, id, \"checked\")\n";
    s += "\tlocal f = " + g + ".state.quests[id] or {}\n";
    s += "\t" + g + ".QuestStage(id, \"check \" .. tostring(f.checks))\n";
    s += "\t" + g + ".Log(\"BRIDGE quest \" .. id .. \" checked \" .. tostring(f.checks) .. \" of \" .. tostring(quest.count) .. \" -- \" .. quest.name)\n";
    s += "\tif " + g + ".mindDue ~= nil then " + g + ".mindDue[id] = nil end\n";
    s += "\tpcall(" + g + ".QuestFlags)\n";
    s += "\tif (f.checks or 0) >= (quest.count or 1) then pcall(" + g + ".StateQuest, id, \"handover\") end\n";
    s += "end\n";
    s += "\n";
    // Named in the ExitScript of a dispute holder's broker row. Nothing moves; the store
    // learns Henry has offered to carry the grievance, which puts the apology on the
    // giver's menu (the app reads the b flag off the dump) and the brokered row on the
    // holder's (QuestFlags writes the global).
    s += "function " + g + ".QuestBroker(id)\n";
    s += "\tlocal quest = " + g + ".quests[id]\n";
    s += "\tif quest == nil then\n";
    s += "\t\t" + g + ".Log(\"BRIDGE quest unknown id=\" .. tostring(id) .. \" broker\")\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    s += "\t" + g + ".QuestStage(id, \"row broker\")\n";
    s += "\t" + g + ".Log(\"BRIDGE quest \" .. id .. \" broker -- \" .. quest.name)\n";
    s += "\tpcall(" + g + ".StateQuest, id, \"broker\")\n";
    s += "end\n";
    s += "\n";
    // A deliver's letter, made in the player's pocket the moment the writer's errand is
    // taken up: inventory:CreateItem(class, health, amount), as the reward's money is
    // made, and the game's own "received" notice.
    s += "function " + g + ".GiveLetter(quest) " + g + ".GiveThing(quest, 1) end\n";
    s += "\n";
    // The same for a leave's or a return's thing, count of them (a return's grave goods are
    // two or three pieces).
    s += "function " + g + ".GiveThing(quest, count) " + g + ".GiveClass(quest.item, count, \"thing\") end\n";
    s += "\n";
    // Any class, count of them, by name for the log: a buy's carried thing at accept, an
    // ambush's directions on the first stage of its menu.
    // `again` true makes it whether or not he has one: a reward is his pay, not a token
    // the errand looks for in his pocket.
    s += "function " + g + ".GiveClass(class, count, label, again)\n";
    s += "\tif class == nil or class == \"\" then return end\n";
    s += "\tlocal p = " + g + ".Player()\n";
    s += "\tif not again and " + g + ".Count(p, class) > 0 then\n";
    s += "\t\t" + g + ".Log(\"BRIDGE quest \" .. tostring(label) .. \" already in the player's pocket\")\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    // One call per piece: CreateItem's amount is a stack, and a custom MiscItem that does
    // not stack (the grave goods, three of them) came out as one when asked for three in
    // one call (return_probe, 1 Oct).
    s += "\tlocal ok, err = pcall(function()\n";
    s += "\t\tfor _ = 1, count do p.inventory:CreateItem(class, 100, 1) end\n";
    s += "\t\tGame.ShowItemsTransfer(class, count)\n";
    s += "\tend)\n";
    s += "\tif ok then\n";
    s += "\t\t" + g + ".Log(\"BRIDGE quest \" .. tostring(label) .. \" given, have=\" .. " + g + ".Count(p, class))\n";
    s += "\telse\n";
    s += "\t\t" + g + ".Log(\"BRIDGE quest \" .. tostring(label) .. \" could not be made: \" .. tostring(err))\n";
    s += "\tend\n";
    s += "end\n";
    s += "\n";
    // Named in the ExitScript of an ambush target's first row, once the false way is given
    // and the port has fired: the directions go into Henry's pocket as a Document, the store
    // learns it (directed), and the stage global moves on so the early row is the one on
    // offer until the trap is sprung. The Watch is armed here too, in case the errand was
    // accepted in a world the watch has since lost.
    s += "function " + g + ".QuestDirected(id)\n";
    s += "\tlocal quest = " + g + ".quests[id]\n";
    s += "\tif quest == nil then\n";
    s += "\t\t" + g + ".Log(\"BRIDGE quest unknown id=\" .. tostring(id) .. \" directed\")\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    s += "\t" + g + ".QuestStage(id, \"directed\")\n";
    s += "\t" + g + ".Log(\"BRIDGE quest \" .. id .. \" directed -- \" .. quest.name)\n";
    s += "\t" + g + ".GiveClass(quest.item, 1, \"directions\")\n";
    s += "\tpcall(" + g + ".StateQuest, id, \"directed\")\n";
    s += "\t" + g + ".WatchArm(\"directed\")\n";
    s += "end\n";
    s += "\n";
    // Named in the ExitScript of the receiver's hand row: the letter goes from Henry's
    // pocket to the receiver's, and the store learns it was delivered, which is what the
    // writer's report row completes on. A player who has no letter to give (sold it,
    // lost it to a guard) still hands over nothing and is still believed by the game's
    // menu, which had no way to check; the log says so.
    // `how` is the interrogation's answer where the receiver asked one (the tell_* rows:
    // truth, pinned, covered, fibbed, evaded, caught) and nil from a plain hand row; the store keeps
    // it as the handover's how, which the giver's report then completes on (complete_<how>
    // where the journal has the line) and the app reads for the giver's answer.
    s += "function " + g + ".QuestDelivered(id, how)\n";
    s += "\tlocal quest = " + g + ".quests[id]\n";
    s += "\tif quest == nil then\n";
    s += "\t\t" + g + ".Log(\"BRIDGE quest unknown id=\" .. tostring(id) .. \" delivered\")\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    s += "\thow = how or \"delivered\"\n";
    s += "\t" + g + ".QuestStage(id, \"hand\")\n";
    s += "\t" + g + ".Log(\"BRIDGE quest \" .. id .. \" handover -- \" .. quest.name .. \" (\" .. how .. \")\")\n";
    s += "\tlocal f = " + g + ".state.quests[id] or {}\n";
    s += "\tf.how = how\n";
    s += "\t" + g + ".state.quests[id] = f\n";
    s += "\tpcall(" + g + ".StateQuest, id, \"handover\")\n";
    // Word carried, no letter: nothing moves, and the errand is done where it was told --
    // there is nothing to carry back to the giver -- so the complete beat is queued from here,
    // the way the app's directive would queue it, and pays what the giver promised.
    s += "\tif quest.item == \"\" then\n";
    s += "\t\t" + g + ".Log(\"BRIDGE quest \" .. id .. \" word carried to \" .. quest.target .. \", complete on the spot\")\n";
    s += "\t\t" + g + ".QuestCommand(id, \"complete\")\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    s += "\tlocal receiver = nil\n";
    s += "\tpcall(function() receiver = System.GetEntityByName(quest.target) end)\n";
    s += "\tlocal moved = 0\n";
    s += "\tif receiver ~= nil then moved = " + g + ".MoveOne(" + g + ".Player(), receiver, quest.item) end\n";
    s += "\tif moved < 1 then\n";
    s += "\t\tpcall(function() " + g + ".Player().inventory:DeleteItemOfClass(quest.item, 1) end)\n";
    s += "\t\t" + g + ".Log(\"BRIDGE quest delivered but no letter moved (had \" .. " + g +
         ".Count(" + g + ".Player(), quest.item) .. \")\")\n";
    s += "\telse\n";
    s += "\t\t" + g + ".Log(\"BRIDGE quest delivered, moved \" .. moved .. \" to \" .. quest.target)\n";
    s += "\tend\n";
    s += "end\n";
    s += "\n";
    // Named in the ExitScript of the onward menu's one row: the word an ask's giver sent
    // on has been said to the one it was for. Nothing moves and nothing is paid (the giver
    // paid at the telling); the errand is done where it was said, so the plain complete is
    // queued from here as a word deliver's is. `carried` is the store's record of it.
    s += "function " + g + ".QuestCarried(id)\n";
    s += "\tlocal quest = " + g + ".quests[id]\n";
    s += "\tif quest == nil then\n";
    s += "\t\t" + g + ".Log(\"BRIDGE quest unknown id=\" .. tostring(id) .. \" carried\")\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    s += "\t" + g + ".QuestStage(id, \"carried\")\n";
    s += "\t" + g + ".Log(\"BRIDGE quest \" .. id .. \" word carried on to \" .. tostring(quest.onward) .. \" -- \" .. quest.name)\n";
    s += "\tlocal f = " + g + ".state.quests[id] or {}\n";
    s += "\tf.carried = true\n";
    s += "\tif not f.onward then\n";
    // The menu opened without the giver's telling behind it (a save from between the two,
    // or a grant made early): the telling is taken as given, since the row is only offered
    // once the graph's last objective is open.
    s += "\t\tf.onward = true\n";
    s += "\tend\n";
    s += "\t" + g + ".state.quests[id] = f\n";
    s += "\t" + g + ".QuestCommand(id, \"complete\")\n";
    s += "end\n";
    s += "\n";
    s += "function " + g + ".OnConversation(npc, user, slotId)\n";
    // Both fields on the one line, so the app has the look at the moment it picks the
    // greeting rather than a marker later. The soul id is a name to nobody -- it is the
    // key npcs.sqlite is built on, which is why it is what gets logged.
    // doing= is the last thing the player was seen to be interrupting, remembered from
    // the look rather than read here: by now the NPC has put the saw down to talk.
    // "none" when nothing was observed, which is what a soul spawning into view and
    // being talked to immediately looks like.
    // have= joins the line only for a soul with a quest, read before anything else here
    // so the app has it at the moment it plans the conversation.
    s += "\tlocal holding = \"\"\n";
    s += "\tpcall(function() holding = " + g + ".Holding(npc, user) end)\n";
    // alms=<letters> on a beggar: what Henry could give them (AlmsLine); nothing on anyone else.
    s += "\tpcall(function() holding = holding .. " + g + ".AlmsLine(npc, user) end)\n";
    s += "\tlocal scene = " + g + ".Seen(user) .. " + g + ".SoulState(npc) .. " + g + ".World() .. holding"
         " .. \" qset=\" .. tostring(" + g + ".questSet)\n";
    // The store first: met goes up, this conversation's gap entry is written with the same
    // readings the begin line carries, and the soul's whole record is dumped, so that by
    // the time the app reads the begin line it already holds everything it needs to plan
    // the conversation. Under pcall, as a fault in the store is not a reason not to talk.
    s += "\tpcall(function() " + g + ".StateBegin(EntityUtils.GetName(npc), scene) end)\n";
    s += "\t" + g + ".Log(\"BRIDGE begin npc=\" .. EntityUtils.GetName(npc)\n";
    s += "\t\t.. \" doing=\" .. (" + g + ".lastAnim[EntityUtils.GetName(npc)] or \"none\")\n";
    s += "\t\t.. scene)\n";
    // Says whether STORM reached this soul at all, which is the difference between
    // a save made before the mod existed and one made after.
    s += "\t" + g + ".Log(\"  metaroles \" .. " + g + ".MetaRoles(npc))\n";
    s += "\tpcall(function() " + g + ".Grant(npc) end)\n";
    // AddMetaRoleByName reports nothing and raises nothing when it declines, so the
    // only honest check is to ask again afterwards and see whether the grant stuck.
    s += "\t" + g + ".Log(\"  after grant \" .. " + g + ".MetaRoles(npc))\n";
    s += "\t" + g + ".npc = npc\n";
    s += "\t" + g + ".user = user\n";
    // Who is speaking, for the front of every line. Looked up here once rather than per
    // line, and guarded, since GetName on something odd is not worth a conversation.
    s += "\tlocal named = \"\"\n";
    s += "\tpcall(function() named = " + g + ".soulNames[EntityUtils.GetName(npc)] or \"\" end)\n";
    s += "\t" + g + ".npcName = named\n";
    // The pump opens the session rather than this, so there is one path in and it is
    // the same one every beat afterwards runs through.
    s += "\tif " + g + ".active then return end\n";
    s += "\t" + g + ".active = true\n";
    s += "\t" + g + ".beganAt = " + g + ".Now()\n";
    // The disk is opened for reading by the pump (Step), once the dialogue is up, and not
    // here: the dialogue coming up is a load of its own -- the faces, the cameras, the
    // sequences -- and with the disk second in line every file the paks lack is a miss
    // that goes to disk, the same cost a level load paid (Disk). The first turn takes the
    // app a second to write, and the dialogue was up at the first tick when measured (12
    // Oct 2026), so nothing waits on this. (The seven-second first line that prompted it
    // turned out to be the app's, reading the sheet in before it read the log again --
    // Session::greet; this stayed as the cheaper order of things.)
    s += "\t" + g + ".diskOn = false\n";
    // And the frame rate, for the voice engine's sake (Frames).
    s += "\tpcall(" + g + ".FramesHold)\n";
    s += "\t" + g + ".opened = false\n";
    s += "\t" + g + ".pushed = false\n";
    s += "\t" + g + ".beats = 0\n";
    s += "\t" + g + ".hold = 0\n";
    s += "\t" + g + ".Face(\"idle\")\n";
    s += "\t" + g + ".questQueue = {}\n";
    s += "\t" + g + ".SetQuestCode(0)\n";
    // Nothing is being held at the moment a conversation opens, and nothing is waiting to
    // be told about a hold, whatever the button was doing when the last one ended.
    s += "\t" + g + ".down = false\n";
    s += "\t" + g + ".pending = 0\n";
    // The keyboard, pointed at this player and with F9 armed. Typing itself waits for
    // the first line.
    s += "\tpcall(function() " + g + ".TypingOpen(user) end)\n";
    // Cleared before the first line so a conversation cannot open showing the last thing
    // the one before it said.
    s += "\t" + g + ".shown = \"\"\n";
    s += "\t" + g + ".above = \"\"\n";
    s += "\t" + g + ".drawShape = 0\n";
    s += "\t" + g + ".paints = 0\n";
    s += "\t" + g + ".paintFrom = " + g + ".ticks\n";
    s += "\t" + g + ".rateTold = false\n";
    // Before the first beat, so the caption is already out of the way by the time the
    // dialogue has a line to put in it -- unless the caption is where our own words are
    // going, in which case it is wanted visible and wanted to have its plate, and a session
    // that hid it earlier in this run has to be undone.
    s += "\t" + g + ".wrote = nil\n";
    s += "\tif " + g + ".mode == \"caption\" then\n";
    s += "\t\t" + g + ".Caption(true)\n";
    s += "\t\t" + g + ".Plate()\n";
    s += "\telse\n";
    s += "\t\t" + g + ".Caption(false)\n";
    s += "\tend\n";
    // The hint gets the screen to itself before the first line, because a conversation
    // that has started is the one moment a player needs telling how to end one, and
    // because a line arriving on top of it would take it away again -- info text by
    // clearing its queue, drawn text by occupying the same position.
    // Holding the pump for as long as it is up is what buys it that time.
    //
    // It doubles as a canary: it is the first thing either road is asked to carry, so
    // this and then nothing means the words are being blocked rather than refused, and
    // nothing at all means the session never opened.
    s += "\tif " + g + ".leaveHint ~= \"\" then\n";
    s += "\t\tlocal seconds = " + g + ".Reading(" + g + ".leaveHint)\n";
    s += "\t\t" + g + ".Show(" + g + ".leaveHint, seconds)\n";
    // And again down the notice widget when the words are being drawn, because drawing has
    // a failure nothing can catch -- a call the engine accepts and then does not put on
    // screen, which a master build is entitled to do with anything that started life as
    // debug text -- and a player who cannot see the hint is a player with no way out at
    // all: the prompt is gone, movement is gone, and the watchdog will not fire on a
    // conversation that is working perfectly well.
    //
    // Only that road. The caption needs no such insurance and looked like exactly what it
    // was, a second message in the corner the game puts its own notices in, at the one
    // moment a player is being asked to look at a face instead.
    s += "\t\tif " + g + ".mode == \"draw\" then\n";
    s += "\t\t\tpcall(function()\n";
    s += "\t\t\t\tGame.SendInfoText(" + g + ".leaveHint, true, nil, seconds)\n";
    s += "\t\t\tend)\n";
    s += "\t\tend\n";
    s += "\t\t" + g + ".hold = " + g + ".ticks + math.ceil(seconds / 0.4)\n";
    s += "\t\t" + g + ".Log(\"BRIDGE hint -- \" .. " + g + ".leaveHint)\n";
    s += "\tend\n";
    // Not the turn counter, which carries on from wherever the last conversation left
    // it. The writer numbers turns across the whole run of the game, so starting again
    // at one here would have us re-read words that have already been said.
    s += "\tpcall(" + g + ".Pump)\n";
    s += "end\n";
    s += "\n";
    s += state_lua(g);
    // The player has this soul in their sights from talking distance -- GetActions is only
    // asked then -- and has not pressed. Told to the app as a begin that has not happened:
    // the record dumped as StateBegin would dump it, then a `look` line with the begin's
    // own readings, so the app can have the opening line ready by the time they press
    // (Session::look). Once per look, not per frame: the same soul again is not repeated
    // until the eye has been elsewhere or some seconds have passed, since the readings
    // (the hour, what they are doing) drift and a stale guess is a wasted one. Not while a
    // conversation is up, though GetActions does not run then anyway.
    s += g + ".lookAt = nil\n";
    s += g + ".lookWhen = 0\n";
    s += g + ".lookAgain = 20\n";
    s += "function " + g + ".Look(npc, user)\n";
    s += "\tif " + g + ".active then return end\n";
    s += "\tlocal id = EntityUtils.GetName(npc)\n";
    s += "\tlocal now = " + g + ".Now()\n";
    s += "\tif " + g + ".lookAt == id and now - " + g + ".lookWhen < " + g + ".lookAgain then return end\n";
    s += "\t" + g + ".lookAt = id\n";
    s += "\t" + g + ".lookWhen = now\n";
    s += "\tlocal holding = \"\"\n";
    s += "\tpcall(function() holding = " + g + ".Holding(npc, user) end)\n";
    s += "\tlocal scene = " + g + ".Seen(user) .. " + g + ".SoulState(npc) .. " + g + ".World() .. holding"
         " .. \" qset=\" .. tostring(" + g + ".questSet)\n";
    s += "\tpcall(function() " + g + ".Dump(id, { k = \"gap\", t = " + g + ".WorldTime(), s = scene }) end)\n";
    s += "\t" + g + ".Log(\"BRIDGE look npc=\" .. id .. \" doing=\" .. (" + g + ".lastAnim[id] or \"none\") .. scene)\n";
    s += "end\n";
    s += "\n";
    s += "function " + g + ".AddConversationAction(npc, user, firstFast, output)\n";
    s += "\tif user == nil or user.actor == nil or npc.actor == nil then return end\n";
    s += "\tif " + g + ".namedSouls[npc:GetName()] ~= true then return end\n";
    s += "\tif not user.actor:CanInteractWith(npc.id) then return end\n";
    s += "\tif npc.actor:IsDead() or npc.actor:IsUnconscious() then return end\n";
    s += "\n";
    s += "\t-- The whole reason the activity is readable at all: this runs while the NPC is\n";
    s += "\t-- only being looked at, so the saw is still in their hands. Remember comes\n";
    s += "\t-- first and unconditionally, because the log line below is throttled and the\n";
    s += "\t-- app needs the freshest reading regardless of what was last said.\n";
    s += "\t" + g + ".Remember(npc)\n";
    // Not keyed on the soul, so this says itself once and then only again when the hour
    // rolls over -- the point is which of the four spellings answers, not how often.
    s += "\tlocal clock = " + g + ".Clock()\n";
    s += "\t" + g + ".LogOnce(\"clock \" .. clock, \"clock \" .. clock)\n";
    s += "\t-- Keyed on the reading and not just the soul, so a villager who sits down or\n";
    s += "\t-- picks up a saw reports it, while one who carries on doing the same thing\n";
    s += "\t-- stays quiet. Looking at an NPC calls this every frame.\n";
    s += "\tlocal doing = " + g + ".Doing(npc)\n";
    s += "\t" + g + ".LogOnce(\"doing \" .. npc:GetName() .. \" \" .. doing,\n";
    s += "\t\t\"doing \" .. npc:GetName() .. \" \" .. doing)\n";
    s += "\tpcall(function() " + g + ".QuestProbe(\"world\", npc, user) end)\n";
    s += "\t-- A quest's holder is given their menu's role the first time they are looked\n";
    s += "\t-- at, so that the game has its prompt ready by the time the quest is accepted.\n";
    s += "\t-- On the frame that grants it, nothing is offered: the engine's own prompt\n";
    s += "\t-- cannot have caught up yet, and the menu is rebuilt next frame anyway.\n";
    s += "\tlocal grantedNow = false\n";
    s += "\tpcall(function() grantedNow = " + g + ".GrantHolder(npc) end)\n";
    s += "\tif grantedNow then return end\n";
    // The engine's own answer to "is there a dialogue to enter here", which is what puts
    // the base Talk on the menu. Logged for the two souls of a quest only, and again only
    // when it changes, because it is the one reading that says whether the holder's menu
    // is live: false on a holder with the errand accepted means either the quest never
    // went Active or the engine does not count our dialogue, and nothing else in the log
    // tells those apart from a menu that simply was not offered.
    s += "\tif " + g + ".questsBySoul[npc:GetName()] ~= nil then\n";
    s += "\t\t-- The holder menus' globals, if a load left them unset (Variables may not be\n";
    s += "\t\t-- ready when OnLoad runs); by the time somebody is looked at, it is.\n";
    s += "\t\tif not " + g + ".flagsSynced then pcall(" + g + ".QuestFlags) end\n";
    s += "\t\tlocal ok, can = pcall(function() return npc.actor:CanTalk(user.id) end)\n";
    s += "\t\tlocal said = ok and tostring(can) or (\"raised \" .. tostring(can))\n";
    s += "\t\t" + g + ".LogOnce(\"cantalk \" .. npc:GetName() .. \" \" .. said,\n";
    s += "\t\t\t\"BRIDGE quest CanTalk on \" .. npc:GetName() .. \" -> \" .. said)\n";
    s += "\tend\n";
    s += "\n";
    s += "\t-- Give way to real dialogue, whoever's it is: a soul the game offers its own\n";
    s += "\t-- Talk for has something to say and gets to say it, whether that is a shipped\n";
    s += "\t-- line, a trader's counter, or the errand this mod's own quest gave them. Our\n";
    s += "\t-- held dialogue cannot be what put the prompt there, because every one of its\n";
    s += "\t-- entries is gated on the talking global and that is only up mid-conversation.\n";
    s += "\t-- Greyed out counts too: the game grey out its prompt for a crime or a fight,\n";
    s += "\t-- and a second prompt that works when the first does not would be a way round.\n";
    s += "\tfor i = 1, #output do\n";
    s += "\t\tif output[i].action == " + g + ".action then\n";
    s += "\t\t\t" + g + ".LogOnce(npc:GetName(),\n";
    s += "\t\t\t\t\"leaving the base talk option alone on \" .. npc:GetName())\n";
    s += "\t\t\treturn\n";
    s += "\t\tend\n";
    s += "\tend\n";
    s += "\n";
    s += "\t-- This soul is one the conversation would be offered on, so the app is told\n";
    s += "\t-- they are being looked at. Before the firstFast return, which is most frames.\n";
    s += "\tpcall(" + g + ".Look, npc, user)\n";
    s += "\n";
    s += "\t-- firstFast asks only whether anything at all is on offer and stops at the\n";
    s += "\t-- first hit, so appending a second entry there would break the contract.\n";
    s += "\tif firstFast and #output > 0 then return end\n";
    s += "\n";
    s += "\t-- The giver of a favour still to be taken up gets the favour's own prompt; the\n";
    s += "\t-- save decides, so it goes out when the errand is accepted and never comes back\n";
    s += "\t-- once it is done -- and on a chain it lights again when the next link comes\n";
    s += "\t-- due (QuestOfferable). Holders and everyone else keep the plain one.\n";
    s += "\tlocal hint = " + g + ".hint\n";
    s += "\tpcall(function()\n";
    s += "\t\tfor _, qid in ipairs(" + g + ".questsBySoul[npc:GetName()] or {}) do\n";
    s += "\t\t\tlocal q = " + g + ".quests[qid]\n";
    s += "\t\t\tif q ~= nil and q.giver == npc:GetName() and " + g + ".QuestOfferable(qid) then\n";
    s += "\t\t\t\thint = " + g + ".favourHint\n";
    s += "\t\t\tend\n";
    s += "\t\tend\n";
    s += "\tend)\n";
    s += "\n";
    s += "\t-- uiOrder and interaction copy the base game's talk entry, so the prompt reads\n";
    s += "\t-- and sits where a player already expects one.\n";
    s += "\tAddInteractorAction(output, false, Action()\n";
    s += "\t\t:hint(hint)\n";
    s += "\t\t:action(" + g + ".action)\n";
    s += "\t\t:uiOrder(0)\n";
    s += "\t\t:interaction(inr_talk)\n";
    s += "\t\t:func(" + g + ".OnConversation)\n";
    s += "\t\t:enabled(true))\n";
    s += "end\n";
    s += "\n";
    s += "-- The game rules' start-of-level callback, which the engine makes once the level is\n";
    s += "-- up rather than while it is arriving. Patched again whenever the table is a new one,\n";
    s += "-- since the rules script may be run afresh for a level. False when there is nothing\n";
    s += "-- to patch yet.\n";
    s += "function " + g + ".PatchRules()\n";
    s += "\tlocal rules = rawget(_G, \"SinglePlayer\")\n";
    s += "\tif type(rules) ~= \"table\" or type(rules.Server) ~= \"table\" or type(rules.Server.OnStartLevel) ~= \"function\" then\n";
    s += "\t\treturn false\n";
    s += "\tend\n";
    s += "\tif " + g + ".rulesTable == rules then return true end\n";
    s += "\t" + g + ".rulesTable = rules\n";
    s += "\tlocal baseStart = rules.Server.OnStartLevel\n";
    s += "\trules.Server.OnStartLevel = function(self, ...)\n";
    s += "\t\tpcall(" + g + ".OnWorld, \"OnStartLevel\", self)\n";
    s += "\t\treturn baseStart(self, ...)\n";
    s += "\tend\n";
    s += "\t" + g + ".Log(\"patched SinglePlayer.Server.OnStartLevel\")\n";
    s += "\treturn true\n";
    s += "end\n";
    s += "\n";
    s += "-- Returns how many classes are still waiting to be patched.\n";
    s += "function " + g + ".Install()\n";
    s += "\tlocal pending = 0\n";
    // Wrapped the same way the NPC classes are, and for the same reason: the shipped
    // function has to keep running, since the pause menu and anything else the engine
    // routes through it are not ours to break.
    s += "\tif Player == nil or Player.OnAction == nil then\n";
    s += "\t\tpending = pending + 1\n";
    s += "\telseif not Player.moonshineActionPatched then\n";
    s += "\t\tlocal base = Player.OnAction\n";
    s += "\t\tPlayer.moonshineActionPatched = true\n";
    s += "\t\tPlayer.OnAction = function(self, action, activation, value)\n";
    s += "\t\t\tpcall(" + g + ".AutoHeartbeat, \"OnAction\")\n";
    // The console pump's surest heartbeat: Client.OnUpdate never came on the Windows laptop and GetActions
    // comes only with something usable in view, but every key the player presses lands here
    // (measured 5 Oct 2026: a loaded world sat three minutes with the pump unarmed until a
    // key was pressed). Cheap while the chain lives; Console returns at once then.
    s += "\t\t\tpcall(" + g + ".Console, \"OnAction\")\n";
    // And the quest watch's, for the loop a load wiped (WatchArm); a comparison of two
    // numbers while it lives.
    s += "\t\t\tpcall(" + g + ".WatchArm, \"OnAction\")\n";
    // Ours stop here; everything else goes on to the shipped handler. An error in ours
    // counts as not ours, so the game is never denied an action by a bug in this file.
    s += "\t\t\tlocal ok, taken = pcall(" + g + ".OnPlayerAction, action, activation)\n";
    s += "\t\t\tif ok and taken == true then return end\n";
    s += "\t\t\treturn base(self, action, activation, value)\n";
    s += "\t\tend\n";
    s += "\t\t" + g + ".Log(\"patched Player.OnAction\")\n";
    s += "\tend\n";
    // The save hooks, wrapped the same way. The shipped Player:OnSaveAI puts self.AI on
    // the table and OnLoadAI takes it back, but the build never calls either on the
    // player; the engine's own script-proxy pair, OnSave/OnLoad, is what fires (measured
    // on every save and load of the spike), so both pairs are wrapped (or defined, since
    // Player has no OnSave/OnLoad), each labelled, and the log says which one ran. Ours
    // run after the shipped body and are pcall'd, so a fault in the store can never cost
    // the player a save.
    s += "\tif Player == nil then\n";
    s += "\t\tpending = pending + 1\n";
    s += "\telseif not Player.moonshineSavePatched then\n";
    s += "\t\tPlayer.moonshineSavePatched = true\n";
    s += "\t\tlocal function wrapSave(name)\n";
    s += "\t\t\tlocal base = Player[name]\n";
    s += "\t\t\tPlayer[name] = function(self, save)\n";
    s += "\t\t\t\tif base ~= nil then base(self, save) end\n";
    s += "\t\t\t\tlocal ok, err = pcall(" + g + ".OnSave, save, name)\n";
    s += "\t\t\t\tif not ok then " + g + ".Log(\"BRIDGE state save raised \" .. tostring(err)) end\n";
    s += "\t\t\tend\n";
    s += "\t\tend\n";
    s += "\t\tlocal function wrapLoad(name)\n";
    s += "\t\t\tlocal base = Player[name]\n";
    s += "\t\t\tPlayer[name] = function(self, saved)\n";
    s += "\t\t\t\tif base ~= nil then base(self, saved) end\n";
    s += "\t\t\t\tlocal ok, err = pcall(" + g + ".OnLoad, saved, name)\n";
    s += "\t\t\t\tif not ok then " + g + ".Log(\"BRIDGE state load raised \" .. tostring(err)) end\n";
    s += "\t\t\tend\n";
    s += "\t\tend\n";
    s += "\t\twrapSave(\"OnSaveAI\")\n";
    s += "\t\twrapLoad(\"OnLoadAI\")\n";
    s += "\t\twrapSave(\"OnSave\")\n";
    s += "\t\twrapLoad(\"OnLoad\")\n";
    // The player's spawn-time callbacks are the one sure sign a world has arrived, since
    // no load hook fired for the player on the Windows laptop and timers armed before a world never run.
    s += "\t\tlocal function wrapArrive(name)\n";
    s += "\t\t\tlocal base = Player[name]\n";
    s += "\t\t\tif base == nil then return end\n";
    s += "\t\t\tPlayer[name] = function(self, ...)\n";
    s += "\t\t\t\tpcall(" + g + ".OnWorld, name, self)\n";
    s += "\t\t\t\treturn base(self, ...)\n";
    s += "\t\t\tend\n";
    s += "\t\tend\n";
    s += "\t\twrapArrive(\"OnInit\")\n";
    s += "\t\twrapArrive(\"OnSpawn\")\n";
    s += "\t\twrapArrive(\"OnReset\")\n";
    // An entity timer on the player, armed from OnInit (see OnWorld), lands here if
    // entity timers outlive the end of loading where script timers do not.
    // Per-frame control in a world: the engine calls Client.OnUpdate on an entity script
    // that has one. It is the autopilot's clock, and nothing else runs from it.
    s += "\t\tif type(Player.Client) == \"table\" then\n";
    s += "\t\t\tlocal baseUpdate = Player.Client.OnUpdate\n";
    s += "\t\t\tPlayer.Client.OnUpdate = function(self, frameTime)\n";
    s += "\t\t\t\tlocal a = " + g + ".auto\n";
    s += "\t\t\t\tif a.dueAt ~= nil or a.polling or a.awaitLoad or not " + g + ".heartbeatSources.OnUpdate then\n";
    s += "\t\t\t\t\tpcall(" + g + ".AutoHeartbeat, \"OnUpdate\")\n";
    s += "\t\t\t\tend\n";
    // The console pump's chance to arm once the player is in the world and moving; a
    // comparison of two numbers per frame while the chain lives (Console).
    s += "\t\t\t\tpcall(" + g + ".Console, \"OnUpdate\")\n";
    s += "\t\t\t\tif baseUpdate ~= nil then return baseUpdate(self, frameTime) end\n";
    s += "\t\t\tend\n";
    s += "\t\tend\n";
    s += "\t\tlocal baseTimer = Player.OnTimer\n";
    s += "\t\tPlayer.OnTimer = function(self, timerId, msec)\n";
    s += "\t\t\tif timerId == 31337 then pcall(" + g + ".OnWorld, \"OnTimer\") end\n";
    s += "\t\t\tif baseTimer ~= nil then return baseTimer(self, timerId, msec) end\n";
    s += "\t\tend\n";
    s += "\t\t" + g + ".Log(\"patched Player.OnSaveAI, OnLoadAI, OnSave, OnLoad, OnInit, OnSpawn, OnReset, OnTimer\")\n";
    s += "\tend\n";
    // The game rules' start-of-level callback, which the engine makes once the level is
    // up rather than while it is arriving.
    // The flag is ours, not a field on the rules table: a field read there came back
    // truthy on a fresh table (an __index fallback, presumably) and the patch was skipped.
    s += "\tif " + g + ".PatchRules() == false then pending = pending + 1 end\n";
    s += "\tfor _, className in ipairs(" + g + ".classNames) do\n";
    s += "\t\tlocal class = _G[className]\n";
    s += "\t\tif class == nil or class.GetActions == nil then\n";
    s += "\t\t\tpending = pending + 1\n";
    s += "\t\telseif not class.moonshinePatched then\n";
    s += "\t\t\tlocal base = class.GetActions\n";
    s += "\t\t\tclass.moonshinePatched = true\n";
    s += "\t\t\tclass.GetActions = function(npc, user, firstFast)\n";
    s += "\t\t\t\tpcall(" + g + ".AutoHeartbeat, \"GetActions\")\n";
    s += "\t\t\t\tpcall(" + g + ".Console, \"GetActions\")\n";
    s += "\t\t\t\tlocal output = base(npc, user, firstFast)\n";
    s += "\t\t\t\t" + g + ".AddConversationAction(npc, user, firstFast, output)\n";
    s += "\t\t\t\treturn output\n";
    s += "\t\t\tend\n";
    s += "\t\t\t" + g + ".Log(\"patched \" .. className)\n";
    s += "\t\tend\n";
    s += "\tend\n";
    s += "\treturn pending\n";
    s += "end\n";
    s += "\n";
    s += "-- The mod entry point runs while the main menu is still loading, so the entity\n";
    s += "-- classes are usually but not certainly registered by now. The budget is\n";
    s += "-- generous because the menu can sit idle for as long as the player likes.\n";
    s += "function " + g + ".InstallWhenReady(attempt)\n";
    s += "\tattempt = attempt or 1\n";
    s += "\tif " + g + ".Install() == 0 then\n";
    s += "\t\t" + g + ".Log(\"conversation option ready after \" .. attempt .. \" attempt(s)\")\n";
    s += "\t\tpcall(function() " + g + ".QuestProbe(\"install\", nil, nil) end)\n";
    s += "\t\tlocal autoOk, autoErr = pcall(" + g + ".Autopilot)\n";
    s += "\t\tif not autoOk then " + g + ".Log(\"BRIDGE auto raised \" .. tostring(autoErr)) end\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    s += "\tif attempt >= 900 then\n";
    s += "\t\t" + g + ".Log(\"giving up: NPC entity classes never appeared\")\n";
    s += "\t\treturn\n";
    s += "\tend\n";
    s += "\t" + g + ".installAttempt = attempt + 1\n";
    s += "\tScript.SetTimer(1000, " + g + ".Retry)\n";
    s += "end\n";
    s += "\n";
    s += "function " + g + ".Retry()\n";
    s += "\t" + g + ".InstallWhenReady(" + g + ".installAttempt)\n";
    s += "end\n";
    s += "\n";
    s += g + ".Log(\"script loaded, \" .. " + g + ".soulCount .. \" named souls\")\n";
    s += g + ".DiskStart()\n";
    s += "pcall(" + g + ".ConsoleRegister)\n";
    s += "pcall(" + g + ".LoadTyping)\n";
    s += g + ".InstallWhenReady()\n";
    return s;
}

std::string typing_actionmaps_path(const std::string& modid) {
    return "Libs/Config/" + modid + "_typing.xml";
}

std::string emit_typing_actionmaps(const std::string& modid) {
    // The names the Lua enables and disables by. Prefixed with the modid because
    // CActionMapManager::LoadFromXML warns and skips a map whose name is already taken,
    // and a second mod trying the same trick should not silence this one.
    const std::string toggle = modid + "_typing_toggle";
    const std::string keys = modid + "_typing";

    // The keys a line is made of.
    const auto letters = [](std::string& out) {
        // Backspace repeats while held, at the rate a text field would.
        out += "\t\t<action name=\"mtype_backspace\" onPress=\"1\" onHold=\"1\" "
               "holdTriggerDelay=\"0.4\" holdRepeatDelay=\"0.05\" keyboard=\"backspace\" />\n";
        out += "\t\t<action name=\"mtype_space\" onPress=\"1\" keyboard=\"space\" />\n";
        // Both edges, since shift is state rather than a keystroke.
        out += "\t\t<action name=\"mtype_lshift\" onPress=\"1\" onRelease=\"1\" keyboard=\"lshift\" />\n";
        out += "\t\t<action name=\"mtype_rshift\" onPress=\"1\" onRelease=\"1\" keyboard=\"rshift\" />\n";
        for (const char* name : {"comma", "period", "apostrophe", "minus", "slash", "semicolon"}) {
            out += "\t\t<action name=\"mtype_" + std::string(name) + "\" onPress=\"1\" keyboard=\"" +
                   name + "\" />\n";
        }
        for (char c = '0'; c <= '9'; ++c) {
            out += "\t\t<action name=\"mtype_" + std::string(1, c) + "\" onPress=\"1\" keyboard=\"" +
                   std::string(1, c) + "\" />\n";
        }
        for (char c = 'a'; c <= 'z'; ++c) {
            out += "\t\t<action name=\"mtype_" + std::string(1, c) + "\" onPress=\"1\" keyboard=\"" +
                   std::string(1, c) + "\" />\n";
        }
    };

    std::string s;
    s += "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
    s += "<!-- " + modid + ": a keyboard, as actionmaps. Generated by kcd2modgen.\n";
    s += "  The root tag and version attribute mirror Libs/Config/defaultProfile.xml, since\n";
    s += "  LoadFromXML refuses a root without \"version\" and reads <actionmap> children off\n";
    s += "  it. Priorities are names from that file's <priorities> block: \"cheats\" (13) is\n";
    s += "  what the game's own debug maps use so they survive every exclusive gameplay map,\n";
    s += "  and \"cheats_haste\" (14) is where its own exclusive haste map sits. F9 is the one\n";
    s += "  F-key no shipped map binds.\n";
    s += "  An exclusive map silences every map under it for every key (that is how the\n";
    s += "  game's own dialog map takes movement away), so when the keyboard is open from the\n";
    s += "  menu's say row the pad's A and B and the mouse button are bound here again under\n";
    s += "  the dialogue's own action names: A or the button sends the line as Enter does, B\n";
    s += "  clears it or brings the menu back as escape does. The names reach the mod's\n";
    s += "  OnAction only; the engine's dialogue does not listen to a map that is not its own. -->\n";
    s += "<profile version=\"0\">\n";
    s += "\t<actionmap name=\"" + toggle + "\" priority=\"cheats\" exclusivity=\"0\">\n";
    s += "\t\t<action name=\"mtype_toggle\" onPress=\"1\" keyboard=\"f9\" />\n";
    s += "\t</actionmap>\n";
    s += "\t<actionmap name=\"" + keys + "\" priority=\"cheats_haste\" exclusivity=\"1\">\n";
    s += "\t\t<action name=\"mtype_toggle\" onPress=\"1\" keyboard=\"f9\" />\n";
    s += "\t\t<action name=\"mtype_enter\" onPress=\"1\" keyboard=\"enter\" />\n";
    s += "\t\t<action name=\"mtype_escape\" onPress=\"1\" keyboard=\"escape\" />\n";
    s += "\t\t<action name=\"dialog_confirm\" onPress=\"1\" onRelease=\"1\" xboxpad=\"xi_a\" mouse=\"mouse1\" />\n";
    s += "\t\t<action name=\"dialog_cancel\" onPress=\"1\" onRelease=\"1\" xboxpad=\"xi_b\" />\n";
    letters(s);
    s += "\t</actionmap>\n";
    s += "</profile>\n";
    return s;
}

std::string emit_manifest(const std::string& modid,
                          const std::string& name,
                          const std::string& description,
                          const std::string& version,
                          const std::string& author,
                          const std::string& kcd_version,
                          const std::string& created_on) {
    std::string s;
    s += "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n";
    s += "<kcd_mod>\r\n";
    s += "\t<info>\r\n";
    s += "\t\t<name>" + xml_escape(name) + "</name>\r\n";
    s += "\t\t<modid>" + xml_escape(modid) + "</modid>\r\n";
    s += "\t\t<description>" + xml_escape(description) + "</description>\r\n";
    s += "\t\t<author>" + xml_escape(author) + "</author>\r\n";
    s += "\t\t<version>" + xml_escape(version) + "</version>\r\n";
    s += "\t\t<created_on>" + xml_escape(created_on) + "</created_on>\r\n";
    s += "\t\t<modifies_level>false</modifies_level>\r\n";
    s += "\t</info>\r\n";
    s += "\t<supports>\r\n";
    s += "\t\t<kcd_version>" + xml_escape(kcd_version) + "</kcd_version>\r\n";
    s += "\t</supports>\r\n";
    s += "</kcd_mod>\r\n";
    return s;
}

}  // namespace kcd2
