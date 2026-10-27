// The development backend: Ollama over HTTP on localhost.
//
// Every measurement in benchmarks/README.md was taken through this, which is the
// reason it exists rather than going straight to llama.cpp. It is not what ships:
// Ollama is a separate install that is not ours to redistribute, and the research
// notes put it 10-14% behind llama.cpp with fewer of the knobs that matter here.
// The interface in llm.h is the seam where the shipping backend replaces it.

#ifndef MOONSHINE_OLLAMA_H
#define MOONSHINE_OLLAMA_H

#include <memory>
#include <string>
#include <vector>

#include "llm.h"

namespace moonshine {

class OllamaLlm : public Llm {
public:
    explicit OllamaLlm(std::string host = "http://127.0.0.1:11434");
    ~OllamaLlm() override;

    Timing chat(const std::vector<Message>& messages,
                const Options& options,
                const OnDelta& on_delta) override;

    std::string describe() const override;

    // Whether the server is up, and which models it has. Used by the status UI,
    // and worth distinguishing from a failed turn: a missing Ollama is something
    // the player can fix, a failed turn usually is not.
    bool reachable();
    std::vector<std::string> models();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace moonshine

#endif  // MOONSHINE_OLLAMA_H
