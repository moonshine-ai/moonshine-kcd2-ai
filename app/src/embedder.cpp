#include "embedder.h"

#include <cmath>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

#include "llama.h"

namespace moonshine {

namespace {

// The files the conversion writes (knowledge-design plan section 6c), in the order they are
// wanted, looked for from where the app is run: the checkout's models folder, or beside
// the binary. Q8_0 first: measured against the f16 -- which matched the sentence-transformers
// reference the stored vectors came from -- it scores within 0.002 on every line tried with
// the same top item, at half the size (334 MB). The 4-bit one (187 MB, the token embeddings
// at 4-bit too) is what Moonshine Voice's default `q4` amounts to and drifts up to 0.013,
// which is the order of the picker's margins, so it is the fallback and not the default;
// the f16 is kept as the reference the others are checked against.
constexpr const char* kWeightsNames[] = {
    "embeddinggemma-300m-q8_0.gguf",
    "embeddinggemma-300m-q4_k_s.gguf",
    "embeddinggemma-300m-f16.gguf",
};

void start_backend_once() {
    static std::once_flag once;
    std::call_once(once, [] {
        // Quiet, but errors kept: see llamacpp.cpp, the logger is the process's.
        llama_log_set([](ggml_log_level level, const char* text, void*) {
            if (level == GGML_LOG_LEVEL_ERROR) std::cerr << "embedder: " << text << std::flush;
        }, nullptr);
        llama_backend_init();
    });
}

}  // namespace

struct LlamaEmbedder::Impl {
    std::filesystem::path path;
    llama_model* model = nullptr;
    llama_context* ctx = nullptr;
    const llama_vocab* vocab = nullptr;
    int n_embd = 0;
    int n_ctx = 0;

    ~Impl() {
        if (ctx) llama_free(ctx);
        if (model) llama_model_free(model);
    }
};

LlamaEmbedder::LlamaEmbedder(std::filesystem::path gguf) : impl_(std::make_unique<Impl>()) {
    start_backend_once();
    impl_->path = std::move(gguf);
    if (!std::filesystem::exists(impl_->path)) {
        throw std::runtime_error("no embedding model at " + impl_->path.string());
    }

    llama_model_params weights = llama_model_default_params();
    // On the CPU like the talking model: the game owns the GPU, and at 300M parameters the
    // whole pass is tens of milliseconds there anyway.
    weights.n_gpu_layers = 0;
    // And no GPU device, so that no backend is opened on one: see llamacpp.cpp, where the
    // same default list cost a CUDA context per model beside the game.
    static ggml_backend_dev_t no_devices[] = {nullptr};
    weights.devices = no_devices;
    impl_->model = llama_model_load_from_file(impl_->path.string().c_str(), weights);
    if (!impl_->model) {
        throw std::runtime_error("llama.cpp could not load " + impl_->path.string());
    }
    impl_->vocab = llama_model_get_vocab(impl_->model);
    // What comes out after the dense layers, which for this model is the hidden size again
    // (768 -> 3072 -> 768) but need not be.
    impl_->n_embd = llama_model_n_embd_out(impl_->model);

    llama_context_params params = llama_context_default_params();
    // A line of the player's is a sentence or two; the longest text this is ever asked for
    // is a tale under the document prompt, well inside 512 tokens. A non-causal model needs
    // the whole sequence in one physical batch, so the two sizes are the same.
    params.n_ctx = 512;
    params.n_batch = 512;
    params.n_ubatch = 512;
    // And no operations shipped to a GPU this build may have, for the reason in llamacpp.cpp.
    params.op_offload = false;
    params.embeddings = true;
    // Mean over the tokens, as the model's 1_Pooling says. The GGUF carries this too; said
    // here as well so a file converted without it still pools the right way.
    params.pooling_type = LLAMA_POOLING_TYPE_MEAN;
    const unsigned all = std::thread::hardware_concurrency();
    params.n_threads = all == 0 ? 4 : static_cast<int>((all + 1) / 2);
    params.n_threads_batch = params.n_threads;
    impl_->ctx = llama_init_from_model(impl_->model, params);
    if (!impl_->ctx) {
        throw std::runtime_error("llama.cpp could not make an embedding context for " +
                                 impl_->path.string());
    }
    impl_->n_ctx = static_cast<int>(llama_n_ctx(impl_->ctx));
}

LlamaEmbedder::~LlamaEmbedder() = default;

int LlamaEmbedder::dim() const { return impl_->n_embd; }

std::string LlamaEmbedder::describe() const {
    // The filename, not the GGUF's general.name: the converter writes the Hugging Face
    // snapshot's hash there, and the filename is what says which quant this is.
    return impl_->path.filename().string() + ", " + std::to_string(impl_->n_embd) +
           " dimensions";
}

std::vector<float> LlamaEmbedder::embed(std::string_view text, bool query) {
    std::string prompted(query ? kQueryPrompt : kDocumentPrompt);
    prompted.append(text);

    // BOS and EOS both, as the tokenizer adds them (add_eos_token is true in this model's
    // GGUF): sentence-transformers pooled over both, so leaving one out moves the mean.
    const int needed = -llama_tokenize(impl_->vocab, prompted.data(),
                                       static_cast<int>(prompted.size()), nullptr, 0,
                                       /*add_special=*/true, /*parse_special=*/false);
    std::vector<llama_token> tokens(static_cast<std::size_t>(needed));
    int got = llama_tokenize(impl_->vocab, prompted.data(), static_cast<int>(prompted.size()),
                             tokens.data(), needed, true, false);
    if (got < 0) throw std::runtime_error("llama.cpp could not tokenise the text to embed");
    tokens.resize(static_cast<std::size_t>(got));
    // Cut to fit rather than refuse: a line too long for the window is still a line, and the
    // front of it is the part that says what it is about. The last token is kept as EOS.
    if (static_cast<int>(tokens.size()) > impl_->n_ctx) {
        const llama_token last = tokens.back();
        tokens.resize(static_cast<std::size_t>(impl_->n_ctx));
        tokens.back() = last;
    }

    // Each call is its own sequence: nothing is kept between lines.
    llama_memory_clear(llama_get_memory(impl_->ctx), true);
    llama_batch batch = llama_batch_init(static_cast<int>(tokens.size()), 0, 1);
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        batch.token[i] = tokens[i];
        batch.pos[i] = static_cast<llama_pos>(i);
        batch.n_seq_id[i] = 1;
        batch.seq_id[i][0] = 0;
        batch.logits[i] = 1;  // every token's output is wanted, since the mean is over all
    }
    batch.n_tokens = static_cast<int>(tokens.size());
    const int rc = llama_decode(impl_->ctx, batch);
    llama_batch_free(batch);
    if (rc != 0) throw std::runtime_error("llama.cpp could not run the embedding model");

    const float* pooled = llama_get_embeddings_seq(impl_->ctx, 0);
    if (!pooled) throw std::runtime_error("the embedding model returned no pooled vector");
    std::vector<float> out(pooled, pooled + impl_->n_embd);
    // Unit length, as the stored vectors are, so cosine is a dot product.
    double norm = 0.0;
    for (const float v : out) norm += static_cast<double>(v) * v;
    norm = std::sqrt(norm);
    if (norm > 0.0) {
        for (float& v : out) v = static_cast<float>(v / norm);
    }
    return out;
}

std::filesystem::path LlamaEmbedder::find_weights() {
    std::error_code bad;
    for (const char* name : kWeightsNames) {
        for (const char* dir : {"models", ".", "../models", "../../models"}) {
            const std::filesystem::path candidate = std::filesystem::path(dir) / name;
            if (std::filesystem::exists(candidate, bad)) return candidate;
        }
    }
    return {};
}

}  // namespace moonshine
