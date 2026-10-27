#include "clone_llama.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <random>
#include <thread>

#include "ggml-backend.h"
#include "ggml.h"
#include "gguf.h"
#include "llama.h"
#include "mtmd.h"
#include "word_align.h"

// How a line is made, in the words the vendored engine (vendor/qwen3-tts/qwen_tts.c) uses
// for the same thing, so that the two can be read side by side:
//
//   prompt   [role: <|im_start|> assistant \n]
//            [tts_pad + think] [tts_pad + think_bos] [tts_pad + lang] [tts_pad + think_eos]
//            [tts_pad + x-vector]                     the speaker, scaled as the engine does
//            [tts_bos + codec_pad]
//            [ref text token + codec_pad] ...        the reference transcript from the .qvoice
//            [text token + codec_pad] ...            the line
//            [tts_eos + codec_pad]
//            [tts_pad + codec_bos]
//            [tts_pad + frame] ...                   the reference's codec frames, 16 codes each
//   frame    the talker's logits give codebook 0; the code predictor (libmtmd) gives the other
//            15 and the summed embedding of all 16, which plus tts_pad is the next talker input
//   audio    code2wav (libmtmd) turns the codes into 24 kHz PCM, a window at a time
//
// The talker is a libllama model whose token table is the text vocabulary with the 3072 codec
// rows appended, so every row above is a sum of two of its rows -- read straight from the
// GGUF, since libllama does not hand the table out. The frame rows need the code predictor's
// per-codebook tables too, which live in the mmproj GGUF; those are read the same way.
//
// Where the words fall. The talker is given the whole line before it makes a frame, so
// nothing in what it emits says which word a frame belongs to -- but each frame's row
// attends over the text rows, and in a few of the talker's heads that attention walks
// along the text as the speech advances, the way Whisper's alignment heads do. Those heads
// are read as the line is made: flash attention is turned off for the talker so that the
// softmaxed scores exist as a tensor (kq_soft_max-<layer>), and ggml's eval callback hands
// them over after each layer. A monotonic path through the frames x text-tokens matrix
// (dynamic time warping, as Whisper does it) then gives each token the frame it starts on,
// and tokens group into words by their leading space. The heads to read were chosen by
// scoring every head of every layer against Moonshine's word timestamps on the same audio
// (scripts/attn_heads.py); QWEN_GGUF_ATTN_HEADS overrides them, QWEN_GGUF_WORDS=0 turns the
// reading off (and flash attention back on), and QWEN_GGUF_ATTN_DUMP=<prefix> writes every
// head of every layer, whole rows, for that script to look at.

namespace moonshine {

namespace {

int env_int(const char* name, int fallback) {
    const char* value = std::getenv(name);
    if (!value || !*value) return fallback;
    return std::atoi(value);
}

// ggml, llama.cpp and libmtmd narrate every device they find and every tensor they load,
// and warn about this model's tokenizer config at every start. Keep their errors, which say
// why a line failed, and drop the rest, as llamacpp.cpp does for the LLM. QWEN_GGUF_VERBOSE=1
// lets it all through.
void quiet_logs() {
    static std::once_flag once;
    std::call_once(once, [] {
        // A failed GGML_ASSERT or CUDA call ends in ggml_abort, which by default prints to C's
        // stderr -- which the app's --log tee does not see -- and calls abort(). Three such
        // crashes on 12 Oct 2026 left a log that simply stopped, and it took the crash dump
        // to learn what had happened. Say it through cerr, flushed, before the process goes.
        ggml_set_abort_callback([](const char* message) {
            std::cerr << "voice: ggml is aborting the process: " << message << std::endl;
        });
        if (std::getenv("QWEN_GGUF_VERBOSE")) return;
        const ggml_log_callback quiet = [](ggml_log_level level, const char* text, void*) {
            if (level == GGML_LOG_LEVEL_ERROR) std::cerr << "voice: " << text << std::flush;
        };
        llama_log_set(quiet, nullptr);
        mtmd_log_set(quiet, nullptr);
    });
}

bool voice_name_ok(std::string_view name) {
    if (name.empty() || name.size() > 64) return false;
    for (char c : name) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (!std::isalnum(u) && c != '-' && c != '_') return false;
    }
    return true;
}

void to_pcm16(const float* samples, std::size_t count, std::int16_t* out) {
    for (std::size_t i = 0; i < count; ++i) {
        float sample = samples[i];
        if (sample > 1.f) sample = 1.f;
        if (sample < -1.f) sample = -1.f;
        out[i] = static_cast<std::int16_t>(sample * 32767.f);
    }
}

// The codec ids, as the model config names them. Codec row c is talker token text_vocab + c.
constexpr int kCodecPad = 2148;
constexpr int kCodecBos = 2149;
constexpr int kCodecEos = 2150;
constexpr int kCodecThink = 2154;
constexpr int kCodecNoThink = 2155;
constexpr int kCodecThinkBos = 2156;
constexpr int kCodecThinkEos = 2157;
constexpr int kCodecEnglish = 2050;
// The preset speaker whose row the engine scales the x-vector to (qwen_tts.c, "ryan").
constexpr int kCodecNormRow = 3061;
constexpr int kCodebooks = 16;
constexpr int kCodebookSize = 2048;
constexpr int kModelWindowFrames = 72;  // code2wav's attention window (wav_tfm_swa)
constexpr double kFrameSeconds = 0.08;   // the codec's 12.5 Hz
// The talker heads whose attention tracks the text (see "Where the words fall"), as
// layer.head. Found by scripts/attn_heads.py over 21 lines in three voices (13 Oct 2026):
// against Moonshine's word timestamps these four, averaged, place a word's start within
// 0.10s on average (p90 0.21s); every other head is off by 0.6s or more. The talker's
// attention runs ahead of the sound by a steady four frames (the lead was 0.27-0.39s on
// every line), hence kAlignmentLeadFrames. QWEN_GGUF_ATTN_HEADS overrides the heads.
constexpr const char* kAlignmentHeads = "6.4,5.13,6.5,6.1";
constexpr int kAlignmentLeadFrames = 4;

// Rows of one tensor in a GGUF file, dequantised on demand. The talker's token table and the
// code predictor's codebooks are too large to want in RAM twice and are only needed a few
// hundred rows at a time.
class TensorRows {
public:
    bool open(const std::filesystem::path& file, const char* tensor, std::string& err) {
        gguf_init_params params{};
        params.no_alloc = true;
        ggml_context* meta = nullptr;
        params.ctx = &meta;
        gguf_context* gguf = gguf_init_from_file(file.string().c_str(), params);
        if (!gguf) {
            err = "could not read " + file.string();
            return false;
        }
        const int64_t id = gguf_find_tensor(gguf, tensor);
        if (id < 0) {
            err = file.filename().string() + " has no tensor " + tensor;
            gguf_free(gguf);
            ggml_free(meta);
            return false;
        }
        const ggml_tensor* t = ggml_get_tensor(meta, tensor);
        type_ = gguf_get_tensor_type(gguf, id);
        ne0_ = t->ne[0];
        ne1_ = t->ne[1];
        ne2_ = t->ne[2];
        row_bytes_ = ggml_row_size(type_, ne0_);
        offset_ = gguf_get_data_offset(gguf) + gguf_get_tensor_offset(gguf, id);
        gguf_free(gguf);
        ggml_free(meta);
        const ggml_type_traits* traits = ggml_get_type_traits(type_);
        if (type_ != GGML_TYPE_F32 && !(traits && traits->to_float)) {
            err = std::string(tensor) + " is stored as " + ggml_type_name(type_) + ", which cannot be read back";
            return false;
        }
#ifdef _WIN32
        fopen_s(&fp_, file.string().c_str(), "rb");
#else
        fp_ = std::fopen(file.string().c_str(), "rb");
#endif
        if (!fp_) {
            err = "could not open " + file.string();
            return false;
        }
        return true;
    }

    ~TensorRows() {
        if (fp_) std::fclose(fp_);
    }

    int64_t rows() const { return ne1_; }
    int64_t width() const { return ne0_; }
    int64_t planes() const { return ne2_; }

    // Row `row` of plane `plane` (0 for a 2-D tensor), as floats. Empty when out of range.
    std::vector<float> read(int64_t row, int64_t plane = 0) {
        std::vector<float> out;
        if (!fp_ || row < 0 || row >= ne1_ || plane < 0 || plane >= ne2_) return out;
        const uint64_t at = offset_ + static_cast<uint64_t>(plane * ne1_ + row) * row_bytes_;
        std::vector<unsigned char> raw(row_bytes_);
#ifdef _WIN32
        if (_fseeki64(fp_, static_cast<long long>(at), SEEK_SET) != 0) return out;
#else
        if (std::fseek(fp_, static_cast<long>(at), SEEK_SET) != 0) return out;
#endif
        if (std::fread(raw.data(), 1, row_bytes_, fp_) != row_bytes_) return out;
        out.resize(static_cast<std::size_t>(ne0_));
        if (type_ == GGML_TYPE_F32) {
            std::memcpy(out.data(), raw.data(), row_bytes_);
        } else {
            ggml_get_type_traits(type_)->to_float(raw.data(), out.data(), ne0_);
        }
        return out;
    }

private:
    std::FILE* fp_ = nullptr;
    ggml_type type_ = GGML_TYPE_F32;
    int64_t ne0_ = 0, ne1_ = 1, ne2_ = 1;
    std::size_t row_bytes_ = 0;
    uint64_t offset_ = 0;
};

// What a .qvoice holds that this engine uses: the small part. The talker weight deltas and
// the WOVR/TPAD tables that follow are the model's business (baked into the graft GGUF).
struct VoiceFile {
    std::vector<float> xvector;
    std::string ref_text;
    std::vector<int> ref_codes;  // n_frames * 16
    int n_frames = 0;
    int language_id = 0;
};

bool read_u32(std::FILE* f, uint32_t& v) { return std::fread(&v, 4, 1, f) == 1; }

bool read_qvoice(const std::filesystem::path& path, VoiceFile& voice, std::string& err) {
    std::FILE* f = nullptr;
#ifdef _WIN32
    fopen_s(&f, path.string().c_str(), "rb");
#else
    f = std::fopen(path.string().c_str(), "rb");
#endif
    if (!f) {
        err = "could not open " + path.string();
        return false;
    }
    struct Close {
        std::FILE* f;
        ~Close() { std::fclose(f); }
    } close{f};
    char magic[4];
    uint32_t version = 0, enc_dim = 1024;
    if (std::fread(magic, 1, 4, f) != 4 || std::memcmp(magic, "QVCE", 4) != 0 || !read_u32(f, version) ||
        version < 1 || version > 3) {
        err = path.filename().string() + " is not a .qvoice this engine knows";
        return false;
    }
    if (version >= 2 && !read_u32(f, enc_dim)) {
        err = "truncated " + path.filename().string();
        return false;
    }
    if (enc_dim == 0 || enc_dim > 8192) {
        err = path.filename().string() + ": implausible x-vector width";
        return false;
    }
    voice.xvector.resize(enc_dim);
    uint32_t text_len = 0, n_frames = 0;
    if (std::fread(voice.xvector.data(), sizeof(float), enc_dim, f) != enc_dim || !read_u32(f, text_len) ||
        text_len > (1u << 20)) {
        err = "truncated " + path.filename().string();
        return false;
    }
    voice.ref_text.resize(text_len);
    if (text_len && std::fread(voice.ref_text.data(), 1, text_len, f) != text_len) {
        err = "truncated " + path.filename().string();
        return false;
    }
    if (!read_u32(f, n_frames) || n_frames > 100000) {
        err = "truncated " + path.filename().string();
        return false;
    }
    voice.n_frames = static_cast<int>(n_frames);
    voice.ref_codes.resize(static_cast<std::size_t>(n_frames) * kCodebooks);
    if (n_frames &&
        std::fread(voice.ref_codes.data(), sizeof(int), voice.ref_codes.size(), f) != voice.ref_codes.size()) {
        err = "truncated " + path.filename().string();
        return false;
    }
    if (version >= 3) {
        char tag[4];
        if (std::fread(tag, 1, 4, f) == 4 && std::memcmp(tag, "META", 4) == 0) {
            uint32_t lang_id = 0;
            if (read_u32(f, lang_id)) voice.language_id = static_cast<int>(lang_id);
        }
    }
    return true;
}

// Sampling as the engine does it (qwen_tts.c: temperature 0.9, top-k 50, top-p 1, repetition
// penalty 1.05 over every code so far, EOS held off for the first frames and then nudged up
// once the line has run past three frames a text token).
struct Sampler {
    float temperature = 0.9f;
    int top_k = 50;
    float top_p = 1.0f;
    float rep_penalty = 1.05f;
    int eos_suppress_frames = 2;
    float eos_frames_per_token = 3.0f;
    float eos_start_multiple = 2.0f;
    float eos_ramp_per_frame = 0.5f;
    float eos_ramp_cap = 10.0f;
    std::mt19937 rng;

    explicit Sampler(unsigned seed) : rng(seed ? seed : std::random_device{}()) {}

    // logits over the codec rows (3072); returns a codec id in [0, 2048) or kCodecEos.
    int sample(std::vector<float>& logits, const std::vector<int>& previous, int frame, int text_tokens) {
        for (float& l : logits) l = std::clamp(l, -100.f, 100.f);
        for (int c = kCodebookSize; c < static_cast<int>(logits.size()); ++c) {
            if (c != kCodecEos) logits[c] = -1e30f;
        }
        if (frame < eos_suppress_frames) logits[kCodecEos] = -1e30f;
        const float expected = text_tokens * eos_frames_per_token;
        const float boost_start = eos_start_multiple * expected;
        if (expected > 0 && frame > boost_start) {
            logits[kCodecEos] += std::min(eos_ramp_cap, eos_ramp_per_frame * (frame - boost_start));
        }
        if (rep_penalty != 1.f) {
            for (int tok : previous) {
                if (tok < 0 || tok >= static_cast<int>(logits.size())) continue;
                if (logits[tok] > 0) logits[tok] /= rep_penalty;
                else logits[tok] *= rep_penalty;
            }
        }
        const int n = static_cast<int>(logits.size());
        if (temperature < 1e-6f) {
            return static_cast<int>(std::max_element(logits.begin(), logits.end()) - logits.begin());
        }
        std::vector<int> order(n);
        for (int i = 0; i < n; ++i) order[i] = i;
        const int k = top_k > 0 && top_k < n ? top_k : n;
        std::partial_sort(order.begin(), order.begin() + k, order.end(),
                          [&](int a, int b) { return logits[a] > logits[b]; });
        order.resize(k);
        std::vector<double> probs(k);
        const double top = logits[order[0]];
        double sum = 0;
        for (int i = 0; i < k; ++i) {
            probs[i] = std::exp((logits[order[i]] - top) / temperature);
            sum += probs[i];
        }
        for (double& p : probs) p /= sum;
        int keep = k;
        if (top_p > 0 && top_p < 1.f) {
            double running = 0;
            for (int i = 0; i < k; ++i) {
                running += probs[i];
                if (running >= top_p) {
                    keep = i + 1;
                    break;
                }
            }
        }
        double total = 0;
        for (int i = 0; i < keep; ++i) total += probs[i];
        std::uniform_real_distribution<double> uniform(0.0, total);
        double draw = uniform(rng);
        for (int i = 0; i < keep; ++i) {
            draw -= probs[i];
            if (draw <= 0) return order[i];
        }
        return order[keep - 1];
    }
};

// One attention head to read for word timing: layer and head index in the talker.
struct Head {
    int layer = 0;
    int head = 0;
};

// "l.h,l.h,..." -> heads; anything unparseable is dropped.
std::vector<Head> parse_heads(const std::string& spec) {
    std::vector<Head> heads;
    std::size_t at = 0;
    while (at < spec.size()) {
        std::size_t end = spec.find(',', at);
        if (end == std::string::npos) end = spec.size();
        const std::string item = spec.substr(at, end - at);
        at = end + 1;
        const std::size_t dot = item.find('.');
        if (dot == std::string::npos) continue;
        char* stop = nullptr;
        const long l = std::strtol(item.c_str(), &stop, 10);
        if (stop != item.c_str() + dot) continue;
        const long h = std::strtol(item.c_str() + dot + 1, &stop, 10);
        if (*stop != '\0') continue;
        heads.push_back({static_cast<int>(l), static_cast<int>(h)});
    }
    return heads;
}

}  // namespace

struct LlamaClone::Impl {
    std::filesystem::path model_dir;
    std::filesystem::path voices_dir;

    std::mutex mutex;
    std::condition_variable loaded_cv;
    bool loaded = false;
    std::string failure;
    std::thread loader;

    llama_model* model = nullptr;
    llama_context* ctx = nullptr;
    mtmd_context* mctx = nullptr;
    const llama_vocab* vocab = nullptr;
    int n_embd = 0;
    int text_vocab = 0;  // codec row c is token text_vocab + c
    bool mrope = false;
    int sample_rate = 24000;
    // The most one flush_wav decodes. code2wav's attention window is 72 frames (5.76s);
    // QWEN_GGUF_WINDOW lowers it, and main.cpp does so beside the game (see load_or_fail).
    int window_frames = kModelWindowFrames;
    bool cuda_graphs = true;

    TensorRows token_rows;  // token_embd.weight of the talker
    TensorRows code_rows;   // a.gen.code.embd.weight of the mmproj, [15][2048][n_embd]
    std::map<int, std::vector<float>> row_cache;

    // Special text tokens, found by their text.
    llama_token tok_pad = -1, tok_bos = -1, tok_eos = -1;
    llama_token tok_im_start = -1, tok_assistant = -1, tok_newline = -1;

    // Word timing ("Where the words fall", above). The heads read, the layers they are in
    // (the callback reads whole layers and keeps the heads wanted), and the reading of the
    // line being made.
    std::vector<Head> heads;
    std::vector<int> layers;  // sorted, distinct
    bool reads = false;       // the context was made with the callback and without flash attention
    int n_head = 0;
    int n_layer = 0;
    std::string dump_prefix;  // QWEN_GGUF_ATTN_DUMP: every head, whole rows, to <prefix>.attn
    int dumped = 0;
    struct Reading {
        bool on = false;
        int text_first = 0;  // the line's text rows: prompt rows [text_first, text_first + text_count)
        int text_count = 0;
        int rows = 0;                // query rows read so far (one per frame, the first from the prompt's last row)
        std::vector<float> weights;  // rows x heads x text_count, the heads in `heads` order
        // The dump: rows x n_layer x n_head x cols, whole attention rows (fp16 on disk).
        int cols = 0;
        std::vector<float> all;
    } reading;

    // ggml asks, before each node, whether it is wanted, then hands over the wanted ones
    // once computed. Only the softmaxed attention of the layers read, only while a line's
    // frames are being made.
    static bool eval_callback(ggml_tensor* t, bool ask, void* user) {
        Impl* self = static_cast<Impl*>(user);
        if (!self->reading.on) return !ask;
        if (std::strncmp(t->name, "kq_soft_max-", 12) != 0) return !ask;
        const int layer = std::atoi(t->name + 12);
        const bool dumping = !self->dump_prefix.empty();
        if (!dumping && !std::binary_search(self->layers.begin(), self->layers.end(), layer)) return !ask;
        if (ask) return true;
        self->take(t, layer);
        return true;
    }

    // t is [n_kv, n_query, n_head] floats; the last query row is the one whose output is
    // about to be sampled from. Appends this layer's share of the current reading row.
    void take(const ggml_tensor* t, int layer) {
        if (t->type != GGML_TYPE_F32 || t->ne[2] != n_head) return;
        const int64_t n_kv = t->ne[0];
        const int64_t last = t->ne[1] - 1;
        const int first = reading.text_first, count = reading.text_count;
        if (first + count > n_kv) return;
        const std::size_t row_base = static_cast<std::size_t>(reading.rows) * heads.size() * count;
        if (reading.weights.size() < row_base + heads.size() * count) {
            reading.weights.resize(row_base + heads.size() * count, 0.f);
        }
        for (std::size_t i = 0; i < heads.size(); ++i) {
            if (heads[i].layer != layer) continue;
            const int h = heads[i].head;
            if (h < 0 || h >= n_head) continue;
            ggml_backend_tensor_get(t, reading.weights.data() + row_base + i * count,
                                    static_cast<std::size_t>(h) * t->nb[2] + static_cast<std::size_t>(last) * t->nb[1] +
                                        static_cast<std::size_t>(first) * t->nb[0],
                                    sizeof(float) * count);
        }
        if (!dump_prefix.empty()) {
            if (reading.cols == 0) reading.cols = static_cast<int>(n_kv);
            const int cols = reading.cols;
            const std::size_t per_row = static_cast<std::size_t>(n_layer) * n_head * cols;
            const std::size_t base = static_cast<std::size_t>(reading.rows) * per_row;
            if (reading.all.size() < base + per_row) reading.all.resize(base + per_row, 0.f);
            const int64_t take_cols = std::min<int64_t>(cols, n_kv);
            for (int h = 0; h < n_head; ++h) {
                ggml_backend_tensor_get(t, reading.all.data() + base + (static_cast<std::size_t>(layer) * n_head + h) * cols,
                                        static_cast<std::size_t>(h) * t->nb[2] + static_cast<std::size_t>(last) * t->nb[1],
                                        sizeof(float) * take_cols);
            }
        }
    }

    struct Voice {
        VoiceFile file;
        std::vector<float> speaker_row;        // tts_pad + scaled x-vector
        std::vector<float> frame_rows;         // n_frames rows of tts_pad + 16 summed codes
        std::vector<llama_token> ref_tokens;   // the reference transcript
    };
    std::map<std::string, std::unique_ptr<Voice>> open_voices;

    Impl(std::filesystem::path model, std::filesystem::path voices)
        : model_dir(std::move(model)), voices_dir(std::move(voices)) {
        loader = std::thread([this] { load(); });
    }

    ~Impl() {
        if (loader.joinable()) loader.join();
        std::lock_guard<std::mutex> held(mutex);
        open_voices.clear();
        if (mctx) mtmd_free(mctx);
        if (ctx) llama_free(ctx);
        if (model) llama_model_free(model);
    }

    static std::filesystem::path talker_file(const std::filesystem::path& dir) {
        std::error_code error;
        if (!std::filesystem::is_directory(dir, error)) return {};
        for (const auto& entry : std::filesystem::directory_iterator(dir, error)) {
            const std::string name = entry.path().filename().string();
            if (entry.path().extension() == ".gguf" && name.rfind("mmproj", 0) != 0) return entry.path();
        }
        return {};
    }

    static std::filesystem::path mmproj_file(const std::filesystem::path& dir) {
        std::error_code error;
        if (!std::filesystem::is_directory(dir, error)) return {};
        for (const auto& entry : std::filesystem::directory_iterator(dir, error)) {
            const std::string name = entry.path().filename().string();
            if (entry.path().extension() == ".gguf" && name.rfind("mmproj", 0) == 0) return entry.path();
        }
        return {};
    }

    // The token whose stored text is `text` -- the special tokens, whose text is the tag
    // itself. Plain text is stored byte-level ("\n" is "Ċ"), so for that see single_token().
    llama_token find_token(const char* text) const {
        const int32_t n = llama_vocab_n_tokens(vocab);
        for (llama_token t = 0; t < n; ++t) {
            if (std::strcmp(text, llama_vocab_get_text(vocab, t)) == 0) return t;
        }
        return -1;
    }

    // The one token `text` tokenizes to, or -1 when it takes more than one.
    llama_token single_token(const char* text) const {
        llama_token out[4];
        const int32_t n = llama_tokenize(vocab, text, static_cast<int32_t>(std::strlen(text)), out, 4, false, false);
        return n == 1 ? out[0] : -1;
    }

    void load() {
        const auto started = std::chrono::steady_clock::now();
        std::string why = load_or_fail();
        const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started);
        if (!why.empty()) {
            if (mctx) mtmd_free(mctx);
            if (ctx) llama_free(ctx);
            if (model) llama_model_free(model);
            mctx = nullptr;
            ctx = nullptr;
            model = nullptr;
        }
        {
            std::lock_guard<std::mutex> held(mutex);
            failure = why;
            loaded = true;
        }
        loaded_cv.notify_all();
        if (why.empty()) {
            std::cerr << "voice: Qwen3-TTS (llama.cpp, GPU) loaded from " << model_dir.string() << " in "
                      << (took.count() / 1000.0) << "s";
            if (window_frames != kModelWindowFrames) std::cerr << ", code2wav windows of " << window_frames << " frames";
            if (!cuda_graphs) std::cerr << ", CUDA graphs off";
            std::cerr << "\n";
        } else {
            std::cerr << "voice: llama.cpp clone engine: " << why << "\n";
        }
    }

    // Empty on success, otherwise why not.
    std::string load_or_fail() {
        const std::filesystem::path talker = talker_file(model_dir);
        const std::filesystem::path mmproj = mmproj_file(model_dir);
        if (talker.empty() || mmproj.empty()) {
            return "no GGUF pair (<talker>.gguf and mmproj-*.gguf) in " + model_dir.string();
        }
        quiet_logs();
        // Beside the game the GPU is shared with a renderer that already has it flat out, and
        // Windows resets the GPU -- taking the game, the desktop and this process with it --
        // when any one submission fails to yield within two seconds (a TDR, three of them on
        // 12 Oct 2026, each while a line was being made). What this engine submits is bounded
        // two ways. ggml normally records each graph as one CUDA graph and launches it whole,
        // the code predictor's fifteen steps or a code2wav window in a single submission;
        // QWEN_GGUF_CUDA_GRAPHS=0 has ggml launch the kernels one by one instead, which the
        // scheduler can interleave with the game's frames. And QWEN_GGUF_WINDOW caps the
        // frames a code2wav window decodes at once, the largest graph here. Measured on the Windows laptop
        // (GPU idle, a 16.6s line): the window cap is free, graphs off is 2.1x the time per
        // line, so main.cpp sets only the window for --game (share_the_gpu, which says why).
        cuda_graphs = env_int("QWEN_GGUF_CUDA_GRAPHS", 1) != 0;
        if (!cuda_graphs) {
            // ggml reads this once, at its first graph, so it is set before any backend runs.
#ifdef _WIN32
            _putenv_s("GGML_CUDA_DISABLE_GRAPHS", "1");
#else
            setenv("GGML_CUDA_DISABLE_GRAPHS", "1", 1);
#endif
        }
        window_frames = std::clamp(env_int("QWEN_GGUF_WINDOW", kModelWindowFrames), kFirstBatchFrames,
                                   kModelWindowFrames);
        llama_backend_init();
        llama_model_params mp = llama_model_default_params();
        mp.n_gpu_layers = env_int("QWEN_GGUF_GPU_LAYERS", 999);
        model = llama_model_load_from_file(talker.string().c_str(), mp);
        if (!model) return "could not load " + talker.string();
        vocab = llama_model_get_vocab(model);
        n_embd = llama_model_n_embd(model);

        llama_context_params cp = llama_context_default_params();
        cp.n_ctx = static_cast<uint32_t>(env_int("QWEN_GGUF_CTX", 2048));
        // Small physical batches: a prompt of a hundred-odd rows goes through in short kernels
        // rather than one long one, which matters on a GPU the game is drawing with.
        cp.n_batch = 256;
        cp.n_ubatch = static_cast<uint32_t>(env_int("QWEN_GGUF_UBATCH", 32));
        cp.n_seq_max = 1;
        // The CPU threads, where any of this runs on the CPU (QWEN_GGUF_GPU_LAYERS=0, or no
        // GPU): the same knob the mmproj and the vendored engine take, in place of ggml's
        // default of four.
        if (const int threads = env_int("QWEN_THREADS", 0); threads > 0) {
            cp.n_threads = threads;
            cp.n_threads_batch = threads;
        }
        cp.embeddings = true;
        cp.pooling_type = LLAMA_POOLING_TYPE_NONE;
        cp.no_perf = true;
        // Word timing reads the attention scores, which flash attention never materialises.
        n_head = llama_model_n_head(model);
        n_layer = llama_model_n_layer(model);
        if (const char* dump = std::getenv("QWEN_GGUF_ATTN_DUMP"); dump && *dump) dump_prefix = dump;
        if (env_int("QWEN_GGUF_WORDS", 1) != 0 || !dump_prefix.empty()) {
            const char* spec = std::getenv("QWEN_GGUF_ATTN_HEADS");
            heads = parse_heads(spec && *spec ? spec : kAlignmentHeads);
            for (const Head& h : heads) layers.push_back(h.layer);
            std::sort(layers.begin(), layers.end());
            layers.erase(std::unique(layers.begin(), layers.end()), layers.end());
            cp.flash_attn_type = LLAMA_FLASH_ATTN_TYPE_DISABLED;
            cp.cb_eval = eval_callback;
            cp.cb_eval_user_data = this;
            reads = true;
        }
        ctx = llama_init_from_model(model, cp);
        if (!ctx) return "could not make a context for " + talker.filename().string();

        mtmd_context_params mparams = mtmd_context_params_default();
        mparams.use_gpu = env_int("QWEN_GGUF_MMPROJ_GPU", 1) != 0;
        mparams.n_threads = std::max(1, env_int("QWEN_THREADS", 4));
        mparams.print_timings = false;
        mparams.warmup = false;
        mctx = mtmd_init_from_file(mmproj.string().c_str(), model, mparams);
        if (!mctx) return "could not load " + mmproj.string();
        const mtmd_gen_audio_info info = mtmd_gen_audio_get_info(mctx);
        if (info.type != MTMD_GEN_AUDIO_TYPE_QWEN3TTS) return mmproj.filename().string() + " is not a Qwen3-TTS mmproj";
        sample_rate = info.sample_rate;

        tok_im_start = find_token("<|im_start|>");
        tok_assistant = find_token("assistant");
        tok_newline = single_token("\n");
        tok_pad = find_token("<tts_pad>");
        tok_bos = find_token("<tts_text_bos>");
        tok_eos = find_token("<tts_text_eod>");
        const llama_token codec_zero = find_token("<|codec_0|>");
        if (tok_im_start < 0 || tok_assistant < 0 || tok_newline < 0 || tok_pad < 0 || tok_bos < 0 ||
            tok_eos < 0 || codec_zero < 0) {
            return "the talker's vocabulary is missing a Qwen3-TTS special token";
        }
        text_vocab = codec_zero;
        mrope = llama_model_rope_type(model) == LLAMA_ROPE_TYPE_MROPE ||
                llama_model_rope_type(model) == LLAMA_ROPE_TYPE_IMROPE;

        std::string err;
        if (!token_rows.open(talker, "token_embd.weight", err)) return err;
        if (token_rows.width() != n_embd) return "token_embd is not n_embd wide";
        if (!code_rows.open(mmproj, "a.gen.code.embd.weight", err)) return err;
        if (code_rows.width() != n_embd || code_rows.planes() != kCodebooks - 1) {
            return "the code predictor's codebooks are not the talker's width; is this the 0.6B?";
        }
        return {};
    }

    const std::vector<float>& row(int token) {
        auto it = row_cache.find(token);
        if (it != row_cache.end()) return it->second;
        std::vector<float> r = token_rows.read(token);
        if (r.empty()) r.assign(static_cast<std::size_t>(n_embd), 0.f);
        return row_cache.emplace(token, std::move(r)).first->second;
    }
    const std::vector<float>& codec_row(int codec) { return row(text_vocab + codec); }

    static void add_into(std::vector<float>& dst, const std::vector<float>& src) {
        for (std::size_t i = 0; i < dst.size() && i < src.size(); ++i) dst[i] += src[i];
    }
    static float norm_of(const std::vector<float>& v) {
        double s = 0;
        for (float x : v) s += double(x) * x;
        return static_cast<float>(std::sqrt(s));
    }

    std::vector<llama_token> tokenize(const std::string& text) {
        std::vector<llama_token> out(text.size() + 8);
        int n = llama_tokenize(vocab, text.data(), static_cast<int>(text.size()), out.data(),
                               static_cast<int>(out.size()), false, true);
        if (n < 0) {
            out.resize(static_cast<std::size_t>(-n));
            n = llama_tokenize(vocab, text.data(), static_cast<int>(text.size()), out.data(),
                               static_cast<int>(out.size()), false, true);
        }
        out.resize(static_cast<std::size_t>(std::max(0, n)));
        return out;
    }

    // Caller holds `mutex`.
    Voice* voice_named(std::string_view name) {
        const std::string key(name);
        auto it = open_voices.find(key);
        if (it != open_voices.end()) return it->second.get();
        std::unique_ptr<Voice> voice;
        if (voice_name_ok(name)) {
            const std::filesystem::path file = voices_dir / (key + ".qvoice");
            std::error_code error;
            std::string err;
            VoiceFile parsed;
            if (!std::filesystem::is_regular_file(file, error)) {
                std::cerr << "voice: no clone " << file.string() << "\n";
            } else if (!read_qvoice(file, parsed, err)) {
                std::cerr << "voice: " << err << "\n";
            } else if (static_cast<int>(parsed.xvector.size()) != n_embd) {
                std::cerr << "voice: " << file.filename().string() << " has an x-vector of " << parsed.xvector.size()
                          << ", the talker wants " << n_embd << "\n";
            } else {
                voice = std::make_unique<Voice>();
                voice->file = std::move(parsed);
                prepare(*voice);
            }
        }
        Voice* raw = voice.get();
        open_voices[key] = std::move(voice);
        return raw;
    }

    // The rows a voice adds to every prompt, made once.
    void prepare(Voice& voice) {
        const std::vector<float>& pad = row(tok_pad);
        // The x-vector sits where a preset speaker's codec row would, scaled to that row's
        // norm, as the engine does for its presets' sake.
        voice.speaker_row = pad;
        const float emb_norm = norm_of(voice.file.xvector);
        const float ref_norm = norm_of(codec_row(kCodecNormRow));
        const float scale = (ref_norm > 0.1f && emb_norm > 0.1f) ? ref_norm / emb_norm : 1.f;
        for (int i = 0; i < n_embd; ++i) voice.speaker_row[i] += voice.file.xvector[i] * scale;

        voice.frame_rows.assign(static_cast<std::size_t>(voice.file.n_frames) * n_embd, 0.f);
        for (int f = 0; f < voice.file.n_frames; ++f) {
            float* dst = voice.frame_rows.data() + static_cast<std::size_t>(f) * n_embd;
            std::memcpy(dst, pad.data(), sizeof(float) * n_embd);
            const int* codes = voice.file.ref_codes.data() + f * kCodebooks;
            const std::vector<float>& first = codec_row(codes[0]);
            for (int i = 0; i < n_embd; ++i) dst[i] += first[i];
            for (int g = 0; g < kCodebooks - 1; ++g) {
                const int code = codes[g + 1];
                if (code < 0 || code >= kCodebookSize) continue;
                const std::vector<float> r = code_rows.read(code, g);
                for (int i = 0; i < n_embd && i < static_cast<int>(r.size()); ++i) dst[i] += r[i];
            }
        }
        voice.ref_tokens = tokenize(voice.file.ref_text);
    }

    // One batch of embedding rows at consecutive positions from `pos`; the last row's output
    // is wanted. Returns false when the decode failed.
    bool decode_rows(llama_batch_ext* batch, const float* rows, int n, int pos) {
        llama_batch_ext_clear(batch);
        for (int i = 0; i < n; ++i) {
            const llama_embd e{rows + static_cast<std::size_t>(i) * n_embd, 1, static_cast<std::size_t>(n_embd)};
            const int32_t idx = llama_batch_ext_add_embd(batch, 0, e);
            if (idx < 0) return false;
            llama_pos p[4] = {pos + i, pos + i, pos + i, pos + i};
            llama_batch_ext_set_pos(batch, idx, p);
            // An embeddings context outputs every row whatever is asked (and says so when
            // asked otherwise); only the last row's output is read.
            llama_batch_ext_set_output_logits(batch, idx, true);
            llama_batch_ext_set_output_embd(batch, idx, true);
        }
        return llama_process(ctx, LLAMA_PROCESS_TYPE_DECODE, batch) == 0;
    }

    struct Line {
        std::vector<int32_t> codes;        // frames * 16, not yet turned into audio
        std::vector<uint8_t> wav_state;    // code2wav's carried state between windows
        std::vector<float> pcm;
        std::size_t delivered = 0;
        uint32_t seed = 0;
        // Batching of code2wav, see flush_wav: frames to gather before the next flush, when
        // the last flush was and when the first audio went out.
        int batch_frames = kFirstBatchFrames;
        std::chrono::steady_clock::time_point last_flush{};
        std::chrono::steady_clock::time_point first_audio{};
    };

    static constexpr int kFirstBatchFrames = 12;  // about a second of speech

    // code2wav over the buffered codes, once enough have gathered. Carried state makes the
    // batches join seamlessly, and the graph is built for however many frames are given (the
    // patch in vendor/patches/llama.cpp), so the first batch can be short: 12 frames, about a
    // second of speech, is enough to start playing. The batches then grow, so that the rest
    // costs fewer, larger kernels, but only as far as the speaker can stand: playback starts
    // on the first chunk and the next must land before that one has played out. So each
    // batch is sized from what the last one measured -- wall time per frame, talker and
    // code2wav together, which is whatever the GPU gives this process while the game has it
    // too -- against the audio the speaker is still holding, with room to spare.
    bool flush_wav(Line& line, bool last) {
        const std::size_t frames = line.codes.size() / kCodebooks;
        if (frames == 0) return true;
        if (!last && frames < static_cast<std::size_t>(line.batch_frames)) return true;
        mtmd_gen_inp inp = mtmd_gen_inp_default(mctx);
        inp.type = MTMD_GEN_PROCESS_TYPE_GEN_WAV;
        inp.codes = line.codes.data();
        inp.n_codes = line.codes.size();
        inp.seed = line.seed;
        inp.state_data = line.wav_state.empty() ? nullptr : reinterpret_cast<const char*>(line.wav_state.data());
        inp.state_size = line.wav_state.size();
        mtmd_gen_out out{};
        if (mtmd_gen_audio_process(mctx, &inp, &out) != 0) return false;
        line.pcm.insert(line.pcm.end(), out.audio, out.audio + out.n_samples);
        line.wav_state.assign(out.state_data, out.state_data + out.state_size);
        line.codes.clear();

        const auto now = std::chrono::steady_clock::now();
        if (line.first_audio == std::chrono::steady_clock::time_point{}) {
            line.first_audio = now;
        } else {
            const double per_frame = std::chrono::duration<double>(now - line.last_flush).count() / frames;
            const double made_s = line.pcm.size() / static_cast<double>(sample_rate);
            const double played_s = std::chrono::duration<double>(now - line.first_audio).count();
            const double held_s = made_s - played_s;  // what the speaker still has to play
            // Spend at most half of that on the next batch.
            const double affordable = per_frame > 0 ? held_s * 0.5 / per_frame : window_frames;
            const int next = static_cast<int>(std::min<double>(affordable, window_frames));
            line.batch_frames = std::max(kFirstBatchFrames, std::min(next, line.batch_frames * 2));
        }
        line.last_flush = now;
        return true;
    }

    // The text of a token, as the tokenizer would print it (a leading space where the text
    // had one).
    std::string piece(llama_token token) const {
        char buf[64];
        int n = llama_token_to_piece(vocab, token, buf, sizeof(buf), 0, true);
        if (n < 0) {
            std::string big(static_cast<std::size_t>(-n), '\0');
            n = llama_token_to_piece(vocab, token, big.data(), static_cast<int32_t>(big.size()), 0, true);
            big.resize(static_cast<std::size_t>(std::max(0, n)));
            return big;
        }
        return std::string(buf, static_cast<std::size_t>(n));
    }

    // The words placed from the frames read so far (word_align.h does the placing).
    std::vector<Word> words_so_far(const std::vector<llama_token>& tokens, int frames) {
        if (heads.empty() || frames <= 0) return {};
        const int count = reading.text_count;
        const std::vector<int> starts = align_tokens(reading.weights, frames, static_cast<int>(heads.size()), count);
        std::vector<std::string> pieces;
        pieces.reserve(tokens.size());
        for (llama_token t : tokens) pieces.push_back(piece(t));
        std::vector<Word> words;
        for (const WordSpan& span : words_of(pieces, starts, frames, kAlignmentLeadFrames, kFrameSeconds)) {
            words.push_back({span.text, span.start, span.end});
        }
        return words;
    }

    // QWEN_GGUF_ATTN_DUMP: the whole reading of this line, every layer and head, for
    // scripts/attn_heads.py. A text header, then fp16 rows x n_layer x n_head x cols.
    void write_dump(const std::vector<llama_token>& tokens, int frames, int n_prompt) {
        if (dump_prefix.empty() || frames <= 0 || reading.cols == 0) return;
        std::string path = dump_prefix;
        if (dumped > 0) path += "-" + std::to_string(dumped);
        path += ".attn";
        ++dumped;
        std::ofstream out(path, std::ios::binary);
        if (!out) {
            std::cerr << "voice: could not write " << path << "\n";
            return;
        }
        out << "qwen3tts-attn 1\n";
        out << "frames " << frames << " layers " << n_layer << " heads " << n_head << " cols " << reading.cols
            << " text_first " << reading.text_first << " text_count " << reading.text_count << " n_prompt " << n_prompt
            << "\n";
        for (llama_token t : tokens) {
            std::string p = piece(t);
            for (char& c : p) {
                if (c == '\n' || c == '\r' || c == '\t') c = ' ';
            }
            out << "tok " << p << "\n";
        }
        out << "end\n";
        const std::size_t per_row = static_cast<std::size_t>(n_layer) * n_head * reading.cols;
        std::vector<ggml_fp16_t> half(per_row);
        for (int f = 0; f < frames; ++f) {
            ggml_fp32_to_fp16_row(reading.all.data() + static_cast<std::size_t>(f) * per_row, half.data(),
                                  static_cast<int64_t>(per_row));
            out.write(reinterpret_cast<const char*>(half.data()), static_cast<std::streamsize>(per_row * sizeof(ggml_fp16_t)));
        }
        std::cerr << "voice: attention written to " << path << "\n";
    }

    // The line, to its end or until stopped. on_audio may be null (collect only). Returns
    // the PCM made, or nothing with the reason on stderr. `words` gets the word timing
    // (empty when the reading is off); on_words, if given, hears it as it settles.
    std::vector<float> generate(Voice& voice, const std::string& text, const OnAudio* on_audio,
                                const Cancelled* cancelled, const OnWords* on_words, std::vector<Word>& words,
                                bool& cut_off, double& first_audio_s) {
        cut_off = false;
        first_audio_s = 0;
        words.clear();
        const auto started = std::chrono::steady_clock::now();
        const std::vector<llama_token> text_tokens = tokenize(text);
        if (text_tokens.empty()) {
            std::cerr << "voice: nothing to say\n";
            return {};
        }

        // The prompt, row by row.
        std::vector<float> prompt;
        auto push = [&](const std::vector<float>& a, const std::vector<float>* b) {
            const std::size_t at = prompt.size();
            prompt.insert(prompt.end(), a.begin(), a.end());
            if (b) for (int i = 0; i < n_embd; ++i) prompt[at + i] += (*b)[i];
        };
        push(row(tok_im_start), nullptr);
        push(row(tok_assistant), nullptr);
        push(row(tok_newline), nullptr);
        const int lang = voice.file.language_id > 0 ? voice.file.language_id : kCodecEnglish;
        const std::vector<float>& pad = row(tok_pad);
        push(pad, &codec_row(kCodecThink));
        push(pad, &codec_row(kCodecThinkBos));
        push(pad, &codec_row(lang));
        push(pad, &codec_row(kCodecThinkEos));
        push(voice.speaker_row, nullptr);
        const std::vector<float>& codec_pad = codec_row(kCodecPad);
        push(row(tok_bos), &codec_pad);
        for (llama_token t : voice.ref_tokens) push(row(t), &codec_pad);
        const int text_first = static_cast<int>(prompt.size() / n_embd);
        for (llama_token t : text_tokens) push(row(t), &codec_pad);
        push(row(tok_eos), &codec_pad);
        push(pad, &codec_row(kCodecBos));
        prompt.insert(prompt.end(), voice.frame_rows.begin(), voice.frame_rows.end());
        const int n_prompt = static_cast<int>(prompt.size() / n_embd);

        llama_memory_clear(llama_get_memory(ctx), true);
        llama_batch_ext* batch = llama_batch_ext_init(ctx);
        struct FreeBatch {
            llama_batch_ext* b;
            ~FreeBatch() { llama_batch_ext_free(b); }
        } free_batch{batch};

        // The prompt, all but its last row; that row goes alone with the reading on, since
        // its attention is the first frame's.
        reading = Reading{};
        reading.text_first = text_first;
        reading.text_count = static_cast<int>(text_tokens.size());
        const bool read_words = reads && (!heads.empty() || !dump_prefix.empty());
        struct ReadingOff {
            Reading& r;
            ~ReadingOff() { r.on = false; }
        } reading_off{reading};
        // Where the time goes, for QWEN_GGUF_VERBOSE: the prompt, then per frame the talker's
        // step, the code predictor's fifteen and code2wav's windows.
        const bool stages = std::getenv("QWEN_GGUF_VERBOSE") != nullptr;
        double t_prompt = 0, t_talker = 0, t_cp = 0, t_wav = 0;
        auto lap = [&](std::chrono::steady_clock::time_point& from) {
            const auto now = std::chrono::steady_clock::now();
            const double d = std::chrono::duration<double>(now - from).count();
            from = now;
            return d;
        };
        auto mark = std::chrono::steady_clock::now();
        const int n_batch = 256;
        int pos = 0;
        while (pos < n_prompt - 1) {
            const int n = std::min(n_batch, n_prompt - 1 - pos);
            if (!decode_rows(batch, prompt.data() + static_cast<std::size_t>(pos) * n_embd, n, pos)) {
                std::cerr << "voice: the talker refused the prompt\n";
                return {};
            }
            pos += n;
        }
        reading.on = read_words;
        if (!decode_rows(batch, prompt.data() + static_cast<std::size_t>(pos) * n_embd, 1, pos)) {
            std::cerr << "voice: the talker refused the prompt\n";
            return {};
        }
        ++pos;
        ++reading.rows;
        if (stages) t_prompt += lap(mark);

        Line line;
        line.seed = static_cast<uint32_t>(env_int("QWEN_SEED", 42));
        Sampler sampler(line.seed);
        std::vector<int> previous;
        std::vector<float> logits(static_cast<std::size_t>(llama_vocab_n_tokens(vocab) - text_vocab));
        std::vector<float> next(static_cast<std::size_t>(n_embd));
        const int max_frames = env_int("QWEN_GGUF_MAX_FRAMES", 1024);
        const int text_count = static_cast<int>(text_tokens.size());
        auto deliver = [&]() -> bool {
            if (line.pcm.size() <= line.delivered) return true;
            if (first_audio_s == 0) {
                first_audio_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            }
            if (!on_audio) {
                line.delivered = line.pcm.size();
                return true;
            }
            std::vector<std::int16_t> pcm(line.pcm.size() - line.delivered);
            to_pcm16(line.pcm.data() + line.delivered, pcm.size(), pcm.data());
            line.delivered = line.pcm.size();
            return (*on_audio)(pcm.data(), pcm.size(), sample_rate);
        };
        // The words placed for good: the path's tail is pinned to the latest frame, so the
        // words there shift as more frames come, but a word the path left three or more
        // frames ago never moved again (scripts/attn_heads.py, settle); with the four-frame
        // lead that is every word whose end is within the audio made. Only words whose
        // audio has gone out are given, so the listener never hears of a word before its
        // sound.
        auto settled_words = [&]() -> std::vector<Word> {
            const int frames = static_cast<int>(previous.size());
            std::vector<Word> all = words_so_far(text_tokens, frames);
            const double sure = std::min(line.delivered / static_cast<double>(sample_rate), frames * kFrameSeconds);
            // (>=: the word still being said ends, clamped, exactly at the audio made.)
            while (!all.empty() && all.back().end >= sure) all.pop_back();
            return all;
        };

        for (int frame = 0; frame < max_frames; ++frame) {
            if (cancelled && (*cancelled)()) {
                cut_off = true;
                break;
            }
            const float* all = llama_get_logits_ith(ctx, -1);
            const float* h_state = llama_get_embeddings_ith(ctx, -1);
            if (!all || !h_state) {
                std::cerr << "voice: the talker gave no output\n";
                return {};
            }
            std::memcpy(logits.data(), all + text_vocab, logits.size() * sizeof(float));
            const int code0 = sampler.sample(logits, previous, frame, text_count);
            if (code0 == kCodecEos) break;
            previous.push_back(code0);

            mtmd_gen_inp inp = mtmd_gen_inp_default(mctx);
            inp.type = MTMD_GEN_PROCESS_TYPE_GEN_CODE;
            inp.code0 = code0;
            inp.embd = const_cast<float*>(h_state);
            inp.top_k = sampler.top_k;
            inp.top_p = sampler.top_p;
            inp.seed = line.seed;
            mtmd_gen_out out{};
            if (stages) t_talker += lap(mark);
            if (mtmd_gen_audio_process(mctx, &inp, &out) != 0) {
                std::cerr << "voice: the code predictor failed at frame " << frame << "\n";
                return {};
            }
            if (stages) t_cp += lap(mark);
            line.codes.insert(line.codes.end(), out.codes, out.codes + out.n_codes);
            if (!flush_wav(line, false)) {
                std::cerr << "voice: code2wav failed at frame " << frame << "\n";
                return {};
            }
            if (stages) t_wav += lap(mark);
            const std::size_t delivered_before = line.delivered;
            if (!deliver()) {
                cut_off = true;
                break;
            }
            if (on_words && read_words && !heads.empty() && line.delivered != delivered_before) {
                (*on_words)(settled_words(), false);
            }
            for (int i = 0; i < n_embd; ++i) next[i] = out.embd[i] + pad[i];
            if (!decode_rows(batch, next.data(), 1, pos)) {
                std::cerr << "voice: the talker failed at frame " << frame << "\n";
                return {};
            }
            ++pos;
            ++reading.rows;
        }
        reading.on = false;
        const int frames = static_cast<int>(previous.size());
        if (!cut_off) {
            if (!flush_wav(line, true)) {
                std::cerr << "voice: code2wav failed on the last window\n";
                return {};
            }
            deliver();
        }
        if (stages) {
            t_wav += lap(mark);
            std::cerr << "voice: stages over " << frames << " frames: prompt " << std::fixed << std::setprecision(3)
                      << t_prompt << "s, talker " << t_talker << "s, code predictor " << t_cp << "s, code2wav "
                      << t_wav << "s\n";
        }
        if (read_words) {
            if (!heads.empty()) {
                words = words_so_far(text_tokens, frames);
                if (on_words) (*on_words)(words, true);
            }
            write_dump(text_tokens, frames, n_prompt);
            reading = Reading{};
        }
        return std::move(line.pcm);
    }
};

LlamaClone::LlamaClone(std::filesystem::path model_dir, std::filesystem::path voices_dir)
    : impl_(std::make_unique<Impl>(std::move(model_dir), std::move(voices_dir))) {}

LlamaClone::~LlamaClone() = default;

std::filesystem::path LlamaClone::find_model() {
    if (const char* env = std::getenv("QWEN_GGUF_DIR"); env && *env) return std::filesystem::path(env);
    std::error_code error;
    std::filesystem::path dir = std::filesystem::current_path(error);
    for (int i = 0; i < 4 && !dir.empty(); ++i) {
        const std::filesystem::path candidate = dir / "models" / "qwen3-tts-0.6b-gguf";
        if (!Impl::talker_file(candidate).empty() && !Impl::mmproj_file(candidate).empty()) return candidate;
        const std::filesystem::path parent = dir.parent_path();
        if (parent == dir) break;
        dir = parent;
    }
    return {};
}

bool LlamaClone::gpu_present() {
    quiet_logs();
    ggml_backend_load_all();
    for (std::size_t i = 0; i < ggml_backend_dev_count(); ++i) {
        if (ggml_backend_dev_type(ggml_backend_dev_get(i)) == GGML_BACKEND_DEVICE_TYPE_GPU) return true;
    }
    return false;
}

bool LlamaClone::ready() {
    std::unique_lock<std::mutex> held(impl_->mutex);
    impl_->loaded_cv.wait(held, [&] { return impl_->loaded; });
    return impl_->ctx != nullptr;
}

const std::string& LlamaClone::failure() const { return impl_->failure; }

LlamaClone::Audio LlamaClone::speak(std::string_view voice, std::string_view text) {
    Audio audio;
    std::unique_lock<std::mutex> held(impl_->mutex);
    impl_->loaded_cv.wait(held, [&] { return impl_->loaded; });
    if (!impl_->ctx) return audio;
    Impl::Voice* clone = impl_->voice_named(voice);
    if (!clone) return audio;
    const auto started = std::chrono::steady_clock::now();
    bool cut_off = false;
    double first = 0;
    const std::vector<float> pcm =
        impl_->generate(*clone, std::string(text), nullptr, nullptr, nullptr, audio.words, cut_off, first);
    audio.rate = impl_->sample_rate;
    audio.samples.resize(pcm.size());
    to_pcm16(pcm.data(), pcm.size(), audio.samples.data());
    const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    std::cerr << "voice: " << voice << " " << (pcm.size() / static_cast<double>(audio.rate)) << "s in " << took
              << "s (gpu)\n";
    if (std::getenv("QWEN_GGUF_VERBOSE") && !audio.words.empty()) {
        std::cerr << "voice: words:";
        for (const Word& w : audio.words) {
            std::cerr << " " << w.text << "@" << std::fixed << std::setprecision(2) << w.start << "-" << w.end;
        }
        std::cerr << "\n";
    }
    return audio;
}

std::size_t LlamaClone::speak(std::string_view voice, std::string_view text, const OnAudio& on_audio,
                              const Cancelled& cancelled, const OnWords& on_words) {
    std::unique_lock<std::mutex> held(impl_->mutex);
    impl_->loaded_cv.wait(held, [&] { return impl_->loaded; });
    if (!impl_->ctx) return 0;
    Impl::Voice* clone = impl_->voice_named(voice);
    if (!clone) return 0;
    if (cancelled && cancelled()) return 0;
    const auto started = std::chrono::steady_clock::now();
    bool cut_off = false;
    double first = 0;
    std::vector<Word> words;
    const std::vector<float> pcm =
        impl_->generate(*clone, std::string(text), &on_audio, cancelled ? &cancelled : nullptr,
                        on_words ? &on_words : nullptr, words, cut_off, first);
    const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    std::cerr << "voice: " << voice << " " << (pcm.size() / static_cast<double>(impl_->sample_rate))
              << "s, first audio in " << first << "s, done in " << took << "s (gpu)" << (cut_off ? ", cut off" : "")
              << "\n";
    return pcm.size();
}

std::vector<std::string> LlamaClone::voices() const {
    std::vector<std::string> names;
    std::error_code error;
    if (!std::filesystem::is_directory(impl_->voices_dir, error)) return names;
    for (const auto& entry : std::filesystem::directory_iterator(impl_->voices_dir, error)) {
        if (entry.path().extension() == ".qvoice") names.push_back(entry.path().stem().string());
    }
    return names;
}

const std::filesystem::path& LlamaClone::model_dir() const { return impl_->model_dir; }
const std::filesystem::path& LlamaClone::voices_dir() const { return impl_->voices_dir; }

}  // namespace moonshine
