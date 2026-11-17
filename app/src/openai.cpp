#include "openai.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <thread>

#include "httplib.h"
#include "json.hpp"

namespace moonshine {

namespace {

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

constexpr std::string_view kOpen = "<think>";
constexpr std::string_view kClose = "</think>";

// How much of what a server said is passed on. Enough for its reason, not for a page.
constexpr std::size_t kSaidLimit = 240;
// How much of a refusal's body is kept to read the reason from.
constexpr std::size_t kErrorBodyLimit = 64 * 1024;
// Room added to the token limit for a model that reasons before it answers (Impl::reasons).
// The reasoning of the small Qwen3 measured was 150 to 300 tokens a turn; the big models'
// runs to a thousand and more.
constexpr int kReasoningRoom = 2048;
// How much of an answer that is not a stream is kept, to read as a plain completion.
constexpr std::size_t kPlainBodyLimit = 1024 * 1024;

std::string trimmed(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
    return std::string(text);
}

bool ends_with(std::string_view text, std::string_view tail) {
    return text.size() >= tail.size() && text.substr(text.size() - tail.size()) == tail;
}

// The longest tail of `text` that is the start of `tag` without being all of it: what has
// to be held back in case the next piece finishes the tag.
std::size_t tag_begun(std::string_view text, std::string_view tag) {
    const std::size_t most = std::min(text.size(), tag.size() - 1);
    for (std::size_t n = most; n > 0; --n) {
        if (text.substr(text.size() - n) == tag.substr(0, n)) return n;
    }
    return 0;
}

void replace_all(std::string* text, std::string_view what, std::string_view with) {
    if (what.empty()) return;
    for (std::size_t at = text->find(what); at != std::string::npos; at = text->find(what, at + with.size())) {
        text->replace(at, what.size(), with);
    }
}

// What the server said was wrong, out of the usual shapes: {"error":{"message":..}},
// {"error":".."}, {"message":..}, {"detail":..}; failing those, the body itself when it is
// short enough to be a sentence and not a page of HTML.
std::string said_in(std::string_view body) {
    json parsed = json::parse(body, nullptr, false);
    // Gemini's compatibility endpoint wraps the usual object in an array.
    if (parsed.is_array() && !parsed.empty() && parsed[0].is_object()) parsed = json(parsed[0]);
    if (parsed.is_object()) {
        if (parsed.contains("error")) {
            const json& error = parsed["error"];
            if (error.is_string()) return error.get<std::string>();
            if (error.is_object() && error.contains("message") && error["message"].is_string()) {
                return error["message"].get<std::string>();
            }
        }
        for (const char* field : {"message", "detail"}) {
            if (parsed.contains(field) && parsed[field].is_string()) return parsed[field].get<std::string>();
        }
        return {};
    }
    if (body.find('<') != std::string_view::npos) return {};
    return trimmed(body);
}

// Fit to be shown: one line, shortened, valid UTF-8 as far as the cut goes, and without
// the key, which some servers repeat back in saying it is wrong.
std::string shown(std::string said, std::string_view key) {
    // A key of a few characters is no key (a local server's placeholder), and taking it
    // out would take it out of every word that holds those letters.
    if (key.size() >= 8) replace_all(&said, key, "[the key]");
    for (char& c : said) {
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    }
    said = trimmed(said);
    if (said.size() > kSaidLimit) {
        std::size_t cut = kSaidLimit;
        // Not through the middle of a character.
        while (cut > 0 && (static_cast<unsigned char>(said[cut]) & 0xC0) == 0x80) --cut;
        said.resize(cut);
        said += "...";
    }
    return said;
}

ApiError network_error(httplib::Error error, const ApiAddress& address, bool begun) {
    using E = httplib::Error;
    switch (error) {
        case E::SSLServerVerification:
        case E::SSLServerHostnameVerification:
        case E::SSLLoadingCerts:
            return ApiError(ApiError::Kind::Certificate, 0,
                            address.host + " could not prove it is who it says it is, so nothing was sent to it.");
        case E::SSLConnection:
            return ApiError(ApiError::Kind::Certificate, 0,
                            "A secure connection to " + address.host + " could not be made.");
        default:
            break;
    }
    if (begun) {
        return ApiError(ApiError::Kind::Network, 0, address.host + " stopped answering part-way through.");
    }
    if (error == E::Read || error == E::Timeout) {
        return ApiError(ApiError::Kind::Network, 0, address.host + " took too long to answer.");
    }
    return ApiError(ApiError::Kind::Network, 0,
                    "Could not reach " + address.host +
                        ". Check the address in Settings, and that this PC is online.");
}

// The names a refusal might use for the fields this app can rename or do without.
constexpr const char* kLimitOld = "max_tokens";
constexpr const char* kLimitNew = "max_completion_tokens";
constexpr const char* kOptional[] = {"stream_options", "top_p", "temperature"};

}  // namespace

ApiAddress parse_api_url(std::string_view base_url) {
    std::string url = trimmed(base_url);
    if (url.empty()) {
        throw ApiError(ApiError::Kind::Settings, 0, "No API address is set.");
    }
    const auto bad = [&] {
        return ApiError(ApiError::Kind::Settings, 0,
                        "The API address is not one this can use: " + std::string(base_url).substr(0, 120));
    };
    ApiAddress address;
    std::string rest;
    if (const std::size_t at = url.find("://"); at != std::string::npos) {
        std::string scheme = url.substr(0, at);
        std::transform(scheme.begin(), scheme.end(), scheme.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (scheme == "https") address.encrypted = true;
        else if (scheme == "http") address.encrypted = false;
        else throw bad();
        rest = url.substr(at + 3);
    } else {
        // An address typed without one is taken for the safe kind.
        rest = url;
    }
    const std::size_t slash = rest.find('/');
    std::string authority = rest.substr(0, slash);
    std::string path = slash == std::string::npos ? std::string() : rest.substr(slash);
    if (authority.empty()) throw bad();
    for (const char c : authority) {
        if (std::isspace(static_cast<unsigned char>(c)) || c == '@' || c == '?' || c == '#') throw bad();
    }
    for (const char c : path) {
        if (std::isspace(static_cast<unsigned char>(c)) || c == '?' || c == '#') throw bad();
    }
    while (!path.empty() && path.back() == '/') path.pop_back();
    // The whole endpoint pasted in, which is what some providers' pages show.
    if (ends_with(path, "/chat/completions")) path.resize(path.size() - std::string_view("/chat/completions").size());
    while (!path.empty() && path.back() == '/') path.pop_back();

    address.origin = std::string(address.encrypted ? "https://" : "http://") + authority;
    address.path = path;
    // Without the port, for saying. An IPv6 literal keeps its brackets and its port.
    address.host = authority;
    if (authority.front() != '[') {
        if (const std::size_t colon = authority.rfind(':'); colon != std::string::npos) {
            address.host = authority.substr(0, colon);
        }
    }
    if (address.host.empty()) throw bad();
    return address;
}

ApiError api_error(const int status, std::string_view body, const ApiAddress& address,
                   std::string_view model, std::string_view key) {
    // As the server numbered it, whatever it is taken for below.
    const std::string http = " (HTTP " + std::to_string(status) + ")";
    const std::string said = shown(said_in(body), key);
    const std::string quoted = said.empty() ? std::string() : " It said: " + said;
    const std::string& host = address.host;
    // What it is taken for. A wrong key is a 401 from nearly everybody; Gemini says 400
    // and "API key".
    int taken = status;
    if (status == 400) {
        std::string low = said;
        std::transform(low.begin(), low.end(), low.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (low.find("api key") != std::string::npos || low.find("api_key") != std::string::npos) taken = 401;
    }
    switch (taken) {
        case 401:
            // What the server said is left out here: it is the one message likely to
            // repeat the key back.
            return ApiError(ApiError::Kind::Key, status,
                            host + " did not accept the API key" + http + ". Check the key in Settings.");
        case 402:
            return ApiError(ApiError::Kind::Limit, status,
                            "The account at " + host + " is out of credit" + http + "." + quoted);
        case 403:
            return ApiError(ApiError::Kind::Forbidden, status,
                            host + " will not let this key use " + std::string(model) + http + "." + quoted);
        case 404:
        case 405:
            return ApiError(ApiError::Kind::NotFound, status,
                            host + " has no model called " + std::string(model) +
                                ", or the address is not an API's" + http + "." + quoted);
        case 429:
            return ApiError(ApiError::Kind::Limit, status,
                            host + " is turning requests away" + http +
                                ": too many at once, or the account is out of credit." + quoted);
        default:
            break;
    }
    if (status >= 500) {
        return ApiError(ApiError::Kind::Server, status,
                        host + " has trouble of its own" + http + ". Try again in a moment." + quoted);
    }
    return ApiError(ApiError::Kind::Refused, status, host + " did not accept the request" + http + "." + quoted);
}

void SseParser::line(std::string_view text, std::vector<std::string>* out) {
    if (text.empty()) {
        // A blank line ends the event.
        if (has_data_) out->push_back(std::move(data_));
        data_.clear();
        has_data_ = false;
        return;
    }
    // A comment: what a server sends to keep the connection open while it thinks.
    if (text.front() == ':') return;
    const std::size_t colon = text.find(':');
    const std::string_view field = text.substr(0, colon);
    std::string_view value = colon == std::string_view::npos ? std::string_view() : text.substr(colon + 1);
    if (!value.empty() && value.front() == ' ') value.remove_prefix(1);
    // event:, id: and retry: mean nothing here.
    if (field != "data") return;
    if (has_data_) data_ += '\n';
    data_.append(value);
    has_data_ = true;
}

std::vector<std::string> SseParser::push(std::string_view bytes) {
    std::vector<std::string> out;
    partial_.append(bytes);
    std::size_t from = 0;
    for (std::size_t newline; (newline = partial_.find('\n', from)) != std::string::npos; from = newline + 1) {
        std::string_view text(partial_.data() + from, newline - from);
        if (!text.empty() && text.back() == '\r') text.remove_suffix(1);
        line(text, &out);
    }
    partial_.erase(0, from);
    return out;
}

std::vector<std::string> SseParser::finish() {
    std::vector<std::string> out;
    if (!partial_.empty()) {
        std::string_view text(partial_);
        if (text.back() == '\r') text.remove_suffix(1);
        if (!text.empty()) line(text, &out);
        partial_.clear();
    }
    line({}, &out);
    return out;
}

std::string ThinkFilter::push(std::string_view piece) {
    held_.append(piece);
    std::string out;
    for (;;) {
        if (thinking_) {
            const std::size_t end = held_.find(kClose);
            if (end == std::string::npos) {
                // All of it is reasoning, but for a tail that may be the closing tag begun.
                held_.erase(0, held_.size() - tag_begun(held_, kClose));
                break;
            }
            held_.erase(0, end + kClose.size());
            thinking_ = false;
            after_close_ = true;
            continue;
        }
        if (after_close_) {
            std::size_t space = 0;
            while (space < held_.size() && std::isspace(static_cast<unsigned char>(held_[space]))) ++space;
            held_.erase(0, space);
            if (held_.empty()) break;
            after_close_ = false;
        }
        const std::size_t begin = held_.find(kOpen);
        if (begin == std::string::npos) {
            const std::size_t keep = tag_begun(held_, kOpen);
            out.append(held_, 0, held_.size() - keep);
            held_.erase(0, held_.size() - keep);
            break;
        }
        out.append(held_, 0, begin);
        held_.erase(0, begin + kOpen.size());
        thinking_ = true;
        found_ = true;
    }
    return out;
}

std::string ThinkFilter::flush() {
    std::string out;
    if (!thinking_) out = std::move(held_);
    held_.clear();
    return out;
}

std::string api_request_body(const ApiSettings& settings, const std::vector<Message>& messages,
                             const Options& options, std::string_view limit_field,
                             const std::vector<std::string>& without) {
    json said = json::array();
    for (const Message& m : messages) {
        said.push_back({{"role", m.role}, {"content", m.content}});
    }
    // No top_k: it is not in the API, and the servers that take it are outnumbered by the
    // ones that refuse a request for having it.
    json body = {
        {"model", settings.model},
        {"stream", true},
        // Asks for the token counts on the last chunk, which a stream otherwise omits.
        {"stream_options", {{"include_usage", true}}},
        {"temperature", options.temperature},
        {"top_p", options.top_p},
        {std::string(limit_field), options.reply_tokens},
    };
    for (const std::string& field : without) body.erase(field);
    if (!trimmed(settings.extra_json).empty()) {
        const json extra = json::parse(settings.extra_json, nullptr, false);
        if (!extra.is_object()) {
            throw ApiError(ApiError::Kind::Settings, 0,
                           "The extra request JSON in Settings is not a JSON object.");
        }
        body.merge_patch(extra);
    }
    // After the extras, which may not change what is said or how it comes back.
    body["messages"] = std::move(said);
    body["stream"] = true;
    return body.dump(-1, ' ', false, json::error_handler_t::replace);
}

bool adapt_to_refusal(std::string_view body, std::string* limit_field,
                      std::vector<std::string>* without) {
    // The field the server names, where it names one (OpenAI's error.param); otherwise
    // whichever of ours its message mentions first.
    std::string param;
    std::string said = said_in(body);
    const json parsed = json::parse(body, nullptr, false);
    if (parsed.is_object() && parsed.contains("error") && parsed["error"].is_object()) {
        const json& error = parsed["error"];
        if (error.contains("param") && error["param"].is_string()) param = error["param"].get<std::string>();
    }
    const auto left_out = [&](std::string_view field) {
        return std::find(without->begin(), without->end(), field) != without->end();
    };
    const auto named = [&](std::string_view field) -> std::size_t {
        // "max_tokens" is not found inside "max_completion_tokens", so a plain search will do.
        if (param == field) return 0;
        // stream_options is named by its part as often as by itself.
        if (field == "stream_options" && param.rfind("stream_options", 0) == 0) return 0;
        const std::size_t at = said.find(field);
        return at == std::string::npos ? at : at + 1;
    };

    std::string first;
    std::size_t first_at = std::string::npos;
    const auto consider = [&](std::string_view field) {
        const std::size_t at = named(field);
        if (at < first_at) {
            first_at = at;
            first = std::string(field);
        }
    };
    consider(*limit_field);
    for (const char* field : kOptional) {
        if (!left_out(field)) consider(field);
    }
    if (first.empty()) return false;
    if (first == *limit_field) {
        *limit_field = *limit_field == kLimitOld ? kLimitNew : kLimitOld;
        return true;
    }
    without->push_back(first);
    return true;
}

struct OpenAiLlm::Impl {
    ApiSettings settings;
    ApiAddress address;
    std::unique_ptr<httplib::Client> client;
    // One conversation at a time: the client holds one connection.
    std::mutex busy;
    // What the server has taught this run about its dialect (adapt_to_refusal).
    std::string limit_field = kLimitOld;
    std::vector<std::string> without;
    bool limit_swapped = false;
    // Whether the model has been seen to reason before it answers. The token limit is the
    // server's count of everything the model writes, reasoning included, so a limit sized
    // for the words alone -- four tokens for a yes or a no -- is spent before a word is
    // said. Once it is known, every request is given room to reason in as well.
    bool reasons = false;

    explicit Impl(ApiSettings given) : settings(std::move(given)) {
        address = parse_api_url(settings.base_url);
        settings.model = trimmed(settings.model);
        settings.key = trimmed(settings.key);
        if (settings.model.empty()) {
            throw ApiError(ApiError::Kind::Settings, 0, "No model name is set for the API.");
        }
        if (!trimmed(settings.extra_json).empty() &&
            !json::parse(settings.extra_json, nullptr, false).is_object()) {
            throw ApiError(ApiError::Kind::Settings, 0,
                           "The extra request JSON in Settings is not a JSON object.");
        }
        client = std::make_unique<httplib::Client>(address.origin);
        // The connection is kept between turns: a TLS handshake is a quarter of a second
        // that would otherwise be on the front of every reply.
        client->set_keep_alive(true);
        client->set_connection_timeout(10, 0);
        client->set_write_timeout(30, 0);
        // Between one piece and the next, not for the whole reply. Long, because a model
        // that reasons before it speaks sends nothing while it does.
        client->set_read_timeout(120, 0);
        // A redirect is not followed: the key would go with it, to somewhere not named.
        client->set_follow_location(false);
    }

    struct Outcome {
        Timing timing;
        // HTTP 400, with what the server said: the one failure that may be worth another go.
        bool refused = false;
        int status = 0;
        std::string body;
        // The model reasoned, in a field of its own or between think tags.
        bool reasoned = false;
        // ...and the limit ran out before it had said a word.
        bool starved = false;
    };

    Outcome once(const std::vector<Message>& messages, const Options& options, const OnDelta& on_delta);
};

OpenAiLlm::Impl::Outcome OpenAiLlm::Impl::once(const std::vector<Message>& messages,
                                               const Options& options, const OnDelta& on_delta) {
    // The socket is read on a thread of its own, and what it reads is handed across to
    // this one, so that on_delta is only ever called on the caller's thread and is called
    // while the server is still silent: a caller who wants to stop does not have to wait
    // for a first token that may be seconds away.
    struct Shared {
        std::mutex lock;
        std::condition_variable woke;
        std::deque<std::string> pieces;
        std::string error_body;
        int status = 0;
        bool done = false;
        bool sent = false;
        bool abandoned = false;
        httplib::Error error = httplib::Error::Success;
    } shared;

    httplib::Request request;
    request.method = "POST";
    request.path = address.path + "/chat/completions";
    request.headers.emplace("Content-Type", "application/json");
    request.headers.emplace("Accept", "text/event-stream");
    if (!settings.key.empty()) request.headers.emplace("Authorization", "Bearer " + settings.key);
    Options asked = options;
    if (reasons) asked.reply_tokens += kReasoningRoom;
    request.body = api_request_body(settings, messages, asked, limit_field, without);
    request.response_handler = [&shared](const httplib::Response& response) {
        std::lock_guard<std::mutex> held(shared.lock);
        shared.status = response.status;
        return !shared.abandoned;
    };
    request.content_receiver = [&shared](const char* data, std::size_t length, std::uint64_t, std::uint64_t) {
        std::lock_guard<std::mutex> held(shared.lock);
        if (shared.abandoned) return false;
        if (shared.status == 200) {
            shared.pieces.emplace_back(data, length);
        } else if (shared.error_body.size() < kErrorBodyLimit) {
            shared.error_body.append(data, std::min(length, kErrorBodyLimit - shared.error_body.size()));
        }
        shared.woke.notify_one();
        return true;
    };

    const Clock::time_point began = Clock::now();
    std::thread reader([&] {
        httplib::Response response;
        httplib::Error error = httplib::Error::Success;
        const bool sent = client->send(request, response, error);
        std::lock_guard<std::mutex> held(shared.lock);
        shared.sent = sent;
        shared.error = error;
        // httplib's "no status yet" is -1; here it is 0.
        if (shared.status == 0 && response.status > 0) shared.status = response.status;
        shared.done = true;
        shared.woke.notify_one();
    });
    // However this function is left, the reader is told to stop and waited for: it holds
    // references to what is on this stack.
    struct Joined {
        Shared& shared;
        httplib::Client& client;
        std::thread& reader;
        ~Joined() {
            bool running;
            {
                std::lock_guard<std::mutex> held(shared.lock);
                running = !shared.done;
                shared.abandoned = true;
            }
            if (running) client.stop();
            if (reader.joinable()) reader.join();
        }
    } joined{shared, *client, reader};

    SseParser events;
    ThinkFilter think;
    Timing timing;
    std::string plain;
    Clock::time_point first_at{};
    bool spoke = false;
    bool any_event = false;
    bool finished = false;
    bool reasoned = false;
    bool usage_counted = false;
    int pieces = 0;
    std::string finish_reason;

    const auto speak = [&](std::string&& text) {
        if (text.empty()) return;
        if (!spoke) {
            spoke = true;
            first_at = Clock::now();
        }
        ++pieces;
        on_delta(text);
    };
    const auto usage_from = [&](const json& usage) {
        if (!usage.is_object()) return;
        usage_counted = true;
        if (usage.contains("prompt_tokens") && usage["prompt_tokens"].is_number()) {
            timing.prompt_tokens = usage["prompt_tokens"].get<int>();
        }
        if (usage.contains("completion_tokens") && usage["completion_tokens"].is_number()) {
            timing.reply_tokens = usage["completion_tokens"].get<int>();
        }
        // How much of the prompt the server had already: OpenAI's field, then DeepSeek's.
        if (usage.contains("prompt_tokens_details") && usage["prompt_tokens_details"].is_object()) {
            const json& details = usage["prompt_tokens_details"];
            if (details.contains("cached_tokens") && details["cached_tokens"].is_number()) {
                timing.cached_tokens = details["cached_tokens"].get<int>();
            }
        } else if (usage.contains("prompt_cache_hit_tokens") && usage["prompt_cache_hit_tokens"].is_number()) {
            timing.cached_tokens = usage["prompt_cache_hit_tokens"].get<int>();
        }
    };
    const auto message_from = [&](const json& said) {
        if (!said.is_object()) return;
        for (const char* field : {"reasoning_content", "reasoning"}) {
            // Reasoning, parsed out by the server into a field of its own: not speech, and
            // the player must not hear it.
            if (said.contains(field) && said[field].is_string() && !said[field].get_ref<const std::string&>().empty()) {
                reasoned = true;
            }
        }
        if (said.contains("content") && said["content"].is_string()) {
            speak(think.push(said["content"].get_ref<const std::string&>()));
        }
    };
    const auto failed_inside = [&](const json& error) {
        // An error sent as an event, after a 200: a provider that began and could not go on.
        int code = 0;
        if (error.is_object() && error.contains("code") && error["code"].is_number_integer()) {
            code = error["code"].get<int>();
        }
        const std::string body = json{{"error", error}}.dump(-1, ' ', false, json::error_handler_t::replace);
        if (code >= 400 && code < 600) return api_error(code, body, address, settings.model, settings.key);
        const std::string said = shown(said_in(body), settings.key);
        return ApiError(ApiError::Kind::Server, 0,
                        address.host + " gave up part-way through." + (said.empty() ? "" : " It said: " + said));
    };
    const auto consume = [&](const std::string& event) {
        any_event = true;
        if (event == "[DONE]") {
            finished = true;
            return;
        }
        const json chunk = json::parse(event, nullptr, false);
        if (!chunk.is_object()) {
            throw ApiError(ApiError::Kind::Protocol, 0,
                           address.host + " sent something that is not a Chat Completions stream.");
        }
        if (chunk.contains("error") && !chunk["error"].is_null()) throw failed_inside(chunk["error"]);
        if (chunk.contains("usage")) usage_from(chunk["usage"]);
        if (!chunk.contains("choices") || !chunk["choices"].is_array() || chunk["choices"].empty()) return;
        const json& choice = chunk["choices"][0];
        if (!choice.is_object()) return;
        if (choice.contains("delta")) message_from(choice["delta"]);
        if (choice.contains("finish_reason") && choice["finish_reason"].is_string()) {
            finish_reason = choice["finish_reason"].get<std::string>();
            finished = true;
        }
    };

    for (;;) {
        std::deque<std::string> got;
        bool done = false;
        {
            std::unique_lock<std::mutex> held(shared.lock);
            shared.woke.wait_for(held, std::chrono::milliseconds(50),
                                 [&] { return !shared.pieces.empty() || shared.done; });
            got.swap(shared.pieces);
            done = shared.done;
        }
        if (got.empty() && !done) {
            // Nothing yet. The caller is given the chance to stop (llm.h).
            on_delta(std::string_view());
            continue;
        }
        for (const std::string& bytes : got) {
            if (!any_event && plain.size() < kPlainBodyLimit) plain.append(bytes);
            for (const std::string& event : events.push(bytes)) consume(event);
        }
        if (done) break;
    }
    for (const std::string& event : events.finish()) consume(event);

    // The reader is finished, so what it left can be read without the lock.
    const int status = shared.status;
    if (!shared.sent && status == 0) throw network_error(shared.error, address, false);
    if (status != 200) {
        Outcome outcome;
        outcome.status = status;
        outcome.body = std::move(shared.error_body);
        outcome.refused = status == 400 || status == 422;
        if (outcome.refused) return outcome;
        throw api_error(status, outcome.body, address, settings.model, settings.key);
    }
    if (!any_event) {
        // Not a stream after all: a server that ignores "stream" and answers whole.
        const json whole = json::parse(plain, nullptr, false);
        if (!whole.is_object()) {
            if (!shared.sent) throw network_error(shared.error, address, !plain.empty());
            throw ApiError(ApiError::Kind::Protocol, 0,
                           address.host + " answered, but not as a Chat Completions API does. Check the address in Settings.");
        }
        if (whole.contains("error") && !whole["error"].is_null()) throw failed_inside(whole["error"]);
        if (whole.contains("usage")) usage_from(whole["usage"]);
        if (!whole.contains("choices") || !whole["choices"].is_array() || whole["choices"].empty() ||
            !whole["choices"][0].is_object() || !whole["choices"][0].contains("message")) {
            throw ApiError(ApiError::Kind::Protocol, 0,
                           address.host + " answered, but not as a Chat Completions API does. Check the address in Settings.");
        }
        const json& choice = whole["choices"][0];
        message_from(choice["message"]);
        if (choice.contains("finish_reason") && choice["finish_reason"].is_string()) {
            finish_reason = choice["finish_reason"].get<std::string>();
        }
    } else if (!shared.sent && !finished) {
        // The stream broke before its end. (One that broke after it -- a server that
        // says [DONE] and drops the line without closing the body -- said all it had.)
        throw network_error(shared.error, address, true);
    }
    speak(think.flush());

    Outcome outcome;
    outcome.reasoned = reasoned || think.found();
    // The whole limit went on reasoning and nothing was said. Not an empty reply to be
    // passed on as one: the caller asks again with room to reason in, or says so.
    outcome.starved = !spoke && outcome.reasoned && finish_reason == "length";

    const Clock::time_point ended = Clock::now();
    const auto seconds = [](Clock::duration d) { return std::chrono::duration<double>(d).count(); };
    // The wait for the first word is the nearest thing an API has to a prefill; the rest
    // is the reply arriving.
    timing.prefill_seconds = seconds((spoke ? first_at : ended) - began);
    timing.decode_seconds = spoke ? seconds(ended - first_at) : 0.0;
    // A server that sent no counts: the pieces that arrived, which is a token each from
    // most servers and a fair figure from the rest.
    if (!usage_counted || timing.reply_tokens == 0) timing.reply_tokens = std::max(timing.reply_tokens, pieces);
    outcome.timing = timing;
    outcome.status = status;
    return outcome;
}

OpenAiLlm::OpenAiLlm(ApiSettings settings) : impl_(std::make_unique<Impl>(std::move(settings))) {}
OpenAiLlm::~OpenAiLlm() = default;

std::string OpenAiLlm::describe() const {
    return impl_->settings.model + " at " + impl_->address.host + " (API)";
}

Timing OpenAiLlm::chat(const std::vector<Message>& messages, const Options& options,
                       const OnDelta& on_delta) {
    std::lock_guard<std::mutex> held(impl_->busy);
    // A refusal that names a field this can rename or leave out is met by doing so and
    // asking again, and what was learned is kept for the rest of the run. Bounded: there
    // are four such fields, and the limit's name is swapped once.
    for (int attempt = 0;; ++attempt) {
        Impl::Outcome outcome = impl_->once(messages, options, on_delta);
        if (outcome.starved) {
            // A model that reasons, met with a limit sized for the words alone. Asked
            // again, once, with room for the reasoning; and every request after this one
            // is given that room from the start (Impl::reasons).
            if (impl_->reasons) {
                throw ApiError(ApiError::Kind::Refused, 0,
                               impl_->settings.model +
                                   " spent its whole reply reasoning and said nothing. Turn its reasoning off or "
                                   "down with the extra request JSON in Settings, or choose a model that does not reason.");
            }
            impl_->reasons = true;
            continue;
        }
        if (!outcome.refused) {
            if (outcome.reasoned) impl_->reasons = true;
            return outcome.timing;
        }
        std::string limit = impl_->limit_field;
        std::vector<std::string> without = impl_->without;
        const bool changed = attempt < 4 && adapt_to_refusal(outcome.body, &limit, &without);
        const bool swapped = limit != impl_->limit_field;
        if (!changed || (swapped && impl_->limit_swapped)) {
            throw api_error(outcome.status, outcome.body, impl_->address, impl_->settings.model,
                            impl_->settings.key);
        }
        if (swapped) impl_->limit_swapped = true;
        impl_->limit_field = std::move(limit);
        impl_->without = std::move(without);
    }
}

bool OpenAiLlm::reasons() const {
    return impl_->reasons;
}

std::string OpenAiLlm::probe() {
    // Enough room for a word from a model that says one, and for the usual sampling to
    // be refused or accepted as it will be in a conversation.
    Options options;
    options.reply_tokens = 16;
    std::string said;
    chat({{"user", "Reply with the single word: ready"}}, options,
         [&](std::string_view piece) { said.append(piece); });
    return trimmed(said);
}

}  // namespace moonshine
