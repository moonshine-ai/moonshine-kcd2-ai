// A model somewhere else: any server that speaks OpenAI's Chat Completions.
//
// The player's own choice, made in the window's settings, in place of the model that ships.
// What it buys is a bigger model than a CPU beside the game can run; what it costs is that
// the conversation leaves the machine, to whoever they named, and is paid for by the token.
// Everything else stays here: the ear, the voice and the knowledge search do not change.
//
// Chat Completions rather than any vendor's newer API, because it is the one shape nearly
// everything accepts: OpenAI, OpenRouter, Groq, Together, Mistral, DeepSeek, Gemini's
// compatibility endpoint, and a llama.cpp, vLLM, LM Studio or Ollama server on the
// player's own network. The request is the plain one -- messages, a temperature, a token
// limit, streamed -- and the differences between servers are met as they are found: a
// parameter a server refuses is renamed or left out and the request sent once more
// (adapt_to_refusal), reasoning that arrives in a field of its own is dropped, and
// reasoning that arrives in the text, between <think> tags, is taken out (ThinkFilter).
//
// Metered, which changes what the app asks of it (Llm::wants_warming): nothing is sent to
// warm a cache, and nothing is guessed ahead unless the player asked for that.

#ifndef MOONSHINE_OPENAI_H
#define MOONSHINE_OPENAI_H

#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "llm.h"

namespace moonshine {

struct ApiSettings {
    // Up to and including the version: https://api.openai.com/v1. A base given with
    // /chat/completions on the end, or a slash, is taken as meant.
    std::string base_url;
    // Sent as a bearer token. Empty for a server that wants none (one on the player's own
    // network). Never printed, never logged, and scrubbed from anything a server says back.
    std::string key;
    std::string model;
    // A JSON object merged over the request (RFC 7386, so a null removes a field), for
    // whatever one server needs that the others do not: {"reasoning_effort":"low"},
    // {"enable_thinking":false}, a provider's routing. Empty is none.
    std::string extra_json;
};

// What went wrong, in the terms the player can act on. what() is one sentence fit for the
// window; it never holds the key.
class ApiError : public std::runtime_error {
public:
    enum class Kind {
        Network,      // nothing answered: no connection, a wrong address, a timeout
        Certificate,  // something answered, and could not prove it was who was asked for
        Key,          // 401: the key is wrong or missing
        Forbidden,    // 403: the key is not allowed this
        NotFound,     // 404: no such model, or no API at this address
        Limit,        // 429: too many requests, or out of credit
        Refused,      // 400 and its kin: the request was not accepted
        Server,       // 5xx: their trouble
        Protocol,     // an answer that was not Chat Completions
        Settings,     // the settings themselves cannot be used: no address, bad extra JSON
    };
    ApiError(Kind kind, int status, const std::string& what)
        : std::runtime_error(what), kind_(kind), status_(status) {}
    Kind kind() const { return kind_; }
    // The HTTP status, or 0 when there was none.
    int status() const { return status_; }

private:
    Kind kind_;
    int status_;
};

// A base URL taken apart: what the HTTP client connects to, and the path the endpoints
// hang off. Throws ApiError (Settings) for one that cannot be an address.
struct ApiAddress {
    std::string origin;  // https://api.openai.com, with a port if one was given
    std::string host;    // api.openai.com, for saying
    std::string path;    // /v1, or empty
    bool encrypted = true;
};
ApiAddress parse_api_url(std::string_view base_url);

// The sentence for an HTTP status and whatever the server said with it. `body` is the
// response as it came (usually {"error":{"message":...}}); what the server said is quoted
// after the app's own words, shortened, with `key` taken out of it should it be there.
ApiError api_error(int status, std::string_view body, const ApiAddress& address,
                   std::string_view model, std::string_view key);

// Server-sent events, taken from the socket in whatever pieces it gives up: a line can be
// split across two reads, and one read can hold several events. push() returns the data of
// every event completed by what was given; finish() returns one left without its blank
// line when the stream ended.
class SseParser {
public:
    std::vector<std::string> push(std::string_view bytes);
    std::vector<std::string> finish();

private:
    void line(std::string_view text, std::vector<std::string>* out);
    std::string partial_;
    std::string data_;
    bool has_data_ = false;
};

// Takes <think>...</think> out of a reply as it streams. Some servers put a model's
// reasoning in the text itself rather than a field of its own, and it is not speech: the
// player must not hear it. A tag can be split anywhere across two pieces, so what might
// be the start of one is held until it is known. The space after a closing tag goes too.
class ThinkFilter {
public:
    std::string push(std::string_view piece);
    // What was held at the end of the stream: text that looked like the start of a tag and
    // was not one. Reasoning never closed is dropped.
    std::string flush();
    // Whether any reasoning was taken out, for saying so once.
    bool found() const { return found_; }

private:
    std::string held_;
    bool thinking_ = false;
    bool after_close_ = false;
    bool found_ = false;
};

// The request, as JSON text. Exposed so that what is sent can be tested without a server.
// `limit_field` is "max_tokens" or "max_completion_tokens"; `without` names fields to
// leave out (a server refused them).
std::string api_request_body(const ApiSettings& settings, const std::vector<Message>& messages,
                             const Options& options, std::string_view limit_field,
                             const std::vector<std::string>& without);

// What a refusal (HTTP 400, `body`) says to change about the next attempt, if anything:
// the token limit's other name, or a sampling field left out. Returns false when the
// refusal is not about a field this app can do without, or the change is already made.
bool adapt_to_refusal(std::string_view body, std::string* limit_field,
                      std::vector<std::string>* without);

class OpenAiLlm : public Llm {
public:
    // Throws ApiError (Settings) when the address or the extra JSON cannot be used.
    explicit OpenAiLlm(ApiSettings settings);
    ~OpenAiLlm() override;

    // Throws ApiError. on_delta is also called with an empty piece several times a second
    // while the server has yet to answer, on the calling thread, so that a caller who
    // wants to stop can throw out of it then (llm.h); the request is abandoned at once.
    Timing chat(const std::vector<Message>& messages,
                const Options& options,
                const OnDelta& on_delta) override;

    std::string describe() const override;
    bool wants_warming() const override { return false; }

    // Whether the model has been seen to reason before it answers, in a field of its own or
    // between think tags. Neither is ever said; but it is slower, the reasoning is paid for,
    // and the limit on every request is raised to leave room for it (openai.cpp), so it is
    // worth telling the player, who may be able to turn it off (ApiSettings::extra_json).
    bool reasons() const;

    // One short question, asked for the sake of the answer to "does this work": the
    // settings screen's test, and the check at start-up. Returns what the model said;
    // throws ApiError saying what is wrong.
    std::string probe();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace moonshine

#endif  // MOONSHINE_OPENAI_H
