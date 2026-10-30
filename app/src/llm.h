// A model, behind the smallest surface a conversation needs.
//
// Two real backends are expected. Ollama is what development uses, because it is
// already on the machine and exposes the timings these decisions were made from.
// llama.cpp is what ships, because Ollama is not ours to redistribute. Tests use
// a third that answers from a script, since none of the logic around a model
// needs a model in order to be exercised.

#ifndef MOONSHINE_LLM_H
#define MOONSHINE_LLM_H

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace moonshine {

struct Message {
    std::string role;
    std::string content;
};

struct Options {
    // The better writer, and NOT the shipping choice. Measured against
    // qwen3:4b-instruct on the same prompt in the same session: 34 median words
    // against 54, and it never broke the two-or-three-sentence rule where the
    // other broke it in 6 of 16 replies. On the Mac it scored 0 of 16 on every
    // fault in scripts/probe_output_shape.py, the only model measured to do so.
    //
    // It costs latency on the machine that matters. On the Windows laptop it decodes at 8.1 tok/s
    // against 22.2, so first audio is 2.91s against 0.81s and playback ran dry in 4
    // of 15 turns against 1 -- see the gemma4-12b benchmark in moonshine-voice-characters. The Mac runs the
    // same weights at 23.6 tok/s on unified memory, which is why it is pleasant here
    // and unusable there, and why the launcher still names qwen explicitly.
    // Two other things have to be settled before this could ship: the licence is
    // Gemma's rather than Apache-2.0, which an installer has to redistribute under,
    // and llama.cpp does not recognise its chat template, so it depends on
    // gemma_turns() in llamacpp.h rather than on upstream.
    std::string model = "gemma4:12b";
    int context_tokens = 4096;
    int reply_tokens = 512;

    // Loose on purpose. qwen3:4b-instruct ships recommending temperature 0.7 and
    // top-p 0.8, and at those settings a blind comparison read its dialogue as
    // flat and clipped -- "No delays. No excuses." Run at the settings Qwen3.5
    // recommends instead, the same model gained imagery and most of the gap to the
    // better-writing model closed. See benchmarks/blind-loose.md.
    double temperature = 1.0;
    double top_p = 0.95;
    int top_k = 20;

    // Sent rather than left out. Qwen3.5 reasons by default and honours this, but
    // a model that reasons regardless is made worse by it: the switch stops the
    // backend separating the reasoning into its own field, so it arrives in the
    // text meant to be spoken. Whether a model can be quietened is a property of
    // the build, checked by scripts/probe_thinking.py before it is chosen.
    bool disable_thinking = true;

    // No layers on the GPU. The game owns the GPU, and the whole point of
    // choosing a 4B was that the CPU could carry it.
    bool cpu_only = true;

    // Whether llama.cpp may rearrange the weights for the CPU's matrix kernels as it loads
    // them. Off, because the rearranged copy is a second copy of the model: the file stays
    // mapped (the tensors that are not rearranged are read from it) while the rearranged
    // ones are written into memory of the process's own. Measured on the shipping
    // Qwen3-4B Q4_K_M, 29 Oct 2026, on the Mac: with it the process holds 3271 MB of its
    // own beside the 2390 MB mapping; without, 1055 MB beside 2403, and the mapping is
    // the kind of memory the OS takes back under pressure. The cost there was the second
    // decimal place: decode 52-53 tok/s against 55-56, the soul's prefill 1.07 s against
    // 0.75, the world's 3.66 s against 4.86. On the Windows laptop (AVX2, whose Q4_K
    // kernel differs; 30 Oct 2026, release build, no game) there was no cost at all:
    // 14-15 tok/s either way, the world prefilled in 10.4 s against 11.3 with the copy,
    // the soul in 1.8 against 1.9; private bytes 1,188 MB against 2,875 after two turns,
    // working set 3.2 GB against 4.9. The game runs beside this on 32 GB, and was at 34 GB
    // committed when its GPU driver gave up on 29 Oct 2026. --repack turns it back on.
    //
    // Not something a prepared file could give back: the rearranged layout is the CPU
    // backend's own, picked by the CPU's features at load (8x8 blocks on AVX2, 8x4 on
    // Arm NEON) and never a GGUF type -- llama.cpp dropped the on-disk Q4_0_4_4/4_8/8_8
    // kinds for this very rearranging at load (Dec 2024). And on the machine that ships
    // it there is nothing to give back.
    bool repack_weights = false;

    // How long the backend should keep the model and its caches resident. This is
    // not a tuning knob but the thing the whole prefill strategy rests on: once
    // this expires the world prefix is gone and the next conversation pays 6s of
    // prefill again. Longer than any plausible gap between conversations.
    std::string keep_alive = "60m";
};

// What a turn cost. Worth carrying around rather than logging and forgetting,
// because prefill time is the thing that silently regresses: a backend that has
// stopped reusing the cached prefix returns the same words, correctly, ten times
// slower, and nothing else reports it.
struct Timing {
    int prompt_tokens = 0;
    // How many of those the backend already had, and so did not evaluate.
    //
    // -1 when the backend will not say, which is not the same as none. Ollama reports the
    // whole prompt as evaluated whether it reused any of it or not, so there this has to be
    // estimated from the elapsed time against a measured rate -- see tokens_already_cached,
    // and the nonce-filler measurement that exists only because of this gap. llama.cpp, which
    // is in this process and whose cache is ours, simply knows.
    int cached_tokens = -1;
    double prefill_seconds = 0.0;
    int reply_tokens = 0;
    double decode_seconds = 0.0;
    // How long the model stood aside during this call (Llm::stand_aside_while), which is
    // taken out of the two figures above so that they stay rates of the model's own work.
    double held_seconds = 0.0;
    // Set by the session when the reply was generated before the player picked it
    // (Session::speculate), so the times above were paid while they were reading the
    // list, not after they chose.
    bool guessed = false;
};

// Called for each piece of the reply as it arrives. A backend that prefills in this process
// also calls it with an EMPTY piece now and then while the prompt is still being read in,
// so that a caller who wants to stop early -- a guess made ahead of need, session.h -- can
// throw out of it before the reply has begun rather than only once the first token is out.
// Nothing need be done with an empty piece.
using OnDelta = std::function<void(std::string_view)>;

class Llm {
public:
    virtual ~Llm() = default;

    // Streams a reply, calling on_delta for every piece. Throws on transport or
    // protocol failure, since a conversation that cannot reach the model has
    // nothing useful to do with a return code.
    virtual Timing chat(const std::vector<Message>& messages,
                        const Options& options,
                        const OnDelta& on_delta) = 0;

    // A name for the status UI.
    virtual std::string describe() const = 0;

    // Prefills exactly the text every prompt begins with -- the system message up to where a
    // soul's sheet would start -- and keeps a copy of the state at that point, to return to
    // when a later prompt starts the same way but the cache cannot be cut back to it. A no-op
    // returning an empty Timing for a backend that cannot or need not: an HTTP backend keeps
    // its own cache, and a full-attention model can cut its cache anywhere. The reason it
    // exists is measured in data/kcd2/state-design/plan.md section 7: a hybrid model's state
    // is a fold over the sequence and cannot be cut, so without this every soul was a cold
    // prefill of the world as well as the sheet.
    virtual Timing hold_branch(std::string_view /*system*/) { return {}; }

    // How many tokens the backend holds in its cache right now, for a status line saying
    // how far a guess got before it was stopped. Zero for a backend that cannot say (an
    // HTTP one keeps its cache to itself), which a caller reads as "unknown", not "none".
    virtual std::size_t held_tokens() const { return 0; }

    // Something the model stands aside for while it answers true. Asked between the pieces
    // of a prefill and between the tokens of a reply, after on_delta has had the piece; while
    // it holds, the next is not asked for, and the wait is reported in Timing::held_seconds.
    // What it is for: the voice. The model and the speech engine run on the same cores
    // (main.cpp), and measured beside each other on the Windows laptop (11 Oct 2026) the model at sixteen
    // threads left speech at half of real time, so a line being spoken comes first and the
    // model takes it up again when the line is done. A guess made ahead of need stops
    // altogether for the same reason (Session::speculate); this is for the calls that
    // cannot stop -- the reply itself, the judge, the folding of a memory -- which pause.
    // A backend that cannot pause (an HTTP one, whose server runs on regardless) ignores it.
    virtual void stand_aside_while(std::function<bool()> /*held*/) {}
};

}  // namespace moonshine

#endif  // MOONSHINE_LLM_H
