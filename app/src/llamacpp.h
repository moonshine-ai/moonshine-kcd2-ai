// The shipping backend: llama.cpp, in this process, with the KV cache under our own hand.
//
// Ollama got the project started and is still the easier thing to develop against, but it
// cannot ship. It is a separate install that is not ours to redistribute, and the cache the
// whole latency argument rests on lives inside it: measured, `ollama stop` -- which is what
// memory pressure from the game amounts to -- threw away 647 of the world prefix's 848
// tokens, and nothing told the app it had happened. The answer there was to ask for the
// prefix again every two minutes and hope. Here there is nothing to hope about. The cache is
// a member of this object, it is emptied only when this code empties it, and what it holds is
// known exactly rather than inferred from how long a prefill took.
//
// Being in-process rather than talking to a bundled llama-server is the same argument twice:
// one thing to install, and one owner of the cache. A server would have kept the existing
// HTTP client and cost nothing to write, but it would have put the cache back behind a socket
// and left a child process to launch, watch and kill.

#ifndef MOONSHINE_LLAMACPP_H
#define MOONSHINE_LLAMACPP_H

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "llm.h"

namespace moonshine {

// How many tokens at the front of a new prompt the cache can keep, given what it holds.
//
// The whole of cache reuse is this number. Everything from here on has to be evaluated, and
// everything before it is already paid for -- which with a shared world prefix first is most
// of the prompt: 929 of 981 tokens on the opening turn.
//
// At most one token short of the new prompt, and that is not an off-by-one. Decoding is what
// produces the logits the first word is sampled from, so a prompt with nothing left to decode
// has nothing to speak from. Keeping the lot is the obvious implementation and it either
// samples from a stale distribution or asserts inside llama.cpp, depending on the version.
//
// Inline and typed on int32_t rather than llama_token so that the one piece of this file
// worth testing can be tested without a model, without llama.h, and without the submodule.
inline std::size_t reusable_prefix(const std::vector<std::int32_t>& held,
                                   const std::vector<std::int32_t>& next) {
    if (next.empty()) return 0;
    const std::size_t most = next.size() - 1;
    std::size_t same = 0;
    while (same < most && same < held.size() && held[same] == next[same]) ++same;
    return same;
}

// Whether a held branch can stand in for the front of a prompt: every token of the branch is
// the same token of the prompt, and the prompt goes on past it, for the same reason
// reusable_prefix stops one short -- something has to be decoded to have logits to sample.
inline bool branch_covers(const std::vector<std::int32_t>& branch,
                          const std::vector<std::int32_t>& next) {
    if (branch.empty() || next.size() <= branch.size()) return false;
    for (std::size_t i = 0; i < branch.size(); ++i) {
        if (branch[i] != next[i]) return false;
    }
    return true;
}

// Whether a chat template is Gemma 4's, which llama.cpp does not recognise.
//
// `llama_chat_apply_template` runs no Jinja whatever the GGUF holds -- it substring-matches
// against a fixed list and formats with hand-written C++ -- and Gemma 4 renamed the markers
// Gemma 3 used, `<start_of_turn>`/`<end_of_turn>` becoming `<|turn>`/`<turn|>`. The old
// spelling is the one llama.cpp looks for, so the call fails and there is nothing to send.
//
// Being Jinja was never the problem. qwen3:4b-instruct's own template is 4,040 bytes of it
// and is recognised, because it contains `<|im_start|>`. So what is missing is one format,
// not an engine, and scripts/probe_gguf_template.py reads the marker out of any GGUF header
// to say which case a candidate model falls into before it is downloaded.
inline bool uses_gemma_turns(const std::string& tmpl) {
    return tmpl.find("<|turn>") != std::string::npos;
}

// Gemma 4's prompt format, pinned against its real template rendered through Jinja rather
// than guessed -- see the cases in test_llamacpp.cpp, which are that output byte for byte.
//
// No BOS token: the caller tokenises with `add_special`, so llama.cpp prepends it. Emitting
// one here would double it, which costs quality quietly and shows up as nothing at all.
inline std::string gemma_turns(const std::vector<Message>& messages) {
    std::string out;
    for (const Message& message : messages) {
        // Gemma calls the assistant "model". Unlike Gemma 3 it does have a system turn, which
        // is where the shared world prefix goes, so nothing has to be merged into the user's.
        out += "<|turn>";
        out += message.role == "assistant" ? "model" : message.role;
        out += '\n';
        const std::size_t first = message.content.find_first_not_of(" \t\n\r");
        if (first != std::string::npos) {
            const std::size_t last = message.content.find_last_not_of(" \t\n\r");
            out += message.content.substr(first, last - first + 1);
        }
        out += "<turn|>\n";
    }
    // The generation prompt, ending in a thought channel that is empty and already closed.
    // That is how Gemma 4 is asked not to reason: its own template emits exactly this when
    // `enable_thinking` is false, which is the default it ships with, and it is the same trick
    // granite-4.2-8b uses. Being a fixed string, it needs no variable and so no Jinja engine --
    // which is what separates this model from the two that do need one.
    out += "<|turn>model\n<|channel>thought\n<channel|>";
    return out;
}

#ifdef MOONSHINE_HAS_LLAMA

class LlamaLlm : public Llm {
public:
    // Loads the weights, which takes seconds and megabytes, so it happens once here rather
    // than per turn. Throws if the file is not a model this build can read.
    LlamaLlm(std::filesystem::path gguf, const Options& options);
    ~LlamaLlm() override;

    LlamaLlm(const LlamaLlm&) = delete;
    LlamaLlm& operator=(const LlamaLlm&) = delete;

    Timing chat(const std::vector<Message>& messages,
                const Options& options,
                const OnDelta& on_delta) override;

    std::string describe() const override;

    // Decodes the system text up to where it would diverge from any prompt built on it, and
    // copies the sequence's state there, so a hybrid model, whose cache refuses to be cut back
    // to the prefix, can be put back to it in a millisecond instead of prefilling the world
    // again for every soul. Measured for Qwen3.5-4B: 77 MB, 1 ms either way, against 3.7 s of
    // prefill saved on every conversation.
    Timing hold_branch(std::string_view system) override;

    // How many tokens the cache is holding. Reported because it is the thing that used to be
    // unknowable: with Ollama the only way to ask was to time a prefill and do arithmetic.
    std::size_t held_tokens() const override;
    void stand_aside_while(std::function<bool()> held) override;
    // How many tokens the held branch covers, nought when none is held.
    std::size_t branch_tokens() const;

    // Where a model can be found without one having been downloaded for it.
    //
    // Ollama stores models as GGUF -- the blob for qwen3:4b-instruct opens with the GGUF
    // magic -- so a machine with Ollama already has a file this backend can read, which is
    // both development machines. Shipping means fetching one instead, and that belongs with
    // the installer, but it should not block getting this working.
    static std::filesystem::path find_ollama_blob(const std::string& model);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

#endif  // MOONSHINE_HAS_LLAMA

}  // namespace moonshine

#endif  // MOONSHINE_LLAMACPP_H
