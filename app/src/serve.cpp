#include "serve.h"

#include <chrono>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include "httplib.h"
#include "json.hpp"
#include "prefill.h"
#include "prompt.h"
#include "quest.h"

namespace moonshine {

namespace {

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

constexpr double kNanosPerSecond = 1e9;

double seconds_since(Clock::time_point from) {
    return std::chrono::duration<double>(Clock::now() - from).count();
}

// One NDJSON line, in the shape the playground's client already reads.
//
// That shape is Ollama's, and it is reused rather than replaced for two reasons. It is the
// natural shape for the problem -- a run of deltas followed by one object carrying the
// counts -- and the browser, the Node proxy and the stats panel all already parse it, so
// swapping the backend touches none of them. What this is NOT is an impersonation: /api/version
// says moonshine, there is no model management, and the extra field below is one Ollama has no
// way to fill in.
std::string ndjson_line(const json& chunk) { return chunk.dump() + "\n"; }

json delta_chunk(const std::string& model, std::string_view piece) {
    return {
        {"model", model},
        {"message", {{"role", "assistant"}, {"content", piece}}},
        {"done", false},
    };
}

void bad_request(httplib::Response& res, int status, const std::string& why) {
    res.status = status;
    res.set_content(json{{"error", why}}.dump(), "application/json");
}

}  // namespace

json final_chunk(const std::string& model, const Timing& timing, double whole_seconds) {
    json done = {
        {"model", model},
        {"message", {{"role", "assistant"}, {"content", ""}}},
        {"done", true},
        {"done_reason", "stop"},
        // Nanoseconds, because that is what this shape means by a duration.
        {"total_duration", static_cast<std::int64_t>(whole_seconds * kNanosPerSecond)},
        {"load_duration", 0},
        {"prompt_eval_count", timing.prompt_tokens},
        {"prompt_eval_duration",
         static_cast<std::int64_t>(timing.prefill_seconds * kNanosPerSecond)},
        {"eval_count", timing.reply_tokens},
        {"eval_duration", static_cast<std::int64_t>(timing.decode_seconds * kNanosPerSecond)},
    };
    // The field that is the whole point. Ollama reports the entire prompt as evaluated whether
    // it reused any of it or not, so a playground talking to Ollama cannot show prefix reuse
    // at all. Omitted rather than sent as 0 when the backend will not say, so that a panel can
    // tell "nothing was reused" from "nobody knows".
    if (timing.cached_tokens >= 0) done["cached_eval_count"] = timing.cached_tokens;
    return done;
}

// The messages out of a request body, or a reason the body was no good.
std::vector<Message> messages_from(const json& body) {
    if (!body.contains("messages") || !body["messages"].is_array()) {
        throw std::runtime_error("the request needs a messages array");
    }
    std::vector<Message> messages;
    for (const json& one : body["messages"]) {
        if (!one.contains("role") || !one.contains("content")) {
            throw std::runtime_error("every message needs a role and content");
        }
        messages.push_back({one["role"].get<std::string>(), one["content"].get<std::string>()});
    }
    if (messages.empty()) throw std::runtime_error("the messages array was empty");
    return messages;
}

// Sampling as the request asked for it, falling back to the app's own defaults per field.
//
// Per field rather than all or nothing, because the playground exists to vary one thing at a
// time: a request that sets only the temperature should get the app's top-p, not a second set
// of defaults written here that could drift from Options.
Options options_from(const json& body, const Options& defaults) {
    Options options = defaults;
    if (body.contains("options") && body["options"].is_object()) {
        const json& asked = body["options"];
        options.reply_tokens = asked.value("num_predict", options.reply_tokens);
        options.temperature = asked.value("temperature", options.temperature);
        options.top_p = asked.value("top_p", options.top_p);
        options.top_k = asked.value("top_k", options.top_k);
    }
    // num_ctx is not read at all: the context was sized when the model was loaded and cannot
    // be changed without dropping the cache, which is the thing being demonstrated. Left in the
    // defaults rather than quietly honoured, so a request asking for a different one gets the
    // real context and the real reuse instead of a silently ignored setting.
    return options;
}

int serve(Llm& llm, const Options& options, std::string_view world, int port) {
    httplib::Server server;

    // One at a time. The KV cache is a single sequence, so two turns in flight would each
    // evict the other's tail and answer from a cache that had moved. Held across the whole of
    // a generation rather than around the call into the model, because it is the cache that is
    // being protected and the cache is written throughout.
    std::mutex talking;

    // Browsers will not read a cross-origin stream without being told they may, and the
    // playground is served from a different port by Node.
    server.set_post_routing_handler([](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
    });
    server.Options(R"(/.*)", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Content-Type");
        res.status = 204;
    });

    // Named moonshine rather than reporting an Ollama version, so anything looking at this
    // is told what it is actually talking to.
    server.Get("/api/version", [&llm](const httplib::Request&, httplib::Response& res) {
        res.set_content(json{
                            {"version", "moonshine"},
                            {"backend", llm.describe()},
                        }
                            .dump(),
                        "application/json");
    });

    // One model, because one is loaded. Shaped like a list so the playground's model picker
    // needs no special case, and it is an honest answer to "what have you got".
    server.Get("/api/tags", [&llm, &options](const httplib::Request&, httplib::Response& res) {
        res.set_content(json{{"models", json::array({json{
                                 {"name", options.model},
                                 {"details", {{"parameter_size", nullptr}}},
                                 {"description", llm.describe()},
                             }})}}
                            .dump(),
                        "application/json");
    });

    server.Post("/api/chat", [&](const httplib::Request& req, httplib::Response& res) {
        json body = json::parse(req.body, nullptr, false);
        if (body.is_discarded() || !body.is_object()) {
            bad_request(res, 400, "the request body was not a JSON object");
            return;
        }

        std::vector<Message> messages;
        Options asked = options;
        try {
            messages = messages_from(body);
            asked = options_from(body, options);
        } catch (const std::exception& why) {
            bad_request(res, 400, why.what());
            return;
        }

        // Streaming is the default, as it is in the shape this borrows from. The playground
        // needs it, since a first-audio time cannot be taken from a reply that arrives whole.
        // The measurement scripts want the opposite: one object they can read a field off,
        // with no stream to assemble. Both are served because both already exist -- the probes
        // in scripts/ were written against Ollama, and honouring this one field is what lets
        // them point at the shipping backend instead.
        if (!body.value("stream", true)) {
            std::lock_guard<std::mutex> held(talking);
            const Clock::time_point began = Clock::now();
            std::string said;
            try {
                const Timing timing = llm.chat(messages, asked, [&](std::string_view piece) {
                    said.append(piece);
                });
                json whole = final_chunk(asked.model, timing, seconds_since(began));
                whole["message"]["content"] = said;
                res.set_content(whole.dump(), "application/json");
                std::printf("-- asked: %d tok in", timing.prompt_tokens);
                if (timing.cached_tokens >= 0) std::printf(" (%d cached)", timing.cached_tokens);
                std::printf(", %d tok out in %.2fs\n", timing.reply_tokens,
                            timing.decode_seconds);
            } catch (const std::exception& why) {
                // A status is still available here, unlike in the streaming branch, because
                // nothing has been written yet.
                bad_request(res, 500, why.what());
                std::printf("-- asked: %s\n", why.what());
            }
            std::fflush(stdout);
            return;
        }

        // The generation happens inside the content provider, so the first token reaches the
        // browser as soon as the model produces it rather than after the whole reply. That is
        // the entire reason the playground can report a first-audio time at all.
        res.set_chunked_content_provider(
            "application/x-ndjson",
            [&, messages, asked](std::size_t, httplib::DataSink& sink) {
                std::lock_guard<std::mutex> held(talking);
                const Clock::time_point began = Clock::now();
                try {
                    const Timing timing =
                        llm.chat(messages, asked, [&](std::string_view piece) {
                            // The prefill's heartbeats (llm.h) are not words.
                            if (piece.empty()) return;
                            const std::string line =
                                ndjson_line(delta_chunk(asked.model, piece));
                            sink.write(line.data(), line.size());
                        });
                    const std::string last =
                        ndjson_line(final_chunk(asked.model, timing, seconds_since(began)));
                    sink.write(last.data(), last.size());
                    std::printf("-- playground: %d tok in", timing.prompt_tokens);
                    if (timing.cached_tokens >= 0) {
                        std::printf(" (%d cached)", timing.cached_tokens);
                    }
                    std::printf(", %d tok out in %.2fs\n", timing.reply_tokens,
                                timing.decode_seconds);
                } catch (const std::exception& why) {
                    // Sent as a line rather than a status, because the headers went out with
                    // the first token and there is no status left to change. The playground's
                    // reader checks every line for this.
                    const std::string oops = ndjson_line(json{{"error", why.what()}});
                    sink.write(oops.data(), oops.size());
                    std::printf("-- playground: %s\n", why.what());
                }
                std::fflush(stdout);
                sink.done();
                return true;
            });
    });

    // The quest director's one question, asked of the same model over the same prefix. The
    // playground sends the conversation it is about to ask a reply for -- system, history,
    // and the player's words last -- with the question the persona JSON gave it, and gets the
    // verdict back as the app itself would read it, so the browser holds no second copy of
    // how a yes is recognised.
    server.Post("/api/judge", [&](const httplib::Request& req, httplib::Response& res) {
        json body = json::parse(req.body, nullptr, false);
        if (body.is_discarded() || !body.is_object()) {
            bad_request(res, 400, "the request body was not a JSON object");
            return;
        }
        std::vector<Message> messages;
        try {
            messages = messages_from(body);
        } catch (const std::exception& why) {
            bad_request(res, 400, why.what());
            return;
        }
        const std::string question = body.value("question", std::string());
        if (question.empty()) {
            bad_request(res, 400, "the request needs a question");
            return;
        }
        std::lock_guard<std::mutex> held(talking);
        const Clock::time_point began = Clock::now();
        std::string raw;
        try {
            const Timing timing = llm.chat(judge_messages(messages, question),
                                           judge_options(options),
                                           [&](std::string_view piece) { raw.append(piece); });
            const Verdict verdict = read_verdict(raw);
            const char* word = verdict == Verdict::Yes ? "yes"
                               : verdict == Verdict::No ? "no"
                                                        : "unclear";
            res.set_content(json{{"verdict", word},
                                 {"raw", raw},
                                 {"prompt_eval_count", timing.prompt_tokens},
                                 {"cached_eval_count", timing.cached_tokens},
                                 {"seconds", seconds_since(began)}}
                                .dump(),
                            "application/json");
            std::printf("-- judged: %s (\"%s\"), %d tok in", word, raw.c_str(),
                        timing.prompt_tokens);
            if (timing.cached_tokens >= 0) std::printf(" (%d cached)", timing.cached_tokens);
            std::printf(", %.2fs\n", seconds_since(began));
        } catch (const std::exception& why) {
            bad_request(res, 500, why.what());
            std::printf("-- judged: %s\n", why.what());
        }
        std::fflush(stdout);
    });

    // Warmed before the port opens, so the first turn the playground asks for reuses the
    // prefix exactly as the first conversation in the game does. Without this the playground
    // would show a cold opening turn and a warm second one, and report the first as the cost.
    Prefiller prefiller(llm, options);
    const Warmth warmth = prefiller.prove_world(world);
    const double rate = warmth.prefill_rate();
    if (rate > 0.0) {
        std::printf("world:      %d tok cached in %.2fs, at %.0f tok/s\n",
                    warmth.warm.prompt_tokens, warmth.warm.prefill_seconds, rate);
    }
    std::printf("serving:    http://127.0.0.1:%d -- point the playground's back end here\n",
                port);
    std::printf("            one turn at a time, sharing the cache the game would use\n");
    std::fflush(stdout);

    if (!server.listen("127.0.0.1", port)) {
        std::fprintf(stderr, "moonshine_app: could not listen on port %d\n", port);
        return 1;
    }
    return 0;
}

}  // namespace moonshine
