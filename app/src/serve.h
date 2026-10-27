// The app's model, offered over HTTP so the Character Playground can use the real one.
//
// The playground already gets everything that SHAPES a reply from the app -- the system
// prompt, the sampling, the greetings, the speech buffer, all out of `--soul --json`. What it
// did not get was the reply itself: that went to Ollama from the browser. So the words were
// near enough right and two things were not. The latency it reported was Ollama's through an
// extra hop, and the KV cache it exercised was Ollama's, which means the part of the shipping
// path with the actual trap in it -- prefix reuse and eviction -- was the one part the
// playground could not see.
//
// This closes that. `--serve PORT` hands out the same llama.cpp context the game talks to,
// so a turn in the playground reuses the same prefix, reports the same cached-token count,
// and is timed on the same clock.
//
// Deliberately NOT served alongside the game loop. There is one llama_context holding one
// sequence, so a playground turn arriving during a real conversation would evict the
// conversation's tail and answer from a cache that had just been rearranged underneath it.
// One mode or the other, and one request at a time within this one.

#ifndef MOONSHINE_SERVE_H
#define MOONSHINE_SERVE_H

#include <string>
#include <string_view>
#include <vector>

#include "json.hpp"
#include "llm.h"

namespace moonshine {

// Runs until the process is stopped. Returns 0 for a clean stop and 1 if the port could not
// be taken, which is the only failure worth a different exit code: everything else is a bad
// request and is answered as one.
int serve(Llm& llm, const Options& options, std::string_view world, int port);

// The three decisions a request turns on, out here so they can be tested without a socket or a
// model. Everything else in serve.cpp is httplib wiring.

// The messages out of a request body. Throws std::runtime_error naming what was wrong with it,
// which is the text the caller gets back as a 400.
std::vector<Message> messages_from(const nlohmann::json& body);

// Sampling as the request asked for it, falling back to the app's own defaults per field.
Options options_from(const nlohmann::json& body, const Options& defaults);

// The last NDJSON object of a reply, carrying the counts.
nlohmann::json final_chunk(const std::string& model, const Timing& timing,
                           double whole_seconds);

}  // namespace moonshine

#endif  // MOONSHINE_SERVE_H
