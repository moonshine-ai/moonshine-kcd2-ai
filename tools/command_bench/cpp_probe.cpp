// Standalone C++20 benchmark & embedding driver for the NPC Command Pipeline bake-off.
//
// Links directly against `moonshine_core` (`LlamaEmbedder`, `LlamaLlm`, `PersonaDb`, `prompt.h`)
// so embeddings, GBNF constrained decoding, KV-cache prefix reuse, CPU wall-clock timings,
// and process RSS match `moonshine_app` identically.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

#ifdef __APPLE__
#include <mach/mach.h>
#elif defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#endif

#include "embedder.h"
#include "json.hpp"
#include "llama.h"
#include "llamacpp.h"
#include "personadb.h"
#include "prompt.h"

namespace {

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

double current_rss_mb() {
#ifdef __APPLE__
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS) {
        return static_cast<double>(info.resident_size) / (1024.0 * 1024.0);
    }
    return 0.0;
#elif defined(_WIN32)
    PROCESS_MEMORY_COUNTERS pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        return static_cast<double>(pmc.WorkingSetSize) / (1024.0 * 1024.0);
    }
    return 0.0;
#else
    struct rusage ru {};
    if (getrusage(RUSAGE_SELF, &ru) == 0) {
        return static_cast<double>(ru.ru_maxrss) / 1024.0;
    }
    return 0.0;
#endif
}

std::string read_whole_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

const char* kCompactGrammar = R"GBNF(
root   ::= ("NONE" | ("LOCATE: " span) | ("FOLLOW: " span) | ("TRAVEL: " span)) "\n"?
span   ::= [A-Za-z0-9] [A-Za-z0-9 '.,-]{0,48}
)GBNF";

const char* kCompactSuffix =
    "\n\n(Out-of-character classifier: Does the player's last line ask for someone's CURRENT "
    "whereabouts, command you to physically FOLLOW someone, or ask to TRAVEL together to a place? "
    "Answer NONE for past/biographical questions like 'Where did X learn...' or 'Where was X born?', "
    "opinions/gossip, idioms like 'I don't follow', or normal chat. "
    "Reply with ONE line only: NONE, LOCATE: <who>, FOLLOW: <who>, or TRAVEL: <place>.)";

const char* kCompactSidecarSystem =
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

// Pre-rendered FunctionGemma developer declarations matching `chat_template.jinja` byte-for-byte
const char* kFunctionGemmaDeveloper =
    "You are a model that can do function calling with the following functions. "
    "If the player utterance is normal conversation, gossip, or a past-tense biographical question "
    "(e.g. Where was X born?), do NOT call any function and reply NO_ACTION."
    "<start_function_declaration>declaration:locate_npc{description:<escape>The player asks where a specific person, relative, or tradesperson is right now or how to find them. Do NOT call for biographical questions ('Where was X born?'), opinions, or general chat.<escape>,parameters:{properties:{target_mention:{description:<escape>Name, kinship role ('your brother'), trade ('the blacksmith'), or pronoun ('him', 'her').<escape>,type:<escape>STRING<escape>}},required:[<escape>target_mention<escape>],type:<escape>OBJECT<escape>}}<end_function_declaration>"
    "<start_function_declaration>declaration:follow_npc{description:<escape>The player commands the NPC to physically follow someone or follow the player ('Follow me'). Do NOT call for idioms like 'I do not follow your meaning'.<escape>,parameters:{properties:{target_mention:{description:<escape>Person to follow, or 'me' for the player.<escape>,type:<escape>STRING<escape>}},required:[<escape>target_mention<escape>],type:<escape>OBJECT<escape>}}<end_function_declaration>"
    "<start_function_declaration>declaration:travel_to{description:<escape>The player asks or commands the NPC to travel/journey together to a town, village, castle, or landmark.<escape>,parameters:{properties:{target_mention:{description:<escape>Destination place name (e.g. 'Trosky Castle', 'Kuttenberg').<escape>,type:<escape>STRING<escape>}},required:[<escape>target_mention<escape>],type:<escape>OBJECT<escape>}}<end_function_declaration>";

std::string format_sidecar_user(const json& row) {
    std::string settlement = row.value("settlement", "");
    if (settlement.empty()) settlement = "Bohemia";
    std::string speaker_name = row.value("speaker_name", "Villager");
    std::string out = "NPC Speaker: " + speaker_name + " (" + settlement + ")";
    if (row.contains("history") && row["history"].is_array()) {
        const auto& h = row["history"];
        const std::size_t start = h.size() > 4 ? h.size() - 4 : 0;
        for (std::size_t i = start; i < h.size(); ++i) {
            const std::string role = h[i].value("role", "user");
            const std::string who = (role == "user") ? "Player" : speaker_name;
            out += "\n" + who + ": " + h[i].value("content", "");
        }
    }
    out += "\nPlayer: " + row.value("player_utterance", "");
    return out;
}

int cmd_embed(int argc, char** argv) {
    if (argc < 5) {
        std::cerr << "Usage: moonshine_cmd_probe embed <dataset.jsonl> <out.f32> <out_stats.json> [gguf]\n";
        return 1;
    }
    const std::string dataset_path = argv[2];
    const std::string out_f32 = argv[3];
    const std::string out_stats = argv[4];
    const std::string gguf_path = (argc >= 6) ? argv[5] : "models/embeddinggemma-300m-q8_0.gguf";

    const double rss_before = current_rss_mb();
    moonshine::LlamaEmbedder embedder(gguf_path);
    const double rss_after = current_rss_mb();

    std::ifstream in(dataset_path);
    std::ofstream out(out_f32, std::ios::binary);
    if (!in || !out) throw std::runtime_error("failed opening dataset or output file");

    std::vector<double> ms_list;
    std::string line;
    int count = 0;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        json row = json::parse(line);
        const std::string text = row.value("player_utterance", "");
        const auto t0 = Clock::now();
        std::vector<float> vec = embedder.embed(text, /*query=*/true);
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        ms_list.push_back(ms);
        out.write(reinterpret_cast<const char*>(vec.data()),
                  static_cast<std::streamsize>(vec.size() * sizeof(float)));
        ++count;
        if (count % 500 == 0) {
            std::cerr << "Embedded " << count << " rows...\n";
        }
    }

    std::sort(ms_list.begin(), ms_list.end());
    const double mean_ms =
        std::accumulate(ms_list.begin(), ms_list.end(), 0.0) / std::max<std::size_t>(1, ms_list.size());
    json stats = {
        {"count", count},
        {"dim", embedder.dim()},
        {"rss_delta_mb", rss_after - rss_before},
        {"rss_total_mb", rss_after},
        {"embed_ms_mean", mean_ms},
        {"embed_ms_p50", ms_list.empty() ? 0.0 : ms_list[ms_list.size() / 2]},
        {"embed_ms_p95", ms_list.empty() ? 0.0 : ms_list[static_cast<std::size_t>(ms_list.size() * 0.95)]},
    };
    std::ofstream st(out_stats);
    st << stats.dump(2) << "\n";
    std::cout << stats.dump(2) << "\n";
    return 0;
}

int cmd_arm_a(int argc, char** argv) {
    if (argc < 5) {
        std::cerr << "Usage: moonshine_cmd_probe arm_a <dataset.jsonl> <split> <out_preds.jsonl> [limit]\n";
        return 1;
    }
    const std::string dataset_path = argv[2];
    const std::string want_split = argv[3];
    const std::string out_preds = argv[4];
    const int limit = (argc >= 6) ? std::stoi(argv[5]) : 100000;

    moonshine::PersonaDb db("data/kcd2/npcs.sqlite");
    const std::string world = read_whole_file("setting.txt");

    moonshine::Options base_opts;
    base_opts.context_tokens = 4096;
    base_opts.cpu_only = true;
    base_opts.repack_weights = false;
    base_opts.disable_thinking = true;

    const double rss_before = current_rss_mb();
    moonshine::LlamaLlm llm("models/llm/Qwen3-4B-Instruct-2507-Q4_K_M.gguf", base_opts);
    llm.hold_branch(moonshine::shared_prefix(world));
    const double rss_after_load = current_rss_mb();

    std::vector<json> rows;
    {
        std::ifstream in(dataset_path);
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty()) continue;
            json r = json::parse(line);
            if (want_split == "all" || r.value("split", "") == want_split) {
                rows.push_back(std::move(r));
            }
        }
    }
    // Group by speaker_soul so soul sheets stay warm across same-speaker turns, mirroring Session
    std::stable_sort(rows.begin(), rows.end(), [](const json& a, const json& b) {
        return a.value("speaker_soul", "") < b.value("speaker_soul", "");
    });
    if (static_cast<int>(rows.size()) > limit) rows.resize(static_cast<std::size_t>(limit));

    std::ofstream out(out_preds);
    std::string current_soul;
    moonshine::Persona persona;
    std::vector<double> total_ms_list, prefill_ms_list, decode_ms_list;

    moonshine::Options warm_opts = base_opts;
    warm_opts.reply_tokens = 1;
    warm_opts.temperature = 0.0;

    moonshine::Options cls_opts = base_opts;
    cls_opts.reply_tokens = 24;
    cls_opts.temperature = 0.0;
    cls_opts.grammar = kCompactGrammar;

    for (std::size_t i = 0; i < rows.size(); ++i) {
        const json& r = rows[i];
        const std::string soul = r.value("speaker_soul", "");
        if (soul != current_soul) {
            current_soul = soul;
            persona = db.persona(soul);
            // Warm soul sheet in KV cache just as `Session::look()` / greeting does before player speaks
            auto warm_msgs = moonshine::conversation(world, persona, {}, "");
            llm.chat(warm_msgs, warm_opts, [](std::string_view) {});
        }

        std::vector<moonshine::Message> hist;
        if (r.contains("history") && r["history"].is_array()) {
            for (const auto& m : r["history"]) {
                hist.push_back({m.value("role", "user"), m.value("content", "")});
            }
        }
        if (!hist.empty()) {
            // If multi-turn history exists, pre-warm prior turns so timing reflects the live turn only
            std::vector<moonshine::Message> prior_msgs = {
                {"system", moonshine::system_prompt(world, persona)}};
            for (const auto& m : hist) prior_msgs.push_back(m);
            llm.chat(prior_msgs, warm_opts, [](std::string_view) {});
        }

        const std::string said = r.value("player_utterance", "");
        auto msgs = moonshine::conversation(world, persona, hist, said);
        msgs.back().content += kCompactSuffix;

        std::string raw;
        const auto t0 = Clock::now();
        moonshine::Timing tim = llm.chat(msgs, cls_opts, [&](std::string_view piece) {
            raw.append(piece);
        });
        const double wall_ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        const double pf_ms = tim.prefill_seconds * 1000.0;
        const double dc_ms = tim.decode_seconds * 1000.0;
        total_ms_list.push_back(wall_ms);
        prefill_ms_list.push_back(pf_ms);
        decode_ms_list.push_back(dc_ms);

        json rec = {
            {"example_id", r.value("example_id", "")},
            {"raw", raw},
            {"wall_ms", wall_ms},
            {"prefill_ms", pf_ms},
            {"decode_ms", dc_ms},
            {"prompt_tokens", tim.prompt_tokens},
            {"cached_tokens", tim.cached_tokens},
            {"reply_tokens", tim.reply_tokens},
            {"rss_mb", current_rss_mb()},
            {"rss_load_delta_mb", rss_after_load - rss_before},
        };
        out << rec.dump() << "\n";
        if ((i + 1) % 50 == 0 || i + 1 == rows.size()) {
            std::cerr << "[Arm A] " << (i + 1) << "/" << rows.size()
                      << " last_wall_ms=" << wall_ms << " raw='" << raw << "'\n";
        }
    }
    return 0;
}

int cmd_sidecar(int argc, char** argv) {
    if (argc < 7) {
        std::cerr << "Usage: moonshine_cmd_probe sidecar <gguf> <compact|fg> <dataset.jsonl> <split> <out.jsonl> [use_grammar:0|1] [limit]\n";
        return 1;
    }
    const std::string gguf_path = argv[2];
    const std::string mode = argv[3];
    const std::string dataset_path = argv[4];
    const std::string want_split = argv[5];
    const std::string out_preds = argv[6];
    const bool use_grammar = (argc >= 8) ? (std::stoi(argv[7]) != 0) : false;
    const int limit = (argc >= 9) ? std::stoi(argv[8]) : 100000;

    moonshine::Options opts;
    opts.context_tokens = 1024;
    opts.reply_tokens = 36;
    opts.temperature = 0.0;
    opts.cpu_only = true;
    opts.repack_weights = false;
    opts.disable_thinking = true;
    if (use_grammar && mode == "compact") {
        opts.grammar = kCompactGrammar;
    }

    const double rss_before = current_rss_mb();
    moonshine::LlamaLlm llm(gguf_path, opts);
    const double rss_after_load = current_rss_mb();

    // Warm the static system/developer prefix once in the sidecar's KV cache
    {
        moonshine::Options warm = opts;
        warm.reply_tokens = 1;
        warm.grammar.clear();
        std::vector<moonshine::Message> init_msgs;
        if (mode == "fg") {
            init_msgs = {{"developer", kFunctionGemmaDeveloper}, {"user", "Player: Hello"}};
        } else {
            init_msgs = {{"system", kCompactSidecarSystem}, {"user", "Player: Hello"}};
        }
        llm.chat(init_msgs, warm, [](std::string_view) {});
    }
    const double rss_after_warm = current_rss_mb();

    std::ifstream in(dataset_path);
    std::ofstream out(out_preds);
    std::string line;
    int count = 0;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        json r = json::parse(line);
        if (want_split != "all" && r.value("split", "") != want_split) continue;

        std::vector<moonshine::Message> msgs;
        if (mode == "fg") {
            msgs = {{"developer", kFunctionGemmaDeveloper}, {"user", format_sidecar_user(r)}};
        } else {
            msgs = {{"system", kCompactSidecarSystem}, {"user", format_sidecar_user(r)}};
        }

        std::string raw;
        const auto t0 = Clock::now();
        moonshine::Timing tim = llm.chat(msgs, opts, [&](std::string_view piece) {
            raw.append(piece);
        });
        const double wall_ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();

        json rec = {
            {"example_id", r.value("example_id", "")},
            {"raw", raw},
            {"wall_ms", wall_ms},
            {"prefill_ms", tim.prefill_seconds * 1000.0},
            {"decode_ms", tim.decode_seconds * 1000.0},
            {"prompt_tokens", tim.prompt_tokens},
            {"cached_tokens", tim.cached_tokens},
            {"reply_tokens", tim.reply_tokens},
            {"rss_before_mb", rss_before},
            {"rss_after_warm_mb", rss_after_warm},
            {"rss_delta_mb", rss_after_warm - rss_before},
        };
        out << rec.dump() << "\n";
        ++count;
        if (count % 100 == 0) {
            std::cerr << "[Sidecar " << mode << "] " << count << " wall_ms=" << wall_ms
                      << " cached=" << tim.cached_tokens << "/" << tim.prompt_tokens
                      << " raw='" << raw << "'\n";
        }
        if (count >= limit) break;
    }
    return 0;
}

int cmd_quantize(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "Usage: moonshine_cmd_probe quantize <in_gguf> <out_gguf>\n";
        return 1;
    }
    llama_backend_init();
    llama_model_quantize_params qp = llama_model_quantize_default_params();
    qp.ftype = LLAMA_FTYPE_MOSTLY_Q4_K_M;
    qp.allow_requantize = true;
    const uint32_t rc = llama_model_quantize(argv[2], argv[3], &qp);
    llama_backend_free();
    return rc == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 2) {
            std::cerr << "Subcommands: embed | arm_a | sidecar | quantize\n";
            return 1;
        }
        const std::string sub = argv[1];
        if (sub == "embed") return cmd_embed(argc, argv);
        if (sub == "arm_a") return cmd_arm_a(argc, argv);
        if (sub == "sidecar") return cmd_sidecar(argc, argv);
        if (sub == "quantize") return cmd_quantize(argc, argv);
        std::cerr << "Unknown subcommand: " << sub << "\n";
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "Fatal: " << e.what() << "\n";
        return 2;
    }
}
