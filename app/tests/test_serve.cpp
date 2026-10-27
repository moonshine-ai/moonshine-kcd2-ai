// What the playground endpoint decides, tested without a socket or a model.
//
// The httplib wiring in serve.cpp is not tested here; it is exercised end to end by holding a
// conversation in the playground, which is the only thing that proves a browser can read the
// stream. What is here is the three decisions that shape a reply and are easy to get quietly
// wrong: which messages a body yields, which sampling a request gets, and whether the final
// chunk tells the truth about what the cache reused.

#include "doctest.h"
#include "json.hpp"
#include "serve.h"

using namespace moonshine;
using json = nlohmann::json;

TEST_CASE("a body with messages yields them in order") {
    const json body = {{"messages", json::array({
                                        json{{"role", "system"}, {"content", "you are Bartel"}},
                                        json{{"role", "user"}, {"content", "good day"}},
                                    })}};
    const std::vector<Message> messages = messages_from(body);
    REQUIRE(messages.size() == 2);
    CHECK(messages[0].role == "system");
    CHECK(messages[0].content == "you are Bartel");
    CHECK(messages[1].role == "user");
    CHECK(messages[1].content == "good day");
}

TEST_CASE("a body with no messages is refused, naming what was missing") {
    CHECK_THROWS_WITH_AS(messages_from(json::object()), "the request needs a messages array",
                         std::runtime_error);
    CHECK_THROWS_AS(messages_from(json{{"messages", "good day"}}), std::runtime_error);
    CHECK_THROWS_WITH_AS(messages_from(json{{"messages", json::array()}}),
                         "the messages array was empty", std::runtime_error);
}

TEST_CASE("a message missing a role or content is refused rather than filled in") {
    // Filling in a default role would put the turn in the wrong voice and still answer, which
    // is worse than not answering: the reply would look fine and be built from the wrong prompt.
    CHECK_THROWS_AS(messages_from(json{{"messages", json::array({json{{"content", "hm"}}})}}),
                    std::runtime_error);
    CHECK_THROWS_AS(messages_from(json{{"messages", json::array({json{{"role", "user"}}})}}),
                    std::runtime_error);
}

TEST_CASE("sampling not asked for keeps the app's own") {
    const Options defaults{.model = "qwen3:4b-instruct",
                           .reply_tokens = 120,
                           .temperature = 1.0,
                           .top_p = 0.95,
                           .top_k = 20};
    const Options got = options_from(json{{"messages", json::array()}}, defaults);
    CHECK(got.temperature == doctest::Approx(1.0));
    CHECK(got.top_p == doctest::Approx(0.95));
    CHECK(got.top_k == 20);
    CHECK(got.reply_tokens == 120);
}

TEST_CASE("one field asked for changes only that field") {
    // The reason the playground exists: varying the temperature alone should leave top-p at the
    // app's value, not at a second set of defaults written in the server that could drift.
    const Options defaults{.temperature = 1.0, .top_p = 0.95, .top_k = 20};
    const Options got =
        options_from(json{{"options", json{{"temperature", 0.2}}}}, defaults);
    CHECK(got.temperature == doctest::Approx(0.2));
    CHECK(got.top_p == doctest::Approx(0.95));
    CHECK(got.top_k == 20);
}

TEST_CASE("the context size is the loaded one whatever is asked for") {
    // It was fixed when the model was loaded and cannot be changed without dropping the cache,
    // which is the one thing this endpoint exists to demonstrate.
    const Options defaults{.context_tokens = 4096};
    const Options got = options_from(json{{"options", json{{"num_ctx", 512}}}}, defaults);
    CHECK(got.context_tokens == 4096);
}

TEST_CASE("the final chunk reports cached tokens when the backend counted them") {
    const Timing timing{.prompt_tokens = 978,
                        .cached_tokens = 835,
                        .prefill_seconds = 0.41,
                        .reply_tokens = 25,
                        .decode_seconds = 1.1};
    const json done = final_chunk("qwen3:4b-instruct", timing, 1.6);
    CHECK(done["done"] == true);
    CHECK(done["prompt_eval_count"] == 978);
    CHECK(done["cached_eval_count"] == 835);
    CHECK(done["eval_count"] == 25);
    // Nanoseconds, which is what this shape means by a duration.
    CHECK(done["total_duration"] == 1600000000);
    CHECK(done["eval_duration"] == 1100000000);
}

TEST_CASE("nothing reused is reported as nothing reused, not as silence") {
    const Timing timing{.prompt_tokens = 848, .cached_tokens = 0};
    const json done = final_chunk("qwen3:4b-instruct", timing, 7.0);
    REQUIRE(done.contains("cached_eval_count"));
    CHECK(done["cached_eval_count"] == 0);
}

TEST_CASE("the whole-reply shape is the final chunk with the words put back in") {
    // What a `stream: false` request gets. Built from final_chunk rather than assembled
    // separately, so the counts cannot disagree between the two ways of asking.
    const Timing timing{.prompt_tokens = 40, .cached_tokens = 4, .reply_tokens = 22};
    json whole = final_chunk("qwen3:4b-instruct", timing, 1.1);
    whole["message"]["content"] = "My name is Bartel.";
    CHECK(whole["message"]["content"] == "My name is Bartel.");
    CHECK(whole["message"]["role"] == "assistant");
    CHECK(whole["eval_count"] == 22);
    CHECK(whole["cached_eval_count"] == 4);
    CHECK(whole["done"] == true);
}

TEST_CASE("a backend that will not say leaves the field out rather than claiming zero") {
    // Sent as 0 it would read as "the cache did nothing", which is a measurement. Left out it
    // reads as "nobody knows", which is the truth, and is what the panel shows a plain count for.
    const Timing timing{.prompt_tokens = 848};
    REQUIRE(timing.cached_tokens == -1);
    CHECK_FALSE(final_chunk("qwen3:4b-instruct", timing, 7.0).contains("cached_eval_count"));
}
