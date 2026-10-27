#include "doctest.h"

#include <chrono>
#include <thread>

#include "fake_game.h"
#include "gamelog.h"

using namespace moonshine;

TEST_CASE("the markers the mod prints are recognised in the lines it prints them on") {
    // Real lines, with the timestamp the game prefixes and the trailing detail the
    // mod appends, because a classifier that only passes on trimmed markers is a
    // classifier that has not been tested.
    const Event begin = classify("<12:04:31> BRIDGE begin npc=kkut_additive_man_47");
    CHECK(begin.marker == Marker::Begin);
    // A soul id, not a name, whatever the mod calls the field. The engine has no
    // soul:GetName(), so this is the only identity available -- and it happens to be
    // what npcs.sqlite is keyed on.
    CHECK(begin.soul == "kkut_additive_man_47");

    const Event played = classify("<12:04:35> BRIDGE played turn=7 -- Good day to you, sir.");
    CHECK(played.marker == Marker::Played);
    CHECK(played.turn == 7);

    CHECK(classify("<12:05:02> BRIDGE left, player walked away, 3.4m away").marker == Marker::Left);

    // The engine's own last line, which is how the app learns the run is over while the
    // file is still there to read (a canned app stops on it).
    CHECK(classify("CSystem::Quit invoked from thread 63208 (main is 63208), reason: Quit requested by "
                   "C_UISaveLoad::ExitGame(), exit code: 0").marker == Marker::Quit);
    CHECK(classify("<12:05:02> [moonshine_names] BRIDGE played turn=3 -- Quit? Not I.").marker == Marker::Played);
}

TEST_CASE("the say row going live and stopping are markers, with the mod's reason") {
    CHECK(classify("<12:05:02> [moonshine_names] BRIDGE say start").marker == Marker::SayStart);
    const Event sent = classify("<12:05:09> [moonshine_names] BRIDGE say stop -- sent");
    CHECK(sent.marker == Marker::SayStop);
    CHECK(sent.said == "sent");
    const Event moved = classify("<12:05:09> BRIDGE say stop -- moved down");
    CHECK(moved.marker == Marker::SayStop);
    CHECK(moved.said == "moved down");
    CHECK(classify("<12:05:09> BRIDGE say stop").said.empty());
    // The pick that opened the row is a pick with the say intent and no words; the pick
    // that sent it carries the words, and the said line that follows is the one answered.
    const Event opened = classify("<12:05:02> BRIDGE picked 4 say -- ");
    CHECK(opened.marker == Marker::Picked);
    CHECK(opened.intent == "say");
    CHECK(opened.said.empty());
    const Event picked = classify("<12:05:08> BRIDGE picked 1 say -- Where is the miller?");
    CHECK(picked.marker == Marker::Picked);
    CHECK(picked.intent == "say");
    CHECK(picked.said == "Where is the miller?");
    // The player's own words quoting the marker are still their words.
    CHECK(classify("<12:05:08> BRIDGE said BRIDGE say start").marker == Marker::Said);
}

TEST_CASE("a line typed in the game arrives as the player's words") {
    const Event said = classify("<12:04:40> [moonshine_names] BRIDGE said Which way to Rattay?");
    CHECK(said.marker == Marker::Said);
    CHECK(said.said == "Which way to Rattay?");

    // The words are the player's and may be anything, including another marker's text.
    // A begin line, by contrast, can never contain this marker.
    const Event tricky = classify("<12:04:40> BRIDGE said BRIDGE left, I am off");
    CHECK(tricky.marker == Marker::Said);
    CHECK(tricky.said == "BRIDGE left, I am off");

    // Trailing whitespace is the log's, not the player's; and nothing is not a line.
    const Event voice = classify("<12:04:41> BRIDGE voice henry What do you want?");
    CHECK(voice.marker == Marker::Voice);
    CHECK(voice.intent == "henry");
    CHECK(voice.said == "What do you want?");
    // The player's words win when they quote the marker, because theirs were written first.
    const Event quoted = classify("<12:04:41> BRIDGE said BRIDGE voice henry no");
    CHECK(quoted.marker == Marker::Said);
    CHECK(quoted.said == "BRIDGE voice henry no");
    const Event quotes_said = classify("<12:04:41> BRIDGE voice npc BRIDGE said nothing of the sort");
    CHECK(quotes_said.marker == Marker::Voice);
    CHECK(quotes_said.intent == "npc");
    CHECK(quotes_said.said == "BRIDGE said nothing of the sort");
    // A voiced Talk outside a session names its speaker ahead of the words, for the
    // mouth; the words are what is spoken and keyed, and a session's line has no soul.
    const Event named = classify(
        "<12:04:42> BRIDGE voice male soul=kkut_additive_man_138 From that woman? Hand it here.");
    CHECK(named.marker == Marker::Voice);
    CHECK(named.intent == "male");
    CHECK(named.soul == "kkut_additive_man_138");
    CHECK(named.said == "From that woman? Hand it here.");
    CHECK(voice.soul.empty());
    CHECK(classify("BRIDGE voice male soul=kkut_additive_man_138").marker == Marker::None);

    CHECK(classify("BRIDGE said hello   ").said == "hello");
    CHECK(classify("BRIDGE said    ").marker == Marker::None);
    // The mod's own log of the same event under another name is not a second line.
    CHECK(classify("BRIDGE typing on").marker == Marker::None);
}

TEST_CASE("a turn that was only asked for is not a turn that was heard") {
    // This distinction cost a conversation's worth of turns in two minutes, with
    // three lines in four never said, so it is pinned here.
    CHECK(classify("<12:04:34> BRIDGE spoke turn=7").marker == Marker::None);
    CHECK(classify("<12:04:34> BRIDGE beat 3 turn=7").marker == Marker::None);
    CHECK(classify("<12:04:36> BRIDGE skipped turn=7").marker == Marker::None);
}

TEST_CASE("the rest of the log is not ours") {
    CHECK(classify("").marker == Marker::None);
    CHECK(classify("$3[CryAnimation] loaded 41 animations").marker == Marker::None);
    // Chatter from the mod that is not a conversation event.
    CHECK(classify("<12:04:31> BRIDGE screen 2560x1440 from r_Width").marker == Marker::None);
    CHECK(classify("<12:04:31> BRIDGE face speak").marker == Marker::None);
}

TEST_CASE("a marker with a number that is not a number is ignored, not thrown on") {
    CHECK(classify("BRIDGE played turn=").marker == Marker::None);
    CHECK(classify("BRIDGE played turn=nil").marker == Marker::None);
    CHECK(classify("BRIDGE played turn=0").marker == Marker::None);
}

TEST_CASE("a begin marker carries the raw state, alongside the soul") {
    // Many fields on one line, which is why the soul id no longer runs to the end of it.
    // The mod reports numbers and not a verdict: what "rch=1" means about how this player
    // is greeted is decided in approach.cpp, so that the bands live in one place.
    const Event begin = classify("<12:04:31> BRIDGE begin npc=kkut_additive_man_47 "
                                 "rch=0.473429 bld=0.142857 drt=0.0196078 dru=0 hp=100 "
                                 "drawn=false asleep=false chatting=false");
    CHECK(begin.marker == Marker::Begin);
    CHECK(begin.soul == "kkut_additive_man_47");
    CHECK(begin.approach.charisma == doctest::Approx(0.473429));
    CHECK(begin.approach.blood == doctest::Approx(0.142857));
    CHECK(begin.approach.dirt == doctest::Approx(0.0196078));
    CHECK(begin.approach.drink == doctest::Approx(0.0));
    CHECK(begin.approach.health == doctest::Approx(100.0));
    CHECK_FALSE(begin.approach.armed);
    CHECK_FALSE(begin.approach.asleep);
    CHECK_FALSE(begin.approach.chatting);
}

TEST_CASE("a state the game would not give up reads as unread, not as zero") {
    // The difference matters: zero dirt is a clean player, while an unread dirt is one
    // nothing can be said about. Reading nil as 0 would have called every player clean.
    const Event begin = classify("BRIDGE begin npc=kkut_woman_30 rch=nil bld=0 drawn=true");
    CHECK(begin.approach.charisma == kUnread);
    CHECK(begin.approach.blood == doctest::Approx(0.0));
    CHECK(begin.approach.armed);
    // Never reported at all is the same to the app as reported nil.
    CHECK(begin.approach.health == kUnread);
}

TEST_CASE("a begin marker carries what the soul was caught doing") {
    // Three fields now. The animation name is the engine's own spelling, underscores and
    // all, and it is the last field on the line -- so this also pins that a field stops at
    // the space and does not swallow what follows.
    const Event begin = classify("<12:04:31> BRIDGE begin npc=kkut_woman_30 rch=1 "
                                 "doing=Bartender_TakeAndTapStein");
    CHECK(begin.marker == Marker::Begin);
    CHECK(begin.soul == "kkut_woman_30");
    CHECK(begin.approach.charisma == doctest::Approx(1.0));
    CHECK(begin.doing == "Bartender_TakeAndTapStein");
}

TEST_CASE("a look is a begin that has not happened, and reads the same way") {
    // The mod sends it from the frame the player has a soul in their sights from talking
    // distance, with the same readings a begin would carry, so the app can have the opening
    // line ready before they press. Everything the begin's parser knows, it knows.
    const Event look = classify("<12:04:29> [moonshine_names] BRIDGE look npc=kkut_woman_30 "
                                "doing=Bartender_TakeAndTapStein rch=1 bld=0.5 hour=16.7823 "
                                "rain=0.24 have=1");
    CHECK(look.marker == Marker::Look);
    CHECK(look.soul == "kkut_woman_30");
    CHECK(look.doing == "Bartender_TakeAndTapStein");
    CHECK(look.approach.charisma == doctest::Approx(1.0));
    CHECK(look.approach.blood == doctest::Approx(0.5));
    CHECK(look.approach.hour == doctest::Approx(16.7823));
    CHECK(look.approach.have == 1);
    // A said line that happens to say the words is the player's, not a look.
    CHECK(classify("<12:04:40> [moonshine_names] BRIDGE said BRIDGE look npc=nobody").marker ==
          Marker::Said);
}

TEST_CASE("a begin marker carries the clock and the rain") {
    const Event begin = classify("<12:04:31> BRIDGE begin npc=kkut_woman_30 rch=1 "
                                 "hour=16.7823 rain=0.24 doing=none");
    CHECK(begin.approach.hour == doctest::Approx(16.7823));
    CHECK(begin.approach.rain == doctest::Approx(0.24));
}

TEST_CASE("a begin marker carries where the soul stands, or leaves it unread") {
    // "at=<x>,<y>" in world metres; both or neither, and a mod too old to say leaves both
    // unread, which keeps every remark (not knowing is not being away).
    const Event begin = classify("<12:04:31> BRIDGE begin npc=kkut_woman_30 rch=1 "
                                 "asleep=false chatting=false at=1823,-2410 hour=16.7823");
    CHECK(begin.approach.x == doctest::Approx(1823.0));
    CHECK(begin.approach.y == doctest::Approx(-2410.0));
    const Event old = classify("<12:04:31> BRIDGE begin npc=kkut_woman_30 rch=1 hour=16.7823");
    CHECK(old.approach.x == kUnread);
    CHECK(old.approach.y == kUnread);
    const Event half = classify("<12:04:31> BRIDGE begin npc=kkut_woman_30 at=1823, hour=1");
    CHECK(half.approach.x == kUnread);
    CHECK(half.approach.y == kUnread);
}

TEST_CASE("a begin or look marker carries which deal the mod was built from") {
    // qset= is the mod's fingerprint of its quest ids (Moonshine.questSet), set against
    // PersonaDb::quest_set() in main.cpp; a mod too old to send it leaves it empty, and an
    // empty one is never a mismatch.
    const Event begin = classify("<12:04:31> BRIDGE begin npc=kkut_woman_30 rch=1 at=1823,-2410 "
                                 "hour=16.7823 qset=9f1a2b3c4d5e6f70");
    CHECK(begin.approach.quest_set == "9f1a2b3c4d5e6f70");
    const Event look = classify("BRIDGE look npc=kkut_woman_30 doing=none qset=9f1a2b3c4d5e6f70 hour=1");
    CHECK(look.approach.quest_set == "9f1a2b3c4d5e6f70");
    const Event old = classify("<12:04:31> BRIDGE begin npc=kkut_woman_30 rch=1 hour=16.7823");
    CHECK(old.approach.quest_set.empty());
}

TEST_CASE("a mod too old to report the world leaves it unread rather than midnight") {
    // The mod and the app are deployed separately -- scripts/build_mod.sh refuses to run while
    // the game is up -- so an app newer than the mod on disk is the normal state for a lap or
    // two. Reading a missing hour as 0 would have every one of those conversations open at
    // midnight, and unlike a wrong dirt band that reads as plausible dialogue.
    const Event begin = classify("BRIDGE begin npc=kkut_woman_30 rch=1 drawn=false");
    CHECK(begin.approach.hour == kUnread);
    CHECK(begin.approach.rain == kUnread);

    // Dry is a reading and reads as zero, which is not the same as unread: the sky was looked
    // at and there was nothing to say about it.
    const Event dry = classify("BRIDGE begin npc=kkut_woman_30 hour=0 rain=0");
    CHECK(dry.approach.hour == doctest::Approx(0.0));
    CHECK(dry.approach.rain == doctest::Approx(0.0));
}

TEST_CASE("a soul the mod watched and learned nothing about reports none") {
    // What a soul spawning into view and being talked to at once looks like.
    const Event begin = classify("BRIDGE begin npc=kkut_additive_man_47 rch=1 doing=none");
    CHECK(begin.doing == "none");
}

TEST_CASE("a mod too old to report any state still opens a conversation") {
    // Served the everyday greeting rather than silence, and no clause rather than none.
    const Event begin = classify("<12:04:31> BRIDGE begin npc=kkut_additive_man_47");
    CHECK(begin.marker == Marker::Begin);
    CHECK(begin.soul == "kkut_additive_man_47");
    CHECK(begin.approach.charisma == kUnread);
    CHECK(begin.doing.empty());
}

TEST_CASE("a conversation with no soul given still opens") {
    // Older saves predate the id being logged. Talking to a stranger beats refusing
    // to talk.
    const Event begin = classify("<12:04:31> BRIDGE begin");
    CHECK(begin.marker == Marker::Begin);
    CHECK(begin.soul.empty());
}

TEST_CASE("only what happens from here counts") {
    FakeGame game;
    game.begin("kkut_from_an_earlier_run");
    game.say("<12:04:35> BRIDGE played turn=1 -- said in an earlier run");

    // Opening at the end, because replaying a finished run would spend turns that
    // have already been said.
    LogTailer tailer(game.log());
    REQUIRE(tailer.ok());
    CHECK(tailer.poll().empty());

    game.begin("kkut_additive_man_47");
    const std::vector<Event> events = tailer.poll();
    REQUIRE(events.size() == 1);
    CHECK(events[0].soul == "kkut_additive_man_47");
}

TEST_CASE("an app that opens late reads the game's count of turns off the log as it stands") {
    FakeGame game;
    CHECK(last_played_turn(game.log()) == 0);
    game.begin("kkut_additive_man_47");
    game.say("<12:04:35> BRIDGE played turn=1 -- Good day to you.");
    game.say("<12:04:39> BRIDGE played turn=2 -- And to you.");
    game.left();
    CHECK(last_played_turn(game.log()) == 2);
    // A run that has ended has played nothing the next run counts.
    game.say("CSystem::Quit invoked from thread 1 (main is 1), reason: Quit requested, exit code: 0");
    CHECK(last_played_turn(game.log()) == 0);
    CHECK(last_played_turn("/nowhere/kcd.log") == 0);
}

TEST_CASE("a hook the game ran is a marker with its number and its words, and is counted") {
    Event ran = classify("<12:04:35> BRIDGE hook 3 ran -- going beside Heinz, who was 40m away (1.0 2.0 3.0)");
    CHECK(ran.marker == Marker::Hook);
    CHECK(ran.turn == 3);
    CHECK(ran.said == "ran -- going beside Heinz, who was 40m away (1.0 2.0 3.0)");
    Event raised = classify("<12:04:35> BRIDGE hook 4 raised attempt to index a nil value");
    CHECK(raised.marker == Marker::Hook);
    CHECK(raised.turn == 4);
    CHECK(raised.said == "raised attempt to index a nil value");
    CHECK(classify("<12:04:35> BRIDGE hook 5 did not parse: [string \"hook_5\"]:1: unexpected symbol").turn == 5);
    // No number is not a hook line.
    CHECK(classify("<12:04:35> BRIDGE hook pump").marker == Marker::None);
    // The console's own lines for the keyboard.
    Event said = classify("<12:04:36> BRIDGE console Henry is beside Heinz, who was 40m away");
    CHECK(said.marker == Marker::Console);
    CHECK(said.said == "Henry is beside Heinz, who was 40m away");
    // A command typed at the game's console, which the plain marker is a prefix of.
    Event typed = classify("<12:04:37> [moonshine_names] BRIDGE console line /goto 21 target");
    CHECK(typed.marker == Marker::ConsoleLine);
    CHECK(typed.said == "/goto 21 target");

    FakeGame game;
    CHECK(run_counts(game.log()).hooked == 0);
    game.say("<12:04:35> BRIDGE hook 1 ran");
    game.say("<12:04:35> BRIDGE played turn=1 -- Good day to you.");
    game.say("<12:04:35> BRIDGE hook 2 raised oops");
    const RunCounts counts = run_counts(game.log());
    CHECK(counts.played == 1);
    CHECK(counts.hooked == 2);
    game.say("CSystem::Quit invoked from thread 1 (main is 1), reason: Quit requested, exit code: 0");
    CHECK(run_counts(game.log()).hooked == 0);
}

TEST_CASE("the log is followed while it is being written") {
    FakeGame game;
    LogTailer tailer(game.log());
    REQUIRE(tailer.ok());

    game.begin("kkut_additive_man_47");
    game.say("$3[CryAction] something unrelated");
    game.say("<12:04:35> BRIDGE played turn=1 -- Good day to you.");

    std::vector<Event> events = tailer.poll();
    REQUIRE(events.size() == 2);
    CHECK(events[0].marker == Marker::Begin);
    CHECK(events[1].marker == Marker::Played);
    CHECK(events[1].turn == 1);

    // Nothing new is not the same thing as the same thing again.
    CHECK(tailer.poll().empty());

    game.left();
    events = tailer.poll();
    REQUIRE(events.size() == 1);
    CHECK(events[0].marker == Marker::Left);
}

TEST_CASE("a look at the log says whether anything is waiting, and takes nothing") {
    FakeGame game;
    LogTailer tailer(game.log());
    REQUIRE(tailer.ok());
    CHECK_FALSE(tailer.pending());

    // A line that is not ours is not something waiting.
    game.say("$3[CryAction] something unrelated");
    CHECK_FALSE(tailer.pending());

    game.begin("kkut_additive_man_47");
    CHECK(tailer.pending());
    CHECK(tailer.pending());
    // What was looked at is handed out once, in order with whatever came after it.
    game.say("<12:04:35> BRIDGE played turn=1 -- Good day to you.");
    const std::vector<Event> events = tailer.poll();
    REQUIRE(events.size() == 2);
    CHECK(events[0].marker == Marker::Begin);
    CHECK(events[1].marker == Marker::Played);
    CHECK_FALSE(tailer.pending());
    CHECK(tailer.poll().empty());
}

TEST_CASE("a relaunch is reported, and nothing from the new run is missed") {
    FakeGame game;
    LogTailer tailer(game.log());
    game.begin("kkut_additive_man_47");
    for (int turn = 1; turn <= 4; ++turn) {
        game.say("<12:04:35> BRIDGE played turn=" + std::to_string(turn) + " -- a line");
    }
    REQUIRE(tailer.poll().size() == 5);

    // The engine writes its log from the beginning on every start, so the file is
    // now shorter than where it was being read from.
    game.launch();
    game.begin("kkut_nameless_99");
    game.say("<12:04:35> BRIDGE played turn=1 -- the new run's first line");

    const std::vector<Event> events = tailer.poll();
    // The restart comes first, so a reader cannot reset its turn count to the wrong
    // side of the new run's opening conversation. Reading those lines from the old
    // offset and losing them is the bug this pins.
    REQUIRE(events.size() == 3);
    CHECK(events[0].marker == Marker::Restarted);
    CHECK(events[1].marker == Marker::Begin);
    CHECK(events[1].soul == "kkut_nameless_99");
    CHECK(events[2].marker == Marker::Played);
    CHECK(events[2].turn == 1);

    // And it is said once, not on every poll thereafter.
    game.say("<12:04:40> BRIDGE left, done, 2.0m away");
    const std::vector<Event> after = tailer.poll();
    REQUIRE(after.size() == 1);
    CHECK(after[0].marker == Marker::Left);
}

TEST_CASE("a half written line is read once, when the rest of it arrives") {
    FakeGame game;
    LogTailer tailer(game.log());

    // The game flushes mid-line often enough that this happens in practice, and a
    // tailer that read the fragment would then read the whole line again.
    std::ofstream raw(game.log(), std::ios::binary | std::ios::app);
    raw << "<12:04:35> BRIDGE played turn=";
    raw.flush();
    CHECK(tailer.poll().empty());

    raw << "3 -- Good day to you.\n";
    raw.flush();
    const std::vector<Event> events = tailer.poll();
    REQUIRE(events.size() == 1);
    CHECK(events[0].turn == 3);
}

TEST_CASE("a last line the engine has not ended is read once it has sat still") {
    FakeGame game;
    LogTailer tailer(game.log());

    // The engine puts the newline at the front of the next line, so the last thing it
    // said has none until it says something else -- minutes, outside a conversation.
    std::ofstream raw(game.log(), std::ios::binary | std::ios::app);
    raw << "<12:04:35> [moonshine_names] BRIDGE hook 2 ran -- going beside Heinz";
    raw.flush();
    CHECK(tailer.poll().empty());
    CHECK(tailer.poll().empty());
    std::this_thread::sleep_for(std::chrono::milliseconds(350));
    std::vector<Event> events = tailer.poll();
    REQUIRE(events.size() == 1);
    CHECK(events[0].marker == Marker::Hook);
    CHECK(events[0].turn == 2);
    CHECK(events[0].said == "ran -- going beside Heinz");
    // Not again when its newline arrives with the next line; that line is read as usual.
    raw << "\n<12:04:36> BRIDGE played turn=4 -- Hello.";
    raw.flush();
    CHECK(tailer.poll().empty());
    std::this_thread::sleep_for(std::chrono::milliseconds(350));
    events = tailer.poll();
    REQUIRE(events.size() == 1);
    CHECK(events[0].marker == Marker::Played);
    CHECK(events[0].turn == 4);
    // A fragment that grows is still a line being written: the clock starts again.
    raw << "\n<12:04:37> BRIDGE played turn=";
    raw.flush();
    CHECK(tailer.poll().empty());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    raw << "5 -- More.";
    raw.flush();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(tailer.poll().empty());
    std::this_thread::sleep_for(std::chrono::milliseconds(350));
    events = tailer.poll();
    REQUIRE(events.size() == 1);
    CHECK(events[0].turn == 5);
}

TEST_CASE("the app can start before the game does") {
    FakeGame game;
    const std::filesystem::path missing = game.paths().root / "not-yet" / "kcd.log";
    LogTailer tailer(missing);
    CHECK_FALSE(tailer.ok());
    CHECK(tailer.poll().empty());

    std::filesystem::create_directories(missing.parent_path());
    {
        std::ofstream out(missing);
        out << "<12:04:31> BRIDGE begin npc=kkut_additive_man_47\n";
    }
    // Starting a launcher only once the game is already up is not a thing to ask of
    // a player, so a log that appears later is picked up.
    const std::vector<Event> events = tailer.poll();
    REQUIRE(events.size() == 1);
    CHECK(events[0].soul == "kkut_additive_man_47");
}
