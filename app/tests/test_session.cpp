#include "doctest.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <thread>

#include "fake_game.h"
#include "gamelog.h"
#include "session.h"
#include "stub_llm.h"

using namespace moonshine;

namespace {

// Real soul ids, because that is what the mod logs. Reading that field as a display
// name is what handed a Kuttenberg varlet a beggar's character sheet.
const std::string kBartel = "kkut_additive_man_47";
const std::string kStranger = "kkut_not_in_the_database";
const std::string kWorld = "Bohemia, 1403. The king is taken and the country is at war.";

// A conversation with the game faked and the model scripted, which between them
// cover everything the turn accounting depends on.
struct Fixture {
    FakeGame game;
    StubLlm llm;
    TurnWriter writer{game.paths()};
    Options options;
    Prefiller prefiller{llm, options};
    Session session{llm, writer, prefiller, options};
    std::vector<std::string> spoken;
    std::vector<std::string> statuses;

    Fixture() {
        session.set_world(kWorld);
        // Only the finished reply reaches the fake game, so the tests about turns are not
        // at the mercy of how long the stub takes between chunks. The streaming tests set
        // this to zero themselves.
        session.set_stream_interval(std::chrono::hours(1));
        // Looked up by soul id and answering with a person, as PersonaDb does. Any
        // other id is somebody the database has never heard of.
        session.set_persona_source(
            [](std::string_view soul, const Approach&, std::string_view,
               const SoulRecord&) -> std::optional<Persona> {
                if (soul != kBartel) return std::nullopt;
                return Persona{"Bartel", "Bartel is a varlet in Kuttenberg."};
            });
        Listener listener;
        listener.on_say = [this](std::string_view piece) { spoken.emplace_back(piece); };
        listener.on_status = [this](std::string_view text) { statuses.emplace_back(text); };
        session.set_listener(std::move(listener));
    }

    // Everything the mod has printed since the last drain, handed to the session.
    void drain(LogTailer& tailer) {
        for (const Event& event : tailer.poll()) session.handle(event);
    }

    // A persona who has an opening line written for them already.
    void with_greeting(std::string words) {
        session.set_persona_source(
            [said = std::move(words)](std::string_view soul, const Approach&, std::string_view,
                                      const SoulRecord&) -> std::optional<Persona> {
                if (soul != kBartel) return std::nullopt;
                return Persona{"Bartel", "Bartel is a varlet in Kuttenberg.", said};
            });
    }
};

}  // namespace

TEST_CASE("a greeting written ahead of time is said without waiting for the model") {
    Fixture f;
    f.with_greeting("What is it? Make it quick, I have water to draw.");
    f.session.handle({Marker::Begin, 0, kBartel});

    // Generating the opening line means prefilling the world and the sheet before a word
    // can be spoken, which cost 15.15s in the first real conversation. This is the fix,
    // so the test has to prove the words were not asked for: the begin asks the model
    // nothing at all. The prefill the player's answer will need is left as speculation
    // for the poller to do between looks at the log (since 12 Oct 2026, when doing it on
    // the begin held the voice's cue for the length of the read), and that one call asks
    // for no reply.
    CHECK(f.game.play() == "What is it? Make it quick, I have water to draw.");
    REQUIRE(f.llm.calls.empty());
    REQUIRE(f.session.has_speculation());
    f.session.speculate([] { return false; });
    REQUIRE(f.llm.calls.size() == 1);
    CHECK(f.llm.calls.back().options.reply_tokens <= 1);

    // And the next turn answers as the person who said it, rather than starting over.
    f.llm.replies.clear();
    f.llm.replies.push_back("The well, over yonder. Mind the mud.");
    f.session.say("Where can I find water?");
    CHECK(f.game.play() == "The well, over yonder. Mind the mud.");

    const std::vector<Message>& sent = f.llm.last().messages;
    const auto greeting = std::find_if(sent.begin(), sent.end(), [](const Message& message) {
        return message.content.find("I have water to draw") != std::string::npos;
    });
    REQUIRE(greeting != sent.end());
    CHECK(greeting->role == "assistant");
}

TEST_CASE("the NPC speaks first, so the player is not left to open it") {
    Fixture f;
    f.llm.replies.push_back("God be with you, traveller. What brings you to Rattay?");
    f.session.handle({Marker::Begin, 0, kBartel});

    // Written, and written where the mod looks, before the player has said a word.
    CHECK(f.game.play() == "God be with you, traveller. What brings you to Rattay?");
    CHECK(f.session.npc() == "Bartel");
}

TEST_CASE("a turn is answered only once the one before it has been heard") {
    Fixture f;
    LogTailer tailer(f.game.log());
    f.llm.replies.push_back("God be with you.");
    f.llm.replies.push_back("Rattay is a day's ride east.");

    f.game.begin(kBartel);
    f.drain(tailer);
    REQUIRE(f.game.play() == "God be with you.");
    f.drain(tailer);
    CHECK(f.session.played() == 1);

    f.session.say("Which way to Rattay?");
    // The second turn is turn 2 because the first was played, not because this app
    // wrote a first one.
    CHECK(f.game.play() == "Rattay is a day's ride east.");
    f.drain(tailer);
    CHECK(f.session.played() == 2);
}

TEST_CASE("a line typed in the game is answered like one typed at the app") {
    Fixture f;
    LogTailer tailer(f.game.log());
    f.llm.replies.push_back("God be with you.");
    f.llm.replies.push_back("Rattay is a day's ride east.");

    f.game.begin(kBartel);
    f.drain(tailer);
    REQUIRE(f.game.play() == "God be with you.");
    f.drain(tailer);

    // The mod's in-game keyboard has no way out but the log, so the player's words come
    // down the same file as every other marker and are answered from there, with no
    // hand on this side's stdin at all.
    f.game.typed("Which way to Rattay?");
    f.drain(tailer);
    CHECK(f.game.play() == "Rattay is a day's ride east.");

    // And the model was asked about what was typed, not about a blank.
    const std::vector<Message>& sent = f.llm.last().messages;
    const auto asked = std::find_if(sent.begin(), sent.end(), [](const Message& message) {
        return message.role == "user" && message.content.find("Which way to Rattay?") != std::string::npos;
    });
    CHECK(asked != sent.end());
}

TEST_CASE("a line that asks after Jews or Romani is turned aside without the model") {
    Fixture f;
    f.with_greeting("What is it?");
    f.session.handle({Marker::Begin, 0, kBartel});
    REQUIRE(f.game.play() == "What is it?");
    const std::size_t calls = f.llm.calls.size();
    f.llm.replies.push_back("Nobody should say this.");

    f.session.say("What do you think of the Jews?");
    const std::optional<std::string> turned = f.game.play();
    REQUIRE(turned.has_value());
    CHECK(turned->find("Nobody should say this") == std::string::npos);
    CHECK(f.llm.calls.size() == calls);
    const auto told = std::find_if(f.statuses.begin(), f.statuses.end(), [](const std::string& s) {
        return s.find("turned aside as written") != std::string::npos;
    });
    CHECK(told != f.statuses.end());
    // In the history as asked and answered, so the model goes on from it next turn.
    REQUIRE(f.session.history().size() >= 3);
    CHECK(f.session.history()[1].content == "What do you think of the Jews?");
    CHECK(f.session.history()[2].content == *turned);

    // The forms and the other people too; the town's own street is a direction and passes.
    f.session.say("Are the gypsies at the camp thieves?");
    CHECK(f.llm.calls.size() == calls);
    CHECK(f.game.play().has_value());
    f.session.say("Tell me about the Romani.");
    CHECK(f.llm.calls.size() == calls);
    CHECK(f.game.play().has_value());
    f.session.say("Where is Jewish Street?");
    CHECK(f.llm.calls.size() == calls + 1);
    CHECK(f.game.play() == "Nobody should say this.");
    f.llm.replies.push_back("A fine ring, that.");
    f.session.say("I bought a jewel in Rome from a Roman.");
    CHECK(f.llm.calls.size() == calls + 2);
    CHECK(f.game.play() == "A fine ring, that.");
}

TEST_CASE("the say row opens the ear, and what it hears goes to the slot as hooks") {
    Fixture f;
    LogTailer tailer(f.game.log());
    std::vector<bool> listens;
    {
        Listener listener;
        listener.on_say = [&f](std::string_view piece) { f.spoken.emplace_back(piece); };
        listener.on_status = [&f](std::string_view text) { f.statuses.emplace_back(text); };
        listener.on_listen = [&listens](bool on) { listens.push_back(on); };
        f.session.set_listener(std::move(listener));
    }
    f.llm.replies.push_back("God be with you.");
    f.llm.replies.push_back("The mill is down by the river.");
    f.game.begin(kBartel);
    f.drain(tailer);
    REQUIRE(f.game.play() == "God be with you.");
    f.drain(tailer);

    // Nothing heard before the row is live reaches the game.
    f.session.heard("where");
    CHECK(f.game.hook(1).empty());
    CHECK_FALSE(f.session.listening());

    // The player picked the say row: the mod logs the pick with no words, then the start.
    f.game.say("<12:04:41> [moonshine_names] BRIDGE picked 4 say -- ");
    f.game.say("<12:04:41> [moonshine_names] BRIDGE say start");
    f.drain(tailer);
    CHECK(f.session.listening());
    REQUIRE(listens.size() == 1);
    CHECK(listens.back());
    // No guessing at the rows while the ear has the cores.
    CHECK_FALSE(f.session.has_speculation());

    // The words so far go to the slot, escaped for Lua, and only as they change: the
    // same words again are not a hook, and nothing is sent again within the mod's beat.
    f.session.heard("where is");
    CHECK(f.game.hook(1) == "Moonshine.SayHeard(\"where is\")\n");
    f.session.heard("where is");
    CHECK(f.game.hook(2).empty());
    f.session.heard("where is the \"miller\"");
    CHECK(f.game.hook(2).empty());  // too soon after the last
    std::this_thread::sleep_for(std::chrono::milliseconds(450));
    f.session.heard("where is the \"miller\"");
    CHECK(f.game.hook(2) == "Moonshine.SayHeard(\"where is the \\\"miller\\\"\")\n");

    // The player's A sent it: the pick carries the words, the stop closes the ear, and
    // the said line that follows is answered as a typed line, judge and all -- the say
    // intent is nobody's row.
    f.game.say("<12:04:45> [moonshine_names] BRIDGE picked 1 say -- Where is the miller?");
    f.game.say("<12:04:45> [moonshine_names] BRIDGE say stop -- sent");
    f.game.typed("Where is the miller?");
    f.drain(tailer);
    CHECK_FALSE(f.session.listening());
    REQUIRE(listens.size() == 2);
    CHECK_FALSE(listens.back());
    CHECK(f.game.play() == "The mill is down by the river.");
    const std::vector<Message>& sent = f.llm.last().messages;
    const auto asked = std::find_if(sent.begin(), sent.end(), [](const Message& message) {
        return message.role == "user" && message.content.find("Where is the miller?") != std::string::npos;
    });
    CHECK(asked != sent.end());
    const auto picked = std::find_if(f.statuses.begin(), f.statuses.end(), [](const std::string& s) {
        return s.find("picked as") != std::string::npos;
    });
    CHECK(picked == f.statuses.end());

    // Heard after the stop is nothing; a second stop is nothing.
    std::this_thread::sleep_for(std::chrono::milliseconds(450));
    f.session.heard("more words");
    CHECK(f.game.hook(3).empty());
    f.game.say("<12:04:46> BRIDGE say stop -- back");
    f.drain(tailer);
    CHECK(listens.size() == 2);
}

TEST_CASE("the conversation ending closes the ear") {
    Fixture f;
    std::vector<bool> listens;
    {
        Listener listener;
        listener.on_listen = [&listens](bool on) { listens.push_back(on); };
        f.session.set_listener(std::move(listener));
    }
    f.with_greeting("What is it?");
    f.session.handle({Marker::Begin, 0, kBartel});
    f.session.handle({Marker::SayStart, 0, {}});
    REQUIRE(f.session.listening());
    f.session.handle({Marker::Left, 0, {}});
    CHECK_FALSE(f.session.listening());
    REQUIRE(listens.size() == 2);
    CHECK_FALSE(listens.back());
    // And a start with nobody to talk to is not a start.
    f.session.handle({Marker::SayStart, 0, {}});
    CHECK_FALSE(f.session.listening());
    CHECK(listens.size() == 2);
}

TEST_CASE("a line typed at a closed conversation is reported, not answered") {
    Fixture f;
    LogTailer tailer(f.game.log());
    f.llm.replies.push_back("Nobody should say this.");

    f.game.typed("Hello?");
    f.drain(tailer);
    CHECK(f.llm.calls.empty());
    CHECK_FALSE(f.game.play().has_value());
    const auto told = std::find_if(f.statuses.begin(), f.statuses.end(), [](const std::string& s) {
        return s.find("nobody is listening") != std::string::npos;
    });
    CHECK(told != f.statuses.end());
}

TEST_CASE("the turn counter carries across conversations, because file names must") {
    Fixture f;
    LogTailer tailer(f.game.log());
    f.llm.replies = {"God be with you.", "Fare well.", "What do you want?"};

    f.game.begin(kBartel);
    f.drain(tailer);
    f.game.play();
    f.drain(tailer);
    f.session.say("Good day.");
    f.game.play();
    f.drain(tailer);
    REQUIRE(f.session.played() == 2);

    f.game.left();
    f.drain(tailer);
    CHECK_FALSE(f.session.talking());

    // The next person starts at turn 3. The engine caches what it reads by path, so
    // starting again at one would hand them words already spoken.
    f.game.begin(kStranger);
    f.drain(tailer);
    CHECK(f.game.has_turn(3));
    CHECK(f.game.play() == "What do you want?");
}

TEST_CASE("a new conversation does not inherit the last one's history") {
    Fixture f;
    f.llm.replies = {"God be with you.", "A fine day.", "What do you want?"};
    f.session.handle({Marker::Begin, 0, kBartel});
    f.game.play();
    f.session.handle({Marker::Played, 1, {}});
    f.session.say("Good day to you.");
    REQUIRE(f.session.history().size() == 3);

    f.session.handle({Marker::Left, 0, {}});
    f.session.handle({Marker::Begin, 0, kStranger});

    // The greeting only, and no trace of the previous conversation in the prompt.
    CHECK(f.session.history().size() == 1);
    for (const Message& message : f.llm.last().messages) {
        CHECK(message.content.find("Good day to you.") == std::string::npos);
    }
}

TEST_CASE("somebody the database does not know is still talked to") {
    Fixture f;
    f.llm.replies.push_back("What do you want?");
    // A save from before the generator ran has souls with no sheet. Standing mute
    // in front of them is worse than being vague.
    f.session.handle({Marker::Begin, 0, kStranger});
    CHECK(f.session.npc() == "Stranger");
    CHECK(f.game.play() == "What do you want?");
}

TEST_CASE("a conversation with nobody named still opens") {
    Fixture f;
    f.llm.replies.push_back("Hm?");
    f.session.handle({Marker::Begin, 0, ""});
    CHECK(f.game.play() == "Hm?");
}

TEST_CASE("the opening line is told that somebody has walked up") {
    Fixture f;
    f.llm.replies.push_back("God be with you, traveller.");
    f.session.handle({Marker::Begin, 0, kBartel});

    // A blank turn is not a prompt. Handed one, the model free-associates from the
    // sheet and greets somebody who is not there.
    const Message& player = f.llm.last().messages.back();
    CHECK(player.role == "user");
    CHECK_FALSE(player.content.empty());
    CHECK(player.content == std::string(kApproached));

    // And the direction is not carried in the history as though the player said it.
    REQUIRE(f.session.history().size() == 1);
    CHECK(f.session.history()[0].role == "assistant");
}

TEST_CASE("the status line says what the world was, and says nothing when it was not read") {
    // The only place in play that shows whether the mod managed to read the clock at all. An
    // hour nobody read is not mentioned, and that is indistinguishable from a dry morning
    // nobody remarked on -- so without this line a mod a lap behind looks exactly like fine
    // weather, and the prompt quietly loses a sentence.
    Fixture f;
    f.llm.replies.push_back("God be with you.");
    f.session.handle({Marker::Begin, 0, kBartel, "none", {.hour = 19.5, .rain = 0.8}});
    REQUIRE_FALSE(f.statuses.empty());
    CHECK(f.statuses[0] == "talking to Bartel (kkut_additive_man_47), evening, pouring with rain");

    Fixture silent;
    silent.llm.replies.push_back("God be with you.");
    silent.session.handle({Marker::Begin, 0, kBartel});
    REQUIRE_FALSE(silent.statuses.empty());
    CHECK(silent.statuses[0] == "talking to Bartel (kkut_additive_man_47)");
}

TEST_CASE("a turn is taken off disk once the game has said it") {
    Fixture f;
    f.llm.replies.push_back("God be with you.");
    f.session.handle({Marker::Begin, 0, kBartel});
    REQUIRE(f.game.has_turn(1));

    f.session.handle({Marker::Played, 1, {}});
    // The engine has the words cached by now, so the file is only a hazard: after a
    // relaunch the mod counts from one again and would find this and say it.
    CHECK_FALSE(f.game.has_turn(1));
}

TEST_CASE("a restart resets the count and clears what was left on disk") {
    Fixture f;
    LogTailer tailer(f.game.log());
    f.llm.replies = {"God be with you.", "Fare well.", "Good morrow."};

    f.game.begin(kBartel);
    f.drain(tailer);
    f.game.play();
    f.drain(tailer);
    f.session.say("Good day.");
    f.drain(tailer);
    REQUIRE(f.game.has_turn(2));

    f.game.launch();
    f.game.begin(kBartel);
    f.drain(tailer);

    // The mod counts from one again after a reload, so this has to as well, and
    // turn 2 from the last run must not be sitting there to be walked into.
    CHECK_FALSE(f.game.has_turn(2));
    CHECK(f.game.play() == "Good morrow.");
}

TEST_CASE("the world text comes before the character sheet, so the prefix is shared") {
    Fixture f;
    f.llm.replies.push_back("God be with you.");
    f.session.handle({Marker::Begin, 0, kBartel});

    const std::string& system = StubLlm::system_of(f.llm.last()).content;
    const std::size_t world = system.find("Bohemia, 1403");
    const std::size_t sheet = system.find("Bartel is a varlet");
    REQUIRE(world != std::string::npos);
    REQUIRE(sheet != std::string::npos);
    CHECK(world < sheet);
}

TEST_CASE("the player's silence is not a turn") {
    Fixture f;
    f.llm.replies.push_back("God be with you.");
    f.session.handle({Marker::Begin, 0, kBartel});
    f.game.play();
    f.session.handle({Marker::Played, 1, {}});

    const std::size_t asked = f.llm.calls.size();
    // A failed transcription and a player saying nothing look the same from here,
    // and neither should spend a turn.
    f.session.say("");
    CHECK(f.llm.calls.size() == asked);
    CHECK_FALSE(f.game.has_turn(2));
}

TEST_CASE("nothing is said to a conversation that is not open") {
    Fixture f;
    f.llm.replies.push_back("Nobody asked.");
    f.session.say("Hello?");
    CHECK(f.llm.calls.empty());
    CHECK_FALSE(f.game.has_turn(1));
}

TEST_CASE("the reply is handed over in speakable pieces as it streams") {
    Fixture f;
    f.llm.replies.push_back(
        "God be with you, traveller. Rattay is a day's ride east of here. "
        "Mind the road after dark.");
    f.session.handle({Marker::Begin, 0, kBartel});

    // More than one piece, so the voice starts before the reply is finished, and
    // every piece is long enough not to run the audio dry.
    REQUIRE(f.spoken.size() > 1);
    for (const std::string& piece : f.spoken) CHECK_FALSE(piece.empty());

    // The game, with updates held back, gets the whole reply as one turn.
    const std::optional<std::string> said = f.game.play();
    REQUIRE(said.has_value());
    CHECK(*said ==
          "God be with you, traveller. Rattay is a day's ride east of here. "
          "Mind the road after dark.");
    CHECK_FALSE(f.game.play().has_value());
}

TEST_CASE("the reply reaches the screen as it is written, not when it is finished") {
    Fixture f;
    f.session.set_stream_interval(std::chrono::milliseconds(0));
    f.llm.chunk = 4;
    f.llm.replies.push_back(" God be with you, traveller.");
    f.session.handle({Marker::Begin, 0, kBartel});

    // A turn per update, each the text so far and each but the last flagged @more, so
    // the mod replaces the line rather than starting another and keeps the keyboard shut.
    // The model's opening blank never reaches the screen.
    std::vector<std::string> shown;
    while (const std::optional<std::string> update = f.game.play()) shown.push_back(*update);
    REQUIRE(shown.size() > 2);
    CHECK(shown.front() == "God");
    CHECK(shown.back() == "God be with you, traveller.");
    for (std::size_t i = 0; i < shown.size(); ++i) {
        const int turn = static_cast<int>(i) + 1;
        const bool last = i + 1 == shown.size();
        CHECK(f.game.whole(turn).rfind("@more ", 0) == (last ? std::string::npos : 0));
        // Each update carries everything before it.
        if (i > 0) CHECK(shown[i].rfind(shown[i - 1], 0) == 0);
    }

    // The voice was still given the reply in one speakable piece, and the history holds
    // it whole.
    REQUIRE(f.spoken.size() == 1);
    CHECK(f.spoken.front() == "God be with you, traveller.");
    REQUIRE(f.session.history().size() == 1);
    CHECK(f.session.history().back().content == "God be with you, traveller.");
}

TEST_CASE("a reply that opens by repeating the last line loses the repeat, and never shows it") {
    Fixture f;
    f.session.set_stream_interval(std::chrono::milliseconds(0));
    f.llm.chunk = 5;
    f.with_greeting("What do you want?");
    f.session.handle({Marker::Begin, 0, kBartel});
    CHECK(f.game.play() == "What do you want?");

    // What qwen3:4b did on every pick of the first seed: the greeting again, then the answer.
    f.llm.replies.push_back("What do you want? Julian has my axe. Fifty groschen to get it back.");
    f.session.say("Is there anything I can help you with?");
    std::vector<std::string> shown;
    while (const std::optional<std::string> update = f.game.play()) shown.push_back(*update);
    REQUIRE_FALSE(shown.empty());
    // Nothing went to the screen while the text so far could still have been the copy, and
    // what did go never began with the old line.
    for (const std::string& update : shown) {
        CHECK(update.rfind("What do you want?", 0) == std::string::npos);
        CHECK(std::string("Julian has my axe. Fifty groschen to get it back.").rfind(update, 0) == 0);
    }
    CHECK(shown.back() == "Julian has my axe. Fifty groschen to get it back.");
    CHECK(f.session.history().back().content == "Julian has my axe. Fifty groschen to get it back.");
    bool noted = false;
    for (const std::string& line : f.statuses) {
        if (line.find("repeating the last line") != std::string::npos) noted = true;
    }
    CHECK(noted);

    // The voice was never given the copy either: it spoke the greeting once, as the greeting.
    int greeted = 0;
    for (const std::string& piece : f.spoken) {
        if (piece.find("What do you want?") != std::string::npos) ++greeted;
    }
    CHECK(greeted == 1);

    // A reply that is nothing but the repeat never reaches the game; the model is asked
    // once more, a little freer, and that answer is the one said.
    const std::size_t calls = f.llm.calls.size();
    f.llm.replies.push_back("Julian has my axe. Fifty groschen to get it back.");
    f.llm.replies.push_back("Are you deaf? Julian. The axe. Fifty groschen.");
    f.session.say("Say that again?");
    shown.clear();
    while (const std::optional<std::string> update = f.game.play()) shown.push_back(*update);
    REQUIRE_FALSE(shown.empty());
    for (const std::string& update : shown) CHECK(update.find("has my axe") == std::string::npos);
    CHECK(shown.back() == "Are you deaf? Julian. The axe. Fifty groschen.");
    REQUIRE(f.llm.calls.size() >= calls + 2);
    CHECK(f.llm.calls[calls + 1].options.temperature > f.llm.calls[calls].options.temperature);
    // Told plainly the second time, in the turn and not in the history kept afterwards.
    CHECK(f.llm.calls[calls].messages.back().content == "Say that again?");
    CHECK(f.llm.calls[calls + 1].messages.back().content.find("Do not say it again") !=
          std::string::npos);
    CHECK(f.session.history()[f.session.history().size() - 2].content == "Say that again?");

    // Twice a copy, and it is said as it stands rather than leaving the game holding.
    f.llm.replies.push_back("Are you deaf? Julian. The axe. Fifty groschen.");
    f.llm.replies.push_back("Are you deaf? Julian. The axe. Fifty groschen.");
    f.session.say("Once more?");
    shown.clear();
    while (const std::optional<std::string> update = f.game.play()) shown.push_back(*update);
    REQUIRE_FALSE(shown.empty());
    CHECK(shown.back() == "Are you deaf? Julian. The axe. Fifty groschen.");
    CHECK(shown.size() == 1);  // held back throughout, then the whole line at the end
}

TEST_CASE("a reply that opens with the tail of the last line, from a sentence on, loses that too") {
    Fixture f;
    f.session.set_stream_interval(std::chrono::milliseconds(0));
    f.llm.chunk = 7;
    f.with_greeting("Hm? What is it you want, sir? The wood is still wet, and I have no time to spare.");
    f.session.handle({Marker::Begin, 0, kBartel});
    f.game.play();

    f.llm.replies.push_back("The wood is still wet, and I have no time to spare. Julian has my axe.");
    f.session.say("Is there anything I can help you with?");
    std::vector<std::string> shown;
    while (const std::optional<std::string> update = f.game.play()) shown.push_back(*update);
    REQUIRE_FALSE(shown.empty());
    for (const std::string& update : shown) CHECK(update.find("still wet") == std::string::npos);
    CHECK(shown.back() == "Julian has my axe.");

    // A short tail is not a copy: "Aye." is anyone's to say twice.
    f.llm.replies.push_back("Aye. Aye, I said.");
    f.session.say("Julian, you say?");
    f.llm.replies.push_back("Aye. Fifty groschen for it.");
    f.session.say("And the pay?");
    shown.clear();
    while (const std::optional<std::string> update = f.game.play()) shown.push_back(*update);
    REQUIRE_FALSE(shown.empty());
    CHECK(shown.back() == "Aye. Fifty groschen for it.");
}

TEST_CASE("a quest directive rides on the first update of a streamed reply, not on all") {
    // Owed commands go out with the first words to reach the game, and are then spent:
    // a directive repeated on every update would move the axe twenty times.
    Fixture f;
    f.session.set_stream_interval(std::chrono::milliseconds(0));
    f.llm.chunk = 4;
    f.llm.replies.push_back("Off you go, then, and mind the road.");
    f.session.set_persona_source(
        [](std::string_view, const Approach&, std::string_view,
           const SoulRecord&) -> std::optional<Persona> {
            Persona bartel{"Bartel", "Bartel is a varlet in Kuttenberg."};
            bartel.quest_of.id = "c00183e1da7e";
            bartel.quest_of.role = QuestRole::Giver;
            bartel.quest.opening_commands = {"complete"};
            return bartel;
        });
    f.session.handle({Marker::Begin, 0, kBartel});

    int directives = 0;
    for (int turn = 1; f.game.play().has_value(); ++turn) {
        if (f.game.whole(turn).find("@quest") != std::string::npos) ++directives;
    }
    CHECK(directives == 1);
    CHECK(f.game.whole(1).find("@quest c00183e1da7e complete") != std::string::npos);
}

TEST_CASE("a model that cannot be reached is reported, not crashed on") {
    struct Broken : Llm {
        Timing chat(const std::vector<Message>&, const Options&, const OnDelta&) override {
            throw std::runtime_error("connection refused");
        }
        std::string describe() const override { return "broken"; }
    };

    FakeGame game;
    Broken broken;
    Options options;
    TurnWriter writer(game.paths());
    Prefiller prefiller(broken, options);
    Session session(broken, writer, prefiller, options);

    std::vector<std::string> statuses;
    Listener listener;
    listener.on_status = [&](std::string_view text) { statuses.emplace_back(text); };
    session.set_listener(std::move(listener));

    session.handle({Marker::Begin, 0, kBartel});
    CHECK_FALSE(game.has_turn(1));

    bool told = false;
    for (const std::string& status : statuses) {
        if (status.find("connection refused") != std::string::npos) told = true;
    }
    CHECK(told);
}

TEST_CASE("a turn nobody could write is not remembered as said") {
    BlockedPaths blocked;
    StubLlm llm;
    Options options;
    TurnWriter writer(blocked.paths());
    Prefiller prefiller(llm, options);
    Session session(llm, writer, prefiller, options);
    llm.replies.push_back("God be with you.");
    session.handle({Marker::Begin, 0, kBartel});

    // The player heard nothing, so the model must not carry on as though it spoke.
    CHECK(session.history().empty());
}

TEST_CASE("an NPC already warmed is not warmed again on approach") {
    Fixture f;
    const Persona bartel{"Bartel", "Bartel is a varlet in Kuttenberg."};
    f.prefiller.warm_persona(kWorld, bartel);
    const std::size_t warmed = f.llm.calls.size();

    f.llm.replies.push_back("God be with you.");
    f.session.handle({Marker::Begin, 0, kBartel});

    // One call for the greeting, and no complaint about a cold open.
    CHECK(f.llm.calls.size() == warmed + 1);
    for (const std::string& status : f.statuses) {
        CHECK(status.find("opens slowly") == std::string::npos);
    }
}

// The save's record, as the mod dumps it into the log just before the begin line, and what
// the session does with it: hands it to whoever builds the persona, sends the player's words
// back for it, and folds it when it has grown.

namespace {

// A dump with two earlier conversations, one of them long enough that the two together
// run past the budget for remembered lines.
std::vector<FakeGame::Entry> grown(int words_in_first) {
    std::string wall;
    for (int i = 0; i < words_in_first; ++i) wall += "word ";
    return {
        {"gap", 100000, "rch=0.5 hour=18"},
        {"p", -1, wall},
        {"n", -1, "Enough of that."},
        {"gap", 300000, "rch=0.5 hour=9"},
        {"p", -1, "Still here?"},
        {"n", -1, "Where else."},
        {"gap", 500000, "rch=0.5 hour=12"},
    };
}

}  // namespace

TEST_CASE("the save's record reaches the persona source, whole, with the begin") {
    Fixture f;
    LogTailer tailer(f.game.log());
    SoulRecord given;
    Approach seen;
    f.session.set_persona_source(
        [&](std::string_view, const Approach& approach, std::string_view,
            const SoulRecord& record) -> std::optional<Persona> {
            given = record;
            seen = approach;
            return Persona{"Bartel", "Bartel is a varlet in Kuttenberg."};
        });
    f.llm.replies.push_back("You again.");

    f.game.dump(kBartel, 2,
                {{"gap", 1000, "rch=0.5 hour=18.5"},
                 {"p", -1, "Which way to the mill?\nAnd hurry."},
                 {"n", -1, "I said BRIDGE left, did I not?"},
                 {"gap", 90000, "rch=0.5 hour=9"}},
                "quest=c00183e1da7e accepted=true handed=false done=false");
    f.game.begin(kBartel);
    f.drain(tailer);

    CHECK(given.soul == kBartel);
    CHECK(given.met == 2);
    CHECK(given.complete);
    REQUIRE(given.memory.size() == 4);
    CHECK(given.memory[1].text == "Which way to the mill?\nAnd hurry.");
    // The escaping round-trips, so the memory of a marker is words again.
    CHECK(given.memory[2].text == "I said BRIDGE left, did I not?");
    CHECK(given.quest == "c00183e1da7e");
    CHECK(given.facts.accepted);
    // The session does not judge the record; met is for whoever builds the persona.
    CHECK_FALSE(seen.met);
    // The status line says how much came with them.
    const auto told = std::find_if(f.statuses.begin(), f.statuses.end(), [](const std::string& s) {
        return s.find("met 1 time before (3 entries remembered)") != std::string::npos;
    });
    CHECK(told != f.statuses.end());
}

TEST_CASE("a record of somebody else is not believed, and a first meeting has none") {
    Fixture f;
    LogTailer tailer(f.game.log());
    SoulRecord given;
    given.met = 99;
    f.session.set_persona_source(
        [&](std::string_view, const Approach&, std::string_view,
            const SoulRecord& record) -> std::optional<Persona> {
            given = record;
            return Persona{"Bartel", "Bartel is a varlet in Kuttenberg."};
        });
    f.llm.replies = {"Who are you?", "Who are you?"};

    // A dump for one soul followed by a begin for another: the mod did not dump for the
    // second, so the second is treated as unknown rather than given the first's memory.
    f.game.dump(kStranger, 5, {{"gap", 1000, "hour=9"}});
    f.game.begin(kBartel);
    f.drain(tailer);
    CHECK(given.empty());
    f.game.play();
    f.game.left();
    f.drain(tailer);

    // A save that has just been loaded wipes anything held from before it.
    f.game.dump(kBartel, 3, {{"gap", 1000, "hour=9"}});
    f.game.say("<12:04:31> BRIDGE state loaded via OnLoad version=1 souls=1");
    f.game.begin(kBartel);
    f.drain(tailer);
    CHECK(given.empty());
}

TEST_CASE("words the game never heard are sent to the save on the reply's turn file") {
    Fixture f;
    LogTailer tailer(f.game.log());
    f.llm.replies = {"God be with you.", "Over yonder.", "Mind the mud."};
    f.game.begin(kBartel);
    f.drain(tailer);
    REQUIRE(f.game.play() == "God be with you.");
    f.drain(tailer);

    // Typed at the app -- or heard by its microphone -- so the game has no record of it.
    f.session.say("Where is\nthe  well?");
    CHECK(f.game.play() == "Over yonder.");
    REQUIRE(f.game.remembered().size() == 1);
    CHECK(f.game.remembered()[0] == "Where is the well?");
    f.drain(tailer);

    // Typed in the game, which remembered it as it was typed: nothing to send back.
    f.game.typed("And the mill?");
    f.drain(tailer);
    CHECK(f.game.play() == "Mind the mud.");
    CHECK(f.game.remembered().size() == 1);
}

TEST_CASE("a grown record is folded after the conversation, and the fold rides the next turn") {
    Fixture f;
    LogTailer tailer(f.game.log());
    f.llm.replies = {"You again.", "Henry came to you twice: once for the way to the mill, which "
                                   "you gave him, and once to see if you were still there."};

    f.game.dump(kBartel, 3, grown(700), "", 4);
    f.game.begin(kBartel);
    f.drain(tailer);
    REQUIRE(f.game.play() == "You again.");
    f.drain(tailer);
    CHECK(f.session.pending().empty());

    // The player walks away: the fold is planned, and the poller does it between looks at
    // the log -- cold and briefly -- after which the directive waits for a turn file.
    f.game.left();
    f.drain(tailer);
    CHECK(f.llm.calls.size() == 1);
    REQUIRE(f.session.has_speculation());
    CHECK_FALSE(f.session.speculate([] { return false; }));
    CHECK_FALSE(f.session.has_speculation());
    REQUIRE(f.llm.calls.size() == 2);
    const StubLlm::Call& fold = f.llm.last();
    CHECK(fold.options.temperature == doctest::Approx(0.3));
    CHECK(fold.messages[0].content.find("Bartel") != std::string::npos);
    CHECK(fold.messages[1].content.find("You: Enough of that.") != std::string::npos);
    REQUIRE(f.session.pending().size() == 1);
    // Everything before the conversation that just happened: six entries, at revision 4.
    CHECK(f.session.pending()[0].rfind("@memory " + kBartel + " compact 4 6 ", 0) == 0);

    // The next conversation, with anybody, carries it on its first turn.
    f.llm.replies.push_back("Who are you?");
    f.game.begin(kStranger);
    f.drain(tailer);
    REQUIRE(f.game.play() == "Who are you?");
    REQUIRE(f.game.directives().size() == 1);
    CHECK(f.game.directives()[0].find("compact 4 6 ") != std::string::npos);
    CHECK(f.game.directives()[0].find("once for the way to the mill") != std::string::npos);
    CHECK(f.session.pending().empty());
}

TEST_CASE("a fold the model botches is refused, and a record under budget is left alone") {
    Fixture f;
    LogTailer tailer(f.game.log());

    // A transcript instead of a paragraph.
    f.llm.replies = {"You again.", "Henry: Still here? You: Where else."};
    f.game.dump(kBartel, 3, grown(700));
    f.game.begin(kBartel);
    f.drain(tailer);
    f.game.play();
    f.game.left();
    f.drain(tailer);
    REQUIRE(f.session.has_speculation());
    CHECK_FALSE(f.session.speculate([] { return false; }));
    CHECK(f.llm.calls.size() == 2);
    CHECK(f.session.pending().empty());
    CHECK_FALSE(f.session.has_speculation());
    const auto refused = std::find_if(f.statuses.begin(), f.statuses.end(), [](const std::string& s) {
        return s.find("refused") != std::string::npos;
    });
    CHECK(refused != f.statuses.end());

    // Short enough to keep as it is: the model is not asked at all.
    f.llm.replies = {"You again."};
    f.game.dump(kBartel, 3, grown(20));
    f.game.begin(kBartel);
    f.drain(tailer);
    f.game.play();
    f.game.left();
    f.drain(tailer);
    CHECK_FALSE(f.session.has_speculation());
    CHECK(f.llm.calls.size() == 3);
    CHECK(f.session.pending().empty());
}

TEST_CASE("a fold due is not in the way of the next begin, and is stopped by a line of the log") {
    Fixture f;
    LogTailer tailer(f.game.log());
    // The paragraph twice: the stub spends a reply on the call the log stops below.
    f.llm.replies = {"You again.", "Who are you?", "A paragraph: Henry asked the way and you gave it.",
                     "A paragraph: Henry asked the way and you gave it."};
    f.game.dump(kBartel, 3, grown(700), "", 4);
    f.game.begin(kBartel);
    f.drain(tailer);
    REQUIRE(f.game.play() == "You again.");
    f.drain(tailer);
    f.game.left();
    f.drain(tailer);
    REQUIRE(f.session.has_speculation());

    // The player walks straight up to somebody else before the poller got to the fold
    // (29 Oct 2026: Christine's 34 entries took 35 s to fold on the loop, and every soul
    // pressed on in that time got the camera and no words). The begin is answered from
    // the model at once, the fold still waiting.
    f.game.begin(kStranger);
    f.drain(tailer);
    REQUIRE(f.game.play() == "Who are you?");
    CHECK(f.llm.calls.size() == 2);
    CHECK(f.game.directives().empty());
    f.drain(tailer);
    f.game.left();
    f.drain(tailer);

    // Back between conversations, a line arriving stops the fold within a token, and it
    // is taken up again next time round; the directive then rides the next turn.
    REQUIRE(f.session.has_speculation());
    CHECK(f.session.speculate([] { return true; }));
    CHECK(f.llm.calls.size() == 3);
    REQUIRE(f.session.has_speculation());
    CHECK(f.session.pending().empty());
    CHECK_FALSE(f.session.speculate([] { return false; }));
    CHECK(f.llm.calls.size() == 4);
    CHECK_FALSE(f.session.has_speculation());
    REQUIRE(f.session.pending().size() == 1);
    CHECK(f.session.pending()[0].rfind("@memory " + kBartel + " compact 4 6 ", 0) == 0);
    const auto stopped = std::find_if(f.statuses.begin(), f.statuses.end(), [](const std::string& s) {
        return s.find("stopped 1 times by the log") != std::string::npos;
    });
    CHECK(stopped != f.statuses.end());
}
