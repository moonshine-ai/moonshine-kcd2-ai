// Turning a line into a vector, so what the player said can be matched against what a soul
// knows (knowledge.h).
//
// The vectors in npcs-vectors.sqlite were written by EmbeddingGemma through
// sentence-transformers: each text under the document prompt, mean-pooled, through the
// two dense layers, and L2-normalised (data/kcd2/knowledge-design/plan.md section 6a), and
// stored as bytes with a scale (data/schema.sql). A query has to go through the same model
// under the query prompt or the cosine means nothing, so this is a second, small llama.cpp
// context beside the one that talks -- 300M parameters, 334 MB at Q8_0, 30 ms a line on
// the CPU -- loaded from a GGUF converted with the dense modules kept
// (`convert_hf_to_gguf.py --sentence-transformers-dense-modules`; the public GGUFs leave
// them out, and without them the vectors do not agree with the stored ones).

#ifndef MOONSHINE_EMBEDDER_H
#define MOONSHINE_EMBEDDER_H

#include <cmath>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace moonshine {

// The two prompts sentence-transformers puts in front of a text for this model, read out of
// its config_sentence_transformers.json. The stored vectors carry the document one; a line
// of the player's takes the query one. `include_prompt` is true for this model, so the
// prompt's tokens are pooled with the text's, which is why it has to be the same string.
inline constexpr std::string_view kQueryPrompt = "task: search result | query: ";
inline constexpr std::string_view kDocumentPrompt = "title: none | text: ";

class Embedder {
public:
    virtual ~Embedder() = default;
    // A unit vector for the text, under the query prompt or the document one.
    virtual std::vector<float> embed(std::string_view text, bool query) = 0;
    virtual int dim() const = 0;
    virtual std::string describe() const = 0;
};

// Cosine of two unit vectors, which is their dot product. Zero when the lengths differ,
// which is a vector from another model and not a near miss.
inline float cosine(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size()) return 0.0f;
    float dot = 0.0f;
    for (std::size_t i = 0; i < a.size(); ++i) dot += a[i] * b[i];
    return dot;
}

// The first `dim` components, made a unit vector again. EmbeddingGemma is trained so that
// a prefix of its embedding is itself an embedding (Matryoshka representation learning:
// 768, 512, 256 or 128 wide), so a query is brought to meet stored vectors cut shorter
// than it (the shipped ones are the whole 768; 256 was tried and ranked worse). A vector
// no longer than `dim` comes back as it is.
inline std::vector<float> truncated(const std::vector<float>& v, std::size_t dim) {
    if (v.size() <= dim) return v;
    std::vector<float> out(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(dim));
    float sum = 0.0f;
    for (const float x : out) sum += x * x;
    if (sum > 0.0f) {
        const float inv = 1.0f / std::sqrt(sum);
        for (float& x : out) x *= inv;
    }
    return out;
}

#ifdef MOONSHINE_HAS_LLAMA

class LlamaEmbedder : public Embedder {
public:
    // Loads the weights. Throws when the file is not there or is not an embedding model this
    // build can pool.
    explicit LlamaEmbedder(std::filesystem::path gguf);
    ~LlamaEmbedder() override;

    LlamaEmbedder(const LlamaEmbedder&) = delete;
    LlamaEmbedder& operator=(const LlamaEmbedder&) = delete;

    std::vector<float> embed(std::string_view text, bool query) override;
    int dim() const override;
    std::string describe() const override;

    // Where the weights are without being told: beside the app, or in the models folder of
    // the checkout the app was built in. Empty when neither has them.
    static std::filesystem::path find_weights();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

#endif  // MOONSHINE_HAS_LLAMA

}  // namespace moonshine

#endif  // MOONSHINE_EMBEDDER_H
