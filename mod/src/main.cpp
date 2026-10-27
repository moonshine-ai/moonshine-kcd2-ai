// Builds the moonshine_names KCD2 mod from the persona names in npcs.sqlite.
//
// Most of the mod is data: a STORM rules file that renames one soul each via
// <hasName>/<setUiName>, plus the localization table those names resolve
// through. The one moving part is a Lua file that adds a conversation option to
// the interactor menu of exactly the souls named here.

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "emit.hpp"
#include "npcdb.hpp"
#include "pak.hpp"

namespace {

struct Options {
    std::string db = "data/kcd2/npcs.sqlite";
    std::string out = "build/mods/moonshine_names";
    std::string modid = "moonshine_names";
    std::string language = "English";
    std::string version = "1.0";
    // Matched against the game's wh_sys_version, which is "1.5.6" on retail 1.5.
    // A bare "1.5" matches nothing and the engine disables the mod outright, so
    // this has to be a wildcard.
    std::string kcd_version = "1.5.*";
    std::string author = "moonshine-voice-characters";
    // What the manifest says the mod is, as the game's save and a Workshop page show it.
    // Settable so a tester build can say what it needs (the companion app) without a
    // rebuild of the generator; the defaults are the words the mod has always carried.
    std::string name = "Moonshine NPC Names";
    std::string description =
        "Period-appropriate personal names for the unnamed NPCs of Kuttenberg and Trosky, "
        "and a conversation option on each of them.";
    std::string created_on;
    std::string settlements_file;
    std::string households_file;
    std::string souls_file;
    // Files that are not generated from the database but ship in the data pak as they
    // are (mod/assets): the wildman costume's table fragments, materials and
    // textures, and the fire particle library. Every file under the directory goes in
    // at its path relative to it, and the rows of the text file beside it join the
    // localization table.
    std::string assets_dir;
    std::string assets_text;
    int rule_order = 0;
    // Souls with no dialogue never offer the base game's talk prompt, so the
    // focused dialogue camera and UI are unreachable for them. Shipping a dialogue
    // of our own and granting each soul the role that speaks it is what opens that
    // door. Off by default because it changes what these NPCs will do, not just
    // what they are called.
    bool dialogue = false;
    std::string dialogue_souls_file;
    // The small quests the bio generator dealt between talkable souls. They ride on
    // the dialogue -- a quest's ports are fired by its sequences -- so they need it,
    // and are shipped whenever it is unless told not to: a quest between souls that
    // cannot be talked to has no way to be offered.
    bool quests = true;
    // Invented, but unlike a setUiName key a role has to be declared in
    // Libs/Tables/rpg/role before anything can grant it, so the mod ships that
    // declaration too. The metarole is the group holding it, and needs a name of
    // its own because it is the handle a running game has: STORM cannot reach a
    // soul a save already created, while soul:AddMetaRoleByName can.
    std::string dialogue_role = "MOONSHINE_SPEAKER";
    std::string dialogue_metarole = "MOONSHINE_TALKER";
    // How long one turn of the holding loop lasts, in seconds. Nothing is said in
    // that time -- the words go through Game.SendInfoText and keep their own time on
    // screen -- so this is only how often the loop comes back to look at whether the
    // conversation is still wanted, and how long a player who has walked away might
    // wait to be let go.
    //
    // It is not free to make small: a wordless response with nothing waiting on it
    // finishes in the frame it starts, and a loop of those spins as fast as the game
    // draws -- measured at 0.06s a turn before the wait was added.
    //
    // Six, because that is what the loop was actually doing when it worked: two
    // responses of three, back when a response with a line to say took its
    // ReferenceLength. The wait now sits on the second response alone, so this is the
    // whole turn rather than half of it.
    double beat_seconds = 6;
    // Shown in the interactor menu, so it goes through the localization table. "Chat"
    // since 4 Oct 2026 (was "Start a conversation"): the game's own one-word labels.
    std::string conversation_hint = "Chat";
    // On a giver with a favour to ask, in place of the above. The game's own "Talk"
    // since 4 Oct 2026 (was "Hear them out"): with the giver now marked on the map like
    // any of the game's, the label matches the marker; the design plan's §5.5 worry
    // about cheapening the game's Talk gave way to the player's expectation.
    std::string favour_hint = "Talk";
    // Shown on the HUD on its own as a conversation starts, if set. It was
    // "(tap B to skip, hold to leave)" while holding the button was the only way out of
    // a focused conversation; now that Esc leaves from the keyboard, it is off by
    // default and kept as an option for a build without one.
    std::string leave_hint = "";
    // Which road the words take to the screen, and where they land if they are drawn.
    kcd2::ScreenText screen;
};

void usage() {
    std::cout <<
        "kcd2modgen -- build the moonshine_names KCD2 mod\n"
        "\n"
        "  --db PATH           npcs.sqlite to read (default data/kcd2/npcs.sqlite)\n"
        "  --out DIR           mod folder to write (default build/mods/moonshine_names)\n"
        "  --modid ID          mod id, lowercase letters and underscores only\n"
        "\n"
        "Scope filters, ANDed together, each a file with one value per line.\n"
        "Omit all of them to rename every soul that has a generated name.\n"
        "  --settlements FILE  only souls in these npc.settlement values\n"
        "  --households FILE   only souls in these generated_name.household_key values\n"
        "  --souls FILE        only these soul_names\n"
        "\n"
        "\n"
        "A dialogue of our own, which is what makes these souls talkable and so\n"
        "reachable by the focused dialogue camera and UI. Changes NPC behaviour\n"
        "rather than just their names, so it is opt-in.\n"
        "  --dialogue          ship a dialogue and grant each soul the role for it\n"
        "  --dialogue-souls FILE  restrict that grant to these soul_names\n"
        "  --no-quests            leave out the generated quests (npc_quest rows with\n"
        "                         text whose giver and holder are both talkable) that\n"
        "                         --dialogue otherwise ships beside the dialogue\n"
        "  --dialogue-role NAME   role the dialogue speaks through, default\n"
        "                         MOONSHINE_SPEAKER\n"
        "  --dialogue-metarole NAME  group that role belongs to, and the only\n"
        "                         handle a running game has on it, default\n"
        "                         MOONSHINE_TALKER\n"
        "  --beat-seconds N       how long one turn of the holding loop lasts, which\n"
        "                         is how often it looks at whether the conversation\n"
        "                         is still wanted. Face switching is capped at 2.5s\n"
        "                         so Speak() can start and stop on a short jaw cycle;\n"
        "                         this value still wins when it is shorter. Waited\n"
        "                         out on the second of the beat's two responses,\n"
        "                         since a wait on the first holds up the fade that\n"
        "                         opens a session (default 6)\n"
        "\n"
        "  --rule-order N      write <rule order=\"N\">; omit to rely on source order\n"
        "  --hint TEXT         interactor menu label for the conversation option\n"
        "  --favour-hint TEXT  the label on a giver whose favour is still to be taken up\n"
        "  --text-mode WAY     how a line reaches the screen: caption for the dialogue\n"
        "                      caption itself through UIAction SetSubtitles, which is the\n"
        "                      game's own face, background and placing; draw for\n"
        "                      System.DrawText, which puts it where it is told in a face\n"
        "                      the mod ships; or info for Game.SendInfoText, the game's\n"
        "                      notice widget (default caption)\n"
        "  --draw-x N          where drawn text sits across the screen, as a fraction\n"
        "                      of it, taking the middle of the line (default 0.5)\n"
        "  --draw-y N          and down it, taking the top (default 0.82)\n"
        "  --draw-size N       drawn text size, picked off a ladder of six as the one\n"
        "                      nearest the game's own subtitles (default 3)\n"
        "  --draw-font NAME    the Fonts/<name>.xml the mod ships over the engine's own,\n"
        "                      which is the only way the face can be chosen since\n"
        "                      DrawText has no font parameter (default default)\n"
        "  --draw-face TTF     the face to put in it (default AlexanderQuill.ttf, the\n"
        "                      only period one the engine carries)\n"
        "  --draw-pixels N     pixels a character stands per unit of size, which only\n"
        "                      centring uses and nothing can measure (default 9)\n"
        "  --leave-hint TEXT   shown on its own as a conversation starts, saying how\n"
        "                      to end it; empty (the default) shows nothing\n"
        "  --assets DIR        ship every file under DIR in the data pak at its\n"
        "                      relative path, as it is (table fragments, .mtl, .dds)\n"
        "  --assets-text FILE  key<TAB>text rows for the localization table, one per\n"
        "                      line, the names of whatever --assets adds\n"
        "  --language NAME     localization pak language (default English)\n"
        "  --version V         mod version (default 1.0)\n"
        "  --kcd-version V     game version to support, wildcards ok (default 1.5.*)\n"
        "  --author NAME       mod author\n"
        "  --name TEXT         the mod's name in the manifest (default Moonshine NPC Names)\n"
        "  --description TEXT  the manifest's description, one paragraph\n"
        "  --created-on DATE   YYYY-MM-DD, defaults to today\n"
        "  -h, --help          this text\n";
}

// Blank lines and # comments are ignored so a filter file can explain itself.
std::vector<std::string> read_lines(const std::string& path) {
    std::ifstream f(path);
    if (!f) {
        throw std::runtime_error("cannot read " + path);
    }
    std::vector<std::string> out;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::size_t begin = line.find_first_not_of(" \t");
        if (begin == std::string::npos) continue;
        std::size_t end = line.find_last_not_of(" \t");
        std::string trimmed = line.substr(begin, end - begin + 1);
        if (trimmed[0] == '#') continue;
        out.push_back(trimmed);
    }
    return out;
}

std::string today() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1,
                  tm.tm_mday);
    return buf;
}

bool valid_modid(const std::string& id) {
    if (id.empty()) return false;
    return std::all_of(id.begin(), id.end(),
                       [](unsigned char c) { return (c >= 'a' && c <= 'z') || c == '_'; });
}

Options parse_args(int argc, char** argv, bool* want_help) {
    Options o;
    const std::map<std::string, std::string*> string_opts = {
        {"--db", &o.db},
        {"--out", &o.out},
        {"--modid", &o.modid},
        {"--settlements", &o.settlements_file},
        {"--households", &o.households_file},
        {"--souls", &o.souls_file},
        {"--dialogue-souls", &o.dialogue_souls_file},
        {"--dialogue-role", &o.dialogue_role},
        {"--dialogue-metarole", &o.dialogue_metarole},
        {"--assets", &o.assets_dir},
        {"--assets-text", &o.assets_text},
        {"--language", &o.language},
        {"--version", &o.version},
        {"--kcd-version", &o.kcd_version},
        {"--author", &o.author},
        {"--name", &o.name},
        {"--description", &o.description},
        {"--created-on", &o.created_on},
        {"--hint", &o.conversation_hint},
        {"--favour-hint", &o.favour_hint},
        {"--leave-hint", &o.leave_hint},
        {"--text-mode", &o.screen.mode},
        {"--draw-font", &o.screen.font},
        {"--draw-face", &o.screen.face},
    };
    const std::map<std::string, double*> number_opts = {
        {"--beat-seconds", &o.beat_seconds},
        {"--draw-x", &o.screen.x},
        {"--draw-y", &o.screen.y},
        {"--draw-size", &o.screen.size},
        {"--draw-pixels", &o.screen.pixels},
    };
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            *want_help = true;
            return o;
        }
        auto it = string_opts.find(arg);
        if (it != string_opts.end()) {
            if (i + 1 >= argc) throw std::runtime_error(arg + " needs a value");
            *it->second = argv[++i];
            continue;
        }
        if (arg == "--dialogue") {
            o.dialogue = true;
            continue;
        }
        if (arg == "--no-quests") {
            o.quests = false;
            continue;
        }
        if (arg == "--rule-order") {
            if (i + 1 >= argc) throw std::runtime_error(arg + " needs a value");
            o.rule_order = std::stoi(argv[++i]);
            continue;
        }
        auto num = number_opts.find(arg);
        if (num != number_opts.end()) {
            if (i + 1 >= argc) throw std::runtime_error(arg + " needs a value");
            *num->second = std::stod(argv[++i]);
            continue;
        }
        throw std::runtime_error("unknown argument " + arg);
    }
    return o;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        bool want_help = false;
        Options o = parse_args(argc, argv, &want_help);
        if (want_help) {
            usage();
            return 0;
        }
        if (!valid_modid(o.modid)) {
            throw std::runtime_error("modid '" + o.modid +
                                     "' must be lowercase letters and underscores only");
        }
        if (o.created_on.empty()) o.created_on = today();

        kcd2::Filter filter;
        if (!o.settlements_file.empty()) filter.settlements = read_lines(o.settlements_file);
        if (!o.households_file.empty()) filter.households = read_lines(o.households_file);
        if (!o.souls_file.empty()) filter.souls = read_lines(o.souls_file);

        std::vector<kcd2::Persona> personas = kcd2::read_personas(o.db, filter);
        if (personas.empty()) {
            throw std::runtime_error("no personas matched; nothing to build");
        }

        // Two souls sharing a soul_name would mean two rules fighting over one
        // <hasName> selector, so refuse rather than ship an ambiguous mod.
        std::vector<std::string> names;
        names.reserve(personas.size());
        for (const auto& p : personas) names.push_back(p.soul_name);
        std::sort(names.begin(), names.end());
        auto dup = std::adjacent_find(names.begin(), names.end());
        if (dup != names.end()) {
            throw std::runtime_error("duplicate soul_name '" + *dup + "' in the database");
        }

        // The interactor label, plus a row per spoken line once there is a
        // dialogue to speak. Both end up in the same table as the persona names.
        std::vector<std::pair<std::string, std::string>> loc_extras = {
            {kcd2::conversation_hint_key(o.modid), o.conversation_hint},
            {kcd2::favour_hint_key(o.modid), o.favour_hint}};

        // The concept graph reaches the dialogue beside it by relative path, so
        // the node, the file and the string keys are all named from this.
        const std::string dialogue_name = "talk";

        // Where the game looks for a turn's words, tried in this order. All of them
        // work: System.LoadTextFile reached a loose file under every one of these
        // paths, which is the measurement the whole design now rests on, since it is
        // how words decided outside the game get inside it.
        //
        // A turn is a text file holding the words and nothing else -- no table, no id,
        // no wrapper -- because the call that shows them takes the text itself.
        // The turn number goes where the # is. Not a printf placeholder, because
        // %USER% is a path the engine expands itself and would be read as one.
        const std::vector<std::string> inboxes = {
            "%USER%/moonshine/turn_#.txt",
            "moonshine/turn_#.txt",
            "Data/moonshine/turn_#.txt",
            "mods/" + o.modid + "/Data/moonshine/turn_#.txt",
            "mods/" + o.modid + "/moonshine/turn_#.txt",
        };

        kcd2::Pak data_pak;
        data_pak.add("Libs/Storm/storm__" + o.modid + ".xml",
                     kcd2::emit_storm_root(o.modid, o.dialogue));
        data_pak.add("Libs/Storm/names/" + o.modid + ".xml",
                     kcd2::emit_storm_rules(o.modid, personas, o.rule_order));
        std::size_t talkable = 0;
        std::vector<kcd2::Quest> quests;
        std::size_t beats = 0;  // one per quest command in the dialogue, for the summary
        if (o.dialogue) {
            std::vector<std::string> only;
            if (!o.dialogue_souls_file.empty()) only = read_lines(o.dialogue_souls_file);
            // Quests between souls that can be talked to, which is the renamed set
            // narrowed by --dialogue-souls when there is one. A quest is offered and
            // paid off through the dialogue, so both parties have to have it. Read
            // before the roles, since each quest's holder gets a role of their own.
            if (o.quests) {
                std::vector<kcd2::Persona> talkers;
                const std::set<std::string> allowed(only.begin(), only.end());
                for (const auto& p : personas) {
                    if (allowed.empty() || allowed.count(p.soul_name)) talkers.push_back(p);
                }
                quests = kcd2::read_quests(o.db, talkers);
            }
            std::string roles = kcd2::emit_storm_roles(o.modid, personas, only, o.dialogue_role,
                                                       quests, &talkable);
            if (talkable == 0) {
                throw std::runtime_error("--dialogue-souls matched none of the renamed souls");
            }
            // Declared before either is granted or spoken through. A name missing
            // from these tables resolves to nothing at every end, and nothing
            // anywhere says so.
            data_pak.add("Libs/Tables/rpg/metarole__" + o.modid + ".xml",
                         kcd2::emit_metarole_table(o.modid, o.dialogue_metarole, quests));
            data_pak.add("Libs/Tables/rpg/role__" + o.modid + ".xml",
                         kcd2::emit_role_table(o.modid, o.dialogue_role, o.dialogue_metarole,
                                               quests));
            data_pak.add("Libs/Storm/roles/" + o.modid + ".xml", roles);

            // One dialogue, listed by the project beside it. A definition the
            // project does not name is a file the graph never reads.
            data_pak.add("Quests/" + o.modid + ".xml", kcd2::emit_dialogue_project(o.modid));
            // The Level under it, which is where the dialogue and the quests sit (the
            // journal's region comes from it), and its folder, which is where their files go.
            const std::string level_dir = "Quests/" + o.modid + "/" + kcd2::kQuestLevel + "/";
            data_pak.add("Quests/" + o.modid + "/" + kcd2::kQuestLevel + ".xml",
                         kcd2::emit_quest_level(o.modid, dialogue_name, quests));
            const std::vector<kcd2::QuestBeat> all_beats = kcd2::quest_beats(quests);
            beats = all_beats.size();
            data_pak.add(level_dir + dialogue_name + ".xml",
                         kcd2::emit_dialogue(o.modid, dialogue_name, o.dialogue_role,
                                             o.beat_seconds, all_beats));
            // One file per quest beside the dialogue, and its journal rows in the same
            // table as everything else the mod says.
            // One file per run (a story's same-giver scenes under one journal entry,
            // stages.md), named for its head; the holder's and onward's files and the
            // journal rows per scene as before.
            for (const kcd2::QuestRun& run : kcd2::quest_runs(quests)) {
                data_pak.add(level_dir + kcd2::quest_node(run.legs.front()->id) + ".xml", kcd2::emit_quest(o.modid, run));
            }
            for (const auto& q : quests) {
                for (auto& row : kcd2::quest_localization(o.modid, q)) {
                    loc_extras.push_back(std::move(row));
                }
                // And the one an ask's word is carried on to, their one row beside it.
                if (q.carries_on()) {
                    data_pak.add(level_dir + kcd2::onward_node(q.id) + ".xml", kcd2::emit_onward_dialogue(o.modid, q));
                    for (auto& row : kcd2::onward_localization(o.modid, q)) {
                        loc_extras.push_back(std::move(row));
                    }
                }
                // And the one who can let Henry in (In the Stocks' Elsbeth), her menu.
                if (q.has_helper()) {
                    data_pak.add(level_dir + kcd2::helper_node(q.id) + ".xml", kcd2::emit_helper_dialogue(o.modid, q));
                    for (auto& row : kcd2::helper_localization(o.modid, q)) {
                        loc_extras.push_back(std::move(row));
                    }
                }
                // And the giver's hail as a bark of his own (In the Stocks' Benesh).
                if (!q.hail.empty()) {
                    data_pak.add(level_dir + kcd2::hail_node(q.id) + ".xml", kcd2::emit_hail_dialogue(o.modid, q));
                }
                // And the holder's menu beside it, which the quest file lists as its own
                // definition, with the twelve lines it says. A find has no holder.
                if (!q.has_holder()) continue;
                data_pak.add(level_dir + kcd2::holder_node(q.id) + ".xml", kcd2::emit_holder_dialogue(o.modid, q));
                for (auto& row : kcd2::holder_localization(o.modid, q)) {
                    loc_extras.push_back(std::move(row));
                }
            }
            // The mod's own item rows, when a favour is about a thing the shipped tables
            // cannot put in a pocket, and their names for the inventory.
            if (const std::string items = kcd2::emit_item_table(o.modid, quests); !items.empty()) {
                data_pak.add("Libs/Tables/item/item__" + o.modid + ".xml", items);
                for (auto& row : kcd2::item_localization(o.modid, quests)) {
                    loc_extras.push_back(std::move(row));
                }
            }
            // Two rows, and the same two on every beat of every conversation. That is
            // what took the ceiling off: the cost was never the id, it was needing a
            // *new* id per turn -- an id keeps the first wording it is given, so
            // changing words meant a fresh id and a sequence authored to name it. Words
            // that change go through the caption directly and need no id at all.
            //
            // One row, and it is the prompt: a sequence with nothing to show cannot be
            // offered, and this is the only thing in the dialogue that has to be shown. The
            // responses say nothing at all now, no id and no row, because anything they said
            // would be a caption and the caption is where our own words go.
            loc_extras.push_back(
                {kcd2::dialogue_hold_key(o.modid, dialogue_name), "..."});
            // And one per row of the reply menu, the same dots: the rows are drawn over
            // with the words of the moment, and these are what shows if that ever fails.
            for (int row = 1; row <= kcd2::kMenuRows; ++row) {
                loc_extras.push_back({kcd2::menu_row_key(o.modid, row), "..."});
            }
        }
        // The keyboard a reply is typed on. Shipped with the dialogue rather than only
        // with it, because the Lua below loads it at start-up whether or not any soul is
        // talkable, and a load that finds no file warns on every launch.
        // Static assets, verbatim. Table fragments among them must carry a suffix of
        // their own (item__<modid>_wildman.xml, not item__<modid>.xml): the loader takes
        // any suffix, as the shipped item__aux.xml shows, and the generated item table
        // above already owns the plain one.
        std::size_t assets = 0;
        if (!o.assets_dir.empty()) {
            namespace fs = std::filesystem;
            const fs::path root(o.assets_dir);
            if (!fs::is_directory(root)) {
                throw std::runtime_error("--assets " + o.assets_dir + " is not a directory");
            }
            std::vector<fs::path> files;
            for (const auto& entry : fs::recursive_directory_iterator(root)) {
                if (entry.is_regular_file()) files.push_back(entry.path());
            }
            std::sort(files.begin(), files.end());
            for (const auto& file : files) {
                std::ifstream in(file, std::ios::binary);
                if (!in) throw std::runtime_error("cannot read " + file.string());
                std::string bytes((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
                data_pak.add(fs::relative(file, root).generic_string(), bytes);
                ++assets;
            }
        }
        if (!o.assets_text.empty()) {
            for (const std::string& line : read_lines(o.assets_text)) {
                const std::size_t tab = line.find('\t');
                if (tab == std::string::npos) {
                    throw std::runtime_error("--assets-text line without a tab: " + line);
                }
                loc_extras.push_back({line.substr(0, tab), line.substr(tab + 1)});
            }
        }
        data_pak.add(kcd2::typing_actionmaps_path(o.modid), kcd2::emit_typing_actionmaps(o.modid));
        data_pak.add("Scripts/mods/" + o.modid + ".lua",
                     kcd2::emit_conversation_lua(o.modid, personas, o.dialogue_role,
                                                 o.dialogue_metarole, o.leave_hint, o.screen,
                                                 inboxes, quests));
        data_pak.write(o.out + "/Data/" + o.modid + ".pak");

        kcd2::Pak loc_pak;
        loc_pak.add("text__" + o.modid + ".xml",
                    kcd2::emit_localization(o.modid, personas, loc_extras));
        loc_pak.write(o.out + "/Localization/" + o.language + "_xml.pak");

        // Only wanted by the road that draws its own words. The caption comes in the game's
        // own face, which is Scaleform's and not the engine font system's to replace.
        //
        // Outside the pak, outside the mod folder, and under Engine rather than beside it.
        // All three were found the hard way. Inside the mod's own pak it changed nothing,
        // because an engine pak outranks a mod's. Loose in the game folder it also changed
        // nothing, because the fonts LoadFont reads are not there: it prepends a bare
        // "fonts/" and that resolves against the engine root, which is where Engine.pak
        // lives -- so fonts/subtitles.xml means Engine/Fonts/subtitles.xml, and a
        // game-folder Fonts is never consulted. Loose under Engine/Fonts does win, but only
        // at sys_PakPriority = 0, which opens from disk before looking in a pak -- and that
        // setting costs about seven seconds of every level load, so it is not on any more.
        // Overriding a file a pak also holds needs it; merely being found on disk, which is
        // all the words need, is satisfied by 1.
        if (o.screen.mode == "draw") {
            kcd2::write_file(o.out + "/loose/Engine/Fonts/" + o.screen.font + ".xml",
                             kcd2::emit_font_xml(o.screen.face));
        }

        kcd2::write_file(o.out + "/mod.manifest",
                         kcd2::emit_manifest(o.modid, o.name, o.description,
                                             o.version, o.author, o.kcd_version,
                                             o.created_on));

        std::cout << "wrote " << o.out << "\n"
                  << "  souls renamed: " << personas.size() << "\n"
                  << "  rule order:    "
                  << (o.rule_order ? std::to_string(o.rule_order) : std::string("source order"))
                  << "\n"
                  << "  assets:        " << assets << " files from "
                  << (o.assets_dir.empty() ? std::string("nowhere") : o.assets_dir) << "\n"
                  << "  menu option:   " << o.conversation_hint << "\n"
                  << "  on a giver:    " << o.favour_hint << "\n"
                  << "  dialogue:      "
                  << (o.dialogue ? std::to_string(talkable) + " souls given " + o.dialogue_role +
                                       " under " + o.dialogue_metarole +
                                       ", lines unlimited, beat " +
                                       std::to_string(o.beat_seconds).substr(0, 4) + "s"
                                 : std::string("none"))
                  << "\n"
                  << "  quests:        "
                  << (!o.dialogue ? std::string("none, they need the dialogue")
                      : !o.quests ? std::string("left out")
                      : quests.empty()
                          ? std::string("none written between talkable souls")
                          : std::to_string(quests.size()) + " between talkable souls, " +
                                std::to_string(beats) + " beats in the dialogue")
                  << "\n"
                  << "  words on screen: "
                  << (o.screen.mode == "caption"
                          ? std::string("the dialogue's own caption, in the game's face, on "
                                        "its background, through UIAction SetSubtitles")
                          : o.screen.mode == "draw"
                          ? "drawn " + std::to_string(o.screen.x).substr(0, 4) + " across and " +
                                std::to_string(o.screen.y).substr(0, 4) +
                                " down the screen, size " +
                                std::to_string(o.screen.size).substr(0, 4) + " in " +
                                o.screen.face + " through Fonts/" + o.screen.font + ".xml"
                          : std::string("the game's own notice widget"))
                  << "\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "kcd2modgen: " << e.what() << "\n";
        return 1;
    }
}
