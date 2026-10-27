// A model that answers from a script.
//
// Everything worth testing around a model is about what is sent and what is done
// with what comes back, neither of which needs a model. It also records the calls,
// because the prefill logic is only correct if it asks for the right prompt with
// the right budget, and that is not visible in a reply.

#ifndef MOONSHINE_TESTS_STUB_LLM_H
#define MOONSHINE_TESTS_STUB_LLM_H

#include <deque>
#include <string>
#include <vector>

#include "llm.h"

namespace moonshine {

class StubLlm : public Llm {
public:
    struct Call {
        std::vector<Message> messages;
        Options options;
    };

    // Handed out in order, one per chat(). An exhausted queue answers with
    // nothing, which is what a caller should cope with anyway.
    std::deque<std::string> replies;
    std::vector<Call> calls;
    // Split into pieces of this many characters, to stand in for token streaming.
    std::size_t chunk = 3;
    Timing next_timing;
    // Taken in order when it has anything in it, falling back to next_timing. Needed
    // because some answers are about the difference between two calls -- a cold prefill
    // and the cache hit that follows it -- which one timing cannot express.
    std::deque<Timing> timings;

    Timing chat(const std::vector<Message>& messages,
                const Options& options,
                const OnDelta& on_delta) override {
        calls.push_back({messages, options});
        if (!replies.empty()) {
            const std::string reply = replies.front();
            replies.pop_front();
            for (std::size_t at = 0; at < reply.size(); at += chunk) {
                on_delta(std::string_view(reply).substr(at, chunk));
            }
        }
        return timed();
    }

    std::string describe() const override { return "stub"; }

    const Call& last() const { return calls.back(); }

private:
    Timing timed() {
        if (timings.empty()) return next_timing;
        const Timing now = timings.front();
        timings.pop_front();
        return now;
    }

public:

    static const Message& system_of(const Call& call) { return call.messages.front(); }
};

}  // namespace moonshine

#endif  // MOONSHINE_TESTS_STUB_LLM_H
