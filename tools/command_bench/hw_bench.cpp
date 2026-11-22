// Cross-platform C++20 hardware benchmark (macOS Apple Silicon + Windows x86_64 CPU)
// measuring real RAM (Private + Working Set/RSS) and wall-clock CPU latency across
// Moonshine's resident baseline + the winning NPC Command Pipeline.

#include "command.h"
#include "embedder.h"
#include "llamacpp.h"
#include "moonshine-c-api.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <numeric>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#elif defined(__APPLE__)
#include <mach/mach.h>
#endif

namespace {

struct MemSnapshot {
    double private_mb = 0.0;
    double working_set_mb = 0.0;
};

MemSnapshot sample_memory() {
    MemSnapshot m;
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
                             sizeof(pmc))) {
        m.private_mb = static_cast<double>(pmc.PrivateUsage) / (1024.0 * 1024.0);
        m.working_set_mb = static_cast<double>(pmc.WorkingSetSize) / (1024.0 * 1024.0);
    }
#elif defined(__APPLE__)
    task_vm_info_data_t vm_info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO,
                  reinterpret_cast<task_info_t>(&vm_info), &count) == KERN_SUCCESS) {
        m.private_mb = static_cast<double>(vm_info.phys_footprint) / (1024.0 * 1024.0);
        m.working_set_mb = static_cast<double>(vm_info.resident_size) / (1024.0 * 1024.0);
    }
#endif
    return m;
}

struct DistStats {
    double mean_ms = 0.0;
    double p50_ms = 0.0;
    double p95_ms = 0.0;
    double min_ms = 0.0;
    double max_ms = 0.0;
};

DistStats summarize(std::vector<double> vals) {
    DistStats d{};
    if (vals.empty()) return d;
    std::sort(vals.begin(), vals.end());
    double sum = std::accumulate(vals.begin(), vals.end(), 0.0);
    d.mean_ms = sum / static_cast<double>(vals.size());
    d.min_ms = vals.front();
    d.max_ms = vals.back();
    auto pct = [&](double q) {
        double idx = q * static_cast<double>(vals.size() - 1);
        size_t lo = static_cast<size_t>(std::floor(idx));
        size_t hi = static_cast<size_t>(std::ceil(idx));
        double frac = idx - static_cast<double>(lo);
        return vals[lo] * (1.0 - frac) + vals[hi] * frac;
    };
    d.p50_ms = pct(0.50);
    d.p95_ms = pct(0.95);
    return d;
}

void print_stats_json(std::ostream& out, const DistStats& s, int indent = 4) {
    std::string pad(static_cast<size_t>(indent), ' ');
    out << "{\n"
        << pad << "  \"mean_ms\": " << std::fixed << std::setprecision(4) << s.mean_ms << ",\n"
        << pad << "  \"p50_ms\": " << s.p50_ms << ",\n"
        << pad << "  \"p95_ms\": " << s.p95_ms << ",\n"
        << pad << "  \"min_ms\": " << s.min_ms << ",\n"
        << pad << "  \"max_ms\": " << s.max_ms << "\n"
        << pad << "}";
}

void print_mem_json(std::ostream& out, const MemSnapshot& m) {
    out << "{ \"private_mb\": " << std::fixed << std::setprecision(1) << m.private_mb
        << ", \"working_set_mb\": " << m.working_set_mb << " }";
}

// Simulates sharing KnowledgeDirector's per-turn query embedding so marginal overhead
// of CommandDirector is measured separately from EmbeddingGemma's already-paid ~30 ms.
class SharedTurnEmbedder : public moonshine::Embedder {
public:
    explicit SharedTurnEmbedder(moonshine::Embedder& inner) : inner_(inner) {}

    void preload(std::string_view text, std::vector<float> vec) {
        cache_[std::string(text)] = std::move(vec);
    }

    std::vector<float> embed(std::string_view text, bool query) override {
        if (query) {
            auto it = cache_.find(std::string(text));
            if (it != cache_.end()) return it->second;
        }
        return inner_.embed(text, query);
    }
    int dim() const override { return inner_.dim(); }
    std::string describe() const override { return inner_.describe(); }

private:
    moonshine::Embedder& inner_;
    std::map<std::string, std::vector<float>> cache_;
};

std::vector<float> load_wav_16k(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.size() < 44) return {};
    size_t pos = 12;
    while (pos + 8 <= bytes.size()) {
        uint32_t sz = 0;
        std::memcpy(&sz, bytes.data() + pos + 4, 4);
        if (std::memcmp(bytes.data() + pos, "data", 4) == 0) {
            size_t data_off = pos + 8;
            size_t avail = std::min<size_t>(sz, bytes.size() - data_off);
            size_t n = avail / 2;
            std::vector<float> pcm(n);
            const auto* s16 = reinterpret_cast<const int16_t*>(bytes.data() + data_off);
            for (size_t i = 0; i < n; ++i) {
                pcm[i] = static_cast<float>(s16[i]) / 32768.0f;
            }
            return pcm;
        }
        pos += 8 + sz + (sz & 1u);
    }
    return {};
}

}  // namespace

int main(int argc, char** argv) {
    std::string db_path = "data/kcd2/npcs.sqlite";
    std::string main_llm_path = "models/llm/Qwen3-4B-Instruct-2507-Q4_K_M.gguf";
    std::string embed_path = "models/embeddinggemma-300m-q8_0.gguf";
    std::string stt_dir = "models/stt/medium-streaming-en";
    std::string gate_path = "tools/command_bench/stage1_gate_weights.json";
    std::string qwen06_path;
    std::string fg270_path;
    std::string wav_path;
    int n_threads = 0;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--db" && i + 1 < argc) db_path = argv[++i];
        else if (a == "--main-llm" && i + 1 < argc) main_llm_path = argv[++i];
        else if (a == "--embed" && i + 1 < argc) embed_path = argv[++i];
        else if (a == "--stt" && i + 1 < argc) stt_dir = argv[++i];
        else if (a == "--gate" && i + 1 < argc) gate_path = argv[++i];
        else if (a == "--qwen06" && i + 1 < argc) qwen06_path = argv[++i];
        else if (a == "--fg270" && i + 1 < argc) fg270_path = argv[++i];
        else if (a == "--wav" && i + 1 < argc) wav_path = argv[++i];
        else if (a == "--threads" && i + 1 < argc) n_threads = std::stoi(argv[++i]);
    }

    const int hw_threads = static_cast<int>(std::thread::hardware_concurrency());
    if (n_threads <= 0) {
        n_threads = std::max(4, std::min(8, hw_threads / 2));
    }

    MemSnapshot mem_start = sample_memory();

    // 1. Load Moonshine baseline resident stack on CPU (repack_weights = false, cpu_only = true).
    moonshine::Options main_opts;
    main_opts.context_tokens = 4096;
    main_opts.cpu_only = true;
    main_opts.repack_weights = false;
    moonshine::LlamaLlm main_llm(main_llm_path, main_opts);
    MemSnapshot mem_after_main = sample_memory();

    moonshine::LlamaEmbedder embedder(embed_path);
    MemSnapshot mem_after_embed = sample_memory();

    moonshine_option_t stt_opts[1] = {{"identify_speakers", "false"}};
    int32_t transcriber = moonshine_load_transcriber_from_files(
        stt_dir.c_str(), MOONSHINE_MODEL_ARCH_MEDIUM_STREAMING, stt_opts, 1, MOONSHINE_HEADER_VERSION);
    MemSnapshot mem_baseline = sample_memory();

    // 2. Load CommandGraph + Stage1Gate (without sidecar LLM yet).
    moonshine::CommandGraph graph(db_path);
    auto gate_opt = moonshine::Stage1Gate::load_json(gate_path);
    moonshine::Stage1Gate gate = gate_opt.value_or(moonshine::Stage1Gate{});
    // Warm up graph candidate cache for one circle.
    std::string speaker = "kkut_additive_man_47";
    (void)graph.candidates_for(speaker);
    MemSnapshot mem_after_graph_gate = sample_memory();

    moonshine::SoulRecord rec;
    std::vector<moonshine::Message> hist = {
        {"user", "Good day to you."},
        {"assistant", "God give you good health, traveller."},
    };

    const std::vector<std::string> chat_queries = {
        "How have things been in town lately?",
        "What do you make of Sigismund's soldiers?",
        "Where were you born before coming to Kuttenberg?",
        "Tell me about your trade and how you earn your bread.",
        "Have you heard any rumours from the silver mines?",
        "What do you think of the bailiff?",
        "Where did your family live during the war?",
        "It looks like rain before nightfall.",
        "Do you know anything about the guild dispute?",
        "I don't follow your reasoning at all.",
        "Where does a man get a decent mug of ale in peace?",
        "God be with you, I should be moving on.",
    };

    const std::vector<std::string> cmd_queries = {
        "Where can I find your wife right now?",
        "Do you know where the blacksmith is working?",
        "Point me to where Margaret usually is.",
        "Where might I find Father Ernest?",
        "Could you show me the way to her?",
        "Follow behind me for a moment.",
        "Come along with me to the gate.",
        "Lead the way to Kuttenberg.",
        "Take me toward Trosky Castle.",
        "Do you know where I can find the bailiff?",
        "Where is your brother keeping himself today?",
    };

    // Warm up embedder once.
    (void)embedder.embed("Good morrow.", true);

    SharedTurnEmbedder shared_embed(embedder);
    std::vector<double> embed_ms;
    std::vector<std::vector<float>> chat_vecs;

    for (const auto& q : chat_queries) {
        auto t0 = std::chrono::steady_clock::now();
        auto v = embedder.embed(q, true);
        auto t1 = std::chrono::steady_clock::now();
        embed_ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
        shared_embed.preload(q, v);
        chat_vecs.push_back(std::move(v));
    }
    for (const auto& q : cmd_queries) {
        auto t0 = std::chrono::steady_clock::now();
        auto v = embedder.embed(q, true);
        auto t1 = std::chrono::steady_clock::now();
        embed_ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
        shared_embed.preload(q, std::move(v));
    }

    // Sub-microsecond Stage-1 linear gate timing across 1,000 evaluations.
    std::vector<double> gate_only_ms;
    for (int iter = 0; iter < 1000; ++iter) {
        const auto& v = chat_vecs[static_cast<size_t>(iter) % chat_vecs.size()];
        auto t0 = std::chrono::steady_clock::now();
        volatile bool run = gate.should_run_extractor(v);
        (void)run;
        auto t1 = std::chrono::steady_clock::now();
        gate_only_ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
    }

    // 4. Standalone C++ CommandGraph + Salience/Phonetic Entity Linker latency.
    moonshine::CommandDirector dir_graph_only(&shared_embed, nullptr, graph, gate);
    std::vector<double> linker_ms;
    std::vector<double> keyterm_ms;
    for (int iter = 0; iter < 50; ++iter) {
        auto tk0 = std::chrono::steady_clock::now();
        volatile auto kt = dir_graph_only.pass1_keyterms(speaker, hist, rec);
        (void)kt;
        auto tk1 = std::chrono::steady_clock::now();
        keyterm_ms.push_back(std::chrono::duration<double, std::milli>(tk1 - tk0).count());

        moonshine::ExtractedCommand ext;
        ext.action = moonshine::CommandAction::LocateNpc;
        ext.target_mention = (iter % 2 == 0) ? "your wife" : "Margaret";
        auto tl0 = std::chrono::steady_clock::now();
        auto res = dir_graph_only.link(speaker, "Heinz", ext, hist, rec);
        (void)res;
        auto tl1 = std::chrono::steady_clock::now();
        linker_ms.push_back(std::chrono::duration<double, std::milli>(tl1 - tl0).count());
    }

    struct SidecarBench {
        bool loaded = false;
        MemSnapshot mem_loaded{};
        MemSnapshot mem_after_infer{};
        double delta_private_mb = 0.0;
        double delta_working_set_mb = 0.0;
        DistStats chat_turn_overhead{};
        DistStats cmd_turn_marginal_cpu{};
        DistStats cmd_turn_including_embed{};
    };

    auto bench_sidecar = [&](const std::string& gguf_path, bool use_grammar) -> SidecarBench {
        SidecarBench sb;
        if (gguf_path.empty()) return sb;
        std::ifstream chk(gguf_path, std::ios::binary);
        if (!chk) return sb;
        chk.close();

        MemSnapshot before = sample_memory();
        moonshine::Options sc_opts;
        sc_opts.context_tokens = 512;
        sc_opts.reply_tokens = 24;
        sc_opts.temperature = 0.0;
        sc_opts.cpu_only = true;
        sc_opts.repack_weights = false;
        sc_opts.disable_thinking = true;
        if (use_grammar) {
            sc_opts.grammar = moonshine::kCompactCommandGrammar;
        }
        auto sidecar = std::make_unique<moonshine::LlamaLlm>(gguf_path, sc_opts);
        sb.mem_loaded = sample_memory();

        // Director 1: shares KnowledgeDirector's query embedding (true marginal gameplay cost).
        moonshine::CommandDirector director_marginal(&shared_embed, sidecar.get(), graph, gate, sc_opts);
        // Director 2: runs its own EmbeddingGemma query from scratch.
        moonshine::CommandDirector director_full(&embedder, sidecar.get(), graph, gate, sc_opts);

        // Warmup 2 turns so threadpools and KV scratch settle.
        (void)director_marginal.resolve(speaker, "Heinz", "Kuttenberg", cmd_queries[0], hist, rec);
        (void)director_marginal.resolve(speaker, "Heinz", "Kuttenberg", cmd_queries[1], hist, rec);
        sb.mem_after_infer = sample_memory();
        sb.delta_private_mb = std::max(0.0, sb.mem_after_infer.private_mb - before.private_mb);
        sb.delta_working_set_mb = std::max(0.0, sb.mem_after_infer.working_set_mb - before.working_set_mb);

        std::vector<double> chat_lat;
        for (const auto& q : chat_queries) {
            auto t0 = std::chrono::steady_clock::now();
            auto r = director_marginal.resolve(speaker, "Heinz", "Kuttenberg", q, hist, rec);
            (void)r;
            auto t1 = std::chrono::steady_clock::now();
            chat_lat.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
        }

        std::vector<double> cmd_marg;
        std::vector<double> cmd_full;
        for (const auto& q : cmd_queries) {
            auto t0 = std::chrono::steady_clock::now();
            auto r1 = director_marginal.resolve(speaker, "Heinz", "Kuttenberg", q, hist, rec);
            (void)r1;
            auto t1 = std::chrono::steady_clock::now();
            cmd_marg.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());

            auto t2 = std::chrono::steady_clock::now();
            auto r2 = director_full.resolve(speaker, "Heinz", "Kuttenberg", q, hist, rec);
            (void)r2;
            auto t3 = std::chrono::steady_clock::now();
            cmd_full.push_back(std::chrono::duration<double, std::milli>(t3 - t2).count());
        }

        sb.loaded = true;
        sb.chat_turn_overhead = summarize(chat_lat);
        sb.cmd_turn_marginal_cpu = summarize(cmd_marg);
        sb.cmd_turn_including_embed = summarize(cmd_full);
        return sb;
    };

    SidecarBench bench_qwen06 = bench_sidecar(qwen06_path, /*use_grammar=*/true);
    SidecarBench bench_fg270 = bench_sidecar(fg270_path, /*use_grammar=*/false);

    // 5. Moonshine medium-streaming-en Pass-2 buffered PCM replay (4.5s @ 16 kHz + 95 keyterms).
    std::vector<float> pcm = wav_path.empty() ? std::vector<float>{} : load_wav_16k(wav_path);
    if (pcm.empty()) {
        pcm.resize(16000 * 9 / 2);
        for (size_t i = 0; i < pcm.size(); ++i) {
            double t = static_cast<double>(i) / 16000.0;
            pcm[i] = static_cast<float>(0.15 * std::sin(2.0 * 3.14159265 * 145.0 * t) *
                                        (0.5 + 0.5 * std::sin(2.0 * 3.14159265 * 4.0 * t)));
        }
    }
    double pcm_sec = static_cast<double>(pcm.size()) / 16000.0;

    std::vector<double> pass2_ms;
    if (transcriber >= 0) {
        std::string p2_terms = dir_graph_only.pass2_keyterms(speaker, hist, rec);
        auto run_replay = [&]() {
            moonshine_transcriber_set_keyterms(transcriber, p2_terms.c_str());
            int32_t s = moonshine_create_stream(transcriber, 0);
            moonshine_start_stream(transcriber, s);
            constexpr size_t kChunk = 8000;
            struct transcript_t* out = nullptr;
            for (size_t off = 0; off < pcm.size(); off += kChunk) {
                size_t n = std::min(kChunk, pcm.size() - off);
                moonshine_transcribe_add_audio_to_stream(
                    transcriber, s, pcm.data() + off, static_cast<uint64_t>(n), 16000, 0);
                moonshine_transcribe_stream(transcriber, s, 0, &out);
            }
            std::vector<float> pad(4800, 0.0f);
            moonshine_transcribe_add_audio_to_stream(
                transcriber, s, pad.data(), static_cast<uint64_t>(pad.size()), 16000, 0);
            moonshine_stop_stream(transcriber, s);
            moonshine_transcribe_stream(transcriber, s, MOONSHINE_FLAG_FORCE_UPDATE, &out);
            moonshine_free_stream(transcriber, s);
        };
        run_replay();
        for (int iter = 0; iter < 6; ++iter) {
            auto t0 = std::chrono::steady_clock::now();
            run_replay();
            auto t1 = std::chrono::steady_clock::now();
            pass2_ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
        }
        moonshine_free_transcriber(transcriber);
    }

    auto print_sidecar = [&](const SidecarBench& sb) {
        if (!sb.loaded) {
            std::cout << "null";
            return;
        }
        std::cout << "{\n"
                  << "      \"mem_after_load\": ";
        print_mem_json(std::cout, sb.mem_loaded);
        std::cout << ",\n      \"mem_after_infer\": ";
        print_mem_json(std::cout, sb.mem_after_infer);
        std::cout << ",\n      \"delta_private_mb\": " << std::fixed << std::setprecision(1) << sb.delta_private_mb
                  << ",\n      \"delta_working_set_mb\": " << sb.delta_working_set_mb
                  << ",\n      \"chat_turn_gated_overhead\": ";
        print_stats_json(std::cout, sb.chat_turn_overhead, 6);
        std::cout << ",\n      \"command_turn_marginal_cpu\": ";
        print_stats_json(std::cout, sb.cmd_turn_marginal_cpu, 6);
        std::cout << ",\n      \"command_turn_including_embed\": ";
        print_stats_json(std::cout, sb.cmd_turn_including_embed, 6);
        std::cout << "\n    }";
    };

    std::cout << "{\n"
              << "  \"hw_threads\": " << hw_threads << ",\n"
              << "  \"bench_threads\": " << n_threads << ",\n"
              << "  \"pcm_replay_seconds\": " << std::fixed << std::setprecision(2) << pcm_sec << ",\n"
              << "  \"memory\": {\n"
              << "    \"process_start\": ";
    print_mem_json(std::cout, mem_start);
    std::cout << ",\n    \"after_qwen3_4b\": ";
    print_mem_json(std::cout, mem_after_main);
    std::cout << ",\n    \"after_embeddinggemma_300m\": ";
    print_mem_json(std::cout, mem_after_embed);
    std::cout << ",\n    \"baseline_resident_stack\": ";
    print_mem_json(std::cout, mem_baseline);
    std::cout << ",\n    \"after_command_graph_and_gate\": ";
    print_mem_json(std::cout, mem_after_graph_gate);
    std::cout << "\n  },\n"
              << "  \"components\": {\n"
              << "    \"embeddinggemma_query\": ";
    print_stats_json(std::cout, summarize(embed_ms), 4);
    std::cout << ",\n    \"stage1_linear_gate_only\": ";
    print_stats_json(std::cout, summarize(gate_only_ms), 4);
    std::cout << ",\n    \"pass1_keyterm_gen\": ";
    print_stats_json(std::cout, summarize(keyterm_ms), 4);
    std::cout << ",\n    \"cpp_graph_entity_linker\": ";
    print_stats_json(std::cout, summarize(linker_ms), 4);
    std::cout << ",\n    \"pass2_moonshine_stt_replay\": ";
    print_stats_json(std::cout, summarize(pass2_ms), 4);
    std::cout << "\n  },\n"
              << "  \"sidecars\": {\n"
              << "    \"qwen3_0_6b_lora_q4_k_m\": ";
    print_sidecar(bench_qwen06);
    std::cout << ",\n    \"functiongemma_270m_lora_q8_0\": ";
    print_sidecar(bench_fg270);
    std::cout << "\n  }\n}\n";
    return 0;
}
