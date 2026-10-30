#include "llamacpp.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <thread>

#include "llama.h"

namespace moonshine {

namespace {

using Clock = std::chrono::steady_clock;

double seconds_between(Clock::time_point from, Clock::time_point to) {
    return std::chrono::duration<double>(to - from).count();
}

// ggml's backends register themselves once per process, and llama.cpp logs a page of device
// information while doing it. Called from the constructor rather than from main so that
// nothing but this file has to know the backend exists.
void start_backend_once(bool quiet) {
    static std::once_flag once;
    std::call_once(once, [quiet] {
        if (quiet) {
            // Everything llama.cpp has to say goes to the log at load time: tensor counts,
            // metadata keys, the whole GGUF header. Useful when a model will not load and
            // noise on top of the app's own status lines every other time, so it is off
            // unless asked for. The load error itself comes back as a null pointer, which is
            // reported by this file in its own words. Errors stay: the logger is one for the
            // whole process (ggml's), so whichever of this, the embedder and the voice set it
            // last decides what a CUDA failure in any of them leaves in the log -- a crash on
            // 13 Oct 2026 said only "CUDA error" because the embedder's callback dropped the
            // lines naming it.
            llama_log_set([](ggml_log_level level, const char* text, void*) {
                if (level == GGML_LOG_LEVEL_ERROR) std::cerr << "llm: " << text << std::flush;
            }, nullptr);
        }
        llama_backend_init();
    });
}

// How many threads to prefill and decode on.
//
// Half of what the machine reports, rounded up, which on both machines here is the physical
// core count rather than the hyperthreaded one: prefill is memory-bound, and two threads
// sharing one core's load-store path contend rather than add. Measured at 22-23 tok/s decode
// against Ollama's 20-22 on the Windows laptop, so it is at least not worse than what Ollama chose.
//
// Not the last word. The game is on the same CPU, the Windows laptop is Raptor Lake-HX with P-cores and
// E-cores, and hardware_concurrency does not distinguish them -- half of 24 could be 12
// threads spread across both kinds. Pinning to P-cores is a separate open question with a
// claimed 20-30% on this architecture.
int threads_for(const Options&) {
    const unsigned all = std::thread::hardware_concurrency();
    if (all == 0) return 4;
    return static_cast<int>((all + 1) / 2);
}

// A token turned into text can stop in the middle of a UTF-8 sequence, and a Czech name is
// where that shows: "Zikmund" is safe, "Jan Žižka" is two tokens where the second begins
// mid-character. Handing a half sequence to the caller puts a replacement character into the
// subtitle, or into the middle of a word that the sentence gate then splits on.
class Utf8Gate {
public:
    // Adds bytes and returns however much of the buffer is now whole. What is returned is
    // gone from the buffer; what is not stays until the rest of it arrives.
    std::string_view take(std::string_view bytes) {
        pending_ += bytes;
        std::size_t whole = 0;
        while (whole < pending_.size()) {
            const unsigned char lead = static_cast<unsigned char>(pending_[whole]);
            std::size_t width = 1;
            if ((lead & 0x80u) == 0x00u) width = 1;
            else if ((lead & 0xE0u) == 0xC0u) width = 2;
            else if ((lead & 0xF0u) == 0xE0u) width = 3;
            else if ((lead & 0xF8u) == 0xF0u) width = 4;
            // A continuation byte with no lead before it is not the start of anything. Pass
            // it through rather than waiting for a lead that is never coming.
            if (whole + width > pending_.size()) break;
            whole += width;
        }
        ready_.assign(pending_, 0, whole);
        pending_.erase(0, whole);
        return ready_;
    }

private:
    std::string pending_;
    std::string ready_;
};

}  // namespace

struct LlamaLlm::Impl {
    std::filesystem::path path;
    llama_model* model = nullptr;
    llama_context* ctx = nullptr;
    const llama_vocab* vocab = nullptr;
    std::string chat_template;
    int context_tokens = 0;
    bool cpu_only = true;

    // What the KV cache holds, in order. The whole reason for being in this process: with
    // Ollama this was a guess made from a stopwatch.
    std::vector<llama_token> held;

    // A copy of the state after `branch_tokens`, taken by hold_branch(). The way back to the
    // world prefix on a model whose cache cannot be cut: a recurrent or hybrid state is a fold
    // over the sequence, so llama_memory_seq_rm refuses a partial range, but a whole state can
    // be copied out and put back. Empty until the app asks for it.
    std::vector<llama_token> branch_tokens;
    std::vector<std::uint8_t> branch_state;

    // What the model stands aside for (Llm::stand_aside_while); nothing until the app says.
    std::function<bool()> aside_for;

    // Waits while `aside_for` says so, and says how long that was. Polled rather than
    // signalled, because what is waited on is a line of speech a second or more long and the
    // polling thread is the one that would otherwise be decoding -- a hundredth of a second
    // of latency on a wait that long is nothing, and no thread has to know about this one.
    double stand_aside() const {
        if (!aside_for || !aside_for()) return 0.0;
        const auto began = std::chrono::steady_clock::now();
        while (aside_for()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
    }

    ~Impl() {
        if (ctx) llama_free(ctx);
        if (model) llama_model_free(model);
    }

    std::vector<llama_token> tokenize(const std::string& text) const {
        // Negative means "this many would be needed". Asked first rather than guessing a
        // bound from the character count, which is wrong in both directions.
        const int needed = -llama_tokenize(vocab, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0, /*add_special=*/true,
                                           /*parse_special=*/true);
        std::vector<llama_token> tokens(static_cast<std::size_t>(needed));
        const int got = llama_tokenize(vocab, text.data(), static_cast<int>(text.size()),
                                       tokens.data(), needed, true, true);
        if (got < 0) throw std::runtime_error("llama.cpp could not tokenise the prompt");
        tokens.resize(static_cast<std::size_t>(got));
        return tokens;
    }

    std::string piece(llama_token token) const {
        char buf[256];
        const int len = llama_token_to_piece(vocab, token, buf, sizeof(buf), 0,
                                             /*special=*/false);
        if (len < 0) return {};
        return std::string(buf, static_cast<std::size_t>(len));
    }

    // The prompt as the model's own template says it should look. Taken from the GGUF rather
    // than written out here: the template is a property of the weights, and a second copy in
    // this file would be a second thing to get wrong when the model changes. Ollama was
    // applying the same template on the other side of the socket.
    std::string apply_template(const std::vector<Message>& messages) const {
        std::vector<llama_chat_message> chat;
        chat.reserve(messages.size());
        for (const Message& message : messages) {
            chat.push_back({message.role.c_str(), message.content.c_str()});
        }
        // Options::disable_thinking has no equivalent here and is not honoured. Ollama takes
        // `think: false` over the wire; llama_chat_apply_template renders the template with
        // no way to pass it a variable, so a template branching on enable_thinking would take
        // its own default. This is safe for the model that ships -- qwen3:4b-instruct's
        // template has no such branch, checked -- and would put reasoning into the subtitles
        // of a thinking model pointed at by --gguf. That is the same disqualification
        // benchmarks/README.md already applies to thinking models for latency.
        const char* tmpl = chat_template.empty() ? nullptr : chat_template.c_str();
        // Sized by asking, for the same reason as tokenising: the answer depends on the
        // template, which came out of a file.
        int needed = llama_chat_apply_template(tmpl, chat.data(), chat.size(),
                                               /*add_ass=*/true, nullptr, 0);
        if (needed < 0) {
            // llama.cpp's fixed list has not caught up with this model. If the format is one
            // we know by hand, write it; otherwise refuse, because sending a prompt in a shape
            // the model was never trained on produces bad dialogue rather than an error, and
            // that is the harder failure to notice.
            if (uses_gemma_turns(chat_template)) return gemma_turns(messages);
            throw std::runtime_error(
                "llama.cpp does not recognise this model's chat template; it matches a fixed "
                "list rather than running Jinja, so a newly-formatted model needs adding");
        }
        std::string out(static_cast<std::size_t>(needed), '\0');
        needed = llama_chat_apply_template(tmpl, chat.data(), chat.size(), true, out.data(),
                                           static_cast<int>(out.size()));
        if (needed < 0) {
            throw std::runtime_error("llama.cpp could not apply the model's chat template");
        }
        out.resize(static_cast<std::size_t>(needed));
        return out;
    }
};

LlamaLlm::LlamaLlm(std::filesystem::path gguf, const Options& options)
    : impl_(std::make_unique<Impl>()) {
    start_backend_once(/*quiet=*/true);
    impl_->path = std::move(gguf);

    if (!std::filesystem::exists(impl_->path)) {
        throw std::runtime_error("no model file at " + impl_->path.string());
    }

    llama_model_params weights = llama_model_default_params();
    // None of it on the GPU when the game owns the GPU, which is the measured default for
    // this project and the reason a 4B was chosen in the first place.
    weights.n_gpu_layers = options.cpu_only ? 0 : 999;
    // And no GPU device at all, not merely no layers on one. Left to its default list,
    // llama.cpp puts every GPU it can see into the model's devices whatever n_gpu_layers
    // says, and the context then opens a backend on each: on the Windows laptop a CUDA context, a cuBLAS
    // workspace and a VRAM pool, taken while the game was short of both. That is how an app
    // that called itself CPU-only sat in nvidia-smi's process list beside the game from the
    // start, and what the display driver's resets tore down into the processes that would
    // not die (22-23 Oct 2026). An empty list leaves the CPU as the only device.
    static ggml_backend_dev_t no_devices[] = {nullptr};
    if (options.cpu_only) weights.devices = no_devices;
    impl_->cpu_only = options.cpu_only;
    // The "extra" buffer types are the CPU backend's repacking ones; the model is then held
    // once, in the mapping of its file. Why, and what it costs, is at Options::repack_weights.
    weights.use_extra_bufts = options.repack_weights;
    impl_->model = llama_model_load_from_file(impl_->path.string().c_str(), weights);
    if (!impl_->model) {
        throw std::runtime_error("llama.cpp could not load " + impl_->path.string());
    }
    impl_->vocab = llama_model_get_vocab(impl_->model);

    if (const char* tmpl = llama_model_chat_template(impl_->model, nullptr)) {
        impl_->chat_template = tmpl;
    }

    llama_context_params ctx_params = llama_context_default_params();
    ctx_params.n_ctx = static_cast<std::uint32_t>(options.context_tokens);
    // The whole prompt in one batch. It is about a thousand tokens and splitting it would
    // only add round trips through the scheduler.
    ctx_params.n_batch = static_cast<std::uint32_t>(options.context_tokens);
    ctx_params.n_threads = threads_for(options);
    ctx_params.n_threads_batch = ctx_params.n_threads;
    // With no layers on the GPU, llama.cpp would still ship the prompt's large matrix
    // multiplies there and copy the weights over for each (op_offload). A build with a GPU
    // backend in it -- CUDA on the Windows laptop for the voices -- must not do that behind this setting:
    // cpu_only means the game has the GPU to itself.
    ctx_params.op_offload = !options.cpu_only;
    impl_->ctx = llama_init_from_model(impl_->model, ctx_params);
    if (!impl_->ctx) {
        throw std::runtime_error("llama.cpp could not make a context for " +
                                 impl_->path.string());
    }
    impl_->context_tokens = static_cast<int>(llama_n_ctx(impl_->ctx));
}

LlamaLlm::~LlamaLlm() = default;

std::string LlamaLlm::describe() const {
    // The model's own name out of the GGUF header, not the filename. When the weights came
    // from Ollama's store the filename is a 64-character digest, which says nothing to
    // anybody reading a status line.
    auto header = [this](const char* key) {
        char buf[256];
        const int len = llama_model_meta_val_str(impl_->model, key, buf, sizeof(buf));
        return len > 0 ? std::string(buf, static_cast<std::size_t>(len)) : std::string{};
    };

    // general.name is not always set -- gemma4:12b leaves it out, and went straight to the
    // digest this function exists to avoid. Architecture and size say the same thing in the
    // cases that matter ("gemma4 12B"), and are what the header does carry when the name is
    // missing. The filename is the last resort rather than the second.
    std::string called = header("general.name");
    if (called.empty()) {
        const std::string arch = header("general.architecture");
        const std::string size = header("general.size_label");
        if (!arch.empty()) called = size.empty() ? arch : arch + " " + size;
    }
    if (called.empty()) called = impl_->path.filename().string();
    // Where it runs is said every time, so a log from a machine with a GPU shows which side
    // the model was on without anybody having to remember what the default was that week.
    return "llama.cpp in this process, " + called + (impl_->cpu_only ? " on the CPU" : " on the GPU");
}

std::size_t LlamaLlm::held_tokens() const { return impl_->held.size(); }

std::size_t LlamaLlm::branch_tokens() const { return impl_->branch_tokens.size(); }

Timing LlamaLlm::hold_branch(std::string_view system) {
    // Where the shared text ends, in tokens. The template wraps the system message and puts
    // the turns after it, so what every prompt shares is the rendering up to the end of the
    // system text -- found as the front two renderings agree on, one with the text as given
    // and one with more of it, since the tokeniser may merge the text's last characters with
    // whatever follows and the agreement stops safely short of that.
    const std::vector<Message> as_given = {{"system", std::string(system)}, {"user", "x"}};
    const std::vector<Message> longer = {{"system", std::string(system) + " and more"},
                                         {"user", "x"}};
    const std::vector<llama_token> a = impl_->tokenize(impl_->apply_template(as_given));
    const std::vector<llama_token> b = impl_->tokenize(impl_->apply_template(longer));
    std::size_t same = 0;
    while (same < a.size() && same < b.size() && a[same] == b[same]) ++same;
    if (same == 0) return {};
    std::vector<llama_token> branch(a.begin(), a.begin() + static_cast<std::ptrdiff_t>(same));

    // Decode it, reusing whatever of it the cache already holds.
    llama_memory_t memory = llama_get_memory(impl_->ctx);
    std::size_t keep = reusable_prefix(impl_->held, branch);
    if (keep > 0 && !llama_memory_seq_rm(memory, 0, static_cast<llama_pos>(keep), -1)) keep = 0;
    if (keep == 0) llama_memory_seq_rm(memory, 0, -1, -1);
    impl_->held.resize(keep);

    Timing timing;
    timing.prompt_tokens = static_cast<int>(branch.size());
    timing.cached_tokens = static_cast<int>(keep);
    const Clock::time_point began = Clock::now();
    std::vector<llama_token> fresh(branch.begin() + static_cast<std::ptrdiff_t>(keep), branch.end());
    llama_batch batch = llama_batch_get_one(fresh.data(), static_cast<int>(fresh.size()));
    if (llama_decode(impl_->ctx, batch) != 0) {
        throw std::runtime_error("llama.cpp could not prefill the shared prefix");
    }
    impl_->held = branch;
    timing.prefill_seconds = seconds_between(began, Clock::now());

    const std::size_t size = llama_state_seq_get_size(impl_->ctx, 0);
    std::vector<std::uint8_t> state(size);
    if (llama_state_seq_get_data(impl_->ctx, state.data(), state.size(), 0) == size) {
        impl_->branch_state = std::move(state);
        impl_->branch_tokens = branch;
    }
    return timing;
}

Timing LlamaLlm::chat(const std::vector<Message>& messages,
                      const Options& options,
                      const OnDelta& on_delta) {
    const std::vector<llama_token> prompt = impl_->tokenize(impl_->apply_template(messages));
    if (prompt.empty()) throw std::runtime_error("the prompt came to no tokens at all");

    // Room for the reply as well as the prompt. Said plainly rather than left to fail
    // somewhere inside a decode, because the arithmetic is knowable here and the message
    // "the conversation no longer fits" is one a caller can do something about.
    if (static_cast<int>(prompt.size()) + options.reply_tokens > impl_->context_tokens) {
        throw std::runtime_error("the prompt is " + std::to_string(prompt.size()) +
                                 " tokens and the reply wants " +
                                 std::to_string(options.reply_tokens) + ", which is more than "
                                 "the " + std::to_string(impl_->context_tokens) +
                                 " this context holds");
    }

    // The cache decision, and the only part of this file that needed a test.
    std::size_t keep = reusable_prefix(impl_->held, prompt);

    llama_memory_t memory = llama_get_memory(impl_->ctx);
    // Everything from the divergence on, and nothing before it. The world prefix survives
    // every conversation of the game this way without anything having to re-send it: it is
    // the front of every prompt, so it is never what diverges.
    //
    // This REFUSES on a recurrent or hybrid model, because a linear-attention state is a fold
    // over the sequence rather than a list indexed by position, so there is nothing to cut at
    // `keep`. llama.cpp says so with a false and leaves the cache whole rather than half-done.
    // Taking it for success is worse than being slow: the next prompt would be decoded on top
    // of the last conversation's state, so the model answers as the previous NPC -- measured,
    // with `scripts/probe_branch_cache.py`, and it reported a 94% cache hit while doing it.
    // Dropping the lot is always allowed, and costs a full prefill instead of a wrong answer.
    if (keep > 0 && !llama_memory_seq_rm(memory, 0, static_cast<llama_pos>(keep), -1)) {
        llama_memory_seq_rm(memory, 0, -1, -1);
        keep = 0;
        // The way back for such a model: the state copied after the world prefix, put back
        // whole, so only the soul's sheet is prefilled rather than the world before it too.
        // Measured on Qwen3.5-4B, that is 4.4 s against 8.1 s. Only when the prompt still
        // begins with the branch -- a changed world text is a miss, and a miss is a cold
        // prefill, which is always correct.
        if (branch_covers(impl_->branch_tokens, prompt) &&
            llama_state_seq_set_data(impl_->ctx, impl_->branch_state.data(),
                                     impl_->branch_state.size(), 0) > 0) {
            keep = impl_->branch_tokens.size();
            impl_->held = impl_->branch_tokens;
        } else {
            impl_->held.clear();
        }
    }
    impl_->held.resize(keep);

    Timing timing;
    timing.prompt_tokens = static_cast<int>(prompt.size());
    timing.cached_tokens = static_cast<int>(keep);

    const Clock::time_point began = Clock::now();
    {
        // In pieces rather than one batch, with the caller told between them, so that a
        // prompt read in ahead of need (a soul's sheet, while the player only looks at them)
        // can be given up part way when something else wants the model: a sheet is a few
        // hundred tokens and this machine reads about 126 a second, so one batch would be
        // two or three seconds nothing could interrupt. Each piece is recorded as held the
        // moment it is in, so a prefill abandoned after three pieces has left the cache three
        // pieces further on and the next prompt starting the same way takes it from there.
        // Pieces this size cost nothing measurable in throughput on the CPU.
        constexpr std::size_t kPiece = 64;
        for (std::size_t at = keep; at < prompt.size(); at += kPiece) {
            if (at > keep) {
                on_delta({});
                // After on_delta, so a caller who would rather stop than wait has thrown
                // first and is not held here instead.
                timing.held_seconds += impl_->stand_aside();
            }
            const std::size_t count = std::min(kPiece, prompt.size() - at);
            std::vector<llama_token> fresh(prompt.begin() + static_cast<std::ptrdiff_t>(at),
                                           prompt.begin() + static_cast<std::ptrdiff_t>(at + count));
            llama_batch batch = llama_batch_get_one(fresh.data(), static_cast<int>(fresh.size()));
            if (llama_decode(impl_->ctx, batch) != 0) {
                throw std::runtime_error("llama.cpp could not prefill the prompt");
            }
            impl_->held.insert(impl_->held.end(), fresh.begin(), fresh.end());
        }
    }
    const Clock::time_point prefilled = Clock::now();
    timing.prefill_seconds = seconds_between(began, prefilled) - timing.held_seconds;
    const double held_in_prefill = timing.held_seconds;

    // Sampling in the app's own terms. The order is the one llama.cpp's own tools use: cut
    // the tail by count, then by mass, then flatten what is left by temperature, then draw.
    llama_sampler* chain = llama_sampler_chain_init(llama_sampler_chain_default_params());
    if (options.top_k > 0) {
        llama_sampler_chain_add(chain, llama_sampler_init_top_k(options.top_k));
    }
    llama_sampler_chain_add(chain,
                            llama_sampler_init_top_p(static_cast<float>(options.top_p), 1));
    llama_sampler_chain_add(chain,
                            llama_sampler_init_temp(static_cast<float>(options.temperature)));
    llama_sampler_chain_add(chain, llama_sampler_init_dist(LLAMA_DEFAULT_SEED));
    struct Free {
        llama_sampler* chain;
        ~Free() { llama_sampler_free(chain); }
    } free_chain{chain};

    Utf8Gate gate;
    for (int spoken = 0; spoken < options.reply_tokens; ++spoken) {
        const llama_token token = llama_sampler_sample(chain, impl_->ctx, -1);
        if (llama_vocab_is_eog(impl_->vocab, token)) break;

        ++timing.reply_tokens;
        if (const std::string_view ready = gate.take(impl_->piece(token)); !ready.empty()) {
            on_delta(ready);
        }
        timing.held_seconds += impl_->stand_aside();

        llama_sampler_accept(chain, token);
        impl_->held.push_back(token);
        llama_batch one = llama_batch_get_one(&impl_->held.back(), 1);
        if (llama_decode(impl_->ctx, one) != 0) {
            throw std::runtime_error("llama.cpp could not decode the reply");
        }
    }
    timing.decode_seconds =
        seconds_between(prefilled, Clock::now()) - (timing.held_seconds - held_in_prefill);
    return timing;
}

void LlamaLlm::stand_aside_while(std::function<bool()> held) {
    impl_->aside_for = std::move(held);
}

std::filesystem::path LlamaLlm::find_ollama_blob(const std::string& model) {
    // A model name as Ollama spells it, which is where the manifest for it lives: a tag under
    // library/<name>/<tag>, defaulting to latest.
    std::string name = model;
    std::string tag = "latest";
    if (const std::size_t colon = name.find(':'); colon != std::string::npos) {
        tag = name.substr(colon + 1);
        name = name.substr(0, colon);
    }

    std::filesystem::path home;
#ifdef _WIN32
    if (const char* profile = std::getenv("USERPROFILE")) home = profile;
#else
    if (const char* h = std::getenv("HOME")) home = h;
#endif
    if (home.empty()) return {};

    const std::filesystem::path manifest = home / ".ollama" / "models" / "manifests" /
                                           "registry.ollama.ai" / "library" / name / tag;
    std::error_code bad;
    if (!std::filesystem::exists(manifest, bad)) return {};

    // The manifest is JSON, and the model is the layer whose mediaType ends in `.model`.
    // Read by hand rather than by pulling nlohmann/json into this file, because the shape
    // wanted is one digest out of a list and the parse is three lines of find().
    std::string text;
    {
        FILE* file = std::fopen(manifest.string().c_str(), "rb");
        if (!file) return {};
        char buf[4096];
        while (const std::size_t got = std::fread(buf, 1, sizeof(buf), file)) {
            text.append(buf, got);
        }
        std::fclose(file);
    }

    // The whole mediaType tail, not just ".model": these manifests carry four layers and the
    // others are the template, the licence and the sampling params, any of which a looser
    // needle could grow into matching.
    const std::size_t model_layer = text.find("image.model\"");
    if (model_layer == std::string::npos) return {};
    const std::size_t digest = text.find("sha256:", model_layer);
    if (digest == std::string::npos) return {};
    const std::size_t end = text.find('"', digest);
    if (end == std::string::npos) return {};

    std::string blob = text.substr(digest, end - digest);
    // Blobs are named with a dash where the digest has a colon.
    if (const std::size_t colon = blob.find(':'); colon != std::string::npos) {
        blob[colon] = '-';
    }
    std::filesystem::path path = home / ".ollama" / "models" / "blobs" / blob;
    return std::filesystem::exists(path, bad) ? path : std::filesystem::path{};
}

}  // namespace moonshine
