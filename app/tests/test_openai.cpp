// The model behind an API (openai.h), tested against a server in this process.
//
// No real endpoint is asked anything here: what is worth pinning down is what this side
// does with what arrives, and a server on the loopback that says exactly what a test tells
// it to is the only way to make the awkward cases arrive on demand -- an event cut in half
// by the socket, a refusal that names a field, a server that goes quiet.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "doctest.h"
#include "httplib.h"
#include "json.hpp"
#include "openai.h"

using namespace moonshine;
using json = nlohmann::json;

namespace {

std::string joined(const std::vector<std::string>& parts) {
    std::string out;
    for (const std::string& part : parts) out += "[" + part + "]";
    return out;
}

// One chunk of a stream, as a server would write it.
std::string event(const json& chunk) {
    return "data: " + chunk.dump() + "\n\n";
}
std::string said(const std::string& content) {
    return event({{"choices", json::array({json{{"index", 0}, {"delta", {{"content", content}}}}})}});
}
std::string stop_chunk(const char* reason = "stop") {
    return event({{"choices", json::array({json{{"index", 0}, {"delta", json::object()}, {"finish_reason", reason}}})}});
}
const std::string kDone = "data: [DONE]\n\n";

// A Chat Completions server on a port of its own, answering as the test's handler says.
// What each request carried is kept, since half of what matters is what was sent.
class FakeApi {
public:
    struct Asked {
        json body;
        std::string authorization;
        std::string path;
    };
    using Handler = std::function<void(const Asked&, httplib::Response&)>;

    explicit FakeApi(Handler handler) : handler_(std::move(handler)) {
        const auto answer = [this](const httplib::Request& request, httplib::Response& response) {
            Asked asked{json::parse(request.body, nullptr, false), request.get_header_value("Authorization"),
                        request.path};
            {
                std::lock_guard<std::mutex> held(lock_);
                asked_.push_back(asked);
            }
            handler_(asked, response);
        };
        server_.Post("/v1/chat/completions", answer);
        port_ = server_.bind_to_any_port("127.0.0.1");
        thread_ = std::thread([this] { server_.listen_after_bind(); });
        server_.wait_until_ready();
    }
    ~FakeApi() {
        server_.stop();
        if (thread_.joinable()) thread_.join();
    }

    std::string url() const { return "http://127.0.0.1:" + std::to_string(port_) + "/v1"; }
    std::vector<Asked> asked() {
        std::lock_guard<std::mutex> held(lock_);
        return asked_;
    }

    // An answer that streams the given pieces of bytes, one write each, a moment apart, so
    // that they reach the client as separate reads.
    static void stream(httplib::Response& response, std::vector<std::string> pieces) {
        response.set_chunked_content_provider(
            "text/event-stream", [pieces = std::move(pieces)](std::size_t, httplib::DataSink& sink) {
                for (const std::string& piece : pieces) {
                    if (!sink.write(piece.data(), piece.size())) return false;
                    std::this_thread::sleep_for(std::chrono::milliseconds(3));
                }
                sink.done();
                return true;
            });
    }
    static void refuse(httplib::Response& response, int status, const json& error) {
        response.status = status;
        response.set_content(json{{"error", error}}.dump(), "application/json");
    }

private:
    Handler handler_;
    httplib::Server server_;
    std::thread thread_;
    int port_ = 0;
    std::mutex lock_;
    std::vector<Asked> asked_;
};

ApiSettings settings_for(const FakeApi& api) {
    return {api.url(), "sk-test-0123456789", "test-model", ""};
}

struct Heard {
    std::string text;
    int pieces = 0;
    int beats = 0;
    OnDelta on_delta() {
        return [this](std::string_view piece) {
            if (piece.empty()) {
                ++beats;
                return;
            }
            ++pieces;
            text.append(piece);
        };
    }
};

const std::vector<Message> kAsk = {{"system", "You are Bartel."}, {"user", "Good day."}};

}  // namespace

TEST_CASE("events are found however the socket cuts them up") {
    const std::string stream = "data: one\n\ndata: two\r\n\r\n: a comment to keep the line open\n\ndata: three\n\n";
    // Every way of cutting the stream in two gives the same three events.
    for (std::size_t cut = 0; cut <= stream.size(); ++cut) {
        SseParser parser;
        std::vector<std::string> got = parser.push(stream.substr(0, cut));
        for (std::string& more : parser.push(stream.substr(cut))) got.push_back(std::move(more));
        CAPTURE(cut);
        CHECK(joined(got) == "[one][two][three]");
        CHECK(parser.finish().empty());
    }
    // And a byte at a time.
    SseParser parser;
    std::vector<std::string> got;
    for (const char c : stream) {
        for (std::string& more : parser.push(std::string_view(&c, 1))) got.push_back(std::move(more));
    }
    CHECK(joined(got) == "[one][two][three]");
}

TEST_CASE("an event's data lines are joined, and its other fields are not data") {
    SseParser parser;
    const std::vector<std::string> got =
        parser.push("event: message\nid: 7\ndata: first\ndata:second\nretry: 10\n\n");
    REQUIRE(got.size() == 1);
    // One leading space is the separator's; a line without it loses nothing.
    CHECK(got[0] == "first\nsecond");
}

TEST_CASE("a stream that ends without its blank line still gives up its last event") {
    SseParser parser;
    CHECK(parser.push("data: one\n\ndata: [DONE]").size() == 1);
    const std::vector<std::string> last = parser.finish();
    REQUIRE(last.size() == 1);
    CHECK(last[0] == "[DONE]");
    CHECK(parser.finish().empty());
}

TEST_CASE("reasoning between think tags is taken out of the reply") {
    ThinkFilter filter;
    CHECK(filter.push("<think>He greets me. I am a miller.</think>\n\nGod be with you.") == "God be with you.");
    CHECK(filter.flush().empty());
    CHECK(filter.found());
}

TEST_CASE("a think tag split across pieces is still a tag") {
    const std::string whole = "<think>\nshould I be rude? no.\n</think>\n\nGood day to you, <b>sir</b>. 2 < 3.";
    const std::string want = "Good day to you, <b>sir</b>. 2 < 3.";
    for (std::size_t size = 1; size <= 9; ++size) {
        ThinkFilter filter;
        std::string out;
        for (std::size_t at = 0; at < whole.size(); at += size) out += filter.push(whole.substr(at, size));
        out += filter.flush();
        CAPTURE(size);
        CHECK(out == want);
    }
}

TEST_CASE("text with no reasoning in it passes through, held only where a tag might be starting") {
    ThinkFilter filter;
    CHECK(filter.push("A loaf is two groschen") == "A loaf is two groschen");
    // Could be the start of <think>: held until the next piece says.
    CHECK(filter.push(", <th") == ", ");
    CHECK(filter.push("ree for a pair>.") == "<three for a pair>.");
    CHECK(filter.push(" Less <") == " Less ");
    // The stream ended there: it was only an angle bracket.
    CHECK(filter.flush() == "<");
    CHECK_FALSE(filter.found());
}

TEST_CASE("reasoning that never closes is not said") {
    ThinkFilter filter;
    CHECK(filter.push("<think>the budget ran out while I was still").empty());
    CHECK(filter.flush().empty());
    CHECK(filter.found());
}

TEST_CASE("an API address is taken as meant") {
    ApiAddress a = parse_api_url("https://api.openai.com/v1");
    CHECK(a.origin == "https://api.openai.com");
    CHECK(a.host == "api.openai.com");
    CHECK(a.path == "/v1");
    CHECK(a.encrypted);

    // A trailing slash, the whole endpoint pasted in, and spaces round it.
    CHECK(parse_api_url("  https://openrouter.ai/api/v1/ ").path == "/api/v1");
    CHECK(parse_api_url("https://api.groq.com/openai/v1/chat/completions").path == "/openai/v1");

    // A server on the player's own network: a port, no encryption, perhaps no path.
    a = parse_api_url("http://192.168.0.20:8080");
    CHECK(a.origin == "http://192.168.0.20:8080");
    CHECK(a.host == "192.168.0.20");
    CHECK(a.path.empty());
    CHECK_FALSE(a.encrypted);

    // No scheme is taken for the safe one.
    CHECK(parse_api_url("api.mistral.ai/v1").origin == "https://api.mistral.ai");
    CHECK(parse_api_url("HTTPS://Example.com/v1").origin == "https://Example.com");
}

TEST_CASE("an address that cannot be one is refused with a sentence") {
    for (const char* bad : {"", "   ", "ftp://example.com/v1", "https://", "https://exa mple.com/v1",
                            "https://user@example.com/v1", "https://example.com/v1?key=abc"}) {
        CAPTURE(bad);
        try {
            parse_api_url(bad);
            FAIL("accepted");
        } catch (const ApiError& why) {
            CHECK(why.kind() == ApiError::Kind::Settings);
        }
    }
}

TEST_CASE("the request is the plain one, with the extras merged over it") {
    ApiSettings settings{"https://example.com/v1", "sk-secret", "big-model", ""};
    Options options;
    options.reply_tokens = 300;
    options.temperature = 0.7;
    options.top_p = 0.9;
    options.top_k = 20;
    json body = json::parse(api_request_body(settings, kAsk, options, "max_tokens", {}));
    CHECK(body["model"] == "big-model");
    CHECK(body["stream"] == true);
    CHECK(body["stream_options"]["include_usage"] == true);
    CHECK(body["max_tokens"] == 300);
    CHECK(body["temperature"] == doctest::Approx(0.7));
    CHECK(body["top_p"] == doctest::Approx(0.9));
    // Not in the API, and refused by more servers than take it.
    CHECK_FALSE(body.contains("top_k"));
    // The key travels in a header, never in the body.
    CHECK(body.dump().find("sk-secret") == std::string::npos);
    REQUIRE(body["messages"].size() == 2);
    CHECK(body["messages"][0] == json{{"role", "system"}, {"content", "You are Bartel."}});
    CHECK(body["messages"][1] == json{{"role", "user"}, {"content", "Good day."}});

    // The other name for the limit, and a field a server refused.
    body = json::parse(api_request_body(settings, kAsk, options, "max_completion_tokens", {"top_p"}));
    CHECK(body["max_completion_tokens"] == 300);
    CHECK_FALSE(body.contains("max_tokens"));
    CHECK_FALSE(body.contains("top_p"));

    // Extras: a new field, a nested one, a null that removes one of ours -- and neither
    // the messages nor the streaming are theirs to change.
    settings.extra_json = R"({"reasoning_effort":"low","provider":{"sort":"latency"},"top_p":null,)"
                          R"("stream":false,"messages":[]})";
    body = json::parse(api_request_body(settings, kAsk, options, "max_tokens", {}));
    CHECK(body["reasoning_effort"] == "low");
    CHECK(body["provider"]["sort"] == "latency");
    CHECK_FALSE(body.contains("top_p"));
    CHECK(body["stream"] == true);
    CHECK(body["messages"].size() == 2);

    settings.extra_json = "[1, 2]";
    CHECK_THROWS_AS(api_request_body(settings, kAsk, options, "max_tokens", {}), ApiError);
    settings.extra_json = "{not json";
    CHECK_THROWS_AS(api_request_body(settings, kAsk, options, "max_tokens", {}), ApiError);
}

TEST_CASE("a refusal that names a field says how to ask again") {
    std::string limit = "max_tokens";
    std::vector<std::string> without;

    // OpenAI's reasoning models, as they say it.
    CHECK(adapt_to_refusal(
        R"({"error":{"message":"Unsupported parameter: 'max_tokens' is not supported with this model. Use 'max_completion_tokens' instead.","type":"invalid_request_error","param":"max_tokens","code":"unsupported_parameter"}})",
        &limit, &without));
    CHECK(limit == "max_completion_tokens");
    CHECK(without.empty());

    CHECK(adapt_to_refusal(
        R"({"error":{"message":"Unsupported value: 'temperature' does not support 0 with this model. Only the default (1) value is supported.","param":"temperature"}})",
        &limit, &without));
    CHECK(without == std::vector<std::string>{"temperature"});

    // Named only in the message, by a server with no param field.
    CHECK(adapt_to_refusal(R"({"error":"unknown field stream_options"})", &limit, &without));
    CHECK(without == std::vector<std::string>{"temperature", "stream_options"});
    // Already left out: there is nothing more to change about it.
    CHECK_FALSE(adapt_to_refusal(R"({"error":"unknown field stream_options"})", &limit, &without));

    // A server that never learned the new name.
    CHECK(adapt_to_refusal(R"({"message":"max_completion_tokens: extra fields not permitted"})", &limit, &without));
    CHECK(limit == "max_tokens");

    // Nothing this can do without.
    CHECK_FALSE(adapt_to_refusal(R"({"error":{"message":"This model's maximum context length is 8192 tokens.","param":"messages"}})",
                                 &limit, &without));
    CHECK_FALSE(adapt_to_refusal("<html>Bad Request</html>", &limit, &without));
    CHECK(limit == "max_tokens");
}

TEST_CASE("each failure is a sentence the player can act on, and never holds the key") {
    const ApiAddress address = parse_api_url("https://api.example.com/v1");
    const std::string key = "sk-live-abcdef123456";

    ApiError error = api_error(401, R"({"error":{"message":"Incorrect API key provided: sk-live-abcdef123456."}})",
                               address, "big-model", key);
    CHECK(error.kind() == ApiError::Kind::Key);
    CHECK(error.status() == 401);
    CHECK(std::string(error.what()) == "api.example.com did not accept the API key (HTTP 401). Check the key in Settings.");

    error = api_error(404, R"({"error":{"message":"The model `big-model` does not exist"}})", address, "big-model", key);
    CHECK(error.kind() == ApiError::Kind::NotFound);
    CHECK(std::string(error.what()).find("has no model called big-model") != std::string::npos);
    CHECK(std::string(error.what()).find("does not exist") != std::string::npos);

    error = api_error(429, R"({"error":{"message":"Rate limit reached for key sk-live-abcdef123456 in org"}})",
                      address, "big-model", key);
    CHECK(error.kind() == ApiError::Kind::Limit);
    CHECK(std::string(error.what()).find(key) == std::string::npos);
    CHECK(std::string(error.what()).find("[the key]") != std::string::npos);

    CHECK(api_error(402, "{}", address, "m", key).kind() == ApiError::Kind::Limit);
    CHECK(api_error(403, "{}", address, "m", key).kind() == ApiError::Kind::Forbidden);
    CHECK(api_error(400, R"({"detail":"bad"})", address, "m", key).kind() == ApiError::Kind::Refused);

    // A gateway's page of HTML is not quoted; a long message is cut, on a character's edge.
    error = api_error(502, "<html><body><h1>502 Bad Gateway</h1></body></html>", address, "m", key);
    CHECK(error.kind() == ApiError::Kind::Server);
    CHECK(std::string(error.what()).find('<') == std::string::npos);
    std::string longwinded(300, 'x');
    longwinded += "\xC3\xA9\xC3\xA9";
    error = api_error(500, json{{"error", {{"message", std::string(239, 'x') + "\xC3\xA9" + longwinded}}}}.dump(),
                      address, "m", key);
    const std::string what = error.what();
    CHECK(what.size() < 400);
    CHECK(json(what).dump(-1, ' ', false, json::error_handler_t::strict).size() > 0);
}

TEST_CASE("a reply streams through, with what it cost") {
    FakeApi api([](const FakeApi::Asked&, httplib::Response& response) {
        // Cut in awkward places: mid-event, mid-word, two events in one write.
        const std::string all =
            event({{"choices", json::array({json{{"index", 0}, {"delta", {{"role", "assistant"}, {"content", ""}}}}})}}) +
            said("God be ") + said("with you, ") + said("stranger.") + stop_chunk() +
            event({{"choices", json::array()},
                   {"usage", {{"prompt_tokens", 1234}, {"completion_tokens", 9},
                              {"prompt_tokens_details", {{"cached_tokens", 1024}}}}}}) +
            kDone;
        FakeApi::stream(response, {all.substr(0, 17), all.substr(17, 140), all.substr(157, 3), all.substr(160)});
    });
    OpenAiLlm llm(settings_for(api));
    Heard heard;
    Options options;
    options.reply_tokens = 77;
    const Timing timing = llm.chat(kAsk, options, heard.on_delta());

    CHECK(heard.text == "God be with you, stranger.");
    CHECK(heard.pieces == 3);
    CHECK(timing.prompt_tokens == 1234);
    CHECK(timing.reply_tokens == 9);
    CHECK(timing.cached_tokens == 1024);
    CHECK(timing.prefill_seconds > 0.0);
    CHECK(timing.decode_seconds >= 0.0);

    const std::vector<FakeApi::Asked> asked = api.asked();
    REQUIRE(asked.size() == 1);
    CHECK(asked[0].authorization == "Bearer sk-test-0123456789");
    CHECK(asked[0].body["model"] == "test-model");
    CHECK(asked[0].body["max_tokens"] == 77);
    CHECK(asked[0].body["messages"].size() == 2);
    CHECK(llm.describe() == "test-model at 127.0.0.1 (API)");
    CHECK_FALSE(llm.wants_warming());
}

TEST_CASE("a server that sends no token counts is counted by its pieces, and its cache is unknown") {
    FakeApi api([](const FakeApi::Asked&, httplib::Response& response) {
        FakeApi::stream(response, {said("Aye."), said(" What of it?"), stop_chunk(), kDone});
    });
    OpenAiLlm llm(settings_for(api));
    Heard heard;
    const Timing timing = llm.chat(kAsk, Options{}, heard.on_delta());
    CHECK(heard.text == "Aye. What of it?");
    CHECK(timing.prompt_tokens == 0);
    CHECK(timing.reply_tokens == 2);
    // Will not say, which is not the same as none (llm.h).
    CHECK(timing.cached_tokens == -1);
}

TEST_CASE("no key is sent to a server that wants none") {
    FakeApi api([](const FakeApi::Asked&, httplib::Response& response) {
        FakeApi::stream(response, {said("Yes."), stop_chunk(), kDone});
    });
    ApiSettings settings = settings_for(api);
    settings.key.clear();
    OpenAiLlm llm(settings);
    Heard heard;
    llm.chat(kAsk, Options{}, heard.on_delta());
    CHECK(api.asked().at(0).authorization.empty());
}

TEST_CASE("reasoning is not said, whichever way the server sends it") {
    FakeApi api([](const FakeApi::Asked&, httplib::Response& response) {
        const auto thought = [](const char* field, const char* text) {
            return event({{"choices", json::array({json{{"index", 0}, {"delta", {{field, text}}}}})}});
        };
        FakeApi::stream(response, {thought("reasoning_content", "He is a stranger. "),
                                   thought("reasoning", "Be curt."),
                                   said("<thi"), said("nk>and in the text too</th"), said("ink>\n\nWhat do you "),
                                   said("want?"), stop_chunk(), kDone});
    });
    OpenAiLlm llm(settings_for(api));
    Heard heard;
    llm.chat(kAsk, Options{}, heard.on_delta());
    CHECK(heard.text == "What do you want?");
}

TEST_CASE("a model that spends its whole reply reasoning is an error that says what to change") {
    FakeApi api([](const FakeApi::Asked&, httplib::Response& response) {
        FakeApi::stream(response,
                        {event({{"choices", json::array({json{{"index", 0}, {"delta", {{"reasoning_content", "Let me think"}}}}})}}),
                         stop_chunk("length"), kDone});
    });
    OpenAiLlm llm(settings_for(api));
    Heard heard;
    try {
        llm.chat(kAsk, Options{}, heard.on_delta());
        FAIL("an empty reply was passed off as one");
    } catch (const ApiError& why) {
        CHECK(std::string(why.what()).find("spent its whole reply reasoning") != std::string::npos);
    }
    CHECK(heard.text.empty());
    // Asked twice: as usual, and again with room to reason in, which this server also spent.
    const std::vector<FakeApi::Asked> asked = api.asked();
    REQUIRE(asked.size() == 2);
    CHECK(asked[1].body["max_tokens"].get<int>() > asked[0].body["max_tokens"].get<int>() + 1000);
}

TEST_CASE("a model that reasons is given room to, once it is seen to") {
    // The judge's question has four tokens to be answered in (quest.cpp), and a model that
    // reasons spends those before it has said yes or no. The first time that happens the
    // question is asked again with room for the reasoning, and from then on every request
    // has the room from the start.
    FakeApi api([](const FakeApi::Asked& asked, httplib::Response& response) {
        const auto thought = event({{"choices", json::array({json{{"index", 0}, {"delta", {{"reasoning_content", "Is it? Let me see."}}}}})}});
        if (asked.body["max_tokens"].get<int>() < 100) {
            FakeApi::stream(response, {thought, stop_chunk("length"), kDone});
        } else {
            FakeApi::stream(response, {thought, said("YES"), stop_chunk(), kDone});
        }
    });
    OpenAiLlm llm(settings_for(api));
    CHECK_FALSE(llm.reasons());
    Options judge;
    judge.reply_tokens = 4;

    Heard first;
    llm.chat(kAsk, judge, first.on_delta());
    CHECK(first.text == "YES");
    CHECK(llm.reasons());
    std::vector<FakeApi::Asked> asked = api.asked();
    REQUIRE(asked.size() == 2);
    CHECK(asked[0].body["max_tokens"] == 4);
    CHECK(asked[1].body["max_tokens"].get<int>() > 1000);

    Heard second;
    llm.chat(kAsk, judge, second.on_delta());
    CHECK(second.text == "YES");
    asked = api.asked();
    REQUIRE(asked.size() == 3);
    CHECK(asked[2].body["max_tokens"].get<int>() > 1000);
}

TEST_CASE("a refusal of the token limit's name is met by the other name, once, and remembered") {
    FakeApi api([](const FakeApi::Asked& asked, httplib::Response& response) {
        if (asked.body.contains("max_tokens")) {
            FakeApi::refuse(response, 400,
                            {{"message", "Unsupported parameter: 'max_tokens' is not supported with this model. Use 'max_completion_tokens' instead."},
                             {"param", "max_tokens"}});
            return;
        }
        if (asked.body.contains("top_p")) {
            FakeApi::refuse(response, 400, {{"message", "Unsupported parameter: 'top_p'"}, {"param", "top_p"}});
            return;
        }
        FakeApi::stream(response, {said("Well met."), stop_chunk(), kDone});
    });
    OpenAiLlm llm(settings_for(api));
    Heard heard;
    llm.chat(kAsk, Options{}, heard.on_delta());
    CHECK(heard.text == "Well met.");
    // Asked three times: as usual, with the limit renamed, and without top_p.
    std::vector<FakeApi::Asked> asked = api.asked();
    REQUIRE(asked.size() == 3);
    CHECK(asked[2].body.contains("max_completion_tokens"));
    CHECK_FALSE(asked[2].body.contains("top_p"));
    CHECK(asked[2].body.contains("temperature"));

    // The next turn asks the way that worked, at once.
    Heard again;
    llm.chat(kAsk, Options{}, again.on_delta());
    CHECK(again.text == "Well met.");
    CHECK(api.asked().size() == 4);
}

TEST_CASE("a refusal this cannot meet is reported, not retried for ever") {
    FakeApi api([](const FakeApi::Asked&, httplib::Response& response) {
        FakeApi::refuse(response, 400, {{"message", "max_tokens and max_completion_tokens are both wrong today"}});
    });
    OpenAiLlm llm(settings_for(api));
    Heard heard;
    try {
        llm.chat(kAsk, Options{}, heard.on_delta());
        FAIL("no error");
    } catch (const ApiError& why) {
        CHECK(why.kind() == ApiError::Kind::Refused);
        CHECK(why.status() == 400);
        CHECK(std::string(why.what()).find("both wrong today") != std::string::npos);
    }
    // The first, and the one go with the other name.
    CHECK(api.asked().size() == 2);
}

TEST_CASE("the statuses a player meets come back as their own kinds") {
    std::atomic<int> status{401};
    FakeApi api([&](const FakeApi::Asked&, httplib::Response& response) {
        FakeApi::refuse(response, status, {{"message", "no"}});
    });
    OpenAiLlm llm(settings_for(api));
    const std::pair<int, ApiError::Kind> cases[] = {
        {401, ApiError::Kind::Key},      {403, ApiError::Kind::Forbidden}, {404, ApiError::Kind::NotFound},
        {429, ApiError::Kind::Limit},    {402, ApiError::Kind::Limit},     {500, ApiError::Kind::Server},
        {503, ApiError::Kind::Server},
    };
    for (const auto& [code, kind] : cases) {
        status = code;
        Heard heard;
        CAPTURE(code);
        try {
            llm.chat(kAsk, Options{}, heard.on_delta());
            FAIL("no error");
        } catch (const ApiError& why) {
            CHECK(why.kind() == kind);
            CHECK(why.status() == code);
            CHECK(std::string(why.what()).find("sk-test") == std::string::npos);
        }
        CHECK(heard.text.empty());
    }
}

TEST_CASE("nothing listening is a network error that names the host") {
    // A port that was free a moment ago and has nobody on it now.
    std::string url;
    {
        FakeApi gone([](const FakeApi::Asked&, httplib::Response&) {});
        url = gone.url();
    }
    OpenAiLlm llm({url, "sk-test", "m", ""});
    Heard heard;
    try {
        llm.chat(kAsk, Options{}, heard.on_delta());
        FAIL("no error");
    } catch (const ApiError& why) {
        CHECK(why.kind() == ApiError::Kind::Network);
        CHECK(std::string(why.what()).find("Could not reach 127.0.0.1") != std::string::npos);
    }
}

TEST_CASE("an error sent as an event after the stream began is an error") {
    FakeApi api([](const FakeApi::Asked&, httplib::Response& response) {
        FakeApi::stream(response, {said("I was about to"), event({{"error", {{"message", "Provider returned error"}, {"code", 429}}}})});
    });
    OpenAiLlm llm(settings_for(api));
    Heard heard;
    try {
        llm.chat(kAsk, Options{}, heard.on_delta());
        FAIL("no error");
    } catch (const ApiError& why) {
        CHECK(why.kind() == ApiError::Kind::Limit);
    }
}

TEST_CASE("a server that answers whole instead of streaming is still heard") {
    FakeApi api([](const FakeApi::Asked&, httplib::Response& response) {
        response.set_content(json{{"choices", json::array({json{{"message", {{"role", "assistant"}, {"content", "<think>hm</think>Fine weather."}}},
                                                                {"finish_reason", "stop"}}})},
                                  {"usage", {{"prompt_tokens", 50}, {"completion_tokens", 4}}}}
                                 .dump(),
                             "application/json");
    });
    OpenAiLlm llm(settings_for(api));
    Heard heard;
    const Timing timing = llm.chat(kAsk, Options{}, heard.on_delta());
    CHECK(heard.text == "Fine weather.");
    CHECK(timing.prompt_tokens == 50);
    CHECK(timing.reply_tokens == 4);
}

TEST_CASE("an answer that is no API's is said to be one") {
    FakeApi api([](const FakeApi::Asked&, httplib::Response& response) {
        response.set_content("<html><body>Welcome to nginx</body></html>", "text/html");
    });
    OpenAiLlm llm(settings_for(api));
    Heard heard;
    try {
        llm.chat(kAsk, Options{}, heard.on_delta());
        FAIL("no error");
    } catch (const ApiError& why) {
        CHECK(why.kind() == ApiError::Kind::Protocol);
    }
}

TEST_CASE("a caller who throws out of a piece stops the reply there, promptly") {
    FakeApi api([&](const FakeApi::Asked&, httplib::Response& response) {
        // A reply that would go on for ten seconds, if anybody stayed to hear it.
        response.set_chunked_content_provider("text/event-stream", [&](std::size_t, httplib::DataSink& sink) {
            for (int i = 0; i < 2000; ++i) {
                const std::string piece = said("word ");
                if (!sink.write(piece.data(), piece.size())) return false;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            sink.done();
            return true;
        });
    });
    OpenAiLlm llm(settings_for(api));
    struct Enough {};
    int pieces = 0;
    const auto began = std::chrono::steady_clock::now();
    CHECK_THROWS_AS(llm.chat(kAsk, Options{},
                             [&](std::string_view piece) {
                                 if (!piece.empty() && ++pieces == 3) throw Enough{};
                             }),
                    Enough);
    const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
    CHECK(pieces == 3);
    CHECK(took < 2.0);

    // And the next turn is asked on a fresh connection, as if nothing had happened.
    std::string after;
    CHECK_THROWS_AS(llm.chat(kAsk, Options{},
                             [&](std::string_view piece) {
                                 after.append(piece);
                                 if (after.size() >= 20) throw Enough{};
                             }),
                    Enough);
    CHECK(after == "word word word word ");
}

TEST_CASE("a caller can stop before the server has said anything at all") {
    std::mutex lock;
    std::condition_variable woke;
    bool release = false;
    FakeApi api([&](const FakeApi::Asked&, httplib::Response& response) {
        // A server thinking it over, for as long as the test lets it.
        std::unique_lock<std::mutex> held(lock);
        woke.wait_for(held, std::chrono::seconds(20), [&] { return release; });
        FakeApi::stream(response, {said("Too late."), stop_chunk(), kDone});
    });
    OpenAiLlm llm(settings_for(api));
    struct Enough {};
    int beats = 0;
    const auto began = std::chrono::steady_clock::now();
    CHECK_THROWS_AS(llm.chat(kAsk, Options{},
                             [&](std::string_view piece) {
                                 // The empty piece is the chance to stop (llm.h).
                                 CHECK(piece.empty());
                                 if (++beats == 2) throw Enough{};
                             }),
                    Enough);
    const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
    CHECK(took < 2.0);
    {
        std::lock_guard<std::mutex> held(lock);
        release = true;
    }
    woke.notify_all();
}

TEST_CASE("the probe asks one short question and returns the answer") {
    FakeApi api([](const FakeApi::Asked&, httplib::Response& response) {
        FakeApi::stream(response, {said(" ready\n"), stop_chunk(), kDone});
    });
    OpenAiLlm llm(settings_for(api));
    CHECK(llm.probe() == "ready");
    const std::vector<FakeApi::Asked> asked = api.asked();
    REQUIRE(asked.size() == 1);
    CHECK(asked[0].body["messages"].size() == 1);
    CHECK(asked[0].body["max_tokens"].get<int>() <= 32);
}

TEST_CASE("settings that cannot be used are refused before anything is sent") {
    CHECK_THROWS_AS(OpenAiLlm({"", "k", "m", ""}), ApiError);
    CHECK_THROWS_AS(OpenAiLlm({"https://example.com/v1", "k", "  ", ""}), ApiError);
    CHECK_THROWS_AS(OpenAiLlm({"https://example.com/v1", "k", "m", "not json"}), ApiError);
}

TEST_CASE("a wrong key is a wrong key however the server numbers it") {
    // Gemini's compatibility endpoint, as it answered on 16 Nov 2026: a 400, and the usual
    // object wrapped in an array.
    const ApiAddress address = parse_api_url("https://generativelanguage.googleapis.com/v1beta/openai");
    const ApiError error = api_error(
        400, R"([{"error":{"code":400,"message":"Please pass a valid API key","status":"INVALID_ARGUMENT"}}])",
        address, "gemini-2.5-flash", "k");
    // (A key of one letter is not scrubbed out of the message: it would take every k.)
    CHECK(error.kind() == ApiError::Kind::Key);
    CHECK(error.status() == 400);
    CHECK(std::string(error.what()).find("did not accept the API key (HTTP 400)") != std::string::npos);
    // A page that is not an API at all answers a POST with "method not allowed".
    CHECK(api_error(405, "", address, "m", "k").kind() == ApiError::Kind::NotFound);
}
