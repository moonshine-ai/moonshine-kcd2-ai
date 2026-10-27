#include "ollama.h"

#include <stdexcept>

#include "httplib.h"
#include "json.hpp"

namespace moonshine {

namespace {

using json = nlohmann::json;

// Ollama reports every duration in nanoseconds.
constexpr double kNanosPerSecond = 1e9;

json body_for(const std::vector<Message>& messages, const Options& o) {
    json opts = {
        {"num_ctx", o.context_tokens},
        {"num_predict", o.reply_tokens},
        {"temperature", o.temperature},
        {"top_p", o.top_p},
        {"top_k", o.top_k},
    };
    if (o.cpu_only) opts["num_gpu"] = 0;

    json msgs = json::array();
    for (const Message& m : messages) {
        msgs.push_back({{"role", m.role}, {"content", m.content}});
    }

    json body = {
        {"model", o.model},
        {"messages", std::move(msgs)},
        {"stream", true},
        {"options", std::move(opts)},
        {"keep_alive", o.keep_alive},
    };
    // Only sent when asked for. See the note in llm.h: for a model that reasons
    // regardless, sending this is worse than leaving it out.
    if (o.disable_thinking) body["think"] = false;
    return body;
}

// One NDJSON line. Returns true when it was the final line, which is the one
// carrying the timings.
bool consume_line(const std::string& line, const OnDelta& on_delta, Timing* timing) {
    if (line.empty()) return false;

    json chunk = json::parse(line, nullptr, false);
    if (chunk.is_discarded()) {
        throw std::runtime_error("Ollama sent a line that was not JSON: " + line.substr(0, 120));
    }
    if (chunk.contains("error")) {
        throw std::runtime_error("Ollama: " + chunk["error"].get<std::string>());
    }

    if (chunk.contains("message")) {
        const json& message = chunk["message"];
        // Reasoning arrives here when the backend has parsed it out, and is
        // deliberately dropped: it is not speech, and the player must not hear it.
        if (message.contains("content")) {
            const std::string piece = message["content"].get<std::string>();
            if (!piece.empty()) on_delta(piece);
        }
    }

    if (!chunk.value("done", false)) return false;

    timing->prompt_tokens = chunk.value("prompt_eval_count", 0);
    timing->prefill_seconds = chunk.value("prompt_eval_duration", 0.0) / kNanosPerSecond;
    timing->reply_tokens = chunk.value("eval_count", 0);
    timing->decode_seconds = chunk.value("eval_duration", 0.0) / kNanosPerSecond;
    return true;
}

}  // namespace

struct OllamaLlm::Impl {
    std::string host;
    httplib::Client client;

    explicit Impl(std::string h) : host(std::move(h)), client(host) {
        // Prefill of a cold prompt was measured at over 20s on a 9B, and a long
        // conversation turn on a contended CPU is not quick either, so the default
        // read timeout would abandon perfectly healthy work.
        client.set_read_timeout(600, 0);
        client.set_connection_timeout(5, 0);
    }
};

OllamaLlm::OllamaLlm(std::string host) : impl_(std::make_unique<Impl>(std::move(host))) {}
OllamaLlm::~OllamaLlm() = default;

std::string OllamaLlm::describe() const {
    return "Ollama at " + impl_->host;
}

bool OllamaLlm::reachable() {
    auto res = impl_->client.Get("/api/tags");
    return res && res->status == 200;
}

std::vector<std::string> OllamaLlm::models() {
    std::vector<std::string> names;
    auto res = impl_->client.Get("/api/tags");
    if (!res || res->status != 200) return names;

    json parsed = json::parse(res->body, nullptr, false);
    if (parsed.is_discarded() || !parsed.contains("models")) return names;
    for (const json& m : parsed["models"]) {
        if (m.contains("name")) names.push_back(m["name"].get<std::string>());
    }
    return names;
}

Timing OllamaLlm::chat(const std::vector<Message>& messages,
                       const Options& options,
                       const OnDelta& on_delta) {
    Timing timing;
    // NDJSON arrives in whatever sized pieces the socket gives up, so a line can
    // be split across two chunks and a chunk can hold several lines.
    std::string partial;
    std::exception_ptr failure;

    httplib::Request req;
    req.method = "POST";
    req.path = "/api/chat";
    req.headers.emplace("Content-Type", "application/json");
    req.body = body_for(messages, options).dump();
    req.content_receiver = [&](const char* data, std::size_t length, std::uint64_t, std::uint64_t) {
        partial.append(data, length);
        std::size_t newline;
        while ((newline = partial.find('\n')) != std::string::npos) {
            const std::string line = partial.substr(0, newline);
            partial.erase(0, newline + 1);
            try {
                consume_line(line, on_delta, &timing);
            } catch (...) {
                // Thrown out of a callback the HTTP client is walking, so it is
                // held and rethrown once the transfer has been unwound.
                failure = std::current_exception();
                return false;
            }
        }
        return true;
    };

    httplib::Response res;
    httplib::Error error = httplib::Error::Success;
    const bool sent = impl_->client.send(req, res, error);

    if (failure) std::rethrow_exception(failure);
    if (!sent) {
        throw std::runtime_error("cannot reach " + impl_->host + ": " +
                                 httplib::to_string(error));
    }
    if (res.status != 200) {
        throw std::runtime_error("Ollama returned HTTP " + std::to_string(res.status));
    }

    // A body that did not end in a newline leaves its last line here.
    if (!partial.empty()) consume_line(partial, on_delta, &timing);
    return timing;
}

}  // namespace moonshine
